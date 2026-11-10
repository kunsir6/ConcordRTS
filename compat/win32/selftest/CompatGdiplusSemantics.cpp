// compat/win32 自测 · GDI+ PNG 导出（S6）
//
// 断言的是"像素缓冲 → PNG 文件"这条唯一真实链路的语义闭环：
//   · GdiplusStartup 返回 Ok 且给非零 token，Shutdown 幂等；
//   · GetImageEncoders 只报一个编解码器，MimeType 是 "image/png"，Clsid
//     与 muzzle smoke 里硬编码的 PNG CLSID 一致；
//   · Bitmap::Save 真写出 PNG：文件头是 PNG 魔数，且解回后红/绿两个像素的
//     R/G/B 通道次序正确（不是 BGR 被当成 RGB 的静默换色）。
//
// 构建与运行：
//   c++ -std=c++20 -I compat/win32/include -I compat/win32/src \
//       compat/win32/src/RuntimeWin32CompatGdiplus.cpp \
//       compat/win32/src/RuntimeWin32CompatProcess.cpp \
//       compat/win32/selftest/CompatGdiplusSemantics.cpp -o /tmp/ccg \
//       -framework CoreGraphics -framework CoreFoundation -framework ImageIO && /tmp/ccg

#include <windows.h>
#include <gdiplus.h>

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

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

void checkEqual(std::uint32_t expected, std::uint32_t actual, const char* description) {
    ++gChecks;
    if (expected != actual) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected %u, got %u)\n", description, expected, actual);
    }
}

