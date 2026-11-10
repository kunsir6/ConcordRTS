// compat/win32 自测 · 文件 / 进程 / 编码 / 时间族（S10）
//
// 这个自测的取向与前九层一致：不测"函数能调通"，只测**换成表层等价物就会
// 静默改变行为**的地方。本层有六处这样的地方：
//
//   1. GetLastError 必须线程局部，且**成功的调用不清它**。用全局 int 会偶发串线；
//      在函数入口清 0 会让"先做可能失败的事，再读 GetLastError"永远读到 0。
//   2. CloseHandle 收到 GDI 对象指针时**不能**把它当 fd 关掉。判据只能是登记表。
//   3. CreateFileW 的失败码必须区分"已存在"与"拒绝访问"——调用点的重试循环
//      按这两个值分流，给错就是永不重试或空转。
//   4. 4 字节的 emoji 在本平台占**一个** wchar_t，在 Windows 占两个。凡是
//      `size()` 参与判据的地方，两边数字本来就不同。这一条不钉住，所有"长度
//      阈值"的测试都会在两平台给出不同结论且没人发现。
//   5. 非法 UTF-8 在严格模式必须**失败**：调用点 toWide() 正是靠这次失败判定
//      "这不是 UTF-8"再回退 CP_ACP。悄悄替换成 U+FFFD 会让回退分支永远走不到。
//   6. GetEnvironmentVariableW(不存在) 必须返回 0。项目一半的诊断开关是
//      `GetEnvironmentVariableW(L"FLAG", buf, 2) == 1`。
//
// 构建与运行：
//   c++ -std=c++20 -I compat/win32/include \
//       compat/win32/src/RuntimeWin32Compat{Process,File}.cpp \
//       compat/win32/selftest/CompatFileSemantics.cpp -o /tmp/ccs10 && /tmp/ccs10

#include <windows.h>

#include "../src/RuntimeWin32CompatInternal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
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

void checkBytes(const void* data, std::size_t size, const char* expected, std::size_t expectedSize,
                const char* description) {
    ++gChecks;
    if (size != expectedSize || std::memcmp(data, expected, expectedSize) != 0) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected %zu bytes, got %zu)\n", description, expectedSize, size);
    }
}

// ---------------------------------------------------------------------------
// 临时目录
// ---------------------------------------------------------------------------
std::string makeTempDirectory() {
    char pattern[] = "/tmp/concord-compat-s10-XXXXXX";
    char* created = ::mkdtemp(pattern);
    if (created == nullptr) {
        std::printf("  FATAL  mkdtemp failed\n");
        std::exit(2);
    }
    return std::string(created);
}

std::string readWholeFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string out;
    char buffer[512];
    ssize_t got = 0;
    while ((got = ::read(fd, buffer, sizeof(buffer))) > 0) {
        out.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(fd);
    return out;
}

std::size_t fileSize(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return static_cast<std::size_t>(-1);
    return static_cast<std::size_t>(info.st_size);
}

// ---------------------------------------------------------------------------
// [1] LastError 的线程局部与"成功不清"
// ---------------------------------------------------------------------------
void testLastError() {
    std::printf("[1] GetLastError 的线程局部与成功不清\n");
    SetLastError(4321);
    checkEqual(4321, GetLastError(), "SetLastError 之后立刻读回同一个值");

    // 成功路径不许清 LastError。这里用一条**必然会成功**的调用做证据：
    // GetTickCount64 没有参数、没有失败可能。
    (void)GetTickCount64();
    checkEqual(4321, GetLastError(), "成功调用 GetTickCount64 之后 LastError 不变");

    SetLastError(777);
    DWORD observed = 0;
    std::thread worker([&observed] {
        observed = GetLastError();      // 新线程应当是初值 0
        SetLastError(999);
    });
    worker.join();
    checkEqual(0, observed, "新线程的 LastError 初值为 0（线程局部）");
    checkEqual(777, GetLastError(), "子线程改 LastError 不影响主线程");
}

