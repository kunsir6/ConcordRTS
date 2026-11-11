// Win32 兼容层 · 目录变更通知族（S12）
//
// 这一族只有一个真实消费者：`ui/resources/RuntimeMenuResourceWatch.cpp`，它用
// `FindFirstChangeNotificationW` + `WaitForSingleObject(h, 0)` 回答"资源树有没有变"，
// 决定能不能复用已经预热的菜单状态。在那之前 macOS 侧的分支是
//
//     #else
//         // No notification backend exists here yet. Do not certify a stale tree.
//         return false;
//     #endif
//
// ——诚实地拒绝认证，代价是**资源复用永远不成立**（遭遇战 preload/reentry 契约必红）。
//
// 为什么用 FSEvents 而不是 kqueue
// ------------------------------
// kqueue 的 EVFILT_VNODE 只能盯**单个 fd**（一层目录），而调用方要的是
// `bWatchSubtree=TRUE`（整棵子树：资源根 + mix_exports 下面好几层）。用 kqueue
// 就得自己递归注册每个目录、还要在目录被替换后重新注册——那是把"内核已经做好的
// 事"再写一遍。FSEvents 天生是子树级、按路径前缀匹配，正好对上。
//
// 有意偏离 Win32 的两处（都写在这里，不藏在实现里）
// ------------------------------------------------
// 1. **非递归请求（bWatchSubtree=FALSE）在本层按递归处理。** FSEvents 没有"只看
//    一层"的模式。多报（子目录里的变化也唤醒）只会让调用方多做一次重载，比漏报
//    安全；调用方当前只传 TRUE，所以这条实际不会被走到。
// 2. **signaled 是"粘性"的**：一旦观察到变更就一直保持 signaled，直到
//    `FindNextChangeNotification` 复位。这与 Windows 语义一致（Windows 也不会
//    自动清），而且调用方从不复位、只靠 `WaitForSingleObject(h, 0)` 轮询。

#include <windows.h>

#include "RuntimeWin32CompatInternal.h"

// FSEvents 是 CoreServices 下的**子框架**，有独立头：
//   <FSEvents/FSEvents.h>（链接 -framework FSEvents 或 CoreServices）
// 不能用 CoreServices 伞头：它会拉进 CarbonCore/Threads.h，那里把
// GetCurrentThread 定义成另一个签名，与本层 windows.h 的
// `HANDLE GetCurrentThread()` 直接冲突，报错信息离真正原因很远。
#include <FSEvents/FSEvents.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

constexpr const char* kChangeNotifyKind = "changenotify";
constexpr std::uint32_t kChangeNotifyMagic = 0x434E4F54u;  // 'CNOT'

struct CompatChangeNotify {
    std::uint32_t magic = kChangeNotifyMagic;
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
    // FSEvents 的回调在 dispatch queue 上跑，与查询线程不同 → 必须原子。
    // 这一个标志就是 Windows 那个"事件对象"的全部内容。
    std::atomic<bool> signaled{false};
};

void fseventsCallback(ConstFSEventStreamRef /*stream*/, void* info, size_t /*count*/,
                      void* /*paths*/, const FSEventStreamEventFlags /*flags*/[],
                      const FSEventStreamEventId /*ids*/[]) {
    auto* watch = static_cast<CompatChangeNotify*>(info);
    if (watch != nullptr) watch->signaled.store(true, std::memory_order_release);
}

void destroyChangeNotify(void* payload) {
    auto* watch = static_cast<CompatChangeNotify*>(payload);
    if (watch == nullptr) return;
    if (watch->stream != nullptr) {
        FSEventStreamStop(watch->stream);
        FSEventStreamInvalidate(watch->stream);
        FSEventStreamRelease(watch->stream);
        watch->stream = nullptr;
    }
    if (watch->queue != nullptr) {
        // SetDispatchQueue 会 retain，这里抵消我们自己的那一份。
        dispatch_release(watch->queue);
        watch->queue = nullptr;
    }
    watch->magic = 0;
    delete watch;
}

CompatChangeNotify* asChangeNotify(HANDLE handle) {
    if (!concord_compat::isRegisteredHandle(reinterpret_cast<void*>(handle), kChangeNotifyKind)) {
        return nullptr;
    }
    return reinterpret_cast<CompatChangeNotify*>(handle);
}

}  // namespace

