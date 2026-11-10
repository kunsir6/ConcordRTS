// compat/win32 自测 · 对象管理 + 位图传输（S2）
//
// 与 CompatInlineSemantics.cpp 的分工：
//   * CompatInlineSemantics.cpp 只验证头层里那些 inline 的几何/环境工具；
//   * 本文件验证真正落在 .cpp 里的 GDI 实现，且断言的是**像素值**而不是返回值。
//
// 全部用例基于"构造已知输入 → 逐像素比对输出"，不使用近似或容差：这一层的目的
// 就是让 macOS 侧与 Windows 侧像素一致，任何容差都会掩盖真正的偏差。
//
// 构建与运行：
//   c++ -std=c++20 -I compat/win32/include \
//       compat/win32/src/RuntimeWin32CompatGdi.cpp \
//       compat/win32/selftest/CompatGdiSemantics.cpp -o /tmp/ccg && /tmp/ccg

#include <windows.h>

#include <cstdint>
#include <cstdio>
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

void checkEqual(std::uint32_t expected, std::uint32_t actual, const char* description) {
    ++gChecks;
    if (expected != actual) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected 0x%08X, got 0x%08X)\n", description, expected, actual);
    }
}

// 构造一个 32bpp、指定朝向的 DIB。返回位图与像素基址。
struct Dib {
    HBITMAP handle = nullptr;
    void* bits = nullptr;
    HDC dc = nullptr;

    bool create(int width, int height, bool topDown) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = topDown ? -height : height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        handle = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (handle == nullptr) return false;
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) return false;
        SelectObject(dc, reinterpret_cast<HGDIOBJ>(handle));
        return true;
    }

    ~Dib() {
        if (dc != nullptr) DeleteDC(dc);
        if (handle != nullptr) DeleteObject(reinterpret_cast<HGDIOBJ>(handle));
    }
};

// 直接按内存写像素（0xAARRGGBB），用于构造已知图案。
void pokeBits(void* bits, int stride, bool topDown, int height, int x, int y, std::uint32_t pixel) {
    const int row = topDown ? y : (height - 1 - y);
    auto* address = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(bits) +
                                                    static_cast<std::ptrdiff_t>(row) * stride + x * 4);
    *address = pixel;
}

std::uint32_t peekBits(void* bits, int stride, bool topDown, int height, int x, int y) {
    const int row = topDown ? y : (height - 1 - y);
    const auto* address = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(bits) +
                                                                static_cast<std::ptrdiff_t>(row) * stride + x * 4);
    return *address;
}

void testObjectLifecycle() {
    std::printf("[对象生命周期]\n");

    // 库存对象：稳定句柄、不可删除。
    HGDIOBJ white = GetStockObject(WHITE_BRUSH);
    HGDIOBJ black = GetStockObject(BLACK_BRUSH);
    check(white != nullptr && black != nullptr, "库存画刷句柄非空");
    check(white != black, "不同库存对象句柄不同");
    check(GetStockObject(WHITE_BRUSH) == white, "库存对象句柄跨调用稳定");
    check(DeleteObject(white) == FALSE, "库存对象删除失败");
    check(white == GetStockObject(WHITE_BRUSH), "删除失败后库存句柄仍有效");

    // 越界库存编号必须返回空，而不是假对象。
    check(GetStockObject(-1) == nullptr, "GetStockObject(-1) 返回空");
    check(GetStockObject(999) == nullptr, "GetStockObject(999) 返回空");

    HBRUSH brush = CreateSolidBrush(RGB(0x12, 0x34, 0x56));
    check(brush != nullptr, "CreateSolidBrush 成功");
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(0x65, 0x43, 0x21));
    check(pen != nullptr, "CreatePen 成功");

    Dib target;
    check(target.create(8, 8, true), "创建 8x8 顶向下 DIB");

    HGDIOBJ previousBrush = SelectObject(target.dc, reinterpret_cast<HGDIOBJ>(brush));
    check(previousBrush == white, "SelectObject(brush) 返回先前的库存白刷");
    HGDIOBJ previousPen = SelectObject(target.dc, reinterpret_cast<HGDIOBJ>(pen));
    check(previousPen == GetStockObject(BLACK_PEN), "SelectObject(pen) 返回先前的库存黑笔");

    // 仍被选中的对象删除必须失败；还原后成功。
    check(DeleteObject(reinterpret_cast<HGDIOBJ>(brush)) == FALSE, "选中中的画刷删除失败");
    SelectObject(target.dc, previousBrush);
    check(DeleteObject(reinterpret_cast<HGDIOBJ>(brush)) == TRUE, "还原后画刷删除成功");
    SelectObject(target.dc, previousPen);
    check(DeleteObject(reinterpret_cast<HGDIOBJ>(pen)) == TRUE, "还原后笔删除成功");

    // 重复删除已释放句柄不能崩、必须返回失败。
    check(DeleteObject(reinterpret_cast<HGDIOBJ>(brush)) == FALSE, "重复删除返回失败");
}