// ---------------------------------------------------------------------------
// [2] CreateFileW 的五种 disposition 与失败码
// ---------------------------------------------------------------------------
void testCreateFile(const std::string& directory) {
    std::printf("[2] CreateFileW 的 disposition 与失败码\n");
    const std::string path = directory + "/disposition.txt";

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    check(file != INVALID_HANDLE_VALUE, "CREATE_NEW 在空目录上成功");
    DWORD written = 0;
    const char payload[] = "abcdefghij";
    check(WriteFile(file, payload, 10, &written, nullptr) != FALSE, "WriteFile 返回成功");
    checkEqual(10, written, "WriteFile 回报写入字节数");
    check(CloseHandle(file) != FALSE, "CloseHandle 关闭文件句柄");

    checkEqual(10, static_cast<long long>(fileSize(path)), "落盘大小等于写入大小");
    check(readWholeFile(path) == std::string(payload, 10), "落盘内容逐字节一致");

    // 已存在 → 必须是 ERROR_FILE_EXISTS。调用点的重试循环按这个值分流。
    SetLastError(0);
    HANDLE again = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    check(again == INVALID_HANDLE_VALUE, "CREATE_NEW 对已存在文件失败");
    checkEqual(ERROR_FILE_EXISTS, GetLastError(), "已存在时错误码是 ERROR_FILE_EXISTS");

    // 不存在 → ERROR_FILE_NOT_FOUND
    SetLastError(0);
    HANDLE missing = CreateFileW((directory + "/absent.txt").c_str(), GENERIC_READ, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(missing == INVALID_HANDLE_VALUE, "OPEN_EXISTING 对缺失文件失败");
    checkEqual(ERROR_FILE_NOT_FOUND, GetLastError(), "缺失时错误码是 ERROR_FILE_NOT_FOUND");

    // CREATE_ALWAYS 截断已有内容
    HANDLE truncated = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    check(truncated != INVALID_HANDLE_VALUE, "CREATE_ALWAYS 打开已存在文件成功");
    check(CloseHandle(truncated) != FALSE, "关闭截断句柄");
    checkEqual(0, static_cast<long long>(fileSize(path)), "CREATE_ALWAYS 之后文件被截空");

    // 只读 + TRUNCATE_EXISTING 在 Win32 上是非法参数（不是"悄悄不截断"）
    SetLastError(0);
    HANDLE illegal = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, TRUNCATE_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    check(illegal == INVALID_HANDLE_VALUE, "只读 + TRUNCATE_EXISTING 失败");
    checkEqual(ERROR_INVALID_PARAMETER, GetLastError(), "只读 + 截断的错误码是 INVALID_PARAMETER");

    // 非法 disposition
    SetLastError(0);
    check(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, 99, FILE_ATTRIBUTE_NORMAL, nullptr)
              == INVALID_HANDLE_VALUE,
          "未知 disposition 失败");
    checkEqual(ERROR_INVALID_PARAMETER, GetLastError(), "未知 disposition 的错误码");

    // 窄参重载：这正是 std::filesystem::path::c_str() 在项目里的形态
    const std::filesystem::path narrowPath = std::filesystem::path(directory) / "narrow.txt";
    HANDLE narrow = CreateFileW(narrowPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    check(narrow != INVALID_HANDLE_VALUE, "窄参 CreateFileW(path.c_str()) 可用（存档链路入口）");
    if (narrow != INVALID_HANDLE_VALUE) {
        check(WriteFile(narrow, "x", 1, &written, nullptr) != FALSE, "窄参句柄可写");
        check(CloseHandle(narrow) != FALSE, "窄参句柄可关");
    }
    checkEqual(1, static_cast<long long>(fileSize(directory + "/narrow.txt")), "窄参路径落盘");
}

// ---------------------------------------------------------------------------
// [3] WriteFile 的边界
// ---------------------------------------------------------------------------
void testWriteFile(const std::string& directory) {
    std::printf("[3] WriteFile 的边界\n");
    const std::string path = directory + "/write.txt";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        check(false, "WriteFile 测试的前置打开失败");
        return;
    }

    // 二进制内容含 NUL：用字符串长度当写入长度会截断，这里正好证明没有
    const char binary[] = {'\x00', '\x01', '\xFF', '\x00'};
    DWORD written = 12345;
    check(WriteFile(file, binary, 4, &written, nullptr) != FALSE, "写含 NUL 的二进制内容");
    checkEqual(4, written, "二进制写入字节数");

    // 0 字节写入成功且回报 0（不是失败）
    written = 12345;
    check(WriteFile(file, nullptr, 0, &written, nullptr) != FALSE, "0 字节写入成功");
    checkEqual(0, written, "0 字节写入回报 0");
    check(CloseHandle(file) != FALSE, "关闭写句柄");

    const std::string content = readWholeFile(path);
    checkBytes(content.data(), content.size(), binary, sizeof(binary), "二进制内容逐字节落盘");

    // 只读句柄写入 → 拒绝访问
    HANDLE readOnly = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (readOnly != INVALID_HANDLE_VALUE) {
        SetLastError(0);
        written = 999;
        check(WriteFile(readOnly, "y", 1, &written, nullptr) == FALSE, "只读句柄写入失败");
        checkEqual(ERROR_ACCESS_DENIED, GetLastError(), "只读句柄写入的错误码是 ACCESS_DENIED");
        checkEqual(0, written, "失败时写入计数归零（不是留旧值）");
        check(CloseHandle(readOnly) != FALSE, "关闭只读句柄");
    } else {
        check(false, "只读打开测试前置失败");
    }

    // 野句柄：不能崩，不能报告成功
    SetLastError(0);
    check(WriteFile(reinterpret_cast<HANDLE>(&gChecks), "z", 1, &written, nullptr) == FALSE,
          "非本层句柄的 WriteFile 失败而不是崩溃");
    checkEqual(ERROR_INVALID_HANDLE, GetLastError(), "野句柄的错误码是 INVALID_HANDLE");
}