namespace concord_compat {

bool isChangeNotifyHandle(HANDLE handle) {
    return asChangeNotify(handle) != nullptr;
}

DWORD waitChangeNotify(HANDLE handle, DWORD milliseconds) {
    CompatChangeNotify* watch = asChangeNotify(handle);
    if (watch == nullptr) {
        setLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    const bool infinite = (milliseconds == INFINITE);
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(infinite ? 0 : milliseconds);
    for (;;) {
        if (watch->signaled.load(std::memory_order_acquire)) return WAIT_OBJECT_0;
        if (!infinite && std::chrono::steady_clock::now() >= deadline) return WAIT_TIMEOUT;
        // FSEvents 在 dispatch queue 上异步投递，这里只能轮询标志位。
        // 调用方全是 `WaitForSingleObject(h, 0)` / 几十毫秒级，1ms 轮询足够且不空转。
        ::usleep(1000);
    }
}

}  // namespace concord_compat

extern "C" {

HANDLE FindFirstChangeNotificationW(LPCWSTR lpPathName, BOOL bWatchSubtree, DWORD dwNotifyFilter) {
    (void)dwNotifyFilter;  // FSEvents 不支持按变更类型过滤；一律按"有变化"上报。
    if (lpPathName == nullptr || *lpPathName == L'\0') {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    const std::string path = concord_compat::utf8FromWideText(lpPathName);
    if (::access(path.c_str(), F_OK) != 0) {
        concord_compat::setLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    // Win32 要求这个入参是**目录**，传普通文件会失败。FSEvents 不挑，它会照单
    // 全收下并"正常"watch 一个文件——于是调用方会得到"树是新鲜的"这种错误认证。
    // 实测症状：菜单资源契约的 "regular file cannot establish tree freshness" 恒红。
    // 对齐 Win32：非目录一律拒绝，调用方据此走"不认证、必须重载"。
    {
        struct stat info {};
        if (::stat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
            concord_compat::setLastError(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
        }
    }
    if (!bWatchSubtree) {
        // 见文件头"有意偏离 1"：FSEvents 没有单层模式，按子树处理（多报不漏报）。
        std::fprintf(stderr,
            "[compat/win32] FindFirstChangeNotificationW: bWatchSubtree=FALSE "
            "approximated as recursive (FSEvents has no single-level mode)\n");
    }

    auto* watch = new (std::nothrow) CompatChangeNotify();
    if (watch == nullptr) {
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    watch->queue = dispatch_queue_create("concord.compat.changenotify", DISPATCH_QUEUE_SERIAL);
    if (watch->queue == nullptr) {
        delete watch;
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    CFStringRef pathString = CFStringCreateWithCString(
        kCFAllocatorDefault, path.c_str(), kCFStringEncodingUTF8);
    if (pathString == nullptr) {
        destroyChangeNotify(watch);
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    CFArrayRef paths = CFArrayCreate(kCFAllocatorDefault,
        reinterpret_cast<const void**>(&pathString), 1, &kCFTypeArrayCallBacks);
    CFRelease(pathString);
    if (paths == nullptr) {
        destroyChangeNotify(watch);
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    FSEventStreamContext context{};
    context.info = watch;  // 生命周期由句柄表管理，长于 stream。
    // latency=0：变更后尽快投递。NoDefer 让"第一件事发生就报"，而不是等一个批次。
    // FileEvents 让回调能拿到文件级事件（我们只用"发生了"这一位，但打开它能让
    // 单文件改动也被计入，否则纯文件内容变化可能被目录级事件漏掉）。
    watch->stream = FSEventStreamCreate(kCFAllocatorDefault, &fseventsCallback, &context, paths,
        kFSEventStreamEventIdSinceNow, 0.0,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths);
    if (watch->stream == nullptr) {
        destroyChangeNotify(watch);
        concord_compat::setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    FSEventStreamSetDispatchQueue(watch->stream, watch->queue);
    if (!FSEventStreamStart(watch->stream)) {
        destroyChangeNotify(watch);
        concord_compat::setLastError(ERROR_GEN_FAILURE);
        return INVALID_HANDLE_VALUE;
    }

    // 暖机：FSEvents 是系统级守护进程，启动瞬间可能补投一次与"创建监视"重叠的
    // 事件（实测：刚建好句柄就 WaitForSingleObject(h,0) 是 WAIT_TIMEOUT，但紧接着
    // 的 30ms 内会收到一次，导致"什么都没做却报变了"）。
    // Windows 的语义是"句柄只反映建好之后的变化"，所以把这段暖机期的投递丢掉。
    // 代价：创建后 30ms 内发生的真实变化会被漏掉——调用方创建监视与改动资源之间
    // 隔着整个菜单初始化，这个窗口不可能踩到。
    ::usleep(30 * 1000);
    if (watch->signaled.load(std::memory_order_acquire)) {
        std::fprintf(stderr,
            "[compat/win32] change notification warm-up event discarded for %s\n", path.c_str());
        watch->signaled.store(false, std::memory_order_release);
    }

    if (!concord_compat::registerHandle(watch, destroyChangeNotify, kChangeNotifyKind)) {
        destroyChangeNotify(watch);
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<HANDLE>(watch);
}

BOOL FindNextChangeNotification(HANDLE hChangeHandle) {
    CompatChangeNotify* watch = asChangeNotify(hChangeHandle);
    if (watch == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    // Windows 语义：复位为未 signaled，继续等下一次变更。调用方当前不用它，
    // 但少了它，"粘性 signaled"就没有复位手段，用久了会变成恒真。
    watch->signaled.store(false, std::memory_order_release);
    return TRUE;
}

BOOL FindCloseChangeNotification(HANDLE hChangeHandle) {
    if (asChangeNotify(hChangeHandle) == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    concord_compat::closeRegisteredHandle(reinterpret_cast<void*>(hChangeHandle));
    return TRUE;
}

}  // extern "C"
