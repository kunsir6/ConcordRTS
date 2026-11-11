// Win32 兼容层：进程 / 线程 / 运行时 / 编码 / 时间 / 环境变量 族（S10 上半）
//
// 这一批符号的难点不在"实现"，而在"哪些必须说谎、哪些必须诚实"：
//
//   必须诚实（返回失败）：
//     GetModuleHandleW(具名) / GetProcAddress —— macOS 上没有 DLL 与导出表。
//       项目用它们加载 binkw32.dll。若假装成功，GetProcAddress 会返回空指针，
//       调用方把 NULL 当函数指针调用 → 立刻崩；若返回一个"看起来有效"的假地址，
//       崩得更晚、更难查。返回 NULL + ERROR_MOD_NOT_FOUND 让整条 Bink 路径
//       一致地走"没有视频库"分支——那正是这台机器上的真实情况。
//     GetEnvironmentVariableW(不存在的名字) —— 必须返回 0。项目大量用
//       `GetEnvironmentVariableW(L"FLAG", buf, 2) == 1` 做开关判据；假装"空值
//       也算有"会让全部诊断开关默认打开。
//     MultiByteToWideChar(非法字节, MB_ERR_INVALID_CHARS) —— 必须失败。
//       调用点 toWide() 就是靠这次失败判定"这不是 UTF-8"，然后才回退 CP_ACP。
//       若这里悄悄替换成 U+FFFD，回退分支永远走不到，而结果"看起来没坏"。
//
//   必须等价（返回成功）：
//     GetCurrentProcess / GetCurrentProcessId / GetCurrentThreadId / Sleep /
//     GetTickCount64 / QueryPerformanceCounter / GetSystemTime —— 这些没有
//     "平台做不到"的成分，只有"用哪个内核接口"的差别。
//
// 两个刻意偏离 Win32 的地方，都写在对应函数里：
//   1. GetCurrentProcess() 返回令牌指针而不是 (HANDLE)-1。Windows 的伪句柄就是
//      -1，而那正好等于 INVALID_HANDLE_VALUE——本项目多处拿这个值判"打开失败"。
//   2. GetCurrentThreadId() 用进程内自增计数而不是内核线程 id。Windows 的
//      ThreadId 系统范围内唯一、线程退出后可复用；本层保证进程内唯一且永不复用。
//      调用点只做"是不是同一个线程"的比较，两种语义都满足。

#include <windows.h>

#include "RuntimeWin32CompatInternal.h"

#include <crt_externs.h>
#include <mach/mach_time.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace concord_compat {

// ---------------------------------------------------------------------------
// LastError
// ---------------------------------------------------------------------------
namespace {
thread_local DWORD t_lastError = 0;
}  // namespace

DWORD lastError() noexcept { return t_lastError; }
void setLastError(DWORD code) noexcept { t_lastError = code; }

void setLastErrorFromErrno(int code) noexcept {
    switch (code) {
        case 0:            setLastError(0); break;
        case EEXIST:       setLastError(ERROR_FILE_EXISTS); break;
        case ENOENT:       setLastError(ERROR_FILE_NOT_FOUND); break;
        case ENOTDIR:      setLastError(ERROR_PATH_NOT_FOUND); break;
        case EACCES:
        case EPERM:
        case EISDIR:       setLastError(ERROR_ACCESS_DENIED); break;
        case ENOSPC:       setLastError(ERROR_DISK_FULL); break;
        case EMFILE:
        case ENFILE:       setLastError(ERROR_TOO_MANY_OPEN_FILES); break;
        case ENAMETOOLONG: setLastError(ERROR_FILENAME_EXCED_RANGE); break;
        case EINVAL:       setLastError(ERROR_INVALID_PARAMETER); break;
        case ENOMEM:       setLastError(ERROR_NOT_ENOUGH_MEMORY); break;
        // 未知错误说成 GEN_FAILURE 而不是 ACCESS_DENIED：后者会把排查方向
        // 引到权限上，而真实原因可能是别的一切。
        default:           setLastError(ERROR_GEN_FAILURE); break;
    }
}

