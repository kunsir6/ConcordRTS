// Win32 兼容层：小族收口（S9）
//
// 这一批的共同点是"数量少、看着杂"，但每一个都有"糊弄一下就绿"的诱人假实现。
// 本文件逐个记录为什么选了现在这条：
//
//   COM 初始化      CoInitializeEx / CoUninitialize
//   定时器分辨率    timeBeginPeriod / timeEndPeriod
//   MCI             mciSendStringW / mciGetDeviceIDW
//   MSVC CRT 的 _s  _putenv_s / _wgetenv_s / gmtime_s
//
// 三个"可以诚实返回成功"的和三个"必须诚实返回失败"的，理由完全不同——见各函数。

#include <windows.h>
#include <mmsystem.h>

#include "RuntimeWin32CompatInternal.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace {

// 只报一次：MCI 那几个函数在播放入口上，按帧重试就会把日志刷满。
void reportOnce(const char* function, const char* detail) {
    static bool reported = false;
    if (reported) return;
    reported = true;
    std::fprintf(stderr, "[compat/win32] %s: %s\n", function, detail);
}

// ---------------------------------------------------------------------------
// UTF-8 ↔ wchar_t（本平台的 wchar_t 是 32 位 UTF-32，不是 Windows 的 16 位 UTF-16）
// ---------------------------------------------------------------------------
// 环境变量名几乎是 ASCII，但值可能是任意路径。这里不做"够用就行"的窄宽强转：
// 逐码点编码，非法序列替换成 U+FFFD 而不是截断——截断会产生一个"看起来合法"
// 的路径，比报错难查得多。
//
// S9 时刻意没有复用 MultiByteToWideChar/WideCharToMultiByte：那两个当时还不存在，
// 自我调用就是循环依赖。S10 它们实现之后，这里改成了共用同一份内核（见下）。

// 编解码内核在 S10 抬成了全层唯一实现（RuntimeWin32CompatInternal.h）。
// 为什么改：S10 的 MultiByteToWideChar/WideCharToMultiByte 与本文件原本各有一份
// 实现，两份都会"看起来能用"，但非法输入的处置一旦分叉（一份替换 U+FFFD、
// 一份截断），同一条路径在不同调用点上表现就不同——而差异只在喂进非法字节时
// 才显形。现在两边共用 decodeUtf8 / decodeWide，分叉不可能发生。
std::string utf8FromWide(const wchar_t* text) {
    return concord_compat::utf8FromWideText(text);
}

std::wstring wideFromUtf8(const char* text) {
    return concord_compat::wideTextFromUtf8(text);
}

}  // namespace

extern "C" {

// ---------------------------------------------------------------------------
// COM 初始化
// ---------------------------------------------------------------------------
// macOS 上没有 COM，也就没有"套间"（apartment）这个概念。调用方真正依赖的契约
// 只有一条：**返回成功 ⇒ 必须在销毁时配对调用 CoUninitialize**。
//   * 返回 S_OK：uninitializeCom 置真，析构里调 CoUninitialize（本层空操作）——
//     契约成立，且没有任何后续代码会因为"套间状态"而走岔。
//   * 返回 RPC_E_CHANGED_MODE：调用方会**既不配对也不失败**继续往下跑。
//     表面上也能走通，但它宣称"这个线程已被初始化成另一种模式"——一句没发生过
//     的事。撒谎换来的等价行为不值得。
HRESULT CoInitializeEx(LPVOID pvReserved, DWORD dwCoInit) {
    (void)pvReserved;
    (void)dwCoInit;
    return S_OK;
}

void CoUninitialize() {
    // 与 CoInitializeEx 配对。本层没有需要拆的套间，空操作即正确。
}

// ---------------------------------------------------------------------------
// 定时器分辨率
// ---------------------------------------------------------------------------
// Windows 上这两个函数改的是**全局**定时器滴答（默认约 15.6ms），timeBeginPeriod(1)
// 把它降到 1ms；Sleep(1) 因此真的能睡 1ms。
//
// macOS 没有这个全局滴答：nanosleep/usleep 本身就是亚毫秒精度，调度器也不按
// 15.6ms 对齐。所以这里返回 TIMERR_NOERROR 说的是**结果状态**——"你要求的
// 1ms 粒度为真的可用"——而不是"我调了一个内核接口"。
//
// 唯一不成立的是 Windows 才有的副作用：别的进程也受影响。本项目没有任何代码
// 依赖跨进程的定时器精度，故不假装。
MMRESULT timeBeginPeriod(UINT uPeriod) {
    (void)uPeriod;
    return TIMERR_NOERROR;
}

MMRESULT timeEndPeriod(UINT uPeriod) {
    (void)uPeriod;
    return TIMERR_NOERROR;
}

// ---------------------------------------------------------------------------
// MCI（多媒体控制接口）
// ---------------------------------------------------------------------------
// 项目用 MCI 播菜单音乐与 EVA 语音。macOS 上没有 MCI，音频输出由 SDL3 后端
// （P4-01）兑现。
//
// 为什么必须返回错误而不是 0：
//   MCI 的调用方判据是 `error == 0` 与 `mciGetDeviceIDW(alias) != 0`。若这里
//   假装成功，RuntimeMenuSound 会认为"音乐在播"、RuntimeMciPathProducerSmoke
//   会认为"取到了播放位置"——而实际一个采样都没出声。返回
//   MCIERR_DEVICE_NOT_INSTALLED 让所有调用方一致地走"设备不可用"分支，这正是
//   设备真的不存在时 Windows 给出的答案。
MMRESULT mciSendStringW(LPCWSTR lpstrCommand, LPWSTR lpstrReturnString, UINT uReturnLength,
                        HWND hwndCallback) {
    (void)lpstrCommand;
    (void)hwndCallback;
    // 出错时清空返回缓冲：调用方常在错误码之外顺手读缓冲（例如把 status 的
    // 输出先当字符串解析），留着未定义内容是隐患。
    if (lpstrReturnString != nullptr && uReturnLength > 0) lpstrReturnString[0] = L'\0';
    reportOnce("mciSendStringW",
               "MCI is not available on this platform; audio output belongs to the SDL3 backend");
    return MCIERR_DEVICE_NOT_INSTALLED;
}

// 与 mciSendStringW 配套：0 表示"这个别名没有打开的设备"。所有调用点都用它做
// "我是否真的持有一个通道"的判据，返回 0 恰好是"没有"。
MCIDEVICEID mciGetDeviceIDW(LPCWSTR pszDevice) {
    (void)pszDevice;
    return 0;
}

}  // extern "C"

