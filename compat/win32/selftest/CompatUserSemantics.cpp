// compat/win32 自测 · 窗口 / 消息 / 钩子（S7，纯离屏）
//
// 这一族断言的是"逻辑窗口对象模型"的语义闭环，而不是真开窗口：
//   · 注册类 → 建窗口 → IsWindow 成立，WM_NCCREATE 把 lpCreateParams 存进
//     GWLP_USERDATA（窗口过程据此找到自己的对象）；
//   · 消息队列闭环：PostMessage → GetMessage → DispatchMessage → 窗口过程收到；
//   · WH_CBT 钩子在 HCBT_CREATEWND 时能读到正在创建窗口的类名；
//   · GetDC/BeginPaint 返回选入客户区位图的 DC，真画能落到像素；
//   · DestroyWindow 后 IsWindow 失效，句柄表计数归零（无泄漏）。
//
// 构建与运行：
//   c++ -std=c++20 -I compat/win32/include \
//       compat/win32/src/RuntimeWin32CompatUser.cpp \
//       compat/win32/src/RuntimeWin32CompatGdi.cpp \
//       compat/win32/src/RuntimeWin32CompatProcess.cpp \
//       compat/win32/selftest/CompatUserSemantics.cpp -o /tmp/ccu && /tmp/ccu

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

#include "RuntimeWin32CompatInternal.h"

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

// 测试窗口的共享状态：窗口过程把收到的消息序号记录在这里。
struct TestWndData {
    int userTag = 0;
    bool gotCreate = false;
    bool gotPaint = false;
    bool gotCustom = false;
    bool gotClose = false;
};

TestWndData gData;

LRESULT CALLBACK testWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        TestWndData* data = static_cast<TestWndData*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        return TRUE;
    }
    TestWndData* data = reinterpret_cast<TestWndData*>(
        GetWindowLongPtrW(window, GWLP_USERDATA)
    );
    if (data == nullptr) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE: data->gotCreate = true; return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        if (dc != nullptr) {
            // 真画：把客户区左上角一个像素涂成白色（证明 DC 真的连着位图）。
            PatBlt(dc, 0, 0, 1, 1, WHITENESS);
            EndPaint(window, &ps);
        }
        data->gotPaint = true;
        return 0;
    }
    case WM_APP: data->gotCustom = true; return 0;
    case WM_CLOSE:
        data->gotClose = true;
        DestroyWindow(window);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

// WH_CBT 钩子回调：观察 HCBT_CREATEWND 的类名。
int gCbtCreateSeen = 0;
wchar_t gCbtClassName[96]{};

LRESULT CALLBACK testCbtHook(int code, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    if (code == HCBT_CREATEWND) {
        ++gCbtCreateSeen;
        GetClassNameW(reinterpret_cast<HWND>(wParam), gCbtClassName, 96);
    }
    return 0;  // 不阻止
}

void testWindowLifecycle() {
    std::printf("== 窗口生命周期 ==\n");

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = testWndProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    cls.lpszClassName = L"CompatSelfTest.Window";
    check(RegisterClassExW(&cls) != 0, "RegisterClassExW 首次注册返回非零 ATOM");

    // 重复注册返回 0 且错误码是 ERROR_CLASS_ALREADY_EXISTS。
    check(RegisterClassExW(&cls) == 0, "重复注册返回 0");
    checkEqual(static_cast<std::uint32_t>(ERROR_CLASS_ALREADY_EXISTS),
               static_cast<std::uint32_t>(GetLastError()),
               "重复注册后 GetLastError == ERROR_CLASS_ALREADY_EXISTS");

    const std::size_t before = concord_compat::liveHandleCount();

    gData = TestWndData{};
    gData.userTag = 42;

    HWND hwnd = CreateWindowExW(0, L"CompatSelfTest.Window", L"自测窗口",
                                WS_OVERLAPPEDWINDOW, 10, 20, 640, 480,
                                nullptr, nullptr, GetModuleHandleW(nullptr), &gData);
    check(hwnd != nullptr, "CreateWindowExW 返回非空句柄");
    check(IsWindow(hwnd) != FALSE, "IsWindow 成立");
    checkEqual(before + 1, concord_compat::liveHandleCount(), "创建窗口后句柄计数 +1");

    // WM_NCCREATE 已把 lpCreateParams 存进 GWLP_USERDATA。
    checkEqual(42, static_cast<std::uint32_t>(
        reinterpret_cast<TestWndData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))->userTag),
        "WM_NCCREATE 存进 GWLP_USERDATA 的 userTag == 42");
    check(gData.gotCreate, "WM_CREATE 已派发");

    // 类名 / 标题
    wchar_t name[96]{};
    GetClassNameW(hwnd, name, 96);
    check(std::wcscmp(name, L"CompatSelfTest.Window") == 0, "GetClassNameW 返回类名");

    // 几何：无边框，客户区 == 窗口矩形。
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    checkEqual(10, static_cast<std::uint32_t>(wr.left), "GetWindowRect.left == 10");
    checkEqual(640, static_cast<std::uint32_t>(wr.right - wr.left), "GetWindowRect 宽 == 640");
    RECT cr{};
    GetClientRect(hwnd, &cr);
    checkEqual(640, static_cast<std::uint32_t>(cr.right), "GetClientRect.right == 640");
    checkEqual(480, static_cast<std::uint32_t>(cr.bottom), "GetClientRect.bottom == 480");

    // 可见性
    check(!IsWindowVisible(hwnd), "创建后初始不可见");
    ShowWindow(hwnd, SW_SHOW);
    check(IsWindowVisible(hwnd) != FALSE, "ShowWindow(SW_SHOW) 后可见");

    // DC 关联：GetDC/BeginPaint 返回非空，且真画能落到位图。
    HDC dc = GetDC(hwnd);
    check(dc != nullptr, "GetDC 返回非空 DC");
    if (dc != nullptr) {
        ReleaseDC(hwnd, dc);
    }

    // 消息队列闭环：PostMessage → GetMessage → DispatchMessage。
    check(!gData.gotCustom, "初始未收到 WM_APP");
    PostMessageW(hwnd, WM_APP, 0, 0);
    MSG msg{};
    check(GetMessageW(&msg, nullptr, 0, 0) == 1, "GetMessageW 取到消息返回 1");
    checkEqual(WM_APP, static_cast<std::uint32_t>(msg.message), "取到的消息是 WM_APP");
    DispatchMessageW(&msg);
    check(gData.gotCustom, "DispatchMessageW 派发到窗口过程（gotCustom）");

    // 同步发送：SendMessageW 立即调窗口过程。
    check(!gData.gotPaint, "初始未收到 WM_PAINT");
    SendMessageW(hwnd, WM_PAINT, 0, 0);
    check(gData.gotPaint, "SendMessageW(WM_PAINT) 同步触发窗口过程");

    // 销毁：PostMessage(WM_CLOSE) → GetMessage → Dispatch → DestroyWindow。
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    check(GetMessageW(&msg, nullptr, 0, 0) == 1, "GetMessageW 取到 WM_CLOSE");
    DispatchMessageW(&msg);
    check(gData.gotClose, "WM_CLOSE 已派发（gotClose）");
    check(!IsWindow(hwnd), "DestroyWindow 后 IsWindow 失效");
    checkEqual(static_cast<std::uint32_t>(before), concord_compat::liveHandleCount(),
               "销毁窗口后句柄计数归零（无泄漏）");
}