void testDibSection() {
    std::printf("[DIB 创建与像素格式]\n");

    // 只支持 32bpp：其它位深必须显式失败，不能静默返回一张"看起来能画"的位图。
    BITMAPINFO eightBit{};
    eightBit.bmiHeader.biSize = sizeof(eightBit.bmiHeader);
    eightBit.bmiHeader.biWidth = 4;
    eightBit.bmiHeader.biHeight = -4;
    eightBit.bmiHeader.biPlanes = 1;
    eightBit.bmiHeader.biBitCount = 8;
    eightBit.bmiHeader.biCompression = BI_RGB;
    void* ignored = nullptr;
    check(CreateDIBSection(nullptr, &eightBit, DIB_RGB_COLORS, &ignored, nullptr, 0) == nullptr,
          "8bpp DIB 被拒绝");
    check(ignored == nullptr, "失败时 ppvBits 保持空");
    if (ignored != nullptr) DeleteObject(reinterpret_cast<HGDIOBJ>(ignored));

    Dib dib;
    check(dib.create(4, 3, true), "创建 4x3 顶向下 DIB");
    check(dib.bits != nullptr, "DIB 像素指针非空");

    // COLORREF 是 0x00BBGGRR，DIB 内存是 B,G,R,A。这条断言钉死换算式：
    // 内存 0x00112233（B=33,G=22,R=11）对应 COLORREF 0x00332211。
    pokeBits(dib.bits, 16, true, 3, 0, 0, 0x00112233u);
    checkEqual(0x00332211u, GetPixel(dib.dc, 0, 0), "字节序：BGR(A) 内存 → BBGGRR 的 COLORREF");

    checkEqual(RGB(0x11, 0x22, 0x33), SetPixelV(dib.dc, 1, 1, RGB(0x11, 0x22, 0x33)), "SetPixelV 回传设置值");
    checkEqual(0x00332211u, GetPixel(dib.dc, 1, 1), "SetPixelV/GetPixel 往返一致");
    checkEqual(0xFF112233u, peekBits(dib.bits, 16, true, 3, 1, 1), "写入后内存为 0xAARRGGBB（A 恒为不透明 0xFF）");

    // 越界读取返回 CLR_INVALID，且不越界写。
    checkEqual(CLR_INVALID, GetPixel(dib.dc, -1, 0), "负坐标读像素返回 CLR_INVALID");
    checkEqual(CLR_INVALID, GetPixel(dib.dc, 4, 0), "越右边界读像素返回 CLR_INVALID");
    checkEqual(CLR_INVALID, SetPixelV(dib.dc, 0, 3, RGB(1, 2, 3)), "越下边界写像素返回 CLR_INVALID");

    // 自底向上（biHeight 为正）：内存第一行是图像末行。
    Dib bottomUp;
    check(bottomUp.create(2, 2, false), "创建 2x2 自底向上 DIB");
    pokeBits(bottomUp.bits, 8, false, 2, 0, 1, 0x00ABCDEFu);  // 图像第 1 行 = 内存第 0 行
    checkEqual(0x00EFCDABu, GetPixel(bottomUp.dc, 0, 1), "自底向上：内存首行映射到图像末行");
    checkEqual(0x00ABCDEFu, peekBits(bottomUp.bits, 8, false, 2, 0, 1), "自底向上：peek 返回原始像素值（非 COLORREF 换算）");

    // 兼容位图必须确定性清零。
    HBITMAP compatible = CreateCompatibleBitmap(nullptr, 4, 4);
    check(compatible != nullptr, "CreateCompatibleBitmap 成功");
    if (compatible != nullptr) {
        HDC dc = CreateCompatibleDC(nullptr);
        SelectObject(dc, reinterpret_cast<HGDIOBJ>(compatible));
        checkEqual(0x00000000u, GetPixel(dc, 2, 2), "兼容位图初始为不透明黑");
        DeleteDC(dc);
        DeleteObject(reinterpret_cast<HGDIOBJ>(compatible));
    }
}