// ---------------------------------------------------------------------------
// 句柄表
// ---------------------------------------------------------------------------
namespace {

struct HandleEntry {
    HandleDestructor destroy = nullptr;
    const char* kind = "unknown";
};

// 永不析构的锁与表。
//
// 为什么要故意泄漏这两个对象（而不是老老实实放一个 static）：
// 句柄表会被**静态对象**在析构期访问。实测崩溃：产品的 `prewarmedMenuState`
// 是静态 unique_ptr，它的析构里调 `FindCloseChangeNotification` →
// `closeRegisteredHandle` → 锁 handleMutex()；而本 TU 与产品 TU 的静态析构顺序
// 不确定，锁可能已经先被销毁，于是抛
//     libc++abi: terminating due to uncaught exception of type
//     std::__1::system_error: mutex lock failed: Invalid argument
// 进程以 SIGABRT 退出——所有断言都过了，挂在进程收尾，最难联想到根因。
//
// 句柄表本来就该活到进程结束（Windows 的 HANDLE 表同理），所以让它**不析构**
// 是正确语义，不是为了绕开崩溃的权宜之计。
std::mutex& handleMutex() {
    static std::mutex* value = new std::mutex();
    return *value;
}

std::unordered_map<void*, HandleEntry>& handleTable() {
    static std::unordered_map<void*, HandleEntry>* value =
        new std::unordered_map<void*, HandleEntry>();
    return *value;
}

}  // namespace

bool registerHandle(void* handle, HandleDestructor destroy, const char* kind) noexcept {
    if (handle == nullptr) return false;
    std::lock_guard<std::mutex> lock(handleMutex());
    HandleEntry entry;
    entry.destroy = destroy;
    entry.kind = kind;
    // emplace 而不是 operator[]：重复登记同一个地址说明上层生命周期管理已经错了，
    // 覆盖会静默丢掉前一个析构函数。
    return handleTable().emplace(handle, entry).second;
}

bool closeRegisteredHandle(void* handle) noexcept {
    if (handle == nullptr) return false;
    HandleEntry entry;
    {
        std::lock_guard<std::mutex> lock(handleMutex());
        auto found = handleTable().find(handle);
        if (found == handleTable().end()) return false;
        entry = found->second;
        handleTable().erase(found);
    }
    if (entry.destroy != nullptr) entry.destroy(handle);
    return true;
}

std::size_t liveHandleCount() noexcept {
    std::lock_guard<std::mutex> lock(handleMutex());
    return handleTable().size();
}

bool isRegisteredHandle(void* handle, const char* kind) noexcept {
    if (handle == nullptr) return false;
    std::lock_guard<std::mutex> lock(handleMutex());
    auto found = handleTable().find(handle);
    if (found == handleTable().end()) return false;
    return std::strcmp(found->second.kind, kind) == 0;
}

// ---------------------------------------------------------------------------
// 编解码内核
// ---------------------------------------------------------------------------
bool DecodedText::anyInvalid() const noexcept {
    for (std::uint8_t flag : invalid) {
        if (flag != 0) return true;
    }
    return false;
}

void appendUtf8(std::string& out, std::uint32_t code) {
    if (code <= 0x7Fu) {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (code >> 6)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else if (code <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (code >> 12)));
        out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (code >> 18)));
        out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    }
}

void appendWide(std::wstring& out, std::uint32_t code) {
    // wchar_t 在这里是 32 位，所以"一个码点 = 一个 wchar_t"。Windows 上 U+1F600
    // 是两个代理项 wchar_t，这里是一个——凡是按 wchar_t 个数做判据的调用点，
    // 两边的数字本来就不同。本层保证码点序列一致，不保证 wchar_t 个数一致。
    out.push_back(static_cast<wchar_t>(code));
}

