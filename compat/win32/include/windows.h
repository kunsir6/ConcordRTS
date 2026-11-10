#pragma once
// ============================================================================
// ConcordRTS Win32 兼容层 · 头层（S1）
//
// 用途：非 Windows 平台用本文件替代 <windows.h>，只提供【类型 / 常量 / 声明】，
//       不提供任何实现。实现分落在 S2（对象+位图）、S3（矢量）、S4（文本）、
//       S5（窗口+消息）。
//
// 铁律：
//   1. 本文件只在非 Windows 生效（见下方 #error）。Windows 构建图不得包含本目录。
//   2. 本文件不修改任何既有源码；通过 include 路径优先级被自动选中。
//   3. 声明签名以 Win64 语义为准（WPARAM=uintptr_t / LPARAM=intptr_t / LRESULT=intptr_t）。
//      wchar_t 在 macOS 是 4 字节，因此 LPCWSTR 按 wchar_t* 定义——本层内部自洽，
//      但与 Windows 二进制不兼容，跨边界数据必须显式转 UTF-8。
//
// 未覆盖子系统（不在本层声明，相关 TU 在 macOS 上应退出构建图）：
//   Schannel（AcquireCredentialsHandleW / InitializeSecurityContextW / ...）
//   BCrypt  （BCryptOpenAlgorithmProvider / BCryptHash / ...）
//   XAudio2  （XAudio2Create）
//   DbgHelp  （SymInitialize / SymFromAddr / ...）
//   Job 对象（CreateJobObjectW / SetInformationJobObject）
//   WMI / COM 激活（CoCreateInstance）
// 详见 S1-兼容层头层说明.md 的「冲突与排除清单」。
// ============================================================================

#if defined(_WIN32)
#error "compat/win32/include/windows.h is the non-Windows shim; it must never be used on Windows builds."
#endif

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
// <climits> 必须在这里就引入：文件末尾会按 Win32 语义覆写 LONG_MIN/LONG_MAX，
// 先引入可让后续任何 #include <climits> 变成空操作，避免宏重定义。
#include <climits>

// ---------------------------------------------------------------------------
// 调用约定与修饰符：x86-64 / arm64 上无实际意义，保留以满足源码书写习惯
// ---------------------------------------------------------------------------
#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#endif
#ifndef WINBASEAPI
#define WINBASEAPI
#endif
#ifndef WINGDIAPI
#define WINGDIAPI
#endif
#ifndef WINUSERAPI
#define WINUSERAPI
#endif
#ifndef CONST
#define CONST const
#endif
#ifndef VOID
#define VOID void
#endif
#ifndef FAR
#define FAR
#endif
#ifndef NEAR
#define NEAR
#endif
#ifndef __declspec
#define __declspec(x)
#endif

// ---------------------------------------------------------------------------
// 基础标量
// ---------------------------------------------------------------------------
using BOOL = int;
using BOOLEAN = std::uint8_t;
using BYTE = std::uint8_t;
using CHAR = char;
using WCHAR = wchar_t;
using TCHAR = wchar_t;
using SHORT = std::int16_t;
using USHORT = std::uint16_t;
using WORD = std::uint16_t;
using INT = int;
using UINT = unsigned int;
using LONG = std::int32_t;
using ULONG = std::uint32_t;
using DWORD = std::uint32_t;
using LONGLONG = std::int64_t;
using ULONGLONG = std::uint64_t;
using FLOAT = float;
using DOUBLE = double;
using INT8 = std::int8_t;
using INT16 = std::int16_t;
using INT32 = std::int32_t;
using INT64 = std::int64_t;
using UINT8 = std::uint8_t;
using UINT16 = std::uint16_t;
using UINT32 = std::uint32_t;
using UINT64 = std::uint64_t;
using INT_PTR = std::intptr_t;
using UINT_PTR = std::uintptr_t;
using LONG_PTR = std::intptr_t;
using ULONG_PTR = std::uintptr_t;
using DWORD_PTR = std::uintptr_t;
using SIZE_T = std::size_t;
using SSIZE_T = std::ptrdiff_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
using ATOM = std::uint16_t;
using COLORREF = std::uint32_t;
using HRESULT = std::int32_t;
using LSTATUS = std::int32_t;
using MMRESULT = UINT;
using HCOLORSPACE = void*;
using LPVOID = void*;
using LPCVOID = const void*;
using LPBOOL = BOOL*;
using LPBYTE = BYTE*;
using LPWORD = WORD*;
using LPDWORD = DWORD*;
using LPINT = INT*;
using LPUINT = UINT*;
using LPLONG = LONG*;
using LPSTR = char*;
using LPCSTR = const char*;
using LPWSTR = wchar_t*;
using LPCWSTR = const wchar_t*;
using LPTSTR = wchar_t*;
using LPCTSTR = const wchar_t*;

#define TEXT(x) L##x
#define _T(x) L##x
#define __TEXT(x) L##x

// ---------------------------------------------------------------------------
// 句柄
//   忠实于 WinDef.h 的形态：HGDIOBJ 与 HANDLE 同为 void*，故 HBITMAP/HBRUSH/...
//   可以隐式传给 SelectObject/DeleteObject；HMODULE 是 HINSTANCE 的别名。
// ---------------------------------------------------------------------------
#define CONCORD_DECLARE_HANDLE(name) \
    struct name##_handle_tag { int unused; }; \
    using name = name##_handle_tag*

CONCORD_DECLARE_HANDLE(HWND);
CONCORD_DECLARE_HANDLE(HDC);
CONCORD_DECLARE_HANDLE(HBITMAP);
CONCORD_DECLARE_HANDLE(HBRUSH);
CONCORD_DECLARE_HANDLE(HPEN);
CONCORD_DECLARE_HANDLE(HFONT);
CONCORD_DECLARE_HANDLE(HICON);
CONCORD_DECLARE_HANDLE(HCURSOR);
CONCORD_DECLARE_HANDLE(HMENU);
CONCORD_DECLARE_HANDLE(HRGN);
CONCORD_DECLARE_HANDLE(HPALETTE);
CONCORD_DECLARE_HANDLE(HINSTANCE);
CONCORD_DECLARE_HANDLE(HKEY);
CONCORD_DECLARE_HANDLE(HMONITOR);
CONCORD_DECLARE_HANDLE(HDROP);
CONCORD_DECLARE_HANDLE(HANDLE_HELPER_UNUSED);

using HMODULE = HINSTANCE;
using HGDIOBJ = void*;
using HANDLE = void*;
using HLOCAL = HANDLE;
using HGLOBAL = HANDLE;
using HKL = void*;
using HWINSTA = void*;
using HDESK = void*;

// ---------------------------------------------------------------------------
// 结构体
// ---------------------------------------------------------------------------
struct RECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};
using LPRECT = RECT*;
using LPCRECT = const RECT*;

struct POINT {
    LONG x;
    LONG y;
};
using LPPOINT = POINT*;
using LPPOINTS = POINT*;

struct POINTS {
    SHORT x;
    SHORT y;
};

struct SIZE {
    LONG cx;
    LONG cy;
};
using LPSIZE = SIZE*;

struct SIZEL {
    LONG cx;
    LONG cy;
};

struct FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};

struct SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
};

union LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    } u;
    LONGLONG QuadPart;
};

// GUID / CLSID：Win32 上是 16 字节（Data1 为 32 位）。这里必须用固定宽度类型——
// 否则 macOS 的 unsigned long 是 64 位，GUID 会变成 24 字节，聚合初始化的
// {0x557cf406, 0x1a04, 0x11d3, {...}} 也无法与 Windows 对齐。
struct GUID {
    std::uint32_t Data1;
    std::uint16_t Data2;
    std::uint16_t Data3;
    std::uint8_t Data4[8];
};
using CLSID = GUID;
using IID = GUID;
using REFCLSID = const CLSID&;
using REFIID = const IID&;
using LPGUID = GUID*;
using LPCLSID = CLSID*;

static_assert(sizeof(GUID) == 16, "GUID must stay 16 bytes; Data1 is 32-bit even where long is 64-bit");

// GUID 比较：真实 SDK 在 guiddef.h 里给 C++ 提供 operator== 与 IsEqualGUID。
// 只做**逐字节相等**，不引入任何"部分匹配"语义——GUID 没有前缀比较这回事。
inline bool operator==(const GUID& left, const GUID& right) noexcept {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}
inline bool operator!=(const GUID& left, const GUID& right) noexcept { return !(left == right); }
inline bool IsEqualGUID(const GUID& left, const GUID& right) noexcept { return left == right; }
inline bool IsEqualCLSID(const CLSID& left, const CLSID& right) noexcept { return left == right; }
inline bool IsEqualIID(const IID& left, const IID& right) noexcept { return left == right; }

// IID_IUnknown：真实 COM 里是 ole32 的导出符号，本层没有 ole32，故做成
// inline constexpr 值。取值为标准定义，不是编造。
inline constexpr GUID IID_IUnknown{0x00000000u, 0x0000u, 0x0000u,
                                   {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct RGBQUAD {
    BYTE rgbBlue;
    BYTE rgbGreen;
    BYTE rgbRed;
    BYTE rgbReserved;
};

struct RGBTRIPLE {
    BYTE rgbtBlue;
    BYTE rgbtGreen;
    BYTE rgbtRed;
};

struct PALETTEENTRY {
    BYTE peRed;
    BYTE peGreen;
    BYTE peBlue;
    BYTE peFlags;
};

struct BITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
};
using LPBITMAPINFOHEADER = BITMAPINFOHEADER*;

struct BITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
};
using LPBITMAPINFO = BITMAPINFO*;

// .bmp 文件头：Win32 用 #pragma pack(2) 把它压到 14 字节，这里必须一致，
// 否则读写 .bmp 的 smoke 断言会在 bfSize/bfOffBits 的偏移上全部错位。
#pragma pack(push, 2)
struct BITMAPFILEHEADER {
    WORD bfType;
    DWORD bfSize;
    WORD bfReserved1;
    WORD bfReserved2;
    DWORD bfOffBits;
};
#pragma pack(pop)
using LPBITMAPFILEHEADER = BITMAPFILEHEADER*;

struct BITMAP {
    LONG bmType;
    LONG bmWidth;
    LONG bmHeight;
    LONG bmWidthBytes;
    WORD bmPlanes;
    WORD bmBitsPixel;
    LPVOID bmBits;
};

struct DIBSECTION {
    BITMAP dsBm;
    BITMAPINFOHEADER dsBmih;
    DWORD dsBitfields[3];
    HANDLE dshSection;
    DWORD dsOffset;
};
using LPDIBSECTION = DIBSECTION*;

struct PAINTSTRUCT {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
};
using LPPAINTSTRUCT = PAINTSTRUCT*;

struct MSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
};
using LPMSG = MSG*;

struct LOGFONTW {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    WCHAR lfFaceName[32];
};
using LPLOGFONTW = LOGFONTW*;

struct TEXTMETRICW {
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
    WCHAR tmFirstChar;
    WCHAR tmLastChar;
    WCHAR tmDefaultChar;
    WCHAR tmBreakChar;
    BYTE tmItalic;
    BYTE tmUnderlined;
    BYTE tmStruckOut;
    BYTE tmPitchAndFamily;
    BYTE tmCharSet;
};
using LPTEXTMETRICW = TEXTMETRICW*;

struct CREATESTRUCTW {
    LPVOID lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy;
    int cx;
    int y;
    int x;
    LONG style;
    LPCWSTR lpszName;
    LPCWSTR lpszClass;
    DWORD dwExStyle;
};

struct MINMAXINFO {
    POINT ptReserved;
    POINT ptMaxSize;
    POINT ptMaxPosition;
    POINT ptMinTrackSize;
    POINT ptMaxTrackSize;
};

struct WNDCLASSEXW {
    UINT cbSize;
    UINT style;
    LRESULT (*lpfnWndProc)(HWND, UINT, WPARAM, LPARAM);
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
    HICON hIconSm;
};
using LPWNDCLASSEXW = WNDCLASSEXW*;

struct WNDCLASSW {
    UINT style;
    LRESULT (*lpfnWndProc)(HWND, UINT, WPARAM, LPARAM);
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
};

struct TRACKMOUSEEVENT {
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
};
using LPTRACKMOUSEEVENT = TRACKMOUSEEVENT*;

struct SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
};

struct MEMORYSTATUSEX {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    ULONGLONG ullTotalPhys;
    ULONGLONG ullAvailPhys;
    ULONGLONG ullTotalPageFile;
    ULONGLONG ullAvailPageFile;
    ULONGLONG ullTotalVirtual;
    ULONGLONG ullAvailVirtual;
    ULONGLONG ullAvailExtendedVirtual;
};

struct WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[260];
    WCHAR cAlternateFileName[14];
};

struct STARTUPINFOW {
    DWORD cb;
    LPWSTR lpReserved;
    LPWSTR lpDesktop;
    LPWSTR lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
};

struct PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
};

// SetProcessInformation / ProcessPowerThrottling
struct PROCESS_POWER_THROTTLING_STATE {
    DWORD Version;
    DWORD ControlMask;
    DWORD StateMask;
};
using PROCESS_INFORMATION_CLASS = int;
#define ProcessPowerThrottling ((PROCESS_INFORMATION_CLASS)4)

