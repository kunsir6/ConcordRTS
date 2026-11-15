// L10N-03: on-screen text layer (HiDPI). See RuntimeWin32CompatTextLayer.h.
//
// 这一层做的事只有三件：把 drawTextRun 的调用登记下来、按 backing 倍数把它们
// 重新光栅成一张透明 BGRA 位图、交给 Cocoa 呈现层贴成独立图层的 contents。
//
// 三个刻意的取舍：
//
// 1. **只在内容变化时重建。** 一帧 2048x1280 的位图清零 + 重画是 10MB 级别的
//    内存流量，60Hz 下白做就是 600MB/s。用一个覆盖全部绘制上下文的哈希做判据，
//    静止的 tooltip 不会让 CPU 动一下。
//
// 2. **清零按上一帧的脏矩形，不按全屏。** 界面里真正变的是金币数字和计时器，
//    它们只占几百个像素；全屏 memset 会把这层省下来的开销又还回去。
//
// 3. **runs 为空时保留上一张，不替换成空白。** 空 runs 有两种含义：这一帧真的
//    没有文字，或者这一帧根本没重绘。前者在游戏里几乎不存在（任何界面都有文字），
//    后者一旦发生，把图层清空就会让整屏文字凭空消失。宁可多留一帧。

#include "RuntimeWin32CompatTextLayer.h"

