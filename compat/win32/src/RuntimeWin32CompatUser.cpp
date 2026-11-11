// ---------------------------------------------------------------------------
// 窗口 / 消息 / 输入后端（S7）——纯离屏路线
// ---------------------------------------------------------------------------
//
// 设计立场：macOS 上**不开真窗口**，只在进程内维护一套"逻辑窗口对象模型"——
// HWND 是指向 CompatWindow 的指针（靠句柄表 + magic 双重验证），消息队列是
// 线程级的一等对象，窗口 DC 是选入客户区位图的离屏 CompatDevice。
//
// 为什么不开真窗口（本轮）：前面的呈现路径已经是 software-present=ON 的
// 软件帧缓冲，MapNativeOverlay 也按"原生像素、不缩放"渲染。菜单系统要的是
// "创建窗口 → 收消息 → 画到客户区 → 销毁"这条链走通，而不是真的开一个
// NSWindow。等这条链验证通过，再叠一个 Cocoa NSView 把离屏帧贴出来显示即可，
// 窗口对象模型一行都不用改。
//
// 诚实边界（与前面小族一致：看效果不看 API 名）：
//   · 定时器（SetTimer）仍是 Input.cpp 的登记桩——本轮不生成 WM_TIMER，消息
//     循环靠短 sleep 轮询。真正的"虚拟时钟驱动动画"是下一块硬活。
//   · 键盘/鼠标事件无真实宿主来源，SendInput/GetAsyncKeyState 保持诚实 0。
//   · MessageBoxW 不开对话框，恒返回 IDOK 并打日志（诊断 smoke 只看返回值）。
//   · 钩子只在"本线程 + WH_CBT + HCBT_CREATEWND/DESTROYWND"上真实实现。

#include <windows.h>

#include "RuntimeWin32CompatInternal.h"
#include "RuntimeAppleWindowSurface.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// 窗口对象模型
// ---------------------------------------------------------------------------

namespace {

// 纯离屏虚拟屏幕（工作区）。菜单按 SPI_GETWORKAREA 取工作区来居中窗口，
// 这里给出一个稳定的 1024×768 桌面，让"窗口落在屏幕内"的断言成立。
constexpr int kVirtualScreenW = 1024;
constexpr int kVirtualScreenH = 768;

// 窗口对象的 magic。HWND 是指向本结构体的指针；magic + 句柄表双验证防止把
// 野指针当窗口解引用。
constexpr std::uint32_t kWndMagic = 0x574E4400u;  // "WND\0"

struct CompatWindow {
    std::uint32_t magic = kWndMagic;

    std::wstring className;   // 注册时的类名（GetClassNameW 回填）
    std::wstring title;       // 窗口标题（SetWindowTextW 设置）
    DWORD style = 0;
    DWORD exStyle = 0;

    RECT windowRect{0, 0, 0, 0};  // 屏幕坐标，含边框。纯离屏无边框装饰，
                                  // 故窗口矩形 == 客户区矩形按 (x,y) 平移。
    bool visible = false;
    bool iconic = false;
    bool zoomed = false;
    bool enabled = true;

    HWND parent = nullptr;
    HMENU menu = nullptr;
    HINSTANCE instance = nullptr;

    LONG_PTR userData = 0;    // GWLP_USERDATA（WM_NCCREATE 时窗口过程写入）
    WNDPROC wndProc = nullptr;  // 来自窗口类 lpfnWndProc
    // 类光标（WNDCLASSEXW::hCursor 的副本）。DefWindowProc(WM_SETCURSOR) 用它。
    // 内建类（EDIT 等）由系统提供，不走 RegisterClassExW，所以这一项在
    // CreateWindowExW 里按类名填。
    HCURSOR cursor = nullptr;

    HDC dc = nullptr;         // 缓存窗口 DC（客户区位图已选入）
    HBITMAP surface = nullptr;  // 客户区位图（32bpp top-down DIB）
    // 客户区位图的实际尺寸。U-14：窗口被全屏/缩放改成别的尺寸时靠它判断
    // "要不要重建位图"——不能只看 dc/surface 非空。
    int surfaceWidth = 0;
    int surfaceHeight = 0;

    bool dirty = false;       // InvalidateRect 置位，BeginPaint 清位
    bool destroying = false;

    std::vector<HWND> children;                 // 子窗口
    std::unordered_map<std::wstring, HANDLE> props;  // SetProp/GetProp
};

std::mutex& userMutex() { static std::mutex m; return m; }

std::vector<CompatWindow*>& windowTable() { static std::vector<CompatWindow*> v; return v; }

std::unordered_map<std::wstring, WNDCLASSEXW>& classTable() {
    static std::unordered_map<std::wstring, WNDCLASSEXW> v;
    return v;
}

std::deque<MSG>& messageQueue() { static std::deque<MSG> v; return v; }

HWND& focusWindow() { static HWND h = nullptr; return h; }
HWND& foregroundWindow() { static HWND h = nullptr; return h; }

// WH_CBT 钩子（本轮只支持本线程 + WH_CBT + CREATEWND/DESTROYWND）
struct HookEntry {
    int id = 0;
    HOOKPROC proc = nullptr;
    DWORD threadId = 0;
};
std::vector<HookEntry>& hookTable() { static std::vector<HookEntry> v; return v; }
int& nextHookId() { static int v = 1; return v; }

// ---------------------------------------------------------------------------
// ON-2-02a：呈现节奏计数（只计数，不改任何呈现行为）
//
// 为什么先计数再谈"脏帧合并"：ON-2-02 的原始 detail 里有两句是**假设**——
// 「每个 EndPaint 都直接 present」「同一帧可能被贴多次」。2026-11-11 06:44 的
// 读码诊断认为两句都不成立（present 全仓只有一个调用点，失效层已经用 dirty
// 标志合并），但那也是读码结论、不是实测。这里把两件事变成可复算的数字：
//   · paints 按**来源**分类：队列合成 / UpdateWindow / RedrawWindow(RDW_UPDATENOW)
//     / 应用直投（SendMessageW）。四类之和必须等于 paintCalls，否则口径错了。
//   · submits 与 redundant 的比值：同一窗口、同尺寸、同可见性、采样签名相同的
//     连续提交，就是"同一帧被贴两次"的直接证据；反之 redundant==0 就是
//     "present 层不需要再合并"的反证。
//
// 零开销约束：CONCORD_PRESENT_STATS 未设时每个钩子只多一次 bool 读，且不产生
// 任何输出、不写文件（stderr 一条汇总行即够对账，不引入新的磁盘依赖）。
//
// 采样签名的局限：签名只取每 4 行 × 每 8 列（1/32 像素），所以 redundant 是
// **上界**——小于采样格的局部变化会被算成"没变"。反向不会发生（变了绝不可能
// 被算成没变），用它判"存不存在重复提交"是安全的；用它判"具体重复了几帧"会偏低。
// ---------------------------------------------------------------------------
struct PresentStatsCounters {
    long long paintCalls = 0;              // callWindowProc(WM_PAINT) 总数（分发口径）
    long long paintViaQueue = 0;           // 经 DispatchMessageW（含 popMessage 合成）
    long long paintFromUpdateWindow = 0;   // UpdateWindow 的同步旁路
    long long paintFromRedrawNow = 0;      // RedrawWindow(RDW_UPDATENOW) 的同步旁路
    long long endPaintCalls = 0;
    long long dirtyMarks = 0;              // InvalidateRect / RedrawWindow 置脏次数
    long long dirtyMarksMerged = 0;        // 置脏时**已经是脏的** → 被失效层合并
    long long presentAttempts = 0;         // 进入 maybePresentClientFrame 的次数
    long long skippedBlank = 0;            // ON-2-05 的空白首绘过滤
    long long skippedTakeover = 0;         // 抢位规则拒绝（小控件 / 非主人窗口）
    long long skippedTakeoverSelf = 0;     // 其中：拒的是**当前呈现主人自己**（画了但不让上屏）
    long long skippedTakeoverOther = 0;    // 其中：拒的是别的窗口（小控件 / 并列窗口）
    long long submits = 0;                 // 真正调用 apple::present 的次数
    long long redundantSubmits = 0;        // 与上一次提交完全相同（窗口/尺寸/可见性/签名）
};

// 第一次见到某个窗口重绘时打一行（上限 8 个）：只有知道"谁在画、多大"，
// 才能判断被抢位规则拒掉的是主人自己还是旁边的小控件。两者后果完全不同：
// 前者=屏幕冻在第一帧，后者=完全正常。
std::vector<HWND>& presentStatsSeenWindows() { static std::vector<HWND> v; return v; }

bool presentStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONCORD_PRESENT_STATS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void presentStatsNoteWindow(HWND handle, int width, int height) {
    if (!presentStatsEnabled()) return;
    std::vector<HWND>& seen = presentStatsSeenWindows();
    for (HWND entry : seen) {
        if (entry == handle) return;
    }
    if (seen.size() >= 8) return;
    seen.push_back(handle);
    std::fprintf(stderr, "[present-stats] paint-window #%zu hwnd=%p size=%dx%d\n",
                 seen.size(), static_cast<void*>(handle), width, height);
    std::fflush(stderr);
}

int presentStatsUntilSec() {
    static const int seconds = [] {
        const char* value = std::getenv("CONCORD_PRESENT_STATS_UNTIL_SEC");
        return value != nullptr ? std::atoi(value) : 0;
    }();
    return seconds;
}

PresentStatsCounters& presentStats() { static PresentStatsCounters counters; return counters; }

// 计时起点取**第一次取消息**的时刻（也就是主循环真正起来的时候），而不是静态
// 初始化时刻——后者会把动态链接/静态构造的时间也算进"运行时长"。
std::chrono::steady_clock::time_point& presentStatsStart() {
    static std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    return start;
}

double presentStatsElapsedSeconds() {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - presentStatsStart()).count();
}

// 当前上屏那一幅画的尺寸。与后面的 gPresentedArea 同源，单独留一份是为了让
// 汇总行能直接打出"屏幕上是多大的一幅"，而不用反算面积。
int gPresentedWidth = 0;
int gPresentedHeight = 0;

void presentStatsPrint(const char* reason) {
    if (!presentStatsEnabled()) return;
    const PresentStatsCounters& s = presentStats();
    // 应用直投 = 总数减去三条已知路径；负数说明某个入口被重复计数，直接报 -1 让人看见。
    const long long known = s.paintViaQueue + s.paintFromUpdateWindow + s.paintFromRedrawNow;
    const long long appDirect = s.paintCalls >= known ? s.paintCalls - known : -1;
    const double elapsed = presentStatsElapsedSeconds();
    std::fprintf(stderr,
        "[present-stats] reason=%s elapsed=%.2fs paints=%lld"
        " {queue=%lld update=%lld redraw=%lld app=%lld}"
        " endpaint=%lld dirtyMarks=%lld merged=%lld"
        " attempts=%lld blank=%lld takeover=%lld {self=%lld other=%lld}"
        " submits=%lld redundant=%lld owner=%dx%d rate=%.2f/s\n",
        reason, elapsed, s.paintCalls,
        s.paintViaQueue, s.paintFromUpdateWindow, s.paintFromRedrawNow, appDirect,
        s.endPaintCalls, s.dirtyMarks, s.dirtyMarksMerged,
        s.presentAttempts, s.skippedBlank, s.skippedTakeover,
        s.skippedTakeoverSelf, s.skippedTakeoverOther,
        s.submits, s.redundantSubmits, gPresentedWidth, gPresentedHeight,
        elapsed > 0.0005 ? static_cast<double>(s.submits) / elapsed : 0.0);
    std::fflush(stderr);
}

