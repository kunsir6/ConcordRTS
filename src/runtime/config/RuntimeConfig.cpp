#include "runtime/config/RuntimeConfig.h"

#include "core/io/IniFile.h"
#include "core/io/RuntimeExecutableDirectory.h"
#include "runtime/config/RuntimeConfigPaths.h"
#include "runtime/campaign/RuntimeCampaignDataSource.h"
#include "ui/input/RuntimePlayerCommands.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

// Keep the original configuration parsing and path precedence. These small
// private helpers deliberately retain RuntimeShell's existing semantics;
// unrelated runtime parsing is not changed by this module extraction.

int parsePositiveInt(const std::string& value, int fallback) {
    try {
        const int parsed = std::stoi(value);
        return parsed > 0 ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

std::string chooseString(const std::string& overrideValue, const std::string& value) {
    return overrideValue.empty() ? value : overrideValue;
}

int choosePositiveInt(int overrideValue, int value) {
    return overrideValue > 0 ? overrideValue : value;
}

std::string resolveRuntimePath(
    const std::string& value,
    const std::filesystem::path& projectRoot
) {
    if (value.empty()) return "";
    return runtimeConfigLegacyPathString(runtimeResolveConfigPath(
        std::filesystem::path(value), projectRoot, std::filesystem::current_path()));
}

// ON-1-01：数据根命中时的相对路径解析。
//
// 不能直接复用 resolveRuntimePath 的「cwd 存在优先」语义，理由是自污染：
// 进程会在 cwd 下创建 local_data/runtime_map_cache，于是第二次启动时
// `local_data` 相对 cwd"存在"了，localDataRoot 就漂到源码树里的空目录，
// 资源层随即找不到 mix_exports。第一次能跑、第二次跑不动，且没有任何报错。
//
// preferBase 只在「数据根不是源码根」时打开（见 loadRuntimeConfig），
// 单仓库布局（Windows 常规）仍走原来的 cwd 优先，行为逐字节不变。
std::string resolveRuntimePathAgainstBase(
    const std::string& value,
    const std::filesystem::path& base,
    bool preferBase
) {
    if (value.empty()) return "";
    const std::filesystem::path declared(value);
    if (!preferBase || declared.is_absolute() || base.empty()) {
        return resolveRuntimePath(value, base);
    }
    std::error_code error;
    const std::filesystem::path candidate = base / declared;
    if (!std::filesystem::exists(candidate, error) || error) {
        return resolveRuntimePath(value, base);
    }
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, error);
    return runtimeConfigLegacyPathString(error ? candidate.lexically_normal() : canonical);
}

// path 版本：配置文件的解析不能经过窄字符串往返（Windows 上是 ACP）。
std::filesystem::path resolveConfigPathAgainstBase(
    const std::filesystem::path& value,
    const std::filesystem::path& base,
    const std::filesystem::path& currentDirectory,
    bool preferBase
) {
    if (!preferBase || value.is_absolute() || base.empty()) {
        return runtimeResolveConfigPath(value, base, currentDirectory);
    }
    std::error_code error;
    const std::filesystem::path candidate = base / value;
    if (!std::filesystem::exists(candidate, error) || error) {
        return runtimeResolveConfigPath(value, base, currentDirectory);
    }
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, error);
    return error ? candidate.lexically_normal() : canonical;
}

std::string toLowerAsciiLocal(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))
        );
    }
    return value;
}

std::string trimAsciiLocal(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size()
        && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first
        && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }

    return value.substr(first, last - first);
}

bool parseRuntimeInt(const std::string& value, int& output) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(trimAsciiLocal(value), &consumed);
        if (consumed != trimAsciiLocal(value).size()) {
            return false;
        }
        output = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

int parseRuntimeIntOrDefault(const std::string& value, int fallback) {
    int parsed = fallback;
    return parseRuntimeInt(value, parsed) ? parsed : fallback;
}

bool parseRuntimeBool(const std::string& value) {
    const std::string normalized = toLowerAsciiLocal(trimAsciiLocal(value));
    return normalized == "yes"
        || normalized == "true"
        || normalized == "1";
}

int parseRuntimePercent(const std::string& value, int fallback) {
    const std::string trimmed = trimAsciiLocal(value);
    if (trimmed.empty()) {
        return std::clamp(fallback, 0, 100);
    }
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(trimmed, &consumed);
        if (consumed != trimmed.size() || !std::isfinite(parsed)) {
            return std::clamp(fallback, 0, 100);
        }
        // Accept both Concord's integer percentages and the retail ra2.ini
        // 0.0..1.0 volume representation for hand-copied preferences.
        const double percent = parsed >= 0.0 && parsed <= 1.0
            && trimmed.find('.') != std::string::npos
            ? parsed * 100.0
            : parsed;
        return std::clamp(
            static_cast<int>(std::lround(percent)),
            0,
            100
        );
    } catch (...) {
        return std::clamp(fallback, 0, 100);
    }
}

} // namespace

