// compat/win32 自测 · 文本 / 字体（P5-06 度量 + S4-2 真字形）
//
// 这一族现在有三个层次，断言分别对应：
//   · DC 状态：Get/SetTextAlign 是真实状态，SaveDC/RestoreDC 一并保存；
//   · 度量：GetTextExtentPoint32W 与 TextOutW 共用同一套 fontMetricsFor，
//     "先量后画"的居中/对齐不会错位；东亚宽字符占 1:1 单元格而不是 5:7；
//   · 字形：CoreText 真的画出了字——但"真字"没法用硬编码像素断言（字形随系统
//     字体版本变）。所以断言的是**字形语义**，任何可用字体都必须满足：
//       - 有墨迹，且在单元格内；
//       - 同一基线：'T' 与 'e' 的墨迹底边相同，顶边不同（否则是逐字形居中，
//         读起来像剪贴报）；
//       - 'g' 有降部（墨迹伸到基线之下），'T' 没有；
//       - 'T' 的横杠在上部、'L' 的横杠在底部（区分上下，防整体翻转）；
//       - 空格没有墨迹但仍在 OPAQUE 下铺满背景；
//       - 同一调用两次得到逐字节相同的结果（确定性）；
//       - XOR 画两遍整块表面逐字节还原（1bit 合成没有中间灰，这条才成立）。
//
// 构建与运行（走 CMake 目标，见 scripts/verify-win32-compat.sh）：
//   c++ -std=c++20 -I compat/win32/include \
//       compat/win32/src/RuntimeWin32CompatGdi.cpp \
//       compat/win32/src/RuntimeWin32CompatText.cpp \
//       compat/win32/selftest/CompatTextSemantics.cpp \
//       -framework CoreText -framework CoreGraphics -framework CoreFoundation -o /tmp/cct
//   /tmp/cct

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
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

constexpr std::uint32_t kWhite = 0xFFFFFFFFu;
constexpr std::uint32_t kBlack = 0xFF000000u;
constexpr std::uint32_t kRed = 0xFFFF0000u;
constexpr std::uint32_t kGreen = 0xFF00FF00u;
constexpr std::uint32_t kBlue = 0xFF0000FFu;

// 一个 32bpp top-down 内存 DC（宽度已知，用于"真画了"的像素断言）。
struct Dib {
    int width = 0;
    int height = 0;
    HBITMAP handle = nullptr;
    void* bits = nullptr;
    HDC dc = nullptr;

    bool create(int w, int h) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;  // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        handle = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (handle == nullptr) return false;
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) return false;
        SelectObject(dc, reinterpret_cast<HGDIOBJ>(handle));
        width = w;
        height = h;
        return true;
    }

    ~Dib() {
        if (dc != nullptr) DeleteDC(dc);
        if (handle != nullptr) DeleteObject(reinterpret_cast<HGDIOBJ>(handle));
    }

    std::uint32_t peek(int x, int y) const {
        if (bits == nullptr || x < 0 || y < 0 || x >= width || y >= height) return 0xDEADBEEFu;
        const auto* p = static_cast<const std::uint32_t*>(bits);
        return p[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }

    std::vector<std::uint32_t> snapshot() const {
        std::vector<std::uint32_t> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
        if (bits == nullptr) return out;
        std::memcpy(out.data(), bits, out.size() * sizeof(std::uint32_t));
        return out;
    }

    // 区域内非背景像素数。背景固定传白色，测试里都先 PatBlt(WHITENESS)。
    int inkCount(int x0, int y0, int x1, int y1) const {
        int count = 0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (peek(x, y) != kWhite) ++count;
            }
        }
        return count;
    }

    int inkInRow(int y, int x0, int x1) const {
        int count = 0;
        for (int x = x0; x < x1; ++x) {
            if (peek(x, y) != kWhite) ++count;
        }
        return count;
    }

    // 区域内墨迹的行范围；无墨迹返回 false（minRow/maxRow 不修改）。
    bool inkRows(int x0, int y0, int x1, int y1, int& minRow, int& maxRow) const {
        bool found = false;
        for (int y = y0; y < y1; ++y) {
            if (inkInRow(y, x0, x1) <= 0) continue;
            if (!found) minRow = y;
            maxRow = y;
            found = true;
        }
        return found;
    }

    int inkMinColumn(int x0, int y0, int x1, int y1) const {
        for (int x = x0; x < x1; ++x) {
            for (int y = y0; y < y1; ++y) {
                if (peek(x, y) != kWhite) return x;
            }
        }
        return -1;
    }
};