// ---------------------------------------------------------------------------
// [4] CloseHandle 的类型判别
// ---------------------------------------------------------------------------
void testCloseHandle() {
    std::printf("[4] CloseHandle 只关自己登记过的句柄\n");
    SetLastError(0);
    check(CloseHandle(nullptr) == FALSE, "CloseHandle(nullptr) 失败");
    checkEqual(ERROR_INVALID_HANDLE, GetLastError(), "空句柄错误码");

    check(CloseHandle(INVALID_HANDLE_VALUE) == FALSE, "CloseHandle(INVALID_HANDLE_VALUE) 失败");

    // GDI 对象：CloseHandle 必须拒绝，且**对象仍然可用**。
    // 这是本层最危险的一处——把指针当 fd 关掉的后果是关掉一个毫不相干的 fd。
    HDC memoryDc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateCompatibleBitmap(memoryDc, 8, 8);
    check(memoryDc != nullptr && bitmap != nullptr, "GDI 测试对象创建成功");
    if (memoryDc != nullptr && bitmap != nullptr) {
        SetLastError(0);
        check(CloseHandle(bitmap) == FALSE, "CloseHandle 拒绝关闭 GDI 位图");
        checkEqual(ERROR_INVALID_HANDLE, GetLastError(), "拒绝时的错误码是 INVALID_HANDLE");

        BITMAP info{};
        checkEqual(static_cast<long long>(sizeof(BITMAP)), GetObjectW(bitmap, sizeof(BITMAP), &info),
                   "被拒绝之后位图依然有效（GetObjectW 仍返回 sizeof(BITMAP)）");
        checkEqual(8, info.bmWidth, "位图宽度未被破坏");
        DeleteObject(bitmap);
    }
    if (memoryDc != nullptr) DeleteDC(memoryDc);
}

// ---------------------------------------------------------------------------
// [5] 句柄不泄漏
// ---------------------------------------------------------------------------
void testNoHandleLeak(const std::string& directory) {
    std::printf("[5] 句柄登记表不泄漏\n");
    const std::size_t before = concord_compat::liveHandleCount();
    const std::string path = directory + "/leak.txt";
    for (int index = 0; index < 8; ++index) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  (index == 0) ? CREATE_ALWAYS : OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            check(false, "泄漏测试打开文件失败");
            return;
        }
        DWORD written = 0;
        WriteFile(file, "a", 1, &written, nullptr);
        CloseHandle(file);
    }
    checkEqual(static_cast<long long>(before), static_cast<long long>(concord_compat::liveHandleCount()),
               "8 次打开/关闭之后登记表回到基线");
}