void presentStatsAtExit() { presentStatsPrint("atexit"); }

// 到点即退出：世界视图的主循环是 `while (PeekMessageW(...))` + 自己 sleep，
// 既没有 atexit 时机也没有稳定退出路径，_Exit 是唯一确定性的收尾方式
// （与 ON-3-01 的 attest 同一取舍：先 fflush，不跑静态析构）。
void presentStatsMaybeExit() {
    if (!presentStatsEnabled()) return;
    static const bool registered = [] {
        std::atexit(&presentStatsAtExit);
        return true;
    }();
    (void)registered;
    const int until = presentStatsUntilSec();
    if (until <= 0) return;
    if (presentStatsElapsedSeconds() < static_cast<double>(until)) return;
    presentStatsPrint("until-sec");
    std::_Exit(0);
}

// 1/32 像素采样签名。不要拿它当"画面有没有变"的判据（见上面的局限说明），
// 它只服务于 redundant 这个端口。
std::uint64_t sampleFrameSignature(const unsigned char* bits, int width, int height, int stride) {
    std::uint64_t hash = 1469598103934665603ull;
    hash = (hash ^ static_cast<std::uint64_t>(width)) * 1099511628211ull;
    hash = (hash ^ static_cast<std::uint64_t>(height)) * 1099511628211ull;
    const int rowStep = height >= 8 ? 4 : 1;
    const int colStep = width >= 16 ? 8 : 1;
    for (int y = 0; y < height; y += rowStep) {
        const unsigned char* row = bits + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        for (int x = 0; x < width; x += colStep) {
            const unsigned char* px = row + static_cast<std::size_t>(x) * 4u;
            hash = (hash ^ px[0]) * 1099511628211ull;
            hash = (hash ^ px[1]) * 1099511628211ull;
            hash = (hash ^ px[2]) * 1099511628211ull;
        }
    }
    return hash;
}

struct PresentStatsLastSubmit {
    const void* bits = nullptr;
    int width = 0;
    int height = 0;
    bool visible = false;
    std::uint64_t signature = 0;
    bool valid = false;
};

PresentStatsLastSubmit& presentStatsLast() { static PresentStatsLastSubmit last; return last; }

// 本线程 id（SetWindowsHookExW 判定"本线程"用）。懒初始化。
DWORD currentThreadId() {
    static thread_local DWORD id = [] {
        return static_cast<DWORD>(::GetCurrentThreadId());
    }();
    return id;
}

// ---------------------------------------------------------------------------
// 句柄 <-> 对象转换
// ---------------------------------------------------------------------------
//
// asWindow 先走句柄表（isRegisteredHandle 只比较指针值、不碰内存），确认这个
// 指针值确实是本层登记过的窗口句柄，再转回 CompatWindow* 做 magic 校验。两步
// 缺一不可：只查句柄表不知道指针类型对不对；只验 magic 会在野指针上崩。

inline CompatWindow* asWindow(HWND h) {
    if (h == nullptr) return nullptr;
    if (!concord_compat::isRegisteredHandle(reinterpret_cast<void*>(h), "window")) {
        return nullptr;
    }
    auto* w = reinterpret_cast<CompatWindow*>(h);
    return (w->magic == kWndMagic) ? w : nullptr;
}

inline HWND toHwnd(CompatWindow* w) { return reinterpret_cast<HWND>(w); }

// 从窗口表里摘除（不 delete）。DestroyWindow 用它把窗口从全局表移除。
void removeFromTable(CompatWindow* w) {
    auto& table = windowTable();
    for (auto it = table.begin(); it != table.end(); ++it) {
        if (*it == w) {
            table.erase(it);
            return;
        }
    }
}

// 句柄表登记的析构函数：清理窗口 DC + 位图 + 释放对象。
void destroyWindowPayload(void* handle) {
    auto* w = reinterpret_cast<CompatWindow*>(handle);
    if (w->dc != nullptr) ::DeleteDC(w->dc);       // 摘掉客户区位图
    if (w->surface != nullptr) ::DeleteObject(reinterpret_cast<HGDIOBJ>(w->surface));
    w->magic = 0;
    delete w;
}

// 创建（或复用）窗口客户区位图 + DC。尺寸变化时重建。
//
// U-14：**尺寸变化必须重建**。原来这里一旦建好就直接 return true——窗口被
// 全屏改成 1800x1130 之后，客户区位图还是 1024x640：游戏画的是 1024x640 的
// 内容，呈现层却按更大区域贴，右下角留下上一帧的残影；而且游戏读
// GetClientRect 拿到的是新尺寸，两者彻底脱节。
bool ensureSurface(CompatWindow* w, int width, int height) {
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;
    if (w->dc != nullptr && w->surface != nullptr) {
        // 尺寸一致才复用。比对的是建位图时记下的尺寸，而不是去问 GetObjectW：
        // ensureSurface 在 GetObjectW 定义之前（匿名命名空间里），少一个前向依赖。
        if (w->surfaceWidth == width && w->surfaceHeight == height) return true;
        // 先删 DC 再删位图（与 destroyWindowPayload 同一顺序：DeleteDC 会把
        // 选中的位图摘下来）。
        ::DeleteDC(w->dc);
        w->dc = nullptr;
        ::DeleteObject(reinterpret_cast<HGDIOBJ>(w->surface));
        w->surface = nullptr;
        w->surfaceWidth = 0;
        w->surfaceHeight = 0;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // 负值 = top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP surface = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (surface == nullptr) return false;

    HDC dc = ::CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        ::DeleteObject(reinterpret_cast<HGDIOBJ>(surface));
        return false;
    }
    ::SelectObject(dc, reinterpret_cast<HGDIOBJ>(surface));
    w->dc = dc;
    w->surface = surface;
    w->surfaceWidth = width;
    w->surfaceHeight = height;
    return true;
}

// 客户区尺寸（从 windowRect 推得；纯离屏下窗口矩形 == 客户区矩形）。
inline int clientWidth(const CompatWindow* w) {
    return w->windowRect.right - w->windowRect.left;
}
inline int clientHeight(const CompatWindow* w) {
    return w->windowRect.bottom - w->windowRect.top;
}

// 触发 HCBT_CREATEWND / HCBT_DESTROYWND 钩子链。返回非 0 = 有钩子阻止。
LRESULT fireCbtHook(int code, HWND window) {
    std::vector<HookEntry> hooks;
    {
        std::lock_guard<std::mutex> lock(userMutex());
        hooks = hookTable();
    }
    for (const HookEntry& e : hooks) {
        // 只回调本线程钩子；跨线程钩子在本层诚实跳过（无法真正跨线程注入）。
        if (e.threadId != 0 && e.threadId != currentThreadId()) continue;
        LRESULT r = e.proc(code, reinterpret_cast<WPARAM>(window), 0);
        if (r != 0) return r;
    }
    return 0;
}

// 同步调用一个窗口的窗口过程（不持有锁，避免窗口过程重入时死锁）。
LRESULT callWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    CompatWindow* w = asWindow(window);
    if (w == nullptr || w->wndProc == nullptr) return 0;
    // ON-2-02a：分发口径的总数。放在句柄校验之后——发给已销毁窗口的 WM_PAINT
    // 一次都不会真的执行窗口过程，计入就会让"四类之和 == 总数"这条恒等式失真。
    if (message == WM_PAINT && presentStatsEnabled()) ++presentStats().paintCalls;
    WNDPROC proc = w->wndProc;
    return proc(window, message, wParam, lParam);
}

// 匹配消息过滤（GetMessageW/PeekMessageW 的 hWnd + 范围过滤）。
bool messageMatches(const MSG& msg, HWND hWnd, UINT filterMin, UINT filterMax) {
    if (hWnd != nullptr && msg.hwnd != hWnd) return false;
    if (filterMin != 0 || filterMax != 0) {
        if (msg.message < filterMin || msg.message > filterMax) return false;
    }
    return true;
}

