// ============================================================================
// compat/win32/selftest/CompatTimerDeliverySemantics.cpp
//
// S11：定时器**真的会被派发**（不是"登记即成功"）。
//
// 这一层不能用"返回值对了"来验证：上一版 SetTimer 直接 return TRUE，产品照样
// 一路跑进菜单，只是菜单永远不动。所以这里的判据全部是**计数**——在一个真实的
// GetMessageW 循环里数 WM_TIMER 到了几条、间隔对不对、停了之后是不是真的不再来。
//
// 覆盖的真实语义（每一条都有一个"看起来对"的错法）：
//   1. 16ms 定时器在 1 秒里产生 ≥50 条 WM_TIMER（理论 62）。
//   2. 每条消息的 hwnd / wParam(=定时器 id) / message 都正确。
//   3. KillTimer 之后**一条都不再来**（不是"少一些"）。
//   4. 错过的拍子不补：1 秒内的总数不超过上限（补拍会翻倍）。
//   5. 回调型定时器（lpTimerFunc 非空）走回调、**不产生** WM_TIMER。
//   6. 同一个 (hwnd, id) 重复 SetTimer 是重置，不是新增（表里仍然只有一个）。
//   7. PeekMessageW(PM_REMOVE) 同样能取到 WM_TIMER；PM_NOREMOVE 取不到合成消息。
//   8. WM_PAINT 也在取消息时合成：InvalidateRect 之后必然收到一条，BeginPaint
//      之后不再重复（否则菜单会在"脏"上死循环）。
//   9. DestroyWindow 把该窗口的定时器一起删掉（否则每 16ms 往死窗口派发）。
//  10. 间隔小于 USER_TIMER_MINIMUM(10ms) 被抬到 10ms，而不是变成 1ms 忙轮询。
//
// 构建与运行见 cmake/PlatformApple.cmake 的 concord_win32_compat_timer_selftest。
// ============================================================================

#include <windows.h>

#include "RuntimeWin32CompatInternal.h"

#include <chrono>
#include <cstdio>
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

struct Counters {
    int timerMessages = 0;
    int wrongId = 0;
    int wrongWindow = 0;
    int timerCallbacks = 0;
    int paintMessages = 0;
    bool sawQuit = false;
};

Counters gCounters;
HWND gWindow = nullptr;

LRESULT CALLBACK windowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER:
        ++gCounters.timerMessages;
        if (wParam != 1 && wParam != 9 && wParam != 12) ++gCounters.wrongId;
        if (hWnd != gWindow) ++gCounters.wrongWindow;
        // 每次 tick 弄脏一次，顺带把"定时器 → 重绘"这条链走通（真实菜单就是这样）。
        InvalidateRect(gWindow, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        ++gCounters.paintMessages;
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hWnd, &paint);
        if (dc != nullptr) EndPaint(hWnd, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
}

void timerCallback(HWND, UINT, UINT_PTR, DWORD) { ++gCounters.timerCallbacks; }

// 跑真实的消息循环若干毫秒，返回期间收到的 WM_TIMER 条数。
int pumpMilliseconds(int milliseconds, int stopAfterTimerMessages = -1) {
    const int before = gCounters.timerMessages;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    MSG message{};
    while (std::chrono::steady_clock::now() < deadline) {
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) break;
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (stopAfterTimerMessages > 0 && gCounters.timerMessages - before >= stopAfterTimerMessages) break;
    }
    return gCounters.timerMessages - before;
}

// Peek 版本：不阻塞，把当前排队的消息抽干。
int drainPeek() {
    const int before = gCounters.timerMessages;
    MSG message{};
    for (int guard = 0; guard < 10000; ++guard) {
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) break;
        DispatchMessageW(&message);
    }
    return gCounters.timerMessages - before;
}

