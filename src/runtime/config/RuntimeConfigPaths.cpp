#include "runtime/config/RuntimeConfigPaths.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {
namespace fs = std::filesystem;

bool pathExists(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error);
}

// ON-1-01：数据根判据。与 isProjectRoot 刻意不同——数据根不要求
// CMakeLists.txt，因为 1.7 GB 的游戏数据本来就不该压在源码树里。
// 只认两个目录：local_data（配置/缓存/规则）与 RedAlert2（资源根）。
bool isDataRoot(const fs::path& path) {
    return pathExists(path / "local_data") && pathExists(path / "RedAlert2");
}

bool isProjectRoot(const fs::path& path) {
    return pathExists(path / "local_data")
        && pathExists(path / "CMakeLists.txt")
        && (pathExists(path / "RedAlert2") || pathExists(path / ".git"));
}

fs::path normalized(const fs::path& path) {
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(path, error);
    return error ? path.lexically_normal() : canonical;
}

void addAncestors(std::vector<fs::path>& candidates, fs::path path) {
    if (path.empty()) return;
    path = normalized(path);
    while (!path.empty()) {
        candidates.push_back(path);
        const fs::path parent = path.parent_path();
        if (parent.empty() || parent == path) break;
        path = parent;
    }
}

// ON-1-03：读 .app 里的数据根指针文件，返回第一行有效内容（空 = 没有指针）。
// 允许 `#` 注释行与行尾空白：这个文件是给人改的，不是只给机器读的。
std::string readPointerLine(const fs::path& file) {
    std::error_code error;
    if (!fs::is_regular_file(file, error) || error) return {};
    std::ifstream input(file);
    if (!input) return {};
    std::string line;
    while (std::getline(input, line)) {
        while (!line.empty()
               && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') continue;
        return line;
    }
    return {};
}
} // namespace

std::filesystem::path runtimeConfigProjectRoot(
    const std::filesystem::path& currentDirectory,
    const std::filesystem::path& executableDirectory
) {
    std::vector<fs::path> candidates;
    addAncestors(candidates, currentDirectory);
    addAncestors(candidates, executableDirectory);
    for (const fs::path& candidate : candidates) {
        if (isProjectRoot(candidate)) return candidate;
    }
    if (!candidates.empty()) return candidates.front();
    return fs::current_path();
}

std::filesystem::path runtimeResolveConfigPath(
    const std::filesystem::path& value,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& currentDirectory
) {
    if (value.empty()) return {};
    if (value.is_absolute()) return normalized(value);
    const fs::path currentCandidate = currentDirectory / value;
    if (pathExists(currentCandidate)) return normalized(currentCandidate);
    return normalized(projectRoot / value);
}

std::string runtimeConfigLegacyPathString(const std::filesystem::path& path) {
    return path.generic_string();
}

namespace {
// 去重用的键：规范化字符串。探测序列会重复（cwd 的祖先里通常已经含 projectRoot）。
std::string probeKey(const fs::path& path) {
    if (path.empty()) return {};
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(path, error);
    return (error ? path.lexically_normal() : canonical).generic_string();
}

void pushProbe(
    RuntimeDataRootSearch& search,
    std::vector<std::string>& seen,
    const fs::path& candidate,
    const char* stage
) {
    if (candidate.empty()) return;
    const std::string key = probeKey(candidate);
    if (key.empty()) return;
    for (const std::string& existing : seen) {
        if (existing == key) return;
    }
    seen.push_back(key);
    RuntimeDataRootProbe probe;
    probe.path = candidate;
    probe.stage = stage;
    std::error_code error;
    probe.isDirectory = fs::is_directory(candidate, error) && !error;
    // 已判定为普通文件时不再拼子路径查询，省掉两次失败的系统调用。
    if (probe.isDirectory) {
        probe.hasLocalData = pathExists(candidate / "local_data");
        probe.hasRedAlert2 = pathExists(candidate / "RedAlert2");
    }
    search.probed.push_back(probe);
}

// 只枚举一级子目录。刻意不递归：数据根永远在候选根的同一层或上一层，
// 递归会在一台有几十万个文件的机器上造成可感知的启动延迟。
void pushSiblings(
    RuntimeDataRootSearch& search,
    std::vector<std::string>& seen,
    const fs::path& parent
) {
    std::error_code error;
    if (parent.empty() || !fs::is_directory(parent, error)) return;
    fs::directory_iterator it(parent, fs::directory_options::skip_permission_denied, error);
    if (error) return;
    std::vector<fs::path> children;
    for (const fs::directory_entry& entry : it) {
        std::error_code entryError;
        if (!entry.is_directory(entryError) || entryError) continue;
        children.push_back(entry.path());
    }
    // directory_iterator 的顺序取决于文件系统；排序让诊断输出可复现。
    std::sort(children.begin(), children.end());
    for (const fs::path& child : children) {
        pushProbe(search, seen, child, "sibling");
    }
}
}  // namespace