void testBitBlt() {
    std::printf("[BitBlt]\n");

    Dib source;
    Dib dest;
    check(source.create(4, 4, true) && dest.create(4, 4, true), "创建两张 4x4 DIB");
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const std::uint32_t value = 0xFF000000u | (static_cast<std::uint32_t>(y * 4 + x) << 8);
            pokeBits(source.bits, 16, true, 4, x, y, value);
        }
    }

    check(BitBlt(dest.dc, 0, 0, 4, 4, source.dc, 0, 0, SRCCOPY) == TRUE, "SRCCOPY 全图复制成功");
    bool identical = true;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (peekBits(dest.bits, 16, true, 4, x, y) != peekBits(source.bits, 16, true, 4, x, y)) {
                identical = false;
            }
        }
    }
    check(identical, "SRCCOPY 后目标与源逐像素相同");

    // 偏移复制：源 (1,1) 起 2x2 → 目标 (2,2)。
    check(BitBlt(dest.dc, 2, 2, 2, 2, source.dc, 1, 1, SRCCOPY) == TRUE, "偏移 SRCCOPY 成功");
    checkEqual(peekBits(source.bits, 16, true, 4, 1, 1), peekBits(dest.bits, 16, true, 4, 2, 2), "偏移复制首像素对齐");
    checkEqual(peekBits(source.bits, 16, true, 4, 2, 2), peekBits(dest.bits, 16, true, 4, 3, 3), "偏移复制末像素对齐");

    // DSTINVERT 只翻转目标，不看源。
    pokeBits(dest.bits, 16, true, 4, 0, 0, 0xFF102030u);
    check(BitBlt(dest.dc, 0, 0, 1, 1, nullptr, 0, 0, DSTINVERT) == TRUE, "DSTINVERT 不需要源 DC");
    checkEqual(0xFFEFDFCFu, peekBits(dest.bits, 16, true, 4, 0, 0), "DSTINVERT 结果为按位取反");

    // 不支持的 ROP 必须失败，而不是静默当成 SRCCOPY。
    check(BitBlt(dest.dc, 0, 0, 1, 1, source.dc, 0, 0, PATPAINT) == FALSE, "不支持的 ROP 返回失败");
    check(BitBlt(dest.dc, 0, 0, 1, 1, source.dc, 0, 0, MERGECOPY) == FALSE, "MERGECOPY 返回失败");

    // 裁剪：目标越界部分丢弃，源坐标同步平移。
    Dib clippedTarget;
    check(clippedTarget.create(2, 2, true), "创建 2x2 目标");
    check(BitBlt(clippedTarget.dc, 1, 1, 4, 4, source.dc, 0, 0, SRCCOPY) == TRUE, "越界 SRCCOPY 返回成功");
    checkEqual(peekBits(source.bits, 16, true, 4, 0, 0), peekBits(clippedTarget.bits, 8, true, 2, 1, 1),
               "裁剪后右下角像素来自源 (0,0)");

    // 同位图自拷贝（重叠）：等价于 memmove。
    Dib self;
    check(self.create(5, 1, true), "创建 5x1 同位图");
    for (int x = 0; x < 5; ++x) {
        pokeBits(self.bits, 20, true, 1, x, 0, 0xFF000000u | static_cast<std::uint32_t>(x + 1));
    }
    check(BitBlt(self.dc, 1, 0, 4, 1, self.dc, 0, 0, SRCCOPY) == TRUE, "同位图自拷贝成功");
    const std::uint32_t expected[5] = {1u, 1u, 2u, 3u, 4u};
    bool selfOk = true;
    for (int x = 0; x < 5; ++x) {
        if (peekBits(self.bits, 20, true, 1, x, 0) != (0xFF000000u | expected[x])) selfOk = false;
    }
    check(selfOk, "重叠自拷贝结果与 memmove 一致");
}