using WNDPROC = LRESULT (*)(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// 真值 / 哨兵
// ---------------------------------------------------------------------------
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0x00000000u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define STILL_ACTIVE 259
#define ERROR_SUCCESS 0L
#define ERROR_CLASS_ALREADY_EXISTS 1410L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define INVALID_HANDLE_VALUE ((HANDLE)(std::intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define CLR_INVALID 0xFFFFFFFFu
// GDI 的 SelectObject 失败哨兵：Win32 定义成 (HGDIOBJ)(LONG_PTR)-1
#define HGDI_ERROR ((HGDIOBJ)(std::intptr_t)-1)
// GDI 对象类型。数值取自 wingdi.h，必须逐个对得上：GetObjectType 的返回值
// 会被产品代码直接和这些常量比较（`GetObjectType(h)==OBJ_ENHMETADC`），
// 数值错了就是"分支永远走不到"，比编译错误难查得多。
#define OBJ_PEN 1
#define OBJ_BRUSH 2
#define OBJ_DC 3
#define OBJ_METADC 4
#define OBJ_PAL 5
#define OBJ_FONT 6
#define OBJ_BITMAP 7
#define OBJ_REGION 8
#define OBJ_METAFILE 9
#define OBJ_MEMDC 10
#define OBJ_EXTPEN 11
#define OBJ_ENHMETADC 12
#define OBJ_ENHMETAFILE 13
#define OBJ_COLORSPACE 14
// 额外的错误码（不只是 GetLastError 用，ProcessPowerThrottling 分支也会读）
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_IO_PENDING 997L
// S8b：等待类 API 的超时码。1460 是 Windows 的真实取值——调用点会拿它做相等比较
// （`if (GetLastError() == ERROR_TIMEOUT)`），取值写错不会报错、只会让重试分支失效。
#define ERROR_TIMEOUT 1460L
// S10：文件 / 编码 / 模块族的错误码。取值必须与 Win32 一致——调用点拿它们做
// **相等比较**（`result.error == ERROR_FILE_EXISTS`、`== ERROR_ACCESS_DENIED`），
// 数值错了不会报错，只会让"文件已存在→换个名字重试"的分支永远走不到。
#define ERROR_FILE_EXISTS 80L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_TOO_MANY_OPEN_FILES 4L
#define ERROR_DISK_FULL 112L
#define ERROR_GEN_FAILURE 31L
#define ERROR_FILENAME_EXCED_RANGE 206L
#define ERROR_NO_UNICODE_TRANSLATION 1113L
#define ERROR_MOD_NOT_FOUND 126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_ENVVAR_NOT_FOUND 203L
// 电源节流（RuntimeProcessPerformanceScope 会用到）
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4

// ---------------------------------------------------------------------------
// 颜色
// ---------------------------------------------------------------------------
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r)) | (((BYTE)(g)) << 8) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(rgb) ((BYTE)(rgb))
#define GetGValue(rgb) ((BYTE)(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) ((BYTE)((rgb) >> 16))
// PALETTERGB / PALETTEINDEX 的高字节是「调色板标志」，取值必须与 Win32 一致：
// 0x02 表示按 RGB 三元组在当前调色板里查，0x01 表示直接给索引。
#define PALETTEINDEX(i) ((COLORREF)(0x01000000 | (DWORD)(WORD)(i)))
#define PALETTERGB(r, g, b) (0x02000000 | RGB(r, g, b))

// ---------------------------------------------------------------------------
// 位运算 / 消息解包（与 Win32 同名同义）
// ---------------------------------------------------------------------------
#define MAKEWORD(a, b) ((WORD)(((BYTE)((a) & 0xff)) | ((WORD)((BYTE)((b) & 0xff))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)((a) & 0xffff)) | ((DWORD)((WORD)((b) & 0xffff))) << 16))
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)MAKELONG(l, h))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)MAKELONG(l, h))
#define MAKELRESULT(l, h) ((LRESULT)(DWORD)MAKELONG(l, h))
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))
#define GET_KEYSTATE_WPARAM(wp) (LOWORD(wp))
#define GET_WHEEL_DELTA_WPARAM_WORD(wp) ((short)HIWORD(wp))
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define UNREFERENCED_PARAMETER(p) ((void)(p))

// ---------------------------------------------------------------------------
// 窗口类样式 CS_
// ---------------------------------------------------------------------------
#define CS_VREDRAW 0x0001
#define CS_HREDRAW 0x0002
#define CS_DBLCLKS 0x0008
#define CS_OWNDC 0x0020
#define CS_CLASSDC 0x0040
#define CS_PARENTDC 0x0080
#define CS_NOCLOSE 0x0200
#define CS_SAVEBITS 0x0800
#define CS_BYTEALIGNCLIENT 0x1000
#define CS_BYTEALIGNWINDOW 0x2000
#define CS_GLOBALCLASS 0x4000

// ---------------------------------------------------------------------------
// 窗口样式 WS_ / WS_EX_
// ---------------------------------------------------------------------------
#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_CHILD 0x40000000L
#define WS_MINIMIZE 0x20000000L
#define WS_VISIBLE 0x10000000L
#define WS_DISABLED 0x08000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_CLIPCHILDREN 0x02000000L
#define WS_MAXIMIZE 0x01000000L
#define WS_CAPTION 0x00C00000L
#define WS_BORDER 0x00800000L
#define WS_DLGFRAME 0x00400000L
#define WS_VSCROLL 0x00200000L
#define WS_HSCROLL 0x00100000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_GROUP 0x00020000L
#define WS_TABSTOP 0x00010000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_POPUPWINDOW (WS_POPUP | WS_BORDER | WS_SYSMENU)
#define WS_EX_DLGMODALFRAME 0x00000001L
#define WS_EX_TOPMOST 0x00000008L
#define WS_EX_ACCEPTFILES 0x00000010L
#define WS_EX_TRANSPARENT 0x00000020L
#define WS_EX_APPWINDOW 0x00040000L
#define WS_EX_NOPARENTNOTIFY 0x00000004L
#define WS_EX_LAYERED 0x00080000L

// ---------------------------------------------------------------------------
// ShowWindow 命令
// ---------------------------------------------------------------------------
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10

// ---------------------------------------------------------------------------
// 消息 WM_
// ---------------------------------------------------------------------------
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUERYENDSESSION 0x0011
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_GETMINMAXINFO 0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCPAINT 0x0085
#define WM_NCACTIVATE 0x0086
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_COMMAND 0x0111
#define WM_TIMER 0x0113
// S8：同步对象与内存映射的访问位。取值与 Windows SDK 一致；写错不会编译失败，
// 只会让"我要求了写权限"变成一句空话（本层不校验访问位，但探针会断言它们存在）。
#ifndef SYNCHRONIZE
#define SYNCHRONIZE 0x00100000L
#endif
#ifndef EVENT_MODIFY_STATE
#define EVENT_MODIFY_STATE 0x0002
#endif
#ifndef FILE_MAP_WRITE
#define FILE_MAP_WRITE 0x0002
#endif
#ifndef FILE_MAP_READ
#define FILE_MAP_READ 0x0004
#endif
#ifndef FILE_MAP_ALL_ACCESS
#define FILE_MAP_ALL_ACCESS 0x000F001FL
#endif
#ifndef CREATE_NO_WINDOW
#define CREATE_NO_WINDOW 0x08000000
#endif
#ifndef JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000
#endif
// S11：SetTimer 的间隔夹紧边界。真实 user32 把 <10ms 抬到最小值（不是失败），
// 超过上限同样夹紧。缺这两个常量时调用方只能用魔数 10，且写错没人发现。
#define USER_TIMER_MINIMUM 0x0000000A
#define USER_TIMER_MAXIMUM 0x7FFFFFFF
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_MOUSEWHEEL 0x020A
#define WM_MOUSEHWHEEL 0x020E
#define WM_MOUSELEAVE 0x02A3
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP 0x020C
#define WM_XBUTTONDBLCLK 0x020D
#define WM_CAPTURECHANGED 0x0215
#define WM_CANCELMODE 0x001F
#define WM_CTLCOLORSTATIC 0x0138
#define WM_CTLCOLOREDIT 0x0133
#define WM_CTLCOLORLISTBOX 0x0134
#define WM_DRAWITEM 0x002B
#define WM_PRINT 0x0317
#define WM_PRINTCLIENT 0x0318
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_APP 0x8000

// ---------------------------------------------------------------------------
// 虚键码 VK_（只列构建图实际引用到的 + 常见邻居）
// ---------------------------------------------------------------------------
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SELECT 0x29
#define VK_PRINT 0x2A
#define VK_EXECUTE 0x2B
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HELP 0x2F
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F24 0x87
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5

// ---------------------------------------------------------------------------
// DrawText 标志 DT_
// ---------------------------------------------------------------------------
#define DT_TOP 0x00000000
#define DT_LEFT 0x00000000
#define DT_CENTER 0x00000001
#define DT_RIGHT 0x00000002
#define DT_VCENTER 0x00000004
#define DT_BOTTOM 0x00000008
#define DT_WORDBREAK 0x00000010
#define DT_SINGLELINE 0x00000020
#define DT_EXPANDTABS 0x00000040
#define DT_TABSTOP 0x00000080
#define DT_NOCLIP 0x00000100
#define DT_CALCRECT 0x00000400
#define DT_NOPREFIX 0x00000800
#define DT_END_ELLIPSIS 0x00008000
#define DT_PATH_ELLIPSIS 0x00004000

// ---------------------------------------------------------------------------
// GDI 常量
// ---------------------------------------------------------------------------
#define BI_RGB 0L
#define BI_RLE8 1L
#define BI_RLE4 2L
#define BI_BITFIELDS 3L
#define DIB_RGB_COLORS 0
#define DIB_PAL_COLORS 1
// 映射模式 / 图形模式 / 布局
#define MM_TEXT 1
#define MM_LOMETRIC 2
#define MM_HIMETRIC 3
#define MM_LOENGLISH 4
#define MM_HIENGLISH 5
#define MM_TWIPS 6
#define MM_ISOTROPIC 7
#define MM_ANISOTROPIC 8
#define GM_COMPATIBLE 1
#define GM_ADVANCED 2
#define LAYOUT_LTR 0
#define LAYOUT_RTL 1
#define LAYOUT_BITMAPORIENTATIONPRESERVED 8
// 光栅操作模式
#define R2_BLACK 1
#define R2_NOTMERGEPEN 2
#define R2_MASKNOTPEN 3
#define R2_NOTCOPYPEN 4
#define R2_MASKPENNOT 5
#define R2_NOT 6
#define R2_XORPEN 7
#define R2_NOTMASKPEN 8
#define R2_MASKPEN 9
#define R2_NOTXORPEN 10
#define R2_NOP 11
#define R2_MERGENOTPEN 12
#define R2_COPYPEN 13
#define R2_MERGEPENNOT 14
#define R2_MERGEPEN 15
#define R2_WHITE 16
// 区域
#define RGN_AND 1
#define RGN_OR 2
#define RGN_XOR 3
#define RGN_DIFF 4
#define RGN_COPY 5
#define NULLREGION 1
#define SIMPLEREGION 2
#define COMPLEXREGION 3
#define RGN_ERROR 0
// GetClipBox 等返回 ERROR(0) 表示失败；用 #ifndef 兜住，避免和项目里同名的
// 日志级别枚举撞车（真 Win32 也允许被提前定义）。
#ifndef ERROR
#define ERROR 0
#endif
// 多边形填充模式
#define ALTERNATE 1
#define WINDING 2
// 弧方向
#define AD_COUNTERCLOCKWISE 1
#define AD_CLOCKWISE 2
#define GDI_ERROR 0xFFFFFFFFu
#define SRCCOPY 0x00CC0020u
#define SRCPAINT 0x00EE0086u
#define SRCAND 0x008800C6u
#define SRCINVERT 0x00660046u
#define NOTSRCCOPY 0x00330008u
#define DSTINVERT 0x00550009u
#define BLACKNESS 0x00000042u
#define WHITENESS 0x00FF0062u
#define MERGECOPY 0x00C000CAu
#define MERGEPAINT 0x00BB0226u
#define PATCOPY 0x00F00021u
#define PATPAINT 0x00FB0A09u
#define PATINVERT 0x005A0049u
// 拉伸模式（SetStretchBltMode）。取值与 Win32 一致，不能与 TRANSPARENT/OPAQUE 混用。
#define BLACKONWHITE 1
#define STRETCH_ANDSCANS BLACKONWHITE
#define WHITEONBLACK 2
#define STRETCH_ORSCANS WHITEONBLACK
#define COLORONCOLOR 3
#define STRETCH_DELETESCANS COLORONCOLOR
#define HALFTONE 4
#define STRETCH_HALFTONE HALFTONE
#define PS_SOLID 0
#define PS_DASH 1
#define PS_DOT 2
#define PS_DASHDOT 3
// S3-3 补：S1 手抄时漏了这一个（PS_NULL 直接从 3 跳到 5，缺口就是它）。
// 值取自 wingdi.h；漏了它不会报错，只会让 PS_DASHDOTDOT 的笔静默退化成实线。
#define PS_DASHDOTDOT 4
#define PS_NULL 5
#define PS_INSIDEFRAME 6
#define TRANSPARENT 1
#define OPAQUE 2
#define TA_LEFT 0
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_TOP 0
#define TA_BOTTOM 8
#define TA_UPDATECP 1
#define DEFAULT_GUI_FONT 17
#define DEFAULT_CHARSET 1
#define ANSI_CHARSET 0
#define FW_NORMAL 400
#define FW_BOLD 700
#define CLIP_DEFAULT_PRECIS 0
#define PROOF_QUALITY 2
#define NONANTIALIASED_QUALITY 3
#define OUT_DEFAULT_PRECIS 0
#define CLEARTYPE_QUALITY 5
#define FIXED_PITCH 1
#define VARIABLE_PITCH 2
#define FF_DONTCARE 0
#define TTFF_DONTCARE 0