HFONT makeFont(int height, const wchar_t* face, int weight = FW_NORMAL) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

void testTextAlign() {
    std::printf("[文本对齐状态]\n");

    Dib d;
    check(d.create(16, 16), "创建 16x16 位图");
    check(d.dc != nullptr, "拿到 DC");

    checkEqual(0u, GetTextAlign(d.dc), "GetTextAlign 默认 TA_LEFT|TA_TOP == 0");
    checkEqual(0u, SetTextAlign(d.dc, TA_RIGHT | TA_BOTTOM), "SetTextAlign 返回旧值 0");
    checkEqual(static_cast<std::uint32_t>(TA_RIGHT | TA_BOTTOM), GetTextAlign(d.dc),
               "SetTextAlign 后 GetTextAlign 反映新值");
    checkEqual(static_cast<std::uint32_t>(TA_RIGHT | TA_BOTTOM), SetTextAlign(d.dc, TA_LEFT),
               "再次 SetTextAlign 返回上一值");

    // SaveDC/RestoreDC 必须把 textAlign 一起保存恢复（漏了会破坏"污染再还原"用例）。
    SetTextAlign(d.dc, TA_CENTER);
    checkEqual(1u, static_cast<std::uint32_t>(SaveDC(d.dc)), "SaveDC 返回 1");
    SetTextAlign(d.dc, TA_LEFT | TA_TOP);
    checkEqual(0u, GetTextAlign(d.dc), "Save 后污染 textAlign 为 0");
    check(RestoreDC(d.dc, -1) == TRUE, "RestoreDC 返回 TRUE");
    checkEqual(static_cast<std::uint32_t>(TA_CENTER), GetTextAlign(d.dc),
               "RestoreDC 后 textAlign 回到 TA_CENTER");

    checkEqual(static_cast<std::uint32_t>(GDI_ERROR), GetTextAlign(nullptr),
               "空 DC 的 GetTextAlign 返回 GDI_ERROR");
    checkEqual(static_cast<std::uint32_t>(GDI_ERROR), SetTextAlign(nullptr, 0),
               "空 DC 的 SetTextAlign 返回 GDI_ERROR");
}

void testDpiAwareness() {
    std::printf("[DPI 感知]\n");

    // 产品硬门槛：MapNativeOverlayWin32::begin 要求 GetAwarenessFromDpiAwarenessContext
    // (GetThreadDpiAwarenessContext()) == DPI_AWARENESS_PER_MONITOR_AWARE，否则 native
    // overlay 直接失败。软件帧缓冲不缩放、按原生像素渲染，等价于"每显示器感知"。
    DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
    check(ctx != nullptr, "GetThreadDpiAwarenessContext 返回非 NULL 上下文");
    check(GetAwarenessFromDpiAwarenessContext(ctx) == DPI_AWARENESS_PER_MONITOR_AWARE,
          "上下文映射为 DPI_AWARENESS_PER_MONITOR_AWARE（产品硬门槛）");
    check(GetAwarenessFromDpiAwarenessContext(ctx) == 2,
          "DPI_AWARENESS_PER_MONITOR_AWARE 的字面量 == 2");
    check(GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE) == DPI_AWARENESS_UNAWARE,
          "UNAWARE 哨兵映射正确");
    check(GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) == DPI_AWARENESS_SYSTEM_AWARE,
          "SYSTEM_AWARE 哨兵映射正确");
    check(GetAwarenessFromDpiAwarenessContext(nullptr) == DPI_AWARENESS_INVALID,
          "未知上下文返回 INVALID");
}