#include "RuntimeAppleWindowSurface.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace concord::win32compat::textlayer {
namespace {

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct State {
    bool capture = false;
    double scale = 1.0;
    int surfaceWidth = 0;
    int surfaceHeight = 0;

    std::vector<Run> runs;
    std::vector<Rect> painted;  // 上一张位图里画过的像素矩形（用于局部清零）

    std::vector<unsigned char> buffer;  // BGRA，预乘 alpha，top-down
    int bufferWidth = 0;
    int bufferHeight = 0;
    unsigned generation = 0;
    std::size_t signature = 0;
    bool haveSignature = false;
    unsigned emptyFrames = 0;

    Stats stats;
};

State& state() {
    static State instance;
    return instance;
}

std::size_t mix(std::size_t hash, std::uint64_t value) {
    // FNV-1a 64 位。够用且不依赖 <functional> 的实现细节。
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= static_cast<std::size_t>((value >> (byte * 8)) & 0xFFu);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::size_t signatureOf(const State& s) {
    std::size_t hash = 1469598103934665603ull;
    hash = mix(hash, static_cast<std::uint64_t>(s.surfaceWidth));
    hash = mix(hash, static_cast<std::uint64_t>(s.surfaceHeight));
    hash = mix(hash, static_cast<std::uint64_t>(std::lround(s.scale * 1000.0)));
    hash = mix(hash, static_cast<std::uint64_t>(s.runs.size()));
    for (const Run& run : s.runs) {
        for (wchar_t unit : run.text) {
            hash = mix(hash, static_cast<std::uint64_t>(unit));
        }
        for (wchar_t unit : run.face.face) {
            hash = mix(hash, static_cast<std::uint64_t>(unit));
        }
        hash = mix(hash, static_cast<std::uint64_t>(run.x));
        hash = mix(hash, static_cast<std::uint64_t>(run.y));
        hash = mix(hash, static_cast<std::uint64_t>(run.cellWidth));
        hash = mix(hash, static_cast<std::uint64_t>(run.cellHeight));
        hash = mix(hash, static_cast<std::uint64_t>(run.fg));
        hash = mix(hash, static_cast<std::uint64_t>(run.bg));
        hash = mix(hash, static_cast<std::uint64_t>(run.bkMode));
        hash = mix(hash, static_cast<std::uint64_t>(run.face.weight));
        hash = mix(hash, static_cast<std::uint64_t>(run.face.italic ? 1 : 0));
        hash = mix(hash, static_cast<std::uint64_t>(run.hasClip ? 1 : 0));
        if (run.hasClip) {
            hash = mix(hash, static_cast<std::uint64_t>(run.clipLeft));
            hash = mix(hash, static_cast<std::uint64_t>(run.clipTop));
            hash = mix(hash, static_cast<std::uint64_t>(run.clipRight));
            hash = mix(hash, static_cast<std::uint64_t>(run.clipBottom));
        }
    }
    return hash;
}

int scaled(int value, double scale) {
    return static_cast<int>(std::lround(static_cast<double>(value) * scale));
}

// 一像素 src-over（两者都是预乘 BGRA）。
void blendPixel(unsigned char* dest, const unsigned char* src) {
    const unsigned alpha = src[3];
    if (alpha == 0) return;
    if (alpha == 255) {
        dest[0] = src[0];
        dest[1] = src[1];
        dest[2] = src[2];
        dest[3] = 255;
        return;
    }
    const unsigned inverse = 255u - alpha;
    dest[0] = static_cast<unsigned char>(src[0] + (dest[0] * inverse) / 255u);
    dest[1] = static_cast<unsigned char>(src[1] + (dest[1] * inverse) / 255u);
    dest[2] = static_cast<unsigned char>(src[2] + (dest[2] * inverse) / 255u);
    dest[3] = static_cast<unsigned char>(alpha + (dest[3] * inverse) / 255u);
}

void clearRect(State& s, Rect rect) {
    rect.left = std::max(0, std::min(rect.left, s.bufferWidth));
    rect.right = std::max(0, std::min(rect.right, s.bufferWidth));
    rect.top = std::max(0, std::min(rect.top, s.bufferHeight));
    rect.bottom = std::max(0, std::min(rect.bottom, s.bufferHeight));
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    const std::size_t rowBytes = static_cast<std::size_t>(rect.right - rect.left) * 4u;
    for (int y = rect.top; y < rect.bottom; ++y) {
        std::memset(s.buffer.data()
                        + static_cast<std::size_t>(y) * static_cast<std::size_t>(s.bufferWidth) * 4u
                        + static_cast<std::size_t>(rect.left) * 4u,
                    0, rowBytes);
    }
}

// 一个 run 的像素包围盒：1x 的格子按 scale 放大，再并上 clip。
Rect boundsOf(const State& s, const Run& run) {
    const int advance = text::runAdvance(run.text.c_str(),
        static_cast<int>(run.text.size()), run.cellWidth, run.cellHeight);
    Rect rect;
    rect.left = scaled(run.x, s.scale);
    rect.top = scaled(run.y, s.scale);
    rect.right = scaled(run.x + std::max(0, advance), s.scale) + 2;
    rect.bottom = scaled(run.y + std::max(0, run.cellHeight), s.scale) + 2;
    if (run.hasClip) {
        rect.left = std::max(rect.left, scaled(run.clipLeft, s.scale));
        rect.top = std::max(rect.top, scaled(run.clipTop, s.scale));
        rect.right = std::min(rect.right, scaled(run.clipRight, s.scale));
        rect.bottom = std::min(rect.bottom, scaled(run.clipBottom, s.scale));
    }
    return rect;
}

bool ensureBuffer(State& s) {
    if (s.surfaceWidth <= 0 || s.surfaceHeight <= 0 || s.scale <= 0.0) return false;
    const int width = scaled(s.surfaceWidth, s.scale);
    const int height = scaled(s.surfaceHeight, s.scale);
    if (width <= 0 || height <= 0) return false;
    if (width == s.bufferWidth && height == s.bufferHeight && !s.buffer.empty()) return true;
    s.buffer.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0);
    s.bufferWidth = width;
    s.bufferHeight = height;
    s.painted.clear();
    return true;
}

std::uint32_t colorAt(std::uint32_t argb) {
    // 0xAARRGGBB -> 需要的只是三个通道，alpha 由覆盖度决定。
    return argb & 0x00FFFFFFu;
}

void paintRun(State& s, const Run& run) {
    if (run.text.empty()) return;
    const int cellHeight = scaled(run.cellHeight, s.scale);
    if (cellHeight <= 0) return;

    const std::uint32_t fg = colorAt(run.fg);
    const std::uint32_t bg = colorAt(run.bg);
    const bool opaque = run.bkMode == 2;

    Rect runBounds = boundsOf(s, run);
    s.painted.push_back(runBounds);

    int penX = scaled(run.x, s.scale);
    const int top = scaled(run.y, s.scale);
    const int count = static_cast<int>(run.text.size());
    for (int index = 0; index < count;) {
        const char32_t codePoint = text::decodeCodePoint(run.text.c_str(), index, count);
        const int units = text::codePointLength(run.text.c_str(), index, count);
        const int advance1x = text::glyphAdvance(codePoint, run.cellWidth, run.cellHeight);
        const int advance = scaled(advance1x, s.scale);

        std::vector<unsigned char> coverage;
        const bool haveGlyph = text::rasterizeGlyphCoverage(
            codePoint, run.face, advance, cellHeight, coverage);
        ++s.stats.glyphs;
        if (!haveGlyph) ++s.stats.missing;

        const int glyphWidth = haveGlyph ? advance : std::max(1, advance);
        for (int row = 0; row < cellHeight; ++row) {
            const int y = top + row;
            if (y < 0 || y >= s.bufferHeight) continue;
            if (run.hasClip) {
                const int clipTop = scaled(run.clipTop, s.scale);
                const int clipBottom = scaled(run.clipBottom, s.scale);
                if (y < clipTop || y >= clipBottom) continue;
            }
            unsigned char* destRow = s.buffer.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(s.bufferWidth) * 4u;
            const std::size_t coverageRow = static_cast<std::size_t>(row)
                * static_cast<std::size_t>(glyphWidth);
            for (int column = 0; column < glyphWidth; ++column) {
                const int x = penX + column;
                if (x < 0 || x >= s.bufferWidth) continue;
                if (run.hasClip) {
                    const int clipLeft = scaled(run.clipLeft, s.scale);
                    const int clipRight = scaled(run.clipRight, s.scale);
                    if (x < clipLeft || x >= clipRight) continue;
                }
                unsigned char* pixel = destRow + static_cast<std::size_t>(x) * 4u;
                if (opaque) {
                    // GDI 的 OPAQUE 是"先把格子铺成背景色"，再叠字。这里按
                    // 像素顺序做同样的事：先写不透明背景，再 src-over 字形。
                    pixel[0] = static_cast<unsigned char>(bg & 0xFFu);
                    pixel[1] = static_cast<unsigned char>((bg >> 8) & 0xFFu);
                    pixel[2] = static_cast<unsigned char>((bg >> 16) & 0xFFu);
                    pixel[3] = 255;
                }
                if (!haveGlyph || coverage.empty()) continue;
                const unsigned alpha = coverage[coverageRow + static_cast<std::size_t>(column)];
                if (alpha == 0) continue;
                const unsigned char src[4] = {
                    static_cast<unsigned char>(((fg & 0xFFu) * alpha) / 255u),
                    static_cast<unsigned char>((((fg >> 8) & 0xFFu) * alpha) / 255u),
                    static_cast<unsigned char>((((fg >> 16) & 0xFFu) * alpha) / 255u),
                    static_cast<unsigned char>(alpha),
                };
                blendPixel(pixel, src);
            }
        }
        penX += advance;
        index += units;
    }
}

// 重建整张位图：清掉上一张的脏区，再按当前 runs 重画。
// 自证：把这一张位图原样写成 PNG。
//
// 为什么需要它：屏幕录制权限在本机没有授权，整屏截图会返回全黑；而文字层
// 恰恰是"看起来有没有变清晰"唯一能判的东西。dump 出的是**要贴上去的那块内存
// 本身**（预乘 BGRA 原样），所以它同时证明了通道次序、透明底和字形的灰度边缘
// ——三件在截图上都会混在一起看不清的事。
void dumpPng(const State& s) {
#if defined(__APPLE__)
    if (s.buffer.empty() || s.bufferWidth <= 0 || s.bufferHeight <= 0) return;
    static const char* directory = std::getenv("CONCORD_TEXTLAYER_DUMP");
    if (directory == nullptr || directory[0] == '\0') return;
    const std::string path = std::string(directory) + "/textlayer.png";

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    if (space == nullptr) return;
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, s.buffer.data(), s.buffer.size(), nullptr);
    CGImageRef image = provider != nullptr
        ? CGImageCreate(static_cast<std::size_t>(s.bufferWidth),
                        static_cast<std::size_t>(s.bufferHeight), 8, 32,
                        static_cast<std::size_t>(s.bufferWidth) * 4u, space,
                        static_cast<CGBitmapInfo>(
                            static_cast<std::uint32_t>(kCGBitmapByteOrder32Little)
                            | static_cast<std::uint32_t>(kCGImageAlphaPremultipliedFirst)),
                        provider, nullptr, false, kCGRenderingIntentDefault)
        : nullptr;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr,
        reinterpret_cast<const UInt8*>(path.c_str()), static_cast<CFIndex>(path.size()), false);
    CGImageDestinationRef destination = (image != nullptr && url != nullptr)
        ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr) : nullptr;
    if (destination != nullptr) {
        CGImageDestinationAddImage(destination, image, nullptr);
        const bool ok = CGImageDestinationFinalize(destination);
        std::fprintf(stderr, "[textlayer] dump %s %s (%dx%d)\n", path.c_str(),
                     ok ? "ok" : "failed", s.bufferWidth, s.bufferHeight);
        CFRelease(destination);
    }
    if (url != nullptr) CFRelease(url);
    if (image != nullptr) CGImageRelease(image);
    if (provider != nullptr) CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