// ---------------------------------------------------------------------------
// [6] UTF-8 → 宽字符：4 字节码点只占一个 wchar_t
// ---------------------------------------------------------------------------
void testDecode() {
    std::printf("[6] MultiByteToWideChar 的码点语义\n");
    wchar_t buffer[16] = {};

    const char ascii[] = "abc";
    checkEqual(3, MultiByteToWideChar(CP_UTF8, 0, ascii, 3, buffer, 16), "ASCII 三字节 → 3 个宽字符");
    check(buffer[2] == L'c', "ASCII 内容正确");

    const char twoByte[] = "\xC3\xA9";  // é
    checkEqual(1, MultiByteToWideChar(CP_UTF8, 0, twoByte, 2, buffer, 16), "2 字节序列 → 1 个宽字符");
    checkEqual(0xE9, static_cast<long long>(buffer[0]), "é = U+00E9");

    const char threeByte[] = "\xE4\xB8\xAD";  // 中
    checkEqual(1, MultiByteToWideChar(CP_UTF8, 0, threeByte, 3, buffer, 16), "3 字节序列 → 1 个宽字符");
    checkEqual(0x4E2D, static_cast<long long>(buffer[0]), "中 = U+4E2D");

    // 关键平台差异：U+1F600 在 Windows 上是两个代理项 wchar_t，这里是**一个**。
    const char fourByte[] = "\xF0\x9F\x98\x80";  // 😀
    checkEqual(1, MultiByteToWideChar(CP_UTF8, 0, fourByte, 4, buffer, 16),
               "4 字节序列 → 1 个宽字符（Windows 上是 2 个）");
    checkEqual(0x1F600, static_cast<long long>(buffer[0]), "😀 = U+1F600");

    // 负长度：以 NUL 结尾，且 NUL 计入
    checkEqual(3, MultiByteToWideChar(CP_UTF8, 0, "ab", -1, nullptr, 0), "-1 长度含结尾 NUL");
    wchar_t term[4] = {};
    checkEqual(3, MultiByteToWideChar(CP_UTF8, 0, "ab", -1, term, 4), "-1 长度写出 3 个宽字符");
    check(term[2] == L'\0', "写出内容以 NUL 结尾");

    // 查询长度（cch=0）不写缓冲、返回所需数量
    checkEqual(1, MultiByteToWideChar(CP_UTF8, 0, threeByte, 3, nullptr, 0), "cch=0 只查询所需数量");

    // 缓冲不足 → 0 + ERROR_INSUFFICIENT_BUFFER
    SetLastError(0);
    checkEqual(0, MultiByteToWideChar(CP_UTF8, 0, ascii, 3, buffer, 2), "缓冲不足返回 0");
    checkEqual(ERROR_INSUFFICIENT_BUFFER, GetLastError(), "缓冲不足的错误码");

    // 空输入
    checkEqual(0, MultiByteToWideChar(CP_UTF8, 0, ascii, 0, buffer, 16), "cb=0 返回 0");

    // 不支持的码页必须拒绝，不能当 UTF-8 解
    SetLastError(0);
    checkEqual(0, MultiByteToWideChar(950, 0, ascii, 3, buffer, 16), "码页 950 被拒绝");
    checkEqual(ERROR_INVALID_PARAMETER, GetLastError(), "不支持码页的错误码");
}

