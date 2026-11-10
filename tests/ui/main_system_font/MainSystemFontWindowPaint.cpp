// tests/ui/main_system_font/MainSystemFontWindowPaint.cpp
//
// S4-3 · 窗口客户区出字闭环（macOS 侧的可执行契约）
//
// S4-2 已经把 CoreText 字形光栅接进来了，但所有断言都画在测试自建的 DIB 上：
// 产品字体类从来没往"窗口"上画过。而 S4 的 verify 是"菜单/HUD 文本渲染出可读
// 内容"，所以验收链必须是：真窗口 → WM_PAINT → BeginPaint → 产品字体类
// (MainSystemFontsWin32::draw) → BitBlt 回读 → 逐像素断言 + PNG 存盘。
//
// 断言只用"任何真实字体都成立"的语义，不钉硬编码像素（字形随系统字体版本变）：
//   · 每个文本带都有墨迹 —— 区分"真画了"和"返回 TRUE 但什么都没画"；
//   · 墨迹落在 GetTextExtentPoint32W 报出的前进框里 —— 先量后画不错位；
//   · 只平移参照点，墨迹 bbox 必须平移同样的像素数 —— 居中/垂直居中真生效，
//     而且这条不依赖字形，换任何字体都成立；
//   · CJK 单元格**内部**有墨迹 —— 空心豆腐块只有边框，内部恒为零；
//   · 裁剪区生效 —— 裁剪带内、裁剪框右侧一个墨点都没有；
//   · 同一内容画两次逐字节一致 —— 无不确定性。
//
// 构建（cmake）：target concord_main_system_font_window_paint（仅 APPLE）

#include <windows.h>

#include <gdiplus.h>

#include "ui/text/win32/RuntimeMainSystemFontWin32.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string_view>

namespace {

constexpr int kWidth = 800;
constexpr int kHeight = 600;

// PatBlt(WHITENESS) 写的就是这个值（见 RuntimeWin32CompatGdi.cpp）。
constexpr std::uint32_t kWhite = 0xFFFFFFFFu;
// 回读缓冲的边界哨兵：客户区之外的像素必须保持这个值。
constexpr std::uint32_t kSentinel = 0x7F7F7F7Fu;

// 阴影 + 正文字色。取什么颜色不重要，只要都不等于底色；这里刻意都不取白。
constexpr std::uint32_t kShadowRgb = 0x00000000u;
constexpr std::uint32_t kTextRgb = 0x000040C0u;
const auto kColors = concord::ui::MainSystemFontColors::fromPacked(kShadowRgb, kTextRgb);

int gChecks = 0;
int gFailures = 0;

void check(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::printf("  FAIL  %s\n", what);
    }
}

void checkEqual(long long expected, long long actual, const char* what) {
    ++gChecks;
    if (expected != actual) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected %lld, got %lld)\n", what, expected, actual);
    }
}

// 自建回读 DIB：32bpp、top-down、跨距 = 宽度 × 4。
struct OwnedDib {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    std::uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;

    bool create(int w, int h) {
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;  // 负值 = top-down，第 0 行是顶行
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* data = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        if (bitmap == nullptr || data == nullptr) return false;
        previous = SelectObject(dc, bitmap);
        if (previous == nullptr || previous == HGDI_ERROR) return false;
        pixels = static_cast<std::uint32_t*>(data);
        width = w;
        height = h;
        return true;
    }

    ~OwnedDib() {
        if (dc != nullptr && previous != nullptr) SelectObject(dc, previous);
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (dc != nullptr) DeleteDC(dc);
    }
};

// 一块像素窗口视图：origin 相对回读缓冲的偏移，stride 单位是像素。
struct Frame {
    const std::uint32_t* base = nullptr;
    int stride = 0;

    std::uint32_t at(int x, int y) const {
        return base[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride)
                    + static_cast<std::size_t>(x)];
    }
};

int inkIn(const Frame& f, int left, int top, int right, int bottom) {
    int count = 0;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            if (f.at(x, y) != kWhite) ++count;
        }
    }
    return count;
}