// 第 n 级祖先（0 = 自身）。
fs::path ancestorAt(fs::path path, int level) {
    for (int i = 0; i < level; ++i) {
        if (path.empty()) return {};
        const fs::path parent = path.parent_path();
        if (parent.empty() || parent == path) return {};
        path = parent;
    }
    return path;
}

RuntimeDataRootSearch runtimeLocateDataRoot(
    const std::filesystem::path& currentDirectory,
    const std::filesystem::path& executableDirectory,
    const std::filesystem::path& projectRoot
) {
    RuntimeDataRootSearch search;
    std::vector<std::string> seen;
    // ON-1-02：诊断文本需要知道「从哪儿开始找的」，原样保存入口参数。
    search.currentDirectory = currentDirectory;
    search.executableDirectory = executableDirectory;
    search.projectRoot = projectRoot;

    // ⓪ .app 内的数据根指针：Contents/Resources/data-root。
    //
    //    双击启动时 cwd 是 "/"，可执行文件在 Contents/MacOS/，唯一稳定的锚点是
    //    自身路径。指针文件把打包时选定的数据目录写下来，于是 bundle 整体搬到
    //    任何位置都还能找到数据——这是 symlink 做不到的（拷贝会把它扯断）。
    //    相对路径按「相对 Resources 目录」解释，其次才按 cwd。
    //
    //    顺序上放在环境变量之后：CONCORD_DATA_ROOT 是显式覆盖，仍然最高优先。
    {
        const fs::path resourcesDir = executableDirectory / ".." / "Resources";
        const std::string declared = readPointerLine(resourcesDir / "data-root");
        if (!declared.empty()) {
            const fs::path declaredPath(declared);
            std::vector<fs::path> attempts;
            if (declaredPath.is_absolute()) {
                attempts.push_back(declaredPath);
            } else {
                attempts.push_back(resourcesDir / declaredPath);
                attempts.push_back(currentDirectory / declaredPath);
            }
            for (const fs::path& attempt : attempts) {
                const fs::path resolved = normalized(attempt);
                pushProbe(search, seen, resolved, "app-resources");
                if (isDataRoot(resolved)) {
                    search.root = resolved;
                    search.source = "app-resources";
                    return search;
                }
            }
        }
    }

    // ① 环境变量：容器/CI/多份数据并存的场景靠它一锤定音。
    if (const char* environment = std::getenv("CONCORD_DATA_ROOT")) {
        if (environment[0] != '\0') {
            const fs::path declared(environment);
            const fs::path resolved = declared.is_absolute()
                ? declared
                : currentDirectory / declared;
            pushProbe(search, seen, resolved, "environment");
            if (isDataRoot(resolved)) {
                search.root = resolved;
                search.source = "environment";
                return search;
            }
        }
    }

    // ② 已知根自身。单仓库布局（Windows 常规）在这里就命中，
    //    后续分支一次都不会执行——这是「Windows 零行为变化」的保证。
    for (const fs::path& self : {projectRoot, currentDirectory, executableDirectory}) {
        if (self.empty()) continue;
        pushProbe(search, seen, self, "self");
        if (isDataRoot(self)) {
            search.root = self;
            search.source = "self";
            return search;
        }
    }

    // ③ 兄弟目录：开发期把数据放成 ../ConcordRTS-GameData 的布局。
    //
    //    必须往上探多层，而不是只看直接父目录：可执行文件在
    //    `ConcordRTS/build-product-mac/`，数据在 `ConcordRTS-GameData/`，
    //    两者共同的父是仓库的**上一层**。只看直接父目录就只枚举到
    //    ConcordRTS/ 的子目录，永远看不到兄弟仓库。
    //
    //    .app 布局也由这条兜住：Contents/MacOS/app 与 Contents/Resources/
    //    是同一个父目录下的兄弟。
    for (const fs::path& anchor : {executableDirectory, currentDirectory, projectRoot}) {
        if (anchor.empty()) continue;
        for (int level = 0; level < 4; ++level) {
            const fs::path base = ancestorAt(anchor, level);
            if (base.empty()) break;
            pushSiblings(search, seen, base.parent_path());
        }
    }
    for (const RuntimeDataRootProbe& candidate : search.probed) {
        if (candidate.isDataRoot()) {
            search.root = candidate.path;
            search.source = "sibling";
            return search;
        }
    }

    // ④ 逐级祖先：数据放在任意上级目录（比如 /data/games/{bin,RedAlert2}）。
    for (const fs::path& anchor : {currentDirectory, executableDirectory, projectRoot}) {
        fs::path path = anchor;
        while (!path.empty()) {
            pushProbe(search, seen, path, "ancestor");
            if (isDataRoot(path)) {
                search.root = path;
                search.source = "ancestor";
                return search;
            }
            const fs::path parent = path.parent_path();
            if (parent.empty() || parent == path) break;
            path = parent;
        }
    }
    for (const RuntimeDataRootProbe& candidate : search.probed) {
        if (candidate.isDataRoot()) {
            search.root = candidate.path;
            search.source = "ancestor";
            return search;
        }
    }

    search.root = std::filesystem::path();
    search.source = "not-found";
    return search;
}