DecodedText decodeUtf8(const char* bytes, std::size_t count) {
    DecodedText out;
    out.codes.reserve(count);
    out.invalid.reserve(count);
    if (bytes == nullptr) return out;
    const auto* data = reinterpret_cast<const unsigned char*>(bytes);
    std::size_t index = 0;
    const auto push = [&out](std::uint32_t code, bool bad) {
        out.codes.push_back(code);
        out.invalid.push_back(bad ? 1u : 0u);
    };
    while (index < count) {
        const unsigned char lead = data[index];
        if (lead < 0x80u) {
            push(lead, false);
            ++index;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t code = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xE0u) == 0xC0u) {
            extra = 1;
            code = lead & 0x1Fu;
            minimum = 0x80u;
        } else if ((lead & 0xF0u) == 0xE0u) {
            extra = 2;
            code = lead & 0x0Fu;
            minimum = 0x800u;
        } else if ((lead & 0xF8u) == 0xF0u) {
            extra = 3;
            code = lead & 0x07u;
            minimum = 0x10000u;
        } else {
            // 0x80..0xBF 的孤立后续字节，或 0xF8..0xFF 的非法引导字节。
            push(0xFFFDu, true);
            ++index;
            continue;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset <= extra; ++offset) {
            if (index + offset >= count) {
                valid = false;
                break;
            }
            const unsigned char next = data[index + offset];
            if ((next & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
            code = (code << 6) | (next & 0x3Fu);
        }
        if (!valid) {
            // 截断：只吃掉引导字节。一次吃掉整段会把后面其实合法的字节也丢掉。
            push(0xFFFDu, true);
            ++index;
            continue;
        }
        const bool overlong = code < minimum;
        const bool surrogate = code >= 0xD800u && code <= 0xDFFFu;
        const bool tooLarge = code > 0x10FFFFu;
        if (overlong || surrogate || tooLarge) {
            push(0xFFFDu, true);
            index += extra + 1;
            continue;
        }
        push(code, false);
        index += extra + 1;
    }
    return out;
}

DecodedText decodeWide(const wchar_t* units, std::size_t count) {
    DecodedText out;
    out.codes.reserve(count);
    out.invalid.reserve(count);
    if (units == nullptr) return out;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t code = static_cast<std::uint32_t>(units[index]);
        // 超范围值与代理区码点都不是合法标量。这里**不**把"两个相邻代理项拼回
        // 一个码点"：本平台合法的宽字符串里根本不会有代理项，出现就说明来源是
        // UTF-16 数据——替调用方猜意图比直接判定非法更危险。
        if (code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
            out.codes.push_back(0xFFFDu);
            out.invalid.push_back(1u);
            continue;
        }
        out.codes.push_back(code);
        out.invalid.push_back(0u);
    }
    return out;
}

std::string utf8FromWideText(const wchar_t* text) {
    if (text == nullptr) return {};
    const DecodedText decoded = decodeWide(text, std::wcslen(text));
    std::string out;
    out.reserve(decoded.codes.size());
    for (std::uint32_t code : decoded.codes) appendUtf8(out, code);
    return out;
}

std::wstring wideTextFromUtf8(const char* text) {
    if (text == nullptr) return {};
    const DecodedText decoded = decodeUtf8(text, std::strlen(text));
    std::wstring out;
    out.reserve(decoded.codes.size());
    for (std::uint32_t code : decoded.codes) appendWide(out, code);
    return out;
}

}  // namespace concord_compat

// ===========================================================================
// Win32 入口
// ===========================================================================

