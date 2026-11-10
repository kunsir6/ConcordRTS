// ============================================================================
// ConcordRTS Win32 兼容层 · 头层自测（S1）——第 1 部分：【仅编译】
//
// 这个 TU 只做两件事：
//   ① 用 static_assert 钉死类型宽度与常量取值（错一个字节，像素对拍就全错）；
//   ② 把每一个声明的 API 都实际调用一次，证明签名可被源码接受。
//
// 它**不参与链接**（实现由 S2–S5 提供），因此验收命令是 -fsyntax-only：
//   c++ -std=c++20 -I compat/win32/include -fsyntax-only \
//       compat/win32/selftest/CompatHeaderSelftest.cpp
//
// 可运行的部分（几何语义、COLORREF 打包）在 CompatInlineSemantics.cpp。
// ============================================================================

#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>

#include <type_traits>

// ---------------------------------------------------------------------------
// ① 类型宽度（Win64 语义）
// ---------------------------------------------------------------------------
static_assert(sizeof(BOOL) == 4, "BOOL must be 4 bytes");
static_assert(sizeof(BYTE) == 1, "BYTE must be 1 byte");
static_assert(sizeof(WORD) == 2, "WORD must be 2 bytes");
static_assert(sizeof(DWORD) == 4, "DWORD must be 4 bytes");
static_assert(sizeof(UINT) == 4, "UINT must be 4 bytes");
static_assert(sizeof(LONG) == 4, "LONG must be 4 bytes");
static_assert(sizeof(WPARAM) == sizeof(void*), "WPARAM must be pointer-sized");
static_assert(sizeof(LPARAM) == sizeof(void*), "LPARAM must be pointer-sized");
static_assert(sizeof(LRESULT) == sizeof(void*), "LRESULT must be pointer-sized");
static_assert(sizeof(LONG_PTR) == sizeof(void*), "LONG_PTR must be pointer-sized");
static_assert(sizeof(RECT) == 16, "RECT must be 4 x int32");
static_assert(sizeof(POINT) == 8, "POINT must be 2 x int32");
static_assert(sizeof(SIZE) == 8, "SIZE must be 2 x int32");
static_assert(sizeof(BITMAPINFOHEADER) == 40, "BITMAPINFOHEADER must be 40 bytes");
static_assert(sizeof(RGBQUAD) == 4, "RGBQUAD must be 4 bytes");
static_assert(sizeof(COLORREF) == 4, "COLORREF must be 4 bytes");
static_assert(sizeof(TRACKMOUSEEVENT) == 24, "TRACKMOUSEEVENT must be 24 bytes");

// 句柄语义：HGDIOBJ / HANDLE 必须是 void*，否则 SelectObject 收不下 HBITMAP
static_assert(std::is_same_v<HGDIOBJ, void*>, "HGDIOBJ must be void*");
static_assert(std::is_same_v<HANDLE, void*>, "HANDLE must be void*");
static_assert(std::is_same_v<HMODULE, HINSTANCE>, "HMODULE must alias HINSTANCE");
static_assert(std::is_convertible_v<HBITMAP, HGDIOBJ>, "HBITMAP must convert to HGDIOBJ");
static_assert(std::is_convertible_v<HBRUSH, HGDIOBJ>, "HBRUSH must convert to HGDIOBJ");
static_assert(std::is_convertible_v<HPEN, HGDIOBJ>, "HPEN must convert to HGDIOBJ");
static_assert(std::is_convertible_v<HFONT, HGDIOBJ>, "HFONT must convert to HGDIOBJ");
// 句柄必须互不相同，否则把 HBITMAP 误传给需要 HWND 的位置不会被发现
static_assert(!std::is_same_v<HWND, HDC>, "HWND and HDC must be distinct");