void testStretchBlt() {
    std::printf("[StretchBlt]\n");

    Dib source;
    Dib dest;
    check(source.create(4, 4, true) && dest.create(8, 8, true), "创建 4x4 源与 8x8 目标");
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            pokeBits(source.bits, 16, true, 4, x, y, 0xFF000000u | static_cast<std::uint32_t>(y * 4 + x));
        }
    }

    check(SetStretchBltMode(dest.dc, BLACKONWHITE) == COLORONCOLOR, "SetStretchBltMode 返回先前模式");
    check(SetStretchBltMode(dest.dc, COLORONCOLOR) == BLACKONWHITE, "SetStretchBltMode 再次返回上一次设置");
    check(GetStretchBltMode(dest.dc) == COLORONCOLOR, "拉伸模式读回为 COLORONCOLOR");
    check(StretchBlt(dest.dc, 0, 0, 8, 8, source.dc, 0, 0, 4, 4, SRCCOPY) == TRUE, "2x 放大成功");

    // 最近邻：目标 2x2 块必须整块同色，且等于源对应像素。
    bool blocksMatch = true;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const std::uint32_t expected = 0xFF000000u | static_cast<std::uint32_t>(y * 4 + x);
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    if (peekBits(dest.bits, 32, true, 8, x * 2 + dx, y * 2 + dy) != expected) blocksMatch = false;
                }
            }
        }
    }
    check(blocksMatch, "2x 放大为最近邻（每 2x2 块同色且与源像素对应）");

    // HALFTONE 不支持，必须失败而不是假装成功。
    SetStretchBltMode(dest.dc, HALFTONE);
    check(StretchBlt(dest.dc, 0, 0, 8, 8, source.dc, 0, 0, 4, 4, SRCCOPY) == FALSE, "HALFTONE 模式返回失败");
    SetStretchBltMode(dest.dc, COLORONCOLOR);
}

void testDibUpload() {
    std::printf("[StretchDIBits / SetDIBitsToDevice]\n");

    // 4x4 源图案，2x2 色块分四色。
    const std::uint32_t pattern[4][4] = {
        {0xFF0000FFu, 0xFF0000FFu, 0xFF00FF00u, 0xFF00FF00u},
        {0xFF0000FFu, 0xFF0000FFu, 0xFF00FF00u, 0xFF00FF00u},
        {0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFFFFFFu},
        {0xFFFF0000u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFFFFFFFFu},
    };
    std::uint32_t source[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) source[y * 4 + x] = pattern[y][x];
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = 4;
    info.bmiHeader.biHeight = -4;  // 顶向下，与项目内 5 处调用一致
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    Dib dest;
    check(dest.create(8, 8, true), "创建 8x8 目标");

    const int lines = StretchDIBits(dest.dc, 0, 0, 8, 8, 0, 0, 4, 4, source, &info, DIB_RGB_COLORS, SRCCOPY);
    check(lines != static_cast<int>(GDI_ERROR), "StretchDIBits 成功（返回值非 GDI_ERROR）");
    check(lines == 8, "StretchDIBits 返回写入扫描线数 8");

    // 2x 放大后每个 2x2 块同色。
    bool stretchedOk = true;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    if (peekBits(dest.bits, 32, true, 8, x * 2 + dx, y * 2 + dy) != pattern[y][x]) stretchedOk = false;
                }
            }
        }
    }
    check(stretchedOk, "StretchDIBits 2x 放大像素正确");

    // 不支持的 ROP 必须是 GDI_ERROR（调用方据此判失败）。
    check(StretchDIBits(dest.dc, 0, 0, 8, 8, 0, 0, 4, 4, source, &info, DIB_RGB_COLORS, SRCAND) ==
              static_cast<int>(GDI_ERROR),
          "StretchDIBits 不支持 ROP 时返回 GDI_ERROR");

    // 1:1 传输，落在偏移 (2,2)。
    Dib offsetTarget;
    check(offsetTarget.create(8, 8, true), "创建偏移目标 8x8");
    const int scanLines = SetDIBitsToDevice(offsetTarget.dc, 2, 2, 4, 4, 0, 0, 0, 4, source, &info,
                                            DIB_RGB_COLORS);
    check(scanLines == 4, "SetDIBitsToDevice 返回 4 条扫描线");
    bool offsetOk = true;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (peekBits(offsetTarget.bits, 32, true, 8, x + 2, y + 2) != pattern[y][x]) offsetOk = false;
        }
    }
    check(offsetOk, "SetDIBitsToDevice 1:1 偏移写入正确");
    checkEqual(0x00000000u, peekBits(offsetTarget.bits, 32, true, 8, 0, 0), "偏移写入未污染区域外像素（未写区保持初始零值）");
}

