// Glyph backend for the Win32 compatibility layer (S4-2).
//
// Scope: turn a Unicode code point into a 1-bit coverage mask sized to the
// advance box, on macOS, without any game asset. The rasteriser is CoreText via
// CoreGraphics, one glyph at a time:
//
//   LOGFONTW face name --> installed macOS family --> CTFontRef (cached)
//   code point         --> CGGlyph (primary font, then CoreText cascade)
//   CGGlyph            --> 8-bit gray scratch buffer --> threshold --> 1-bit mask
//   mask               --> advance box (compress only when the ink overflows it)
//
// Four decisions that are not obvious from the code:
//
// 1. **One glyph at a time, not a whole CTLine.** A CTLine would give proper
//    kerning, but it also takes the advance away from the fixed layout grid that
//    fontMetricsFor() defines. Per-glyph rasterisation keeps "measure == draw"
//    exactly true for any string, including CJK mixed with Latin.
//
// 2. **Cascade per code point.** CTFontCreateForString on the *whole* string
//    returns the font for the string's first run, so "Text 中文 A" would get a
//    Latin font and render three .notdef boxes for the Chinese (measured: that is
//    what the first prototype did). Asking per code point is the only way to get
//    both scripts right in one run.
//
// 3. **Font size is chosen so ascent + descent == cell height.** That is the
//    GDI semantic for a negative lfHeight ("character height"), and it is what
//    keeps a 16px cell from clipping ascenders. The size therefore differs from
//    the nominal height (Helvetica at a 16px cell lands around 13.8pt).
//
// 4. **Threshold at 50%, no font smoothing.** See the header: XOR text must be
//    exactly revertible, and 1-bit keeps that. Antialiasing is on (it improves
//    the binarised shape), font smoothing is explicitly off so the result does
//    not depend on the "smooth fonts" user setting.

#include "RuntimeWin32CompatTextInternal.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace concord::win32compat::text {
namespace {

// --- East Asian width -------------------------------------------------------

struct CodePointRange {
    char32_t first;
    char32_t last;
};

// Unicode "East Asian Wide / Fullwidth" blocks that matter for a Red Alert 2
// port: CJK ideographs, kana, hangul, CJK punctuation, fullwidth forms, and the
// emoji planes (which arrive as single wchar_t units on macOS).
constexpr CodePointRange kWideRanges[] = {
    {0x1100, 0x115F}, {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF},
    {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60},
    {0xFFE0, 0xFFE6}, {0x1F300, 0x1F64F}, {0x1F900, 0x1F9FF}, {0x20000, 0x3FFFD},
};

constexpr unsigned kMissingGlyph = 0xFFFFu;  // == GGI_MARK_NONEXISTING_GLYPHS
constexpr int kInkThreshold = 128;           // 50% coverage, see file header (4)
constexpr int kScratchPad = 4;               // room for overhang/accents

// --- face name mapping ------------------------------------------------------
//
// The product names Windows faces ("Microsoft YaHei", "Arial", "Arial Narrow").
// macOS has none of them. Mapping is by intent rather than by spelling: a UI
// sans maps to the system sans, a CJK UI face maps to PingFang (which also
// covers Latin, so mixed runs stay in one family), and an unknown name is passed
// through -- CTFontCreateWithName never fails, it returns a default face.

std::wstring lowercase(std::wstring value) {
    for (wchar_t& c : value) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return value;
}

bool contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

std::wstring installedFamilyFor(const std::wstring& face) {
    const std::wstring key = lowercase(face);
    if (key.empty()) return L"Helvetica";
    if (contains(key, L"yahei") || contains(key, L"微软雅黑")) return L"PingFang SC";
    if (contains(key, L"simsun") || contains(key, L"宋体")) return L"Songti SC";
    if (contains(key, L"dengxian") || contains(key, L"等线")) return L"PingFang SC";
    // "Arial Narrow" is a condensed UI face; Helvetica Neue is the closest match
    // that still has both weights.
    if (contains(key, L"narrow")) return L"Helvetica Neue";
    if (contains(key, L"arial") || contains(key, L"helvetica")
        || contains(key, L"shell dlg") || contains(key, L"tahoma")
        || contains(key, L"segoe") || contains(key, L"verdana")
        || contains(key, L"ms sans serif")) {
        return L"Helvetica";
    }
    return face;
}

#if defined(__APPLE__)

CFStringRef cfStringFromWide(const std::wstring& text) {
    std::vector<UniChar> units;
    units.reserve(text.size());
    for (wchar_t unit : text) {
        char32_t cp = static_cast<char32_t>(unit);
        if (cp > 0xFFFFu) {  // macOS wchar_t is 32-bit; CoreText wants UTF-16
            cp -= 0x10000u;
            units.push_back(static_cast<UniChar>(0xD800u + (cp >> 10)));
            units.push_back(static_cast<UniChar>(0xDC00u + (cp & 0x3FFu)));
        } else {
            units.push_back(static_cast<UniChar>(cp));
        }
    }
    return CFStringCreateWithCharacters(nullptr, units.data(),
        static_cast<CFIndex>(units.size()));
}

// UTF-16 representation of one code point, for cmap lookups and CTFontCreateForString.
int utf16Units(char32_t cp, UniChar out[2]) {
    if (cp > 0xFFFFu) {
        cp -= 0x10000u;
        out[0] = static_cast<UniChar>(0xD800u + (cp >> 10));
        out[1] = static_cast<UniChar>(0xDC00u + (cp & 0x3FFu));
        return 2;
    }
    out[0] = static_cast<UniChar>(cp);
    return 1;
}

// --- font cache -------------------------------------------------------------

struct FaceKey {
    std::wstring face;
    int weight = 400;
    bool italic = false;
    int tall = 16;