// ---------------------------------------------------------------------------
// ② 常量取值（Win32 真值）
// ---------------------------------------------------------------------------
static_assert(WM_KEYDOWN == 0x0100, "WM_KEYDOWN");
static_assert(WM_PAINT == 0x000F, "WM_PAINT");
static_assert(WM_MOUSEWHEEL == 0x020A, "WM_MOUSEWHEEL");
static_assert(WM_MOUSELEAVE == 0x02A3, "WM_MOUSELEAVE");
static_assert(WM_QUIT == 0x0012, "WM_QUIT");
static_assert(WM_PRINTCLIENT == 0x0318, "WM_PRINTCLIENT");
static_assert(WM_NCDESTROY == 0x0082, "WM_NCDESTROY");
static_assert(DT_CENTER == 0x0001, "DT_CENTER");
static_assert(DT_VCENTER == 0x0004, "DT_VCENTER");
static_assert(DT_SINGLELINE == 0x0020, "DT_SINGLELINE");
static_assert(DT_WORDBREAK == 0x0010, "DT_WORDBREAK");
static_assert(DT_CALCRECT == 0x0400, "DT_CALCRECT");
static_assert(DT_END_ELLIPSIS == 0x8000, "DT_END_ELLIPSIS");
static_assert(DT_NOPREFIX == 0x0800, "DT_NOPREFIX");
static_assert(VK_ESCAPE == 0x1B, "VK_ESCAPE");
static_assert(VK_F24 == 0x87, "VK_F24");
static_assert(VK_LMENU == 0xA4, "VK_LMENU");
static_assert(VK_NUMPAD9 == 0x69, "VK_NUMPAD9");
static_assert(SRCCOPY == 0x00CC0020u, "SRCCOPY");
static_assert(DSTINVERT == 0x00550009u, "DSTINVERT");
static_assert(BLACKNESS == 0x00000042u, "BLACKNESS");
static_assert(PS_DOT == 2, "PS_DOT");
static_assert(BI_RGB == 0, "BI_RGB");
static_assert(WS_CHILD == 0x40000000L, "WS_CHILD");
static_assert(WS_POPUP == 0x80000000L, "WS_POPUP");
static_assert(WS_OVERLAPPEDWINDOW == 0x00CF0000L, "WS_OVERLAPPEDWINDOW");
static_assert(CS_DBLCLKS == 0x0008, "CS_DBLCLKS");
static_assert(PM_REMOVE == 0x0001, "PM_REMOVE");
static_assert(GWLP_USERDATA == -21, "GWLP_USERDATA");
static_assert(GWL_STYLE == -16, "GWL_STYLE");
static_assert(GW_CHILD == 5, "GW_CHILD");
static_assert(OFN_FILEMUSTEXIST == 0x00001000, "OFN_FILEMUSTEXIST");
static_assert(MB_ERR_INVALID_CHARS == 0x00000008L, "MB_ERR_INVALID_CHARS");
static_assert(TIMERR_NOERROR == 0, "TIMERR_NOERROR");
static_assert(WHITE_BRUSH == 0 && BLACK_BRUSH == 4, "stock brush ordinals");
static_assert(NULL_BRUSH == 5 && HOLLOW_BRUSH == NULL_BRUSH, "NULL_BRUSH");
static_assert(DEFAULT_GUI_FONT == 17, "DEFAULT_GUI_FONT");
static_assert(COLOR_WINDOW == 5, "COLOR_WINDOW");

// COLORREF 必须是 0x00BBGGRR（不是 RGB 顺序），否则 GetPixel 对拍会全错
static_assert(RGB(0x11, 0x22, 0x33) == 0x00332211u, "RGB packs as 0x00BBGGRR");
static_assert(GetRValue(0x00332211u) == 0x11, "GetRValue");
static_assert(GetGValue(0x00332211u) == 0x22, "GetGValue");
static_assert(GetBValue(0x00332211u) == 0x33, "GetBValue");

// 位运算宏
static_assert(MAKEWORD(0x12, 0x34) == 0x3412, "MAKEWORD");
static_assert(MAKELONG(0x1234, 0x5678) == 0x56781234L, "MAKELONG");
static_assert(LOWORD(0x56781234u) == 0x1234, "LOWORD");
static_assert(HIWORD(0x56781234u) == 0x5678, "HIWORD");
static_assert(GET_WHEEL_DELTA_WPARAM(static_cast<WPARAM>(0x00780000u)) == 0x0078, "GET_WHEEL_DELTA_WPARAM");
static_assert(GET_X_LPARAM(static_cast<LPARAM>(-2)) == -2, "GET_X_LPARAM sign-extends");
static_assert(GET_Y_LPARAM(static_cast<LPARAM>(-2)) == -1, "GET_Y_LPARAM sign-extends");