void testPatBltAndState() {
    std::printf("[PatBlt 与 DC 状态栈]\n");

    Dib target;
    check(target.create(4, 4, true), "创建 4x4 目标");

    check(PatBlt(target.dc, 0, 0, 4, 4, BLACKNESS) == TRUE, "PatBlt BLACKNESS 成功");
    checkEqual(0xFF000000u, peekBits(target.bits, 16, true, 4, 0, 0), "BLACKNESS 写入不透明黑");

    check(PatBlt(target.dc, 0, 0, 4, 4, WHITENESS) == TRUE, "PatBlt WHITENESS 成功");
    checkEqual(0xFFFFFFFFu, peekBits(target.bits, 16, true, 4, 3, 3), "WHITENESS 写入不透明白");

    check(PatBlt(target.dc, 0, 0, 4, 4, DSTINVERT) == TRUE, "PatBlt DSTINVERT 成功");
    checkEqual(0xFF000000u, peekBits(target.bits, 16, true, 4, 1, 1), "DSTINVERT 取反白得到不透明黑（alpha 保留）");

    HBRUSH brush = CreateSolidBrush(RGB(0x0A, 0x0B, 0x0C));
    HGDIOBJ oldBrush = SelectObject(target.dc, reinterpret_cast<HGDIOBJ>(brush));
    check(PatBlt(target.dc, 1, 1, 2, 2, PATCOPY) == TRUE, "PatBlt PATCOPY 成功");
    checkEqual(0xFF0A0B0Cu, peekBits(target.bits, 16, true, 4, 2, 2), "PATCOPY 用画刷颜色填充（R=0x0A 落在高字节）");
    checkEqual(0xFF000000u, peekBits(target.bits, 16, true, 4, 0, 0), "PATCOPY 只填指定矩形（矩形外保持前一步的黑）");

    // 空画刷不该填充，应显式失败。
    SelectObject(target.dc, GetStockObject(NULL_BRUSH));
    check(PatBlt(target.dc, 0, 0, 4, 4, PATCOPY) == FALSE, "空画刷 PATCOPY 返回失败");
    SelectObject(target.dc, oldBrush);
    DeleteObject(reinterpret_cast<HGDIOBJ>(brush));

    // SaveDC/RestoreDC 必须还原笔刷与文字颜色。
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x33, 0x44, 0x55));
    HGDIOBJ previousPen = SelectObject(target.dc, reinterpret_cast<HGDIOBJ>(pen));
    const COLORREF previousText = SetTextColor(target.dc, RGB(1, 2, 3));
    const int saved = SaveDC(target.dc);
    check(saved == 1, "SaveDC 返回栈深度 1");
    SelectObject(target.dc, GetStockObject(WHITE_PEN));
    SetTextColor(target.dc, RGB(9, 9, 9));
    check(RestoreDC(target.dc, -1) == TRUE, "RestoreDC(-1) 成功");
    checkEqual(RGB(1, 2, 3), GetTextColor(target.dc), "RestoreDC 还原文字颜色");
    check(SetBkMode(target.dc, TRANSPARENT) == OPAQUE, "SetBkMode 返回先前值 OPAQUE");
    check(GetBkMode(target.dc) == TRANSPARENT, "BkMode 设置为 TRANSPARENT");

    SelectObject(target.dc, previousPen);
    DeleteObject(reinterpret_cast<HGDIOBJ>(pen));
    (void)previousText;
}