void testCreateFontAndMetrics() {
    std::printf("[CreateFontW / 字体度量]\n");

    Dib d;
    check(d.create(64, 64), "创建 64x64 位图");

    // 负高度 = "字符高度" 16px；宽 0 → 按 5:7 比例 round(16*5/7)=11。
    HFONT font = makeFont(-16, L"Arial");
    check(font != nullptr, "CreateFontW 返回非 NULL 字体句柄");

    // GetObject 回填 LOGFONTW，字高与入参一致。
    LOGFONTW lf{};
    check(GetObjectW(reinterpret_cast<HGDIOBJ>(font), sizeof(LOGFONTW), &lf) ==
              static_cast<int>(sizeof(LOGFONTW)),
          "GetObjectW 返回 sizeof(LOGFONTW)");
    check(lf.lfHeight == -16, "LOGFONTW.lfHeight 与入参一致（-16）");
    check(lf.lfWeight == FW_NORMAL, "LOGFONTW.lfWeight == FW_NORMAL");

    HGDIOBJ oldFont = SelectObject(d.dc, reinterpret_cast<HGDIOBJ>(font));
    check(oldFont != nullptr, "SelectObject(font) 返回旧字体");

    SIZE sz{};
    check(GetTextExtentPoint32W(d.dc, L"AB", 2, &sz) == TRUE, "GetTextExtentPoint32W 返回 TRUE");
    checkEqual(22u, static_cast<std::uint32_t>(sz.cx), "两字符宽度 = 2*11 = 22");
    checkEqual(16u, static_cast<std::uint32_t>(sz.cy), "字高 = 16");

    // 东亚宽字符占 1:1 单元格：16px 的字体里"中"就该是 16 宽，不是 11。
    // 这一条是 S4-2 新增的：以前按 c * width 算，中文会被排成半个字宽，
    // 汉字互相压住，而 ASCII 用例全绿。
    SIZE wide{};
    check(GetTextExtentPoint32W(d.dc, L"\u4E2D", 1, &wide) == TRUE, "宽字符度量返回 TRUE");
    checkEqual(16u, static_cast<std::uint32_t>(wide.cx), "汉字宽度 = 字高 = 16（1:1 单元格）");
    checkEqual(16u, static_cast<std::uint32_t>(wide.cy), "汉字高度 = 16");

    SIZE mixed{};
    check(GetTextExtentPoint32W(d.dc, L"A\u4E2D", 2, &mixed) == TRUE, "混排度量返回 TRUE");
    checkEqual(27u, static_cast<std::uint32_t>(mixed.cx), "混排宽度 = 11 + 16 = 27");

    SIZE empty{};
    check(GetTextExtentPoint32W(d.dc, L"", 0, &empty) == TRUE, "空串度量返回 TRUE");
    checkEqual(0u, static_cast<std::uint32_t>(empty.cx), "空串宽度 0");
    checkEqual(16u, static_cast<std::uint32_t>(empty.cy), "空串高度仍为字高");

    // 度量可加：整串宽度 == 逐字符宽度之和（否则居中和裁剪会互相矛盾）。
    SIZE three{};
    check(GetTextExtentPoint32W(d.dc, L"ABC", 3, &three) == TRUE, "三字符度量返回 TRUE");
    checkEqual(33u, static_cast<std::uint32_t>(three.cx), "三字符宽度 = 3*11 = 33");

    SelectObject(d.dc, oldFont);
    DeleteObject(reinterpret_cast<HGDIOBJ>(font));
}