// 合成消息（WM_TIMER / WM_PAINT）走同一个范围规则。**min==max==0 表示不过滤**，
// 这是 Win32 的约定，产品也正是用 GetMessageW(&msg, nullptr, 0, 0) 取消息；
// 若这里写成 `min <= msg && msg <= max`，0..0 会把一切合成消息挡在门外——
// 表现为"定时器已注册但 GetMessageW 永久阻塞"，而任何单看返回值的断言都不会红。
bool rangeAccepts(UINT message, UINT filterMin, UINT filterMax) {
    if (filterMin == 0 && filterMax == 0) return true;
    return message >= filterMin && message <= filterMax;
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口（extern "C"：与 windows.h 声明、产品源码的 C 链接一致）
// ---------------------------------------------------------------------------

extern "C" {

// ===========================================================================
// 窗口类
// ===========================================================================

ATOM RegisterClassExW(const WNDCLASSEXW* lpWndClass) {
    if (lpWndClass == nullptr || lpWndClass->lpszClassName == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const std::wstring name(lpWndClass->lpszClassName);
    std::lock_guard<std::mutex> lock(userMutex());
    auto& table = classTable();
    if (table.find(name) != table.end()) {
        // 已存在：登记一个"重复类"错误，让调用点走
        // `GetLastError() == ERROR_CLASS_ALREADY_EXISTS` 的成功分支。
        concord_compat::setLastError(ERROR_CLASS_ALREADY_EXISTS);
        return 0;
    }
    table.emplace(name, *lpWndClass);
    return 1;  // ATOM 非零即成功
}

ATOM RegisterClassW(const WNDCLASSW* lpWndClass) {
    if (lpWndClass == nullptr || lpWndClass->lpszClassName == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    WNDCLASSEXW ex{};
    ex.cbSize = sizeof(ex);
    ex.style = lpWndClass->style;
    ex.lpfnWndProc = lpWndClass->lpfnWndProc;
    ex.hInstance = lpWndClass->hInstance;
    ex.hIcon = lpWndClass->hIcon;
    ex.hCursor = lpWndClass->hCursor;
    ex.hbrBackground = lpWndClass->hbrBackground;
    ex.lpszMenuName = lpWndClass->lpszMenuName;
    ex.lpszClassName = lpWndClass->lpszClassName;
    return RegisterClassExW(&ex);
}

BOOL UnregisterClassW(LPCWSTR lpClassName, HINSTANCE hInstance) {
    (void)hInstance;
    if (lpClassName == nullptr) return FALSE;
    std::lock_guard<std::mutex> lock(userMutex());
    return classTable().erase(lpClassName) > 0 ? TRUE : FALSE;
}

BOOL GetClassInfoW(HINSTANCE hInstance, LPCWSTR lpClassName, WNDCLASSW* lpWndClass) {
    (void)hInstance;
    if (lpClassName == nullptr || lpWndClass == nullptr) return FALSE;
    std::lock_guard<std::mutex> lock(userMutex());
    auto found = classTable().find(lpClassName);
    if (found == classTable().end()) return FALSE;
    const WNDCLASSEXW& ex = found->second;
    lpWndClass->style = ex.style;
    lpWndClass->lpfnWndProc = ex.lpfnWndProc;
    lpWndClass->hInstance = ex.hInstance;
    lpWndClass->hIcon = ex.hIcon;
    lpWndClass->hCursor = ex.hCursor;
    lpWndClass->hbrBackground = ex.hbrBackground;
    lpWndClass->lpszMenuName = ex.lpszMenuName;
    lpWndClass->lpszClassName = ex.lpszClassName;
    return TRUE;
}

BOOL GetClassInfoExW(HINSTANCE hInstance, LPCWSTR lpszClass, WNDCLASSEXW* lpwcx) {
    (void)hInstance;
    if (lpszClass == nullptr || lpwcx == nullptr) return FALSE;
    std::lock_guard<std::mutex> lock(userMutex());
    auto found = classTable().find(lpszClass);
    if (found == classTable().end()) return FALSE;
    *lpwcx = found->second;
    return TRUE;
}

// ===========================================================================
// 窗口创建 / 销毁
// ===========================================================================

// Windows 的预定义系统窗口类。产品会直接 `CreateWindowExW(0, L"STATIC", ...)`
// 而**不先** RegisterClassExW —— 在真 Windows 上这 8 个类天然存在。
//
// 不对位的后果很隐蔽：CreateWindowExW 返回 nullptr，产品那边只看到
// "hosted child page did not preserve its top-level shell"，完全看不出
// 是"类没注册"。这个失败正是 `--main-menu-ui-smoke-test` 卡住的原因。
// 内建窗口类的**类光标**。真 Windows 上这些值来自系统 WNDCLASS：
//   EDIT → IDC_IBEAM(32513)，其余 → IDC_ARROW(32512)。
// 少了它会怎样：DefWindowProc(WM_SETCURSOR) 装不上光标，GetCursor() 保持 nullptr，
// 而产品侧 `SetCursor(nullptr); SendMessage(WM_SETCURSOR...); GetCursor() != nullptr`
// 这种"编辑框该恢复成 I 形光标"的断言就会红——遭遇战菜单的 cursor smoke 正是这么红的。
HCURSOR stockWindowCursor(const std::wstring& name) {
    const bool ibeam = (name == L"EDIT");
    return reinterpret_cast<HCURSOR>(static_cast<std::uintptr_t>(ibeam ? 32513u : 32512u));
}

bool isStockWindowClass(const std::wstring& name) {
    return name == L"STATIC" || name == L"BUTTON" || name == L"EDIT"
        || name == L"LISTBOX" || name == L"COMBOBOX" || name == L"SCROLLBAR"
        || name == L"MDICLIENT";
}

HWND CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName,
                     DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
                     HWND hWndParent, HMENU hMenu, HINSTANCE hInstance,
                     LPVOID lpParam) {
    if (lpClassName == nullptr) return nullptr;

    WNDCLASSEXW cls{};
    {
        std::lock_guard<std::mutex> lock(userMutex());
        auto found = classTable().find(lpClassName);
        if (found != classTable().end()) {
            cls = found->second;
        } else if (isStockWindowClass(lpClassName)) {
            // 预定义类由系统提供，行为就是默认窗口过程。这里只在**局部**填一个
            // cls 供本次创建使用，不写进 classTable —— 否则产品之后
            // RegisterClassExW(L"STATIC") 会撞上"类已存在"。
            cls.lpfnWndProc = DefWindowProcW;
            cls.lpszClassName = lpClassName;
        } else {
            return nullptr;
        }
    }

    CompatWindow* w = new (std::nothrow) CompatWindow();
    if (w == nullptr) return nullptr;
    w->className = lpClassName;
    w->title = (lpWindowName != nullptr) ? lpWindowName : L"";
    w->style = dwStyle;
    w->exStyle = dwExStyle;
    w->parent = hWndParent;
    w->menu = hMenu;
    w->instance = hInstance;
    w->wndProc = cls.lpfnWndProc;
    // 类光标：内建类用系统默认（EDIT=I-beam，其余箭头）；注册类用 WNDCLASSEXW
    // 里给的值。DefWindowProc(WM_SETCURSOR) 靠它回答"这个窗口该显示什么光标"。
    w->cursor = isStockWindowClass(lpClassName) ? stockWindowCursor(lpClassName) : cls.hCursor;
    w->windowRect = {X, Y, X + nWidth, Y + nHeight};

    if (!ensureSurface(w, nWidth, nHeight)) {
        destroyWindowPayload(w);
        return nullptr;
    }

    HWND hwnd = toHwnd(w);
    if (!concord_compat::registerHandle(reinterpret_cast<void*>(hwnd), destroyWindowPayload, "window")) {
        destroyWindowPayload(w);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(userMutex());
        windowTable().push_back(w);
        if (hWndParent != nullptr) {
            if (CompatWindow* p = asWindow(hWndParent)) p->children.push_back(hwnd);
        }
    }

    // HCBT_CREATEWND：钩子可在此观察类名，或返回非 0 阻止创建。
    if (fireCbtHook(HCBT_CREATEWND, hwnd) != 0) {
        // 撤销创建：从表移除 + 关句柄（触发 delete）。
        {
            std::lock_guard<std::mutex> lock(userMutex());
            removeFromTable(w);
        }
        concord_compat::closeRegisteredHandle(reinterpret_cast<void*>(hwnd));
        return nullptr;
    }

    // WM_NCCREATE：把 lpCreateParams 交给窗口过程（它写入 GWLP_USERDATA）。
    CREATESTRUCTW create{};
    create.lpCreateParams = lpParam;
    create.hInstance = hInstance;
    create.hMenu = hMenu;
    create.hwndParent = hWndParent;
    create.cx = nWidth;
    create.cy = nHeight;
    create.x = X;
    create.y = Y;
    create.style = dwStyle;
    callWindowProc(hwnd, WM_NCCREATE, 0, reinterpret_cast<LPARAM>(&create));
    callWindowProc(hwnd, WM_CREATE, 0, reinterpret_cast<LPARAM>(&create));
    return hwnd;
}

BOOL DestroyWindow(HWND hWnd) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return FALSE;
    if (w->destroying) return FALSE;
    w->destroying = true;

    // S11：窗口的定时器随窗口一起消失。真实 user32 也这样——否则表里会留着一个
    // 指向已销毁句柄的定时器，每 16ms 合成一条 WM_TIMER 往死窗口上派发，而
    // 调用方完全看不出问题（消息"投递成功"、DispatchMessage 静默丢弃）。
    concord_compat::timerDropForWindow(hWnd);

    // 先发销毁消息，让窗口过程清理（WM_NCDESTROY 里会清 GWLP_USERDATA）。
    callWindowProc(hWnd, WM_DESTROY, 0, 0);
    callWindowProc(hWnd, WM_NCDESTROY, 0, 0);
    fireCbtHook(HCBT_DESTROYWND, hWnd);

    {
        std::lock_guard<std::mutex> lock(userMutex());
        removeFromTable(w);
        if (w->parent != nullptr) {
            if (CompatWindow* p = asWindow(w->parent)) {
                for (auto it = p->children.begin(); it != p->children.end(); ++it) {
                    if (*it == hWnd) { p->children.erase(it); break; }
                }
            }
        }
        if (focusWindow() == hWnd) focusWindow() = nullptr;
        if (foregroundWindow() == hWnd) foregroundWindow() = nullptr;
    }

    concord_compat::closeRegisteredHandle(reinterpret_cast<void*>(hWnd));
    return TRUE;
}

BOOL IsWindow(HWND hWnd) {
    return asWindow(hWnd) != nullptr ? TRUE : FALSE;
}

BOOL IsWindowVisible(HWND hWnd) {
    CompatWindow* w = asWindow(hWnd);
    return (w != nullptr && w->visible) ? TRUE : FALSE;
}

HWND GetParent(HWND hWnd) {
    CompatWindow* w = asWindow(hWnd);
    return (w != nullptr) ? w->parent : nullptr;
}

HWND GetAncestor(HWND hwnd, UINT gaFlags) {
    CompatWindow* w = asWindow(hwnd);
    if (w == nullptr) return nullptr;
    if (gaFlags == GA_PARENT) return w->parent;
    // GA_ROOT：一路向上找到没有父窗口的顶层窗口。
    if (gaFlags == GA_ROOT) {
        HWND cur = hwnd;
        while (CompatWindow* c = asWindow(cur)) {
            if (c->parent == nullptr) return cur;
            cur = c->parent;
        }
        return cur;
    }
    return hwnd;  // 其余 GA_* 诚实原样返回
}

// ===========================================================================
// 窗口几何
// ===========================================================================

BOOL GetWindowRect(HWND hWnd, LPRECT lpRect) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr || lpRect == nullptr) return FALSE;
    *lpRect = w->windowRect;
    return TRUE;
}

BOOL GetClientRect(HWND hWnd, LPRECT lpRect) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr || lpRect == nullptr) return FALSE;
    lpRect->left = 0;
    lpRect->top = 0;
    lpRect->right = clientWidth(w);
    lpRect->bottom = clientHeight(w);
    return TRUE;
}

BOOL AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu) {
    return AdjustWindowRectEx(lpRect, dwStyle, bMenu, 0);
}

BOOL AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle) {
    (void)dwStyle;
    (void)bMenu;
    (void)dwExStyle;
    if (lpRect == nullptr) return FALSE;
    // 纯离屏无边框装饰、无菜单栏，窗口矩形 == 客户区矩形。原样返回即如实。
    return TRUE;
}

BOOL MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint) {
    return SetWindowPos(hWnd, nullptr, X, Y, nWidth, nHeight,
                        SWP_NOZORDER | (bRepaint ? 0 : SWP_NOREDRAW));
}

BOOL SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy,
                  UINT uFlags) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return FALSE;
    (void)hWndInsertAfter;

    RECT r = w->windowRect;
    if ((uFlags & SWP_NOMOVE) == 0) { r.left = X; r.top = Y; }
    if ((uFlags & SWP_NOSIZE) == 0) { r.right = r.left + cx; r.bottom = r.top + cy; }
    w->windowRect = r;

    if ((uFlags & SWP_SHOWWINDOW) != 0) w->visible = true;
    if ((uFlags & SWP_HIDEWINDOW) != 0) w->visible = false;
    return TRUE;
}

HWND WindowFromPoint(POINT Point) {
    std::lock_guard<std::mutex> lock(userMutex());
    // 从最上层（表尾）往回找，命中第一个可见且含点的窗口。
    auto& table = windowTable();
    for (auto it = table.rbegin(); it != table.rend(); ++it) {
        CompatWindow* w = *it;
        if (!w->visible) continue;
        const RECT& r = w->windowRect;
        if (Point.x >= r.left && Point.x < r.right && Point.y >= r.top && Point.y < r.bottom) {
            return toHwnd(w);
        }
    }
    return nullptr;
}

// ===========================================================================
// 窗口状态
// ===========================================================================

// 定义在 EndPaint 上方；ShowWindow 先用后定义，所以在这里前置声明。
void maybePresentClientFrame(CompatWindow* w);
// U-14：全屏视口回调（定义在 InvalidateRect 之后）。present 首次绑定时注册。
void appleResizeRequest(int width, int height, void* context);