RuntimeConfig loadRuntimeConfig(const RuntimeConfigOverrides& overrides) {
    RuntimeConfig config;
    std::error_code currentDirectoryError;
    const std::filesystem::path currentDirectory =
        std::filesystem::current_path(currentDirectoryError);
    const std::filesystem::path executableDirectory = runtimeExecutableDirectory();
    const std::filesystem::path projectRoot = runtimeConfigProjectRoot(
        currentDirectory, executableDirectory);

    // ON-1-01：相对路径的解析基准从「源码根」改成「数据根」。
    //
    // 源码根的判据要求 local_data + CMakeLists.txt + (RedAlert2|.git) 三者齐备，
    // 而 1.7 GB 的游戏数据实际落在兄弟目录（../ConcordRTS-GameData）。两边都
    // 不满足时 projectRoot 回退到 cwd，`RedAlert2` / `local_data` 于是都指向
    // 不存在的路径——主菜单拿不到 shell.pal 就静默退出，看起来像"资产缺失"。
    //
    // 未命中数据根（root 为空）时退回 projectRoot，单仓库布局行为逐字节不变。
    const RuntimeDataRootSearch dataRoot = runtimeLocateDataRoot(
        currentDirectory, executableDirectory, projectRoot);
    const std::filesystem::path pathBase =
        dataRoot.root.empty() ? projectRoot : dataRoot.root;
    // 只有当「数据根 != 源码根」时才改相对路径的优先级：
    // 单仓库布局（Windows 常规）下 source 恒为 self，preferBase 为 false，
    // 所有相对路径的解析结果与改动前逐字节一致。
    const bool preferBase = !dataRoot.root.empty() && dataRoot.source != "self";
    if (dataRoot.root.empty()) {
        // ON-1-02：未命中时一次打全「判据 / 试过的候选 / 每个缺哪一条 / 该放哪里」。
        // 旧行为只有一行 `data root <projectRoot> (source=not-found)`——既把源码根
        // 冒充成数据根，也没说缺什么，用户只能看到下游那句 palette not found，
        // 于是把「没找对目录」误判成「资产缺了」。
        std::fputs(runtimeDataRootDiagnosticText(dataRoot).c_str(), stderr);
    } else if (dataRoot.source != "self") {
        std::fprintf(
            stderr,
            "[config] data root %s (source=%s)\n",
            pathBase.generic_string().c_str(),
            dataRoot.source.c_str()
        );
    }
    if (const char* trace = std::getenv("CONCORD_DATA_ROOT_TRACE")) {
        if (trace[0] == '1') {
            std::fprintf(stderr, "[config] data root probes=%zu\n",
                         dataRoot.probed.size());
            for (const RuntimeDataRootProbe& probe : dataRoot.probed) {
                std::fprintf(
                    stderr,
                    "[config]   probe [%s] %s%s%s%s\n",
                    probe.stage == nullptr ? "" : probe.stage,
                    probe.path.generic_string().c_str(),
                    probe.isDirectory ? "" : " (not a directory)",
                    probe.hasLocalData ? " local_data=yes" : " local_data=no",
                    probe.hasRedAlert2 ? " RedAlert2=yes" : " RedAlert2=no"
                );
            }
        }
    }
    const std::filesystem::path configFile = resolveConfigPathAgainstBase(
        std::filesystem::path(chooseString(overrides.configPath, config.configPath)),
        pathBase, currentDirectory, preferBase);
    config.configPath = runtimeConfigLegacyPathString(configFile);

    std::error_code errorCode;
    if (std::filesystem::exists(configFile, errorCode)) {
        IniFile ini;
        if (ini.loadFromPath(configFile, config.configPath)) {
            config.resourceRootPath =
                ini.getValue("Runtime", "ResourceRoot", config.resourceRootPath);
            config.localDataRootPath =
                ini.getValue("Runtime", "LocalDataRoot", config.localDataRootPath);
            config.modRootPath =
                ini.getValue("Runtime", "ModRoot", config.modRootPath);
            config.defaultMapPath =
                ini.getValue("Runtime", "DefaultMap", config.defaultMapPath);
            config.windowWidth = parsePositiveInt(
                ini.getValue("Runtime", "WindowWidth", ""),
                config.windowWidth
            );
            config.windowHeight = parsePositiveInt(
                ini.getValue("Runtime", "WindowHeight", ""),
                config.windowHeight
            );
            config.fixedTickHz = parsePositiveInt(
                ini.getValue("Runtime", "FixedTickHz", ""),
                config.fixedTickHz
            );
            config.targetFps = parsePositiveInt(
                ini.getValue("Runtime", "TargetFps", ""),
                config.targetFps
            );
            config.showMainMenu = parseRuntimeBool(
                ini.getValue(
                    "Runtime",
                    "ShowMainMenu",
                    config.showMainMenu ? "yes" : "no"
                )
            );
            config.showSkirmishMenu = parseRuntimeBool(
                ini.getValue(
                    "Skirmish",
                    "ShowSetup",
                    config.showSkirmishMenu ? "yes" : "no"
                )
            );
            config.gameMode = ini.getValue(
                "Skirmish",
                "GameMode",
                config.gameMode
            );
            config.gameModeRulesOverride = ini.getValue(
                "Skirmish",
                "GameModeRulesOverride",
                config.gameModeRulesOverride
            );
            config.playerCountry = ini.getValue(
                "Skirmish",
                "Country",
                config.playerCountry
            );
            config.playerColor = ini.getValue(
                "Skirmish",
                "Color",
                config.playerColor
            );
            config.startingCredits = parsePositiveInt(
                ini.getValue("Skirmish", "Credits", ""),
                config.startingCredits
            );
            config.startingUnits = std::max(
                0,
                parseRuntimeIntOrDefault(
                    ini.getValue("Skirmish", "UnitCount", ""),
                    config.startingUnits
                )
            );
            config.techLevel = std::clamp(
                parseRuntimeIntOrDefault(
                    ini.getValue("Skirmish", "TechLevel", ""),
                    config.techLevel
                ),
                1,
                10
            );
            config.gameSpeed = std::clamp(
                parseRuntimeIntOrDefault(
                    ini.getValue("Skirmish", "GameSpeed", ""),
                    config.gameSpeed
                ),
                0,
                6
            );
            config.scrollRate = std::clamp(
                parseRuntimeIntOrDefault(
                    ini.getValue("Options", "ScrollRate", ""),
                    config.scrollRate
                ),
                0,
                6
            );
            config.visualDetail = std::clamp(
                parseRuntimeIntOrDefault(
                    ini.getValue("Options", "DetailLevel", ""),
                    config.visualDetail
                ),
                0,
                2
            );
            config.tooltipsEnabled = parseRuntimeBool(
                ini.getValue(
                    "Options",
                    "ToolTips",
                    config.tooltipsEnabled ? "yes" : "no"
                )
            );
            config.targetLinesEnabled = parseRuntimeBool(
                ini.getValue(
                    "Options",
                    "UnitActionLines",
                    config.targetLinesEnabled ? "yes" : "no"
                )
            );
            config.seeHiddenObjects = parseRuntimeBool(ini.getValue("Options",
                "SeeHiddenObjects", config.seeHiddenObjects ? "yes" : "no"));
            config.scrollCoastingEnabled = parseRuntimeBool(
                ini.getValue(
                    "Options",
                    "ScrollCoasting",
                    config.scrollCoastingEnabled ? "yes" : "no"
                )
            );
            config.musicVolume = parseRuntimePercent(
                ini.getValue("Audio", "MusicVolume", ""),
                config.musicVolume
            );
            config.soundVolume = parseRuntimePercent(
                ini.getValue("Audio", "SoundVolume", ""),
                config.soundVolume
            );
            config.voiceVolume = parseRuntimePercent(
                ini.getValue("Audio", "VoiceVolume", ""),
                config.voiceVolume
            );
            config.musicEnabled = parseRuntimeBool(
                ini.getValue(
                    "Audio",
                    "InGameMusic",
                    config.musicEnabled ? "yes" : "no"
                )
            );
            config.soundEffectsEnabled = parseRuntimeBool(
                ini.getValue(
                    "Audio",
                    "SoundEnabled",
                    config.soundEffectsEnabled ? "yes" : "no"
                )
            );
            std::vector<std::string> preferenceBindingIds = {
                "Options", "StopObject", "GuardObject", "ScatterObject", "DeployObject", "CenterView"
            };
            for (const auto& binding : runtimePlayerCommandBindings())
                preferenceBindingIds.push_back(binding.first);
            config.keyboardReferenceLayout = ini.getValue("Hotkey", "ReferenceLayout", "") == "20260913";
            config.keyBindings.clear();
            for (const std::string& bindingId : preferenceBindingIds) {
                const std::string raw = trimAsciiLocal(
                    ini.getValue("Hotkey", bindingId, "")
                );
                if (raw.empty()) {
                    continue;
                }
                const int keyCode = parseRuntimeIntOrDefault(raw, 0);
                if (runtimePlayerValidKeyCode(keyCode)) {
                    config.keyBindings.emplace_back(bindingId, keyCode);
                }
            }
            config.shortGame = parseRuntimeBool(
                ini.getValue(
                    "Skirmish",
                    "ShortGame",
                    config.shortGame ? "yes" : "no"
                )
            );
            config.mcvRepacks = parseRuntimeBool(
                ini.getValue(
                    "Skirmish",
                    "MCVRepacks",
                    config.mcvRepacks ? "yes" : "no"
                )
            );
            config.crates = parseRuntimeBool(
                ini.getValue(
                    "Skirmish",
                    "Crates",
                    config.crates ? "yes" : "no"
                )
            );
            config.superweapons = parseRuntimeBool(
                ini.getValue(
                    "Skirmish",
                    "Superweapons",
                    config.superweapons ? "yes" : "no"
                )
            );
            config.buildOffAlly = parseRuntimeBool(
                ini.getValue(
                    "Skirmish",
                    "BuildOffAlly",
                    config.buildOffAlly ? "yes" : "no"
                )
            );
            config.computerAiEnabled = parseRuntimeBool(
                ini.getValue(
                    "Skirmish",
                    "ComputerAI",
                    config.computerAiEnabled ? "yes" : "no"
                )
            );
            config.noCooldown = parseRuntimeBool(ini.getValue("Skirmish", "NoCooldown", "no"));
            config.campaignId = ini.getValue(
                "Campaign",
                "Campaign",
                config.campaignId
            );
            config.campaignMissionId = ini.getValue(
                "Campaign",
                "Mission",
                config.campaignMissionId
            );
            config.campaignSourceId = normalizeRuntimeCampaignSourceId(
                ini.getValue(
                    "Campaign",
                    "SourceId",
                    config.campaignSourceId
                )
            );
            config.campaignDifficulty = ini.getValue(
                "Campaign",
                "Difficulty",
                config.campaignDifficulty
            );
            config.campaignPlayerHouse = ini.getValue(
                "Campaign",
                "PlayerHouse",
                config.campaignPlayerHouse
            );
            config.campaignNextMissionId = ini.getValue(
                "Campaign",
                "NextMission",
                config.campaignNextMissionId
            );
            config.campaignSavePath = ini.getValue(
                "Campaign",
                "SavePath",
                config.campaignSavePath
            );
            config.runtimeReplayEventOutputPath =
                ini.getValue(
                    "Runtime",
                    "RuntimeReplayEvents",
                    config.runtimeReplayEventOutputPath
                );
        }
    }

    config.resourceRootPath = resolveRuntimePathAgainstBase(
        chooseString(overrides.resourceRootPath, config.resourceRootPath),
        pathBase, preferBase
    );
    config.localDataRootPath = resolveRuntimePathAgainstBase(
        chooseString(overrides.localDataRootPath, config.localDataRootPath),
        pathBase, preferBase
    );
    config.modRootPath = resolveRuntimePathAgainstBase(
        chooseString(overrides.modRootPath, config.modRootPath),
        pathBase, preferBase
    );
    config.defaultMapPath = resolveRuntimePathAgainstBase(
        chooseString(overrides.defaultMapPath, config.defaultMapPath),
        pathBase, preferBase
    );
    config.campaignIntroVideoPath = resolveRuntimePathAgainstBase(
        config.campaignIntroVideoPath,
        pathBase, preferBase
    );
    config.campaignPostScoreVideoPath = resolveRuntimePathAgainstBase(
        config.campaignPostScoreVideoPath,
        pathBase, preferBase
    );
    config.campaignLoadSavePath = resolveRuntimePathAgainstBase(
        config.campaignLoadSavePath,
        pathBase, preferBase
    );
    config.campaignSavePath = resolveRuntimePathAgainstBase(
        config.campaignSavePath,
        pathBase, preferBase
    );
    config.windowWidth =
        choosePositiveInt(overrides.windowWidth, config.windowWidth);
    config.windowHeight =
        choosePositiveInt(overrides.windowHeight, config.windowHeight);
    config.fixedTickHz =
        choosePositiveInt(overrides.fixedTickHz, config.fixedTickHz);
    config.targetFps =
        choosePositiveInt(overrides.targetFps, config.targetFps);
    config.runtimeReplayEventOutputPath = resolveRuntimePathAgainstBase(
        chooseString(
            overrides.runtimeReplayEventOutputPath,
            config.runtimeReplayEventOutputPath
        ),
        pathBase, preferBase
    );

    return config;
}

