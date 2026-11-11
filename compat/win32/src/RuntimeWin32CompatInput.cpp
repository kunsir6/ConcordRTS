// Win32 输入 / 光标 / 捕获 / 定时器族（P5-05）。
//
// 这一族的共同处境：它们都依赖一个**真实的事件循环与窗口宿主**（WM_* 消息、
// 鼠标路由、光标资源、定时器派发），而 macOS 侧的窗口系统（S7）尚未落地。所以
// 判据不是"能不能像 Windows 一样响应"，而是"在没有宿主的情况下，返回什么才是
// 诚实的"：
//
//   · 键盘：无真实键状态 → 状态数组恒 0（"键未按下"）。返回 0 是诚实，返回一个
//     编造的按位组合是撒谎。
//   · 光标：无真实光标资源 → LoadCursorW 对标准 ID 返回稳定的非 NULL 伪句柄
//     （调用点 `SetCursor(LoadCursorW(...))` 拿 NULL 当"隐藏光标"，返回 NULL 会
//     静默把"加载失败"误读成"故意隐藏"）；对自定义字符串名返回 NULL（加载失败）。
//   · 捕获：SetCapture/GetCapture/ReleaseCapture 本质是"哪个窗口当前拥有鼠标捕获"
//     的**逻辑状态**，不依赖真实鼠标路由。维护这个状态比返回 NULL 更接近 Win32
//     语义，也能让产品里 `SetCapture(w); GetCapture()==w` 的判据成立。
//   · 定时器：无事件循环派发 WM_TIMER → SetTimer/KillTimer 是纯登记桩。返回 TRUE
//     表示"登记成功"，只是永不触发；返回 FALSE 会让调用方误判"出错"走错误分支，
//     而实际上两种返回值都不会让菜单自动刷新——所以 TRUE 是更安全的桩。
//   · SendInput：合成输入事件注入依赖真实输入队列 → 诚实返回 0（一个都没注入）。
//   · DrawFrameControl：项目唯一调用点是外交页复选框的 fallback 路径（先试 pcx
//     贴图，失败才走这里）。用已实现的 GDI 画一个真方框 + 勾，比"什么都不画还
//     返回 TRUE"更诚实。

#include <windows.h>

#include "RuntimeWin32CompatInternal.h"
#include "RuntimeAppleWindowSurface.h"

#include <cstdint>
#include <cstring>

namespace {

// 键盘状态：256 个虚拟键，默认全 0（未按下、未切换）。
BYTE g_keyState[256] = {0};

// 当前光标。无真实光标资源，初始为"无光标"。
HCURSOR g_cursor = nullptr;

// 逻辑光标位置。无真实屏幕坐标，初始 (0,0)。
POINT g_cursorPos = {0, 0};

// 鼠标捕获窗口。无真实鼠标路由，初始"无捕获"。
HWND g_capture = nullptr;

}  // namespace