// 回放回调：HEADER/EOF/GDICOMMENT 直接放行，其余记录交给 PlayEnhMetaFileRecord。
int CALLBACK replayEmfRecord(HDC dc, HANDLETABLE* handles, const ENHMETARECORD* record, int count,
                             LPARAM) {
    switch (record->iType) {
    case EMR_HEADER:
    case EMR_EOF:
    case EMR_GDICOMMENT:
        return 1;
    default:
        return PlayEnhMetaFileRecord(dc, handles, record, static_cast<UINT>(count));
    }
}

void testPolyFillAndDeviceCaps() {
    std::printf("[PolyFill 模式与设备能力]\n");

    Dib target;
    check(target.create(6, 4, true), "创建 6x4 目标");

    // polyFillMode：默认 ALTERNATE，Set 返回旧值，非法模式不改状态。
    check(GetPolyFillMode(target.dc) == ALTERNATE, "GetPolyFillMode 默认 ALTERNATE");
    check(SetPolyFillMode(target.dc, WINDING) == ALTERNATE, "SetPolyFillMode 返回旧值 ALTERNATE");
    check(GetPolyFillMode(target.dc) == WINDING, "GetPolyFillMode 读到 WINDING");
    check(SetPolyFillMode(target.dc, ALTERNATE) == WINDING, "SetPolyFillMode 切回返回 WINDING");
    check(SetPolyFillMode(target.dc, 999) == 0, "非法模式返回 0 且不改状态");
    check(GetPolyFillMode(target.dc) == ALTERNATE, "非法模式后仍 ALTERNATE");

    // SaveDC/RestoreDC 保存并恢复 polyFillMode。
    const int saved = SaveDC(target.dc);
    check(saved == 1, "SaveDC 返回栈深度 1");
    SetPolyFillMode(target.dc, WINDING);
    check(GetPolyFillMode(target.dc) == WINDING, "SaveDC 后设为 WINDING");
    check(RestoreDC(target.dc, -1) == TRUE, "RestoreDC(-1) 成功");
    check(GetPolyFillMode(target.dc) == ALTERNATE, "RestoreDC 还原为 ALTERNATE");

    // GetDeviceCaps：像素尺寸来自位图，物理尺寸为正，未知 index 为 0。
    check(GetDeviceCaps(target.dc, HORZRES) == 6, "HORZRES = 位图宽 6");
    check(GetDeviceCaps(target.dc, VERTRES) == 4, "VERTRES = 位图高 4");
    check(GetDeviceCaps(target.dc, HORZSIZE) > 0, "HORZSIZE 为正");
    check(GetDeviceCaps(target.dc, VERTSIZE) > 0, "VERTSIZE 为正");
    check(GetDeviceCaps(target.dc, 0x7FFF) == 0, "未知 index 返回 0");

    // PtVisible：无裁剪全可见；选矩形区域后按条带判点；清空裁剪恢复全可见。
    check(PtVisible(target.dc, 0, 0) == TRUE, "无裁剪时 PtVisible(0,0) 可见");
    HRGN clip = CreateRectRgn(1, 1, 3, 3);
    check(clip != nullptr, "创建裁剪区域 (1,1)-(3,3)");
    check(SelectClipRgn(target.dc, clip) != ERROR, "选择裁剪区域");
    check(PtVisible(target.dc, 2, 2) == TRUE, "裁剪区内点 (2,2) 可见");
    check(PtVisible(target.dc, 0, 0) == FALSE, "裁剪区外点 (0,0) 不可见");
    check(PtVisible(target.dc, 5, 3) == FALSE, "裁剪区外点 (5,3) 不可见");
    check(SelectClipRgn(target.dc, nullptr) != ERROR, "清空裁剪恢复全可见");
    check(PtVisible(target.dc, 5, 3) == TRUE, "清空裁剪后 (5,3) 可见");
    DeleteObject(reinterpret_cast<HGDIOBJ>(clip));

    // 录制型 DC 无像素表面，尺寸诚实返回 0。
    HDC meta = CreateEnhMetaFileW(nullptr, nullptr, nullptr, nullptr);
    check(meta != nullptr, "创建录制 DC");
    check(GetDeviceCaps(meta, HORZRES) == 0, "录制 DC 的 HORZRES 为 0");
    DeleteEnhMetaFile(CloseEnhMetaFile(meta));
}

