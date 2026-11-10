// GDI+ PNG 导出后端（S6）。
//
// 范围：Windows 上 GDI+ 是一整套 2D 绘图 API；但产品只用它做**一件事**——
// 把一块 32bpp 像素内存写成一张 PNG 诊断图。全仓库只有三处调用，全是
// "Startup → 用像素缓冲构造 Bitmap → Save 成 PNG → Shutdown"：
//
//   src/render/core/RuntimeMapRenderer.cpp                        （启动器）
//   src/render/geometry/RuntimeAllWeaponMuzzleRender.inl          （muzzles-N.png）
//   src/render/resources/diagnostics/RuntimeNeutralBuildingPaletteAudit.inl
//                                                                  （调色板审计图）
//
// 渲染结果本身不经过 GDI+，所以这里**不实现任何绘制语义**，只兑现这七个
// 链接符号：
//
//   GdiplusStartup / GdiplusShutdown          token 记账（本层无事可初始化）
//   GetImageEncodersSize / GetImageEncoders   报告唯一一个 PNG 编解码器
//   Bitmap(width,height,stride,fmt,scan0)     持有像素缓冲 + 几何信息
//   Bitmap::Save(path, ...)                   CoreGraphics + ImageIO → PNG
//
// 真正的 PNG 编码走 ImageIO（CGImageDestination + "public.png"）。像素布局是
// GDI+ PixelFormat32bppRGB 在小端机上的 B,G,R,X，对应
// kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst。

