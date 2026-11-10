// compat/win32 自测 · 输入 / 光标 / 捕获 / 定时器（P5-05）
//
// 这一族的风险不在"返回值对不对"，而在"没有真实窗口宿主时，返回什么才是诚实
// 的"。所以这里断言的是**诚实默认**本身：
//   · 键盘状态恒 0（未按下），SetKeyboardState → GetKeyState 往返成立；
//   · LoadCursorW(IDC_ARROW) 返回稳定的非 NULL 伪句柄（NULL 会被读成"隐藏光标"）；
//   · SetCapture → GetCapture 维护逻辑状态（返回 NULL 会让产品判据失效）；
//   · SendInput 诚实返回 0（一个事件都没注入）；
//   · DrawFrameControl 真画了东西（位图里出现非零像素），不是"什么都不画还返回
//     TRUE"。
//
// 构建与运行：
//   c++ -std=c++20 -I compat/win32/include \
//       compat/win32/src/RuntimeWin32CompatInput.cpp \
//       compat/win32/src/RuntimeWin32CompatGdi.cpp \
//       compat/win32/selftest/CompatInputSemantics.cpp -o /tmp/cci && /tmp/cci

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <vector>

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

void checkEqual(std::uint32_t expected, std::uint32_t actual, const char* description) {
    ++gChecks;
    if (expected != actual) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected %u, got %u)\n", description, expected, actual);
    }
}

// 构造一个 32bpp 内存 DC（用于 DrawFrameControl 的"真画了"验证）。
struct Dib {
    HBITMAP handle = nullptr;
    void* bits = nullptr;
    HDC dc = nullptr;

    bool create(int width, int height) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // top-down：row 0 在内存最前
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        handle = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (handle == nullptr) return false;
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) return false;
        SelectObject(dc, reinterpret_cast<HGDIOBJ>(handle));
        return true;
    }

    ~Dib() {
        if (dc != nullptr) DeleteDC(dc);
        if (handle != nullptr) DeleteObject(reinterpret_cast<HGDIOBJ>(handle));
    }

    // 读取 (x, y) 处像素（0xAARRGGBB）。只对 32bpp top-down 位图有效。
    std::uint32_t peek(int x, int y) const {
        if (bits == nullptr) return 0;
        const auto* p = static_cast<const std::uint32_t*>(bits);
        return p[y * 32 + x];  // 调用方给的位图恒为 32 像素宽、stride 无填充
    }
};

void testKeyboard() {
    std::printf("[键盘]\n");

    check(GetKeyState(VK_ESCAPE) == 0, "GetKeyState 默认 0（未按下）");
    check(GetAsyncKeyState(VK_RETURN) == 0, "GetAsyncKeyState 默认 0");
    check(MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC) == 0, "MapVirtualKeyW 诚实返回 0");

    BYTE state[256] = {0};
    check(GetKeyboardState(state) == TRUE, "GetKeyboardState 返回 TRUE");
    bool allZero = true;
    for (int i = 0; i < 256; ++i) {
        if (state[i] != 0) allZero = false;
    }
    check(allZero, "GetKeyboardState 默认全 0");

    state[VK_ESCAPE] = 0x80;  // 模拟按下 ESC
    check(SetKeyboardState(state) == TRUE, "SetKeyboardState 返回 TRUE");
    check(GetKeyState(VK_ESCAPE) == 0x80, "Set 后 GetKeyState 反映（0x80 按下位）");
    check(GetKeyState(VK_RETURN) == 0, "Set 不影响其它键");

    wchar_t name[8] = {L'X', L'X', L'X'};
    check(GetKeyNameTextW(0, name, 8) == 0, "GetKeyNameTextW 诚实返回 0（无键名）");

    INPUT in{};
    in.type = INPUT_KEYBOARD;
    check(SendInput(1, &in, sizeof(INPUT)) == 0, "SendInput 诚实返回 0（无事件循环）");
}