// 与 muzzle smoke 硬编码的 PNG 编码器 CLSID 一致。
const CLSID kExpectedPngClsid = {0x557cf406, 0x1a04, 0x11d3,
                                 {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

bool guidEqual(const GUID& a, const GUID& b) {
    if (a.Data1 != b.Data1 || a.Data2 != b.Data2 || a.Data3 != b.Data3) return false;
    for (int i = 0; i < 8; ++i) {
        if (a.Data4[i] != b.Data4[i]) return false;
    }
    return true;
}

void testStartupAndCodecs() {
    std::printf("== 启动与编解码器枚举 ==\n");

    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;
    check(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok,
          "GdiplusStartup 返回 Ok");
    check(token != 0, "GdiplusStartup 给非零 token");
    Gdiplus::GdiplusShutdown(token);

    check(Gdiplus::GdiplusStartup(nullptr, &input, nullptr) == Gdiplus::InvalidParameter,
          "GdiplusStartup(nullptr token) 返回 InvalidParameter");

    UINT count = 0, size = 0;
    check(Gdiplus::GetImageEncodersSize(&count, &size) == Gdiplus::Ok,
          "GetImageEncodersSize 返回 Ok");
    checkEqual(1, count, "编解码器数量 == 1");
    checkEqual(static_cast<std::uint32_t>(sizeof(Gdiplus::ImageCodecInfo)), size,
               "size == sizeof(ImageCodecInfo)");

    Gdiplus::ImageCodecInfo info{};
    check(Gdiplus::GetImageEncoders(1, size, &info) == Gdiplus::Ok,
          "GetImageEncoders 返回 Ok");
    check(std::wstring(info.MimeType) == L"image/png",
          "MimeType == image/png");
    check(guidEqual(info.Clsid, kExpectedPngClsid), "Clsid 与 PNG 编码器一致");
    check(Gdiplus::GetImageEncoders(0, size, &info) == Gdiplus::InsufficientBuffer,
          "numEncoders=0 返回 InsufficientBuffer");
}

#if defined(__APPLE__)
void testSavePng() {
    std::printf("== PNG 写出与颜色保真 ==\n");

    // 2x1：左红（0x00FF0000）右绿（0x0000FF00）。uint32 值 0x00RRGGBB 在小端
    // 机上内存字节序是 B,G,R,X。
    std::uint32_t pixels[2] = {0x00FF0000u, 0x0000FF00u};
    const char* path = "/tmp/concord_gdiplus_selftest.png";

    Gdiplus::Bitmap bmp(2, 1, 2 * 4, PixelFormat32bppRGB,
                        reinterpret_cast<BYTE*>(pixels));
    const CLSID clsid = kExpectedPngClsid;
    check(bmp.Save(path, &clsid, nullptr) == Gdiplus::Ok,
          "Bitmap::Save(const char*) 返回 Ok");

    // 1) 文件头必须是 PNG 魔数。
    FILE* f = std::fopen(path, "rb");
    check(f != nullptr, "PNG 文件已写出");
    if (f != nullptr) {
        unsigned char magic[8] = {};
        const size_t got = std::fread(magic, 1, 8, f);
        std::fclose(f);
        checkEqual(8, static_cast<std::uint32_t>(got), "PNG 文件至少 8 字节");
        static const unsigned char expected[8] = {0x89, 0x50, 0x4e, 0x47,
                                                  0x0d, 0x0a, 0x1a, 0x0a};
        bool matches = got == 8;
        for (size_t i = 0; i < got && i < 8; ++i) matches = matches && (magic[i] == expected[i]);
        check(matches, "文件头是 PNG 魔数");
    }

    // 2) 解回后颜色保真：红在左、绿在右，通道次序 R/G/B 正确。
    CFStringRef pathStr = CFStringCreateWithCString(nullptr, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, pathStr, kCFURLPOSIXPathStyle, false);
    CFRelease(pathStr);
    check(url != nullptr, "能构造 PNG 的 file URL");
    if (url != nullptr) {
        CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
        CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
        check(img != nullptr, "ImageIO 能解回 PNG");
        if (img != nullptr) {
            checkEqual(2, static_cast<std::uint32_t>(CGImageGetWidth(img)), "解回宽度 == 2");
            checkEqual(1, static_cast<std::uint32_t>(CGImageGetHeight(img)), "解回高度 == 1");

            // 画进默认（大端 RGBA，premultiplied last）的 2x1 缓冲。
            unsigned char rgba[2 * 4] = {};
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGContextRef ctx = CGBitmapContextCreate(
                rgba, 2, 1, 8, 2 * 4, cs, kCGImageAlphaPremultipliedLast);
            CGColorSpaceRelease(cs);
            check(ctx != nullptr, "能建 RGBA 回读上下文");
            if (ctx != nullptr) {
                CGContextDrawImage(ctx, CGRectMake(0, 0, 2, 1), img);
                CGContextRelease(ctx);
                // 左像素应是红：R 高、G/B 低。
                check(rgba[0] >= 200 && rgba[1] < 60 && rgba[2] < 60,
                      "左像素是红（R 高，G/B 低）");
                // 右像素应是绿。
                check(rgba[4] < 60 && rgba[5] >= 200 && rgba[6] < 60,
                      "右像素是绿（G 高，R/B 低）");
            }
            CGImageRelease(img);
        }
        if (src) CFRelease(src);
        CFRelease(url);
    }

    // 3) 宽路径（WCHAR*）也写出（只验返回值，颜色与 char* 版共用一个内核）。
    const wchar_t* wpath = L"/tmp/concord_gdiplus_selftest_wide.png";
    Gdiplus::Bitmap bmp2(2, 1, 2 * 4, PixelFormat32bppRGB,
                         reinterpret_cast<BYTE*>(pixels));
    check(bmp2.Save(wpath, &clsid, nullptr) == Gdiplus::Ok,
          "Bitmap::Save(const WCHAR*) 返回 Ok");

    // 4) 诚实边界：不支持的格式拒绝。
    Gdiplus::Bitmap bmp3(2, 1, 2 * 4, PixelFormat32bppARGB,
                         reinterpret_cast<BYTE*>(pixels));
    check(bmp3.Save(path, &clsid, nullptr) == Gdiplus::InvalidParameter,
          "PixelFormat32bppARGB 诚实拒绝（未实现）");
}
#endif  // __APPLE__

}  // namespace

int main() {
    testStartupAndCodecs();
#if defined(__APPLE__)
    testSavePng();
#else
    std::printf("(非 Apple 平台：跳过 PNG 编码器自测)\n");
#endif

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