// ---------------------------------------------------------------------------
// [7] 非法输入：严格模式必须失败
// ---------------------------------------------------------------------------
void testInvalidInput() {
    std::printf("[7] 非法 UTF-8 的严格/非严格处置\n");
    wchar_t wide[8] = {};
    const struct {
        const char* bytes;
        int length;
        const char* label;
    } cases[] = {
        {"\xC0\x80", 2, "过长编码 C0 80（U+0000 的非最短形式）"},
        {"\xED\xA0\x80", 3, "UTF-8 里编码代理项 U+D800"},
        {"\xE4\xB8", 2, "截断的三字节序列"},
        {"\x80", 1, "孤立的后续字节"},
        {"\xF5\x80\x80\x80", 4, "超出 U+10FFFF"},
    };
    for (const auto& item : cases) {
        SetLastError(0);
        checkEqual(0, MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, item.bytes, item.length,
                                         wide, 8),
                   item.label);
        checkEqual(ERROR_NO_UNICODE_TRANSLATION, GetLastError(), "严格模式失败码是 NO_UNICODE_TRANSLATION");

        // 非严格模式：替换成 U+FFFD，且成功。两条一起才说明"严格"是有意义的开关，
        // 而不是"永远失败"。
        const int count = MultiByteToWideChar(CP_UTF8, 0, item.bytes, item.length, wide, 8);
        check(count > 0, "非严格模式下不失败");
        bool sawReplacement = false;
        for (int index = 0; index < count; ++index) {
            if (wide[index] == static_cast<wchar_t>(0xFFFD)) sawReplacement = true;
        }
        check(sawReplacement, "非严格模式产出 U+FFFD 替换字符");
    }

    // 宽 → 窄方向：孤立代理项
    const wchar_t lone[1] = {static_cast<wchar_t>(0xD800)};
    char narrow[8] = {};
    SetLastError(0);
    checkEqual(0, WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, lone, 1, narrow, 8, nullptr, nullptr),
               "WideCharToMultiByte 严格模式拒绝孤立代理项");
    checkEqual(ERROR_NO_UNICODE_TRANSLATION, GetLastError(), "宽→窄严格失败的错误码");

    BOOL usedDefault = FALSE;
    checkEqual(1, WideCharToMultiByte(CP_UTF8, 0, lone, 1, narrow, 8, nullptr, &usedDefault),
               "非严格模式替换成 1 个字节");
    check(narrow[0] == '?', "默认替换字符是 '?'");
    check(usedDefault != FALSE, "lpUsedDefaultChar 置真");

    usedDefault = TRUE;
    checkEqual(1, WideCharToMultiByte(CP_UTF8, 0, lone, 1, narrow, 8, "*", &usedDefault),
               "自定义默认字符");
    check(narrow[0] == '*', "使用调用方给出的默认字符");
    check(usedDefault != FALSE, "自定义默认字符时仍报告发生过替换");

    usedDefault = TRUE;
    checkEqual(1, WideCharToMultiByte(CP_UTF8, 0, L"a", 1, narrow, 8, nullptr, &usedDefault),
               "合法输入编码成功");
    check(usedDefault == FALSE, "没有替换时 lpUsedDefaultChar 为假");
}

// ---------------------------------------------------------------------------
// [8] 宽 → 窄 的基本语义
// ---------------------------------------------------------------------------
void testEncode() {
    std::printf("[8] WideCharToMultiByte 的基本语义\n");
    char buffer[16] = {};

    checkEqual(3, WideCharToMultiByte(CP_UTF8, 0, L"abc", 3, buffer, 16, nullptr, nullptr),
               "ASCII 3 个宽字符 → 3 字节");
    checkBytes(buffer, 3, "abc", 3, "ASCII 内容");

    const wchar_t emoji[1] = {static_cast<wchar_t>(0x1F600)};
    checkEqual(4, WideCharToMultiByte(CP_UTF8, 0, emoji, 1, buffer, 16, nullptr, nullptr),
               "U+1F600 → 4 字节（1 个宽字符变 4 个字节）");
    checkBytes(buffer, 4, "\xF0\x9F\x98\x80", 4, "😀 的 UTF-8 编码");

    // 负长度含 NUL
    checkEqual(3, WideCharToMultiByte(CP_UTF8, 0, L"ab", -1, buffer, 16, nullptr, nullptr),
               "-1 长度含结尾 NUL");
    checkBytes(buffer, 3, "ab\0", 3, "含 NUL 的编码结果");

    // 查询
    checkEqual(4, WideCharToMultiByte(CP_UTF8, 0, emoji, 1, nullptr, 0, nullptr, nullptr),
               "cb=0 只查询所需字节数");

    // 缓冲不足
    SetLastError(0);
    checkEqual(0, WideCharToMultiByte(CP_UTF8, 0, L"abc", 3, buffer, 2, nullptr, nullptr),
               "缓冲不足返回 0");
    checkEqual(ERROR_INSUFFICIENT_BUFFER, GetLastError(), "缓冲不足的错误码");
}

