// ============================================================================
// ConcordRTS Win32 兼容层 · 头层自测（S1）——第 2 部分：【可运行】
//
// 只验证不需要外部实现的东西：内联几何工具、宏、常量、结构体布局。
// 不引用任何 S2–S5 尚未实现的符号，因此可以直接链接执行：
//
//   c++ -std=c++20 -I compat/win32/include \
//       compat/win32/selftest/CompatInlineSemantics.cpp -o /tmp/concord_compat_inline
//   /tmp/concord_compat_inline
//
// 判据：打印 "N checks, 0 failures" 且退出码 0。
// ============================================================================

#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL  %s\n", what);
    }
}

// ---------------------------------------------------------------------------
// RECT 族：Win32 语义是【左闭右开】
// ---------------------------------------------------------------------------
void testRectFamily() {
    RECT r{};
    check(SetRect(&r, 10, 20, 30, 40) == TRUE, "SetRect 返回 TRUE");
    check(r.left == 10 && r.top == 20 && r.right == 30 && r.bottom == 40, "SetRect 写入四边");
    check(IsRectEmpty(&r) == FALSE, "非空矩形");

    RECT zero{};
    check(SetRectEmpty(&zero) == TRUE, "SetRectEmpty 返回 TRUE");
    check(IsRectEmpty(&zero) == TRUE, "空矩形判定");
    check(IsRectEmpty(nullptr) == TRUE, "IsRectEmpty(nullptr) 视为空");

    RECT inverted{};
    SetRect(&inverted, 30, 40, 10, 20);
    check(IsRectEmpty(&inverted) == TRUE, "反向矩形视为空");

    // PtInRect：右/下边界不含
    RECT a{};
    SetRect(&a, 0, 0, 100, 100);
    check(PtInRect(&a, POINT{0, 0}) == TRUE, "左上角含");
    check(PtInRect(&a, POINT{99, 99}) == TRUE, "右下一格含");
    check(PtInRect(&a, POINT{100, 100}) == FALSE, "右下边界不含（左闭右开）");
    check(PtInRect(&a, POINT{100, 50}) == FALSE, "右边界不含");
    check(PtInRect(&a, POINT{50, 100}) == FALSE, "下边界不含");
    check(PtInRect(&a, POINT{-1, 50}) == FALSE, "左外侧不含");
    check(PtInRect(nullptr, POINT{0, 0}) == FALSE, "PtInRect(nullptr) 为 FALSE");

    // Copy / Equal
    RECT c{};
    check(CopyRect(&c, &a) == TRUE, "CopyRect 返回 TRUE");
    check(EqualRect(&c, &a) == TRUE, "CopyRect 后相等");
    OffsetRect(&c, 1, 0);
    check(EqualRect(&c, &a) == FALSE, "OffsetRect 后不相等");
    check(CopyRect(nullptr, &a) == FALSE, "CopyRect 空指针返回 FALSE");

    // Inflate / Offset 可逆
    RECT d{};
    CopyRect(&d, &a);
    InflateRect(&d, 5, 7);
    check(d.left == -5 && d.top == -7 && d.right == 105 && d.bottom == 107, "InflateRect 四边外扩");
    OffsetRect(&d, 5, 7);
    check(d.left == 0 && d.top == 0 && d.right == 110 && d.bottom == 114, "OffsetRect 平移");

    // Intersect / Union
    RECT b{}, out{};
    SetRect(&b, 25, 25, 200, 200);
    check(IntersectRect(&out, &a, &b) == TRUE, "有交集返回 TRUE");
    check(out.left == 25 && out.top == 25 && out.right == 100 && out.bottom == 100, "交集四边取严格内界");

    RECT far{};
    SetRect(&far, 500, 500, 600, 600);
    check(IntersectRect(&out, &a, &far) == FALSE, "无交集返回 FALSE");
    check(IsRectEmpty(&out) == TRUE, "无交集时输出为空矩形");

    check(UnionRect(&out, &a, &b) == TRUE, "UnionRect 返回 TRUE");
    check(out.left == 0 && out.top == 0 && out.right == 200 && out.bottom == 200, "并集四边取外扩");

    RECT empty{}, copyOfB{};
    CopyRect(&copyOfB, &b);
    check(UnionRect(&out, &empty, &copyOfB) == TRUE, "空 ∪ 非空 = 非空");
    check(EqualRect(&out, &copyOfB) == TRUE, "空 ∪ 非空 结果正确");

    // SubtractRect：仅处理整条切边，其余返回 FALSE
    RECT leftBand{};
    SetRect(&leftBand, 0, 0, 25, 100);
    check(SubtractRect(&out, &a, &leftBand) == TRUE, "减去左侧整条");
    check(out.left == 25 && out.right == 100, "SubtractRect 左移");

    RECT middle{};
    SetRect(&middle, 40, 40, 60, 60);
    check(SubtractRect(&out, &a, &middle) == FALSE, "中间挖洞不支持，返回 FALSE");
}

