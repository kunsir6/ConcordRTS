#include "ui/resources/RuntimeMenuResourceWatch.h"
#include "runtime/config/RuntimeConfig.h"

#include <array>
#include <cstdint>
#include <string>
#include <system_error>

// 变更通知这一族在 macOS 上由 compat 层提供（FindFirstChangeNotificationW 等，
// 实现走 FSEvents），所以这个 TU 在 Apple 上也要拿 windows.h——那是 shim 头，
// 不是真 Windows SDK。反过来"没有通知后端"的平台仍然走下面 #else 的诚实拒绝。
#if defined(_WIN32) || defined(__APPLE__)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;

fs::path absolutePath(const fs::path& path) {
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error);
    return error ? path : absolute.lexically_normal();
}

RuntimeMenuResourcePaths configPaths(const RuntimeConfig& config) {
    // Reuse RuntimeConfig's resolved roots; do not rediscover or reorder them.
    // Like RuntimeConfigPaths, this compatibility input is ACP on Windows.
    return {fs::path(config.resourceRootPath), fs::path(config.localDataRootPath)};
}

struct FileStamp {
    fs::file_time_type modified{};
    std::uintmax_t size = 0;
    bool operator==(const FileStamp&) const = default;
};

FileStamp stamp(const fs::path& path) {
    std::error_code error;
    FileStamp result;
    result.modified = fs::last_write_time(path, error);
    if (!error) result.size = fs::file_size(path, error);
    return result;
}

constexpr std::array<const char*, 4> localInputs{
    "rulesmd.ini", "mpmodesmd.ini", "coopcampmd.ini", "soundmd.ini"
};
} // namespace

struct RuntimeMenuResourceWatch::Impl {
    fs::path resourceRoot;
    fs::path localRoot;
    std::array<FileStamp, localInputs.size()> localStamps{};
#if defined(_WIN32) || defined(__APPLE__)
    std::array<HANDLE, 2> handles{INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
#endif

    explicit Impl(const RuntimeMenuResourcePaths& paths)
        : resourceRoot(absolutePath(paths.resourceRoot)),
          localRoot(absolutePath(paths.localDataRoot)) {
#if defined(_WIN32) || defined(__APPLE__)
        const std::array<fs::path, 2> roots{
            resourceRoot, localRoot / "resources" / "mix_exports"
        };
        for (std::size_t i = 0; i < roots.size(); ++i) {
            // 显式取宽串：Windows 上 path::value_type 就是 wchar_t（与原来
            // c_str() 完全等价），macOS 上是 char，直接传 c_str() 编不过。
            const std::wstring notifyPath = roots[i].wstring();
            handles[i] = FindFirstChangeNotificationW(
                notifyPath.c_str(), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME
                    | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        }
#endif
        for (std::size_t i = 0; i < localInputs.size(); ++i) {
            localStamps[i] = stamp(localRoot / localInputs[i]);
        }
    }

    ~Impl() {
#if defined(_WIN32) || defined(__APPLE__)
        for (HANDLE handle : handles) {
            if (handle != INVALID_HANDLE_VALUE) FindCloseChangeNotification(handle);
        }
#endif
    }

    bool sameRoots(const RuntimeMenuResourcePaths& paths) const {
        return resourceRoot == absolutePath(paths.resourceRoot)
            && localRoot == absolutePath(paths.localDataRoot);
    }

    bool unchanged(const RuntimeMenuResourcePaths& paths) const {
        if (!sameRoots(paths)) return false;
#if defined(_WIN32) || defined(__APPLE__)
        for (HANDLE handle : handles) {
            if (handle == INVALID_HANDLE_VALUE
                || WaitForSingleObject(handle, 0) != WAIT_TIMEOUT) return false;
        }
#else
        // No notification backend for this platform. Do not certify a stale tree.
        return false;
#endif
        for (std::size_t i = 0; i < localInputs.size(); ++i) {
            if (stamp(localRoot / localInputs[i]) != localStamps[i]) return false;
        }
        return true;
    }
};

RuntimeMenuResourceWatch::RuntimeMenuResourceWatch(const RuntimeConfig& config)
    : RuntimeMenuResourceWatch(configPaths(config)) {}
RuntimeMenuResourceWatch::RuntimeMenuResourceWatch(const RuntimeMenuResourcePaths& paths)
    : impl_(std::make_unique<Impl>(paths)) {}
RuntimeMenuResourceWatch::~RuntimeMenuResourceWatch() = default;
bool RuntimeMenuResourceWatch::sameRoots(const RuntimeConfig& config) const {
    return sameRoots(configPaths(config));
}
bool RuntimeMenuResourceWatch::sameRoots(const RuntimeMenuResourcePaths& paths) const {
    return impl_->sameRoots(paths);
}
bool RuntimeMenuResourceWatch::unchanged(const RuntimeConfig& config) const {
    return unchanged(configPaths(config));
}
bool RuntimeMenuResourceWatch::unchanged(const RuntimeMenuResourcePaths& paths) const {
    return impl_->unchanged(paths);
}