#include <gdiplus.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "RuntimeWin32CompatInternal.h"  // concord_compat::utf8FromWideText

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace Gdiplus {

namespace {

// 真正的 PNG 编码器 CLSID。muzzle 诊断 smoke 里硬编码了这个值，所以
// GetImageEncoders 必须报同一个——否则"查表取 Clsid"与"硬编码 Clsid"两个
// 调用点会对不齐，同一张图在两条路径上拿到不同的编码器。
const CLSID kPngEncoderClsid = {0x557cf406, 0x1a04, 0x11d3,
                                {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

// 编解码器描述里的字符串指针必须比 GetImageEncoders 活得久：审计代码把
// MimeType 指针从 ImageCodecInfo 里取出来和 L"image/png" 比较。所以这里是
// 文件级静态，不是栈上缓冲。
const WCHAR kPngCodecName[] = L"Built-in PNG Codec";
const WCHAR kPngDllName[] = L"";
const WCHAR kPngFilenameExtension[] = L"*.png";
const WCHAR kPngMimeType[] = L"image/png";
const BYTE kPngSignature[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};

}  // namespace

Status GdiplusStartup(ULONG_PTR* token, const GdiplusStartupInput* input,
                      GdiplusStartupOutput* output) {
    if (token == nullptr) return InvalidParameter;
    // GDI+ 契约：GdiplusVersion 必须为 1，其它值都是调用方写错。
    if (input != nullptr && input->GdiplusVersion != 1) return InvalidParameter;
    (void)output;
    *token = 1;  // 任意非零值；Shutdown 只需看到"是否启动过"
    return Ok;
}

void GdiplusShutdown(ULONG_PTR token) {
    (void)token;
}

Status GetImageEncodersSize(UINT* numEncoders, UINT* size) {
    if (numEncoders == nullptr || size == nullptr) return InvalidParameter;
    *numEncoders = 1;
    *size = static_cast<UINT>(sizeof(ImageCodecInfo));
    return Ok;
}

Status GetImageEncoders(UINT numEncoders, UINT size, ImageCodecInfo* encoders) {
    // 审计调用方按 GetImageEncodersSize 给的 size 分配存储，再以 count=1 调进来。
    // 比这更小就是调用方违反了契约。
    if (encoders == nullptr) return InvalidParameter;
    if (numEncoders != 1 || size < sizeof(ImageCodecInfo)) return InsufficientBuffer;

    ImageCodecInfo info{};
    info.Clsid = kPngEncoderClsid;
    info.FormatID = kPngEncoderClsid;
    info.CodecName = kPngCodecName;
    info.DllName = kPngDllName;
    info.FilenameExtension = kPngFilenameExtension;
    info.MimeType = kPngMimeType;
    info.Flags = 0;
    info.Version = 1;
    info.SigCount = 1;
    info.SigSize = 8;
    info.SigPattern = kPngSignature;
    info.SigMask = nullptr;
    *encoders = info;
    return Ok;
}

Bitmap::Bitmap(INT width, INT height, INT stride, INT format, BYTE* scan0)
    : width_(width), height_(height), stride_(stride), format_(format), scan0_(scan0) {}

#if defined(__APPLE__)
namespace {

Status savePixelBufferAsPng(const char* utf8Path, INT width, INT height, INT stride,
                            INT format, const BYTE* scan0) {
    if (utf8Path == nullptr || scan0 == nullptr) return InvalidParameter;
    if (width <= 0 || height <= 0) return InvalidParameter;
    // 只兑现真正用到的格式。ARGB/PARGB 全仓库无调用点，伪造实现只会让将来
    // 某天"以为能用"却拿到错序通道，宁可诚实拒绝。
    if (format != PixelFormat32bppRGB) return InvalidParameter;

    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    const size_t bytesPerRow = stride > 0 ? static_cast<size_t>(stride) : w * 4u;

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    if (space == nullptr) return OutOfMemory;

    // 不复制像素：CGDataProvider 直接指向调用方那块帧缓冲。Save 是同步的，
    // CGImageDestinationFinalize 返回前就把数据读完了，之后才释放 provider。
    CGDataProviderRef provider =
        CGDataProviderCreateWithData(nullptr, scan0, bytesPerRow * h, nullptr);
    if (provider == nullptr) {
        CGColorSpaceRelease(space);
        return OutOfMemory;
    }

    // PixelFormat32bppRGB 在小端机上是 B,G,R,X，对应
    // kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst（跳过末尾 X 字节）。
    const CGBitmapInfo bitmapInfo =
        static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) |
        static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst);

    CGImageRef image = CGImageCreate(w, h, 8, 32, bytesPerRow, space, bitmapInfo,
                                     provider, nullptr, false,
                                     kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    if (image == nullptr) return GenericError;

    CFStringRef path =
        CFStringCreateWithCString(nullptr, utf8Path, kCFStringEncodingUTF8);
    CFURLRef url = path == nullptr
                       ? nullptr
                       : CFURLCreateWithFileSystemPath(nullptr, path,
                                                       kCFURLPOSIXPathStyle, false);
    if (path != nullptr) CFRelease(path);
    if (url == nullptr) {
        CGImageRelease(image);
        return GenericError;
    }

    CGImageDestinationRef dest =
        CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    CFRelease(url);
    if (dest == nullptr) {
        CGImageRelease(image);
        return UnknownImageFormat;
    }

    CGImageDestinationAddImage(dest, image, nullptr);
    const bool written = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(image);
    return written ? Ok : GenericError;
}

}  // namespace
#endif  // __APPLE__

Status Bitmap::Save(const WCHAR* filename, const CLSID*, const void*) const {
    if (filename == nullptr) return InvalidParameter;
#if defined(__APPLE__)
    const std::string utf8 = concord_compat::utf8FromWideText(filename);
    return savePixelBufferAsPng(utf8.c_str(), width_, height_, stride_, format_,
                                scan0_);
#else
    (void)filename;
    return NotImplemented;
#endif
}

Status Bitmap::Save(const char* filename, const CLSID*, const void*) const {
    if (filename == nullptr) return InvalidParameter;
#if defined(__APPLE__)
    return savePixelBufferAsPng(filename, width_, height_, stride_, format_, scan0_);
#else
    (void)filename;
    return NotImplemented;
#endif
}

}  // namespace Gdiplus