void testRealGlyphs() {
    std::printf("[TextOutW 画真字形]\n");

    Dib d;
    check(d.create(64, 32), "创建 64x32 位图");
    check(PatBlt(d.dc, 0, 0, 64, 32, WHITENESS) == TRUE, "填充白色背景");

    HFONT font = makeFont(-16, L"Arial");
    check(font != nullptr, "CreateFontW 返回非 NULL");
    HGDIOBJ oldFont = SelectObject(d.dc, reinterpret_cast<HGDIOBJ>(font));
    SetTextColor(d.dc, RGB(0, 0, 0));

    // 'T'：横杠在上部（第 1-2 行），竖笔在中列，底边在基线上（第 12 行附近）。
    check(TextOutW(d.dc, 0, 0, L"T", 1) == TRUE, "TextOutW('T') 返回 TRUE");
    const int tInk = d.inkCount(0, 0, 11, 16);
    check(tInk > 15, "文本真的画出了墨迹（不是返回 TRUE 什么都不画）");
    check(tInk < 11 * 16, "墨迹没有铺满整个单元格（那是填充不是文字）");
    const int tTopRow = d.inkInRow(1, 0, 11);
    check(tTopRow >= 6, "'T' 的横杠在中部以上（第 1 行至少 6 个像素）");
    check(d.inkInRow(15, 0, 11) == 0, "'T' 没有降部（第 15 行无墨迹）");

    int tMin = 99, tMax = -1;
    check(d.inkRows(0, 0, 11, 16, tMin, tMax), "'T' 的墨迹行范围可测");
    check(tMax < 16 && tMax >= 11, "'T' 底边落在基线上（第 11-15 行之间）");
    check(tMin <= 2, "'T' 顶边贴着单元格顶部（大写字母高度）");

    // 'e'：x 高度，顶边比 'T' 低，但底边必须与 'T' 相同（同一条基线）。
    check(TextOutW(d.dc, 20, 0, L"e", 1) == TRUE, "TextOutW('e') 返回 TRUE");
    int eMin = 99, eMax = -1;
    check(d.inkRows(20, 0, 31, 16, eMin, eMax), "'e' 的墨迹行范围可测");
    check(eMin > tMin, "'e' 的顶边低于 'T'（小写 x 高度）");
    check(eMax == tMax, "'e' 与 'T' 共用同一条基线（底边相同）");

    // 'g'：有降部，墨迹伸到基线之下。
    check(TextOutW(d.dc, 40, 0, L"g", 1) == TRUE, "TextOutW('g') 返回 TRUE");
    int gMin = 99, gMax = -1;
    check(d.inkRows(40, 0, 51, 16, gMin, gMax), "'g' 的墨迹行范围可测");
    check(gMax > tMax, "'g' 有降部（墨迹越过基线）");

    // 'L'：横杠在底部。和 'T' 的底部形状完全不同——这条挡的是"整幅图形上下翻转"，
    // 翻转后 'T' 仍然像 'T'，但 'L' 会变成 Γ。
    check(PatBlt(d.dc, 0, 0, 64, 32, WHITENESS) == TRUE, "重填白色背景");
    check(TextOutW(d.dc, 0, 0, L"L", 1) == TRUE, "TextOutW('L') 返回 TRUE");
    const int lBottomRow = d.inkInRow(12, 0, 11);
    check(lBottomRow >= 6, "'L' 的横杠在底部（第 12 行至少 6 个像素）");
    check(d.inkInRow(1, 0, 11) <= 2, "'L' 顶部只有竖笔（第 1 行最多 2 个像素）");
    checkEqual(kBlack, d.peek(1, 5), "'L' 竖笔像素就是文本色（黑），不是别的合成值");

    // 确定性：同一调用两次，整块表面逐字节相同。字形来自系统字体，但同进程
    // 内必须可重复——否则"帧稳定性审计"在 mac 上永远红。
    const auto first = d.snapshot();
    check(TextOutW(d.dc, 0, 0, L"L", 1) == TRUE, "重复 TextOutW('L') 返回 TRUE");
    check(first == d.snapshot(), "同一调用两次得到逐字节相同的结果（确定性）");

    SelectObject(d.dc, oldFont);
    DeleteObject(reinterpret_cast<HGDIOBJ>(font));
}