// 与 Gdi.cpp 一致：对外 API 一律落进显式 extern "C" 块，保证 C linkage 与
// windows.h 的声明对上（落在块外会变成 C++ mangled 符号，链接仍报未定义）。
extern "C" {

// ---------------------------------------------------------------------------
// 键盘
// ---------------------------------------------------------------------------

SHORT GetKeyState(int nVirtKey) {
    if (nVirtKey < 0 || nVirtKey >= 256) return 0;
    return static_cast<SHORT>(g_keyState[nVirtKey]);
}

SHORT GetAsyncKeyState(int vKey) {
    (void)vKey;
    // 无真实异步输入队列，键恒"未按下"。
    return 0;
}

BOOL GetKeyboardState(BYTE* lpKeyState) {
    if (lpKeyState == nullptr) return FALSE;
    std::memcpy(lpKeyState, g_keyState, 256);
    return TRUE;
}

BOOL SetKeyboardState(const BYTE* lpKeyState) {
    if (lpKeyState == nullptr) return FALSE;
    std::memcpy(g_keyState, lpKeyState, 256);
    return TRUE;
}

UINT MapVirtualKeyW(UINT uCode, UINT uMapType) {
    (void)uCode;
    (void)uMapType;
    // 无真实键盘布局（键码 ↔ 扫描码 ↔ 字符 的映射表）。
    return 0;
}

int GetKeyNameTextW(LONG lParam, LPWSTR lpString, int cchSize) {
    (void)lParam;
    if (lpString != nullptr && cchSize > 0) lpString[0] = L'\0';
    // 无键名文本。返回 0 = "名字为空"，调用方（RuntimeShell 的键位显示）走
    // fallback 而不是拿着空串当键名。
    return 0;
}

UINT SendInput(UINT cInputs, LPINPUT pInputs, int cbSize) {
    (void)cInputs;
    (void)pInputs;
    (void)cbSize;
    // 无事件循环，无法合成注入。诚实返回 0 = "一个事件都没送出去"，而不是
    // 假装送出了 cInputs 个。
    return 0;
}

// ---------------------------------------------------------------------------
// 光标
// ---------------------------------------------------------------------------

HCURSOR GetCursor() {
    return g_cursor;
}

BOOL GetCursorInfo(PCURSORINFO pci) {
    if (pci == nullptr || pci->cbSize < sizeof(CURSORINFO)) return FALSE;
    pci->flags = 0;  // 无可见光标（CURSOR_SHOWING 位清 0）
    pci->hCursor = g_cursor;
    pci->ptScreenPos = g_cursorPos;
    return TRUE;
}

BOOL GetCursorPos(LPPOINT lpPoint) {
    if (lpPoint == nullptr) return FALSE;
    *lpPoint = g_cursorPos;
    return TRUE;
}

HCURSOR SetCursor(HCURSOR hCursor) {
    HCURSOR old = g_cursor;
    g_cursor = hCursor;
    // U-05 结论：**不要**把这里的请求镜像到真实 macOS 指针。
    //
    // 曾按 Windows 语义镜像（nullptr→隐藏、handle→显示箭头），理由看起来很充分：
    // 产品用 SetCursor 表达每一屏的意图（RuntimeMainMenu 的 WM_SETCURSOR、
    // CampaignMenuState::setHandCursor）。但截图实测推翻了它：
    //
    //   hide=0（显示系统指针）：鼠标处出现**两个**箭头——白色描边系统箭头
    //                           + 灰色游戏自绘箭头，紧挨着。
    //   hide=1（隐藏系统指针）：只剩游戏那个灰色箭头，位置一致、大小正常。
    //
    // 关键点：世界里产品每秒几十次请求 StockArrow(32512)，而游戏**同时**在帧里
    // 画了自己的箭头。所以产品的 SetCursor 请求在这套引擎里并不代表"系统指针该
    // 不该出现"，它只是 Windows 时代的历史痕迹；真正的指针是自绘的那一个。
    // 因此 macOS 侧一律隐藏系统指针（U-04 的 cursor rect），这里只记账。
    return old;
}

BOOL SetCursorPos(int X, int Y) {
    g_cursorPos.x = X;
    g_cursorPos.y = Y;
    return TRUE;
}

HCURSOR LoadCursorW(HINSTANCE hInstance, LPCWSTR lpCursorName) {
    (void)hInstance;
    // MAKEINTRESOURCEW 的标准系统光标 ID 落在 [32512, 32650]（IDC_ARROW=32512 …
    // IDC_SIZEALL=32646 等）。给一个稳定的非 NULL 伪句柄（编码原 ID，便于观察）；
    // 返回 NULL 会被 SetCursor 语义读成"隐藏光标"，那是把加载失败静默改行为。
    const std::uintptr_t id = reinterpret_cast<std::uintptr_t>(lpCursorName);
    if (id >= 32512 && id <= 32650) {
        return reinterpret_cast<HCURSOR>(id);
    }
    // 自定义光标名（真字符串）：本层无光标资源，诚实加载失败。
    return nullptr;
}

// ---------------------------------------------------------------------------
// 鼠标捕获
// ---------------------------------------------------------------------------

HWND GetCapture() {
    return g_capture;
}

HWND SetCapture(HWND hWnd) {
    HWND old = g_capture;
    g_capture = hWnd;
    return old;
}

BOOL ReleaseCapture() {
    if (g_capture == nullptr) return FALSE;  // 无捕获可释放
    g_capture = nullptr;
    return TRUE;
}

// ---------------------------------------------------------------------------
// 定时器（S11 起是真实现）
// ---------------------------------------------------------------------------
// P5-05 版本是"登记即成功"的桩：两条都直接 return TRUE，于是产品 SetTimer 之后
// 收不到任何 WM_TIMER，菜单动画与 EOF 重启全部静止（见 queue.rules.
// wm_timer_never_delivered）。真正的登记表与"取消息时合成"的逻辑在
// RuntimeWin32CompatTimer.cpp / User.cpp，这里只做转发。
//
// 返回值语义按真实 API：KillTimer 对不存在的定时器返回 FALSE（调用点用它判断
// "有没有真的停掉一个"），SetTimer 失败才是 FALSE。
BOOL SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse,
              void (*lpTimerFunc)(HWND, UINT, UINT_PTR, DWORD)) {
    if (!concord_compat::timerCreate(hWnd, nIDEvent, uElapse, lpTimerFunc)) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

BOOL KillTimer(HWND hWnd, UINT_PTR uIDEvent) {
    return concord_compat::timerRemove(hWnd, uIDEvent) ? TRUE : FALSE;
}

DWORD GetDoubleClickTime() {
    // Windows 系统默认双击时间间隔 500 ms。无宿主可查，返回标准值。
    return 500;
}

// ---------------------------------------------------------------------------
// 鼠标离开跟踪
// ---------------------------------------------------------------------------

BOOL TrackMouseEvent(LPTRACKMOUSEEVENT lpEventTrack) {
    (void)lpEventTrack;
    // 桩：无真实鼠标路由，无法发 WM_MOUSELEAVE / WM_MOUSEHOVER。返回 TRUE 表示
    // "请求已登记"，避免调用方（菜单代码，不检查返回值）走错误分支。
    return TRUE;
}

// ---------------------------------------------------------------------------
// 标准控件边框
// ---------------------------------------------------------------------------

BOOL DrawFrameControl(HDC hdc, LPRECT lprc, UINT type, UINT state) {
    if (hdc == nullptr || lprc == nullptr) return FALSE;
    // 本层只实现按钮族——那是项目里唯一的调用点（外交页复选框）。标题栏 / 菜单 /
    // 滚动条这类真实控件边框需要主题绘制，诚实不画。
    if (type != DFC_BUTTON) return FALSE;

    const bool checked = (state & DFCS_CHECKED) != 0;
    const bool inactive = (state & DFCS_INACTIVE) != 0;
    const COLORREF edge = inactive ? RGB(128, 128, 128) : RGB(0, 0, 0);

    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    if (pen == nullptr) return FALSE;
    HGDIOBJ oldPen = SelectObject(hdc, reinterpret_cast<HGDIOBJ>(pen));
    // 空画刷：只描边不填充，复选框内部保持透明（pcx 失败时才走这里的 fallback，
    // 填充成实色会盖住底下不该盖的东西）。
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, lprc->left, lprc->top, lprc->right, lprc->bottom);
    if (checked) {
        // √ 形折线：左边中点 → 中间底部 → 右边顶部。
        const int left = lprc->left + 2;
        const int right = lprc->right - 2;
        const int top = lprc->top + 2;
        const int bottom = lprc->bottom - 2;
        const int midX = (left + right) / 2;
        const int midY = (top + bottom) / 2;
        MoveToEx(hdc, left, midY, nullptr);
        LineTo(hdc, midX, bottom);
        LineTo(hdc, right, top);
    }
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(reinterpret_cast<HGDIOBJ>(pen));
    return TRUE;
}

}  // extern "C"