// ---------------------------------------------------------------------------
// S5b：窗口 / 输入 / 钩子 / 进程资源 族
//
// sizeof(INPUT) 是 ABI 的一部分（SendInput 的 cbSize 直接用它）。Win32 在
// x64 / arm64 上都是 40，算错的唯一症状是"注射的输入被目标窗口当成脏数据"。
// ---------------------------------------------------------------------------
static_assert(sizeof(MOUSEINPUT) == 32, "MOUSEINPUT must be 32 bytes");
static_assert(sizeof(KEYBDINPUT) == 24, "KEYBDINPUT must be 24 bytes");
static_assert(sizeof(HARDWAREINPUT) == 8, "HARDWAREINPUT must be 8 bytes");
static_assert(sizeof(INPUT) == 40, "INPUT must be 40 bytes");
static_assert(sizeof(CURSORINFO) == 24, "CURSORINFO must be 24 bytes");
static_assert(sizeof(SYSTEM_POWER_STATUS) == 12, "SYSTEM_POWER_STATUS must be 12 bytes");
static_assert(alignof(INPUT) == 8, "INPUT must be 8-byte aligned");

// 钩子码与资源计数：这两个值一旦错，HostLifecycle 诊断会"通过但没在观察"
static_assert(WH_CBT == 5, "WH_CBT");
static_assert(HCBT_CREATEWND == 3, "HCBT_CREATEWND");
static_assert(GR_GDIOBJECTS == 0 && GR_USEROBJECTS == 1, "GetGuiResources flags");
static_assert(MWMO_INPUTAVAILABLE == 0x0004, "MWMO_INPUTAVAILABLE");
static_assert(GA_ROOT == 2, "GA_ROOT");
static_assert(SM_REMOTESESSION == 0x1000, "SM_REMOTESESSION");
static_assert(SM_CXVIRTUALSCREEN == 78 && SM_CYVIRTUALSCREEN == 79, "virtual screen metrics");

// 输入标志：MOUSEEVENTF_* / KEYEVENTF_* 与 MOUSEINPUT/KEYBDINPUT 的联合体选择
static_assert(INPUT_MOUSE == 0 && INPUT_KEYBOARD == 1, "INPUT type ordinals");
static_assert(MOUSEEVENTF_VIRTUALDESK == 0x4000 && MOUSEEVENTF_ABSOLUTE == 0x8000,
              "absolute/virtualdesk flags are what make 0..65535 coordinates meaningful");
static_assert(MOUSEEVENTF_WHEEL == 0x0800, "MOUSEEVENTF_WHEEL");
static_assert(KEYEVENTF_SCANCODE == 0x0008 && KEYEVENTF_KEYUP == 0x0002, "KEYEVENTF_*");
static_assert(DPI_AWARENESS_PER_MONITOR_AWARE == 2, "DPI_AWARENESS_PER_MONITOR_AWARE");
static_assert(std::is_same_v<decltype(HWND_MESSAGE), HWND>,
              "HWND_MESSAGE must keep the HWND type, not decay to void* or int");

