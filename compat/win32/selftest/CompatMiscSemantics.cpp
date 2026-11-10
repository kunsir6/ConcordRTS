// compat/win32 自测 · 小族收口（S9）
//
// 重点不是"函数能调通"，而是三处**换成表层等价物就会静默改变行为**的地方：
//   1. _putenv_s(name, "") 必须删除变量（不是设成空串）——否则 getenv 返回非空
//      的 ""，所有"是否开启诊断"的判据都会翻转；
//   2. _wgetenv_s 的两段式调用（先查长度再读）必须两次给出同一个 count；
//   3. gmtime_s 的参数顺序与 gmtime_r 相反，写反了是静默错值而不是编译错误。
//
// 构建与运行：
//   c++ -std=c++20 -I compat/win32/include \
//       compat/win32/src/RuntimeWin32CompatMisc.cpp \
//       compat/win32/selftest/CompatMiscSemantics.cpp -o /tmp/ccmisc && /tmp/ccmisc

#include <windows.h>
#include <mmsystem.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
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

void checkEqual(long long expected, long long actual, const char* description) {
    ++gChecks;
    if (expected != actual) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected %lld, got %lld)\n", description, expected, actual);
    }
}

const char* const kName = "CONCORD_COMPAT_SELFTEST_FLAG";

// ---------------------------------------------------------------------------
// 1. _putenv_s 的"空串 = 删除"语义
// ---------------------------------------------------------------------------
void testPutenvRemoves() {
    std::printf("[1] _putenv_s 的删除语义\n");
    ::unsetenv(kName);

    checkEqual(0, _putenv_s(kName, "1"), "_putenv_s 设置成功");
    const char* value = std::getenv(kName);
    check(value != nullptr, "设置后 getenv 非空");
    check(value != nullptr && std::strcmp(value, "1") == 0, "设置后 getenv == \"1\"");

    checkEqual(0, _putenv_s(kName, ""), "_putenv_s(name, \"\") 返回成功");
    // 这一条是整个 S9 里最关键的断言：换成了 setenv(name, "", 1) 的话，
    // 下面 getenv 会返回 ""——非空，于是"是否开启"的判据全部翻转。
    check(std::getenv(kName) == nullptr,
          "空串表示**删除**变量：getenv 必须返回 nullptr，而不是指向 \"\" 的指针");

    checkEqual(0, _putenv_s(kName, ""), "对不存在的变量再删一次仍返回成功");
    checkEqual(EINVAL, _putenv_s(nullptr, "x"), "name 为空返回 EINVAL");
    checkEqual(EINVAL, _putenv_s(kName, nullptr), "value 为空指针返回 EINVAL");
    checkEqual(EINVAL, _putenv_s("", "x"), "name 为空串返回 EINVAL");
}