    bool operator==(const FaceKey& other) const {
        return weight == other.weight && italic == other.italic && tall == other.tall
            && face == other.face;
    }
};

struct FaceKeyHash {
    std::size_t operator()(const FaceKey& key) const {
        std::size_t h = std::hash<std::wstring>{}(key.face);
        h ^= static_cast<std::size_t>(key.weight) * 1099511628211ull;
        h ^= static_cast<std::size_t>(key.tall) * 1099511628211ull;
        if (key.italic) h ^= 0x9E3779B97F4A7C15ull;
        return h;
    }
};

// "这个字体什么都覆盖"的判据：它是否声称自己有 U+0378。
//
// U+0378 在 Unicode 里是**未分配**码位，任何真实文字字体都不该有它的字形。
// 实测：CoreText 对缺失字符会级联到一个兜底字体（LastResort 一类），后者
// 的覆盖集声称包含全部码位，并把它们统统映射到 .notdef 字形（Helvetica 上
// 是字形 4）。不做这一步，GetGlyphIndicesW 对任何码位都会报一个"真实"索引，
// 缺字形检测就彻底失效了——而那正是 RuntimePlayerDefeatNoticeSmoke 要问的。
bool faceIsCatchAll(CFCharacterSetRef coverage) {
    return coverage != nullptr
        && CFCharacterSetIsLongCharacterMember(coverage, static_cast<UTF32Char>(0x0378));
}

struct FaceEntry {
    CTFontRef font = nullptr;            // owned; sized so ascent + descent == cell height
    CFCharacterSetRef coverage = nullptr;  // owned; the code points this face really has
    bool catchAll = false;                 // claims to cover even unassigned code points
    double size = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
};

std::unordered_map<FaceKey, FaceEntry, FaceKeyHash>& faces() {
    static std::unordered_map<FaceKey, FaceEntry, FaceKeyHash> cache;
    return cache;
}

void releaseFaces() {
    auto& cache = faces();
    for (auto& entry : cache) {
        if (entry.second.font != nullptr) CFRelease(entry.second.font);
        if (entry.second.coverage != nullptr) CFRelease(entry.second.coverage);
    }
    cache.clear();
}

// Resolve (and cache) the font for a face at one cell height.
const FaceEntry* faceFor(const FaceSpec& spec, int tall) {
    FaceKey key{spec.face, spec.weight, spec.italic, tall};
    auto& cache = faces();
    const auto found = cache.find(key);
    if (found != cache.end()) return &found->second;
    // Bound the cache: a runaway caller (a new face per draw) must not leak
    // hundreds of CTFontRefs. 64 faces is far beyond what the product uses (4).
    if (cache.size() >= 64) releaseFaces();

    const std::wstring family = installedFamilyFor(spec.face);
    CFStringRef cfFamily = cfStringFromWide(family);
    CTFontRef font = cfFamily != nullptr
        ? CTFontCreateWithName(cfFamily, static_cast<CGFloat>(tall), nullptr) : nullptr;
    if (cfFamily != nullptr) CFRelease(cfFamily);
    if (font == nullptr) return nullptr;

    // Weight/italic: ask for the real styled face; if the family has none,
    // CTFontCreateCopyWithSymbolicTraits returns NULL and we keep the regular one
    // rather than faking a slant with a transform (a synthetic slant would move
    // ink outside the advance box the layout promised).
    CTFontSymbolicTraits want = 0;
    if (spec.weight >= 600) want |= kCTFontBoldTrait;
    if (spec.italic) want |= kCTFontItalicTrait;
    if (want != 0) {
        CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(
            font, static_cast<CGFloat>(tall), nullptr, want, want);
        if (styled != nullptr) {
            CFRelease(font);
            font = styled;
        }
    }

    // Size so that ascent + descent == cell height (GDI negative-lfHeight
    // semantic). One step is enough: both quantities scale linearly with size.
    const double nominal = static_cast<double>(tall);
    const double naiveSum = static_cast<double>(CTFontGetAscent(font))
        + static_cast<double>(CTFontGetDescent(font));
    double size = nominal;
    if (naiveSum > 0.5) size = nominal * nominal / naiveSum;
    size = std::max(1.0, std::min(size, nominal * 4.0));
    CTFontRef sized = CTFontCreateCopyWithAttributes(font, static_cast<CGFloat>(size),
        nullptr, nullptr);
    if (sized != nullptr) {
        CFRelease(font);
        font = sized;
    }

    FaceEntry entry;
    entry.font = font;
    // 覆盖集是"这个字体真的有什么码点"的权威答案。少了它，未分配码位会被
    // cmap 映射到 .notdef 字形并算作"有字形"——实测 Helvetica 对所有未分配
    // 码位都返回字形 4，于是 GetGlyphIndicesW 会报一个假的真实索引，
    // 而屏幕上是 CoreText 自己的空心框。两者都错得不明显。
    entry.coverage = CTFontCopyCharacterSet(font);
    entry.catchAll = faceIsCatchAll(entry.coverage);
    entry.size = size;
    entry.ascent = static_cast<double>(CTFontGetAscent(font));
    entry.descent = static_cast<double>(CTFontGetDescent(font));
    return &cache.emplace(key, entry).first->second;
}

// 一个字形的 cmap 查询：先看覆盖集，再取字形索引。
//
// 顺序很重要。只看 CTFontGetGlyphsForCharacters 是不够的——它对未分配码位也
// 会"成功"（cmap 把它们映射到 .notdef 字形 4），于是缺字形会被当成有字形。
CGGlyph glyphFromFace(CTFontRef font, CFCharacterSetRef coverage, bool catchAll, char32_t cp) {
    if (catchAll) return 0;  // 兜底字体：它"有"的每个字形都是占位方块
    if (coverage != nullptr
        && !CFCharacterSetIsLongCharacterMember(coverage, static_cast<UTF32Char>(cp))) {
        return 0;
    }
    UniChar units[2];
    const int count = utf16Units(cp, units);
    CGGlyph glyphs[2] = {0, 0};
    if (!CTFontGetGlyphsForCharacters(font, units, glyphs, count)) return 0;
    return glyphs[0];
}

// Glyph lookup with per-code-point cascade. `cascade` is set (and owned by the
// caller) only when the primary font could not render the code point.
CGGlyph lookupGlyph(const FaceEntry& entry, char32_t cp, CTFontRef* cascade) {
    *cascade = nullptr;
    if (const CGGlyph glyph = glyphFromFace(entry.font, entry.coverage, entry.catchAll, cp)) {
        return glyph;
    }
    UniChar units[2];
    const int count = utf16Units(cp, units);
    CFStringRef text = CFStringCreateWithCharacters(nullptr, units, count);
    if (text == nullptr) return 0;
    CTFontRef alt = CTFontCreateForString(entry.font, text, CFRangeMake(0, count));
    CFRelease(text);
    if (alt == nullptr) return 0;
    CFCharacterSetRef altCoverage = CTFontCopyCharacterSet(alt);
    const CGGlyph glyph = glyphFromFace(alt, altCoverage, faceIsCatchAll(altCoverage), cp);
    if (altCoverage != nullptr) CFRelease(altCoverage);
    if (glyph != 0) {
        *cascade = alt;
        return glyph;
    }
    CFRelease(alt);
    return 0;
}

// --- rasterisation ----------------------------------------------------------

// Draw one glyph into a scratch gray buffer and copy the thresholded ink into
// `out`, an advance-sized mask. Returns false if no ink was produced.
void rasterInto(CTFontRef font, double ascent, CGGlyph glyph, int cellWidth,
                int cellHeight, GlyphMask& out) {
    out.width = cellWidth;
    out.height = cellHeight;
    out.ink.assign(static_cast<std::size_t>(cellWidth) * static_cast<std::size_t>(cellHeight), 0);

    const int scratchW = cellWidth + kScratchPad * 2 + 8;
    const int scratchH = cellHeight + kScratchPad * 2;
    std::vector<unsigned char> buffer(
        static_cast<std::size_t>(scratchW) * static_cast<std::size_t>(scratchH), 0);

    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    if (gray == nullptr) return;
    CGContextRef context = CGBitmapContextCreate(buffer.data(), scratchW, scratchH, 8,
        static_cast<std::size_t>(scratchW), gray, kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    if (context == nullptr) return;

    CGContextSetShouldAntialias(context, true);
    CGContextSetShouldSmoothFonts(context, false);  // decision (4) in the header
    CGContextSetGrayFillColor(context, 1.0, 1.0);
    CGContextSetTextMatrix(context, CGAffineTransformIdentity);
    // CoreGraphics' origin is bottom-left; the baseline sits `ascent` below the
    // top of the scratch box.
    const double baselineFromTop = static_cast<double>(kScratchPad) + ascent;
    const CGPoint position = CGPointMake(static_cast<CGFloat>(kScratchPad),
        static_cast<CGFloat>(static_cast<double>(scratchH) - baselineFromTop));
    CTFontDrawGlyphs(font, &glyph, &position, 1, context);
    CGContextRelease(context);

    // Ink bounding box in top-down scratch coordinates.
    //
    // Row 0 of a CGBitmapContext's buffer is the *top* row (verified with a
    // control fill at CG y=0, which lands on the last row), so no flip is needed
    // here. Getting this backwards is silent: a vertically mirrored glyph still
    // "has ink", and every bbox-derived position is then wrong by twice the
    // distance to the centre -- the first prototype placed 'e' on the cap line.
    int minX = scratchW;
    int minY = scratchH;
    int maxX = -1;
    int maxY = -1;
    for (int row = 0; row < scratchH; ++row) {
        const unsigned char* line = buffer.data()
            + static_cast<std::size_t>(row) * static_cast<std::size_t>(scratchW);
        for (int column = 0; column < scratchW; ++column) {
            if (line[column] < kInkThreshold) continue;
            minX = std::min(minX, column);
            maxX = std::max(maxX, column);
            minY = std::min(minY, row);
            maxY = std::max(maxY, row);
        }
    }
    if (maxX < 0) return;  // blank glyph (space): the mask is the background cell

    const int inkWidth = maxX - minX + 1;
    const int inkHeight = maxY - minY + 1;
    // Where the ink would sit if placed 1:1 (the scratch pad cancels the offset).
    const int originX = minX - kScratchPad;
    const int originY = minY - kScratchPad;

    if (inkWidth <= cellWidth && inkHeight <= cellHeight) {
        for (int row = 0; row < inkHeight; ++row) {
            const int destY = originY + row;
            if (destY < 0 || destY >= cellHeight) continue;
            const unsigned char* line = buffer.data()
                + static_cast<std::size_t>(minY + row)
                * static_cast<std::size_t>(scratchW) + minX;
            for (int column = 0; column < inkWidth; ++column) {
                const int destX = originX + column;
                if (destX < 0 || destX >= cellWidth) continue;
                if (line[column] >= kInkThreshold) {
                    out.ink[static_cast<std::size_t>(destY) * static_cast<std::size_t>(cellWidth)
                        + static_cast<std::size_t>(destX)] = 1;
                }
            }
        }
        return;
    }

    // Overflow: a proportional serif/sans at this size can be a pixel wider than
    // the 5:7 grid allows (measured: Helvetica 'M' and 'W'). Compress uniformly
    // rather than clip, because a clipped 'M' reads as a different letter.
    double scale = 1.0;
    if (inkWidth > cellWidth) scale = std::min(scale, static_cast<double>(cellWidth) / inkWidth);
    if (inkHeight > cellHeight) scale = std::min(scale, static_cast<double>(cellHeight) / inkHeight);
    const int fittedW = std::max(1, static_cast<int>(inkWidth * scale));
    const int fittedH = std::max(1, static_cast<int>(inkHeight * scale));
    const int baseX = std::max(0, std::min(originX, cellWidth - fittedW));
    const int baseY = std::max(0, std::min(originY, cellHeight - fittedH));
    for (int row = 0; row < fittedH; ++row) {
        const int sourceY = minY + row * inkHeight / fittedH;
        const unsigned char* line = buffer.data()
            + static_cast<std::size_t>(sourceY)
            * static_cast<std::size_t>(scratchW);
        for (int column = 0; column < fittedW; ++column) {
            const int sourceX = minX + column * inkWidth / fittedW;
            if (line[sourceX] < kInkThreshold) continue;
            out.ink[static_cast<std::size_t>(baseY + row) * static_cast<std::size_t>(cellWidth)
                + static_cast<std::size_t>(baseX + column)] = 1;
        }
    }
}

// --- mask cache -------------------------------------------------------------

struct MaskKey {
    FaceKey face;
    char32_t codePoint = 0;
    int narrow = 0;

    bool operator==(const MaskKey& other) const {
        return codePoint == other.codePoint && narrow == other.narrow && face == other.face;
    }
};

struct MaskKeyHash {
    std::size_t operator()(const MaskKey& key) const {
        std::size_t h = FaceKeyHash{}(key.face);
        h ^= static_cast<std::size_t>(key.codePoint) * 1099511628211ull;
        h ^= static_cast<std::size_t>(key.narrow) * 0x9E3779B97F4A7C15ull;
        return h;
    }
};

std::unordered_map<MaskKey, GlyphMask, MaskKeyHash>& masks() {
    static std::unordered_map<MaskKey, GlyphMask, MaskKeyHash> cache;
    return cache;
}

// A frame repaints the same few hundred code points every time; re-rasterising
// them per frame would be the single most expensive thing in the compat layer.
constexpr std::size_t kMaskCacheLimit = 8192;

#endif  // __APPLE__

}  // namespace

// --- public API -------------------------------------------------------------

bool isWideCodePoint(char32_t cp) noexcept {
    for (const CodePointRange& range : kWideRanges) {
        if (cp >= range.first && cp <= range.last) return true;
    }
    return false;
}

int glyphAdvance(char32_t cp, int narrow, int tall) noexcept {
    if (narrow <= 0) narrow = 1;
    if (tall <= 0) tall = 1;
    // Control characters occupy the narrow cell: GDI advances them too, and a
    // caller that draws "\r\n" by accident should not get full-width gaps.
    return isWideCodePoint(cp) ? tall : narrow;
}

int codePointLength(const wchar_t* text, int index, int count) noexcept {
    if (text == nullptr || index < 0 || index >= count) return 1;
    const char32_t cp = static_cast<char32_t>(text[index]);
    if (cp > 0xFFFFu) return 1;  // macOS wchar_t already carries the whole code point
    // Windows-shaped input (a real UTF-16 surrogate pair in wchar_t units) still
    // reaches this layer through string literals compiled elsewhere; treat it as
    // one code point so counters do not drift.
    if (cp >= 0xD800u && cp <= 0xDBFFu && index + 1 < count) {
        const char32_t low = static_cast<char32_t>(text[index + 1]);
        if (low >= 0xDC00u && low <= 0xDFFFu) return 2;
    }
    return 1;
}

char32_t decodeCodePoint(const wchar_t* text, int index, int count) noexcept {
    if (text == nullptr || index < 0 || index >= count) return 0;
    char32_t cp = static_cast<char32_t>(text[index]);
    if (cp > 0xFFFFu) return cp;
    if (cp >= 0xD800u && cp <= 0xDBFFu && index + 1 < count) {
        const char32_t low = static_cast<char32_t>(text[index + 1]);
        if (low >= 0xDC00u && low <= 0xDFFFu) {
            return 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
        }
    }
    return cp;
}

int runAdvance(const wchar_t* text, int count, int narrow, int tall) noexcept {
    if (text == nullptr || count <= 0) return 0;
    int total = 0;
    for (int index = 0; index < count;) {
        total += glyphAdvance(decodeCodePoint(text, index, count), narrow, tall);
        index += codePointLength(text, index, count);
    }
    return total;
}

bool rasterizeGlyph(char32_t cp, const FaceSpec& face, int narrow, int tall,
                    GlyphMask& out) {
    const int advance = glyphAdvance(cp, narrow, tall);
    out.width = advance;
    out.height = std::max(1, tall);
    out.ink.assign(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height), 0);
#if !defined(__APPLE__)
    (void)cp;
    (void)face;
    return false;
#else
    const FaceEntry* entry = faceFor(face, std::max(1, tall));
    if (entry == nullptr) return false;

