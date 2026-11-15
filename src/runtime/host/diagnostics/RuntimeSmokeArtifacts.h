// 冒烟产物落点（S-39 / S-40）。
//
// 历史实现把冒烟 BMP 一律写到相对路径 `build/`（或数据根的兄弟目录 `build/`），
// 而 regress 的 cwd 是数据根 —— 于是每跑一轮就在源码树/数据树里堆几十 MB
// 只在该轮判读时有用的图，且完全绕开 R50 的留存策略（regress 只轮转
// progress/regress/<stamp>）。
//
// 约定（三级，前者优先）：
//   ① 各冒烟自己的专用 env（CONCORD_RESULT_PRESENTATION_OUTPUT 等）—— 保留不动
//   ② CONCORD_SMOKE_ARTIFACT_DIR —— regress 用它指到本轮 artifacts
//   ③ 历史默认值（`build`）—— 手工单跑时的行为与改前逐字一致
#ifndef CONCORD_RUNTIME_SMOKE_ARTIFACTS_H
#define CONCORD_RUNTIME_SMOKE_ARTIFACTS_H

#include <cstdlib>
#include <filesystem>
#include <string>

// 返回冒烟产物目录：有 CONCORD_SMOKE_ARTIFACT_DIR 就用它，否则退回调用方给的默认值。
inline std::string runtimeSmokeArtifactDirectory(const std::string& fallback) {
    const char* directory = std::getenv("CONCORD_SMOKE_ARTIFACT_DIR");
    if (directory == nullptr || *directory == '\0') return fallback;
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

inline std::string runtimeSmokeArtifactPath(const std::string& fileName) {
    return (std::filesystem::path(runtimeSmokeArtifactDirectory("build")) / fileName).string();
}

#endif