// GDI 库存对象与系统颜色（值需与 Win32 一致，语义在 S2 实现）
#define WHITE_BRUSH 0
#define LTGRAY_BRUSH 1
#define GRAY_BRUSH 2
#define DKGRAY_BRUSH 3
#define BLACK_BRUSH 4
#define NULL_BRUSH 5
#define HOLLOW_BRUSH NULL_BRUSH
#define WHITE_PEN 6
#define BLACK_PEN 7
#define NULL_PEN 8
#define DEFAULT_PALETTE 15
// 库存字体编号（与默认调色板同属 GetStockObject 的编号空间，10..17）
#define OEM_FIXED_FONT 10
#define ANSI_FIXED_FONT 11
#define ANSI_VAR_FONT 12
#define SYSTEM_FONT 13
#define DEVICE_DEFAULT_FONT 14
#define SYSTEM_FIXED_FONT 16
#define COLOR_SCROLLBAR 0
#define COLOR_BACKGROUND 1
#define COLOR_ACTIVECAPTION 2
#define COLOR_MENU 4
#define COLOR_WINDOW 5
#define COLOR_WINDOWFRAME 6
#define COLOR_MENUTEXT 7
#define COLOR_WINDOWTEXT 8
#define COLOR_CAPTIONTEXT 9
#define COLOR_3DFACE 15
#define COLOR_3DSHADOW 16
#define COLOR_BTNFACE 15
#define COLOR_BTNTEXT 18
// GetStockObject 里 DC_BRUSH / DC_PEN 是「按 DC 当前颜色取笔刷」的序号
#define DC_BRUSH 18
#define DC_PEN 19

// MessageBox 标志 MB_
#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_ABORTRETRYIGNORE 0x00000002L
#define MB_YESNOCANCEL 0x00000003L
#define MB_YESNO 0x00000004L
#define MB_RETRYCANCEL 0x00000005L
#define MB_ICONHAND 0x00000010L
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONASTERISK 0x00000040L
#define MB_ICONWARNING 0x00000030L
#define MB_ICONERROR 0x00000010L
#define MB_ICONINFORMATION 0x00000040L
#define MB_DEFBUTTON1 0x00000000L
#define MB_TASKMODAL 0x00002000L
#define MB_ERR_INVALID_CHARS 0x00000008L
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7
#define IDABORT 3
#define IDRETRY 4

// PeekMessage / MsgWait
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
#define PM_NOYIELD 0x0002
#define QS_KEY 0x0001
#define QS_MOUSEMOVE 0x0002
#define QS_MOUSEBUTTON 0x0004
#define QS_POSTMESSAGE 0x0008
#define QS_TIMER 0x0010
#define QS_PAINT 0x0020
#define QS_SENDMESSAGE 0x0040
#define QS_HOTKEY 0x0080
#define QS_ALLINPUT (QS_KEY | QS_MOUSEMOVE | QS_MOUSEBUTTON | QS_POSTMESSAGE | QS_TIMER | QS_PAINT | QS_SENDMESSAGE | QS_HOTKEY)

// SetWindowPos 标志 SWP_
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080
#define SWP_NOCOPYBITS 0x0100
#define SWP_NOOWNERZORDER 0x0200
#define SWP_NOSENDCHANGING 0x0400
#define HWND_TOP ((HWND)0)
#define HWND_BOTTOM ((HWND)1)
#define HWND_TOPMOST ((HWND)-1)
#define HWND_NOTOPMOST ((HWND)-2)

// Get/SetWindowLong
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_ID (-12)
#define GWLP_USERDATA (-21)
#define GWLP_WNDPROC (-4)
#define GWLP_HINSTANCE (-6)
#define GWLP_ID (-12)
#define GW_CHILD 5
#define GW_HWNDNEXT 2
#define GW_OWNER 4

// RedrawWindow / InvalidateRect
#define RDW_INVALIDATE 0x0001
#define RDW_INTERNALPAINT 0x0002
#define RDW_ERASE 0x0004
#define RDW_VALIDATE 0x0008
#define RDW_NOINTERNALPAINT 0x0010
#define RDW_NOERASE 0x0020
#define RDW_NOCHILDREN 0x0040
#define RDW_ALLCHILDREN 0x0080
#define RDW_UPDATENOW 0x0100
#define RDW_ERASENOW 0x0200
#define RDW_FRAME 0x0400
#define RDW_NOFRAME 0x0800

// SystemParametersInfo
#define SPI_GETWORKAREA 0x0030
#define SPI_SETSCREENSAVEACTIVE 0x0011
#define SPI_GETSCREENSAVEACTIVE 0x0010
// 字体平滑查询（MapNativeOverlay::begin 逐项读取以判定"参考 DC 的字体环境"）。
// 取值取自 winuser.h 官方定义。
#define SPI_GETFONTSMOOTHING 0x004A
#define SPI_GETFONTSMOOTHINGTYPE 0x200A
#define SPI_GETFONTSMOOTHINGCONTRAST 0x200C
#define SPI_GETFONTSMOOTHINGORIENTATION 0x2012

// 光标 / 图标 / 系统度量
// Win32 里 IDC_* / IDI_* 是 MAKEINTRESOURCE(ordinal) 的指针常量，不是 int——
// 因此 LoadCursorW 的第二个参数可以直接吃 IDC_ARROW。
#define MAKEINTRESOURCEW(i) ((LPWSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCE MAKEINTRESOURCEW
#define RT_CURSOR MAKEINTRESOURCEW(1)
#define RT_BITMAP MAKEINTRESOURCEW(2)
#define RT_ICON MAKEINTRESOURCEW(3)
#define RT_MENU MAKEINTRESOURCEW(4)
#define RT_DIALOG MAKEINTRESOURCEW(5)
#define RT_STRING MAKEINTRESOURCEW(6)
#define RT_FONTDIR MAKEINTRESOURCEW(7)
#define RT_FONT MAKEINTRESOURCEW(8)
#define IDC_ARROW MAKEINTRESOURCEW(32512)
#define IDC_IBEAM MAKEINTRESOURCEW(32513)
#define IDC_WAIT MAKEINTRESOURCEW(32514)
#define IDC_CROSS MAKEINTRESOURCEW(32515)
#define IDC_UPARROW MAKEINTRESOURCEW(32516)
#define IDC_SIZEALL MAKEINTRESOURCEW(32646)
#define IDC_HAND MAKEINTRESOURCEW(32649)
#define IDI_APPLICATION MAKEINTRESOURCEW(32512)
#define IDI_WINLOGO MAKEINTRESOURCEW(32517)
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXVSCROLL 2
#define SM_CYHSCROLL 3
#define SM_CYCAPTION 4
#define SM_CXBORDER 5
#define SM_CYBORDER 6
#define SM_CXFRAME 32
#define SM_CYFRAME 33

// 文件属性 / 访问权限
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define GENERIC_READ 0x80000000L
#define GENERIC_WRITE 0x40000000L
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_COPY_ALLOWED 0x00000002
#define PAGE_READWRITE 0x04
#define PAGE_READONLY 0x02
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define MEMORY_MAPPED_VIEW_ADDRESS void*

// 变换 / 文本
#define CP_ACP 0
#define CP_OEMCP 1
#define CP_UTF8 65001
#define CP_UTF16LE 1200
#define MB_PRECOMPOSED 0x00000001
#define WC_COMPOSITECHECK 0x00000200

// 线程优先级
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define NORMAL_PRIORITY_CLASS 0x00000020
#define HIGH_PRIORITY_CLASS 0x00000080
#define REALTIME_PRIORITY_CLASS 0x00000100

#define SEM_FAILCRITICALERRORS 0x0001
#define SEM_NOGPFAULTERRORBOX 0x0002
#define SEM_NOOPENFILEERRORBOX 0x8000
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define CTRL_C_EVENT 0
#define CTRL_BREAK_EVENT 1
#define CTRL_CLOSE_EVENT 2

#define TME_HOVER 0x00000001
#define TME_LEAVE 0x00000002
#define TME_NONCLIENT 0x00000010
#define HOVER_DEFAULT 0xFFFFFFFFu

#define MAPVK_VK_TO_VSC 0
#define MAPVK_VSC_TO_VK 1
#define MAPVK_VK_TO_CHAR 2
// 窗口创建时的默认坐标哨兵
#define CW_USEDEFAULT ((INT)0x80000000)
// 图像色彩匹配（RuntimeTerrainViewportFill 会查询 ICM 状态）
// 取值必须与 Win32 一致：ICM_QUERY=1、ICM_ON=2、ICM_OFF=3。
// （S1 初版把 OFF/QUERY 写反了，会让 SetICMMode(dc,ICM_QUERY)!=ICM_OFF 变成
//  一个永远碰巧成立的假断言，已按 windwingdi.h 校正。）
#define ICM_QUERY 1
#define ICM_ON 2
#define ICM_OFF 3
#define ICM_DONE_OUTSIDEDC 4
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008

#define SW_PARENTCLOSING 1
#define SW_PARENTOPENING 3
#define HTNOWHERE 0
#define HTCLIENT 1
#define HTCAPTION 2
#define HTTRANSPARENT (-1)

#define COINIT_APARTMENTTHREADED 0x2
#define COINIT_MULTITHREADED 0x0
#define CLSCTX_INPROC_SERVER 0x1
#define S_OK ((HRESULT)0L)
#define S_FALSE ((HRESULT)1L)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define ENUMERATOR_NOT_FOUND ((HRESULT)0x80070000L)

// ---------------------------------------------------------------------------
// 函数声明（实现见 S2–S5；未实现的符号在链接期暴露，不会静默通过）
// ---------------------------------------------------------------------------
extern "C" {

// ---- 窗口与消息 ----
HWND CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle,
                     int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
                     HINSTANCE hInstance, LPVOID lpParam);
ATOM RegisterClassExW(const WNDCLASSEXW* lpWndClass);
ATOM RegisterClassW(const WNDCLASSW* lpWndClass);
BOOL GetClassInfoExW(HINSTANCE hInstance, LPCWSTR lpszClass, WNDCLASSEXW* lpwcx);
BOOL DestroyWindow(HWND hWnd);
BOOL ShowWindow(HWND hWnd, int nCmdShow);
BOOL UpdateWindow(HWND hWnd);
BOOL IsWindow(HWND hWnd);
BOOL IsWindowVisible(HWND hWnd);
BOOL IsIconic(HWND hWnd);
BOOL IsZoomed(HWND hWnd);
HWND GetParent(HWND hWnd);
HWND SetParent(HWND hWndChild, HWND hWndNewParent);
HWND GetAncestor(HWND hwnd, UINT gaFlags);
HWND GetFocus();
HWND SetFocus(HWND hWnd);
HWND GetCapture();
HWND SetCapture(HWND hWnd);
BOOL ReleaseCapture();
BOOL SetForegroundWindow(HWND hWnd);
BOOL BringWindowToTop(HWND hWnd);
HWND GetActiveWindow();
HWND SetActiveWindow(HWND hWnd);
BOOL SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
BOOL MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint);
BOOL GetClientRect(HWND hWnd, LPRECT lpRect);
BOOL GetWindowRect(HWND hWnd, LPRECT lpRect);
BOOL ClientToScreen(HWND hWnd, LPPOINT lpPoint);
BOOL ScreenToClient(HWND hWnd, LPPOINT lpPoint);
BOOL AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle);
BOOL AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu);
BOOL InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase);
BOOL ValidateRect(HWND hWnd, const RECT* lpRect);
BOOL RedrawWindow(HWND hWnd, const RECT* lprcUpdate, HRGN hrgnUpdate, UINT flags);
BOOL GetUpdateRect(HWND hWnd, LPRECT lpRect, BOOL bErase);
int GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount);
int GetWindowTextLengthW(HWND hWnd);
BOOL SetWindowTextW(HWND hWnd, LPCWSTR lpString);
int GetClassNameW(HWND hWnd, LPWSTR lpClassName, int nMaxCount);
LONG_PTR GetWindowLongPtrW(HWND hWnd, int nIndex);
LONG_PTR SetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong);
LONG GetWindowLongW(HWND hWnd, int nIndex);
LONG SetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong);
BOOL SetPropW(HWND hWnd, LPCWSTR lpString, HANDLE hData);
HANDLE GetPropW(HWND hWnd, LPCWSTR lpString);
HANDLE RemovePropW(HWND hWnd, LPCWSTR lpString);
BOOL ShowCursor(BOOL bShow);
HCURSOR SetCursor(HCURSOR hCursor);
BOOL GetCursorPos(LPPOINT lpPoint);
BOOL SetCursorPos(int X, int Y);
HCURSOR LoadCursorW(HINSTANCE hInstance, LPCWSTR lpCursorName);
HCURSOR LoadCursorA(HINSTANCE hInstance, LPCSTR lpCursorName);
HICON LoadIconW(HINSTANCE hInstance, LPCWSTR lpIconName);
BOOL SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, void (*lpTimerFunc)(HWND, UINT, UINT_PTR, DWORD));
BOOL KillTimer(HWND hWnd, UINT_PTR uIDEvent);
BOOL TrackMouseEvent(LPTRACKMOUSEEVENT lpEventTrack);
BOOL PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL PostThreadMessageW(DWORD idThread, UINT Msg, WPARAM wParam, LPARAM lParam);
void PostQuitMessage(int nExitCode);
LRESULT SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
BOOL GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
BOOL WaitMessage();
DWORD MsgWaitForMultipleObjects(DWORD nCount, const HANDLE* pHandles, BOOL fWaitAll, DWORD dwMilliseconds, DWORD dwWakeMask);
LRESULT DispatchMessageW(const MSG* lpMsg);
BOOL TranslateMessage(const MSG* lpMsg);
LRESULT DefWindowProcW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT CallWindowProcW(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL IsDialogMessageW(HWND hDlg, LPMSG lpMsg);
int GetSystemMetrics(int nIndex);
BOOL SystemParametersInfoW(UINT uiAction, UINT uiParam, LPVOID pvParam, UINT fWinIni);
BOOL MessageBeep(UINT uType);
int MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType);
BOOL AttachConsole(DWORD dwProcessId);
BOOL FreeConsole();
BOOL AllocConsole();
DWORD GetCurrentThreadId();
HANDLE GetCurrentThread();