BOOL ShowWindow(HWND hWnd, int nCmdShow) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return FALSE;
    switch (nCmdShow) {
    case SW_HIDE: w->visible = false; break;
    case SW_SHOWMAXIMIZED: w->zoomed = true; w->iconic = false; w->visible = true; break;
    case SW_SHOWMINIMIZED: w->iconic = true; w->zoomed = false; w->visible = true; break;
    case SW_RESTORE: w->iconic = false; w->zoomed = false; w->visible = true; break;
    default: w->visible = true; break;  // SW_SHOW / SW_SHOWNORMAL 等
    }
    // Q-05：ShowWindow 必须**立刻**让 NSWindow 出现。只翻 bool 的话，窗口要等
    // 下一帧 EndPaint 才贴上来；而菜单初始化后可能长时间不重绘（S11 的定时器
    // 探针是 1s 一次），用户会先看到"程序没起来"。这里先贴一帧空白也无妨——
    // 紧接着的首次 WM_PAINT 会把它覆盖成真实画面。
    maybePresentClientFrame(w);
    return TRUE;
}

BOOL IsIconic(HWND hWnd) {
    CompatWindow* w = asWindow(hWnd);
    return (w != nullptr && w->iconic) ? TRUE : FALSE;
}

BOOL IsZoomed(HWND hWnd) {
    CompatWindow* w = asWindow(hWnd);
    return (w != nullptr && w->zoomed) ? TRUE : FALSE;
}

BOOL EnableWindow(HWND hWnd, BOOL bEnable) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return FALSE;
    const BOOL previous = w->enabled ? TRUE : FALSE;
    w->enabled = (bEnable != FALSE);
    return previous;
}

BOOL IsWindowEnabled(HWND hWnd) {
    CompatWindow* w = asWindow(hWnd);
    return (w != nullptr && w->enabled) ? TRUE : FALSE;
}

BOOL BringWindowToTop(HWND hWnd) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return FALSE;
    // 纯离屏的"置顶" = 移到窗口表尾（枚举/命中时视为最上层）。
    std::lock_guard<std::mutex> lock(userMutex());
    removeFromTable(w);
    windowTable().push_back(w);
    return TRUE;
}

BOOL UpdateWindow(HWND hWnd) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return FALSE;
    // 纯离屏无真正的同步重绘：直接把脏区画成 WM_PAINT 派发。
    if (w->dirty) {
        if (presentStatsEnabled()) ++presentStats().paintFromUpdateWindow;
        callWindowProc(hWnd, WM_PAINT, 0, 0);
    }
    return TRUE;
}

// ===========================================================================
// 窗口属性 / 长指针
// ===========================================================================

LONG_PTR GetWindowLongPtrW(HWND hWnd, int nIndex) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return 0;
    switch (nIndex) {
    case GWLP_USERDATA: return w->userData;
    case GWLP_WNDPROC: return reinterpret_cast<LONG_PTR>(w->wndProc);
    case GWLP_HINSTANCE: return reinterpret_cast<LONG_PTR>(w->instance);
    case GWL_STYLE: return static_cast<LONG_PTR>(w->style);
    case GWL_EXSTYLE: return static_cast<LONG_PTR>(w->exStyle);
    default: return 0;
    }
}

LONG_PTR SetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return 0;
    LONG_PTR previous = 0;
    switch (nIndex) {
    case GWLP_USERDATA:
        previous = w->userData;
        w->userData = dwNewLong;
        break;
    case GWLP_WNDPROC:
        previous = reinterpret_cast<LONG_PTR>(w->wndProc);
        w->wndProc = reinterpret_cast<WNDPROC>(dwNewLong);
        break;
    case GWLP_HINSTANCE:
        previous = reinterpret_cast<LONG_PTR>(w->instance);
        w->instance = reinterpret_cast<HINSTANCE>(dwNewLong);
        break;
    case GWL_STYLE:
        previous = static_cast<LONG_PTR>(w->style);
        w->style = static_cast<DWORD>(dwNewLong);
        break;
    case GWL_EXSTYLE:
        previous = static_cast<LONG_PTR>(w->exStyle);
        w->exStyle = static_cast<DWORD>(dwNewLong);
        break;
    default: return 0;
    }
    return previous;
}

BOOL SetPropW(HWND hWnd, LPCWSTR lpString, HANDLE hData) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr || lpString == nullptr) return FALSE;
    std::lock_guard<std::mutex> lock(userMutex());
    w->props[lpString] = hData;
    return TRUE;
}

HANDLE GetPropW(HWND hWnd, LPCWSTR lpString) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr || lpString == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(userMutex());
    auto found = w->props.find(lpString);
    return (found != w->props.end()) ? found->second : nullptr;
}

HANDLE RemovePropW(HWND hWnd, LPCWSTR lpString) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr || lpString == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(userMutex());
    auto found = w->props.find(lpString);
    if (found == w->props.end()) return nullptr;
    HANDLE value = found->second;
    w->props.erase(found);
    return value;
}

BOOL SetWindowTextW(HWND hWnd, LPCWSTR lpString) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return FALSE;
    w->title = (lpString != nullptr) ? lpString : L"";
    return TRUE;
}

int GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return 0;
    if (lpString == nullptr || nMaxCount <= 0) return static_cast<int>(w->title.size());
    const int copy = (nMaxCount - 1 < static_cast<int>(w->title.size()))
                         ? nMaxCount - 1
                         : static_cast<int>(w->title.size());
    std::memcpy(lpString, w->title.data(), static_cast<std::size_t>(copy) * sizeof(wchar_t));
    lpString[copy] = L'\0';
    return copy;
}

int GetClassNameW(HWND hWnd, LPWSTR lpClassName, int nMaxCount) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return 0;
    if (lpClassName == nullptr || nMaxCount <= 0) return static_cast<int>(w->className.size());
    const int copy = (nMaxCount - 1 < static_cast<int>(w->className.size()))
                         ? nMaxCount - 1
                         : static_cast<int>(w->className.size());
    std::memcpy(lpClassName, w->className.data(), static_cast<std::size_t>(copy) * sizeof(wchar_t));
    lpClassName[copy] = L'\0';
    return copy;
}

// ===========================================================================
// DC 关联（绘制）
// ===========================================================================

HDC GetDC(HWND hWnd) {
    // GetDC(NULL) = 屏幕 DC。这不是"取个空窗口的 DC"，而是整个屏幕参照设备，
    // 产品拿它给 CreateCompatibleDC/CreateDIBSection 当目标设备描述，并用
    // 非空当成功判据。之前这里直接走 asWindow(nullptr) 返回 nullptr，导致
    // 本仓库 71 处 `GetDC(nullptr)` 调用点**全部**走失败分支——菜单 smoke 的
    // "offscreen surface creation failed" 就是这一句。
    if (hWnd == nullptr) return concord_compat::screenReferenceDc(kVirtualScreenW, kVirtualScreenH);
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return nullptr;
    if (!ensureSurface(w, clientWidth(w), clientHeight(w))) return nullptr;
    return w->dc;
}

int ReleaseDC(HWND hWnd, HDC hDC) {
    (void)hWnd;
    (void)hDC;
    // 纯离屏缓存 DC：不真正释放，返回 1（成功）。DC 随窗口销毁而释放。
    // 屏幕 DC（hWnd==nullptr）同样是单例，ReleaseDC 本来就该是配对语义而**不是**
    // 销毁语义——所以这里无需区分：不释放正是正确行为。
    return 1;
}

HDC BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return nullptr;
    if (!ensureSurface(w, clientWidth(w), clientHeight(w))) return nullptr;
    if (lpPaint != nullptr) {
        lpPaint->hdc = w->dc;
        lpPaint->fErase = FALSE;
        lpPaint->rcPaint.left = 0;
        lpPaint->rcPaint.top = 0;
        lpPaint->rcPaint.right = clientWidth(w);
        lpPaint->rcPaint.bottom = clientHeight(w);
    }
    w->dirty = false;
    return w->dc;
}

// ---------------------------------------------------------------------------
// Q-05（诊断）：把窗口客户区位图导出成 BMP。
//
// 为什么需要它：macOS 侧是"纯离屏"后端——`ShowWindow` 只翻一个 bool，进程里没有
// 任何 NSWindow，所以产品跑起来**只有声音、屏幕上什么都没有**（本文件顶部那条
// 设计立场就是原因）。在真正叠上 Cocoa 呈现层之前，这个钩子是唯一能看见"到底
// 画了什么"的手段：它读的就是 CompatWindow::surface——将来若有 NSView 贴图，
// 贴的正是这块像素。
//
// 关掉时零开销：getenv 只查一次并缓存；未设置直接返回。
// 用法：CONCORD_MENU_DUMP=/tmp/frame.bmp [CONCORD_MENU_DUMP_FRAME=4]
// ---------------------------------------------------------------------------
void maybeDumpClientFrame(CompatWindow* w) {
    static const char* const dumpPath = []() -> const char* {
        const char* value = std::getenv("CONCORD_MENU_DUMP");
        return (value != nullptr && value[0] != '\0') ? value : nullptr;
    }();
    static const int targetFrame = []() {
        const char* value = std::getenv("CONCORD_MENU_DUMP_FRAME");
        return value != nullptr ? std::atoi(value) : 4;
    }();
    static int paintCount = 0;
    static bool alreadyDumped = false;

    if (dumpPath == nullptr || alreadyDumped || w == nullptr || w->surface == nullptr) {
        return;
    }
    if (++paintCount < targetFrame) {
        return;
    }
    alreadyDumped = true;

    DIBSECTION section{};
    if (GetObjectW(w->surface, static_cast<int>(sizeof(section)), &section) == 0) {
        return;
    }
    const int width = section.dsBm.bmWidth;
    const int height = section.dsBm.bmHeight;
    const int bitsPerPixel = section.dsBm.bmBitsPixel;
    const auto* base = static_cast<const unsigned char*>(section.dsBm.bmBits);
    if (base == nullptr || width <= 0 || height <= 0 || bitsPerPixel != 32) {
        return;
    }

    const int sourceStride = width * 4;
    const int rowBytes = width * 3;
    const int paddedRowBytes = (rowBytes + 3) & ~3;
    const std::uint32_t pixelBytes =
        static_cast<std::uint32_t>(paddedRowBytes) * static_cast<std::uint32_t>(height);
    const std::uint32_t headerBytes = 54;
    const std::uint32_t fileBytes = headerBytes + pixelBytes;

    std::vector<unsigned char> blob(fileBytes, 0);
    const auto putLe32 = [&blob](std::size_t offset, std::uint32_t value) {
        blob[offset + 0] = static_cast<unsigned char>(value & 0xFFu);
        blob[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xFFu);
        blob[offset + 2] = static_cast<unsigned char>((value >> 16) & 0xFFu);
        blob[offset + 3] = static_cast<unsigned char>((value >> 24) & 0xFFu);
    };
    const auto putLe16 = [&blob](std::size_t offset, std::uint16_t value) {
        blob[offset + 0] = static_cast<unsigned char>(value & 0xFFu);
        blob[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xFFu);
    };

    blob[0] = 'B';
    blob[1] = 'M';
    putLe32(2, fileBytes);
    putLe32(10, headerBytes);
    putLe32(14, 40);  // BITMAPINFOHEADER
    putLe32(18, static_cast<std::uint32_t>(width));
    putLe32(22, static_cast<std::uint32_t>(height));  // 正数 = 自底向上
    putLe16(26, 1);                                   // planes
    putLe16(28, 24);                                  // bpp
    putLe32(34, pixelBytes);

    for (int row = 0; row < height; ++row) {
        // 源是 32bpp 自顶向下（DIB 像素 0 = 顶行）；BMP 24bpp 是自底向上。
        const unsigned char* src =
            base + static_cast<std::size_t>(height - 1 - row) * sourceStride;
        unsigned char* dst = blob.data() + headerBytes +
            static_cast<std::size_t>(row) * paddedRowBytes;
        for (int x = 0; x < width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];  // B
            dst[x * 3 + 1] = src[x * 4 + 1];  // G
            dst[x * 3 + 2] = src[x * 4 + 2];  // R
        }
    }

    if (std::FILE* out = std::fopen(dumpPath, "wb")) {
        std::fwrite(blob.data(), 1, blob.size(), out);
        std::fclose(out);
        std::fprintf(stderr,
            "[q05] client frame dumped %dx%d -> %s (%u bytes)\n",
            width, height, dumpPath, fileBytes);
    }
}