// ---------------------------------------------------------------------------
// [9] 环境变量
// ---------------------------------------------------------------------------
void testEnvironment() {
    std::printf("[9] GetEnvironmentVariableA/W\n");
    const char* const name = "CONCORD_COMPAT_S10_FLAG";
    const char* const wideName = "CONCORD_COMPAT_S10_WIDE";
    ::unsetenv(name);
    ::unsetenv(wideName);

    SetLastError(0);
    checkEqual(0, GetEnvironmentVariableW(L"CONCORD_COMPAT_S10_MISSING", nullptr, 0),
               "未定义的变量返回 0");
    checkEqual(ERROR_ENVVAR_NOT_FOUND, GetLastError(), "未定义的错误码");

    ::setenv(name, "1", 1);
    wchar_t wideBuffer[8] = {};
    checkEqual(1, GetEnvironmentVariableW(L"CONCORD_COMPAT_S10_FLAG", wideBuffer, 8),
               "取到长度 1（不含 NUL）");
    check(wideBuffer[0] == L'1' && wideBuffer[1] == L'\0', "值为 \"1\" 且以 NUL 结尾");

    // 这正是项目里 `== 1 && buf[0] == L'1'` 这种开关判据的形态
    checkEqual(2, GetEnvironmentVariableW(L"CONCORD_COMPAT_S10_FLAG", nullptr, 0),
               "查询模式返回所需大小（含 NUL）");

    // 大小写不敏感：Windows 语义。POSIX 的 getenv 是敏感的，少这一轮会让同一份
    // 启动脚本在 Windows 上生效、在这里静默无效。
    checkEqual(1, GetEnvironmentVariableW(L"concord_compat_s10_flag", wideBuffer, 8),
               "变量名大小写不敏感（宽版）");
    checkEqual(2, GetEnvironmentVariableA("Concord_Compat_S10_Flag", nullptr, 0),
               "变量名大小写不敏感（窄版查询长度）");

    // 缓冲不足：不写内容，只回所需大小
    ::setenv(wideName, "hello", 1);
    wchar_t small[3] = {L'Z', L'Z', L'Z'};
    checkEqual(6, GetEnvironmentVariableW(L"CONCORD_COMPAT_S10_WIDE", small, 3),
               "缓冲不足返回所需大小（5 + NUL）");
    check(small[0] == L'Z' && small[1] == L'Z', "缓冲不足时不写任何内容");

    // 非 ASCII 值
    ::setenv(wideName, "中文", 1);
    wchar_t chinese[8] = {};
    checkEqual(2, GetEnvironmentVariableW(L"CONCORD_COMPAT_S10_WIDE", chinese, 8),
               "UTF-8 值解码为 2 个宽字符");
    check(chinese[0] == static_cast<wchar_t>(0x4E2D), "第一个字符是「中」");

    char narrow[16] = {};
    checkEqual(6, GetEnvironmentVariableA("CONCORD_COMPAT_S10_WIDE", narrow, 16),
               "窄版返回字节长度");
    check(std::strcmp(narrow, "中文") == 0, "窄版原样返回");

    ::unsetenv(name);
    ::unsetenv(wideName);
}