// ---- DC 与绘制 ----
HDC GetDC(HWND hWnd);
HDC GetWindowDC(HWND hWnd);
int ReleaseDC(HWND hWnd, HDC hDC);
HDC CreateCompatibleDC(HDC hdc);
BOOL DeleteDC(HDC hdc);
HBITMAP CreateCompatibleBitmap(HDC hdc, int cx, int cy);
HBITMAP CreateDIBSection(HDC hdc, const BITMAPINFO* pbmi, UINT usage, void** ppvBits, HANDLE hSection, DWORD offset);
HBITMAP CreateBitmap(int nWidth, int nHeight, UINT nPlanes, UINT nBitCount, const void* lpBits);
HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h);
BOOL DeleteObject(HGDIOBJ ho);
HGDIOBJ GetStockObject(int i);
HBRUSH CreateSolidBrush(COLORREF color);
HBRUSH CreateHatchBrush(int iHatch, COLORREF color);
HPEN CreatePen(int iStyle, int cWidth, COLORREF color);
HPEN CreatePenIndirect(const void* /*LOGPEN**/);
HFONT CreateFontW(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight,
                  DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet,
                  DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily,
                  LPCWSTR pszFaceName);
HFONT CreateFontIndirectW(const LOGFONTW* lplf);
int GetObjectW(HGDIOBJ h, int c, LPVOID pv);
BOOL GetTextMetricsW(HDC hdc, TEXTMETRICW* lptm);
BOOL GetTextExtentPoint32W(HDC hdc, LPCWSTR lpString, int c, LPSIZE psizl);
BOOL TextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c);
BOOL ExtTextOutW(HDC hdc, int x, int y, UINT options, const RECT* lprect, LPCWSTR lpString, UINT c, const int* lpDx);
int DrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format);
COLORREF SetTextColor(HDC hdc, COLORREF color);
COLORREF GetTextColor(HDC hdc);
COLORREF SetBkColor(HDC hdc, COLORREF color);
COLORREF GetBkColor(HDC hdc);
int SetBkMode(HDC hdc, int mode);
int GetBkMode(HDC hdc);
UINT SetTextAlign(HDC hdc, UINT align);
UINT GetTextAlign(HDC hdc);
int FillRect(HDC hdc, const RECT* lprc, HBRUSH hbr);
int FrameRect(HDC hdc, const RECT* lprc, HBRUSH hbr);
int InvertRect(HDC hdc, const RECT* lprc);
BOOL Rectangle(HDC hdc, int left, int top, int right, int bottom);
BOOL Ellipse(HDC hdc, int left, int top, int right, int bottom);
BOOL RoundRect(HDC hdc, int left, int top, int right, int bottom, int width, int height);
BOOL Polygon(HDC hdc, const POINT* apt, int cpt);
BOOL Polyline(HDC hdc, const POINT* apt, int cpt);
BOOL MoveToEx(HDC hdc, int x, int y, LPPOINT lppt);
BOOL LineTo(HDC hdc, int x, int y);
COLORREF SetPixelV(HDC hdc, int x, int y, COLORREF color);
COLORREF SetPixel(HDC hdc, int x, int y, COLORREF color);
COLORREF GetPixel(HDC hdc, int x, int y);
BOOL BitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop);
BOOL StretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
                HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop);
BOOL PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop);
int SetStretchBltMode(HDC hdc, int mode);
int GetStretchBltMode(HDC hdc);
int SetDIBitsToDevice(HDC hdc, int xDest, int yDest, DWORD w, DWORD h, int xSrc, int ySrc,
                      UINT StartScan, UINT cLines, const void* lpvBits, const BITMAPINFO* lpbmi, UINT ColorUse);
int StretchDIBits(HDC hdc, int xDest, int yDest, int DestWidth, int DestHeight,
                  int xSrc, int ySrc, int SrcWidth, int SrcHeight,
                  const void* lpBits, const BITMAPINFO* lpbmi, UINT iUsage, DWORD rop);
int GetDeviceCaps(HDC hdc, int index);
// GetDeviceCaps 的 index 取值（wingdi.h 官方）。MapNativeOverlay::begin 用它读参考
// DC 的物理尺寸换算 EMF 图框，值错一档就会让录出的图元文件比例漂移。
#define HORZSIZE 4
#define VERTSIZE 6
#define HORZRES 8
#define VERTRES 10
int MulDiv(int nNumber, int nNumerator, int nDenominator);
int GetClipBox(HDC hdc, LPRECT lprect);
// S3-2：读回当前裁剪区。S1 的声明清单里漏了它（产品构建图里唯一的调用点
// src/ui/surface/win32/MapNativeOverlayWin32.cpp 属被 strip 的 win32 目录，
// 所以在链接缺口里看不到）。P5-01 把那批 TU 放回构建图时它会立刻变成必需项。
int GetClipRgn(HDC hdc, HRGN hrgn);
DWORD GetSysColor(int nIndex);
HBRUSH GetSysColorBrush(int nIndex);
BOOL GdiFlush();
int SaveDC(HDC hdc);
BOOL RestoreDC(HDC hdc, int nSavedDC);
int IntersectClipRect(HDC hdc, int left, int top, int right, int bottom);
int ExcludeClipRect(HDC hdc, int left, int top, int right, int bottom);
int SelectClipRgn(HDC hdc, HRGN hrgn);
COLORREF SetDCBrushColor(HDC hdc, COLORREF color);
COLORREF GetDCBrushColor(HDC hdc);
COLORREF SetDCPenColor(HDC hdc, COLORREF color);
HGDIOBJ GetCurrentObject(HDC hdc, UINT type);
DWORD GetObjectType(HGDIOBJ h);
// 坐标系 / 模式 / 布局
BOOL SetViewportOrgEx(HDC hdc, int x, int y, LPPOINT lppt);
BOOL GetViewportOrgEx(HDC hdc, LPPOINT lppt);
BOOL SetWindowOrgEx(HDC hdc, int x, int y, LPPOINT lppt);
BOOL GetWindowOrgEx(HDC hdc, LPPOINT lppt);
BOOL SetWindowExtEx(HDC hdc, int x, int y, LPSIZE lpsz);
BOOL GetWindowExtEx(HDC hdc, LPSIZE lpsz);
BOOL SetViewportExtEx(HDC hdc, int x, int y, LPSIZE lpsz);
BOOL GetViewportExtEx(HDC hdc, LPSIZE lpsz);
int SetMapMode(HDC hdc, int iMode);
int GetMapMode(HDC hdc);
int SetGraphicsMode(HDC hdc, int iMode);
int GetGraphicsMode(HDC hdc);
DWORD SetLayout(HDC hdc, DWORD dwLayout);
DWORD GetLayout(HDC hdc);
int SetROP2(HDC hdc, int rop2);
int GetROP2(HDC hdc);
int SetArcDirection(HDC hdc, int dir);
int GetArcDirection(HDC hdc);
int SetPolyFillMode(HDC hdc, int mode);
int GetPolyFillMode(HDC hdc);
BOOL PolyPolyline(HDC hdc, const POINT* apt, const DWORD* asz, DWORD csz);
// 区域对象
HRGN CreateRectRgn(int x1, int y1, int x2, int y2);
HRGN CreateRectRgnIndirect(const RECT* lprect);
HRGN CreateEllipticRgn(int x1, int y1, int x2, int y2);
HRGN CreatePolygonRgn(const POINT* pptl, int cPoint, int iMode);
BOOL SetRectRgn(HRGN hrgn, int left, int top, int right, int bottom);
int CombineRgn(HRGN hrgnDst, HRGN hrgnSrc1, HRGN hrgnSrc2, int iMode);
int GetRgnBox(HRGN hrgn, LPRECT lprc);
BOOL RectInRegion(HRGN hrgn, const RECT* lprc);
BOOL PtInRegion(HRGN hrgn, int x, int y);

// ---- 进程 / 模块 / 线程 ----
HINSTANCE GetModuleHandleW(LPCWSTR lpModuleName);
DWORD GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
HMODULE LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
HMODULE LoadLibraryW(LPCWSTR lpLibFileName);
void* GetProcAddress(HMODULE hModule, const char* lpProcName);
BOOL FreeLibrary(HMODULE hLibModule);
DWORD GetLastError();
void SetLastError(DWORD dwErrCode);
HANDLE GetCurrentProcess();
DWORD GetCurrentProcessId();
BOOL TerminateProcess(HANDLE hProcess, UINT uExitCode);
BOOL GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode);
BOOL CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                    SECURITY_ATTRIBUTES* lpProcessAttributes, SECURITY_ATTRIBUTES* lpThreadAttributes,
                    BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
                    LPCWSTR lpCurrentDirectory, STARTUPINFOW* lpStartupInfo,
                    PROCESS_INFORMATION* lpProcessInformation);
void ExitProcess(UINT uExitCode);

// S8：Job 对象。本轮**只声明不实现**（CreateJobObjectW 等由 S7 之后按进程组/信号重新设计：
// macOS 没有内核 Job，"子进程随父进程退出"要靠进程组 + 信号或 kqueue 监视，语义不能糊）。
// 结构体必须按真实布局给全，否则 SetInformationJobObject 的 Size 参数与字段偏移都会错。
struct IO_COUNTERS {
    ULONGLONG ReadOperationCount;
    ULONGLONG WriteOperationCount;
    ULONGLONG OtherOperationCount;
    ULONGLONG ReadTransferCount;
    ULONGLONG WriteTransferCount;
    ULONGLONG OtherTransferCount;
};

struct JOBOBJECT_BASIC_LIMIT_INFORMATION {
    LARGE_INTEGER PerProcessUserTimeLimit;
    LARGE_INTEGER PerJobUserTimeLimit;
    DWORD LimitFlags;
    SIZE_T MinimumWorkingSetSize;
    SIZE_T MaximumWorkingSetSize;
    DWORD ActiveProcessLimit;
    ULONG_PTR Affinity;
    DWORD PriorityClass;
    DWORD SchedulingClass;
};

struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
    JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
    IO_COUNTERS IoInfo;
    SIZE_T ProcessMemoryLimit;
    SIZE_T JobMemoryLimit;
    SIZE_T PeakProcessMemoryUsed;
    SIZE_T PeakJobMemoryUsed;
};

enum JOBOBJECTINFOCLASS {
    JobObjectBasicLimitInformation = 2,
    JobObjectExtendedLimitInformation = 9
};

HANDLE CreateJobObjectW(SECURITY_ATTRIBUTES* lpJobAttributes, LPCWSTR lpName);
BOOL AssignProcessToJobObject(HANDLE hJob, HANDLE hProcess);
BOOL TerminateJobObject(HANDLE hJob, UINT uExitCode);
BOOL SetInformationJobObject(HANDLE hJob, JOBOBJECTINFOCLASS JobObjectInformationClass,
                             LPVOID lpJobObjectInformation, DWORD cbJobObjectInformationLength);

// S8：strncpy_s 的"截断而不是报错"哨兵。真实值是 (size_t)-1。
// 写成 0 会让调用点以为"目标容量为 0"从而走失败分支，而失败分支往往只把 message 设成
// "unknown error"——于是截断变成了一个看不出原因的空错误。
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
UINT SetErrorMode(UINT uMode);
BOOL SetConsoleCtrlHandler(BOOL (*HandlerRoutine)(DWORD), BOOL Add);
HANDLE CreateEventW(SECURITY_ATTRIBUTES* lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCWSTR lpName);
HANDLE OpenEventW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpName);
BOOL SetEvent(HANDLE hEvent);
BOOL ResetEvent(HANDLE hEvent);
DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);
HANDLE CreateThread(SECURITY_ATTRIBUTES* lpThreadAttributes, SIZE_T dwStackSize,
                    DWORD (*lpStartAddress)(LPVOID), LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId);
BOOL SetThreadPriority(HANDLE hThread, int nPriority);
int GetThreadPriority(HANDLE hThread);
void Sleep(DWORD dwMilliseconds);
DWORD GetTickCount();
ULONGLONG GetTickCount64();
BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency);
void GetSystemTimeAsFileTime(FILETIME* lpSystemTimeAsFileTime);
void GetSystemTime(SYSTEMTIME* lpSystemTime);
void GetLocalTime(SYSTEMTIME* lpSystemTime);
BOOL SetProcessInformation(HANDLE hProcess, PROCESS_INFORMATION_CLASS ProcessInformationClass,
                           LPVOID ProcessInformation, DWORD ProcessInformationSize);
BOOL GetProcessInformation(HANDLE hProcess, PROCESS_INFORMATION_CLASS ProcessInformationClass,
                           LPVOID ProcessInformation, DWORD ProcessInformationSize);
BOOL GetSystemInfo(LPVOID /*SYSTEM_INFO**/ lpSystemInfo);
BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* lpBuffer);
// S8：形参必须是 LONG（32 位），不能写 long——本平台的 long 是 64 位，
// 而产品与探针里的字段是 `volatile LONG`（int32_t）。写成 long* 的后果不是"精度问题"，
// 而是**根本匹配不上**（no matching function），或者更糟：调用点用 reinterpret_cast
// 硬转过去，然后按 8 字节读写一个 4 字节字段，踩掉相邻字段。
// 这四条在 Windows 上签名完全等价（LONG == long），所以改动对 Windows 零影响。
LONG InterlockedIncrement(volatile LONG* lpAddend);
LONG InterlockedDecrement(volatile LONG* lpAddend);
LONG InterlockedExchange(volatile LONG* Target, LONG Value);
LONG InterlockedCompareExchange(volatile LONG* Destination, LONG Exchange, LONG Comperand);
void* InterlockedCompareExchangePointer(void* volatile* Destination, void* Exchange, void* Comperand);
void* InterlockedExchangePointer(void* volatile* Target, void* Value);
HANDLE CreateFileMappingW(HANDLE hFile, SECURITY_ATTRIBUTES* lpAttributes, DWORD flProtect,
                          DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCWSTR lpName);
HANDLE OpenFileMappingW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpName);
void* MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
                    DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
BOOL UnmapViewOfFile(const void* lpBaseAddress);
HRESULT CoInitializeEx(LPVOID pvReserved, DWORD dwCoInit);
void CoUninitialize();

// ---- 文件 ----
HANDLE CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   SECURITY_ATTRIBUTES* lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
              LPDWORD lpNumberOfBytesRead, LPVOID lpOverlapped);
BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
               LPDWORD lpNumberOfBytesWritten, LPVOID lpOverlapped);