namespace {
std::string displayPath(const fs::path& path) {
    return path.empty() ? std::string("(none)") : path.generic_string();
}

std::string padRight(std::string value, std::size_t width) {
    if (value.size() < width) value.append(width - value.size(), ' ');
    return value;
}

// 只描述「这个候选为什么不合格」，措辞固定在 missing <名字> 上：
// 用户拿到一行就能知道自己少建了哪个目录，不必回头读源码里的判据。
std::string shortfall(const RuntimeDataRootProbe& probe) {
    if (!probe.isDirectory) return "not a directory";
    std::string value;
    if (!probe.hasLocalData) value += "missing local_data";
    if (!probe.hasRedAlert2) {
        if (!value.empty()) value += ", ";
        value += "missing RedAlert2";
    }
    return value.empty() ? std::string("ok") : value;
}

bool stageIs(const RuntimeDataRootProbe& probe, const char* stage) {
    return probe.stage != nullptr && std::string(probe.stage) == stage;
}

// 「有一半」的候选：有 local_data 没 RedAlert2，或反之。
// 这类才是真正的线索，其余都是兄弟枚举的噪声。
bool isNearMiss(const RuntimeDataRootProbe& probe) {
    return probe.isDirectory && (probe.hasLocalData != probe.hasRedAlert2);
}

// 平票时源码根不能当答案：真数据根不会同时是代码仓库。
// 场景里 ConcordRTS（源码树，带一个自污染留下的空 local_data）与
// ConcordRTS-GameData 同为「有 local_data、缺 RedAlert2」，答案显然是后者。
int stageRank(const RuntimeDataRootProbe& probe) {
    if (stageIs(probe, "environment")) return 4;
    if (stageIs(probe, "sibling")) return 3;
    if (stageIs(probe, "ancestor")) return 2;
    return 1;  // self
}
} // namespace

