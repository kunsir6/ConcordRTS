#pragma once
// ============================================================================
// ConcordRTS Win32 兼容层 · GDI+ 头层
//
// 用途：非 Windows 平台替代 <gdiplus.h>。
//
// 使用面是实测出来的，不是猜的——全仓库只有 3 处引用 GDI+，且全部只为
// 【把像素写成 PNG 诊断图】：
//   src/render/core/RuntimeMapRenderer.cpp                        （启动器）
//   src/render/geometry/RuntimeAllWeaponMuzzleRender.inl          （muzzles-N.png）
//   src/render/resources/diagnostics/RuntimeNeutralBuildingPaletteAudit.inl
//                                                                 （调色板审计图）
// 也就是渲染结果本身不经过 GDI+，所以本层不需要实现任何绘制语义。
//
// 与 windows.h 一样：只在非 Windows 生效，且只声明不实现。
// ============================================================================

#if defined(_WIN32)
#error "compat/win32/include/gdiplus.h is the non-Windows shim; it must never be used on Windows builds."
#endif

#include <windows.h>

// PixelFormat 编码：低 8 位是每像素通道数，次 8 位是位深，高位是标志。
// 32bppRGB = 8 | (32 << 8) | PixelFormatGDI(0x00020000) | PixelFormatCanonical(0x00200000)
//          = 0x00262008
// 这里必须写对：写错只会让 PNG 通道次序错，编译不报错、图像难查。
#ifndef PixelFormat32bppRGB
#define PixelFormat32bppRGB 0x00262008
#endif
#ifndef PixelFormat32bppARGB
#define PixelFormat32bppARGB 0x0026200A
#endif
#ifndef PixelFormat32bppPARGB
#define PixelFormat32bppPARGB 0x000E200B
#endif

namespace Gdiplus {

enum Status {
    Ok = 0,
    GenericError = 1,
    InvalidParameter = 2,
    OutOfMemory = 3,
    ObjectBusy = 4,
    InsufficientBuffer = 5,
    NotImplemented = 6,
    Win32Error = 7,
    UnknownImageFormat = 13,
    FileNotFound = 17,
    AccessDenied = 18,
};

// GdiplusStartup 的入参。GdiplusVersion 必须为 1，其余保持默认。
struct GdiplusStartupInput {
    UINT32 GdiplusVersion = 1;
    void* DebugEventCallback = nullptr;
    BOOL SuppressBackgroundThread = FALSE;
    BOOL SuppressExternalCodecs = FALSE;
};

struct GdiplusStartupOutput {
    void* NotificationHook = nullptr;
    void* NotificationUnhook = nullptr;
};

Status GdiplusStartup(ULONG_PTR* token, const GdiplusStartupInput* input,
                      GdiplusStartupOutput* output);
void GdiplusShutdown(ULONG_PTR token);

// 编解码器描述。字段顺序与 Gdiplusimaging.h 一致：审计代码会按数组下标
// 连续遍历 GetImageEncoders 的返回值，并比较 MimeType 与取 Clsid。
struct ImageCodecInfo {
    CLSID Clsid;
    GUID FormatID;
    const WCHAR* CodecName;
    const WCHAR* DllName;
    const WCHAR* FilenameExtension;
    const WCHAR* MimeType;
    DWORD Flags;
    DWORD Version;
    DWORD SigCount;
    DWORD SigSize;
    const BYTE* SigPattern;
    const BYTE* SigMask;
};

Status GetImageEncodersSize(UINT* numEncoders, UINT* size);
Status GetImageEncoders(UINT numEncoders, UINT size, ImageCodecInfo* encoders);

// 位图对象。
//
// 目前只覆盖"用一块 32bpp 像素内存构造，再存成 PNG"这一种用法。真正的
// 编码实现留待 macOS 侧 PNG 导出任务；此处只声明，未实现会在链接期暴露。
class Bitmap {
public:
    Bitmap(INT width, INT height, INT stride, INT format, BYTE* scan0);

    // std::filesystem::path::c_str() 在 Windows 返回 const wchar_t*、在 POSIX
    // 返回 const char*，两种都接住，调用点就不需要 #ifdef。
    Status Save(const WCHAR* filename, const CLSID* clsidEncoder, const void* encoderParams) const;
    Status Save(const char* filename, const CLSID* clsidEncoder, const void* encoderParams) const;

private:
    INT width_ = 0;
    INT height_ = 0;
    INT stride_ = 0;
    INT format_ = 0;
    BYTE* scan0_ = nullptr;
};

} // namespace Gdiplus
