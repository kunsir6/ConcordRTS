// Q-06 输入投递闭环自测。
//
// 这个测试**不依赖 AppKit 事件源**：它直接调用 compat 侧的 appleInputDispatch()，
// 用真实创建的 CompatWindow 当目标，然后从消息队列里逐条取出来核对。
//
// 为什么要这一层：真实点击无法在无人值守环境里验证（不能截图、不能合成鼠标）。
// 但"InputEvent 翻译成 WM_*"这一段是可以钉死的，而且这段正是最容易写错的地方：
//   · LPARAM 的坐标是两个 16 位字，负数/超界会被静默截断；
//   · WM_MOUSEWHEEL 的 delta 在**高位**字，放低位就永远等于 0；
//   · 各键的 wParam 要用对应的 MK_* 位。
// 这些错了都不会崩，只会让"点上去没反应"——正是最费时间的排查方向。

#include <windows.h>

#include "RuntimeAppleWindowSurface.h"

#include <cstdio>

// 定义在 RuntimeWin32CompatUser.cpp。
// 注意是 **extern "C"**：那个定义落在文件原有的 extern "C" 块内（插在
// EndPaint 附近），所以符号是 C linkage。链接器报的
// "found _appleInputDispatch ... declaration possibly missing extern C"
// 就是这个原因——不加会被当成 C++ 名字修饰，永远找不到。
extern "C" void appleInputDispatch(const concord_compat::apple::InputEvent& event, void* context);

namespace {

int gChecks = 0;
int gFailures = 0;

void check(bool ok, const char* what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("FAIL %s\n", what);
    }
}

LRESULT CALLBACK testProc(HWND, UINT, WPARAM, LPARAM) { return 0; }

// 取出下一条消息；队列空则返回 false。
bool takeMessage(MSG& message) {
    message = MSG{};
    return PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE;
}

void send(const concord_compat::apple::InputEvent& event, HWND target) {
    appleInputDispatch(event, target);
}

concord_compat::apple::InputEvent makeEvent(concord_compat::apple::InputEvent::Kind kind,
                                            int x = 0, int y = 0,
                                            int keyCode = 0, int wheelDelta = 0) {
    concord_compat::apple::InputEvent event;
    event.kind = kind;
    event.x = x;
    event.y = y;
    event.keyCode = keyCode;
    event.wheelDelta = wheelDelta;
    return event;
}

}  // namespace