std::string runtimeDataRootDiagnosticText(const RuntimeDataRootSearch& search) {
    // 探测序列动辄上百条（兄弟枚举会一路枚举到 home 目录），逐条打印会把启动
    // 日志淹掉。只列有信息量的：三个锚点（从哪儿开始找）＋所有「有一半」的候选
    // （最接近真相的那种），不足 8 条时按原顺序补齐。索引仍是真实位置，
    // 全量走 CONCORD_DATA_ROOT_TRACE=1。
    constexpr std::size_t kMinListed = 8;
    constexpr std::size_t kMaxListed = 16;

    std::vector<std::size_t> shown;
    const auto addIndex = [&shown](std::size_t index) {
        for (const std::size_t existing : shown) {
            if (existing == index) return;
        }
        shown.push_back(index);
    };
    for (std::size_t index = 0; index < search.probed.size(); ++index) {
        const RuntimeDataRootProbe& probe = search.probed[index];
        if (stageIs(probe, "self") || isNearMiss(probe)) addIndex(index);
    }
    for (std::size_t index = 0;
         index < search.probed.size() && shown.size() < kMinListed;
         ++index) {
        addIndex(index);
    }
    std::sort(shown.begin(), shown.end());
    if (shown.size() > kMaxListed) shown.resize(kMaxListed);

    std::string text;
    const auto line = [&text](const std::string& value) {
        text += value;
        text += '\n';
    };

    line("[config] data root NOT found: game assets will not resolve at startup.");
    line("[config]   rule: a data root must contain BOTH local_data/ and RedAlert2/");
    line("[config]   searched from:");
    line("[config]     current dir    : " + displayPath(search.currentDirectory));
    line("[config]     executable dir : " + displayPath(search.executableDirectory));
    line("[config]     project root   : " + displayPath(search.projectRoot));
    line(
        "[config]   probed " + std::to_string(search.probed.size())
        + " candidate(s); " + std::to_string(shown.size())
        + " shown (anchors and near-misses):"
    );

    std::size_t width = 0;
    for (const std::size_t index : shown) {
        width = std::max(width, displayPath(search.probed[index].path).size());
    }
    width = std::min<std::size_t>(width, 72);
    for (const std::size_t index : shown) {
        const RuntimeDataRootProbe& probe = search.probed[index];
        line(
            "[config]     " + std::to_string(index + 1) + ". "
            + padRight(displayPath(probe.path), width) + "  ["
            + (probe.stage == nullptr ? "" : probe.stage) + "] " + shortfall(probe)
        );
    }
    if (search.probed.size() > shown.size()) {
        line(
            "[config]     (" + std::to_string(search.probed.size() - shown.size())
            + " further candidate(s) omitted; CONCORD_DATA_ROOT_TRACE=1 prints all)"
        );
    }

    // 最可能的那一条单独点名：用户看到一行就能去改，不必在列表里比对。
    const RuntimeDataRootProbe* nearest = nullptr;
    int nearestScore = 0;
    for (const RuntimeDataRootProbe& probe : search.probed) {
        if (!isNearMiss(probe)) continue;
        const int score = (probe.hasLocalData ? 1 : 0) * 10 + stageRank(probe);
        if (score > nearestScore) {
            nearestScore = score;
            nearest = &probe;
        }
    }
    if (nearest != nullptr) {
        line(
            "[config]   most likely: " + displayPath(nearest->path) + " ("
            + shortfall(*nearest) + ")"
        );
    } else {
        line("[config]   no near-miss: no candidate had local_data/ or RedAlert2/ at all");
    }

    line("[config]   required layout (all three under the same directory):");
    line("[config]     <data root>/RedAlert2/                        resource root: GameMD/*.mix (shell.pal, *.shp), Taunts/");
    line("[config]     <data root>/local_data/                       runtime state: runtime_config.ini, rulesmd.ini, artmd.ini");
    line("[config]     <data root>/local_data/resources/mix_exports/ extracted assets the runtime actually loads");
    line("[config]   how to fix:");
    line("[config]     1) export CONCORD_DATA_ROOT=<directory containing local_data/ and RedAlert2/>");
    line("[config]     2) or place that directory next to this repository (e.g. ../ConcordRTS-GameData)");
    return text;
}