// ---------------------------------------------------------------------------
// ③ 每个声明的 API 都被实际调用一次（不链接，只验证签名）
// ---------------------------------------------------------------------------
void concordCompatDeclareAll(HWND hwnd, HDC hdc, RECT* rect, MSG* msg) {
    // 窗口与消息
    hwnd = CreateWindowExW(0, L"cls", L"title", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480,
                           nullptr, nullptr, nullptr, nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    RegisterClassExW(&wc);
    WNDCLASSW wcOld{};
    RegisterClassW(&wcOld);
    WNDCLASSEXW info{};
    GetClassInfoExW(nullptr, L"cls", &info);
    DestroyWindow(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    (void)IsWindow(hwnd);
    (void)IsWindowVisible(hwnd);
    (void)IsIconic(hwnd);
    (void)IsZoomed(hwnd);
    hwnd = GetParent(hwnd);
    hwnd = SetParent(hwnd, nullptr);
    hwnd = GetAncestor(hwnd, GW_CHILD);
    hwnd = GetFocus();
    hwnd = SetFocus(hwnd);
    hwnd = GetCapture();
    hwnd = SetCapture(hwnd);
    ReleaseCapture();
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    hwnd = SetActiveWindow(hwnd);
    hwnd = GetActiveWindow();
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    MoveWindow(hwnd, 0, 0, 100, 100, TRUE);
    GetClientRect(hwnd, rect);
    GetWindowRect(hwnd, rect);
    POINT pt{};
    ClientToScreen(hwnd, &pt);
    ScreenToClient(hwnd, &pt);
    AdjustWindowRectEx(rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    InvalidateRect(hwnd, rect, FALSE);
    ValidateRect(hwnd, rect);
    RedrawWindow(hwnd, rect, nullptr, RDW_INVALIDATE | RDW_NOERASE);
    GetUpdateRect(hwnd, rect, FALSE);
    GetWindowTextW(hwnd, nullptr, 0);
    (void)GetWindowTextLengthW(hwnd);
    SetWindowTextW(hwnd, L"x");
    GetClassNameW(hwnd, nullptr, 0);
    (void)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    (void)SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    SetPropW(hwnd, L"p", nullptr);
    GetPropW(hwnd, L"p");
    RemovePropW(hwnd, L"p");
    ShowCursor(TRUE);
    SetCursor(nullptr);
    GetCursorPos(&pt);
    SetCursorPos(0, 0);
    LoadCursorW(nullptr, IDC_ARROW);
    SetTimer(hwnd, 1, 16, nullptr);
    KillTimer(hwnd, 1);
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    TrackMouseEvent(&tme);
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    PostQuitMessage(0);
    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    PeekMessageW(msg, hwnd, 0, 0, PM_REMOVE);
    GetMessageW(msg, hwnd, 0, 0);
    WaitMessage();
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 0, QS_ALLINPUT);
    DispatchMessageW(msg);
    TranslateMessage(msg);
    DefWindowProcW(hwnd, WM_KEYDOWN, 0, 0);
    CallWindowProcW(wc.lpfnWndProc, hwnd, WM_KEYDOWN, 0, 0);
    (void)GetSystemMetrics(SM_CXSCREEN);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, rect, 0);
    MessageBeep(MB_OK);
    MessageBoxW(hwnd, L"a", L"b", MB_OK | MB_ICONERROR | MB_TASKMODAL);

    // DC 与绘制
    hdc = GetDC(hwnd);
    hdc = GetWindowDC(hwnd);
    ReleaseDC(hwnd, hdc);
    HDC mem = CreateCompatibleDC(hdc);
    DeleteDC(mem);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, 16, 16);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 16;
    bmi.bmiHeader.biHeight = -16;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(hdc, &bmi, 0, &bits, nullptr, 0);
    CreateBitmap(16, 16, 1, 32, nullptr);
    HGDIOBJ old = SelectObject(mem, dib);
    DeleteObject(bmp);
    DeleteObject(dib);
    GetStockObject(WHITE_BRUSH);
    HBRUSH brush = CreateSolidBrush(RGB(1, 2, 3));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HFONT font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, 0, 0, L"Arial");
    LOGFONTW lf{};
    CreateFontIndirectW(&lf);
    GetObjectW(brush, sizeof(LOGFONTW), &lf);
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, L"abc", 3, &sz);
    TextOutW(hdc, 0, 0, L"abc", 3);
    ExtTextOutW(hdc, 0, 0, 0, rect, L"abc", 3, nullptr);
    DrawTextW(hdc, L"abc", 3, rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(hdc, RGB(0, 0, 0));
    (void)GetTextColor(hdc);
    SetBkColor(hdc, RGB(0, 0, 0));
    (void)GetBkColor(hdc);
    SetBkMode(hdc, TRANSPARENT);
    (void)GetBkMode(hdc);
    SetTextAlign(hdc, TA_LEFT | TA_TOP);
    (void)GetTextAlign(hdc);
    FillRect(hdc, rect, brush);
    FrameRect(hdc, rect, brush);
    InvertRect(hdc, rect);
    Rectangle(hdc, 0, 0, 10, 10);
    Ellipse(hdc, 0, 0, 10, 10);
    RoundRect(hdc, 0, 0, 10, 10, 3, 3);
    POINT tri[3]{};
    Polygon(hdc, tri, 3);
    Polyline(hdc, tri, 3);
    MoveToEx(hdc, 0, 0, &pt);
    LineTo(hdc, 10, 10);
    SetPixelV(hdc, 0, 0, RGB(0, 0, 0));
    SetPixel(hdc, 0, 0, RGB(0, 0, 0));
    (void)GetPixel(hdc, 0, 0);
    BitBlt(hdc, 0, 0, 16, 16, mem, 0, 0, SRCCOPY);
    StretchBlt(hdc, 0, 0, 32, 32, mem, 0, 0, 16, 16, SRCCOPY);
    SetDIBitsToDevice(hdc, 0, 0, 16, 16, 0, 0, 0, 16, bits, &bmi, 0);
    StretchDIBits(hdc, 0, 0, 16, 16, 0, 0, 16, 16, bits, &bmi, 0, SRCCOPY);
    (void)GetDeviceCaps(hdc, 0);
    (void)MulDiv(1, 2, 3);
    (void)GetSysColor(COLOR_WINDOW);
    GetSysColorBrush(COLOR_WINDOW);
    SelectObject(mem, old);
    DeleteObject(brush);
    DeleteObject(pen);
    DeleteObject(font);
    DeleteDC(mem);

    // 进程 / 模块 / 线程 / 文件 / 编码
    HINSTANCE inst = GetModuleHandleW(nullptr);
    GetModuleFileNameW(inst, nullptr, 0);
    LoadLibraryExW(L"x", nullptr, 0);
    LoadLibraryW(L"x");
    GetProcAddress(inst, "f");
    FreeLibrary(inst);
    (void)GetLastError();
    SetLastError(0);
    HANDLE proc = GetCurrentProcess();
    (void)GetCurrentProcessId();
    TerminateProcess(proc, 0);
    DWORD exitCode = 0;
    GetExitCodeProcess(proc, &exitCode);
    ExitProcess(0);
    SetErrorMode(SEM_FAILCRITICALERRORS);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, L"e");
    SetEvent(ev);
    ResetEvent(ev);
    WaitForSingleObject(ev, INFINITE);
    WaitForMultipleObjects(1, &ev, TRUE, 0);
    Sleep(0);
    (void)GetTickCount();
    (void)GetTickCount64();
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&qpc);
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    volatile long counter = 0;
    InterlockedIncrement(&counter);
    InterlockedDecrement(&counter);
    InterlockedExchange(&counter, 1);
    InterlockedCompareExchange(&counter, 1, 0);
    CloseHandle(ev);
    CloseHandle(CreateFileW(L"f", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    DWORD written = 0;
    WriteFile(nullptr, "x", 1, &written, nullptr);
    ReadFile(nullptr, nullptr, 0, &written, nullptr);
    (void)GetFileAttributesW(L"f");
    (void)MultiByteToWideChar(CP_UTF8, 0, "a", 1, nullptr, 0);
    (void)WideCharToMultiByte(CP_UTF8, 0, L"a", 1, nullptr, 0, nullptr, nullptr);
    (void)lstrlenW(L"a");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CoUninitialize();

    // mmsystem / commdlg
    timeBeginPeriod(1);
    timeEndPeriod(1);
    (void)timeGetTime();
    TIMECAPS tc{};
    timeGetDevCaps(&tc, sizeof(tc));
    PlaySoundW(L"a", nullptr, SND_FILENAME | SND_ASYNC);
    mciSendStringW(L"close all", nullptr, 0, nullptr);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    GetOpenFileNameW(&ofn);
    GetSaveFileNameW(&ofn);
}