int main() {
    using Kind = concord_compat::apple::InputEvent::Kind;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = testProc;
    windowClass.lpszClassName = L"ConcordInputSemantics";
    check(RegisterClassExW(&windowClass) != 0, "RegisterClassExW");

    HWND window = CreateWindowExW(0, L"ConcordInputSemantics", L"input", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1024, 640, nullptr, nullptr, nullptr, nullptr);
    check(window != nullptr, "CreateWindowExW");
    if (window == nullptr) {
        std::printf("InputSemantics: checks=%d failures=%d\n", gChecks, gFailures);
        return 1;
    }

    // 1. 鼠标移动：消息类型与坐标编码
    send(makeEvent(Kind::MouseMove, 100, 200), window);
    {
        MSG message{};
        check(takeMessage(message), "MouseMove produces a queued message");
        check(message.message == WM_MOUSEMOVE, "MouseMove -> WM_MOUSEMOVE");
        check(message.hwnd == window, "message carries the target window");
        check(GET_X_LPARAM(message.lParam) == 100 && GET_Y_LPARAM(message.lParam) == 200,
              "MouseMove coordinates are encoded in lParam");
    }

    // 2. 左键按下/抬起：wParam 要用 MK_LBUTTON
    send(makeEvent(Kind::LeftDown, 10, 20), window);
    {
        MSG message{};
        check(takeMessage(message) && message.message == WM_LBUTTONDOWN, "LeftDown -> WM_LBUTTONDOWN");
        check((message.wParam & MK_LBUTTON) != 0, "LeftDown sets MK_LBUTTON");
        check(GET_X_LPARAM(message.lParam) == 10 && GET_Y_LPARAM(message.lParam) == 20,
              "LeftDown coordinates");
    }
    send(makeEvent(Kind::LeftUp, 11, 21), window);
    {
        MSG message{};
        check(takeMessage(message) && message.message == WM_LBUTTONUP, "LeftUp -> WM_LBUTTONUP");
        check(message.wParam == 0, "LeftUp clears the button state");
    }

    // 3. 右键与中键：各自的 MK_* 位
    send(makeEvent(Kind::RightDown, 5, 6), window);
    {
        MSG message{};
        check(takeMessage(message) && message.message == WM_RBUTTONDOWN, "RightDown -> WM_RBUTTONDOWN");
        check((message.wParam & MK_RBUTTON) != 0, "RightDown sets MK_RBUTTON");
    }
    send(makeEvent(Kind::MiddleDown, 7, 8), window);
    {
        MSG message{};
        check(takeMessage(message) && message.message == WM_MBUTTONDOWN, "MiddleDown -> WM_MBUTTONDOWN");
        check((message.wParam & MK_MBUTTON) != 0, "MiddleDown sets MK_MBUTTON");
    }

    // 4. 滚轮：delta 必须落在**高位**字（放低位就永远是 0，滚轮静默失效）
    send(makeEvent(Kind::Wheel, 300, 400, 0, 120), window);
    {
        MSG message{};
        check(takeMessage(message) && message.message == WM_MOUSEWHEEL, "Wheel -> WM_MOUSEWHEEL");
        const int delta = static_cast<short>((message.wParam >> 16) & 0xFFFFu);
        check(delta == 120, "Wheel delta is in the high word (WHEEL_DELTA=120)");
    }
    send(makeEvent(Kind::Wheel, 0, 0, 0, -240), window);
    {
        MSG message{};
        check(takeMessage(message) && message.message == WM_MOUSEWHEEL, "negative wheel -> WM_MOUSEWHEEL");
        const int delta = static_cast<short>((message.wParam >> 16) & 0xFFFFu);
        check(delta == -240, "negative wheel delta survives the 16-bit high word");
    }

    // 5. 键盘：wParam 是虚拟键码
    send(makeEvent(Kind::KeyDown, 0, 0, VK_ESCAPE, 0), window);
    {
        MSG message{};
        check(takeMessage(message) && message.message == WM_KEYDOWN, "KeyDown -> WM_KEYDOWN");
        check(message.wParam == VK_ESCAPE, "KeyDown carries the virtual key code");
    }
    send(makeEvent(Kind::KeyUp, 0, 0, VK_ESCAPE, 0), window);
    {
        MSG message{};
        check(takeMessage(message) && message.message == WM_KEYUP, "KeyUp -> WM_KEYUP");
        check(message.wParam == VK_ESCAPE, "KeyUp carries the virtual key code");
    }

    // 6. 坐标边界：0 与客户区右下角都要能原样往返
    send(makeEvent(Kind::MouseMove, 0, 0), window);
    {
        MSG message{};
        check(takeMessage(message) && GET_X_LPARAM(message.lParam) == 0
                  && GET_Y_LPARAM(message.lParam) == 0, "origin (0,0) round-trips");
    }
    send(makeEvent(Kind::MouseMove, 1023, 639), window);
    {
        MSG message{};
        check(takeMessage(message) && GET_X_LPARAM(message.lParam) == 1023
                  && GET_Y_LPARAM(message.lParam) == 639, "bottom-right corner round-trips");
    }

    // 7. 空 context 必须**什么都不做**而不是崩：sink 还没绑窗口时会出现
    send(makeEvent(Kind::LeftDown, 1, 2), nullptr);
    {
        MSG message{};
        check(!takeMessage(message), "null context drops the event without queueing");
    }

    DestroyWindow(window);
    std::printf("InputSemantics: checks=%d failures=%d\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