// ---------------------------------------------------------------------------
// Q-06：AppKit 输入 → Win32 消息
// ---------------------------------------------------------------------------
// 只有显示层接管是不够的：窗口能看见，但点击进不了 compat 的消息队列，
// 菜单就是"能看不能点"。这个回调把 .mm 翻译好的事件翻成 WM_* 投回队列，
// 之后产品原有的窗口过程（hover、点按钮、键盘快捷键）全部照常工作。
// 目标窗口通过 sink 的 context 传进来，不用全局变量：
// 这样投递逻辑没有隐藏状态，也才能被自测直接驱动（见 tests 里的用法）。
void appleInputDispatch(const concord_compat::apple::InputEvent& event, void* context) {
    const HWND gInputTarget = reinterpret_cast<HWND>(context);
    if (gInputTarget == nullptr) return;
    using Kind = concord_compat::apple::InputEvent::Kind;
    const LPARAM point = MAKELPARAM(static_cast<WORD>(event.x), static_cast<WORD>(event.y));
    // U-07：鼠标消息的 wParam 必须带上修饰键位。产品里 Shift+点击加选、Ctrl+点击
    // 切换选择，判据就是 `wParam & MK_SHIFT/MK_CONTROL`（RuntimeMapLeftPointerSmoke
    // 的 1759 行把四种组合都测了）。此前这里只写 MK_按键、不带修饰位，
    // 于是"按住 Shift 点单位"和普通点击是同一条路径。
    const WPARAM modifierBits =
        ((GetKeyState(VK_SHIFT) & 0x8000) != 0 ? MK_SHIFT : 0)
        | ((GetKeyState(VK_CONTROL) & 0x8000) != 0 ? MK_CONTROL : 0);
    switch (event.kind) {
    case Kind::MouseMove:  PostMessageW(gInputTarget, WM_MOUSEMOVE, modifierBits, point); break;
    case Kind::LeftDown:   PostMessageW(gInputTarget, WM_LBUTTONDOWN, MK_LBUTTON | modifierBits, point); break;
    case Kind::LeftUp:     PostMessageW(gInputTarget, WM_LBUTTONUP, modifierBits, point); break;
    case Kind::RightDown:  PostMessageW(gInputTarget, WM_RBUTTONDOWN, MK_RBUTTON | modifierBits, point); break;
    case Kind::RightUp:    PostMessageW(gInputTarget, WM_RBUTTONUP, modifierBits, point); break;
    case Kind::MiddleDown: PostMessageW(gInputTarget, WM_MBUTTONDOWN, MK_MBUTTON | modifierBits, point); break;
    case Kind::MiddleUp:   PostMessageW(gInputTarget, WM_MBUTTONUP, modifierBits, point); break;
    case Kind::Wheel:
        // WM_MOUSEWHEEL 的 delta 在高位字，且坐标是**屏幕**坐标——
        // 但本层没有屏幕坐标系（纯离屏），产品只用 delta，所以低位置 0。
        PostMessageW(gInputTarget, WM_MOUSEWHEEL,
                     static_cast<WPARAM>(static_cast<unsigned>(event.wheelDelta) & 0xFFFFu) << 16,
                     point);
        break;
    case Kind::KeyDown:
        // U-07：状态必须先于消息更新——窗口过程在 DispatchMessageW 里读
        // GetKeyState 判断修饰键，晚一步就会读到上一帧的状态。
        concord_compat::setVirtualKeyDown(event.keyCode, true);
        PostMessageW(gInputTarget, WM_KEYDOWN, static_cast<WPARAM>(event.keyCode), 0);
        break;
    case Kind::KeyUp:
        concord_compat::setVirtualKeyDown(event.keyCode, false);
        PostMessageW(gInputTarget, WM_KEYUP, static_cast<WPARAM>(event.keyCode), 0);
        break;
    case Kind::Close:
        // ON-2-04：红叉 = 关掉呈现窗口 + 退出应用（macOS 单窗口应用的常规语义）。
        //
        // 用 SendMessageW 而不是 PostMessageW：要在这里就能判定"窗口到底关没关"，
        // 下面那句 IsWindow 才有意义。Post 的话只能看到消息入队，关没关要等下一轮。
        //
        // 之后的链路全是既有的：产品侧没有 WM_CLOSE 分支（实测 RuntimeMainMenu.cpp
        // 全文无 WM_CLOSE/WM_DESTROY 处理）→ DefWindowProc 调 DestroyWindow →
        // WM_DESTROY 按 Q-11 的结论**不投** WM_QUIT。
        //
        // 所以这里补一句 PostQuitMessage——但只在**窗口真的没了**的时候。
        // 这不违反 Q-11：那条否定的是"DefWindowProc 里任何窗口销毁都代劳 quit"
        // （子窗口、smoke 自建的 STATIC 壳会被误杀）；这里是"用户明确关掉了呈现窗口"
        // 这个具体事件的处理者，而且先验证了销毁已经发生。
        // 若将来产品在 WM_CLOSE 里弹确认框、返回 0 阻止销毁，IsWindow 仍为真，
        // 这里就不会退出——语义正确，不需要改。
        SendMessageW(gInputTarget, WM_CLOSE, 0, 0);
        if (!IsWindow(gInputTarget)) PostQuitMessage(0);
        break;
    }
}

// Q-05：把一帧贴到真正的 NSWindow 上。与 maybeDumpClientFrame 走同一条取像素
// 路径（GetObjectW + DIBSECTION），区别是这一个走**主路径**（无条件执行），
// 那个是 getenv 开关下的取证工具。零配置是刻意的：屏幕上有没有画面不该取决于
// 用户是否设了环境变量。
// 一个进程只呈现**一个**窗口：第一个画完的窗口成为"可见窗口"，之后
// 只有**严格更大**的窗口才接管。
//
// 为什么必须有这条规则：EndPaint 是逐窗口的，而本层的呈现目标是"屏幕上唯一的
// 那个 NSWindow"。不加限制的话，任何小控件一重绘就会把整屏换掉——实测就发生过：
// 随机地图名字输入框（EDIT，10×10）重绘时日志变成
//     [cocoa] NSWindow created 10x10 title= visible=1
// 也就是整个游戏画面被一个 10×10 的输入框顶替。这种缺陷不崩不报，只表现为
// "画面没了"，最难查。
//
// 为什么允许"更大者接管"：宿主子页面（hosted menu child，1024×640 或 800×600）
// 会先画；后来的独立主窗口更大时应由它显示。反过来"更小者接管"没有正当场景，
// 一律拒绝。
HWND gPresentedWindow = nullptr;
// 上次成为可见窗口时的**面积**。为什么要单独记：窗口可以被销毁，而句柄一失效
// 就查不到尺寸了；只比较"当前 owner 的尺寸"会让 owner 一死就无门槛——
// 实测发生过：800x600 的遭遇战壳销毁后，10x10 的随机地图名字输入框重绘，
// 直接把窗口 resize 成 10x10（`[cocoa] NSWindow resized to 10x10`），整屏变成
// 一个小输入框。记住历史面积就能挡住这种"小控件捡漏"。
long long gPresentedArea = 0;

// ON-2-05：判断这一帧是不是"还没画内容的空白首绘"。
//
// 为什么必须挡：主窗口（1024x640）先于主菜单子窗口 EndPaint，那一次 surface
// 里什么都没有（全 0）。它一抢到 gPresentedWindow，主菜单就再也上不来了——
// 两边面积相等，而抢位规则要求"严格更大"。屏幕于是永久停在一张全黑帧上，
// 而日志一切正常（`first frame attached`、`contents=set`、尺寸正确）。
//
// 采样 8x8 网格而不是全扫：1.9 MB 的 surface 逐像素检查每帧要 2 ms 以上，
// 而空白帧的特征是"整幅全 0"，网格采样足够判定，且漏判只会退化成当前行为。
bool surfaceLooksBlank(const unsigned char* bits, int width, int height, int stride) {
    for (int gy = 0; gy < 8; ++gy) {
        const int y = height > 1 ? (height - 1) * gy / 7 : 0;
        const unsigned char* row = bits + static_cast<std::size_t>(y) * stride;
        for (int gx = 0; gx < 8; ++gx) {
            const int x = width > 1 ? (width - 1) * gx / 7 : 0;
            const unsigned char* px = row + static_cast<std::size_t>(x) * 4;
            if (px[0] != 0 || px[1] != 0 || px[2] != 0) return false;
        }
    }
    return true;
}

