#pragma once
// Internal seam between the GDI raster path (RuntimeWin32CompatGdi.cpp) and the
// glyph backend (RuntimeWin32CompatText.cpp).
//
// Why a seam at all: clip state, ROP2 and background mode live in PixelSink in
// the GDI translation unit, and the glyph source (CoreText) must not learn about
// them. So the backend produces a 1-bit mask for one code point and the GDI side
// composites it. Two consequences worth stating up front:
//
//   * The mask is **1 bit per pixel**, not 8-bit coverage. GDI text on a XOR
//     (R2_XORPEN) path must be reversible "draw twice, surface restored", and a
//     partial-alpha blend has no exact inverse. Thresholding at 50% keeps ROP2
//     semantics intact at the cost of ClearType-grade smoothing.
//   * The advance of a glyph is **not** taken from the font. Layout stays on the
//     fixed grid that fontMetricsFor() already defines (5:7 for narrow code
//     points, 1:1 for East Asian wide ones), because five contractors and the
//     P5-06 metric assertions are built on "measure == draw" with that grid.
//     Windows uses proportional advances; this quantises them. Documented
//     deviation, not an oversight.
//
// This header deliberately does not include <windows.h>: the backend must be
// compilable without the Win32 shim (it is pure platform text rasterisation).

#include <cstdint>
#include <string>
#include <vector>

namespace concord::win32compat::text {

// One rasterised glyph, already sized to the advance box.
struct GlyphMask {
    int width = 0;   // advance box width in logical pixels (== this glyph's advance)
    int height = 0;  // cell height in logical pixels
    // rows top-down, one byte per pixel: 0 = background, 1 = ink. A space is a
    // valid mask with all-zero ink (it still occupies its background cell).
    std::vector<unsigned char> ink;

    unsigned char at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0;
        return ink[static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
            + static_cast<std::size_t>(x)];
    }
};

// The subset of LOGFONTW the backend cares about.
struct FaceSpec {
    std::wstring face;  // LOGFONTW.lfFaceName, may be empty or a Windows-only name
    int weight = 400;
    bool italic = false;
};

// East Asian wide (full-width) code points: these get a 1:1 cell instead of 5:7.
bool isWideCodePoint(char32_t cp) noexcept;

// Advance of one code point in logical pixels.
int glyphAdvance(char32_t cp, int narrow, int tall) noexcept;

// Sum of glyphAdvance over a run. `count` is in wchar_t units, as GDI counts them.
int runAdvance(const wchar_t* text, int count, int narrow, int tall) noexcept;

// Number of wchar_t units consumed by the code point starting at text[index].
// macOS wchar_t is 4 bytes, so a non-BMP code point arrives as a single unit
// (not a surrogate pair as on Windows) -- both shapes are handled.
int codePointLength(const wchar_t* text, int index, int count) noexcept;
char32_t decodeCodePoint(const wchar_t* text, int index, int count) noexcept;

// Rasterise one code point. Returns false when the backend has no glyph for it;
// the caller then draws the "missing glyph" box (the old tofu placeholder, which
// keeps its honest meaning: this glyph is genuinely absent).
bool rasterizeGlyph(char32_t cp, const FaceSpec& face, int narrow, int tall,
                    GlyphMask& out);

// Real glyph index, or 0xFFFF when the backend has no glyph. That is exactly the
// contract of GetGlyphIndicesW with GGI_MARK_NONEXISTING_GLYPHS.
unsigned glyphIndex(char32_t cp, const FaceSpec& face, int tall);

// HiDPI coverage rasterisation, for the on-screen text layer only.
//
// Same grid semantics as rasterizeGlyph, two differences:
//
//   * output is 8-bit coverage (0 = background, 255 = full ink) instead of a
//     50% threshold, so the result can be alpha-blended and stays smooth after
//     CoreAnimation maps it onto physical pixels;
//   * the cell is given in **pixels already scaled up**, and the font is looked
//     up at that cell height. A 2x request therefore rasterises a genuinely
//     larger glyph; it does not magnify the 1x mask.
//
// This deliberately does not feed the GDI path. The 1-bit mask is the contract
// that keeps "measure == draw", ROP2 and XOR-text reversibility true; a coverage
// output has no exact inverse and would break all three if it were allowed into
// drawTextRun's pixel path.
bool rasterizeGlyphCoverage(char32_t cp, const FaceSpec& face, int cellWidth,
                            int cellHeight, std::vector<unsigned char>& coverage);

// Test hook: drop cached fonts and masks (lets a test observe first-use cost and
// guarantees a deterministic starting state).
void resetCaches();

}  // namespace concord::win32compat::text