void testBackgroundModes() {
    std::printf("[背景模式]\n");

    Dib d;
    check(d.create(64, 32), "创建 64x32 位图");
    check(PatBlt(d.dc, 0, 0, 64, 32, WHITENESS) == TRUE, "填充白色背景");

    HFONT font = makeFont(-16, L"Arial");
    HGDIOBJ oldFont = SelectObject(d.dc, reinterpret_cast<HGDIOBJ>(font));

    // 空格没有墨迹，但 OPAQUE 下必须铺满背景色——"空格占位"是排版的一部分，
    // 不是"什么都没发生"。
    SetBkColor(d.dc, RGB(0, 0, 255));
    SetTextColor(d.dc, RGB(255, 0, 0));
    SetBkMode(d.dc, OPAQUE);
    check(TextOutW(d.dc, 0, 0, L" ", 1) == TRUE, "OPAQUE 下 TextOutW(' ') 返回 TRUE");
    checkEqual(kBlue, d.peek(0, 0), "OPAQUE + 空格：单元格左上角是背景色");
    checkEqual(kBlue, d.peek(10, 15), "OPAQUE + 空格：单元格右下角是背景色");

    // TRANSPARENT：背景不动。
    check(PatBlt(d.dc, 0, 0, 64, 32, WHITENESS) == TRUE, "重填白色背景");
    SetBkMode(d.dc, TRANSPARENT);
    check(TextOutW(d.dc, 0, 0, L" ", 1) == TRUE, "TRANSPARENT 下 TextOutW(' ') 返回 TRUE");
    checkEqual(kWhite, d.peek(0, 0), "TRANSPARENT + 空格：背景不被改写");

    // TRANSPARENT 下画实字：墨迹是文本色，字间空隙保持原背景。
    check(PatBlt(d.dc, 0, 0, 64, 32, WHITENESS) == TRUE, "重填白色背景");
    SetTextColor(d.dc, RGB(0, 255, 0));
    check(TextOutW(d.dc, 0, 0, L"i", 1) == TRUE, "TRANSPARENT 下 TextOutW('i') 返回 TRUE");
    checkEqual(kGreen, d.peek(1, 6), "墨迹像素是文本色（绿）");
    checkEqual(kWhite, d.peek(9, 1), "字右侧的空白保持背景白");

    SelectObject(d.dc, oldFont);
    DeleteObject(reinterpret_cast<HGDIOBJ>(font));
}

void testXorReverts() {
    std::printf("[ROP2 XOR 两遍还原]\n");

    Dib d;
    check(d.create(48, 24), "创建 48x24 位图");
    check(PatBlt(d.dc, 0, 0, 48, 24, WHITENESS) == TRUE, "填充白色背景");

    HFONT font = makeFont(-16, L"Arial");
    HGDIOBJ oldFont = SelectObject(d.dc, reinterpret_cast<HGDIOBJ>(font));
    SetBkMode(d.dc, TRANSPARENT);
    // 注意颜色：XOR 用的笔色如果是黑（0x000000），白底 ^ 黑 == 白，画了等于没画。
    // 第一版就是这么写的，于是"两遍还原"断言在"什么都没画"上成立——正是
    // 假通过。这里用蓝色，white ^ blue 明显不同。
    SetTextColor(d.dc, RGB(0, 0, 255));
    const int previousRop = SetROP2(d.dc, R2_XORPEN);
    check(previousRop == R2_COPYPEN, "SetROP2 返回旧值 R2_COPYPEN");

    const auto before = d.snapshot();
    check(TextOutW(d.dc, 4, 4, L"A", 1) == TRUE, "XOR 下 TextOutW 返回 TRUE");
    const auto afterFirst = d.snapshot();
    check(before != afterFirst, "XOR 第一遍确实改了像素（否则还原断言无意义）");
    check(TextOutW(d.dc, 4, 4, L"A", 1) == TRUE, "XOR 下第二次 TextOutW 返回 TRUE");
    // 1bit 合成（无中间灰）才能让这条成立：部分透明混色没有精确逆元。
    check(before == d.snapshot(), "XOR 画两遍整块表面逐字节还原");

    SetROP2(d.dc, previousRop);
    SelectObject(d.dc, oldFont);
    DeleteObject(reinterpret_cast<HGDIOBJ>(font));
}