// ---------------------------------------------------------------------------
// [10] 时间
// ---------------------------------------------------------------------------
void testTime() {
    std::printf("[10] 时钟\n");
    const ULONGLONG start = GetTickCount64();
    Sleep(30);
    const ULONGLONG after = GetTickCount64();
    check(after >= start, "GetTickCount64 单调不减");
    check(after - start >= 25, "Sleep(30) 之后 GetTickCount64 至少前进 25ms");
    check(after - start < 5000, "GetTickCount64 前进量在合理范围（不是毫秒/秒混淆）");

    Sleep(0);  // 不应崩溃，也不应真的睡

    LARGE_INTEGER first{};
    LARGE_INTEGER second{};
    check(QueryPerformanceCounter(&first) != FALSE, "QueryPerformanceCounter 第一次成功");
    check(QueryPerformanceCounter(&second) != FALSE, "QueryPerformanceCounter 第二次成功");
    check(second.QuadPart >= first.QuadPart, "QPC 单调不减");
    check(second.QuadPart - first.QuadPart < 1000000000LL, "QPC 单位是纳秒量级");
    SetLastError(0);
    check(QueryPerformanceCounter(nullptr) == FALSE, "空指针的 QPC 失败");
    checkEqual(ERROR_INVALID_PARAMETER, GetLastError(), "空指针 QPC 的错误码");

    SYSTEMTIME systemTime{};
    GetSystemTime(&systemTime);
    check(systemTime.wYear >= 2024, "GetSystemTime 的年份合理");
    check(systemTime.wMonth >= 1 && systemTime.wMonth <= 12, "月份在 1..12");
    check(systemTime.wDay >= 1 && systemTime.wDay <= 31, "日在 1..31");
    check(systemTime.wDayOfWeek <= 6, "星期在 0..6");
    check(systemTime.wMilliseconds < 1000, "毫秒在 0..999");

    // 与 gmtime 对拍：必须是 UTC，不是本地时间
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    ::gmtime_r(&now, &utc);
    checkEqual(utc.tm_year + 1900, systemTime.wYear, "GetSystemTime 的年份与 UTC 一致");
    checkEqual(utc.tm_mon + 1, systemTime.wMonth, "GetSystemTime 的月份与 UTC 一致");
    checkEqual(utc.tm_mday, systemTime.wDay, "GetSystemTime 的日与 UTC 一致");
    checkEqual(utc.tm_wday, systemTime.wDayOfWeek, "GetSystemTime 的星期与 UTC 一致");
}

// ---------------------------------------------------------------------------
// [11] 进程 / 线程 / 模块
// ---------------------------------------------------------------------------
void testProcess() {
    std::printf("[11] 进程 / 线程 / 模块\n");
    check(GetCurrentProcessId() == static_cast<DWORD>(::getpid()), "GetCurrentProcessId == getpid()");

    const HANDLE process = GetCurrentProcess();
    check(process != nullptr, "GetCurrentProcess 非空");
    // 刻意偏离 Win32：不返回 (HANDLE)-1。这条断言就是钉住这个偏离。
    check(process != INVALID_HANDLE_VALUE,
          "GetCurrentProcess 不是 INVALID_HANDLE_VALUE（刻意的偏离）");
    SetLastError(0);
    check(CloseHandle(process) == FALSE, "CloseHandle 拒绝关闭伪句柄");
    checkEqual(ERROR_INVALID_HANDLE, GetLastError(), "伪句柄的错误码");

    const DWORD mainThread = GetCurrentThreadId();
    check(mainThread != 0, "GetCurrentThreadId 非零");
    checkEqual(mainThread, GetCurrentThreadId(), "同一线程两次调用相同");

    DWORD otherThread = 0;
    std::thread worker([&otherThread] { otherThread = GetCurrentThreadId(); });
    worker.join();
    check(otherThread != 0, "子线程有 id");
    check(otherThread != mainThread, "不同线程 id 不同");

    const HINSTANCE image = GetModuleHandleW(nullptr);
    check(image != nullptr, "GetModuleHandleW(nullptr) 返回本映像令牌");

    SetLastError(0);
    check(GetModuleHandleW(L"kernel32.dll") == nullptr, "具名模块诚实失败");
    checkEqual(ERROR_MOD_NOT_FOUND, GetLastError(), "具名模块的错误码");

    SetLastError(0);
    check(GetProcAddress(image, "CreateFileW") == nullptr, "GetProcAddress 诚实失败");
    checkEqual(ERROR_PROC_NOT_FOUND, GetLastError(), "GetProcAddress 的错误码");
}

}  // namespace

int main() {
    std::printf("compat/win32 语义自测 · 文件 / 进程族（S10）\n\n");
    const std::string directory = makeTempDirectory();

    testLastError();
    testCreateFile(directory);
    testWriteFile(directory);
    testCloseHandle();
    testNoHandleLeak(directory);
    testDecode();
    testInvalidInput();
    testEncode();
    testEnvironment();
    testTime();
    testProcess();

    // 清理：删掉本测试创建的文件与目录
    for (const char* leaf : {"/disposition.txt", "/narrow.txt", "/write.txt", "/leak.txt"}) {
        ::unlink((directory + leaf).c_str());
    }
    ::rmdir(directory.c_str());

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
