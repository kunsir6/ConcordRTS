// Win32 兼容层：文件族（S10 下半）—— CreateFileW（宽/窄两版）+ WriteFile
//
// 为什么这一族值得单独一轮而不是"顺手补上"：
//
//   1. `CreateFileW` 是整个兼容层里唯一**同时有两个入口**的函数。项目里
//      `CreateFileW(path.c_str(), ...)` 在 Windows 上匹配宽版，在本平台
//      `path.c_str()` 是 `const char*`，只匹配窄版。窄版不是"方便用的糖"，
//      它是原子存档链路（保存游戏）真正走的那个入口。
//
//   2. 失败必须带上**正确的错误码**。调用点的判据不是"成功没成功"，而是
//      `GetLastError() == ERROR_FILE_EXISTS`（换一个临时名重试）与
//      `== ERROR_ACCESS_DENIED`（再查一次属性，存在就重试）。错误码若一律给
//      0 或一律给 ACCESS_DENIED，重试循环要么永不重试、要么空转 8 次。
//
//   3. `dwShareMode` 无法实现。POSIX 没有共享模式，这里**接受但忽略**——
//      刻意不假装"已按共享模式打开"，因为没有任何观测点能区分，而假装会让
//      将来真的需要排除性打开时找不到线索。
//
// 句柄形态：返回一个堆对象指针。不用 `fd` 直接当 HANDLE（`(void*)(intptr_t)fd`），
// 理由不是洁癖——fd 是小整数，`(void*)3` 这种值既可能撞上 nullptr 附近，
// 也无法与"别的族恰好也用小整数编码的句柄"区分。

#include <windows.h>

#include "RuntimeWin32CompatInternal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

// 文件对象本体。句柄就是这个对象的地址（见文件头最后一段）。
struct FileHandleObject {
    int fd = -1;
    bool readable = false;
    bool writable = false;
    std::string path;  // 只用于诊断输出，不参与任何判据
};

void destroyFileHandle(void* payload) {
    auto* object = static_cast<FileHandleObject*>(payload);
    if (object->fd >= 0) ::close(object->fd);
    delete object;
}

// 把任意 HANDLE 解释成文件对象之前，先证明它**真是**本族的句柄。
// 顺序不能反：`reinterpret_cast` 之后再检查，检查本身就已经越界了。
FileHandleObject* asFileHandle(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    if (!concord_compat::isRegisteredHandle(handle, "file")) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    return reinterpret_cast<FileHandleObject*>(handle);
}

// dwCreationDisposition + dwDesiredAccess → open(2) 的 flags。
// 返回 false 表示这组组合在 Win32 里本身就是非法参数（例如只读打开 + 截断）。
bool openFlagsFor(DWORD desiredAccess, DWORD creationDisposition, int& flags, DWORD& error) {
    const int access = ((desiredAccess & GENERIC_READ) != 0)
        ? (((desiredAccess & GENERIC_WRITE) != 0) ? O_RDWR : O_RDONLY)
        : (((desiredAccess & GENERIC_WRITE) != 0) ? O_WRONLY : O_RDONLY);

    int creation = 0;
    switch (creationDisposition) {
        case CREATE_NEW:        creation = O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS:     creation = O_CREAT | O_TRUNC; break;
        case OPEN_EXISTING:     creation = 0; break;
        case OPEN_ALWAYS:       creation = O_CREAT; break;
        case TRUNCATE_EXISTING: creation = O_TRUNC; break;
        default:
            error = ERROR_INVALID_PARAMETER;
            return false;
    }
    if (creationDisposition == TRUNCATE_EXISTING && (desiredAccess & GENERIC_WRITE) == 0) {
        // 只读 + 截断在 Win32 上是 ERROR_INVALID_PARAMETER。若照抄成 O_TRUNC，
        // 只读 fd 上的 O_TRUNC 在部分系统上会被静默忽略、在另一些上真的截断——
        // 两种结果都不该发生。
        error = ERROR_INVALID_PARAMETER;
        return false;
    }
    flags = access | creation | O_CLOEXEC;
    return true;
}