BOOL CloseHandle(HANDLE hObject);
DWORD GetFileAttributesW(LPCWSTR lpFileName);
BOOL SetFileAttributesW(LPCWSTR lpFileName, DWORD dwFileAttributes);
BOOL DeleteFileW(LPCWSTR lpFileName);
BOOL MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags);
BOOL CreateDirectoryW(LPCWSTR lpPathName, SECURITY_ATTRIBUTES* lpSecurityAttributes);
BOOL RemoveDirectoryW(LPCWSTR lpPathName);
DWORD GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR* lpFilePart);
DWORD GetCurrentDirectoryW(DWORD nBufferLength, LPWSTR lpBuffer);
BOOL SetCurrentDirectoryW(LPCWSTR lpPathName);
DWORD GetTempPathW(DWORD nBufferLength, LPWSTR lpBuffer);
HANDLE FindFirstFileW(LPCWSTR lpFileName, WIN32_FIND_DATAW* lpFindFileData);
BOOL FindNextFileW(HANDLE hFindFile, WIN32_FIND_DATAW* lpFindFileData);
BOOL FindClose(HANDLE hFindFile);

// ---- 字符串 / 编码 ----
int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr, int cbMultiByte,
                        LPWSTR lpWideCharStr, int cchWideChar);
int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr, int cchWideChar,
                        LPSTR lpMultiByteStr, int cbMultiByte, LPCSTR lpDefaultChar, LPBOOL lpUsedDefaultChar);
int lstrlenW(LPCWSTR lpString);
int lstrlenA(LPCSTR lpString);
LPWSTR lstrcpyW(LPWSTR lpString1, LPCWSTR lpString2);
int lstrcmpW(LPCWSTR lpString1, LPCWSTR lpString2);
int lstrcmpiW(LPCWSTR lpString1, LPCWSTR lpString2);

// ---- 内存 ----
LPVOID VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
HANDLE GetProcessHeap();
LPVOID HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
BOOL HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);

// ---- 注册表 ----
LSTATUS RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, DWORD samDesired, HKEY* phkResult);
LSTATUS RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD* lpReserved, DWORD* lpType,
                         BYTE* lpData, DWORD* lpcbData);
LSTATUS RegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved, DWORD dwType,
                       const BYTE* lpData, DWORD cbData);
LSTATUS RegCloseKey(HKEY hKey);

} // extern "C"

// ---------------------------------------------------------------------------
// UNICODE 别名：真 Win32 在定义 UNICODE 时把不带后缀的名字映射到 *W 版本。
// 项目源码两种写法都有（`GetObject` 与 `GetObjectW` 混用），所以这里补上同一套
// 映射，行为与 Windows 一致，不新增语义。
// ---------------------------------------------------------------------------
#define GetObject GetObjectW
#define CreateFont CreateFontW
#define CreateFontIndirect CreateFontIndirectW
#define DrawText DrawTextW
#define TextOut TextOutW
#define ExtTextOut ExtTextOutW
#define GetTextExtentPoint32 GetTextExtentPoint32W
#define GetTextMetrics GetTextMetricsW
#define CreateWindowEx CreateWindowExW
#define RegisterClassEx RegisterClassExW
#define RegisterClass RegisterClassW
#define GetClassInfoEx GetClassInfoExW
#define SetWindowText SetWindowTextW
#define GetWindowText GetWindowTextW
#define GetWindowTextLength GetWindowTextLengthW
#define GetClassName GetClassNameW
#define MessageBox MessageBoxW
#define LoadCursor LoadCursorW
#define LoadIcon LoadIconW
#define SetProp SetPropW
#define GetProp GetPropW
#define RemoveProp RemovePropW
#define SendMessage SendMessageW
#define PostMessage PostMessageW
#define GetMessage GetMessageW
#define PeekMessage PeekMessageW
#define DispatchMessage DispatchMessageW
#define DefWindowProc DefWindowProcW
#define CallWindowProc CallWindowProcW
#define GetWindowLongPtr GetWindowLongPtrW
#define SetWindowLongPtr SetWindowLongPtrW
#define GetWindowLong GetWindowLongW
#define SetWindowLong SetWindowLongW
#define PlaySound PlaySoundW
#define mciSendString mciSendStringW
#define GetOpenFileName GetOpenFileNameW
#define GetSaveFileName GetSaveFileNameW
#define GetModuleHandle GetModuleHandleW
#define GetModuleFileName GetModuleFileNameW
#define LoadLibrary LoadLibraryW
#define LoadLibraryEx LoadLibraryExW
#define CreateFile CreateFileW
#define GetFileAttributes GetFileAttributesW
#define SetFileAttributes SetFileAttributesW
#define DeleteFile DeleteFileW
#define FindFirstFile FindFirstFileW
#define FindNextFile FindNextFileW
#define CreateEvent CreateEventW
#define OpenEvent OpenEventW
#define CreateFileMapping CreateFileMappingW
#define MoveFileEx MoveFileExW
#define CreateDirectory CreateDirectoryW
#define RemoveDirectory RemoveDirectoryW
#define GetFullPathName GetFullPathNameW
#define GetCurrentDirectory GetCurrentDirectoryW
#define SetCurrentDirectory SetCurrentDirectoryW
#define GetTempPath GetTempPathW
#define RegOpenKeyEx RegOpenKeyExW
#define RegQueryValueEx RegQueryValueExW
#define RegSetValueEx RegSetValueExW
#define lstrlen lstrlenW
#define lstrcpy lstrcpyW
#define lstrcmp lstrcmpW
#define lstrcmpi lstrcmpiW

// ---------------------------------------------------------------------------
// MSVC CRT 安全函数
//
// 项目里直接用了 _dupenv_s（MSVC 的 getenv 安全版）。这里按 MSVC 语义实现：
// 变量不存在时返回 0 且 *buffer == nullptr；返回缓冲区用 malloc 分配，调用方
// free() 即可。这样源码一行不用改，也不会把 malloc/free 的配对规则搞混。
// ---------------------------------------------------------------------------
using errno_t = int;

inline errno_t _dupenv_s(char** buffer, std::size_t* numberOfElements, const char* varname) {
    if (buffer == nullptr) return 22;  // EINVAL
    *buffer = nullptr;
    if (numberOfElements != nullptr) *numberOfElements = 0;
    if (varname == nullptr) return 22;
    const char* value = std::getenv(varname);
    if (value == nullptr) return 0;    // 变量不存在不是错误
    const std::size_t length = std::strlen(value) + 1;
    char* copy = static_cast<char*>(std::malloc(length));
    if (copy == nullptr) return 12;    // ENOMEM
    std::memcpy(copy, value, length);
    *buffer = copy;
    if (numberOfElements != nullptr) *numberOfElements = length;
    return 0;
}

// HKEY 根键：只作为符号存在，S2 起按需实现语义
#define HKEY_CLASSES_ROOT ((HKEY)0x80000000)
#define HKEY_CURRENT_USER ((HKEY)0x80000001)
#define HKEY_LOCAL_MACHINE ((HKEY)0x80000002)
#define HKEY_USERS ((HKEY)0x80000003)
#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define REG_SZ 1
#define REG_DWORD 4

// ---------------------------------------------------------------------------
// 简单内联几何工具：Win32 里这些是导出函数，但语义纯数学，直接内联可减少
// S2 的实现面，且行为与 Win32 完全一致（左闭右开）。
// ---------------------------------------------------------------------------
inline BOOL SetRect(RECT* r, int l, int t, int rr, int b) {
    if (!r) return FALSE;
    r->left = l; r->top = t; r->right = rr; r->bottom = b;
    return TRUE;
}
inline BOOL SetRectEmpty(RECT* r) { return SetRect(r, 0, 0, 0, 0); }
inline BOOL CopyRect(RECT* d, const RECT* s) {
    if (!d || !s) return FALSE;
    *d = *s; return TRUE;
}
inline BOOL IsRectEmpty(const RECT* r) {
    return (!r || r->left >= r->right || r->top >= r->bottom) ? TRUE : FALSE;
}
inline BOOL EqualRect(const RECT* a, const RECT* b) {
    if (!a || !b) return FALSE;
    return (a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom) ? TRUE : FALSE;
}
inline BOOL InflateRect(RECT* r, int dx, int dy) {
    if (!r) return FALSE;
    r->left -= dx; r->top -= dy; r->right += dx; r->bottom += dy;
    return TRUE;
}
inline BOOL OffsetRect(RECT* r, int dx, int dy) {
    if (!r) return FALSE;
    r->left += dx; r->top += dy; r->right += dx; r->bottom += dy;
    return TRUE;
}
inline BOOL PtInRect(const RECT* r, POINT p) {
    // Win32 语义：左闭右开
    return (r && p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom) ? TRUE : FALSE;
}
inline BOOL IntersectRect(RECT* d, const RECT* a, const RECT* b) {
    if (!d || !a || !b) return FALSE;
    RECT out;
    out.left = a->left > b->left ? a->left : b->left;
    out.top = a->top > b->top ? a->top : b->top;
    out.right = a->right < b->right ? a->right : b->right;
    out.bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    *d = out;
    return IsRectEmpty(&out) ? FALSE : TRUE;
}
inline BOOL UnionRect(RECT* d, const RECT* a, const RECT* b) {
    if (!d || !a || !b) return FALSE;
    if (IsRectEmpty(a)) { *d = *b; return IsRectEmpty(b) ? FALSE : TRUE; }
    if (IsRectEmpty(b)) { *d = *a; return TRUE; }
    d->left = a->left < b->left ? a->left : b->left;
    d->top = a->top < b->top ? a->top : b->top;
    d->right = a->right > b->right ? a->right : b->right;
    d->bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
    return TRUE;
}
inline BOOL SubtractRect(RECT* d, const RECT* a, const RECT* b) {
    if (!d || !a || !b) return FALSE;
    RECT tmp;
    if (!IntersectRect(&tmp, a, b)) { *d = *a; return TRUE; }
    *d = *a;
    if (tmp.top == a->top && tmp.bottom == a->bottom) {
        if (tmp.left == a->left) d->left = tmp.right;
        else if (tmp.right == a->right) d->right = tmp.left;
        else return FALSE;
    } else if (tmp.left == a->left && tmp.right == a->right) {
        if (tmp.top == a->top) d->top = tmp.bottom;
        else if (tmp.bottom == a->bottom) d->bottom = tmp.top;
        else return FALSE;
    } else {
        return FALSE;
    }
    return TRUE;
}

// ===========================================================================
// S5 第一阶段（2026-11-10）：窗口 / 输入 / 钩子 / EMF 的常量与类型补齐
//
// 清单来源不是猜测，而是 RuntimeShell.cpp 补上 <list> 后 -ferror-limit=0
// 的全量错误取证：69 个 undeclared identifier + 9 个 unknown type name。
// 本段只做【声明】，不含任何实现——未实现的符号会在链接期报 undefined
// symbol，这是 S1 定下的规矩：漏一个就是一条明确的错，不静默降级。
// ===========================================================================

// --- 虚拟键码：F13–F24 与十进制键（键盘路由 smoke 用它们当"不冲突的测试键"）
#define VK_F13 0x7C
#define VK_F14 0x7D
#define VK_F15 0x7E
#define VK_F16 0x7F
#define VK_F17 0x80
#define VK_F18 0x81
#define VK_F19 0x82
#define VK_F20 0x83
#define VK_F21 0x84
#define VK_F22 0x85
#define VK_F23 0x86
#define VK_F24 0x87
#define VK_DECIMAL 0x6E

// --- 扩展窗口样式
#ifndef WS_EX_TOOLWINDOW
#define WS_EX_TOOLWINDOW 0x00000080L
#endif
#ifndef WS_EX_NOACTIVATE
#define WS_EX_NOACTIVATE 0x08000000L
#endif

// --- 输入
#define WHEEL_DELTA 120
#define PRF_CLIENT 0x00000004L
#define SIZE_MINIMIZED 1

// --- 消息
#define WM_MOUSEACTIVATE 0x0021
#define WM_DPICHANGED 0x02E0
#define WM_USER 0x0400
#define PM_QS_SENDMESSAGE 0x0040

// --- 宽字符转换标志
#define WC_ERR_INVALID_CHARS 0x00000080L
// WC_NO_BEST_FIT_CHARS：Windows 用它禁止"最接近字符"映射。本平台没有码页表，
// 也就不存在 best-fit 这一层，所以这个标志在这里是**无操作**——但必须定义：
// 传它的调用点如果拿到未定义标识符就会编译失败，而它想表达的语义已经天然成立。
#define WC_NO_BEST_FIT_CHARS 0x00000400L

// --- 控件绘制（DFC_/DFCS_）
#define DFC_BUTTON 4
#ifndef DFCS_BUTTONCHECK
#define DFCS_BUTTONCHECK 0x0000
#endif
#ifndef DFCS_CHECKED
#define DFCS_CHECKED 0x0004
#endif
#ifndef DFCS_INACTIVE
#define DFCS_INACTIVE 0x0100
#endif

// --- 显示器
#ifndef MONITOR_DEFAULTTONEAREST
#define MONITOR_DEFAULTTONEAREST 2
#endif
#ifndef MONITOR_DEFAULTTOPRIMARY
#define MONITOR_DEFAULTTOPRIMARY 1
#endif
// MONITORINFO.dwFlags 的位（GetMonitorInfoW 回填）。纯离屏只有一块虚拟主屏。
#ifndef MONITORINFOF_PRIMARY
#define MONITORINFOF_PRIMARY 0x00000001
#endif

// --- DXGI（D3D11 presenter 的失败判定；HRESULT 必须保持为负值）
#ifndef DXGI_ERROR_DEVICE_REMOVED
#define DXGI_ERROR_DEVICE_REMOVED ((HRESULT)0x887A0005UL)
#endif

// --- 字体重量的连续取值（S4 文本族会用到整条梯度）
#define FW_DONTCARE 0
#define FW_THIN 100
#define FW_EXTRALIGHT 200
#define FW_LIGHT 300
#define FW_NORMAL 400
#define FW_MEDIUM 500
#define FW_SEMIBOLD 600
#define FW_BOLD 700
#define FW_EXTRABOLD 800
#define FW_HEAVY 900
#define FW_BLACK 900

