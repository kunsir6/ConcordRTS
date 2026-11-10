// ============================================================================
// compat/win32/src/RuntimeWin32CompatTimer.cpp
//
// S11：Win32 定时器的**真语义**。
//
// 上一版（P5-05）的两个函数是"登记即成功"的桩：SetTimer 什么都不记、KillTimer
// 直接返回 TRUE 而 GetMessageW 只从队列里取消息。结果是产品进菜单后
// `SetTimer(window, 1, 16, nullptr)` 注册的 WM_TIMER **一次都不出现**，
// 菜单动画 / updateDynamicState / ensureMusicPlaying / 音乐 EOF 重启全部静止——
// 现象是"程序活着但什么都不动"（见 queue.rules.wm_timer_never_delivered）。
//
// 这里必须复刻的是三条**容易被忽略**的真实语义，写成桩或写成"入队一条就完事"
// 都会立刻出问题：
//
// 1) WM_TIMER **不是投递进队列的**，它在 GetMessage/PeekMessage **取消息时**才被
//    合成，而且只在队列为空时。做成 PostMessage 会让定时器消息压在真实输入前面，
//    并且攒一堆（队列里有 100 条 WM_TIMER 时菜单会先空转 100 帧才响应鼠标）。
//
// 2) 错过的拍子**不补**。本层在取走一个到期定时器后把 due 推到 now+interval，
//    而不是 due+=interval：真实 USER32 也合并重复的 WM_TIMER（低优先级消息），
//    补拍会让一次长帧之后突然连甩几十条定时器消息。
//
// 3) 同一个 (hWnd, id) 再调 SetTimer 是**重置**那个定时器，不是新增一个。
//    产品在 setInterval 风格的调用里会反复 SetTimer 同一个 id。
//
// 与真实 user32 的差异（刻意的）：
//   · 定时器表是**进程级**而不是线程级——与本层消息队列的既有取舍一致
//     （messageQueue() 也是进程级静态）。多窗口线程共享一个表在本项目里不成立
//     （产品只有一条 UI 线程），但真要多线程就得改成 thread_local。
//   · 时钟用 steady_clock（单调），不用 GetTickCount 的可回绕语义。
// ============================================================================

#include <windows.h>

#include "RuntimeWin32CompatInternal.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using TimerProc = void (*)(HWND, UINT, UINT_PTR, DWORD);

struct CompatTimer {
    HWND hwnd = nullptr;
    UINT_PTR id = 0;
    UINT interval = 0; // 已夹到 [USER_TIMER_MINIMUM, USER_TIMER_MAXIMUM]
    TimerProc proc = nullptr;
    Clock::time_point due{};
};

std::mutex& timerMutex() {
    static std::mutex m;
    return m;
}

std::vector<CompatTimer>& timerTable() {
    static std::vector<CompatTimer> v;
    return v;
}

UINT clampInterval(UINT elapse) {
    // 真实 user32：小于 USER_TIMER_MINIMUM(10ms) 的间隔会被抬到最小值，而不是
    // 失败。抬到 0 会让 GetMessageW 变成忙轮询（每取一条消息都"到期"）。
    if (elapse < USER_TIMER_MINIMUM) return USER_TIMER_MINIMUM;
    if (elapse > USER_TIMER_MAXIMUM) return USER_TIMER_MAXIMUM;
    return elapse;
}

} // namespace

namespace concord_compat {

bool timerCreate(HWND hwnd, UINT_PTR id, UINT elapse, TimerProc proc) {
    if (id == 0) return false; // 真实 API：0 不是合法 id
    const UINT interval = clampInterval(elapse);
    std::lock_guard<std::mutex> lock(timerMutex());
    auto& table = timerTable();
    for (auto& timer : table) {
        if (timer.hwnd == hwnd && timer.id == id) {
            // 重置（真实语义），不是新增第二个。
            timer.interval = interval;
            timer.proc = proc;
            timer.due = Clock::now() + std::chrono::milliseconds(interval);
            return true;
        }
    }
    CompatTimer timer;
    timer.hwnd = hwnd;
    timer.id = id;
    timer.interval = interval;
    timer.proc = proc;
    timer.due = Clock::now() + std::chrono::milliseconds(interval);
    table.push_back(timer);
    return true;
}

bool timerRemove(HWND hwnd, UINT_PTR id) {
    std::lock_guard<std::mutex> lock(timerMutex());
    auto& table = timerTable();
    const auto found = std::find_if(table.begin(), table.end(), [&](const CompatTimer& timer) {
        return timer.hwnd == hwnd && timer.id == id;
    });
    if (found == table.end()) return false;
    table.erase(found);
    return true;
}

bool timerTakeDue(HWND filter, bool consume, TimerDue& out) {
    std::lock_guard<std::mutex> lock(timerMutex());
    auto& table = timerTable();
    const auto now = Clock::now();
    // 取**最早到期**且匹配窗口过滤的那一个。窗口过滤不匹配的定时器保持原样
    // ——把它消费掉等于"替另一个窗口丢了它的一拍"。
    std::size_t best = table.size();
    for (std::size_t index = 0; index < table.size(); ++index) {
        const CompatTimer& timer = table[index];
        if (timer.due > now) continue;
        if (filter != nullptr && timer.hwnd != filter) continue;
        if (best == table.size() || timer.due < table[best].due) best = index;
    }
    if (best == table.size()) return false;
    out.hwnd = table[best].hwnd;
    out.id = table[best].id;
    out.proc = table[best].proc;
    if (consume) {
        // 不补拍：以"现在"为基准重排。see 文件头第 2 条。
        table[best].due = now + std::chrono::milliseconds(table[best].interval);
    }
    return true;
}

long long timerMillisUntilNext() {
    std::lock_guard<std::mutex> lock(timerMutex());
    if (timerTable().empty()) return -1;
    auto earliest = Clock::time_point::max();
    for (const CompatTimer& timer : timerTable()) earliest = std::min(earliest, timer.due);
    const auto now = Clock::now();
    if (earliest <= now) return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now).count();
}

void timerDropForWindow(HWND hwnd) {
    if (hwnd == nullptr) return;
    std::lock_guard<std::mutex> lock(timerMutex());
    auto& table = timerTable();
    table.erase(std::remove_if(table.begin(), table.end(),
                               [&](const CompatTimer& timer) { return timer.hwnd == hwnd; }),
                table.end());
}

std::size_t timerCount() {
    std::lock_guard<std::mutex> lock(timerMutex());
    return timerTable().size();
}

} // namespace concord_compat