// "应当什么都没有"的场合**不能**用 GetMessageW：(真实 API 与这层实现都）会一直
// 阻塞到有消息为止，而这条断言的前提恰恰是"没有消息"。第一版就是这样写的，
// 结果自测在 KillTimer 之后永久挂住、连一行输出都没有——判据本身把测试变成了
// 死锁。这里改用 Peek 轮询固定时长。
int peekForMilliseconds(int milliseconds) {
    const int before = gCounters.timerMessages;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    MSG message{};
    while (std::chrono::steady_clock::now() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return gCounters.timerMessages - before;
}

HWND createTestWindow() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &windowProc;
    windowClass.hInstance = nullptr;
    windowClass.lpszClassName = L"ConcordTimerSelftestWindow";
    if (RegisterClassExW(&windowClass) == 0) return nullptr;
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"timer", WS_OVERLAPPEDWINDOW,
                                  0, 0, 800, 600, nullptr, nullptr, nullptr, nullptr);
    if (window == nullptr) return nullptr;
    gWindow = window;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return window;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    HWND window = createTestWindow();
    check(window != nullptr, "test window created");
    if (window == nullptr) {
        std::printf("Timer delivery: checks=%d failures=%d\n", gChecks, gFailures);
        return 1;
    }

    // --- 1/2/4. 16ms 定时器在 1 秒里应当到 ~62 条 ---
    check(SetTimer(window, 1, 16, nullptr) == TRUE, "SetTimer(16ms) returns TRUE");
    check(concord_compat::timerCount() == 1, "one timer is registered in the table");
    const int firstSecond = pumpMilliseconds(1000);
    std::printf("  16ms timer in 1s: %d messages\n", firstSecond);
    check(firstSecond >= 50, "16ms timer delivers >= 50 WM_TIMER in one second");
    check(firstSecond <= 90, "missed ticks are coalesced, not replayed (<= 90 in one second)");
    check(gCounters.wrongId == 0, "every WM_TIMER carries the timer id in wParam");
    check(gCounters.wrongWindow == 0, "every WM_TIMER carries the timer's hwnd");
    check(gCounters.paintMessages > 0, "the invalidation raised by each tick actually repaints");

    // --- 3. KillTimer 之后一条都不再来 ---
    check(KillTimer(window, 1) == TRUE, "KillTimer removes a registered timer");
    check(concord_compat::timerCount() == 0, "the timer table is empty after KillTimer");
    check(peekForMilliseconds(200) == 0, "no WM_TIMER arrives after KillTimer");
    check(KillTimer(window, 1) == FALSE, "KillTimer on an unknown id reports FALSE");

    // --- 5. 回调型定时器：走回调、不产生消息 ---
    gCounters.timerMessages = 0;
    check(SetTimer(window, 7, 16, &timerCallback) == TRUE, "callback timer registered");
    // 回调型定时器**不产生消息**，所以这里不能用 GetMessageW 驱动——那会一直阻塞
    // 等一条永远不会出现的消息（真实 Win32 里 GetMessage 也是如此：回调在等待期间
    // 由系统调用，GetMessage 本身不返回）。用 Peek 轮询来观测回调计数。
    const int duringCallback = peekForMilliseconds(500);
    std::printf("  callback timer in 0.5s: %d callbacks, %d messages\n", gCounters.timerCallbacks,
                duringCallback);
    check(gCounters.timerCallbacks >= 15, "callback timer fires >= 15 times in 0.5s");
    check(duringCallback == 0, "a callback timer produces no WM_TIMER message");
    check(KillTimer(window, 7) == TRUE, "callback timer removed");

    // --- 6. 同一 (hwnd,id) 重复 SetTimer 是重置 ---
    check(SetTimer(window, 9, 16, nullptr) == TRUE, "timer 9 registered at 16ms");
    check(SetTimer(window, 9, 100, nullptr) == TRUE, "timer 9 re-registered at 100ms");
    check(concord_compat::timerCount() == 1, "re-registering the same id does not add a second timer");
    const int resetWindow = pumpMilliseconds(250);
    std::printf("  timer 9 after reset to 100ms: %d messages in 250ms\n", resetWindow);
    check(resetWindow <= 4, "the reset interval (100ms) wins over the original 16ms");
    check(resetWindow >= 1, "the reset timer still fires");
    check(KillTimer(window, 9) == TRUE, "timer 9 removed");

    // --- 7. PeekMessage 也能取到 WM_TIMER，但 PM_NOREMOVE 看不到 ---
    gCounters.timerMessages = 0;
    check(SetTimer(window, 1, 20, nullptr) == TRUE, "timer 1 registered for the peek test");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    MSG peek{};
    check(PeekMessageW(&peek, nullptr, 0, 0, PM_NOREMOVE) == FALSE,
          "PM_NOREMOVE does not report synthesized messages (they have no queue slot)");
    const int peeked = drainPeek();
    std::printf("  PeekMessage(PM_REMOVE) drained %d timer messages\n", peeked);
    check(peeked >= 1, "PeekMessage(PM_REMOVE) delivers a due WM_TIMER");
    check(KillTimer(window, 1) == TRUE, "timer 1 removed after the peek test");

    // --- 8. WM_PAINT 合成：脏一次画一次 ---
    {
        int paints = gCounters.paintMessages;
        InvalidateRect(window, nullptr, FALSE);
        MSG message{};
        check(GetMessageW(&message, nullptr, 0, 0) == 1 && message.message == WM_PAINT,
              "a dirty window yields exactly one synthesized WM_PAINT");
        check(message.hwnd == window, "the synthesized WM_PAINT targets the dirty window");
        DispatchMessageW(&message);
        check(gCounters.paintMessages == paints + 1, "the WM_PAINT reached the window procedure");
        check(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) == FALSE,
              "BeginPaint cleared the dirty flag: no second WM_PAINT");
    }

    // --- 10. 间隔夹紧到 USER_TIMER_MINIMUM ---
    gCounters.timerMessages = 0;
    check(SetTimer(window, 12, 1, nullptr) == TRUE, "SetTimer(1ms) is accepted");
    const int clamped = pumpMilliseconds(200);
    std::printf("  SetTimer(1ms) yielded %d messages in 200ms\n", clamped);
    check(clamped <= 30, "1ms is clamped to USER_TIMER_MINIMUM (10ms), not a 1ms busy loop");
    check(clamped >= 8, "the clamped timer still fires at roughly 10ms");
    check(KillTimer(window, 12) == TRUE, "clamped timer removed");

    // --- 9. DestroyWindow 连带清掉定时器 ---
    check(SetTimer(window, 11, 16, nullptr) == TRUE, "timer 11 registered before DestroyWindow");
    check(DestroyWindow(window) == TRUE, "window destroyed");
    check(concord_compat::timerCount() == 0, "DestroyWindow dropped the window's timers");

    std::printf("Timer delivery: checks=%d failures=%d\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