// ---------------------------------------------------------------------------
// 2. _wgetenv_s 的两段式读取
// ---------------------------------------------------------------------------
void testWgetenv() {
    std::printf("[2] _wgetenv_s 两段式读取\n");
    const std::wstring name = L"CONCORD_COMPAT_SELFTEST_WIDE";

    // 未定义时：返回 0（不是错误），count == 0。
    std::size_t count = 12345;
    checkEqual(0, _wgetenv_s(&count, nullptr, 0, name.c_str()), "未定义变量返回 0");
    checkEqual(0, static_cast<long long>(count), "未定义变量时 count == 0");
    std::wstring missing;
    // 走窄接口设置一个含非 ASCII 的值，验证 UTF-8 ↔ wchar_t 往返。
    const std::string utf8Value = "路径/\xE4\xB8\xAD\xE6\x96\x87/emoji-\xF0\x9F\x8E\xAE";
    ::unsetenv("CONCORD_COMPAT_SELFTEST_WIDE");
    checkEqual(0, _putenv_s("CONCORD_COMPAT_SELFTEST_WIDE", utf8Value.c_str()), "写入含非 ASCII 的值");

    // 第一段：只查长度。
    count = 0;
    checkEqual(0, _wgetenv_s(&count, nullptr, 0, name.c_str()), "查询长度返回 0");
    const std::size_t required = count;
    check(required > 1, "查询到的长度 > 1");

    // 第二段：按长度分配再读。
    std::vector<wchar_t> buffer(required, L'\0');
    std::size_t readBack = 0;
    checkEqual(0, _wgetenv_s(&readBack, buffer.data(), buffer.size(), name.c_str()),
               "按查询到的长度读取成功");
    checkEqual(static_cast<long long>(required), static_cast<long long>(readBack),
               "两段式调用两次给出同一个 count");

    // 逐码点比对：非 ASCII 必须原样回来，不能是问号或截断。
    const std::wstring expected = L"路径/\u4E2D\u6587/emoji-\U0001F3AE";
    const std::wstring actual(buffer.data());
    ++gChecks;
    if (actual != expected) {
        ++gFailures;
        std::printf("  FAIL  非 ASCII 往返不一致（长度 %zu vs %zu）\n", actual.size(), expected.size());
    }

    // 缓冲区太小：返回 ERANGE，但仍要给出所需大小，调用方才能扩容重试。
    std::vector<wchar_t> small(required > 2 ? required - 1 : 1, L'\0');
    std::size_t tooSmall = 0;
    checkEqual(ERANGE, _wgetenv_s(&tooSmall, small.data(), small.size(), name.c_str()),
               "缓冲区不足返回 ERANGE");
    checkEqual(static_cast<long long>(required), static_cast<long long>(tooSmall),
               "ERANGE 时仍给出所需大小");
    checkEqual(L'\0', small[0], "ERANGE 时缓冲区被清空而不是留下半截内容");

    checkEqual(EINVAL, _wgetenv_s(nullptr, nullptr, 0, name.c_str()), "pReturnValue 为空返回 EINVAL");
    checkEqual(EINVAL, _wgetenv_s(&count, nullptr, 0, nullptr), "varname 为空返回 EINVAL");
    ::unsetenv("CONCORD_COMPAT_SELFTEST_WIDE");
}

// getenv_s 是 _wgetenv_s 的窄字符版，返回形态必须逐条一致。这里不重复 UTF-8↔wchar_t
// 往返（窄版本就是窄字符），只验三态 + 两段式 + 参数校验，与宽版对称。
void testGetsenv() {
    std::printf("[2b] getenv_s 窄版两段式读取\n");
    const char* name = "CONCORD_COMPAT_SELFTEST_NARROW";

    std::size_t count = 12345;
    checkEqual(0, getenv_s(&count, nullptr, 0, name), "未定义变量返回 0");
    checkEqual(0, static_cast<long long>(count), "未定义变量时 count == 0");

    const std::string value = "窄值-\xE4\xB8\xAD\xE6\x96\x87";
    ::unsetenv(name);
    checkEqual(0, _putenv_s(name, value.c_str()), "写入窄值");

    count = 0;
    checkEqual(0, getenv_s(&count, nullptr, 0, name), "查询长度返回 0");
    const std::size_t required = count;
    check(required > 1, "查询到的长度 > 1");

    std::vector<char> buffer(required, '\0');
    std::size_t readBack = 0;
    checkEqual(0, getenv_s(&readBack, buffer.data(), buffer.size(), name), "按查询到的长度读取成功");
    checkEqual(static_cast<long long>(required), static_cast<long long>(readBack), "两段式两次同 count");
    check(value == std::string(buffer.data()), "窄值原样回来");

    std::vector<char> small(required > 2 ? required - 1 : 1, '\0');
    std::size_t tooSmall = 0;
    checkEqual(ERANGE, getenv_s(&tooSmall, small.data(), small.size(), name), "缓冲区不足返回 ERANGE");
    checkEqual(static_cast<long long>(required), static_cast<long long>(tooSmall), "ERANGE 仍给所需大小");
    checkEqual('\0', small[0], "ERANGE 时缓冲区被清空");

    checkEqual(EINVAL, getenv_s(nullptr, nullptr, 0, name), "pReturnValue 为空返回 EINVAL");
    checkEqual(EINVAL, getenv_s(&count, nullptr, 0, nullptr), "varname 为空返回 EINVAL");
    checkEqual(EINVAL, getenv_s(&count, nullptr, 0, ""), "varname 为空串返回 EINVAL");
    ::unsetenv(name);
}

