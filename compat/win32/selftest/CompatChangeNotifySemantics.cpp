// compat/win32 自测 · 目录变更通知（S12）
//
// 这一族只有一个消费者（RuntimeMenuResourceWatch），但它的答案决定了"资源能不能
// 复用"：答"没变"就会拿旧资源继续跑，答"变了"就重载。所以语义必须钉死：
//   · 未变更时 `WaitForSingleObject(h, 0)` 必须是 WAIT_TIMEOUT（**不能**假阳性，
//     否则每次进菜单都重载，预热等于白做）；
//   · 变更后必须是 WAIT_OBJECT_0，而且**保持** signaled（Windows 语义，调用方
//     从不复位，只轮询 0 超时）；
//   · `FindNextChangeNotification` 必须能把状态复回 WAIT_TIMEOUT；
//   · `bWatchSubtree=TRUE` 时**子目录**里的改动也要命中（kqueue 单层做不到，
//     这正是本层用 FSEvents 的原因）；
//   · 路径不存在要返回 INVALID_HANDLE_VALUE（而不是一个永远不 signaled 的句柄，
//     后者会让调用方一直认为"资源没变"）。
//
// FSEvents 是异步投递，所以断言里都用有限等待（最多 3s），不假设"立刻可见"。

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

int gChecks = 0;
int gFailures = 0;

void check(bool condition, const char* description) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::printf("  FAIL  %s\n", description);
    }
}

void checkWait(DWORD expected, DWORD actual, const char* description) {
    ++gChecks;
    if (expected != actual) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected %lu, got %lu)\n", description,
                    static_cast<unsigned long>(expected), static_cast<unsigned long>(actual));
    }
}

// FSEvents 回调在 dispatch queue 上，投递有延迟；用有限轮询等待"变成 signaled"。
bool waitSignaled(HANDLE watch, int timeoutMs) {
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 5) {
        if (WaitForSingleObject(watch, 0) == WAIT_OBJECT_0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::wstring wideFromUtf8(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (const char c : text) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return out;
}

void testMissingPath() {
    std::printf("[1] 路径不存在必须直接失败\n");

    const std::wstring missing = wideFromUtf8("/tmp/concord_change_notify_does_not_exist");
    SetLastError(0);
    HANDLE watch = FindFirstChangeNotificationW(missing.c_str(), TRUE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    check(watch == INVALID_HANDLE_VALUE,
          "missing path returns INVALID_HANDLE_VALUE (not a dead handle)");
    check(GetLastError() == ERROR_PATH_NOT_FOUND, "missing path sets ERROR_PATH_NOT_FOUND");
}

void testQuietThenChanged() {
    std::printf("[2] 未变更 → WAIT_TIMEOUT；变更 → WAIT_OBJECT_0 且粘性\n");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "concord_change_notify_test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "nested", error);
    {
        std::ofstream seed(root / "seed.ini");
        seed << "[X]\n";
    }

    const std::wstring wideRoot = wideFromUtf8(root.string());
    HANDLE watch = FindFirstChangeNotificationW(wideRoot.c_str(), TRUE,
                                                FILE_NOTIFY_CHANGE_FILE_NAME
                                                    | FILE_NOTIFY_CHANGE_SIZE
                                                    | FILE_NOTIFY_CHANGE_LAST_WRITE);
    check(watch != INVALID_HANDLE_VALUE, "watch on existing tree succeeds");
    if (watch == INVALID_HANDLE_VALUE) return;

    // 静默：刚建好的监视不能立刻报"变了"。
    checkWait(WAIT_TIMEOUT, WaitForSingleObject(watch, 0),
              "fresh watch reports WAIT_TIMEOUT (no false positive)");
    checkWait(WAIT_TIMEOUT, WaitForSingleObject(watch, 30),
              "blocking wait times out when nothing changed");

    // 子目录里改一个文件：子树监视必须命中。
    {
        std::ofstream edit(root / "nested" / "inner.ini", std::ios::app);
        edit << "[Y]\n";
    }
    check(waitSignaled(watch, 3000),
          "change inside a nested directory signals the watch (subtree)");

    // 粘性：不清就一直 signaled —— 调用方只做 0 超时轮询。
    checkWait(WAIT_OBJECT_0, WaitForSingleObject(watch, 0), "signaled state is sticky (1)");
    checkWait(WAIT_OBJECT_0, WaitForSingleObject(watch, 0), "signaled state is sticky (2)");

    // 复位后必须回到未 signaled。
    check(FindNextChangeNotification(watch) == TRUE, "FindNextChangeNotification succeeds");
    checkWait(WAIT_TIMEOUT, WaitForSingleObject(watch, 0),
              "after FindNextChangeNotification the watch is quiet again");

    check(FindCloseChangeNotification(watch) == TRUE, "FindCloseChangeNotification succeeds");
    check(FindCloseChangeNotification(watch) == FALSE,
          "closing twice reports FALSE (handle already gone)");
    checkWait(WAIT_FAILED, WaitForSingleObject(watch, 0),
              "waiting on a closed change handle fails (does not impersonate success)");

    std::filesystem::remove_all(root, error);
}

}  // namespace

int main() {
    std::printf("compat/win32 目录变更通知语义自测\n\n");

    testMissingPath();
    testQuietThenChanged();

    std::printf("\nChangeNotifySemantics: checks=%d failures=%d\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