// 墨迹包围盒；没有墨迹时返回 false 且不改写 out。
bool inkBounds(const Frame& f, int left, int top, int right, int bottom, RECT* out) {
    int minX = right, minY = bottom, maxX = left, maxY = top;
    bool any = false;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            if (f.at(x, y) == kWhite) continue;
            any = true;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    if (!any) return false;
    out->left = minX;
    out->top = minY;
    out->right = maxX + 1;
    out->bottom = maxY + 1;
    return true;
}

struct PaintContext {
    concord::ui::MainSystemFontsWin32 fonts;
    int paints = 0;
    RECT lastPaintRect{};
    bool paintRectWasFullClient = false;
};

void drawBand(HDC dc, const PaintContext& ctx, concord::ui::MainSystemFontRole role,
              const wchar_t* text, RECT rect, UINT format) {
    ctx.fonts.draw(dc, role,
                   concord::ui::MainSystemWideTextRequest{
                       std::wstring_view{text, std::wcslen(text)},
                       RuntimeInputPixelRect{rect.left, rect.top, rect.right, rect.bottom},
                       static_cast<std::uint32_t>(format), kColors});
}

// 客户区内容：六个带，各测一件事。整幅从 WHITENESS 开始，所以每次重绘都从同一
// 初始态出发——"画两次逐字节一致"才有意义。
void paintClientArea(HDC dc, const PaintContext& ctx) {
    PatBlt(dc, 0, 0, kWidth, kHeight, WHITENESS);

    // A 左对齐英文（对齐基线）
    drawBand(dc, ctx, concord::ui::MainSystemFontRole::Title, L"CONCORD RTS",
             RECT{40, 20, 760, 56}, DT_LEFT | DT_TOP | DT_SINGLELINE);
    // B 水平+垂直居中（菜单按钮的典型用法）
    drawBand(dc, ctx, concord::ui::MainSystemFontRole::Title, L"MAIN MENU",
             RECT{40, 70, 760, 110}, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // C 中英混排（汉字必须占满 1:1 单元格，否则互相压住）
    drawBand(dc, ctx, concord::ui::MainSystemFontRole::Title, L"中文菜单 与 Mixed",
             RECT{40, 120, 760, 156}, DT_LEFT | DT_TOP | DT_SINGLELINE);
    // D 裁剪区：整串画进 760 宽的矩形，但只允许落在 240 以内
    {
        HRGN region = CreateRectRgn(40, 170, 240, 206);
        if (region != nullptr) {
            SelectClipRgn(dc, region);
            drawBand(dc, ctx, concord::ui::MainSystemFontRole::Title,
                     L"CLIPPED REGION OVERFLOW TEST",
                     RECT{40, 170, 760, 206}, DT_LEFT | DT_TOP | DT_SINGLELINE);
            SelectClipRgn(dc, nullptr);
            DeleteObject(region);
        }
    }
    // E 长句 + DT_WORDBREAK（换行尚未实现，属下一轮；这里只要"没崩、有字"）
    drawBand(dc, ctx, concord::ui::MainSystemFontRole::Title,
             L"THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG AND KEEPS RUNNING FAR "
             L"BEYOND THE RIGHT EDGE OF ANY CONTROL IN THE MENU",
             RECT{40, 220, 760, 320}, DT_LEFT | DT_TOP | DT_WORDBREAK);
    // F 小号字（Small = 14px），验证"三种角色各自成字"
    drawBand(dc, ctx, concord::ui::MainSystemFontRole::Small, L"Small 14px 小字",
             RECT{40, 340, 760, 376}, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

LRESULT CALLBACK paintWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    auto* ctx = reinterpret_cast<PaintContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        if (ctx != nullptr && dc != nullptr) {
            RECT client{};
            GetClientRect(window, &client);
            ctx->lastPaintRect = ps.rcPaint;
            ctx->paintRectWasFullClient =
                ps.rcPaint.left == client.left && ps.rcPaint.top == client.top
                && ps.rcPaint.right == client.right && ps.rcPaint.bottom == client.bottom;
            paintClientArea(dc, *ctx);
            ++ctx->paints;
        }
        EndPaint(window, &ps);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// 经消息队列走一遍：PostMessageW → GetMessageW → DispatchMessageW。
bool paintThroughQueue(HWND window, PaintContext& ctx) {
    const int before = ctx.paints;
    if (PostMessageW(window, WM_PAINT, 0, 0) == FALSE) return false;
    MSG message{};
    if (GetMessageW(&message, nullptr, 0, 0) != 1) return false;
    if (message.message != WM_PAINT) return false;
    DispatchMessageW(&message);
    return ctx.paints == before + 1;
}

struct Readback {
    OwnedDib dib;
    // 客户区在回读缓冲里的起点
    int originX = 0;
    int originY = 0;

    bool create() {
        if (!dib.create(kWidth + 64, kHeight + 64)) return false;
        originX = 16;
        originY = 16;
        const std::size_t total = static_cast<std::size_t>(dib.width)
                                  * static_cast<std::size_t>(dib.height);
        for (std::size_t i = 0; i < total; ++i) dib.pixels[i] = kSentinel;
        return true;
    }

    // 窗口客户区 → 回读缓冲；返回 FALSE 表示 BitBlt 失败。
    bool grab(HDC windowDc) {
        GdiFlush();
        return BitBlt(dib.dc, originX, originY, kWidth, kHeight, windowDc, 0, 0, SRCCOPY) != FALSE;
    }

    Frame frame() const {
        return Frame{dib.pixels + static_cast<std::size_t>(originY) * static_cast<std::size_t>(dib.width)
                                 + static_cast<std::size_t>(originX),
                     dib.width};
    }

    bool borderIsSentinel() const {
        for (int y = 0; y < dib.height; ++y) {
            const bool rowOutside = y < originY || y >= originY + kHeight;
            for (int x = 0; x < dib.width; ++x) {
                const bool outside = rowOutside || x < originX || x >= originX + kWidth;
                if (outside && dib.pixels[static_cast<std::size_t>(y)
                                          * static_cast<std::size_t>(dib.width)
                                          + static_cast<std::size_t>(x)] != kSentinel) {
                    return false;
                }
            }
        }
        return true;
    }
};

// 与 muzzle smoke 一致的 PNG 编码器 CLSID。
const CLSID kPngClsid = {0x557cf406, 0x1a04, 0x11d3,
                         {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

bool savePng(const Frame& frame, int stride, const char* path) {
    Gdiplus::Bitmap bitmap(kWidth, kHeight, stride, PixelFormat32bppRGB,
                           reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(frame.base)));
    return bitmap.Save(path, &kPngClsid, nullptr) == Gdiplus::Ok;
}

bool fileHasPngMagic(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    unsigned char magic[8] = {};
    const std::size_t got = std::fread(magic, 1, sizeof(magic), file);
    std::fclose(file);
    static const unsigned char expected[8] = {0x89, 0x50, 0x4e, 0x47,
                                              0x0d, 0x0a, 0x1a, 0x0a};
    return got == sizeof(expected) && std::memcmp(magic, expected, sizeof(expected)) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    // 崩溃时也要看到"跑到哪一步了"：默认 stdout 是块缓冲，段错误会把缓冲全丢掉。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pngPath = argc > 1 ? argv[1] : "/tmp/concord_main_system_font_window.png";
    using concord::ui::MainSystemFontRole;

    std::printf("== 窗口与字体 ==\n");

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = paintWndProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"Concord.MainSystemFont.WindowPaint";
    check(RegisterClassExW(&cls) != 0, "RegisterClassExW 返回非零 ATOM");

    PaintContext ctx;
    ctx.fonts.initialize();
    check(ctx.fonts.available(MainSystemFontRole::Button)
              && ctx.fonts.available(MainSystemFontRole::Title)
              && ctx.fonts.available(MainSystemFontRole::Small),
          "Button/Title/Small 三种角色都有 HFONT");
    check(ctx.fonts.capability() == concord::ui::MainSystemFontCapability::NativeText,
          "字体类上报 NativeText 能力");

    HWND window = CreateWindowExW(0, cls.lpszClassName, L"S4-3 窗口出字",
                                  WS_OVERLAPPEDWINDOW, 0, 0, kWidth, kHeight, nullptr,
                                  nullptr, GetModuleHandleW(nullptr), &ctx);
    check(window != nullptr, "CreateWindowExW 返回非空句柄");
    if (window == nullptr) {
        std::printf("checks=%d failures=%d\n", gChecks, gFailures);
        return gFailures != 0;
    }
    check(IsWindow(window) != FALSE, "IsWindow 成立");

    RECT client{};
    GetClientRect(window, &client);
    checkEqual(kWidth, client.right - client.left, "客户区宽 == 800");
    checkEqual(kHeight, client.bottom - client.top, "客户区高 == 600");

    check(ctx.paints == 0, "创建后尚未绘制");
    check(paintThroughQueue(window, ctx),
          "PostMessageW(WM_PAINT)+GetMessageW+DispatchMessageW 到达窗口过程并完成绘制");
    check(ctx.paintRectWasFullClient, "rcPaint 覆盖整个客户区（BeginPaint 语义）");

    HDC windowDc = GetDC(window);
    check(windowDc != nullptr, "GetDC 返回窗口客户区 DC");
    if (windowDc == nullptr) {
        DestroyWindow(window);
        std::printf("checks=%d failures=%d (没有窗口 DC，后续断言无法进行)\n", gChecks, gFailures);
        return gFailures != 0;
    }

    // Win32 的 GetObject **不接受 HDC**（真 Windows 上也返回 0）：必须先
    // GetCurrentObject(dc, OBJ_BITMAP) 取出"DC 当前位图"，再对它 GetObject。
    // 产品侧四处 raster 快路径本来就是这么写的（RuntimeSelectedPathRaster.h:209、
    // RuntimeSelectionFrameRaster.h:160、RuntimeRadarPixelStoreWin32.cpp:44、
    // RuntimeTerrainViewportFill.h:84），错的是本测试。
    DIBSECTION section{};
    HGDIOBJ windowBitmap = GetCurrentObject(windowDc, OBJ_BITMAP);
    check(windowBitmap != nullptr, "GetCurrentObject(dc, OBJ_BITMAP) 返回窗口 surface 位图");
    check(GetObjectW(windowBitmap, sizeof(section), &section) == sizeof(section),
          "GetObjectW(GetCurrentObject(dc,OBJ_BITMAP), sizeof(DIBSECTION)) 成立（三条 raster 快路径的硬门槛）");
    check(section.dsBm.bmBits != nullptr, "窗口 DC 关联真实像素指针");
    checkEqual(kWidth, section.dsBm.bmWidth, "DIBSECTION 宽 == 客户区宽");

    Readback readback;
    check(readback.create(), "回读 DIB 已建立（864x704，边界填哨兵）");
    // 没有像素缓冲就没有任何断言可做。这里必须提前收尾：继续往下走会解空指针，
    // 崩溃会掩盖真正的失败原因（本轮就踩了这条：段错误让人以为是绘制/字形出错）。
    if (readback.dib.pixels == nullptr) {
        ReleaseDC(window, windowDc);
        DestroyWindow(window);
        std::printf("checks=%d failures=%d (回读 DIB 建立失败：CreateDIBSection/SelectObject 未成功)\n",
                    gChecks, gFailures);
        return gFailures != 0;
    }
    check(readback.grab(windowDc), "BitBlt(窗口 DC → 回读 DIB) 成功");
    check(readback.borderIsSentinel(), "客户区以外的回读像素仍是哨兵（回读矩形精确等于客户区）");
    const Frame frame = readback.frame();

    std::printf("== 六个文本带都有墨迹 ==\n");
    const RECT bandA{40, 20, 760, 56};
    const RECT bandB{40, 70, 760, 110};
    const RECT bandC{40, 120, 760, 156};
    const RECT bandD{40, 170, 760, 206};
    const RECT bandE{40, 220, 780, 320};
    const RECT bandF{40, 340, 780, 376};
    const struct {
        const char* name;
        RECT band;
    } bands[] = {{"A 左对齐英文", bandA},
                 {"B 居中英文", bandB},
                 {"C 中英混排", bandC},
                 {"D 裁剪带", bandD},
                 {"E 长句（DT_WORDBREAK 未换行）", bandE},
                 {"F 小号字 14px", bandF}};
    for (const auto& band : bands) {
        const int ink = inkIn(frame, band.band.left, band.band.top, band.band.right,
                              band.band.bottom);
        check(ink > 0, band.name);
        if (ink == 0) std::printf("        （%s 带内墨迹 = 0）\n", band.name);
    }

    std::printf("== 先量后画不错位 ==\n");
    const std::wstring_view probeA = L"CONCORD RTS";
    HGDIOBJ previousFont = SelectObject(windowDc, ctx.fonts.nativeFont(MainSystemFontRole::Title));
    SIZE extent{};
    check(GetTextExtentPoint32W(windowDc, probeA.data(), static_cast<int>(probeA.size()), &extent)
              != FALSE,
          "GetTextExtentPoint32W 成功");
    SelectObject(windowDc, previousFont);
    // Title = 16px，窄字符前进宽度是固定栅格 5:7 → (16*5+3)/7 == 11。
    checkEqual(11LL * static_cast<long long>(probeA.size()), extent.cx,
               "GetTextExtentPoint32W 宽度 == 字宽栅格 × 字符数（11 × 11）");
    checkEqual(16, extent.cy, "GetTextExtentPoint32W 高度 == 字高（16）");

    RECT inkA{};
    const bool haveA = inkBounds(frame, bandA.left, bandA.top, bandA.right, bandA.bottom, &inkA);
    check(haveA, "A 带墨迹包围盒可求");
    if (haveA) {
        // 阴影整体偏移 (1,1)，所以右/下边界放宽 1 像素。
        check(inkA.left >= bandA.left && inkA.right <= bandA.left + extent.cx + 1,
              "A 带墨迹横向落在前进宽度内（含阴影 +1）");
        check(inkA.top >= bandA.top && inkA.bottom <= bandA.top + extent.cy + 1,
              "A 带墨迹纵向落在字高内（含阴影 +1）");
    }

    // DT_CALCRECT 与 GetTextExtentPoint32W 必须一致：产品用前者排版、后者量宽。
    {
        RECT calc{40, 400, 40, 400};
        previousFont = SelectObject(windowDc, ctx.fonts.nativeFont(MainSystemFontRole::Title));
        const int height = DrawTextW(windowDc, probeA.data(), static_cast<int>(probeA.size()),
                                     &calc, DT_CALCRECT | DT_SINGLELINE | DT_LEFT);
        SelectObject(windowDc, previousFont);
        checkEqual(extent.cx, calc.right - calc.left, "DrawTextW(DT_CALCRECT) 宽 == GetTextExtentPoint32W 宽");
        checkEqual(extent.cy, calc.bottom - calc.top, "DrawTextW(DT_CALCRECT) 高 == 字高");
        checkEqual(extent.cy, height, "DrawTextW(DT_CALCRECT) 返回值 == 字高");
    }

    std::printf("== 对齐真的生效（只平移参照点，看 bbox 位移） ==\n");
    const std::wstring_view probeText = L"ALIGN PROBE";
    auto clearBand = [&](int top, int height) { PatBlt(windowDc, 0, top, kWidth, height, WHITENESS); };
    auto drawProbe = [&](int left, int top, int right, int bottom, UINT format,
                         const std::wstring_view& text) {
        previousFont = SelectObject(windowDc, ctx.fonts.nativeFont(MainSystemFontRole::Title));
        SetBkMode(windowDc, TRANSPARENT);
        SetTextColor(windowDc, kColors.packedMiddle());
        RECT rect{left, top, right, bottom};
        DrawTextW(windowDc, text.data(), static_cast<int>(text.size()), &rect, format);
        SelectObject(windowDc, previousFont);
    };
    auto bboxAt = [&](int top, int height, RECT* out) {
        if (!readback.grab(windowDc)) return false;
        return inkBounds(readback.frame(), 0, top, kWidth, top + height, out);
    };

    RECT leftOne{}, leftTwo{};
    clearBand(400, 44);
    drawProbe(100, 400, 700, 436, DT_LEFT | DT_TOP | DT_SINGLELINE, probeText);
    const bool haveLeftOne = bboxAt(400, 40, &leftOne);
    clearBand(400, 44);
    drawProbe(140, 400, 700, 436, DT_LEFT | DT_TOP | DT_SINGLELINE, probeText);
    const bool haveLeftTwo = bboxAt(400, 40, &leftTwo);
    check(haveLeftOne && haveLeftTwo, "DT_LEFT 两次绘制都有墨迹");
    if (haveLeftOne && haveLeftTwo) {
        checkEqual(40, leftTwo.left - leftOne.left, "DT_LEFT：参照点 +40，墨迹整体 +40");
        checkEqual(0, leftTwo.top - leftOne.top, "DT_LEFT：纵向未移动");
    }

    RECT centeredOne{}, centeredTwo{};
    clearBand(444, 40);
    drawProbe(100, 444, 700, 480, DT_CENTER | DT_TOP | DT_SINGLELINE, probeText);
    const bool haveCenterOne = bboxAt(444, 36, &centeredOne);
    clearBand(444, 40);
    // 矩形左移 40 → 中心右移 20（宽度不变）
    drawProbe(140, 444, 700, 480, DT_CENTER | DT_TOP | DT_SINGLELINE, probeText);
    const bool haveCenterTwo = bboxAt(444, 36, &centeredTwo);
    check(haveCenterOne && haveCenterTwo, "DT_CENTER 两次绘制都有墨迹");
    if (haveCenterOne && haveCenterTwo) {
        checkEqual(20, centeredTwo.left - centeredOne.left, "DT_CENTER：矩形中心 +20，墨迹整体 +20");
    }

    RECT vcenterOne{}, vcenterTwo{};
    clearBand(484, 60);
    drawProbe(100, 484, 700, 520, DT_LEFT | DT_VCENTER | DT_SINGLELINE, probeText);
    const bool haveVOne = bboxAt(484, 40, &vcenterOne);
    clearBand(484, 60);
    drawProbe(100, 504, 700, 540, DT_LEFT | DT_VCENTER | DT_SINGLELINE, probeText);
    const bool haveVTwo = bboxAt(484, 60, &vcenterTwo);
    check(haveVOne && haveVTwo, "DT_VCENTER 两次绘制都有墨迹");
    if (haveVOne && haveVTwo) {
        checkEqual(20, vcenterTwo.top - vcenterOne.top, "DT_VCENTER：矩形下移 20，墨迹整体 +20");
    }

    std::printf("== CJK：索引可查 + 单元格内部有墨 ==\n");
    {
        WORD glyphs[4] = {0, 0, 0, 0};
        const DWORD count = GetGlyphIndicesW(windowDc, L"中文菜单", 4, glyphs,
                                             GGI_MARK_NONEXISTING_GLYPHS);
        checkEqual(4, count, "GetGlyphIndicesW 返回 4 个索引");
        bool allPresent = true;
        for (int i = 0; i < 4; ++i) {
            if (glyphs[i] == 0xFFFF) allPresent = false;
        }
        // 这一条挡的是"GetGlyphIndicesW 恒报 0xFFFF（字体缺失）"与
        // "恒报有效索引（未分配码位也返回 .notdef）"两种错法。
        check(allPresent, "四个汉字都有真实字形索引（无 0xFFFF）");

        clearBand(550, 44);
        previousFont = SelectObject(windowDc, ctx.fonts.nativeFont(MainSystemFontRole::Title));
        SetBkMode(windowDc, TRANSPARENT);
        SetTextColor(windowDc, kColors.packedMiddle());
        SetTextAlign(windowDc, TA_LEFT | TA_TOP);
        check(TextOutW(windowDc, 40, 560, L"中", 1) != FALSE, "TextOutW 画单个汉字返回 TRUE");
        SelectObject(windowDc, previousFont);
        check(readback.grab(windowDc), "汉字带回读成功");
        // 单元格 = 16x16（1:1），内部 = 去掉 1 像素边框。空心豆腐块只有边框，
        // 内部恒为零；真实汉字内部必然有若干墨点。
        const int interior = inkIn(readback.frame(), 41, 561, 55, 575);
        check(interior > 5, "汉字单元格内部有墨迹（不是空心豆腐块）");
        if (interior <= 5) std::printf("        （内部墨迹 = %d）\n", interior);
    }

    std::printf("== 裁剪区生效 ==\n");
    {
        const int insideRight = inkIn(frame, bandD.left, bandD.top, 240, bandD.bottom);
        const int outsideRight = inkIn(frame, 240, bandD.top, bandD.right, bandD.bottom);
        check(insideRight > 0, "裁剪框内（x < 240）有墨迹");
        checkEqual(0, outsideRight, "裁剪框右侧（x >= 240）一个墨点都没有");
    }

    std::printf("== 确定性：同一内容画两次逐字节一致 ==\n");
    {
        Readback first;
        Readback second;
        const bool firstReady = first.create();
        const bool secondReady = second.create();
        check(firstReady && secondReady, "两份确定性回读 DIB 已建立");
        if (firstReady && secondReady) {
            check(paintThroughQueue(window, ctx), "第 1 次重绘完成");
            check(first.grab(windowDc), "第 1 次回读成功");
            check(paintThroughQueue(window, ctx), "第 2 次重绘完成");
            check(second.grab(windowDc), "第 2 次回读成功");
            std::size_t differences = 0;
            const Frame a = first.frame();
            const Frame b = second.frame();
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) {
                    if (a.at(x, y) != b.at(x, y)) ++differences;
                }
            }
            checkEqual(0, static_cast<long long>(differences), "两次绘制的客户区逐字节相同");
        }
    }

    std::printf("== PNG 存盘（肉眼验收物） ==\n");
    {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        check(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok, "GdiplusStartup 返回 Ok");
        check(paintThroughQueue(window, ctx), "存盘前再重绘一次（清掉探针痕迹）");
        OwnedDib picture;
        const bool pictureReady = picture.create(kWidth, kHeight);
        check(pictureReady, "PNG 用的 800x600 DIB 已建立");
        if (pictureReady) {
            GdiFlush();
            check(BitBlt(picture.dc, 0, 0, kWidth, kHeight, windowDc, 0, 0, SRCCOPY) != FALSE,
                  "BitBlt 客户区 → PNG 缓冲");
            const Frame pictureFrame{picture.pixels, picture.width};
            check(savePng(pictureFrame, picture.width * 4, pngPath), "GDI+ Bitmap::Save 写出 PNG");
            check(fileHasPngMagic(pngPath), "写出文件的头 8 字节是 PNG 魔数");
        }
        std::printf("        PNG: %s\n", pngPath);
        if (token != 0) Gdiplus::GdiplusShutdown(token);
    }

    ReleaseDC(window, windowDc);
    check(DestroyWindow(window) != FALSE, "DestroyWindow 成功");
    check(IsWindow(window) == FALSE, "销毁后 IsWindow 失效");

    std::printf(
        "Main system font window paint: checks=%d paints=%d png=%s failures=%d\n",
        gChecks, ctx.paints, pngPath, gFailures);
    return gFailures != 0;
}