void testCursor() {
    std::printf("[光标]\n");

    check(GetCursor() == nullptr, "GetCursor 默认无光标");

    HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    check(arrow != nullptr, "LoadCursorW(IDC_ARROW) 返回非 NULL 伪句柄");
    check(LoadCursorW(nullptr, IDC_ARROW) == arrow, "标准光标伪句柄跨调用稳定");
    check(LoadCursorW(nullptr, IDC_HAND) != arrow, "不同标准光标伪句柄不同");
    check(LoadCursorW(nullptr, L"MyCustomCursor") == nullptr, "自定义光标名诚实加载失败");

    HCURSOR prev = SetCursor(arrow);
    check(prev == nullptr, "SetCursor 返回旧光标（默认无）");
    check(GetCursor() == arrow, "SetCursor 后 GetCursor 反映");
    prev = SetCursor(nullptr);
    check(prev == arrow, "SetCursor 再返回上一只光标");
    check(GetCursor() == nullptr, "SetCursor(NULL) 后无光标");

    POINT p{123, 456};
    check(GetCursorPos(&p) == TRUE, "GetCursorPos 返回 TRUE");
    check(p.x == 0 && p.y == 0, "GetCursorPos 默认 (0,0)");
    check(SetCursorPos(77, 88) == TRUE, "SetCursorPos 返回 TRUE");
    check(GetCursorPos(&p) == TRUE && p.x == 77 && p.y == 88, "SetCursorPos 后 GetCursorPos 反映");

    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    check(GetCursorInfo(&ci) == TRUE, "GetCursorInfo 返回 TRUE");
    check(ci.flags == 0, "GetCursorInfo flags=0（无可见光标）");
    check(ci.hCursor == nullptr, "GetCursorInfo hCursor 与当前一致");
    check(ci.ptScreenPos.x == 77 && ci.ptScreenPos.y == 88, "GetCursorInfo 坐标与 SetCursorPos 一致");
}

void testCapture() {
    std::printf("[捕获]\n");

    check(GetCapture() == nullptr, "GetCapture 默认无捕获");
    check(ReleaseCapture() == FALSE, "无捕获时 ReleaseCapture 返回 FALSE");

    HWND a = reinterpret_cast<HWND>(0x1000);
    HWND b = reinterpret_cast<HWND>(0x2000);
    check(SetCapture(a) == nullptr, "SetCapture(a) 返回旧值（默认无）");
    check(GetCapture() == a, "SetCapture(a) 后 GetCapture==a");
    check(SetCapture(b) == a, "SetCapture(b) 返回旧值 a");
    check(GetCapture() == b, "SetCapture(b) 后 GetCapture==b");
    check(ReleaseCapture() == TRUE, "有捕获时 ReleaseCapture 返回 TRUE");
    check(GetCapture() == nullptr, "ReleaseCapture 后无捕获");
}

void testTimerAndTracking() {
    std::printf("[定时器 / 鼠标跟踪]\n");

    // S11 起这两条不再是桩：登记进真表、能取出、删得掉。派发行为（WM_TIMER
    // 计数）由 CompatTimerDeliverySemantics 在真实消息循环里验证。
    check(SetTimer(nullptr, 1, 16, nullptr) == TRUE, "SetTimer 登记成功");
    check(KillTimer(nullptr, 1) == TRUE, "KillTimer 移除已登记的定时器返回 TRUE");
    check(KillTimer(nullptr, 999) == FALSE, "KillTimer 对未登记的 id 返回 FALSE");
    checkEqual(500u, GetDoubleClickTime(), "GetDoubleClickTime 返回系统默认 500ms");

    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    check(TrackMouseEvent(&tme) == TRUE, "TrackMouseEvent 桩返回 TRUE");
}

void testDrawFrameControl() {
    std::printf("[DrawFrameControl]\n");

    Dib target;
    check(target.create(32, 32), "创建 32x32 位图");
    // 先填成不透明白（模拟对话框背景），否则黑边框画在 alpha=0 的透明背景上，
    // 结果还是全零（applyRop2 保留目标 alpha），"真画了"这个断言就测不出来。
    check(PatBlt(target.dc, 0, 0, 32, 32, WHITENESS) == TRUE, "填充白色背景");

    RECT rc{2, 2, 20, 20};
    check(DrawFrameControl(target.dc, &rc, DFC_BUTTON, DFCS_BUTTONCHECK) == TRUE,
          "画复选框（未勾选）返回 TRUE");
    check(target.peek(2, 2) == 0xFF000000u, "边框画成不透明黑（真画了，非 no-op）");

    RECT rc2{2, 2, 20, 20};
    check(DrawFrameControl(target.dc, &rc2, DFC_BUTTON, DFCS_BUTTONCHECK | DFCS_CHECKED) == TRUE,
          "画复选框（勾选）返回 TRUE");

    check(DrawFrameControl(nullptr, &rc, DFC_BUTTON, DFCS_BUTTONCHECK) == FALSE,
          "空 DC 返回 FALSE");
    check(DrawFrameControl(target.dc, &rc, 999 /* 非按钮族 */, 0) == FALSE,
          "非按钮族诚实返回 FALSE");
}

}  // namespace

int main() {
    std::printf("compat/win32 输入 / 光标 / 捕获 / 定时器语义自测\n\n");
    testKeyboard();
    testCursor();
    testCapture();
    testTimerAndTracking();
    testDrawFrameControl();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