void testChineseGlyph() {
    std::printf("[汉字字形]\n");

    Dib d;
    check(d.create(48, 32), "创建 48x32 位图");
    check(PatBlt(d.dc, 0, 0, 48, 32, WHITENESS) == TRUE, "填充白色背景");

    // 用产品实际用的族名：Windows 上有、macOS 上没有，必须能映射到系统 CJK 字体。
    HFONT font = makeFont(-16, L"Microsoft YaHei");
    check(font != nullptr, "CreateFontW(Microsoft YaHei) 返回非 NULL");
    HGDIOBJ oldFont = SelectObject(d.dc, reinterpret_cast<HGDIOBJ>(font));
    SetTextColor(d.dc, RGB(0, 0, 0));

    check(TextOutW(d.dc, 0, 0, L"\u4E2D", 1) == TRUE, "TextOutW('中') 返回 TRUE");
    const int ink = d.inkCount(0, 0, 16, 16);
    check(ink > 15, "汉字画出了墨迹（CJK 走 CoreText 逐码点回退，不是豆腐块）");
    check(ink < 16 * 16 - 16, "汉字不是一整块实心方块（那是缺字形占位，不是字形）");

    int minRow = 99, maxRow = -1;
    check(d.inkRows(0, 0, 16, 16, minRow, maxRow), "汉字墨迹行范围可测");
    check(maxRow - minRow >= 6, "汉字墨迹高度合理（≥6 像素）");
    const int left = d.inkMinColumn(0, 0, 16, 16);
    check(left >= 0 && left <= 4, "汉字在 16 宽单元格内左对齐（左边距 ≤4）");
    check(d.inkCount(16, 0, 48, 16) == 0, "汉字没有溢出到下一个单元格");

    SelectObject(d.dc, oldFont);
    DeleteObject(reinterpret_cast<HGDIOBJ>(font));
}

void testMissingGlyphIsMarked() {
    std::printf("[缺字形标记]\n");

    Dib d;
    check(d.create(48, 32), "创建 48x32 位图");
    check(PatBlt(d.dc, 0, 0, 48, 32, WHITENESS) == TRUE, "填充白色背景");

    HFONT font = makeFont(-16, L"Arial");
    HGDIOBJ oldFont = SelectObject(d.dc, reinterpret_cast<HGDIOBJ>(font));
    SetBkMode(d.dc, TRANSPARENT);
    SetTextColor(d.dc, RGB(255, 0, 0));

    // U+0378 是未分配码位，任何字体都不该有它。缺字形仍画空心框（豆腐块）：
    // 那是"确实没有这个字形"的通用标记，不是"功能没做完"。
    check(TextOutW(d.dc, 0, 0, L"\u0378", 1) == TRUE, "缺字形时 TextOutW 仍返回 TRUE");
    checkEqual(kRed, d.peek(0, 0), "缺字形画空心框（左上角是文本色）");
    checkEqual(kRed, d.peek(10, 0), "空心框上边通到最后一列");
    checkEqual(kWhite, d.peek(5, 8), "空心框内部是空心的");

    SelectObject(d.dc, oldFont);
    DeleteObject(reinterpret_cast<HGDIOBJ>(font));
}