// ---------------------------------------------------------------------------
// 宏：COLORREF 打包顺序、位解包、符号扩展
// ---------------------------------------------------------------------------
void testMacros() {
    const COLORREF c = RGB(0x11, 0x22, 0x33);
    check(c == 0x00332211u, "RGB 打包为 0x00BBGGRR");
    check(GetRValue(c) == 0x11, "GetRValue");
    check(GetGValue(c) == 0x22, "GetGValue");
    check(GetBValue(c) == 0x33, "GetBValue");

    check(MAKEWORD(0x12, 0x34) == 0x3412, "MAKEWORD");
    check(MAKELONG(0x1234, 0x5678) == 0x56781234L, "MAKELONG");
    check(LOWORD(0x56781234u) == 0x1234, "LOWORD");
    check(HIWORD(0x56781234u) == 0x5678, "HIWORD");
    check(LOBYTE(0x1234u) == 0x34, "LOBYTE");
    check(HIBYTE(0x1234u) == 0x12, "HIBYTE");

    // LPARAM 里的坐标必须是【符号扩展】的 short，负数不能变成大正数
    const LPARAM lp = MAKELPARAM(-5, -7);
    check(GET_X_LPARAM(lp) == -5, "GET_X_LPARAM 负数保持");
    check(GET_Y_LPARAM(lp) == -7, "GET_Y_LPARAM 负数保持");
    const LPARAM lpPos = MAKELPARAM(300, 400);
    check(GET_X_LPARAM(lpPos) == 300, "GET_X_LPARAM 正数");
    check(GET_Y_LPARAM(lpPos) == 400, "GET_Y_LPARAM 正数");

    // 滚轮增量：高字是有符号 short
    check(GET_WHEEL_DELTA_WPARAM(static_cast<WPARAM>(0x00780000u)) == 120, "滚轮 +120");
    check(GET_WHEEL_DELTA_WPARAM(static_cast<WPARAM>(0xFF880000u)) == -120, "滚轮 -120");

    check(SUCCEEDED(S_OK) && !FAILED(S_OK), "S_OK 判定");
    check(FAILED(E_FAIL) && !SUCCEEDED(E_FAIL), "E_FAIL 判定");
}

// ---------------------------------------------------------------------------
// 结构体：字段可写、布局自洽（Win32 代码依赖这些字段名）
// ---------------------------------------------------------------------------
void testStructures() {
    MSG msg{};
    msg.hwnd = nullptr;
    msg.message = WM_MOUSEWHEEL;
    msg.wParam = MAKEWPARAM(0, 120);
    msg.lParam = MAKELPARAM(10, 20);
    msg.time = 0;
    msg.pt = POINT{10, 20};
    check(msg.message == WM_MOUSEWHEEL, "MSG.message");
    check(GET_WHEEL_DELTA_WPARAM(msg.wParam) == 120, "MSG.wParam 解包");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"ConcordSelfTest";
    check(wc.cbSize == sizeof(WNDCLASSEXW), "WNDCLASSEXW.cbSize");

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 320;
    bmi.bmiHeader.biHeight = -200;   // 负数 = 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    check(bmi.bmiHeader.biHeight == -200, "负高度表示自上而下 DIB");
    check(bmi.bmiHeader.biSize == 40, "biSize 为 40");

    PAINTSTRUCT ps{};
    SetRect(&ps.rcPaint, 1, 2, 3, 4);
    check(ps.rcPaint.right == 3, "PAINTSTRUCT.rcPaint");

    LOGFONTW lf{};
    lf.lfHeight = -12;
    lf.lfWeight = FW_NORMAL;
    std::wcscpy(lf.lfFaceName, L"Arial");
    check(lf.lfHeight == -12, "LOGFONTW.lfHeight");

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    check(ofn.lStructSize == sizeof(OPENFILENAMEW), "OPENFILENAMEW.lStructSize");

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 22050;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = static_cast<WORD>(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    check(wf.nBlockAlign == 4, "WAVEFORMATEX.nBlockAlign");
    check(wf.nAvgBytesPerSec == 88200, "WAVEFORMATEX.nAvgBytesPerSec");

    TIMECAPS tc{};
    check(tc.wPeriodMin == 0 && tc.wPeriodMax == 0, "TIMECAPS 零初始化");
}

// ---------------------------------------------------------------------------
// 哨兵与常量一致性
// ---------------------------------------------------------------------------
void testSentinels() {
    check(TRUE == 1 && FALSE == 0, "TRUE/FALSE");
    check(INVALID_HANDLE_VALUE == reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1)),
          "INVALID_HANDLE_VALUE");
    check(INVALID_FILE_ATTRIBUTES == 0xFFFFFFFFu, "INVALID_FILE_ATTRIBUTES");
    check(CLR_INVALID == 0xFFFFFFFFu, "CLR_INVALID");
    check(INFINITE == 0xFFFFFFFFu, "INFINITE");
    check(MAX_PATH == 260, "MAX_PATH");
    check(IsRectEmpty(nullptr) == TRUE, "空指针安全");
}

} // namespace

int main() {
    testRectFamily();
    testMacros();
    testStructures();
    testSentinels();

    std::printf("compat header selftest (inline): %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
