// ============================================================================
// tests/ui/RuntimeChatTextEncodingSmoke.cpp
//
// T-03：std::wstring / u16string 的**运行期**语义（编译面已由 T-02 清零，但一条
// 运行路径都没执行过）。
//
// 这个文件只钉三件能跑出数字的事，全部围绕"含 BMP 外码点（emoji）的文本与路径
// 不丢字符"：
//
//   A. 产品的真实函数 `encodeRuntimeChatTextUtf8`（src/ui/input/RuntimeChatTextController.cpp，
//      不是复刻实现）—— 代理对必须合成 4 字节 UTF-8，孤立代理必须**整体拒绝**
//      而不是编出一个替换字符。
//   B. 兼容层的窄宽编解码。本平台把 CP_ACP/CP_OEMCP 映射到 UTF-8（
//      RuntimeWin32CompatProcess.cpp 里写明的平台决定），所以同一段 4 字节 UTF-8
//      在 mac 上只产出**一个** wchar_t，而 Windows 上会产出**两个** UTF-16 码元。
//      这条差异必须被钉住：任何"wstring.size() == UTF-16 长度"的假设在 mac 上都是错的。
//   C. std::filesystem::path 的宽窄桥接。POSIX 上 path 的 value_type 是 char，
//      wstring() 要经一次 UTF-8↔UTF-32 转换；含中文与 emoji 的路径必须在往返中
//      逐字节不变，否则"打不开素材"会被误诊成"素材缺失"。
//
// 不覆盖：菜单文本的**像素**（S4-3 的窗口自测 61/61 已覆盖，PNG 在
// progress/S4-窗口出字-800x600.png）。本文件的判据是"字符有没有丢"，不是"字长得对不对"。
// ============================================================================

#include "ui/input/RuntimeChatTextController.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

unsigned checks = 0;
unsigned failures = 0;

void expect(bool ok, const char* name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    }
}

void expectEqual(long long actual, long long wanted, const char* name) {
    ++checks;
    if (actual != wanted) {
        ++failures;
        std::fprintf(stderr, "FAIL %s (actual=%lld wanted=%lld)\n", name, actual, wanted);
    }
}

// 逐字节比较，并把差异打出来——"字符串不相等"这种失败如果不显示字节，
// 排查时会先怀疑编码方向，而真正的原因常常只是某一位差 0x20。
void expectBytes(const std::string& actual, const std::string& expected, const char* name) {
    ++checks;
    if (actual == expected) return;
    ++failures;
    std::fprintf(stderr, "FAIL %s\n  actual  :", name);
    for (unsigned char c : actual) std::fprintf(stderr, " %02X", c);
    std::fprintf(stderr, "\n  expected:");
    for (unsigned char c : expected) std::fprintf(stderr, " %02X", c);
    std::fprintf(stderr, "\n");
}

std::u16string utf16(const std::vector<char16_t>& units) { return std::u16string(units.begin(), units.end()); }

// --- A. 产品的聊天文本编码 ------------------------------------------------
void testChatTextEncoding() {
    expectBytes(encodeRuntimeChatTextUtf8(u"AB"), "AB", "BMP ASCII is unchanged");

    // 中文：U+4E2D → E4 B8 AD
    expectBytes(encodeRuntimeChatTextUtf8(u"\u4E2D"), "\xE4\xB8\xAD", "CJK BMP char is 3 UTF-8 bytes");

    // 😀 U+1F600 = 代理对 D83D DE00 → F0 9F 98 80
    const std::u16string emoji = utf16({0xD83D, 0xDE00});
    expectBytes(encodeRuntimeChatTextUtf8(emoji), "\xF0\x9F\x98\x80",
                "surrogate pair becomes one 4-byte UTF-8 sequence");

    const std::u16string mixed = utf16({0x0041, 0xD83D, 0xDE00, 0x4E2D});
    expectBytes(encodeRuntimeChatTextUtf8(mixed), "A\xF0\x9F\x98\x80\xE4\xB8\xAD",
                "A + emoji + CJK keeps order and length (8 bytes)");
    expectEqual(static_cast<long long>(encodeRuntimeChatTextUtf8(mixed).size()), 8,
                "mixed text is 8 UTF-8 bytes");

    // 孤立代理：必须整体拒绝（返回空串），不能编出一个 U+FFFD 冒充成功。
    expect(encodeRuntimeChatTextUtf8(utf16({0xD83D})).empty(),
           "lone high surrogate is rejected, not replaced");
    expect(encodeRuntimeChatTextUtf8(utf16({0xDE00})).empty(),
           "lone low surrogate is rejected, not replaced");
    expect(encodeRuntimeChatTextUtf8(utf16({0xDE00, 0xD83D})).empty(),
           "reversed surrogate pair is rejected");

    // 上限：256 字节。257 个 ASCII 字符必须整体失败（而不是截断成 256）。
    expect(encodeRuntimeChatTextUtf8(std::u16string(257, u'a')).empty(),
           "over-limit text is rejected instead of truncated");
    expectEqual(static_cast<long long>(encodeRuntimeChatTextUtf8(std::u16string(128, u'a')).size()), 128,
                "128 ASCII chars stay well inside the limit");
}