void testDrawTextAlignment() {
    std::printf("[DrawTextW 对齐]\n");

    Dib d;
    check(d.create(120, 80), "创建 120x80 位图");
    check(PatBlt(d.dc, 0, 0, 120, 80, WHITENESS) == TRUE, "填充白色背景");

    HFONT font = makeFont(-16, L"Arial");
    check(font != nullptr, "CreateFontW 返回非 NULL");
    HGDIOBJ oldFont = SelectObject(d.dc, reinterpret_cast<HGDIOBJ>(font));
    SetTextColor(d.dc, RGB(255, 0, 0));

    // 目标矩形 [10,10,110,50)，单字符宽 11、高 16：
    // x = 10 + (100-11)/2 = 54，y = 10 + (40-16)/2 = 22。
    RECT rc{10, 10, 110, 50};
    check(DrawTextW(d.dc, L"A", 1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER) == 16,
          "DrawTextW 返回字高 16");
    const int left = d.inkMinColumn(0, 0, 120, 80);
    check(left >= 54 && left <= 57, "居中后墨迹左边界落在 54±3（度量与实际绘制一致）");
    check(d.inkCount(10, 10, 54, 50) == 0, "矩形左侧未被画到（居中是按整串宽度算的）");

    int minRow = 99, maxRow = -1;
    check(d.inkRows(54, 0, 70, 80, minRow, maxRow), "墨迹行范围可测");
    check(minRow >= 22 && minRow <= 25, "垂直居中后墨迹顶边落在 22±3");

    // DT_CALCRECT：只测量不改画，lprc 收敛到文本包围盒。
    RECT calc{5, 5, 100, 100};
    check(DrawTextW(d.dc, L"ABC", 3, &calc, DT_CALCRECT) == 16, "DT_CALCRECT 返回字高 16");
    check(calc.right - calc.left == 33 && calc.bottom - calc.top == 16,
          "DT_CALCRECT 把矩形收敛到 3 字符 × 1 行 (33×16)");

    // DrawTextA 与 DrawTextW 用同一套度量与对齐。
    check(PatBlt(d.dc, 0, 0, 120, 80, WHITENESS) == TRUE, "重填白色背景");
    RECT rc2{10, 10, 110, 50};
    check(DrawTextA(d.dc, "A", 1, &rc2, DT_SINGLELINE | DT_CENTER | DT_VCENTER) == 16,
          "DrawTextA 返回字高 16");
    check(d.inkMinColumn(0, 0, 120, 80) == left, "DrawTextA 与 DrawTextW 对齐一致");

    SelectObject(d.dc, oldFont);
    DeleteObject(reinterpret_cast<HGDIOBJ>(font));
}

void testExtTextOutClipAndOpacity() {
    std::printf("[ExtTextOutA 裁剪 / 不透明矩形]\n");

    Dib d;
    check(d.create(64, 32), "创建 64x32 位图");
    check(PatBlt(d.dc, 0, 0, 64, 32, WHITENESS) == TRUE, "填充白色背景");

    HFONT font = makeFont(-16, L"Arial");
    HGDIOBJ oldFont = SelectObject(d.dc, reinterpret_cast<HGDIOBJ>(font));
    SetBkMode(d.dc, TRANSPARENT);
    SetTextColor(d.dc, RGB(0, 0, 0));

    // ETO_CLIPPED：4 个字符宽 44，裁到 [2,2,8,8) —— 只有第一格的前几列可见。
    RECT clip{2, 2, 8, 8};
    check(ExtTextOutA(d.dc, 0, 0, ETO_CLIPPED, &clip, "MMMM", 4, nullptr) == TRUE,
          "ExtTextOutA(ETO_CLIPPED) 返回 TRUE");
    check(d.inkCount(0, 0, 64, 32) > 0, "裁剪区内有墨迹");
    check(d.inkCount(8, 0, 64, 32) == 0, "裁剪矩形右侧（x>=8）没有墨迹");
    check(d.inkCount(0, 8, 64, 32) == 0, "裁剪矩形下方（y>=8）没有墨迹");

    // ETO_OPAQUE：先用背景色铺满矩形，再画字。
    check(PatBlt(d.dc, 0, 0, 64, 32, WHITENESS) == TRUE, "重填白色背景");
    SetBkColor(d.dc, RGB(0, 0, 255));
    SetTextColor(d.dc, RGB(255, 0, 0));
    RECT opaqueRect{2, 2, 20, 10};
    check(ExtTextOutA(d.dc, 2, 2, ETO_OPAQUE | ETO_CLIPPED, &opaqueRect, " ", 1, nullptr) == TRUE,
          "ExtTextOutA(ETO_OPAQUE) 返回 TRUE");
    checkEqual(kBlue, d.peek(2, 2), "ETO_OPAQUE 把矩形左上角铺成背景色");
    checkEqual(kBlue, d.peek(19, 9), "ETO_OPAQUE 把矩形右下角铺成背景色");
    checkEqual(kWhite, d.peek(21, 11), "矩形之外保持原背景");

    SelectObject(d.dc, oldFont);
    DeleteObject(reinterpret_cast<HGDIOBJ>(font));
}