namespace {

// 伪句柄用独立令牌，不复用 (HANDLE)-1。
// Windows 的 GetCurrentProcess() 恰好返回 (HANDLE)-1，而同一个值就是
// INVALID_HANDLE_VALUE；本项目多处拿它判"打开失败"。
char g_processToken = 0;
char g_imageToken = 0;

std::atomic<DWORD> g_nextThreadId{0};
thread_local DWORD t_threadId = 0;

// mach_timebase_info 只需查一次；函数内 static 的初始化自 C++11 起线程安全。
double nanosecondsPerTick() {
    static const double value = [] {
        mach_timebase_info_data_t info{};
        if (mach_timebase_info(&info) != 0 || info.denom == 0) return 1.0;
        return static_cast<double>(info.numer) / static_cast<double>(info.denom);
    }();
    return value;
}

void reportOnce(const char* function, const char* detail) {
    static bool reported = false;
    if (reported) return;
    reported = true;
    std::fprintf(stderr, "[compat/win32] %s: %s\n", function, detail);
}

const char* lookupEnvironment(const std::string& name) {
    if (const char* exact = std::getenv(name.c_str())) return exact;
    // Windows 的环境变量名**大小写不敏感**，POSIX 的 getenv 大小写敏感。少这一轮，
    // 同一份启动脚本里写 `concord_x=1` 在 Windows 上生效、在这里静默无效——
    // "开关按了没反应"是最难查的一类差异。
    char** environment = *_NSGetEnviron();
    for (char** entry = environment; entry != nullptr && *entry != nullptr; ++entry) {
        const char* equals = std::strchr(*entry, '=');
        if (equals == nullptr) continue;
        const std::size_t length = static_cast<std::size_t>(equals - *entry);
        if (length != name.size()) continue;
        if (::strncasecmp(*entry, name.c_str(), length) == 0) return equals + 1;
    }
    return nullptr;
}

// 本平台没有码页表。CP_UTF8 与 CP_ACP/CP_OEMCP 都按 UTF-8 处理：macOS 的
// "ANSI 代码页"就是 UTF-8，这是唯一诚实的映射。
//
// 例外是 CP1252：零售 ini（evamd.ini 的 Text=、rules.ini 的字符串表）是
// Windows-1252，里面的 0x85/0x91/0x92/0x93/0x94 是省略号与引号。把它们当
// UTF-8 解会失败，逐字节 widen 又会变成 U+0085 这类控制字符——字幕里看起来
// 就是"乱码方块"。所以这一张表必须真实现，而不是继续拒绝。
// 其余码页仍然一律拒绝：拒绝比"看起来合理的错字"容易查。
const wchar_t* cp1252HighTable() {
    static const wchar_t table[32] = {
        0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
        0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178
    };
    return table;
}

// CP1252 → Unicode：0x00-0x7F 与 0xA0-0xFF 是恒等映射（Latin-1），
// 0x80-0x9F 才需要查表。Windows 对表里 5 个未定义位置（81/8D/8F/90/9D）
// 也返回替换字符，这里保持一致。
wchar_t cp1252ToUnicode(unsigned char byte) {
    if (byte < 0x80U || byte >= 0xA0U) return static_cast<wchar_t>(byte);
    return cp1252HighTable()[byte - 0x80U];
}

// Unicode → CP1252：表内可逆；不可逆时给 '?' 并报 "used default char"。
bool unicodeToCp1252(wchar_t code, unsigned char& out) {
    if (code < 0x80U) { out = static_cast<unsigned char>(code); return true; }
    if (code >= 0xA0U && code <= 0xFFU) {
        out = static_cast<unsigned char>(code);
        return true;
    }
    const wchar_t* table = cp1252HighTable();
    for (int index = 0; index < 32; ++index) {
        if (table[index] == code && table[index] != 0xFFFD) {
            out = static_cast<unsigned char>(0x80 + index);
            return true;
        }
    }
    out = '?';
    return false;
}

bool codePageSupported(UINT codePage) {
    return codePage == CP_UTF8 || codePage == CP_ACP || codePage == CP_OEMCP
        || codePage == 1252U;
}


}  // namespace