// ---------------------------------------------------------------------------
// 3. gmtime_s 的参数顺序
// ---------------------------------------------------------------------------
void testGmtimeOrder() {
    std::printf("[3] gmtime_s 参数顺序与 UTC 语义\n");
    const std::time_t epoch = 0;  // 1970-01-01T00:00:00Z
    struct tm result{};
    checkEqual(0, gmtime_s(&result, &epoch), "gmtime_s 返回 0");
    // 参数写反（(time_t*, tm*)）时这三条会给出完全不同的值或直接崩。
    checkEqual(1970, result.tm_year + 1900, "tm_year + 1900 == 1970");
    checkEqual(0, result.tm_mon, "tm_mon == 0（一月）");
    checkEqual(1, result.tm_mday, "tm_mday == 1");
    checkEqual(0, result.tm_hour, "tm_hour == 0");
    checkEqual(4, result.tm_wday, "tm_wday == 4（1970-01-01 是星期四）");
    checkEqual(0, result.tm_yday, "tm_yday == 0");

    // 一个不在纪元上的时刻，确认不是"恰好 0 通过"。
    const std::time_t later = 1700000000;  // 2023-11-14T22:13:20Z
    struct tm laterTm{};
    checkEqual(0, gmtime_s(&laterTm, &later), "gmtime_s 第二个时刻返回 0");
    checkEqual(2023, laterTm.tm_year + 1900, "年 == 2023");
    checkEqual(10, laterTm.tm_mon, "月 == 11（tm_mon 从 0 起）");
    checkEqual(14, laterTm.tm_mday, "日 == 14");
    checkEqual(22, laterTm.tm_hour, "时 == 22（UTC 而不是本地时）");
    checkEqual(13, laterTm.tm_min, "分 == 13");
    checkEqual(20, laterTm.tm_sec, "秒 == 20");

    checkEqual(EINVAL, gmtime_s(nullptr, &epoch), "tm 为空返回 EINVAL");
    checkEqual(EINVAL, gmtime_s(&result, nullptr), "time 为空返回 EINVAL");
}

// ---------------------------------------------------------------------------
// 4. 定时器分辨率（无害的诚实 no-op）
// ---------------------------------------------------------------------------
void testTimerResolution() {
    std::printf("[4] 定时器分辨率\n");
    checkEqual(TIMERR_NOERROR, timeBeginPeriod(1), "timeBeginPeriod(1) 返回 TIMERR_NOERROR");
    checkEqual(TIMERR_NOERROR, timeEndPeriod(1), "timeEndPeriod(1) 返回 TIMERR_NOERROR");
    checkEqual(TIMERR_NOERROR, timeBeginPeriod(0), "timeBeginPeriod(0) 不崩");
    checkEqual(TIMERR_NOERROR, timeEndPeriod(0), "timeEndPeriod(0) 不崩");
}

// ---------------------------------------------------------------------------
// 5. MCI：老老实实说"没有设备"
// ---------------------------------------------------------------------------
void testMci() {
    std::printf("[5] MCI 不可用\n");
    wchar_t status[32];
    std::wmemset(status, L'X', 32);
    checkEqual(MCIERR_DEVICE_NOT_INSTALLED,
               mciSendStringW(L"status concord_runtime_eva position", status, 32, nullptr),
               "mciSendStringW 返回 MCIERR_DEVICE_NOT_INSTALLED");
    checkEqual(0, status[0], "失败时返回缓冲被清空（不留未定义内容）");
    checkEqual(0, mciGetDeviceIDW(L"concord_runtime_eva"), "mciGetDeviceIDW 返回 0");
    // 失败也不能崩：空命令、空缓冲、零长度都要能吃下。
    checkEqual(MCIERR_DEVICE_NOT_INSTALLED, mciSendStringW(L"", nullptr, 0, nullptr),
               "空缓冲调用不崩");
    checkEqual(306, MCIERR_DEVICE_NOT_INSTALLED, "MCIERR_DEVICE_NOT_INSTALLED == 306（mmsyscom.h）");
    checkEqual(274, MCIERR_UNSUPPORTED_FUNCTION, "MCIERR_UNSUPPORTED_FUNCTION == 274");
}