// --- B. 兼容层编解码：CP_ACP 在本平台等于 UTF-8 ---------------------------
void testCompatCodec() {
    const char* emojiUtf8 = "\xF0\x9F\x98\x80"; // 😀

    wchar_t wide[8]{};
    const int count = MultiByteToWideChar(CP_ACP, 0, emojiUtf8, 4, wide, 8);
    expectEqual(count, 1, "4-byte UTF-8 becomes exactly ONE wchar_t here (not two UTF-16 units)");
    expectEqual(wide[0], 0x1F600, "the code point survives as U+1F600");
    expectEqual(sizeof(wchar_t), 4, "wchar_t is 4 bytes on this platform (the whole point of T-03)");

    // 严格模式：合法 4 字节序列必须被接受（这条是"UTF-8 判定"的正向样本）。
    wchar_t strict[8]{};
    expectEqual(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, emojiUtf8, 4, strict, 8), 1,
                "MB_ERR_INVALID_CHARS accepts a well-formed 4-byte sequence");
    // 非法字节 0xFF 在严格模式必须失败——调用点 toWide() 正是靠这个失败判定
    // "这段字节不是 UTF-8"，从而走另一条分支。
    expectEqual(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, "\xFF", 1, strict, 8), 0,
                "MB_ERR_INVALID_CHARS rejects 0xFF");

    const std::wstring text = L"\u4E2D\u6587\x1F600";
    char utf8[32]{};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, 32, nullptr, nullptr);
    // 6（两个 CJK）+ 4（emoji）+ 1（NUL）= 11。第一版写成 9 是我自己数错，
    // 代码没问题——这类"硬编码期望值"的测试连写测试的人一起查。
    expectEqual(bytes, 11, "2 CJK chars + emoji + NUL = 11 bytes");
    expectBytes(std::string(utf8), "\xE4\xB8\xAD\xE6\x96\x87\xF0\x9F\x98\x80",
                "wide -> UTF-8 keeps every code point");
}

// --- C. 路径桥接 ----------------------------------------------------------
void testPathBridge() {
    const std::string utf8 = "/tmp/\xE4\xB8\xAD\xE6\x96\x87/\xF0\x9F\x98\x80.wav"; // /tmp/中文/😀.wav
    const std::filesystem::path path(utf8);

    expectBytes(path.string(), utf8, "path::string() round-trips the original UTF-8");

    const std::wstring wide = path.wstring();
    // 关键差异：POSIX 上 path 的 value_type 是 char，wstring() 走一次 UTF-8→UTF-32；
    // 于是 emoji 是**一个** wchar_t。任何按 UTF-16 码元数做的长度断言都会错。
    expectEqual(static_cast<long long>(wide.size()), 13, "13 code points, not 15 UTF-16 units");
    bool sawEmoji = false;
    for (wchar_t unit : wide) {
        if (static_cast<std::uint32_t>(unit) == 0x1F600) sawEmoji = true;
    }
    expect(sawEmoji, "the emoji survives the wide conversion as U+1F600");

    const std::filesystem::path back(wide);
    expectBytes(back.string(), utf8, "wide -> path -> UTF-8 is byte identical");
    expectBytes(back.filename().string(), "\xF0\x9F\x98\x80.wav", "filename alone also round-trips");

    // u8path 在 C++20 起被弃用，但产品里仍有调用点；这里确认它给出的路径与直接
    // 构造一致，避免"同一路径两种构造方式结果不同"这种最难查的差异。
    const std::filesystem::path viaU8 = std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(utf8.data()), reinterpret_cast<const char8_t*>(utf8.data()) + utf8.size()));
    expectBytes(viaU8.string(), utf8, "char8_t construction agrees with char construction");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    testChatTextEncoding();
    testCompatCodec();
    testPathBridge();

    std::printf("Chat text encoding: checks=%u failures=%u sizeof(wchar_t)=%zu\n", checks, failures,
                sizeof(wchar_t));
    return failures == 0 ? 0 : 1;
}
