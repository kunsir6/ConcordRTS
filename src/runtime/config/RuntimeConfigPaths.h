#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Synchronous, owned filesystem values. No directory enumeration, case folding,
// resource selection or INI decoding takes place here. Injected directories make
// root discovery independently testable without changing process state.
std::filesystem::path runtimeConfigProjectRoot(
    const std::filesystem::path& currentDirectory,
    const std::filesystem::path& executableDirectory);

// ON-1-01: 数据根探测。
//
// 背景：projectRoot 的判据要求「local_data + CMakeLists.txt + (RedAlert2|.git)」
// 同时存在，这假设了代码根与数据根是同一个目录。实际开发布局把 1.7 GB 的
// 游戏数据放在兄弟目录（例如 ../ConcordRTS-GameData），于是 projectRoot 永远
// 匹配不上、回退到 cwd，`RedAlert2` 与 `local_data` 都解析到不存在的路径。
// 表现就是主菜单拿不到 shell.pal 后静默退出——看起来像"资产缺了"，
// 实际上是"根本没找对目录"。
//
// 这里把数据根单独当成一个可探测的东西：只要某个目录同时含
// `local_data/` 与 `RedAlert2/`，它就是数据根。探测按「显式覆盖 > 环境变量 >
// .app 指针文件 > 已知根自身 > 兄弟目录 > 逐级祖先」的顺序，覆盖项目常见的四种摆法：
// 单仓库内联、兄弟目录、数据放任意上级目录、以及 macOS 的 .app 打包形态
// （ON-1-03：Contents/Resources/data-root 写下数据目录，命中即 source=app-resources）。
// ON-1-02：每个候选根的判定明细。
//
// 判据只有两条（local_data/ 与 RedAlert2/ 同时存在），失败时用户真正需要知道的是
// 「试过哪些目录、每个目录缺哪一条」。把文件系统事实在探测阶段就记下来，
// 诊断文本的生成于是不碰磁盘：可离线复算，也可以在测试里构造。
struct RuntimeDataRootProbe {
    std::filesystem::path path;
    bool isDirectory = false;
    bool hasLocalData = false;
    bool hasRedAlert2 = false;
    const char* stage = "";                      // environment / self / sibling / ancestor
    bool isDataRoot() const { return hasLocalData && hasRedAlert2; }
};

struct RuntimeDataRootSearch {
    std::filesystem::path root;                  // 命中的根；空表示未命中
    std::vector<RuntimeDataRootProbe> probed;    // 完整探测序列（诊断/测试用）
    std::string source;                          // environment / app-resources / self / sibling / ancestor
    // ON-1-02：诊断需要的三个锚点，探测入口原样保留。
    std::filesystem::path currentDirectory;
    std::filesystem::path executableDirectory;
    std::filesystem::path projectRoot;
};

// ON-1-02：数据根未命中时的启动诊断文本（纯格式化，不访问文件系统）。
// 一次说清四件事：判据是什么、试过哪些候选、每个候选缺哪一条、缺的资产该放哪里。
std::string runtimeDataRootDiagnosticText(const RuntimeDataRootSearch& search);

// projectRoot 只用于构造候选顺序，不参与最终判定；未命中时 root 为空，
// 调用方应退回原有解析逻辑（保证 Windows 单仓库布局逐字节不变）。
RuntimeDataRootSearch runtimeLocateDataRoot(
    const std::filesystem::path& currentDirectory,
    const std::filesystem::path& executableDirectory,
    const std::filesystem::path& projectRoot);

// Absolute input wins; otherwise an existing cwd candidate wins; otherwise use
// the project root even if the optional file does not exist. weakly_canonical
// errors retain the original lexical-normalization fallback.
std::filesystem::path runtimeResolveConfigPath(
    const std::filesystem::path& value,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& currentDirectory);

// Compatibility boundary for RuntimeConfig's existing narrow string consumers:
// generic_string uses the native narrow code page on Windows, not UTF-8.
// Arbitrary Unicode output through these legacy fields is not promised. Native
// paths above never pass through this conversion before filesystem lookup.
std::string runtimeConfigLegacyPathString(const std::filesystem::path& path);