void maybePresentClientFrame(CompatWindow* w) {
    if (w == nullptr || w->surface == nullptr) return;
    if (presentStatsEnabled()) ++presentStats().presentAttempts;
    DIBSECTION section{};
    if (::GetObjectW(w->surface, static_cast<int>(sizeof(section)), &section) == 0) return;
    const auto* bits = static_cast<const unsigned char*>(section.dsBm.bmBits);
    if (bits == nullptr || section.dsBm.bmWidth <= 0 || section.dsBm.bmHeight <= 0
        || section.dsBm.bmBitsPixel != 32) {
        return;
    }
    // 空白帧既不呈现、也不参与抢位——它没有资格成为"屏幕上的那一幅画"。
    // 放在抢位判定之前是关键：先抢位再过滤，等于黑帧先把位置占了。
    if (surfaceLooksBlank(bits, section.dsBm.bmWidth, section.dsBm.bmHeight,
                          section.dsBm.bmWidth * 4)) {
        if (presentStatsEnabled()) ++presentStats().skippedBlank;
        return;
    }
    if (presentStatsEnabled()) {
        presentStatsNoteWindow(toHwnd(w), section.dsBm.bmWidth, section.dsBm.bmHeight);
    }
    {
        const long long area = static_cast<long long>(clientWidth(w))
            * static_cast<long long>(clientHeight(w));
        CompatWindow* owner = asWindow(gPresentedWindow);
        const bool takeOver = [&]() {
            // ON-2-02a（实测发现）：**主人自己的后续帧必须无条件上屏**。
            //
            // 原规则只有"严格更大者才能接管"，而主人自己与主人面积相等，
            // `area > gPresentedArea` 恒假 → 第一次呈现之后，这个窗口**每一次**
            // 重绘都被拒。实测（主菜单挂机 15s，CONCORD_PRESENT_STATS=1）：
            //     paints=201 submissions=1 takeover={self=200 other=0}
            // 也就是进程内部画了 201 帧、屏幕只看得到第 1 帧——"菜单冻住"
            // 不是 CGImage 缓存（ON-2-01 已排除）、不是没有失效、不是没有 present，
            // 而是 present 前的这道门把自己人挡住了。
            if (gPresentedWindow != nullptr && toHwnd(w) == gPresentedWindow) return true;
            if (gPresentedWindow == nullptr || gPresentedArea <= 0) return true;  // 首个窗口
            if (owner != nullptr) return area > gPresentedArea;   // 主人还在：只能被更大者接管
            // 主人已销毁：允许接手，但门槛是"不小于历史面积的一半"。
            // 半数是刻意的宽松档：既挡得住 EDIT/STATIC 这类小控件，
            // 又不会因为"新界面比旧界面小一点"就白屏。
            return area * 2 >= gPresentedArea;
        }();
        if (!takeOver) {
            if (presentStatsEnabled()) {
                ++presentStats().skippedTakeover;
                // 区分"拒的是主人自己"还是"拒的是旁边的小控件"——前者会让屏幕
                // 永久停在这一帧，后者完全正常。混成一个数就分不出是哪种。
                if (toHwnd(w) == gPresentedWindow) ++presentStats().skippedTakeoverSelf;
                else ++presentStats().skippedTakeoverOther;
            }
            return;
        }
        gPresentedWindow = toHwnd(w);
        gPresentedArea = area;
        gPresentedWidth = clientWidth(w);
        gPresentedHeight = clientHeight(w);
    }
    concord_compat::apple::Surface surface;
    surface.pixels = bits;
    surface.width = section.dsBm.bmWidth;
    surface.height = section.dsBm.bmHeight;
    surface.bytesPerRow = surface.width * 4;
    const std::string title = concord_compat::utf8FromWideText(w->title.c_str());
    surface.titleUtf8 = title.c_str();
    surface.visible = w->visible && !w->iconic;
    if (presentStatsEnabled()) {
        // "同一帧被贴两次"的判据：同窗口（同一块 DIB 指针）、同尺寸、同可见性，
        // 且采样签名相同。任何一项不同都算新帧——那也可能是别的窗口接管了画面。
        PresentStatsCounters& counters = presentStats();
        PresentStatsLastSubmit& last = presentStatsLast();
        const std::uint64_t signature = sampleFrameSignature(
            bits, surface.width, surface.height, surface.bytesPerRow);
        const bool sameFrame = last.valid && last.bits == surface.pixels
            && last.width == surface.width && last.height == surface.height
            && last.visible == surface.visible && last.signature == signature;
        ++counters.submits;
        if (sameFrame) ++counters.redundantSubmits;
        last.bits = surface.pixels;
        last.width = surface.width;
        last.height = surface.height;
        last.visible = surface.visible;
        last.signature = signature;
        last.valid = true;
    }
    concord_compat::apple::present(surface);
    // 输入目标跟随**当前正在呈现的窗口**。
    //
    // U-03（2026-11-11）：这里原本是"只在第一次绑一次"：
    //     static HWND boundTarget = nullptr;
    //     if (boundTarget == nullptr) { ... setInputSink(..., boundTarget); }
    // 于是输入永远投给第一个 present 的窗口（主菜单/大厅那个）。主菜单→大厅→世界
    // 每一跳都换 HWND，世界渲染完全正常（present 跟着新窗口走），但鼠标键盘全部
    // PostMessageW 到那个已经退场的旧句柄，被丢弃 —— 症状就是"进了游戏画面在动，
    // 点哪儿都没反应"，且不报错、日志里没有任何异常。
    //
    // 改成一变化就重绑。放在 present 之后：present 已完成 NSApp/NSWindow 初始化，
    // 此时 AppKit 才可能产生事件。
    static HWND boundTarget = nullptr;
    const HWND presented = toHwnd(w);
    if (boundTarget != presented) {
        boundTarget = presented;
        concord_compat::apple::setInputSink(&appleInputDispatch, boundTarget);
        // U-14：同时把"全屏客户区尺寸"的接收方挂到这个窗口上。若不注册，
        // 全屏后游戏仍按窗口态尺寸绘制，屏幕上只能把小图拉伸铺满。
        concord_compat::apple::setResizeSink(&appleResizeRequest, boundTarget);
    }
}

BOOL EndPaint(HWND hWnd, const PAINTSTRUCT* lpPaint) {
    (void)lpPaint;
    CompatWindow* window = asWindow(hWnd);
    if (presentStatsEnabled()) ++presentStats().endPaintCalls;
    // Q-05：一帧画完（EndPaint）之后才导出/贴图，保证拿到的是完整帧。
    maybeDumpClientFrame(window);
    maybePresentClientFrame(window);
    return TRUE;
}

BOOL InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase) {
    (void)lpRect;
    (void)bErase;
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return FALSE;
    // ON-2-02a：置脏时**已经脏**说明这次失效不会产生新的 WM_PAINT —— 这就是
    // "同一帧被贴多次"的抑制机制发生的地方（失效层，不是呈现层）。
    if (presentStatsEnabled()) {
        if (w->dirty) ++presentStats().dirtyMarksMerged;
        else ++presentStats().dirtyMarks;
    }
    w->dirty = true;
    return TRUE;
}

BOOL ValidateRect(HWND hWnd, const RECT* lpRect) {
    (void)lpRect;
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return FALSE;
    w->dirty = false;
    return TRUE;
}

BOOL GetUpdateRect(HWND hWnd, LPRECT lpRect, BOOL bErase) {
    (void)bErase;
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return FALSE;
    if (lpRect != nullptr) {
        lpRect->left = 0;
        lpRect->top = 0;
        lpRect->right = w->dirty ? clientWidth(w) : 0;
        lpRect->bottom = w->dirty ? clientHeight(w) : 0;
    }
    return w->dirty ? TRUE : FALSE;
}

// U-14：全屏视口。宿主（Cocoa 侧）在进入/退出全屏时回调这里，接口见
// RuntimeAppleWindowSurface.h。
//
// 为什么必须由 compat 改窗口矩形：游戏的地图视口、侧边栏、点击命中、后缓冲
// 全部以 GetClientRect 为准。只把 NSWindow 内容区放大、不改这里，游戏仍按
// 1024x640 绘制，屏幕上只能靠 contentsGravity 把小图拉伸铺满——单位被放大，
// 一屏还是那么多内容（用户 2026-11-12 报的正是这个）。
//
// width/height <= 0 = 恢复进全屏之前的客户区尺寸。
void appleResizeRequest(int width, int height, void* context) {
    HWND hwnd = static_cast<HWND>(context);
    CompatWindow* w = asWindow(hwnd);
    if (w == nullptr) return;
    // 进全屏前的客户区尺寸。按窗口记账（换了呈现目标就重新记），这样
    // "退出全屏"永远回到那一刻的窗口态尺寸，而不是某次中间状态。
    static HWND recordedWindow = nullptr;
    static int windowedWidth = 0;
    static int windowedHeight = 0;
    if (recordedWindow != hwnd) {
        recordedWindow = hwnd;
        windowedWidth = 0;
        windowedHeight = 0;
    }
    if (width <= 0 || height <= 0) {
        if (windowedWidth <= 0 || windowedHeight <= 0) return;
        width = windowedWidth;
        height = windowedHeight;
    } else if (windowedWidth <= 0 || windowedHeight <= 0) {
        windowedWidth = clientWidth(w);
        windowedHeight = clientHeight(w);
    }
    if (width == clientWidth(w) && height == clientHeight(w)) return;
    // 只改尺寸、不动位置：纯离屏模型里位置只影响 GetWindowRect 的记账。
    w->windowRect.right = w->windowRect.left + width;
    w->windowRect.bottom = w->windowRect.top + height;
    // 通知游戏重排。WM_SIZE 走队列（不在 pumpEvents 的栈里做重活），
    // 再置脏让它在下一帧按新客户区重绘——客户区位图会在 BeginPaint 里
    // 按新尺寸重建（见 ensureSurface）。
    ::PostMessageW(hwnd, WM_SIZE, 0 /* SIZE_RESTORED */,
                   static_cast<LPARAM>(
                       (static_cast<unsigned long>(height) << 16) | (width & 0xFFFF)));
    InvalidateRect(hwnd, nullptr, FALSE);
    std::fprintf(stderr, "[cocoa] game client -> %dx%d\n", width, height);
    std::fflush(stderr);
}

BOOL RedrawWindow(HWND hWnd, const RECT* lprcUpdate, HRGN hrgnUpdate, UINT flags) {
    (void)lprcUpdate;
    (void)hrgnUpdate;
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return FALSE;
    if (presentStatsEnabled()) {
        if (w->dirty) ++presentStats().dirtyMarksMerged;
        else ++presentStats().dirtyMarks;
    }
    w->dirty = true;
    if ((flags & RDW_UPDATENOW) != 0) {
        if (presentStatsEnabled()) ++presentStats().paintFromRedrawNow;
        callWindowProc(hWnd, WM_PAINT, 0, 0);
    }
    return TRUE;
}

// ===========================================================================
// 消息队列
// ===========================================================================

BOOL PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    MSG msg{};
    msg.hwnd = hWnd;
    msg.message = Msg;
    msg.wParam = wParam;
    msg.lParam = lParam;
    std::lock_guard<std::mutex> lock(userMutex());
    messageQueue().push_back(msg);
    return TRUE;
}

void PostQuitMessage(int nExitCode) {
    PostMessageW(nullptr, WM_QUIT, static_cast<WPARAM>(nExitCode), 0);
}

// 从队列取一条匹配消息（不阻塞）。返回 true 表示取到并填入 lpMsg。
//
// S11：真实队列之外还要**合成**两类低优先级消息，这是 user32 的既有语义，
// 不是本层的发明：
//   · WM_TIMER —— 定时器到期时在"取消息"这一刻生成，从不入队（见
//     RuntimeWin32CompatTimer.cpp 文件头）。
//   · WM_PAINT —— 有窗口脏且队列为空时生成。没有这一条，定时器回调里的
//     InvalidateRect 只是置一个标志，屏幕永远不重画，菜单看起来仍然"冻住"。
//
// 合成的两条都只在 remove=true（PM_REMOVE / GetMessage）时成立：PeekMessage 的
// PM_NOREMOVE 语义是"看一眼但不动队列"，而合成消息没有队列位置可比，返回一条
// 不可消费的 WM_PAINT 会让调用方的 `while (PeekMessageW(..., PM_NOREMOVE)) { ... }`
// 变成死循环。
bool popMessage(LPMSG lpMsg, HWND hWnd, UINT filterMin, UINT filterMax, bool remove) {
    // ON-2-02a：所有消息取用（菜单的 GetMessageW 与世界主循环的 PeekMessageW）
    // 都经过这里，是检查"到点收尾"的唯一共同点。
    presentStatsMaybeExit();
    {
        std::lock_guard<std::mutex> lock(userMutex());
        auto& q = messageQueue();
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (messageMatches(*it, hWnd, filterMin, filterMax)) {
                *lpMsg = *it;
                if (remove) q.erase(it);
                return true;
            }
        }
    }
    if (!remove) return false;

    const bool timerInRange = rangeAccepts(WM_TIMER, filterMin, filterMax);
    const bool paintInRange = rangeAccepts(WM_PAINT, filterMin, filterMax);

    // 1) 定时器。回调形式的定时器（lpTimerFunc 非空）由**消息循环**调用，不产生
    // 消息——所以这里可能连续吃掉几条回调再返回，这正是真实行为。
    if (timerInRange) {
        for (;;) {
            concord_compat::TimerDue due{};
            if (!concord_compat::timerTakeDue(hWnd, true, due)) break;
            if (due.proc != nullptr) {
                // 回调里可能 PostMessage / InvalidateRect，所以**不能**在这里持
                // userMutex 调用；timerTakeDue 已经释放了它自己的锁。
                due.proc(due.hwnd, WM_TIMER, due.id, 0);
                continue;
            }
            *lpMsg = MSG{};
            lpMsg->hwnd = due.hwnd;
            lpMsg->message = WM_TIMER;
            lpMsg->wParam = static_cast<WPARAM>(due.id);
            lpMsg->lParam = 0;
            return true;
        }
    }

    // 2) 重绘。脏标志由 InvalidateRect/RedrawWindow 置位，由 BeginPaint/ValidateRect
    // 清位——本层不在这里清，否则"BeginPaint 之前又脏了一次"的合并语义会丢。
    if (paintInRange) {
        std::lock_guard<std::mutex> lock(userMutex());
        for (CompatWindow* window : windowTable()) {
            if (window == nullptr || !window->dirty) continue;
            if (hWnd != nullptr && toHwnd(window) != hWnd) continue;
            *lpMsg = MSG{};
            lpMsg->hwnd = toHwnd(window);
            lpMsg->message = WM_PAINT;
            return true;
        }
    }
    return false;
}