#endif
}

// 重建整张位图：清掉上一张的脏区，再按当前 runs 重画。
void rebuild(State& s) {
    if (!ensureBuffer(s)) return;
    for (const Rect& rect : s.painted) clearRect(s, rect);
    s.painted.clear();
    s.stats.runs = static_cast<unsigned>(s.runs.size());
    s.stats.glyphs = 0;
    s.stats.missing = 0;
    for (const Run& run : s.runs) paintRun(s, run);
    ++s.generation;
    ++s.stats.rebuilds;
    std::fprintf(stderr, "[textlayer] rebuild #%u runs=%u glyphs=%u missing=%u %dx%d\n",
                 s.stats.rebuilds, s.stats.runs, s.stats.glyphs, s.stats.missing,
                 s.bufferWidth, s.bufferHeight);
    dumpPng(s);
}

}  // namespace

bool captureEnabled() {
    return state().capture;
}

void setCaptureEnabled(bool enabled) {
    state().capture = enabled;
}

void beginFrame() {
    state().runs.clear();
}

void record(const Run& run) {
    state().runs.push_back(run);
}

std::size_t runCount() {
    return state().runs.size();
}

void setSurfaceSize(int width, int height) {
    State& s = state();
    if (width == s.surfaceWidth && height == s.surfaceHeight) return;
    s.surfaceWidth = width;
    s.surfaceHeight = height;
    s.painted.clear();
    s.haveSignature = false;
    // 表面尺寸变了，旧位图整张作废。
    s.buffer.clear();
    s.bufferWidth = 0;
    s.bufferHeight = 0;
}