// --- 字体质量与 pitch
#define DEFAULT_QUALITY 0
#define DRAFT_QUALITY 1
#define PROOF_QUALITY 2
#define NONANTIALIASED_QUALITY 3
#define ANTIALIASED_QUALITY 4
#define CLEARTYPE_QUALITY 5
#define DEFAULT_PITCH 0
#define FIXED_PITCH 1
#define VARIABLE_PITCH 2

// --- GetGlyphIndices
#define GGI_MARK_NONEXISTING_GLYPHS 0x0001

// --- 文本对齐 / 输出
#define TA_NOUPDATECP 0
#define TA_UPDATECP 1
#define ETO_OPAQUE 0x0002
#define ETO_CLIPPED 0x0004

// --- 系统错误码
#ifndef ERROR_SHARING_VIOLATION
#define ERROR_SHARING_VIOLATION 32L
#endif

// --- 菜单项状态
#ifndef DELETE
#define DELETE 0x00010000L
#endif

// --- 窗口遍历（GetWindow）
#define GW_HWNDFIRST 0
#define GW_HWNDLAST 1
#define GW_HWNDNEXT 2
#define GW_HWNDPREV 3
#define GW_OWNER 4
#define GW_CHILD 5
#define GW_ENABLEDPOPUP 6
#define GW_MAX 6

// --- 图像色彩管理（ICM）
// 常量已在上面 "图像色彩匹配" 处定义（ICM_QUERY=1 / ICM_ON=2 / ICM_OFF=3）；
// 本层软件 DC 从不做色彩管理，因此 SetICMMode 的查询结果恒为 ICM_OFF。

// --- BLENDFUNCTION::BlendOp
#define AC_SRC_OVER 0x00
#define AC_SRC_ALPHA 0x01

// --- EMF 记录类型
// 全部枚举值取自 wingdi.h 官方定义（EMR = Enhanced Metafile Record）。iType 是
// 回放器 switch 的判据：值一旦与 Windows 不一致，MapNativeOverlayReplayWin32 会把
// 一条记录当成另一条来解析——编译全绿、运行静默错。中间的空洞（23/28/53/69/99+
// 与 117）是 wingdi.h 里预留/后来占用的位，本层不定义未用到的位，与"不定义未用
// 记录"的旧原则一致，只是覆盖面扩到回放器实际会 switch 到的全部类型。
#define EMR_HEADER                 1
#define EMR_POLYBEZIER             2
#define EMR_POLYGON                3
#define EMR_POLYLINE               4
#define EMR_POLYBEZIERTO           5
#define EMR_POLYLINETO             6
#define EMR_POLYPOLYLINE           7
#define EMR_POLYPOLYGON            8
#define EMR_SETWINDOWEXTEX         9
#define EMR_SETWINDOWORGEX        10
#define EMR_SETVIEWPORTEXTEX      11
#define EMR_SETVIEWPORTORGEX      12
#define EMR_SETBRUSHORGEX         13
#define EMR_EOF                   14
#define EMR_SETPIXELV             15
#define EMR_SETMAPPERFLAGS        16
#define EMR_SETMAPMODE            17
#define EMR_SETBKMODE             18
#define EMR_SETPOLYFILLMODE       19
#define EMR_SETROP2               20
#define EMR_SETSTRETCHBLTMODE     21
#define EMR_SETTEXTALIGN          22
#define EMR_SETTEXTCOLOR          24
#define EMR_SETBKCOLOR            25
#define EMR_OFFSETCLIPRGN         26
#define EMR_MOVETOEX              27
#define EMR_EXCLUDECLIPRECT       29
#define EMR_INTERSECTCLIPRECT     30
#define EMR_SCALEVIEWPORTEXTEX    31
#define EMR_SCALEWINDOWEXTEX      32
#define EMR_SAVEDC                33
#define EMR_RESTOREDC             34
#define EMR_SETWORLDTRANSFORM     35
#define EMR_MODIFYWORLDTRANSFORM  36
#define EMR_SELECTOBJECT          37
#define EMR_CREATEPEN             38
#define EMR_CREATEBRUSHINDIRECT   39
#define EMR_DELETEOBJECT          40
#define EMR_ANGLEARC              41
#define EMR_ELLIPSE               42
#define EMR_RECTANGLE             43
#define EMR_ROUNDRECT             44
#define EMR_ARC                   45
#define EMR_CHORD                 46
#define EMR_PIE                   47
#define EMR_SELECTPALETTE         48
#define EMR_CREATEPALETTE         49
#define EMR_SETPALETTEENTRIES     50
#define EMR_RESIZEPALETTE         51
#define EMR_REALIZEPALETTE        52
#define EMR_LINETO                54
#define EMR_ARCTO                 55
#define EMR_POLYDRAW              56
#define EMR_SETARCDIRECTION       57
#define EMR_SETMITERLIMIT         58
#define EMR_BEGINPATH             59
#define EMR_ENDPATH               60
#define EMR_CLOSEFIGURE           61
#define EMR_FILLPATH              62
#define EMR_STROKEANDFILLPATH     63
#define EMR_STROKEPATH            64
#define EMR_FLATTENPATH           65
#define EMR_WIDENPATH             66
#define EMR_SELECTCLIPPATH        67
#define EMR_ABORTPATH             68
#define EMR_GDICOMMENT            70
#define EMR_FILLRGN               71
#define EMR_FRAMERGN              72
#define EMR_INVERTRGN             73
#define EMR_PAINTRGN              74
#define EMR_EXTSELECTCLIPRGN      75
#define EMR_BITBLT                76
#define EMR_STRETCHBLT            77
#define EMR_MASKBLT               78
#define EMR_PLGBLT                79
#define EMR_SETDIBITSTODEVICE     80
#define EMR_STRETCHDIBITS         81
#define EMR_EXTCREATEFONTINDIRECTW 82
#define EMR_EXTTEXTOUTA           83
#define EMR_EXTTEXTOUTW           84
#define EMR_POLYBEZIER16          85
#define EMR_POLYGON16             86
#define EMR_POLYLINE16            87
#define EMR_POLYBEZIERTO16        88
#define EMR_POLYLINETO16          89
#define EMR_POLYPOLYLINE16        90
#define EMR_POLYPOLYGON16         91
#define EMR_POLYDRAW16            92
#define EMR_CREATEMONOBRUSH       93
#define EMR_CREATEDIBPATTERNBRUSHPT 94
#define EMR_EXTCREATEPEN          95
#define EMR_POLYTEXTOUTA          96
#define EMR_POLYTEXTOUTW          97
#define EMR_SETICMMODE            98
#define EMR_ALPHABLEND           114
#define EMR_SETLAYOUT            115
#define EMR_TRANSPARENTBLT       116
#define EMR_GRADIENTFILL         118

// --- 内存操作宏（Win32 把它们定义到 RtlXxx，最终都是 memcpy/memmove/memset）
#ifndef CopyMemory
#define CopyMemory(destination, source, length) std::memcpy((destination), (source), (length))
#endif
#ifndef RtlCopyMemory
#define RtlCopyMemory(destination, source, length) std::memcpy((destination), (source), (length))
#endif
#ifndef MoveMemory
#define MoveMemory(destination, source, length) std::memmove((destination), (source), (length))
#endif
#ifndef ZeroMemory
#define ZeroMemory(destination, length) std::memset((destination), 0, (length))
#endif
#ifndef FillMemory
#define FillMemory(destination, length, fill) std::memset((destination), (fill), (length))
#endif

// ---------------------------------------------------------------------------
// 几何 / 结构体
// ---------------------------------------------------------------------------
struct XFORM {
    FLOAT eM11 = 1.0f;
    FLOAT eM12 = 0.0f;
    FLOAT eM21 = 0.0f;
    FLOAT eM22 = 1.0f;
    FLOAT eDx = 0.0f;
    FLOAT eDy = 0.0f;
};
using LPXFORM = XFORM*;

struct RECTL {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};

// SetWindowPos 的通知结构。WINDOWPOS 在 NCCALCSIZE_PARAMS 之前定义。
struct WINDOWPOS {
    HWND hwnd;
    HWND hwndInsertAfter;
    int x;
    int y;
    int cx;
    int cy;
    UINT flags;
};
using LPWINDOWPOS = WINDOWPOS*;
using PWINDOWPOS = WINDOWPOS*;

// WM_NCCALCSIZE 带客户区重算时传入。
struct NCCALCSIZE_PARAMS {
    RECT rgrc[3];
    PWINDOWPOS lppos;
};

struct MONITORINFO {
    DWORD cbSize;
    RECT rcMonitor;
    RECT rcWork;
    DWORD dwFlags;
};
using LPMONITORINFO = MONITORINFO*;

// AlphaBlend 的混合描述。
struct BLENDFUNCTION {
    BYTE BlendOp;
    BYTE BlendFlags;
    BYTE SourceConstantAlpha;
    BYTE AlphaFormat;
};
using LPBLENDFUNCTION = BLENDFUNCTION*;

// CreatePenIndirect / GetObjectW(LOGPEN) 用。
struct LOGPEN {
    UINT lopnStyle;
    POINT lopnWidth;
    COLORREF lopnColor;
};
using LPLOGPEN = LOGPEN*;

// 画刷样式与 LOGBRUSH —— GetObjectW(brush, sizeof(LOGBRUSH), ...) 的载体。
// 产品侧取这个结构只为回答一个问题："这支画刷会不会落笔"（BS_NULL）。
// lbHatch 是 ULONG_PTR：Win32 里它同时承载图案位图句柄与阴影线号，宽度随
// 目标架构变化，写成 DWORD 会让 LOGBRUSH 在 x86_64 上少 4 字节。
#define BS_SOLID 0
#define BS_NULL 1
#define BS_HOLLOW BS_NULL
#define BS_HATCHED 2
#define BS_PATTERN 3
#define BS_INDEXED 4
#define BS_DIBPATTERN 5

struct LOGBRUSH {
    UINT lbStyle;
    COLORREF lbColor;
    ULONG_PTR lbHatch;
};
using LPLOGBRUSH = LOGBRUSH*;

// 这几个 struct 的字节数就是 GetObjectW 的返回值判据（调用点写的是
// `GetObjectW(...) == sizeof(X)`），布局一旦漂移，失败会先出现在业务断言里，
// 而不是编译期。数值按 LP64（macOS x86_64 / arm64 同构）钉死；若将来要支持
// 32 位目标，这几条断言会先报出来，而不是让偏移悄悄变化。
static_assert(sizeof(LOGPEN) == 16, "Win32 LOGPEN is 16 bytes (UINT + POINT + COLORREF)");
static_assert(sizeof(BITMAP) == 32, "Win32 BITMAP is 32 bytes on LP64");
static_assert(sizeof(LOGBRUSH) == 16, "Win32 LOGBRUSH is 16 bytes on LP64");
static_assert(sizeof(DIBSECTION) == 104, "Win32 DIBSECTION is 104 bytes on LP64");
static_assert(offsetof(DIBSECTION, dsBmih) == sizeof(BITMAP), "DIBSECTION embeds BITMAP first");
static_assert(offsetof(BITMAP, bmBits) % alignof(void*) == 0, "BITMAP.bmBits must be pointer-aligned");

// ---------------------------------------------------------------------------
// 增强图元文件（EMF）
//
// 本层实现一个**真实的记录器**（不是桩）：CreateEnhMetaFileW 返回一个录制型 DC，
// 往它上面 BitBlt/StretchBlt 会把源位图连同 BITMAPINFOHEADER 一起序列化成
// EMR_BITBLT/EMR_STRETCHBLT 记录；CloseEnhMetaFile 补齐头部与 EMR_EOF；
// EnumEnhMetaFile 按序回放并回调。
//
// 为什么必须真做：RuntimeLoadingFrameStabilitySmoke / runCampaignMenuPaintSmoke
// 把一次重绘录成指令流，再回放审计"是否只有一次完整帧拷贝"——这是发现截屏看不
// 出来的中间黑帧的唯一手段。若 EnumEnhMetaFile 返回 TRUE 却不回调，审计的
// copies 会是 0，"没有中间帧"这个结论就变成了空断言。
// ---------------------------------------------------------------------------
using HENHMETAFILE = void*;

// 回调收到的句柄表。Win32 只有 objectHandle，没有元素计数。
struct HANDLETABLE {
    HGDIOBJ objectHandle[1];
};
using PHANDLETABLE = HANDLETABLE*;
using LPHANDLETABLE = HANDLETABLE*;

// 所有 EMF 记录的公共头部；记录本身按 iType 解释。
struct ENHMETARECORD {
    DWORD iType;
    DWORD nSize;
    DWORD dParm[1];
};
using PENHMETARECORD = ENHMETARECORD*;
using LPENHMETARECORD = ENHMETARECORD*;

// 所有 EMF 记录的基础结构（iType + nSize）。ENHMETARECORD 是它的超集（多 dParm[1]），
// 两者内存上前 8 字节同形；回放器用 sizeof(EMR) 做最小记录长度判据。
struct EMR {
    DWORD iType;
    DWORD nSize;
};
using PEMR = EMR*;

using POINTL = POINT;
using PPOINTL = POINT*;

// EXTLOGPEN32：EMREXTCREATEPEN 的扩展笔描述。回放器只读 elpWidth（拿来算
// penPadding），其余字段为 ABI 对齐而保留。ULONG_PTR 宽度随目标架构（LP64 下 8 字节）。
struct EXTLOGPEN32 {
    DWORD elpPenStyle;
    DWORD elpWidth;
    DWORD elpBrushStyle;
    COLORREF elpColor;
    ULONG_PTR elpHatch;
    DWORD elpNumEntries;
    DWORD elpStyleEntry[1];
};
using PEXTLOGPEN32 = EXTLOGPEN32*;