// ---------------------------------------------------------------------------
// MSVC CRT 的 _s 变体
// ---------------------------------------------------------------------------
// 这三个不是 Win32 API，是 MSVC CRT 的"安全版"标准函数。它们有自己的语义细节，
// 而且细节会改变程序行为——所以不能用 setenv/gmtime_r 直接顶替。

// _putenv_s(name, "") = **删除**该变量，不是设成空字符串。
//
// 这条差别是本文件里最容易踩的一处：项目用
//     std::getenv("CONCORD_DEBUG_TURRET_FACING")
// 判断"是否开启诊断"，关闭时写的是 _putenv_s(name, "")。若把它实现成
// setenv(name, "", 1)，getenv 会返回一个非空的 ""，于是 `if (const char* v = ...)`
// 走进"已开启"分支——诊断图形永远打开，而所有断言照旧通过。
errno_t _putenv_s(const char* varname, const char* value_string) {
    if (varname == nullptr || varname[0] == '\0') return EINVAL;
    if (value_string == nullptr) return EINVAL;
    if (value_string[0] == '\0') {
        ::unsetenv(varname);
        return 0;
    }
    if (::setenv(varname, value_string, 1) != 0) return errno == 0 ? ENOMEM : errno;
    return 0;
}

// _wgetenv_s 的三种返回形态必须都成立，否则调用方会走进错的分支：
//   * 变量存在 → 0，*pReturnValue = 所需元素数（含结尾 NUL）；
//   * 变量不存在 → 0，*pReturnValue = 0，缓冲区置为空串（**不是**错误）；
//   * 缓冲区不够 → ERANGE，*pReturnValue 仍给出所需大小，调用方据此扩容重试。
// 第一和第三种只差返回值，`count` 的语义完全相同——这正是"两段式调用"能工作的前提：
// 先 buffer=nullptr, 0 查长度，再按长度分配重读。
errno_t _wgetenv_s(std::size_t* pReturnValue, wchar_t* buffer, std::size_t numberOfElements,
                   const wchar_t* varname) {
    if (pReturnValue == nullptr) return EINVAL;
    *pReturnValue = 0;
    if (buffer != nullptr && numberOfElements > 0) buffer[0] = L'\0';
    if (varname == nullptr) return EINVAL;
    const std::string name = utf8FromWide(varname);
    if (name.empty()) return EINVAL;
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) return 0;  // 未定义：不是错误
    const std::wstring wide = wideFromUtf8(value);
    const std::size_t required = wide.size() + 1;
    *pReturnValue = required;
    if (buffer == nullptr || numberOfElements == 0) return 0;  // 仅查询长度
    if (required > numberOfElements) return ERANGE;
    std::memcpy(buffer, wide.c_str(), required * sizeof(wchar_t));
    return 0;
}

// getenv_s 是 _wgetenv_s 的窄字符版，返回形态完全相同（存在→0 且给出含 NUL 的元素数；
// 不存在→0 且计 0；缓冲不够→ERANGE）。唯一差别是不需要做窄↔宽转换：varname 与值
// 本来就是窄字符，直接走 std::getenv，逐码点语义与宽版在 ASCII 变量名上逐位一致。
errno_t getenv_s(std::size_t* pReturnValue, char* buffer, std::size_t numberOfElements,
                 const char* varname) {
    if (pReturnValue == nullptr) return EINVAL;
    *pReturnValue = 0;
    if (buffer != nullptr && numberOfElements > 0) buffer[0] = '\0';
    if (varname == nullptr || varname[0] == '\0') return EINVAL;
    const char* value = std::getenv(varname);
    if (value == nullptr) return 0;  // 未定义：不是错误
    const std::size_t required = std::strlen(value) + 1;
    *pReturnValue = required;
    if (buffer == nullptr || numberOfElements == 0) return 0;  // 仅查询长度
    if (required > numberOfElements) return ERANGE;
    std::memcpy(buffer, value, required);
    return 0;
}

// gmtime_s 的参数顺序是 (tm*, time_t*)，与 POSIX 的 gmtime_r(time_t*, tm*) **相反**。
// 写反了照样能编译（两个指针类型不同，但都能隐式转 void* 的写法会放过），
// 运行期则是往一个 time_t 大小的位置写 struct tm —— 现场崩或静默错值。
errno_t gmtime_s(struct tm* tmDest, const std::time_t* sourceTime) {
    if (tmDest == nullptr || sourceTime == nullptr) return EINVAL;
    std::tm scratch{};
    if (::gmtime_r(sourceTime, &scratch) == nullptr) return EINVAL;
    *tmDest = scratch;
    return 0;
}