    MaskKey key{FaceKey{face.face, face.weight, face.italic, std::max(1, tall)}, cp, narrow};
    auto& cache = masks();
    const auto found = cache.find(key);
    if (found != cache.end()) {
        out = found->second;
        return true;
    }
    if (cache.size() >= kMaskCacheLimit) cache.clear();

    CTFontRef cascade = nullptr;
    const CGGlyph glyph = lookupGlyph(*entry, cp, &cascade);
    if (glyph == 0) {
        if (cascade != nullptr) CFRelease(cascade);
        return false;
    }
    CGGlyph local = glyph;
    GlyphMask mask;
    if (cascade != nullptr) {
        // A cascaded face (CJK, emoji) carries its own ascent; CoreText created it
        // at the primary's size, so only the baseline offset differs. Measure it
        // from the cascade face so accents do not drift above the cell.
        rasterInto(cascade, static_cast<double>(CTFontGetAscent(cascade)), local,
            advance, std::max(1, tall), mask);
        CFRelease(cascade);
    } else {
        rasterInto(entry->font, entry->ascent, local, advance, std::max(1, tall), mask);
    }
    auto inserted = cache.emplace(key, std::move(mask));
    out = inserted.first->second;
    return true;
#endif
}

unsigned glyphIndex(char32_t cp, const FaceSpec& face, int tall) {
#if !defined(__APPLE__)
    (void)cp;
    (void)face;
    (void)tall;
    return kMissingGlyph;
#else
    const FaceEntry* entry = faceFor(face, std::max(1, tall));
    if (entry == nullptr) return kMissingGlyph;
    CTFontRef cascade = nullptr;
    const CGGlyph glyph = lookupGlyph(*entry, cp, &cascade);
    if (cascade != nullptr) CFRelease(cascade);
    return glyph == 0 ? kMissingGlyph : static_cast<unsigned>(glyph);
#endif
}

void resetCaches() {
#if defined(__APPLE__)
    releaseFaces();
    masks().clear();
#endif
}

}  // namespace concord::win32compat::text