void testEmfRoundtrip() {
    std::printf("[EMF 往返回放]\n");

    HDC meta = CreateEnhMetaFileW(nullptr, nullptr, nullptr, nullptr);
    check(meta != nullptr, "CreateEnhMetaFileW 创建录制 DC");

    // GdiComment 写私有 marker（12 字节）。
    const std::uint32_t marker[] = {0x314f4d43u, 0, 0};
    check(GdiComment(meta, sizeof(marker), reinterpret_cast<const BYTE*>(marker)) == TRUE,
          "GdiComment 写 marker 成功");

    // BitBlt 在录制 DC 上被序列化成 EMR_BITBLT。
    Dib src;
    check(src.create(2, 2, true), "创建源 2x2");
    check(SetPixelV(src.dc, 0, 0, RGB(0x11, 0x22, 0x33)) != CLR_INVALID, "源 (0,0) 写像素");
    check(SetPixelV(src.dc, 1, 0, RGB(0x44, 0x55, 0x66)) != CLR_INVALID, "源 (1,0) 写像素");
    check(SetPixelV(src.dc, 0, 1, RGB(0x77, 0x88, 0x99)) != CLR_INVALID, "源 (0,1) 写像素");
    check(SetPixelV(src.dc, 1, 1, RGB(0xAA, 0xBB, 0xCC)) != CLR_INVALID, "源 (1,1) 写像素");
    check(BitBlt(meta, 0, 0, 2, 2, src.dc, 0, 0, SRCCOPY) == TRUE, "录制 DC 上 BitBlt 录制成功");

    HENHMETAFILE hmf = CloseEnhMetaFile(meta);
    check(hmf != nullptr, "CloseEnhMetaFile 定稿成功");

    // GetEnhMetaFileBits 两段式：先查长度再拷贝。
    const UINT total = GetEnhMetaFileBits(hmf, 0, nullptr);
    check(total > 0, "GetEnhMetaFileBits 查询返回非零长度");
    std::vector<std::uint8_t> bytes(total);
    check(GetEnhMetaFileBits(hmf, total, bytes.data()) == total, "GetEnhMetaFileBits 拷贝返回 total");

    // EnumEnhMetaFile 用 PlayEnhMetaFileRecord 回放到目标 DC，逐像素验证。
    Dib dest;
    check(dest.create(4, 4, true), "创建目标 4x4");
    check(EnumEnhMetaFile(dest.dc, hmf, replayEmfRecord, nullptr, nullptr) == TRUE,
          "EnumEnhMetaFile 回放成功");
    checkEqual(0xFF112233u, peekBits(dest.bits, 16, true, 4, 0, 0), "回放 BitBlt 像素 (0,0)");
    checkEqual(0xFF445566u, peekBits(dest.bits, 16, true, 4, 1, 0), "回放 BitBlt 像素 (1,0)");
    checkEqual(0xFF778899u, peekBits(dest.bits, 16, true, 4, 0, 1), "回放 BitBlt 像素 (0,1)");
    checkEqual(0xFFAABBCCu, peekBits(dest.bits, 16, true, 4, 1, 1), "回放 BitBlt 像素 (1,1)");
    checkEqual(0x00000000u, peekBits(dest.bits, 16, true, 4, 3, 3), "回放未污染 (3,3)");

    DeleteEnhMetaFile(hmf);
}

}  // namespace

int main() {
    std::printf("compat/win32 GDI 语义自测（对象管理 + 位图传输）\n\n");
    testObjectLifecycle();
    testDibSection();
    testBitBlt();
    testStretchBlt();
    testDibUpload();
    testPatBltAndState();
    testPolyFillAndDeviceCaps();
    testEmfRoundtrip();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