HANDLE openFile(const char* utf8Path, DWORD desiredAccess, DWORD creationDisposition) {
    if (utf8Path == nullptr || utf8Path[0] == '\0') {
        // Windows 对空文件名给 ERROR_PATH_NOT_FOUND（不是 INVALID_PARAMETER）。
        concord_compat::setLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    int flags = 0;
    DWORD parameterError = 0;
    if (!openFlagsFor(desiredAccess, creationDisposition, flags, parameterError)) {
        concord_compat::setLastError(parameterError);
        return INVALID_HANDLE_VALUE;
    }
    // 0600 与 RuntimeAtomicFile 的 POSIX 分支保持一致：存档不做"同组可写"。
    // O_CLOEXEC 对应 Windows 上"SECURITY_ATTRIBUTES 为空 ⇒ 不可继承"。
    const int fd = ::open(utf8Path, flags, static_cast<mode_t>(0600));
    if (fd < 0) {
        concord_compat::setLastErrorFromErrno(errno);
        return INVALID_HANDLE_VALUE;
    }
    auto* object = new FileHandleObject();
    object->fd = fd;
    object->readable = (flags & O_ACCMODE) != O_WRONLY;
    object->writable = (flags & O_ACCMODE) != O_RDONLY;
    object->path = utf8Path;
    if (!concord_compat::registerHandle(object, destroyFileHandle, "file")) {
        // 同一个地址被登记两次只可能是生命周期管理已经错了。这里宁可失败关闭，
        // 也不留下一个"关不掉的 fd"。
        destroyFileHandle(object);
        concord_compat::setLastError(ERROR_GEN_FAILURE);
        return INVALID_HANDLE_VALUE;
    }
    return static_cast<HANDLE>(object);
}

}  // namespace

extern "C" {

HANDLE CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   SECURITY_ATTRIBUTES* lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    (void)dwShareMode;         // POSIX 没有共享模式，见文件头第 3 点
    (void)lpSecurityAttributes; // 见 openFile 里的 O_CLOEXEC 注释
    (void)dwFlagsAndAttributes; // FILE_ATTRIBUTE_* 只影响创建后的属性，不影响打开
    (void)hTemplateFile;
    if (lpFileName == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    const std::string path = concord_compat::utf8FromWideText(lpFileName);
    return openFile(path.c_str(), dwDesiredAccess, dwCreationDisposition);
}

BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
               LPDWORD lpNumberOfBytesWritten, LPVOID lpOverlapped) {
    if (lpNumberOfBytesWritten != nullptr) *lpNumberOfBytesWritten = 0;
    // 本层没有重叠（异步）I/O。假装接受会让调用方等一个永远不来的完成通知。
    if (lpOverlapped != nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    FileHandleObject* file = asFileHandle(hFile);
    if (file == nullptr) return FALSE;
    if (!file->writable) {
        concord_compat::setLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (lpBuffer == nullptr && nNumberOfBytesToWrite > 0) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    const auto* bytes = static_cast<const char*>(lpBuffer);
    std::size_t total = 0;
    const std::size_t requested = nNumberOfBytesToWrite;
    while (total < requested) {
        const ssize_t written = ::write(file->fd, bytes + total, requested - total);
        if (written < 0) {
            if (errno == EINTR) continue;  // 信号打断不算失败，继续写剩下的
            // 部分写入也算失败，但已写字节数必须如实回报——调用方的重试
            // 逻辑依赖它，报 0 会导致重复写入。
            if (lpNumberOfBytesWritten != nullptr) {
                *lpNumberOfBytesWritten = static_cast<DWORD>(total);
            }
            concord_compat::setLastErrorFromErrno(errno);
            return FALSE;
        }
        if (written == 0) {
            // 对普通文件 write 返回 0 意味着"写不进去"，最常见是磁盘满。
            if (lpNumberOfBytesWritten != nullptr) {
                *lpNumberOfBytesWritten = static_cast<DWORD>(total);
            }
            concord_compat::setLastError(ERROR_DISK_FULL);
            return FALSE;
        }
        total += static_cast<std::size_t>(written);
    }
    if (lpNumberOfBytesWritten != nullptr) {
        *lpNumberOfBytesWritten = static_cast<DWORD>(total);
    }
    // 刻意不 fsync：Windows 的 WriteFile 也不保证落盘，落盘是 FlushFileBuffers。
    // 在这里偷偷 fsync 会把存档路径的耗时放大一个数量级，而且掩盖真实语义。
    return TRUE;
}

}  // extern "C"

// ---------------------------------------------------------------------------
// 窄路径入口（C++ 链接，声明在 windows.h 末尾）
// ---------------------------------------------------------------------------
// `std::filesystem::path::c_str()` 在本平台返回 `const char*`，其编码就是本平台
// 的原生路径编码（macOS 为 UTF-8）。所以这里不需要任何转码——直接当 UTF-8 用，
// 与宽版走同一个 openFile，语义上唯一的差别就是"文件名已经是窄字符"。
HANDLE CreateFileW(const char* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   SECURITY_ATTRIBUTES* lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    (void)dwShareMode;
    (void)lpSecurityAttributes;
    (void)dwFlagsAndAttributes;
    (void)hTemplateFile;
    return openFile(lpFileName, dwDesiredAccess, dwCreationDisposition);
}