// ---------------------------------------------------------------------------
// 6. COM：S_OK 要能配对
// ---------------------------------------------------------------------------
void testCom() {
    std::printf("[6] COM 初始化契约\n");
    const HRESULT first = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    check(SUCCEEDED(first), "CoInitializeEx 返回成功");
    check(first == S_OK, "返回 S_OK 而不是 RPC_E_CHANGED_MODE");
    check(first != RPC_E_CHANGED_MODE,
          "不谎报 RPC_E_CHANGED_MODE（那会让调用方既不配对也未真正失败）");
    // 与产品代码一致：成功就必须配对。重复调用也不能崩。
    CoUninitialize();
    (void)CoInitializeEx(nullptr, 0);
    CoUninitialize();
    CoUninitialize();
    check(true, "CoUninitialize 可重复调用");
}

// S8：Interlocked 族。判据里最要紧的不是"加减法对不对"，而是**宽度**：
// 这四条的形参曾经写成 `volatile long*`（本平台 64 位），而字段是 `volatile LONG`（32 位）。
// 硬转过去之后每次交换会写 8 字节，把紧挨着的字段一起改掉——而"返回值对不对"完全看不出来。
void testInterlocked() {
    std::printf("[8] Interlocked 族\n");

    volatile LONG counter = 0;
    checkEqual(InterlockedIncrement(&counter), 1, "InterlockedIncrement 返回**新值** 1");
    checkEqual(InterlockedIncrement(&counter), 2, "InterlockedIncrement 返回**新值** 2");
    checkEqual(InterlockedDecrement(&counter), 1, "InterlockedDecrement 返回**新值** 1");

    checkEqual(InterlockedExchange(&counter, 42), 1, "InterlockedExchange 返回**旧值** 1");
    checkEqual(counter, 42, "InterlockedExchange 写入新值 42");

    // 成功：返回原值，写入交换值。
    checkEqual(InterlockedCompareExchange(&counter, 7, 42), 42,
               "InterlockedCompareExchange 命中时返回原值 42");
    checkEqual(counter, 7, "命中时写入交换值 7");
    // 失败：返回值是**当前值**（不是 0、也不是比较值），而且一个字都不写。
    checkEqual(InterlockedCompareExchange(&counter, 99, 12345), 7,
               "未命中时返回当前值 7");
    checkEqual(counter, 7, "未命中时不写入");

    // 宽度：两个相邻的 LONG，只动第一个，第二个必须逐字节不变。
    struct Pair {
        volatile LONG first;
        volatile LONG second;
    } pair{5, 1234567};
    InterlockedExchange(&pair.first, 9);
    checkEqual(pair.second, 1234567,
               "交换只写 4 字节（相邻字段 1234567 未被踩）");
    checkEqual(sizeof(LONG), 4, "LONG 是 4 字节——写成 long 就是 8 字节，这才是根因");

    // 指针版本：同样返回旧值。
    void* slot = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x11));
    void* previous = InterlockedExchangePointer(&slot, reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x22)));
    check(previous == reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x11)),
          "InterlockedExchangePointer 返回旧指针");
    check(slot == reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x22)),
          "InterlockedExchangePointer 写入新指针");
}

}  // namespace

int main() {
    std::printf("compat/win32 小族收口自测（CRT/COM/定时器/MCI）\n\n");
    testPutenvRemoves();
    testWgetenv();
    testGetsenv();
    testGmtimeOrder();
    testTimerResolution();
    testMci();
    testCom();
    testInterlocked();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