// 下面是 MapNativeOverlayReplayWin32 回放器用 reinterpret_cast 读字段的记录体。
// 字段偏移必须与 wingdi.h 一致——偏移错 4 字节，回放器就拿错误的 ptl/半径去算
// dirty 矩形，结果"看着对、覆盖区全错"。下方 static_assert 逐条钉死关键偏移。

// EMR_GDICOMMENT：GdiComment 写入的私有数据。Data 是变长尾随数组，cbData 给长度。
struct EMRGDICOMMENT {
    EMR emr;
    DWORD cbData;
    BYTE Data[1];
};
using PEMRGDICOMMENT = EMRGDICOMMENT*;

// EMR_MOVETOEX：移动当前点到 ptl。
struct EMRMOVETOEX {
    EMR emr;
    POINTL ptl;
};

// EMR_LINETO：从当前点画线到 ptl。
struct EMRLINETO {
    EMR emr;
    POINTL ptl;
};

// EMR_RESTOREDC：iRelative < 0 表示"恢复到距离栈顶 |iRelative| 的状态"。
struct EMRRESTOREDC {
    EMR emr;
    LONG iRelative;
};

// EMR_CREATEPEN：ihPen 是新对象的句柄索引；lopn 是逻辑笔描述。
struct EMRCREATEPEN {
    EMR emr;
    DWORD ihPen;
    LOGPEN lopn;
};

// EMR_EXTCREATEPEN：扩展笔，elp 是 EXTLOGPEN32。
struct EMREXTCREATEPEN {
    EMR emr;
    DWORD ihPen;
    DWORD offBmi;
    DWORD cbBmi;
    DWORD offBits;
    DWORD cbBits;
    EXTLOGPEN32 elp;
};

// EMR_SETPIXELV：画单像素。ptlPixel 是坐标，crColor 是颜色。
struct EMRSETPIXELV {
    EMR emr;
    POINTL ptlPixel;
    COLORREF crColor;
};

// EMR_ANGLEARC：画圆弧线段。ptlCenter 圆心、nRadius 半径（回放器只读这两个）。
struct EMRANGLEARC {
    EMR emr;
    POINTL ptlCenter;
    DWORD nRadius;
    FLOAT eStartAngle;
    FLOAT eSweepAngle;
};

static_assert(sizeof(EMR) == 8, "Win32 EMR is 8 bytes (iType + nSize)");
static_assert(offsetof(EMRGDICOMMENT, cbData) == 8, "EMRGDICOMMENT::cbData sits at byte 8");
static_assert(offsetof(EMRGDICOMMENT, Data) == 12, "EMRGDICOMMENT::Data sits at byte 12");
static_assert(offsetof(EMRMOVETOEX, ptl) == 8, "EMRMOVETOEX::ptl sits at byte 8");
static_assert(offsetof(EMRLINETO, ptl) == 8, "EMRLINETO::ptl sits at byte 8");
static_assert(offsetof(EMRRESTOREDC, iRelative) == 8, "EMRRESTOREDC::iRelative sits at byte 8");
static_assert(offsetof(EMRCREATEPEN, lopn) == 12, "EMRCREATEPEN::lopn sits at byte 12");
// EXTLOGPEN32 内含 ULONG_PTR（LP64 下 8 字节、对齐 8），所以 EMREXTCREATEPEN 里
// elp 前面 28 字节后要 pad 4 才对齐——elp 落在 32，不是 28。MSVC 与 Clang 默认
// 对齐规则相同，Windows x64 上也是 32。
static_assert(offsetof(EMREXTCREATEPEN, elp) == 32, "EMREXTCREATEPEN::elp sits at byte 32");
static_assert(offsetof(EMREXTCREATEPEN, elp) + offsetof(EXTLOGPEN32, elpWidth) == 36,
              "EMREXTCREATEPEN::elp.elpWidth sits at byte 36");
static_assert(offsetof(EMRSETPIXELV, ptlPixel) == 8, "EMRSETPIXELV::ptlPixel sits at byte 8");
static_assert(offsetof(EMRANGLEARC, ptlCenter) == 8, "EMRANGLEARC::ptlCenter sits at byte 8");
static_assert(offsetof(EMRANGLEARC, nRadius) == 16, "EMRANGLEARC::nRadius sits at byte 16");

// EMR_HEADER 的记录体。**必须逐字段对齐**：回放器按 offBmiSrc 这类偏移在记录
// 内部寻址，而记录之间靠 nSize 串起来，头部大小错 1 字节整条流就散了。
// sizeof == 108 是 Windows 上的定值，已在下方 static_assert 钉住。
#define ENHMETA_SIGNATURE 0x464D4520u  // ' EMF'（小端读作 0x464D4520）
#define ENHMETA_STOCK_OBJECT 0x80000000u

struct ENHMETAHEADER {
    DWORD iType;        // EMR_HEADER
    DWORD nSize;        // sizeof(ENHMETAHEADER)
    RECTL rclBounds;    // 设备单位的外接矩形（闭区间）
    RECTL rclFrame;     // 0.01 毫米单位的图框
    DWORD dSignature;   // ENHMETA_SIGNATURE
    DWORD nVersion;     // 0x00010000
    DWORD nBytes;       // 整条流字节数（含头部与 EOF）
    DWORD nRecords;     // 记录数（含头部与 EOF）
    WORD nHandles;      // 句柄数；0 或 1
    WORD sReserved;
    DWORD nDescription; // 描述字符串的字符数
    DWORD offDescription;
    DWORD nPalEntries;
    SIZEL szlDevice;      // 参考设备的像素尺寸
    SIZEL szlMillimeters; // 参考设备的毫米尺寸
    DWORD cbPixelFormat;
    DWORD offPixelFormat;
    DWORD bOpenGL;
    SIZEL szlMicrometers;
};

// EMR_BITBLT 的布局必须与 Windows 一致：审计代码会按 offset 直接读
// offBmiSrc 处的 BITMAPINFOHEADER，并按 cbBitsSrc / 行数校验步长，
// 因此字段顺序与类型一个都不能错。
struct EMRBITBLT {
    DWORD iType;
    DWORD nSize;
    RECTL rclBounds;
    LONG xDest;
    LONG yDest;
    LONG cxDest;
    LONG cyDest;
    DWORD dwRop;
    LONG xSrc;
    LONG ySrc;
    XFORM xformSrc;
    COLORREF crBkColorSrc;
    DWORD iUsageSrc;
    DWORD offBmiSrc;
    DWORD cbBmiSrc;
    DWORD offBitsSrc;
    DWORD cbBitsSrc;
};

// EMR_STRETCHBLT 比 EMR_BITBLT 多一对目标尺寸（cxSrc/cySrc），其余完全同形。
struct EMRSTRETCHBLT {
    DWORD iType;
    DWORD nSize;
    RECTL rclBounds;
    LONG xDest;
    LONG yDest;
    LONG cxDest;
    LONG cyDest;
    DWORD dwRop;
    LONG xSrc;
    LONG ySrc;
    XFORM xformSrc;
    COLORREF crBkColorSrc;
    DWORD iUsageSrc;
    DWORD offBmiSrc;
    DWORD cbBmiSrc;
    DWORD offBitsSrc;
    DWORD cbBitsSrc;
    LONG cxSrc;
    LONG cySrc;
};

static_assert(sizeof(ENHMETAHEADER) == 108, "ENHMETAHEADER is 108 bytes on Windows");
static_assert(sizeof(EMRBITBLT) == 100, "EMRBITBLT is 100 bytes on Windows");
static_assert(sizeof(EMRSTRETCHBLT) == 108, "EMRSTRETCHBLT is 108 bytes on Windows");
static_assert(offsetof(EMRBITBLT, dwRop) == 40, "EMRBITBLT::dwRop sits at byte 40");
static_assert(offsetof(EMRBITBLT, offBmiSrc) == 84, "EMRBITBLT::offBmiSrc sits at byte 84");

using ENHMFENUMPROC = int (*)(HDC, HANDLETABLE*, const ENHMETARECORD*, int, LPARAM);
using WNDENUMPROC = BOOL (*)(HWND, LPARAM);
using DESKTOPENUMPROCW = BOOL (*)(HWND, LPARAM);

extern "C" {

// --- 窗口类与窗口状态
BOOL UnregisterClassW(LPCWSTR lpClassName, HINSTANCE hInstance);
BOOL GetClassInfoW(HINSTANCE hInstance, LPCWSTR lpClassName, WNDCLASSW* lpWndClass);
HWND GetWindow(HWND hWnd, UINT uCmd);
int SetICMMode(HDC hdc, int mode);
BOOL IsWindowEnabled(HWND hWnd);
BOOL EnableWindow(HWND hWnd, BOOL bEnable);
DWORD GetWindowThreadProcessId(HWND hWnd, LPDWORD lpdwProcessId);
HWND WindowFromPoint(POINT point);
BOOL EnumDesktopWindows(HDESK hDesktop, DESKTOPENUMPROCW lpfn, LPARAM lParam);
HDESK GetThreadDesktop(DWORD dwThreadId);

// --- 键盘 / 光标状态
SHORT GetKeyState(int nVirtKey);
BOOL GetKeyboardState(BYTE* lpKeyState);
BOOL SetKeyboardState(const BYTE* lpKeyState);
UINT MapVirtualKeyW(UINT uCode, UINT uMapType);
int GetKeyNameTextW(LONG lParam, LPWSTR lpString, int cchSize);
HCURSOR GetCursor();

// --- 绘制会话
HDC BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint);
BOOL EndPaint(HWND hWnd, const PAINTSTRUCT* lpPaint);
BOOL GetCurrentPositionEx(HDC hdc, LPPOINT lpPoint);
COLORREF GetDCPenColor(HDC hdc);
BOOL GdiFlush();
int DrawTextA(HDC hdc, LPCSTR lpchText, int cchText, LPRECT lprc, UINT format);
DWORD GdiSetBatchLimit(DWORD dwLimit);

// --- 环境变量（宽/窄两版）
DWORD GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize);
DWORD GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);

// --- EMF
HDC CreateEnhMetaFileW(HDC hdcRef, LPCWSTR lpFilename, const RECT* lpRect, LPCWSTR lpDescription);
HENHMETAFILE CloseEnhMetaFile(HDC hdc);
BOOL DeleteEnhMetaFile(HENHMETAFILE hmf);
BOOL EnumEnhMetaFile(HDC hdc, HENHMETAFILE hmf, ENHMFENUMPROC lpProc, LPVOID lpParam,
                     const RECT* lpRect);
// 回放与序列化配套（MapNativeOverlay 放回构建图后的必需项）：GdiComment 往录制型 DC
// 追加 EMR_GDICOMMENT；GetEnhMetaFileBits 导出定稿图元文件的字节流；PlayEnhMetaFileRecord
// 回放单条记录；PtVisible 判点是否落在当前裁剪区可见范围内。
BOOL GdiComment(HDC hdc, UINT cbSize, const BYTE* lpData);
UINT GetEnhMetaFileBits(HENHMETAFILE hmf, UINT cbBuffer, LPBYTE lpbBuffer);
BOOL PlayEnhMetaFileRecord(HDC hdc, LPHANDLETABLE lpHandles, const ENHMETARECORD* lpEnhMetaRecord,
                           UINT nHandles);
BOOL PtVisible(HDC hdc, int x, int y);

// --- 第二批（第一轮补齐后剩余的 6 个未声明符号）
BOOL GdiAlphaBlend(HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
                   HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, BLENDFUNCTION ftn);
BOOL DrawFrameControl(HDC hdc, LPRECT lprc, UINT type, UINT state);
HMONITOR MonitorFromWindow(HWND hwnd, DWORD dwFlags);
BOOL GetMonitorInfoW(HMONITOR hMonitor, LPMONITORINFO lpmi);
DWORD GetGlyphIndicesW(HDC hdc, LPCWSTR lpstr, int c, LPWORD pgi, DWORD fl);
BOOL ExtTextOutA(HDC hdc, int x, int y, UINT options, const RECT* lprect,
                 LPCSTR lpString, UINT c, const INT* lpDx);

} // extern "C"

// --- MSVC CRT 扩展（C++ 链接，与上面的 _dupenv_s 一致）
errno_t _putenv_s(const char* varname, const char* value_string);
errno_t _wgetenv_s(std::size_t* pReturnValue, wchar_t* buffer, std::size_t numberOfElements,
                   const wchar_t* varname);
// getenv_s 是 _wgetenv_s 的窄字符版（MSVC CRT 同族），语义完全一致：变量存在返回 0
// 且 *pReturnValue=所需元素数（含 NUL）；不存在返回 0 且 *pReturnValue=0；缓冲不够
// 返回 ERANGE。MapNativeOverlay 的 nativeDiagnosticFlag 用它两段式读诊断开关。
errno_t getenv_s(std::size_t* pReturnValue, char* buffer, std::size_t numberOfElements,
                 const char* varname);
errno_t gmtime_s(struct tm* tmDest, const std::time_t* sourceTime);
// S8：strncpy_s。调用点用的是 MSVC 的**数组重载**（三参：dest/src/count，容量由数组推出来），
// 所以这里必须既有四参基础版本、又有按数组长度推导的模板版本；只给四参版本的话，
// 产品里 `strncpy_s(buf, src, _TRUNCATE)` 这条会报 "no matching function"。
errno_t strncpy_s(char* dest, std::size_t destSize, const char* src, std::size_t count);
template <std::size_t N>
inline errno_t strncpy_s(char (&dest)[N], const char* src, std::size_t count) {
    return strncpy_s(dest, N, src, count);
}

// --- 窄路径入口重载
//
// std::filesystem::path::c_str() 在 Windows 上返回 const wchar_t*，在 POSIX 上返回
// const char*（POSIX 的原生路径就是窄字符）。项目里大量 CreateFileW(path.c_str(), ...)
// 在 Windows 天然匹配，在 POSIX 则完全不匹配。
//
// 这里给路径类 API 补一个窄字符重载：语义与宽版完全相同，只是文件名以本地窄编码
// 给出。这样调用点一行都不用改，也不需要 #ifdef 分叉。
//
// 必须放在 extern "C" 之外——C 语言链接同一个名字只允许一个函数。
HANDLE CreateFileW(const char* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   SECURITY_ATTRIBUTES* lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);

