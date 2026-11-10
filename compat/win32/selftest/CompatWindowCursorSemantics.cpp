// compat/win32 自测 · 窗口类光标与 DefWindowProc(WM_SETCURSOR)
//
// 为什么单独立一个自测：
// ---------------------------------------------------------------------------
// `DefWindowProc(WM_SETCURSOR)` 在真 Windows 上做一件很具体的事：**把窗口类的
// 光标装上去**（EDIT → I 形，STATIC / 按钮 / 注册类 → 各自的 hCursor）。
//
// 本层以前无条件 `return TRUE` 且什么都不装，后果分两半：
//   · 产品侧断言红：`SetCursor(nullptr); SendMessage(WM_SETCURSOR); GetCursor()
//     != nullptr` 这种"编辑框该恢复成 I 形光标"的检查必然失败——遭遇战菜单的
//     cursor smoke 报 `native name edit lost its pointer` 就是这个；
//   · 真机上表现为"点进输入框光标没变"，谁都想不到是默认过程没装光标。
//
// 断言重点在**契约**：哪个类该出哪个光标、非客户区该交回去、以及"先隐藏再
// 让默认过程恢复"这条产品实际用到的路径。

#include <windows.h>

#include <cstdint>
#include <cstdio>

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

void checkCursor(std::uintptr_t expected, const char* description) {
    ++gChecks;
    const auto actual = reinterpret_cast<std::uintptr_t>(GetCursor());
    if (actual != expected) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected id %zu, got %zu)\n", description, expected, actual);
    }
}

LRESULT CALLBACK defProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

constexpr std::uintptr_t kArrow = 32512;
constexpr std::uintptr_t kIBeam = 32513;

void testStockClasses() {
    std::printf("[1] 内建类的类光标\n");

    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE,
                                0, 0, 64, 20, nullptr, nullptr, nullptr, nullptr);
    check(edit != nullptr, "CreateWindowExW(EDIT) succeeds");
    if (edit != nullptr) {
        const LRESULT handled = SendMessageW(edit, WM_SETCURSOR,
                                             reinterpret_cast<WPARAM>(edit),
                                             MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        check(handled == TRUE, "EDIT WM_SETCURSOR is handled");
        checkCursor(kIBeam, "EDIT shows the I-beam class cursor");
        DestroyWindow(edit);
    }

    HWND stat = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                0, 0, 64, 20, nullptr, nullptr, nullptr, nullptr);
    check(stat != nullptr, "CreateWindowExW(STATIC) succeeds");
    if (stat != nullptr) {
        SendMessageW(stat, WM_SETCURSOR, reinterpret_cast<WPARAM>(stat),
                     MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        checkCursor(kArrow, "STATIC shows the arrow class cursor");
        DestroyWindow(stat);
    }
}

void testRegisteredClassCursor() {
    std::printf("[2] 注册类的 hCursor\n");

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = &defProc;
    cls.hCursor = reinterpret_cast<HCURSOR>(kIBeam);
    cls.lpszClassName = L"CompatCursorSelftestClass";
    check(RegisterClassExW(&cls) != 0, "RegisterClassExW with hCursor succeeds");

    HWND window = CreateWindowExW(0, L"CompatCursorSelftestClass", L"", WS_OVERLAPPEDWINDOW,
                                  0, 0, 200, 120, nullptr, nullptr, nullptr, nullptr);
    check(window != nullptr, "CreateWindowExW(registered class) succeeds");
    if (window != nullptr) {
        SendMessageW(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window),
                     MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        checkCursor(kIBeam, "registered class hCursor is applied");
        DestroyWindow(window);
    }
}

void testNonClientDefers() {
    std::printf("[3] 非客户区要把消息交回去\n");

    HWND window = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE,
                                  0, 0, 64, 20, nullptr, nullptr, nullptr, nullptr);
    check(window != nullptr, "EDIT for non-client case created");
    if (window != nullptr) {
        // 真 Windows 的默认过程只在客户区装光标；边框/标题栏要交回给系统，
        // 否则窗口边框上也会显示 I 形光标。
        const LRESULT handled = SendMessageW(window, WM_SETCURSOR,
                                             reinterpret_cast<WPARAM>(window),
                                             MAKELPARAM(HTCAPTION, WM_MOUSEMOVE));
        check(handled == FALSE, "WM_SETCURSOR on non-client area returns FALSE");
        DestroyWindow(window);
    }
}

void testProductSequence() {
    std::printf("[4] 产品实际用到的那条路径\n");

    // 产品的写法（RuntimeSkirmishCursorSmoke）：先把系统光标藏掉（游戏自绘光标），
    // 然后让编辑框通过默认过程把它恢复成 I 形。这条路径错一步，断言就红。
    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE,
                                0, 0, 64, 20, nullptr, nullptr, nullptr, nullptr);
    check(edit != nullptr, "EDIT created for product sequence");
    if (edit == nullptr) return;

    SetCursor(nullptr);
    checkCursor(0, "SetCursor(nullptr) hides the cursor");
    SendMessageW(edit, WM_SETCURSOR, reinterpret_cast<WPARAM>(edit),
                 MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    check(GetCursor() != nullptr, "WM_SETCURSOR restores a non-null cursor after hiding");
    checkCursor(kIBeam, "the restored cursor is the EDIT I-beam");

    DestroyWindow(edit);
}

}  // namespace

int main() {
    std::printf("compat/win32 窗口类光标语义自测\n\n");

    testStockClasses();
    testRegisteredClassCursor();
    testNonClientDefers();
    testProductSequence();

    std::printf("\nWindowCursorSemantics: checks=%d failures=%d\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