BOOL PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax,
                  UINT wRemoveMsg) {
    if (lpMsg == nullptr) return FALSE;
    const bool remove = (wRemoveMsg & PM_REMOVE) != 0;
    if (popMessage(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, remove)) return TRUE;
    // U-02（2026-11-11）：队列空了才排空 AppKit，再重试一次出队。
    //
    // 世界主循环不走 GetMessageW：它每帧 `drainMessages()` = PeekMessageW 一圈
    // + `sleepForIdleSlice()` = 裸 sleep_for(1ms)。于是进世界之后同样没有任何人
    // 排空 AppKit —— 画面在动（present 是 active 触发的），但鼠标键盘进不来，
    // 系统也认定应用无响应（采样：世界循环 557/1226 样本在 sleepForIdleSlice，
    // pumpEvents 出现 0 次）。症状就是"进了游戏之后鼠标一直是彩虹圈"。
    //
    // 节流 4ms：PeekMessageW 在 smoke 的空转轮询里可能被高频调用，而 pumpEvents
    // 内部最多等 2ms，不节流会把那些循环拖慢一个量级。4ms 对 60fps（16.7ms/帧）
    // 足够——每帧至少还能排空一次。
    {
        static thread_local std::chrono::steady_clock::time_point lastPeekPump{};
        const auto now = std::chrono::steady_clock::now();
        if (lastPeekPump.time_since_epoch().count() == 0
            || now - lastPeekPump >= std::chrono::milliseconds(4)) {
            lastPeekPump = now;
            concord_compat::apple::pumpEvents();
            if (popMessage(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, remove)) return TRUE;
        }
    }
    return FALSE;
}

BOOL GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) {
    if (lpMsg == nullptr) return -1;
    // 阻塞取消息：无消息时按"下一次定时器到期"睡眠（没有定时器就 1ms 轮询）。
    // 上限 8ms 是留给人手投递的跨线程消息：PostMessageW 不会唤醒这里的睡眠。
    for (;;) {
        if (popMessage(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, true)) {
            if (lpMsg->message == WM_QUIT) return 0;
            return 1;
        }
        // Q-06：主循环是 compat 自己的 GetMessageW，不跑 NSRunLoop，
        // 所以必须在这里主动排空 AppKit 事件——否则事件永远躺在 AppKit 队列里，
        // 窗口看得见、点不动。pumpEvents 内部不阻塞。
        concord_compat::apple::pumpEvents();
        if (popMessage(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, true)) {
            if (lpMsg->message == WM_QUIT) return 0;
            return 1;
        }
        const long long untilNext = concord_compat::timerMillisUntilNext();
        const auto waitMs = untilNext < 0 ? 1 : (untilNext == 0 ? 0 : std::min<long long>(untilNext, 8));
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs > 0 ? waitMs : 1));
    }
}

BOOL WaitMessage() {
    // 阻塞直到有消息。与 GetMessageW 共用轮询语义；定时器到期与脏窗口也算"有消息"，
    // 否则 WaitMessage 会永久挂住（真实 API 里 WM_TIMER 正是唤醒它的来源之一）。
    for (;;) {
        // U-01：与 MsgWaitForMultipleObjectsEx 同理——等待期间不排空 AppKit，
        // 调用方就永远等不到消息（自己的队列只能由 pumpEvents 填充）。
        concord_compat::apple::pumpEvents();
        {
            std::lock_guard<std::mutex> lock(userMutex());
            if (!messageQueue().empty()) return TRUE;
            for (CompatWindow* window : windowTable()) {
                if (window != nullptr && window->dirty) return TRUE;
            }
        }
        if (concord_compat::timerMillisUntilNext() == 0) return TRUE;
        const long long untilNext = concord_compat::timerMillisUntilNext();
        const auto waitMs = untilNext < 0 ? 1 : (untilNext == 0 ? 0 : std::min<long long>(untilNext, 8));
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs > 0 ? waitMs : 1));
    }
}

DWORD MsgWaitForMultipleObjectsEx(DWORD nCount, const HANDLE* pHandles,
                                  DWORD dwMilliseconds, DWORD dwWakeMask, DWORD dwFlags) {
    (void)nCount;
    (void)pHandles;
    (void)dwWakeMask;
    (void)dwFlags;
    // 纯离屏无真实内核对象可等。语义："有消息则立刻返回 WAIT_OBJECT_0，否则
    // 等满 dwMilliseconds 返回 WAIT_TIMEOUT"。调用点是菜单的 16ms 节流循环。
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(dwMilliseconds);
    for (;;) {
        // U-01（2026-11-11）：等待期间必须排空 AppKit 事件。
        //
        // 此前只有 GetMessageW 调 pumpEvents，而遭遇战大厅的主循环是
        // `PeekMessageW + MsgWaitForMultipleObjectsEx(5ms)`，从不经过 GetMessageW，
        // 于是从大厅打开那一刻起就没有任何人在排空 AppKit 队列：窗口画面停在最初
        // 那一帧（看着正常），鼠标点击全部躺在 AppKit 里进不来。用户点「開始遊戲」
        // 时系统来查应用是否响应，直接给彩虹圈——症状看起来像「点了开始就卡死」，
        // 实际是整段大厅期间不处理事件（采样：主线程 1196/1198 个样本停在
        // MsgWaitForMultipleObjectsEx 的 sleep_for）。
        //
        // 放在这里而不是各调用点：Win32 的 MsgWaitForMultipleObjects 本身就是
        // 「等待期间处理消息」，语义一致；而且这是平台层文件（Windows 不编译），
        // 不必在共享的菜单代码里加 #ifdef __APPLE__（方案 §5.2 禁止）。
        // pumpEvents 自带 gWindow/gInputSink 前置判断，离屏 smoke 下立即返回。
        concord_compat::apple::pumpEvents();
        {
            std::lock_guard<std::mutex> lock(userMutex());
            if (!messageQueue().empty()) return WAIT_OBJECT_0;
        }
        if (std::chrono::steady_clock::now() >= deadline) return WAIT_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ===========================================================================
// 消息派发
// ===========================================================================

BOOL TranslateMessage(const MSG* lpMsg) {
    // 无键盘字符映射（无真实输入源），WM_KEYDOWN 不派生 WM_CHAR。诚实返回 FALSE。
    (void)lpMsg;
    return FALSE;
}

LRESULT DispatchMessageW(const MSG* lpMsg) {
    if (lpMsg == nullptr) return 0;
    if (lpMsg->hwnd == nullptr) return 0;
    // U-07：真实 Windows 的键状态由系统维护，消息一入队 GetKeyState 就能反映。
    // 这里没有系统，只能在派发时补上——好处是**合成消息**（smoke 的
    // PostMessageW(WM_KEYDOWN)）也走同一条语义，验证不必依赖真键盘。
    switch (lpMsg->message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        concord_compat::setVirtualKeyDown(static_cast<int>(lpMsg->wParam), true);
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        concord_compat::setVirtualKeyDown(static_cast<int>(lpMsg->wParam), false);
        break;
    default:
        break;
    }
    if (lpMsg->message == WM_PAINT && presentStatsEnabled()) ++presentStats().paintViaQueue;
    return callWindowProc(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
}

LRESULT SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    return callWindowProc(hWnd, Msg, wParam, lParam);
}

LRESULT CallWindowProcW(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg, WPARAM wParam,
                        LPARAM lParam) {
    if (lpPrevWndFunc == nullptr) return 0;
    return lpPrevWndFunc(hWnd, Msg, wParam, lParam);
}

LRESULT DefWindowProcW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    (void)wParam;
    (void)lParam;
    switch (Msg) {
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        // 真实 Windows 的 DefWindowProc(WM_DESTROY) **什么都不做**。投 WM_QUIT
        // 是应用自己的事——最典型的写法是在主窗口的 WM_DESTROY 里
        // `PostQuitMessage(0); return 0;`。
        //
        // 本层曾经在这里 PostQuitMessage(0)，本意是"最后一个窗口关掉就退出"。
        // 那是**错的**，而且错得很隐蔽：它让**任何**窗口的销毁都杀掉整个消息
        // 循环，包括子窗口和测试用的临时窗口。实测证据（`--main-menu-ui-smoke-test`）：
        //     [quit-trace] WM_DESTROY class=ConcordRTS.RuntimeMainMenu parent=0x… child=1  ← 子窗口
        //     [quit-trace] WM_DESTROY class=STATIC                   parent=0x0 child=0  ← smoke 自建的宿主壳
        // 主菜单窗口是宿主壳的子窗口，它一销毁，菜单→遭遇战异步转场的主循环就被
        // 这条 WM_QUIT 带走，报 "skirmish transition received WM_QUIT"。
        //
        // 试过"只在销毁最后一个顶层窗口时才补 quit"——同样不行：smoke 自己就会
        // 建一个 STATIC 顶层壳再销毁它，那时它正好是"最后一个顶层窗口"。
        //
        // 所以这里保持忠实：不投。代价是**独立**菜单/遭遇战可执行文件在主窗口
        // 销毁后不会自动退出（GetMessage 会一直阻塞）——但真 Windows 上同样
        // 如此，那是产品侧该显式 PostQuitMessage 的地方，不该由默认过程代劳。
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hWnd, &ps);
        if (dc != nullptr) EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        // 真 Windows 的默认过程：命中客户区就把**类光标**装上并返回 TRUE；
        // 非客户区返回 FALSE，交给父窗口/系统继续。
        //
        // 曾经这里无条件 return TRUE 且不装光标——后果很具体：EDIT 之类的内建
        // 控件不处理 WM_SETCURSOR，落到默认过程后 GetCursor() 仍是 nullptr，
        // 于是"编辑框该恢复 I 形光标"这类断言必红（遭遇战 cursor smoke 的
        // `native name edit lost its pointer`），而且真机上表现为"点了输入框
        // 光标没变"，谁都查不出是默认过程的锅。
        if (LOWORD(lParam) == HTCLIENT) {
            CompatWindow* target = asWindow(hWnd);
            HCURSOR classCursor = target != nullptr ? target->cursor : nullptr;
            // 类没给光标时退到箭头：Windows 会把这种情况一路交给父窗口，最终
            // 落到箭头上；这里直接给，避免"光标凭空消失"。
            if (classCursor == nullptr) {
                classCursor = reinterpret_cast<HCURSOR>(static_cast<std::uintptr_t>(32512u));
            }
            // U-05：这里只记账。产品的 SetCursor 请求在这套引擎里不代表"系统指针
            // 该不该出现"（世界每秒几十次 StockArrow，而游戏同时自绘箭头），
            // 所以 SetCursor 本身也不再镜像到真实指针，默认过程直接用即可。
            SetCursor(classCursor);
            return TRUE;
        }
        return FALSE;
    default:
        return 0;
    }
}