// ===========================================================================
// S5b：窗口 / 输入 / 钩子 / 进程资源 族
//
// 这一节的全部条目来自 4 个 TU 的真实编译错误（SkirmishMenu / MainMenu /
// CampaignMenu / MapRenderer），不是"照着 windows.h 抄一遍"。分类原则：
//
//   · 常量与结构体 —— 按 Win32 取值 1:1 给出，sizeof 由 selftest 的
//     static_assert 钉住。取值算错不会报错，只会让断言变成碰巧成立的假断言
//     （S5a 的 ICM_QUERY/ICM_OFF 互换就是先例）。
//   · 查询类 API —— 只声明。除下面三个例外，实现留给 SDL3 窗口后端那一轮：
//       GetGuiResources      → 由 S2 的对象表给出真实存活计数（已实现）
//       GetProcessHandleCount→ 数本进程打开的 fd（已实现）
//       DPI 两件             → macOS 的 backing scale 本就是 per-display 语义
//   · 合成类 API（SendInput）—— 只声明。它必须在真实的窗口消息队列上生效，
//     没有窗口就注入不进去；假返回 cInputs 会让"真的注入了输入"这类断言静默失效。
// ===========================================================================

// --- 钩子
using HHOOK = void*;
using HOOKPROC = LRESULT (*)(int nCode, WPARAM wParam, LPARAM lParam);
// CBTProc 是 WH_CBT 钩子的回调类型，签名与 HOOKPROC 完全一致（WinDef.h 里它
// 就是 `LRESULT CALLBACK (*)(int, WPARAM, LPARAM)`）。单独起名是为了让诊断 smoke
// 里 `CBTProc cb = ...` 这类写法能编译。
using CBTProc = HOOKPROC;
// CALLBACK 已在前面定义（可能是空宏），回调指针类型不再重复写 CALLBACK——
// 重复写会在 CALLBACK 为空宏的平台与真实签名不一致。

// --- COM 错误码
#ifndef RPC_E_CHANGED_MODE
#define RPC_E_CHANGED_MODE ((HRESULT)0x80010106L)
#endif
#ifndef E_NOTIMPL
#define E_NOTIMPL ((HRESULT)0x80004001L)
#endif

// --- 输入合成
//
// Win32 x64 / arm64 上 sizeof(INPUT) 都是 40：MOUSEINPUT 32 + type 4 + 对齐 4。
// 这个值同时是 SendInput 的 cbSize 参数，是 ABI 的一部分，不能随平台漂。
struct MOUSEINPUT {
    LONG dx;
    LONG dy;
    DWORD mouseData;
    DWORD dwFlags;
    DWORD time;
    ULONG_PTR dwExtraInfo;
};

struct KEYBDINPUT {
    WORD wVk;
    WORD wScan;
    DWORD dwFlags;
    DWORD time;
    ULONG_PTR dwExtraInfo;
};

struct HARDWAREINPUT {
    DWORD uMsg;
    WORD wParamL;
    WORD wParamH;
};

struct INPUT {
    DWORD type;
    union {
        MOUSEINPUT mi;
        KEYBDINPUT ki;
        HARDWAREINPUT hi;
    };
};
using PINPUT = INPUT*;
using LPINPUT = INPUT*;

// --- COM 基接口
//
// 项目只用 IUnknown 做"效果器句柄"的载体（XAudio2 的 peak limiter），从不
// QueryInterface。这里给出与真实 COM 一致的三方法布局：如果将来有人真的
// QueryInterface，布局对得上才不会读到垃圾。
struct IUnknown {
    virtual HRESULT QueryInterface(const GUID& iid, void** object) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;

protected:
    ~IUnknown() = default;
};

// --- 光标 / 电源
struct CURSORINFO {
    DWORD cbSize;
    DWORD flags;
    HCURSOR hCursor;
    POINT ptScreenPos;
};
using PCURSORINFO = CURSORINFO*;
using LPCURSORINFO = CURSORINFO*;

struct SYSTEM_POWER_STATUS {
    BYTE ACLineStatus;
    BYTE BatteryFlag;
    BYTE BatteryLifePercent;
    BYTE SystemStatusFlag;
    DWORD BatteryLifeTime;
    DWORD BatteryFullLifeTime;
};
using LPSYSTEM_POWER_STATUS = SYSTEM_POWER_STATUS*;

// SYSTEM_POWER_STATUS.ACLineStatus 的取值（GetSystemPowerStatus 回填）。
// 纯离屏宿主没有真实电源，恒报"交流电在线"，与"按原生像素渲染"的软件帧缓冲一致。
#ifndef AC_LINE_OFFLINE
#define AC_LINE_OFFLINE 0
#endif
#ifndef AC_LINE_ONLINE
#define AC_LINE_ONLINE 1
#endif
#ifndef AC_LINE_UNKNOWN
#define AC_LINE_UNKNOWN 255
#endif

// --- DPI 感知
enum DPI_AWARENESS {
    DPI_AWARENESS_INVALID = -1,
    DPI_AWARENESS_UNAWARE = 0,
    DPI_AWARENESS_SYSTEM_AWARE = 1,
    DPI_AWARENESS_PER_MONITOR_AWARE = 2
};
using DPI_AWARENESS_CONTEXT = void*;

// 与 Win32 一致的 DPI 感知上下文哨兵值（HANDLE 型 -1/-2/-3/-4）。Windows 上
// RuntimeMain 用 -4 显式关闭 DPI 位图虚拟化；本平台软件帧缓冲不缩放、天然按
// 原生像素渲染，等价于"每显示器感知"，故 GetThreadDpiAwarenessContext 返回
// PER_MONITOR_AWARE(-3)。见 RuntimeWin32CompatGdi.cpp 的 GetAwarenessFromDpiAwarenessContext。
#define DPI_AWARENESS_CONTEXT_UNAWARE ((DPI_AWARENESS_CONTEXT)(std::intptr_t)-1)
#define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE ((DPI_AWARENESS_CONTEXT)(std::intptr_t)-2)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ((DPI_AWARENESS_CONTEXT)(std::intptr_t)-3)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)(std::intptr_t)-4)

// --- 钩子常量
// 本层只真实实现 WH_CBT（本线程、HCBT_CREATEWND/DESTROYWND）。其余 WH_* 仅给出
// 标准值，让诊断 smoke 里"非 CBT 钩子应诚实失败"这类判据能编译。
#define WH_KEYBOARD 2
#define WH_CBT 5
#define WH_SYSMSGFILTER 6
#define WH_MOUSE 7
#define HCBT_MOVESIZE 0
#define HCBT_MINMAX 1
#define HCBT_QS 2
#define HCBT_CREATEWND 3
#define HCBT_DESTROYWND 4
#define HCBT_ACTIVATE 5

// --- 进程资源（GetGuiResources 的 uiFlags）
#define GR_GDIOBJECTS 0
#define GR_USEROBJECTS 1

// --- 消息等待（MsgWaitForMultipleObjectsEx）
#ifndef MWMO_INPUTAVAILABLE
#define MWMO_INPUTAVAILABLE 0x0004
#endif

// --- 鼠标消息区间（诊断里用它枚举"所有鼠标消息"）
#ifndef WM_MOUSEFIRST
#define WM_MOUSEFIRST 0x0200
#endif
#ifndef WM_MOUSELAST
#define WM_MOUSELAST 0x020E
#endif

// --- GetWindow 的 uCmd
#define GA_PARENT 1
#define GA_ROOT 2
#define GA_ROOTOWNER 3

// --- 编辑框样式与消息（host lifecycle 诊断用来验证子窗口样式）
#define ES_LEFT 0x0000L
#define ES_CENTER 0x0001L
#define ES_RIGHT 0x0002L
#define ES_MULTILINE 0x0004L
#define ES_AUTOVSCROLL 0x0040L
#define ES_AUTOHSCROLL 0x0080L
#define EM_SETLIMITTEXT 0x00C5
#define EM_GETLIMITTEXT 0x00D5
#define EN_CHANGE 0x0300

// --- 虚拟屏幕 / 会话（多显示器与远程桌面判定）
#ifndef SM_XVIRTUALSCREEN
#define SM_XVIRTUALSCREEN 76
#endif
#ifndef SM_YVIRTUALSCREEN
#define SM_YVIRTUALSCREEN 77
#endif
#ifndef SM_CXVIRTUALSCREEN
#define SM_CXVIRTUALSCREEN 78
#endif
#ifndef SM_CYVIRTUALSCREEN
#define SM_CYVIRTUALSCREEN 79
#endif
#ifndef SM_CMONITORS
#define SM_CMONITORS 80
#endif
#ifndef SM_REMOTESESSION
#define SM_REMOTESESSION 0x1000
#endif

// --- 随消息窗口（只收消息、不可见的窗口）
#ifndef HWND_MESSAGE
#define HWND_MESSAGE ((HWND)-3)
#endif

// --- 输入类型与标志
#define INPUT_MOUSE 0
#define INPUT_KEYBOARD 1
#define INPUT_HARDWARE 2

#define MOUSEEVENTF_MOVE 0x0001
#define MOUSEEVENTF_LEFTDOWN 0x0002
#define MOUSEEVENTF_LEFTUP 0x0004
#define MOUSEEVENTF_RIGHTDOWN 0x0008
#define MOUSEEVENTF_RIGHTUP 0x0010
#define MOUSEEVENTF_MIDDLEDOWN 0x0020
#define MOUSEEVENTF_MIDDLEUP 0x0040
#define MOUSEEVENTF_XDOWN 0x0080
#define MOUSEEVENTF_XUP 0x0100
#define MOUSEEVENTF_WHEEL 0x0800
#define MOUSEEVENTF_HWHEEL 0x1000
#define MOUSEEVENTF_VIRTUALDESK 0x4000
#define MOUSEEVENTF_ABSOLUTE 0x8000

#define KEYEVENTF_EXTENDEDKEY 0x0001
#define KEYEVENTF_KEYUP 0x0002
#define KEYEVENTF_UNICODE 0x0004
#define KEYEVENTF_SCANCODE 0x0008

// --- 光标状态（CURSORINFO::flags）
#ifndef CURSOR_SHOWING
#define CURSOR_SHOWING 0x00000001
#endif

extern "C" {

// --- 进程资源核算
//
// GetGuiResources 的语义是"本进程当前存活的 GDI/USER 对象数"。这里返回的
// 是 compat 层对象表里的真实存活数（S2 已实现的 19 个函数所创建的对象），
// 所以调用点的 before/after 相等断言是**真的**泄漏检测，不是恒真式。
// GR_USEROBJECTS 暂时恒为 0——窗口对象在 S5 的 SDL3 后端落地前不存在。
DWORD GetGuiResources(HANDLE hProcess, DWORD uiFlags);
BOOL GetProcessHandleCount(HANDLE hProcess, LPDWORD pdwCount);

// --- 输入合成（实现依赖 SDL3 事件后端，本轮只声明）
UINT SendInput(UINT cInputs, LPINPUT pInputs, int cbSize);
SHORT GetAsyncKeyState(int vKey);

// --- 窗口遍历 / 查找 / 钩子
BOOL EnumThreadWindows(DWORD dwThreadId, WNDENUMPROC lpfn, LPARAM lParam);
HWND FindWindowExW(HWND hWndParent, HWND hWndChildAfter, LPCWSTR lpszClass, LPCWSTR lpszWindow);
HHOOK SetWindowsHookExW(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId);
BOOL UnhookWindowsHookEx(HHOOK hhk);
LRESULT CallNextHookEx(HHOOK hhk, int nCode, WPARAM wParam, LPARAM lParam);
HMENU GetMenu(HWND hWnd);
HWND GetForegroundWindow();
DWORD GetDoubleClickTime();

// --- 消息等待（只声明）
//
// 全部 5 处调用点都是 MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT,
// MWMO_INPUTAVAILABLE)——即"带超时地泵一次消息"。它在没有窗口的平台上等价于
// Sleep，但那会让消息泵变成忙等，故实现在 SDL3 事件后端，这里只给签名。
DWORD MsgWaitForMultipleObjectsEx(DWORD nCount, const HANDLE* pHandles, DWORD dwMilliseconds,
                                  DWORD dwWakeMask, DWORD dwFlags);

// --- 查询（只声明）
BOOL GetCursorInfo(PCURSORINFO pci);
BOOL GetSystemPowerStatus(LPSYSTEM_POWER_STATUS lpSystemPowerStatus);
BOOL SetBrushOrgEx(HDC hdc, int x, int y, LPPOINT lppt);

// --- DPI
DPI_AWARENESS_CONTEXT GetThreadDpiAwarenessContext();
DPI_AWARENESS GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT value);

} // extern "C"

// ===========================================================================
// LONG / ULONG 的取值域必须与 Win32 一致
//
// 本层把 LONG 定成 std::int32_t（保住 RECT=16 字节、BITMAPINFOHEADER=40 字节），
// 但 <climits> 的 LONG_MIN / LONG_MAX 在 macOS 上来自 64 位的 long。于是
//   const RECT viewport{LONG_MIN / 2, LONG_MIN / 2, LONG_MAX / 2, LONG_MAX / 2};
// 在 Windows 上是 ±1.07e9（放得进 LONG），在 macOS 上变成 ±4.6e18（放不进）。
// 这属于"能编译就没问题、编译不过才发现"的隐性语义差异，必须按 Win32 覆写。
// ===========================================================================
#if defined(LONG_MIN)
#undef LONG_MIN
#endif
#if defined(LONG_MAX)
#undef LONG_MAX
#endif
#if defined(ULONG_MAX)
#undef ULONG_MAX
#endif
#define LONG_MIN (-2147483647L - 1)
#define LONG_MAX 2147483647L
#define ULONG_MAX 4294967295UL