void testGlyphIndices() {
    std::printf("[GetGlyphIndicesW]\n");

    Dib d;
    check(d.create(32, 32), "创建 32x32 位图");
    HFONT font = makeFont(-16, L"Arial");
    HGDIOBJ oldFont = SelectObject(d.dc, reinterpret_cast<HGDIOBJ>(font));

    // S4-2 起是**真实**答案（CoreText cmap + 逐码点回退）。以前恒返回
    // GDI_ERROR，让"字体是否覆盖所需汉字"的诊断永远报缺字形——那是假象。
    const wchar_t probe[] = L"A\u4E2D\u0378";
    WORD glyphs[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    checkEqual(3u, GetGlyphIndicesW(d.dc, probe, 3, glyphs, GGI_MARK_NONEXISTING_GLYPHS),
               "返回处理的码元数 3");
    check(glyphs[0] != 0xFFFF, "'A' 有字形索引");
    check(glyphs[1] != 0xFFFF, "'中' 有字形索引（CJK 回退生效）");
    checkEqual(0xFFFFu, static_cast<std::uint32_t>(glyphs[2]),
               "U+0378（未分配码位）报 0xFFFF");

    checkEqual(static_cast<std::uint32_t>(GDI_ERROR), GetGlyphIndicesW(nullptr, probe, 3, glyphs, 0),
               "空 DC 返回 GDI_ERROR");
    checkEqual(0u, GetGlyphIndicesW(d.dc, probe, 0, glyphs, 0), "c == 0 返回 0");

    SelectObject(d.dc, oldFont);
    DeleteObject(reinterpret_cast<HGDIOBJ>(font));
}

void testMeasureMatchesDraw() {
    std::printf("[度量 == 绘制]\n");

    Dib d;
    check(d.create(128, 32), "创建 128x32 位图");
    check(PatBlt(d.dc, 0, 0, 128, 32, WHITENESS) == TRUE, "填充白色背景");

    HFONT font = makeFont(-16, L"Arial");
    HGDIOBJ oldFont = SelectObject(d.dc, reinterpret_cast<HGDIOBJ>(font));
    SetBkMode(d.dc, TRANSPARENT);
    SetTextColor(d.dc, RGB(0, 0, 0));

    // 混排串：绘制出来的墨迹必须落在 GetTextExtentPoint32W 报的宽度之内。
    // 这就是"先量后画不错位"的可观测形式——菜单按钮的居中全押在这条上。
    const wchar_t* text = L"Hud \u4E2D\u6587 ok";
    const int count = static_cast<int>(wcslen(text));
    SIZE sz{};
    check(GetTextExtentPoint32W(d.dc, text, count, &sz) == TRUE, "混排串度量返回 TRUE");
    check(TextOutW(d.dc, 0, 0, text, count) == TRUE, "混排串绘制返回 TRUE");

    int maxColumn = -1;
    for (int x = 0; x < 128; ++x) {
        for (int y = 0; y < 16; ++y) {
            if (d.peek(x, y) != kWhite) { maxColumn = x; break; }
        }
    }
    check(maxColumn >= 0, "混排串画出了墨迹");
    check(maxColumn < sz.cx, "墨迹右边界不超过度量宽度（先量后画一致）");
    check(maxColumn >= sz.cx - 14, "墨迹右边界贴近度量宽度（没有被截断一大块）");

    SelectObject(d.dc, oldFont);
    DeleteObject(reinterpret_cast<HGDIOBJ>(font));
}

}  // namespace

int main() {
    std::printf("compat/win32 文本 / 字体语义自测\n\n");
    testTextAlign();
    testDpiAwareness();
    testCreateFontAndMetrics();
    testRealGlyphs();
    testBackgroundModes();
    testXorReverts();
    testChineseGlyph();
    testMissingGlyphIsMarked();
    testDrawTextAlignment();
    testExtTextOutClipAndOpacity();
    testGlyphIndices();
    testMeasureMatchesDraw();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