// ===========================================================================
// 焦点 / 前台
// ===========================================================================

HWND SetFocus(HWND hWnd) {
    std::lock_guard<std::mutex> lock(userMutex());
    HWND previous = focusWindow();
    focusWindow() = (asWindow(hWnd) != nullptr) ? hWnd : nullptr;
    return previous;
}

HWND GetFocus() {
    std::lock_guard<std::mutex> lock(userMutex());
    return focusWindow();
}

BOOL SetForegroundWindow(HWND hWnd) {
    std::lock_guard<std::mutex> lock(userMutex());
    if (asWindow(hWnd) == nullptr) return FALSE;
    foregroundWindow() = hWnd;
    return TRUE;
}

HWND GetForegroundWindow() {
    std::lock_guard<std::mutex> lock(userMutex());
    return foregroundWindow();
}

HWND GetWindow(HWND hWnd, UINT uCmd) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr) return nullptr;
    auto& table = windowTable();
    std::lock_guard<std::mutex> lock(userMutex());
    // 在窗口表里找 w 的位置。
    auto it = std::find(table.begin(), table.end(), w);
    if (it == table.end()) return nullptr;
    switch (uCmd) {
    case GW_HWNDFIRST:
        return table.empty() ? nullptr : toHwnd(table.front());
    case GW_HWNDNEXT:
        return (it + 1 != table.end()) ? toHwnd(*(it + 1)) : nullptr;
    case GW_CHILD:
        return w->children.empty() ? nullptr : w->children.front();
    default:
        return nullptr;
    }
}

HDESK GetThreadDesktop(DWORD dwThreadId) {
    (void)dwThreadId;
    // 纯离屏只有一个桌面。返回稳定的非空伪句柄（不是真 HDESK，只作身份标记）。
    return reinterpret_cast<HDESK>(static_cast<std::intptr_t>(1));
}

// ===========================================================================
// 枚举 / 查找
// ===========================================================================

BOOL EnumThreadWindows(DWORD dwThreadId, BOOL (*lpfn)(HWND, LPARAM), LPARAM lParam) {
    if (lpfn == nullptr) return FALSE;
    std::vector<HWND> snapshot;
    {
        std::lock_guard<std::mutex> lock(userMutex());
        for (CompatWindow* w : windowTable()) snapshot.push_back(toHwnd(w));
    }
    (void)dwThreadId;  // 纯离屏全部窗口都属本线程
    for (HWND h : snapshot) {
        if (!lpfn(h, lParam)) return FALSE;
    }
    return TRUE;
}

BOOL EnumDesktopWindows(HDESK hDesktop, BOOL (*lpfn)(HWND, LPARAM), LPARAM lParam) {
    (void)hDesktop;
    return EnumThreadWindows(0, lpfn, lParam);
}

HWND FindWindowExW(HWND hWndParent, HWND hWndChildAfter, LPCWSTR lpszClass,
                   LPCWSTR lpszWindow) {
    std::lock_guard<std::mutex> lock(userMutex());
    bool afterSeen = (hWndChildAfter == nullptr);
    for (CompatWindow* w : windowTable()) {
        HWND h = toHwnd(w);
        // 父窗口过滤
        if (hWndParent != nullptr && w->parent != hWndParent) continue;
        // 类名过滤
        if (lpszClass != nullptr && w->className != lpszClass) continue;
        // 标题过滤
        if (lpszWindow != nullptr && w->title != lpszWindow) continue;
        // 从 hWndChildAfter 之后开始
        if (!afterSeen) {
            if (h == hWndChildAfter) afterSeen = true;
            continue;
        }
        return h;
    }
    return nullptr;
}

DWORD GetWindowThreadProcessId(HWND hWnd, LPDWORD lpdwProcessId) {
    if (asWindow(hWnd) == nullptr) return 0;
    if (lpdwProcessId != nullptr) *lpdwProcessId = ::GetCurrentProcessId();
    return currentThreadId();
}

// ===========================================================================
// 系统信息
// ===========================================================================

int GetSystemMetrics(int nIndex) {
    switch (nIndex) {
    case SM_CXSCREEN: return kVirtualScreenW;
    case SM_CYSCREEN: return kVirtualScreenH;
    case SM_CXVIRTUALSCREEN: return kVirtualScreenW;
    case SM_CYVIRTUALSCREEN: return kVirtualScreenH;
    case SM_XVIRTUALSCREEN: return 0;
    case SM_YVIRTUALSCREEN: return 0;
    case SM_REMOTESESSION: return 0;  // 本地会话
    default: return 0;
    }
}

BOOL SystemParametersInfoW(UINT uiAction, UINT uiParam, LPVOID pvParam, UINT fWinIni) {
    (void)uiParam;
    (void)fWinIni;
    if (uiAction == SPI_GETWORKAREA) {
        if (pvParam == nullptr) return FALSE;
        RECT* work = static_cast<RECT*>(pvParam);
        work->left = 0;
        work->top = 0;
        work->right = kVirtualScreenW;
        work->bottom = kVirtualScreenH;
        return TRUE;
    }
    // 其余 SPI_* 诚实不处理（无真实宿主配置可读/写）。
    return FALSE;
}

HMONITOR MonitorFromWindow(HWND hwnd, DWORD dwFlags) {
    (void)hwnd;
    (void)dwFlags;
    // 纯离屏只有一块虚拟主屏。
    return reinterpret_cast<HMONITOR>(static_cast<std::intptr_t>(1));
}

BOOL GetMonitorInfoW(HMONITOR hMonitor, LPMONITORINFO lpmi) {
    (void)hMonitor;
    if (lpmi == nullptr || lpmi->cbSize == 0) return FALSE;
    lpmi->rcMonitor.left = 0;
    lpmi->rcMonitor.top = 0;
    lpmi->rcMonitor.right = kVirtualScreenW;
    lpmi->rcMonitor.bottom = kVirtualScreenH;
    lpmi->rcWork = lpmi->rcMonitor;
    lpmi->dwFlags = MONITORINFOF_PRIMARY;
    return TRUE;
}

BOOL GetSystemPowerStatus(LPSYSTEM_POWER_STATUS lpSystemPowerStatus) {
    if (lpSystemPowerStatus == nullptr) return FALSE;
    // 纯离屏宿主无真实电源：恒报"交流电在线、电量满、无电池"。这是如实默认
    // 而非编造——软件帧缓冲不依赖电池状态。
    lpSystemPowerStatus->ACLineStatus = AC_LINE_ONLINE;
    lpSystemPowerStatus->BatteryFlag = 128;  // 无系统电池
    lpSystemPowerStatus->BatteryLifePercent = 100;
    lpSystemPowerStatus->SystemStatusFlag = 0;
    lpSystemPowerStatus->BatteryLifeTime = 0xFFFFFFFFu;
    lpSystemPowerStatus->BatteryFullLifeTime = 0xFFFFFFFFu;
    return TRUE;
}

// ===========================================================================
// 杂项
// ===========================================================================

HMENU GetMenu(HWND hWnd) {
    CompatWindow* w = asWindow(hWnd);
    return (w != nullptr) ? w->menu : nullptr;
}

BOOL ScreenToClient(HWND hWnd, LPPOINT lpPoint) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr || lpPoint == nullptr) return FALSE;
    lpPoint->x -= w->windowRect.left;
    lpPoint->y -= w->windowRect.top;
    return TRUE;
}

BOOL ClientToScreen(HWND hWnd, LPPOINT lpPoint) {
    CompatWindow* w = asWindow(hWnd);
    if (w == nullptr || lpPoint == nullptr) return FALSE;
    lpPoint->x += w->windowRect.left;
    lpPoint->y += w->windowRect.top;
    return TRUE;
}

BOOL MessageBeep(UINT uType) {
    (void)uType;
    // 无真实音频后端，诚实静默返回 TRUE（调用点不依赖声音）。
    return TRUE;
}

int MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType) {
    (void)hWnd;
    (void)lpText;
    (void)lpCaption;
    (void)uType;
    // 诊断 smoke 只判断返回值（IDOK）。不开对话框，打印一条日志便于排查。
    std::fprintf(stderr, "[compat/win32] MessageBoxW: 纯离屏无对话框，返回 IDOK\n");
    return IDOK;
}

// ===========================================================================
// 钩子
// ===========================================================================

HHOOK SetWindowsHookExW(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId) {
    (void)hmod;
    if (lpfn == nullptr) return nullptr;
    // 只支持 WH_CBT；其余钩子类型诚实失败（不假装）。
    if (idHook != WH_CBT) {
        std::fprintf(stderr, "[compat/win32] SetWindowsHookExW: 仅支持 WH_CBT，"
                             "得到 idHook=%d，诚实失败\n", idHook);
        return nullptr;
    }
    // 只支持本线程钩子（dwThreadId==0 或当前线程）。跨线程注入无宿主可依。
    if (dwThreadId != 0 && dwThreadId != currentThreadId()) {
        std::fprintf(stderr, "[compat/win32] SetWindowsHookExW: 仅支持本线程钩子，"
                             "得到 dwThreadId=%lu，诚实失败\n",
                     static_cast<unsigned long>(dwThreadId));
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(userMutex());
    HookEntry entry;
    entry.id = nextHookId()++;
    entry.proc = lpfn;
    entry.threadId = dwThreadId;
    hookTable().push_back(entry);
    return reinterpret_cast<HHOOK>(static_cast<std::intptr_t>(entry.id));
}

BOOL UnhookWindowsHookEx(HHOOK hhk) {
    if (hhk == nullptr) return FALSE;
    const int id = static_cast<int>(reinterpret_cast<std::intptr_t>(hhk));
    std::lock_guard<std::mutex> lock(userMutex());
    auto& hooks = hookTable();
    for (auto it = hooks.begin(); it != hooks.end(); ++it) {
        if (it->id == id) {
            hooks.erase(it);
            return TRUE;
        }
    }
    return FALSE;
}

LRESULT CallNextHookEx(HHOOK hhk, int nCode, WPARAM wParam, LPARAM lParam) {
    (void)hhk;
    (void)nCode;
    (void)wParam;
    (void)lParam;
    // 本层没有钩子链（钩子在 fireCbtHook 里已按注册顺序全部回调），
    // "下一个钩子"不存在，返回 0。
    return 0;
}

}  // extern "C"