void testCbtHook() {
    std::printf("== WH_CBT 钩子 ==\n");

    // 本线程 WH_CBT 钩子。
    HHOOK hook = SetWindowsHookExW(WH_CBT, testCbtHook, nullptr, 0);
    check(hook != nullptr, "SetWindowsHookExW(WH_CBT) 返回非空钩子");

    // 不支持的钩子类型诚实失败。
    HHOOK bad = SetWindowsHookExW(WH_KEYBOARD, testCbtHook, nullptr, 0);
    check(bad == nullptr, "非 WH_CBT 钩子诚实返回 NULL");

    // 注册一个类并创建窗口，钩子应观察到 HCBT_CREATEWND 且类名可读。
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"CompatSelfTest.HookTarget";
    RegisterClassExW(&cls);

    gCbtCreateSeen = 0;
    gCbtClassName[0] = L'\0';
    HWND hwnd = CreateWindowExW(0, L"CompatSelfTest.HookTarget", L"",
                                WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(hwnd != nullptr, "钩子目标窗口创建成功");
    checkEqual(1, static_cast<std::uint32_t>(gCbtCreateSeen), "钩子观察到一次 HCBT_CREATEWND");
    check(std::wcscmp(gCbtClassName, L"CompatSelfTest.HookTarget") == 0,
          "钩子回调里 GetClassNameW 能读到类名");
    DestroyWindow(hwnd);

    check(UnhookWindowsHookEx(hook) != FALSE, "UnhookWindowsHookEx 成功");
    check(UnhookWindowsHookEx(hook) == FALSE, "重复 Unhook 返回 FALSE");
}

void testSystemInfo() {
    std::printf("== 系统信息 ==\n");

    checkEqual(1024, static_cast<std::uint32_t>(GetSystemMetrics(SM_CXSCREEN)), "SM_CXSCREEN == 1024");
    checkEqual(768, static_cast<std::uint32_t>(GetSystemMetrics(SM_CYSCREEN)), "SM_CYSCREEN == 768");

    RECT work{};
    check(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) != FALSE, "SPI_GETWORKAREA 成功");
    checkEqual(1024, static_cast<std::uint32_t>(work.right), "工作区宽 == 1024");
    checkEqual(768, static_cast<std::uint32_t>(work.bottom), "工作区高 == 768");

    SYSTEM_POWER_STATUS power{};
    check(GetSystemPowerStatus(&power) != FALSE, "GetSystemPowerStatus 成功");
    checkEqual(AC_LINE_ONLINE, static_cast<std::uint32_t>(power.ACLineStatus), "ACLineStatus == 在线");

    // AdjustWindowRect 在纯离屏下原样返回（无边框装饰）。
    RECT r{0, 0, 640, 480};
    check(AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0) != FALSE, "AdjustWindowRectEx 成功");
    checkEqual(640, static_cast<std::uint32_t>(r.right), "AdjustWindowRect 不改变宽度（无边框）");
}

}  // namespace

int main() {
    testWindowLifecycle();
    testCbtHook();
    testSystemInfo();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