void setPixelScale(double scale) {
    State& s = state();
    if (!(scale > 0.0)) scale = 0.0;
    s.stats.scale = scale;
    // <= 1.01（1x 屏）没有意义：1x 屏上帧缓冲本来就是物理像素，文字层只会多一次
    // 无收益的合成，还多一条与 DIB 抢像素的路径。
    const bool wanted = scale > 1.01;
    if (wanted != s.capture) {
        s.capture = wanted;
        std::fprintf(stderr, "[textlayer] capture=%d scale=%.2f\n", wanted ? 1 : 0, scale);
    }
    if (std::fabs(scale - s.scale) > 0.001) {
        s.scale = scale;
        s.haveSignature = false;
        s.buffer.clear();
        s.bufferWidth = 0;
        s.bufferHeight = 0;
    }
}

double pixelScale() {
    return state().scale;
}

Bitmap bitmap() {
    State& s = state();
    Bitmap result;
    if (!s.capture) {
        s.runs.clear();
        return result;
    }

    if (s.runs.empty() && s.bufferWidth > 0) {
        // 这一帧没有登记任何文字：保留上一张（见文件头的取舍 3）。
        //
        // 但"连续空帧"必须能收敛到空白——切到一个真的没有系统字体文字的界面
        // （载入页、过场）时，上一帧的 tooltip 会一直挂在屏幕上。三帧是判据：
        // 整帧没重绘最多持续一两帧，连续三帧空只可能是界面上真的没有字。
        if (++s.emptyFrames >= 3) {
            s.emptyFrames = 0;
            for (const Rect& rect : s.painted) clearRect(s, rect);
            s.painted.clear();
            ++s.generation;
            s.haveSignature = false;
        }
        result.pixels = s.buffer.data();
        result.width = s.bufferWidth;
        result.height = s.bufferHeight;
        result.bytesPerRow = s.bufferWidth * 4;
        result.generation = s.generation;
        return result;
    }
    s.emptyFrames = 0;
    const std::size_t signature = signatureOf(s);
    if (s.haveSignature && signature != s.signature
        && std::getenv("CONCORD_TEXTLAYER_TRACE") != nullptr) {
        const Run& first = s.runs.front();
        std::fprintf(stderr,
            "[textlayer] sig %zx->%zx runs=%zu scale=%.4f surf=%dx%d"
            " r0={x=%d y=%d w=%d h=%d fg=%08x bk=%d clip=%d}\n",
            s.signature, signature, s.runs.size(), s.scale,
            s.surfaceWidth, s.surfaceHeight, first.x, first.y,
            first.cellWidth, first.cellHeight, first.fg, first.bkMode,
            first.hasClip ? 1 : 0);
    }
    if (!s.haveSignature || signature != s.signature || s.buffer.empty()) {
        s.signature = signature;
        s.haveSignature = true;
        rebuild(s);
    }
    s.runs.clear();
    if (s.buffer.empty()) return result;
    result.pixels = s.buffer.data();
    result.width = s.bufferWidth;
    result.height = s.bufferHeight;
    result.bytesPerRow = s.bufferWidth * 4;
    result.generation = s.generation;
    return result;
}

Stats stats() {
    return state().stats;
}

}  // namespace concord::win32compat::textlayer

// ---------------------------------------------------------------------------
// Cocoa 呈现层的入口（头文件里只出现裸指针和 int，理由见 RuntimeAppleWindowSurface.h）
// ---------------------------------------------------------------------------

namespace concord_compat::apple {

TextLayer updateTextLayer(int surfaceWidth, int surfaceHeight, double scale) {
    using namespace concord::win32compat;
    textlayer::setSurfaceSize(surfaceWidth, surfaceHeight);
    textlayer::setPixelScale(scale);
    const textlayer::Bitmap source = textlayer::bitmap();
    TextLayer layer;
    layer.pixels = source.pixels;
    layer.width = source.width;
    layer.height = source.height;
    layer.bytesPerRow = source.bytesPerRow;
    layer.generation = source.generation;
    return layer;
}

}  // namespace concord_compat::apple
