// compat/win32 自测 · 屏幕 DC（GetDC(NULL)）
//
// 为什么单独立一个自测：
// ---------------------------------------------------------------------------
// `HDC screen = GetDC(nullptr);` 是本仓库**出现最多**的 GDI 惯用法
// （src/ 下 71 处），全部性质相同——拿它当 CreateCompatibleDC /
// CreateDIBSection / ensureRuntimeBackBuffer 的**参照设备**，并且用"非空"
// 当成功判据。
//
// 之前它返回 nullptr，于是这 71 处**全部**走进失败分支。症状不是崩溃，而是
// 一句 "... creation failed"：菜单 smoke 报 "offscreen surface creation
// failed"、十几处诊断 smoke 静默降级。这类"返回值是空所以整条路走不通"的
// 缺陷，编译器、链接器、类型检查全都看不见，只有断言能钉住。
//
// 断言的重点是**契约**，不是实现细节：
//   ① 非空 + 单例（同一个指针，两次调用不会各建一个）；
//   ② 尺寸与 GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN) 一致
//      ——两处各写一份常数就会漂移，这里钉的是"同一套值"；
//   ③ 能当参照设备用（CreateCompatibleDC / CreateDIBSection 都成功）；
//   ④ ReleaseDC 之后**仍然有效**（ReleaseDC 是配对语义，不是销毁语义）；
//   ⑤ DeleteDC 被拒绝，且拒绝之后仍然有效（否则单例变野指针）；
//   ⑥ 不计入 GR_GDIOBJECTS（产品侧"对象配平"断言的基线不能被它挤偏一位）；
//   ⑦ 窗口 DC 的行为不变（GetDC(hwnd) 仍返回该窗口的 DC，不是屏幕 DC）。
//
// 构建与运行：
//   c++ -std=c++20 -I compat/win32/include -I compat/win32/src \
//       compat/win32/src/*.cpp compat/win32/selftest/CompatScreenDcSemantics.cpp \
//       -o /tmp/csdc && /tmp/csdc

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

void checkEqual(long expected, long actual, const char* description) {
    ++gChecks;
    if (expected != actual) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected %ld, got %ld)\n", description, expected, actual);
    }
}

LRESULT CALLBACK defProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ① 基线必须在**第一次** GetDC(nullptr) 之前取：这个用例的前提就是它还没被创建。
DWORD gGdiObjectsBeforeScreenDc = 0;

void captureBaseline() {
    gGdiObjectsBeforeScreenDc = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
}

void testIdentityAndSize() {
    std::printf("[1] 非空 / 单例 / 尺寸\n");

    HDC screen = GetDC(nullptr);
    check(screen != nullptr, "GetDC(nullptr) returns non-null");

    check(screen == GetDC(nullptr), "GetDC(nullptr) is a process-wide singleton (same HDC)");
    ReleaseDC(nullptr, screen);

    // 尺寸必须与 GetSystemMetrics 同源。若某天有人只改了其中一处，
    // 产品里"按屏幕尺寸分配缓冲"和"按 DC 尺寸分配"就会得到不同答案。
    checkEqual(GetSystemMetrics(SM_CXSCREEN), GetDeviceCaps(screen, HORZRES),
               "screen DC HORZRES matches GetSystemMetrics(SM_CXSCREEN)");
    checkEqual(GetSystemMetrics(SM_CYSCREEN), GetDeviceCaps(screen, VERTRES),
               "screen DC VERTRES matches GetSystemMetrics(SM_CYSCREEN)");
    check(GetDeviceCaps(screen, HORZRES) > 0 && GetDeviceCaps(screen, VERTRES) > 0,
          "screen DC reports a non-zero surface size");
}

void testUsableAsReference() {
    std::printf("[2] 可作参照设备\n");

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    check(memory != nullptr, "CreateCompatibleDC(screenDc) succeeds");

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 64;
    info.bmiHeader.biHeight = -64;  // top-down，与产品一致
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    check(dib != nullptr, "CreateDIBSection(screenDc, ...) succeeds");
    check(bits != nullptr, "CreateDIBSection(screenDc, ...) exposes writable pixels");

    if (dib != nullptr && memory != nullptr) {
        HGDIOBJ previous = SelectObject(memory, dib);
        check(previous != nullptr && previous != HGDI_ERROR,
              "SelectObject(memoryDc, dib) succeeds (1x1 initial bitmap present)");
        if (previous != nullptr && previous != HGDI_ERROR) SelectObject(memory, previous);
    }

    // 屏幕 DC 自己也能选位图并绘制：真 Windows 上可以，本层同样不该拒绝。
    // 选完必须还原——否则后面 DeleteObject(dib) 时它还挂在屏幕 DC 上，
    // 本层宽容语义会把它留下来，于是对象计数多 1（本条断言最初就是这么红的）。
    HGDIOBJ screenPrevious = SelectObject(screen, dib);
    check(screenPrevious != HGDI_ERROR, "SelectObject(screenDc, dib) does not fail");
    check(PatBlt(screen, 0, 0, 8, 8, WHITENESS) != FALSE, "PatBlt on screen DC succeeds");
    if (screenPrevious != HGDI_ERROR) SelectObject(screen, screenPrevious);

    if (memory != nullptr) DeleteDC(memory);
    if (dib != nullptr) DeleteObject(dib);
    ReleaseDC(nullptr, screen);
}