extern "C" {

DWORD GetLastError() { return concord_compat::lastError(); }
void SetLastError(DWORD dwErrCode) { concord_compat::setLastError(dwErrCode); }

// ---------------------------------------------------------------------------
// CloseHandle
// ---------------------------------------------------------------------------
// 整个兼容层最容易造成"疑难崩溃"的一个函数：它的入参是 `void*`，而本项目里
// 同一时刻存在三种东西会传进来——文件句柄、GDI 对象/DC 指针、以及（S7/S8 之后）
// 窗口与事件句柄。对 GDI 指针执行 `close((int)(intptr_t)ptr)` 的后果是关掉一个
// 毫不相干的 fd；对野指针先解引用 magic 的后果是立刻段错误。
//
// 所以判据只能是"登记表里有它吗"：查表只比较指针值，不碰它指向的内存。
// 不在表里 → FALSE + ERROR_INVALID_HANDLE，且**什么都不做**。这与 Windows 对
// 无效句柄的回答一致，也与"GDI 句柄要用 DeleteObject 关"一致。
BOOL CloseHandle(HANDLE hObject) {
    if (concord_compat::closeRegisteredHandle(hObject)) return TRUE;
    concord_compat::setLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

// ---------------------------------------------------------------------------
// 进程 / 线程
// ---------------------------------------------------------------------------
HANDLE GetCurrentProcess() { return &g_processToken; }

DWORD GetCurrentProcessId() { return static_cast<DWORD>(::getpid()); }

DWORD GetCurrentThreadId() {
    if (t_threadId == 0) {
        // 从 1 起分配，0 保留给"还没分配"。
        t_threadId = g_nextThreadId.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    return t_threadId;
}

// ---------------------------------------------------------------------------
// 模块 / 导出表
// ---------------------------------------------------------------------------
HINSTANCE GetModuleHandleW(LPCWSTR lpModuleName) {
    // 空名 = 询问"本进程映像"，这一条在任何平台上都成立。
    if (lpModuleName == nullptr || lpModuleName[0] == L'\0') {
        return reinterpret_cast<HINSTANCE>(&g_imageToken);
    }
    // 具名模块：macOS 没有 DLL，也没有"按名字查已加载模块"这回事。诚实失败。
    concord_compat::setLastError(ERROR_MOD_NOT_FOUND);
    return nullptr;
}

void* GetProcAddress(HMODULE hModule, const char* lpProcName) {
    (void)hModule;
    (void)lpProcName;
    // 没有导出表可查。返回非空会让调用方把随机地址当函数调用。
    concord_compat::setLastError(ERROR_PROC_NOT_FOUND);
    return nullptr;
}

// ---------------------------------------------------------------------------
// 时间
// ---------------------------------------------------------------------------
void Sleep(DWORD dwMilliseconds) {
    if (dwMilliseconds == 0) {
        // Windows 的 Sleep(0) 是"让出剩余时间片"，不是"睡 0 毫秒"。
        std::this_thread::yield();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(dwMilliseconds));
}

// GetTickCount64 与 QueryPerformanceCounter 用**两个不同**的内核时钟，这不是随手
// 选的：
//   · GetTickCount64 在 Windows 上是"开机以来的毫秒数"，**包含系统睡眠**时段。
//     本项目用它做超时/截止判据（`deadline = GetTickCount64() + N`）。若底层用
//     mach_absolute_time（不含睡眠），合盖一夜再打开，所有截止时间都"还没到"。
//     mach_continuous_time 才是含睡眠的那一个。
//   · QPC 在 Windows 上是不变 TSC，精度纳秒级、不保证跨睡眠累加。
//     mach_absolute_time 正好对应这个语义。
ULONGLONG GetTickCount64() {
    const double milliseconds =
        static_cast<double>(mach_continuous_time()) * nanosecondsPerTick() / 1.0e6;
    return static_cast<ULONGLONG>(milliseconds);
}

BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount) {
    if (lpPerformanceCount == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // 计数单位直接取纳秒，等价于"频率 = 1e9"。Windows 的 QPC 基准是任意值，
    // 调用方只做差值，故基准无所谓；但**必须单调不减**。
    lpPerformanceCount->QuadPart =
        static_cast<LONGLONG>(static_cast<double>(mach_absolute_time()) * nanosecondsPerTick());
    return TRUE;
}

void GetSystemTime(SYSTEMTIME* lpSystemTime) {
    if (lpSystemTime == nullptr) return;  // Win32 无返回值，也没有错误码可设
    struct timespec now {};
    if (::clock_gettime(CLOCK_REALTIME, &now) != 0) {
        std::memset(lpSystemTime, 0, sizeof(*lpSystemTime));
        return;
    }
    std::time_t seconds = now.tv_sec;
    std::tm utc {};
    if (::gmtime_r(&seconds, &utc) == nullptr) {
        std::memset(lpSystemTime, 0, sizeof(*lpSystemTime));
        return;
    }
    lpSystemTime->wYear = static_cast<WORD>(utc.tm_year + 1900);
    lpSystemTime->wMonth = static_cast<WORD>(utc.tm_mon + 1);
    lpSystemTime->wDayOfWeek = static_cast<WORD>(utc.tm_wday);  // 0 = 周日，与 Win32 相同
    lpSystemTime->wDay = static_cast<WORD>(utc.tm_mday);
    lpSystemTime->wHour = static_cast<WORD>(utc.tm_hour);
    lpSystemTime->wMinute = static_cast<WORD>(utc.tm_min);
    lpSystemTime->wSecond = static_cast<WORD>(utc.tm_sec);
    lpSystemTime->wMilliseconds = static_cast<WORD>(now.tv_nsec / 1000000L);
}

// ---------------------------------------------------------------------------
// 环境变量
// ---------------------------------------------------------------------------
DWORD GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize) {
    if (lpName == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const char* value = lookupEnvironment(concord_compat::utf8FromWideText(lpName));
    if (value == nullptr) {
        concord_compat::setLastError(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    const std::wstring wide = concord_compat::wideTextFromUtf8(value);
    const DWORD required = static_cast<DWORD>(wide.size() + 1);  // 含结尾 NUL
    if (lpBuffer == nullptr || nSize == 0) return required;
    if (nSize < required) {
        // Win32 在缓冲不够时**不写任何内容**，只回所需大小。写进截断值会让
        // "先查长度再读"的两段式调用拿到半截字符串，而返回值看起来正常。
        return required;
    }
    std::memcpy(lpBuffer, wide.c_str(), required * sizeof(wchar_t));
    return static_cast<DWORD>(wide.size());  // 不含 NUL
}

DWORD GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize) {
    if (lpName == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const char* value = lookupEnvironment(std::string(lpName));
    if (value == nullptr) {
        concord_compat::setLastError(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    const std::size_t length = std::strlen(value);
    const DWORD required = static_cast<DWORD>(length + 1);
    if (lpBuffer == nullptr || nSize == 0) return required;
    if (nSize < required) return required;
    std::memcpy(lpBuffer, value, length + 1);
    return static_cast<DWORD>(length);
}

// ---------------------------------------------------------------------------
// 多字节 ↔ 宽字符
// ---------------------------------------------------------------------------
int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr, int cbMultiByte,
                        LPWSTR lpWideCharStr, int cchWideChar) {
    if (!codePageSupported(CodePage)) {
        reportOnce("MultiByteToWideChar",
                   "only UTF-8 is supported here (CP_ACP/CP_OEMCP map to UTF-8 on this platform)");
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (lpMultiByteStr == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (cbMultiByte == 0) return 0;  // 空输入：0 个字符，不是错误

    // 负长度 = 以 NUL 结尾，且 NUL **计入**处理范围（Windows 语义）。
    const std::size_t count = (cbMultiByte < 0)
        ? std::strlen(lpMultiByteStr) + 1
        : static_cast<std::size_t>(cbMultiByte);

    if (CodePage == 1252U) {
        // 单字节码页：一个字节一个字符，没有"非法序列"这回事，
        // 所以 MB_ERR_INVALID_CHARS 在这里永远不报错（与 Windows 一致）。
        const int needed = static_cast<int>(count);
        if (cchWideChar == 0) return needed;
        if (cchWideChar < needed) {
            concord_compat::setLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        for (std::size_t index = 0; index < count; ++index) {
            lpWideCharStr[index] = cp1252ToUnicode(
                static_cast<unsigned char>(lpMultiByteStr[index])
            );
        }
        return needed;
    }

    const concord_compat::DecodedText decoded = concord_compat::decodeUtf8(lpMultiByteStr, count);
    if ((dwFlags & MB_ERR_INVALID_CHARS) != 0 && decoded.anyInvalid()) {
        // 严格模式必须失败：调用点 toWide() 正是靠这次失败判定"不是 UTF-8"。
        concord_compat::setLastError(ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    const int needed = static_cast<int>(decoded.codes.size());
    if (cchWideChar == 0) return needed;
    if (cchWideChar < needed) {
        concord_compat::setLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (int index = 0; index < needed; ++index) {
        lpWideCharStr[index] = static_cast<wchar_t>(decoded.codes[static_cast<std::size_t>(index)]);
    }
    return needed;
}

int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr, int cchWideChar,
                        LPSTR lpMultiByteStr, int cbMultiByte, LPCSTR lpDefaultChar,
                        LPBOOL lpUsedDefaultChar) {
    if (!codePageSupported(CodePage)) {
        reportOnce("WideCharToMultiByte",
                   "only UTF-8 is supported here (CP_ACP/CP_OEMCP map to UTF-8 on this platform)");
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (lpWideCharStr == nullptr) {
        concord_compat::setLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (lpUsedDefaultChar != nullptr) *lpUsedDefaultChar = FALSE;
    if (cchWideChar == 0) return 0;

    const std::size_t count = (cchWideChar < 0)
        ? std::wcslen(lpWideCharStr) + 1
        : static_cast<std::size_t>(cchWideChar);

    if (CodePage == 1252U) {
        // 反向：不可映射的宽字符给 lpDefaultChar（默认 '?'），并置 usedDefault。
        // 严格模式（WC_ERR_INVALID_CHARS）下不可映射必须失败，否则调用点
        // 会拿着带 '?' 的字节去写文件。
        const char substitute = (lpDefaultChar != nullptr) ? *lpDefaultChar : '?';
        std::string encoded;
        encoded.reserve(count);
        bool usedDefault = false;
        for (std::size_t index = 0; index < count; ++index) {
            unsigned char byte = 0;
            if (unicodeToCp1252(lpWideCharStr[index], byte)) {
                encoded.push_back(static_cast<char>(byte));
            } else {
                encoded.push_back(substitute);
                usedDefault = true;
            }
        }
        if (usedDefault && (dwFlags & WC_ERR_INVALID_CHARS) != 0) {
            concord_compat::setLastError(ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        if (lpUsedDefaultChar != nullptr) *lpUsedDefaultChar = usedDefault ? TRUE : FALSE;
        const int needed = static_cast<int>(encoded.size());
        if (cbMultiByte == 0) return needed;
        if (cbMultiByte < needed) {
            concord_compat::setLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (needed > 0) std::memcpy(lpMultiByteStr, encoded.data(), static_cast<std::size_t>(needed));
        return needed;
    }

    const concord_compat::DecodedText decoded = concord_compat::decodeWide(lpWideCharStr, count);
    const bool strict = (dwFlags & WC_ERR_INVALID_CHARS) != 0;
    if (strict && decoded.anyInvalid()) {
        concord_compat::setLastError(ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }

    const char substitute = (lpDefaultChar != nullptr) ? *lpDefaultChar : '?';
    std::string encoded;
    encoded.reserve(decoded.codes.size());
    bool usedDefault = false;
    for (std::size_t index = 0; index < decoded.codes.size(); ++index) {
        if (decoded.invalid[index] != 0) {
            encoded.push_back(substitute);
            usedDefault = true;
            continue;
        }
        concord_compat::appendUtf8(encoded, decoded.codes[index]);
    }
    if (lpUsedDefaultChar != nullptr) *lpUsedDefaultChar = usedDefault ? TRUE : FALSE;

    const int needed = static_cast<int>(encoded.size());
    if (cbMultiByte == 0) return needed;
    if (cbMultiByte < needed) {
        concord_compat::setLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (needed > 0) std::memcpy(lpMultiByteStr, encoded.data(), static_cast<std::size_t>(needed));
    return needed;
}

}  // extern "C"