void testReleaseKeepsItAlive() {
    std::printf("[3] ReleaseDC 是配对语义，不是销毁语义\n");

    HDC screen = GetDC(nullptr);
    checkEqual(1, ReleaseDC(nullptr, screen), "ReleaseDC(nullptr, screenDc) returns success");
    // 释放之后再取，必须还是同一个、且仍然可用。
    check(GetDC(nullptr) == screen, "GetDC(nullptr) after ReleaseDC returns the same HDC");
    check(GetDeviceCaps(screen, HORZRES) > 0,
          "screen DC still usable after ReleaseDC (not destroyed)");
}

void testDeleteRefused() {
    std::printf("[4] DeleteDC 必须被拒绝\n");

    HDC screen = GetDC(nullptr);
    // 真 Windows 上对屏幕 DC 调 DeleteDC 同样是错的。这里必须拒绝，
    // 否则单例被释放后每个 GetDC(nullptr) 都返回悬垂句柄——症状是随机
    // 崩溃或乱画，看不出根因。
    check(DeleteDC(screen) == FALSE, "DeleteDC(screenDc) is refused");
    check(GetDeviceCaps(screen, HORZRES) > 0, "screen DC survives refused DeleteDC");
    check(GetDC(nullptr) == screen, "singleton pointer unchanged after refused DeleteDC");
}

// 注意测量点：必须在**刚刚**创建完屏幕 DC 时取，不能放到所有用例之后。
// 后面的用例会创建/删除自己的 DC 与位图，任何一处漏还原都会把计数带偏，
// 那时再断言就分不清"屏幕 DC 被计数了"还是"别的用例泄漏了"。
void testNotCountedAsGdiObject() {
    std::printf("[5] 不计入 GR_GDIOBJECTS\n");

    // 屏幕 DC 是进程级单例，不是应用拥有的 GDI 对象。若把它算进去，产品侧
    // "创建前后对象数配平"的断言会在首帧整体偏移一位——那种偏差看起来像
    // 泄漏，实际是基线被污染。
    const DWORD now = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    checkEqual(static_cast<long>(gGdiObjectsBeforeScreenDc), static_cast<long>(now),
               "screen DC created lazily but not counted in GR_GDIOBJECTS");
}

void testWindowDcUnaffected() {
    std::printf("[6] 窗口 DC 行为不变\n");

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = &defProc;
    cls.lpszClassName = L"CompatScreenDcSelftest";
    check(RegisterClassExW(&cls) != 0, "RegisterClassExW succeeds");

    HWND window = CreateWindowExW(0, L"CompatScreenDcSelftest", L"screen dc selftest",
                                 WS_OVERLAPPEDWINDOW, 0, 0, 320, 200, nullptr, nullptr,
                                 nullptr, nullptr);
    check(window != nullptr, "CreateWindowExW succeeds");

    HDC windowDc = GetDC(window);
    check(windowDc != nullptr, "GetDC(hwnd) returns non-null");
    check(windowDc != GetDC(nullptr), "GetDC(hwnd) is NOT the screen DC");

    // 野句柄仍然失败——屏幕 DC 的兜底不能把"无效窗口"也变成成功。
    check(GetDC(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0x1))) == nullptr,
          "GetDC(invalid hwnd) still returns null");

    if (window != nullptr) {
        ReleaseDC(window, windowDc);
        DestroyWindow(window);
    }
}

}  // namespace

int main() {
    std::printf("compat/win32 屏幕 DC 语义自测\n\n");

    // 基线必须在任何 GetDC(nullptr) 之前取，否则用例 [5] 永远是 0 差异。
    captureBaseline();

    testIdentityAndSize();
    testNotCountedAsGdiObject();  // 紧跟创建，隔离"是它被计数"还是"别人泄漏"
    testUsableAsReference();
    testReleaseKeepsItAlive();
    testDeleteRefused();
    testWindowDcUnaffected();

    std::printf("\nScreenDcSemantics: checks=%d failures=%d\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
