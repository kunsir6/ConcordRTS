// compat/win32 自测 · 图元光栅（S3-3，C 段 11 个符号）
//
// 为什么这一族必须用**硬编码像素**断言，而不能像 raster 契约那样"快慢两路比":
// 那些契约两侧共用同一套 GDI，只能证明快路径与慢路径一致。LineTo 差一像素、
// FillRect 把右边界画成闭区间、裁剪被忽略 —— 这几种错法下两侧**同时**错，
// 断言全绿。S3-3 的三条硬要求（半开边界 / 裁剪真生效 / 映射真生效）全部属于
// 这一类，所以这里的期望值一律写死成 uint32 字面量。
//
// 覆盖：
//   半开边界       FillRect / FrameRect / Rectangle 的 right、bottom 归属
//   LineTo 语义    终点不画、当前位置更新、MoveToEx 回传移动前坐标
//   折线           终点不画、起点只画一次、Polygon 闭合边包含起点
//   形状           椭圆/圆角矩形的逐行区间（四角必须真的是空的）
//   裁剪           SelectClipRgn 之后 FillRect/Ellipse/GdiAlphaBlend 都被裁
//   映射           MM_ANISOTROPIC 缩一半 + 视口原点平移
//   ROP2           XOR 画两遍必须**逐字节**还原（这同时证明填充与描边不重叠）
//   混合           GdiAlphaBlend 的常量 alpha 与预乘 alpha 两条公式
//
// 运行：
//   c++ -std=c++20 -I compat/win32/include \
//       compat/win32/src/RuntimeWin32CompatGdi.cpp \
//       compat/win32/src/RuntimeWin32CompatMisc.cpp \
//       compat/win32/selftest/CompatDrawSemantics.cpp -o /tmp/draw && /tmp/draw

#include <windows.h>

#include <cstdint>
#include <cstdio>

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

void checkInt(long long expected, long long actual, const char* description) {
    ++gChecks;
    if (expected != actual) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected %lld, got %lld)\n", description, expected, actual);
    }
}

// 像素字面量（内存里的 uint32 就是 0xAARRGGBB，与 toPixel 的产物同构）。
constexpr std::uint32_t kBackground = 0xFF112233u;
constexpr std::uint32_t kWhite = 0xFFFFFFFFu;
constexpr std::uint32_t kBlack = 0xFF000000u;
constexpr std::uint32_t kBlue = 0xFF0000FFu;

struct Surface {
    static constexpr int width = 40;
    static constexpr int height = 32;

    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    std::uint32_t* bits = nullptr;
    // 实际尺寸，create() 传入的可以比默认值小（1x1 的混合源）。
    // 用常量 width/height 去填小位图会写穿堆——这不是理论风险，第一版就踩了。
    int actualWidth = 0;
    int actualHeight = 0;

    bool create(int w = width, int h = height, bool topDown = true) {
        actualWidth = w;
        actualHeight = h;
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = topDown ? -h : h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* raw = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
        if (bitmap == nullptr) return false;
        bits = static_cast<std::uint32_t*>(raw);
        previous = SelectObject(dc, reinterpret_cast<HGDIOBJ>(bitmap));
        fillAll(kBackground);
        return true;
    }

    ~Surface() {
        if (dc != nullptr) {
            if (previous != nullptr) SelectObject(dc, previous);
            DeleteDC(dc);
        }
        if (bitmap != nullptr) DeleteObject(reinterpret_cast<HGDIOBJ>(bitmap));
    }

    void fillAll(std::uint32_t value) const {
        for (int i = 0; i < actualWidth * actualHeight; ++i) bits[i] = value;
    }

    std::uint32_t at(int x, int y) const { return bits[y * actualWidth + x]; }
};

void checkPixel(const Surface& surface, int x, int y, std::uint32_t expected, const char* description) {
    ++gChecks;
    const std::uint32_t actual = surface.at(x, y);
    if (actual != expected) {
        ++gFailures;
        std::printf("  FAIL  %s  at (%d,%d): expected 0x%08X, got 0x%08X\n", description, x, y,
                    static_cast<unsigned>(expected), static_cast<unsigned>(actual));
    }
}

// 把整块表面与某个值逐一比较，用于"XOR 两遍必须完全还原"这类全画面断言。
void checkWholeSurface(const Surface& surface, std::uint32_t expected, const char* description) {
    ++gChecks;
    for (int i = 0; i < surface.actualWidth * surface.actualHeight; ++i) {
        if (surface.bits[i] != expected) {
            ++gFailures;
            std::printf("  FAIL  %s  (first mismatch at index %d: expected 0x%08X, got 0x%08X)\n",
                        description, i, static_cast<unsigned>(expected),
                        static_cast<unsigned>(surface.bits[i]));
            return;
        }
    }
}

void reset(Surface& surface) {
    surface.fillAll(kBackground);
    SetROP2(surface.dc, R2_COPYPEN);
    SetMapMode(surface.dc, MM_TEXT);
    SetViewportOrgEx(surface.dc, 0, 0, nullptr);
    SetWindowOrgEx(surface.dc, 0, 0, nullptr);
    SetWindowExtEx(surface.dc, 1, 1, nullptr);
    SetViewportExtEx(surface.dc, 1, 1, nullptr);
    SelectClipRgn(surface.dc, nullptr);
}

// ---------------------------------------------------------------------------

void testFillRectBoundaries() {
    Surface surface;
    check(surface.create(), "表面创建（FillRect）");
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    check(brush != nullptr, "白色画刷创建");
    const RECT rect{2, 3, 6, 7};
    checkInt(1, FillRect(surface.dc, &rect, brush), "FillRect 返回非零");

    checkPixel(surface, 2, 3, kWhite, "FillRect 含左上角");
    checkPixel(surface, 5, 6, kWhite, "FillRect 含右下角内侧像素");
    // 半开：right=6 那一列、bottom=7 那一行都不属于这个矩形。
    checkPixel(surface, 6, 3, kBackground, "FillRect 不含 right 列");
    checkPixel(surface, 2, 7, kBackground, "FillRect 不含 bottom 行");
    checkPixel(surface, 1, 3, kBackground, "FillRect 不含 left 左侧");

    // 空矩形：right<=left 不画，但返回成功（Win32 语义）。
    reset(surface);
    const RECT inverted{8, 8, 4, 4};
    checkInt(1, FillRect(surface.dc, &inverted, brush), "反向矩形返回成功");
    checkPixel(surface, 6, 6, kBackground, "反向矩形不画任何像素");

    // NULL_BRUSH：成功但不填。
    reset(surface);
    HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    checkInt(1, FillRect(surface.dc, &rect, hollow), "NULL_BRUSH 的 FillRect 返回成功");
    checkPixel(surface, 3, 4, kBackground, "NULL_BRUSH 不填任何像素");

    DeleteObject(reinterpret_cast<HGDIOBJ>(brush));
}

void testFillRectClip() {
    Surface surface;
    check(surface.create(), "表面创建（裁剪）");
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    checkInt(2, IntersectClipRect(surface.dc, 3, 0, 5, 32), "裁剪区为单个矩形 → SIMPLEREGION(2)");
    const RECT rect{0, 0, 20, 10};
    checkInt(1, FillRect(surface.dc, &rect, brush), "裁剪下 FillRect 返回成功");

    checkPixel(surface, 2, 2, kBackground, "裁剪左侧不放行");
    checkPixel(surface, 3, 2, kWhite, "裁剪左边界含");
    checkPixel(surface, 4, 2, kWhite, "裁剪右边界内侧含");
    checkPixel(surface, 5, 2, kBackground, "裁剪右边界不含（半开）");
    DeleteObject(reinterpret_cast<HGDIOBJ>(brush));
}

void testFrameRectUsesBrush() {
    Surface surface;
    check(surface.create(), "表面创建（FrameRect）");
    // 笔设成红色：FrameRect 用的是**画刷**，若实现拿笔的颜色画，这条会红。
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
    HGDIOBJ oldPen = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(pen));
    HBRUSH brush = CreateSolidBrush(RGB(0, 0, 255));
    const RECT rect{2, 2, 8, 8};
    checkInt(1, FrameRect(surface.dc, &rect, brush), "FrameRect 返回非零");

    checkPixel(surface, 2, 2, kBlue, "FrameRect 左上角用画刷颜色");
    checkPixel(surface, 7, 2, kBlue, "FrameRect 右上角在 right-1 上");
    checkPixel(surface, 2, 7, kBlue, "FrameRect 下边在 bottom-1 上");
    checkPixel(surface, 8, 2, kBackground, "FrameRect 不含 right 列");
    checkPixel(surface, 2, 8, kBackground, "FrameRect 不含 bottom 行");
    checkPixel(surface, 4, 4, kBackground, "FrameRect 不填内部");

    SelectObject(surface.dc, oldPen);
    DeleteObject(reinterpret_cast<HGDIOBJ>(pen));
    DeleteObject(reinterpret_cast<HGDIOBJ>(brush));
}

void testRectangle() {
    Surface surface;
    check(surface.create(), "表面创建（Rectangle）");
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(pen));
    HGDIOBJ oldBrush = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(brush));
    check(Rectangle(surface.dc, 2, 2, 10, 10), "Rectangle 返回成功");

    checkPixel(surface, 2, 2, kBlack, "Rectangle 左边框");
    checkPixel(surface, 9, 2, kBlack, "Rectangle 右边框在 right-1");
    checkPixel(surface, 2, 9, kBlack, "Rectangle 下边框在 bottom-1");
    checkPixel(surface, 5, 5, kWhite, "Rectangle 内部用画刷填充");
    checkPixel(surface, 3, 2, kBlack, "Rectangle 上边框连续");
    checkPixel(surface, 10, 2, kBackground, "Rectangle 不含 right 列");
    checkPixel(surface, 2, 10, kBackground, "Rectangle 不含 bottom 行");

    // 笔宽 2：按内侧对齐画 2 像素（GDI 是沿路径居中，差半个像素——
    // 见实现文件头「有意偏离」第二条）。
    reset(surface);
    HPEN wide = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
    SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(wide));
    check(Rectangle(surface.dc, 2, 2, 10, 10), "宽笔 Rectangle 返回成功");
    checkPixel(surface, 2, 5, kBlack, "宽笔第 1 圈");
    checkPixel(surface, 3, 5, kBlack, "宽笔第 2 圈（内侧）");
    checkPixel(surface, 4, 5, kWhite, "宽笔内侧之外是填充");
    DeleteObject(reinterpret_cast<HGDIOBJ>(wide));

    SelectObject(surface.dc, oldPen);
    SelectObject(surface.dc, oldBrush);
    DeleteObject(reinterpret_cast<HGDIOBJ>(pen));
    DeleteObject(reinterpret_cast<HGDIOBJ>(brush));
}

void testLineSemantics() {
    Surface surface;
    check(surface.create(), "表面创建（LineTo）");
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HGDIOBJ oldPen = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(pen));

    POINT previous{-1, -1};
    check(MoveToEx(surface.dc, 0, 0, &previous), "MoveToEx 返回成功");
    checkInt(0, previous.x, "首次 MoveToEx 的前一位置是初值 (0,0)");
    check(LineTo(surface.dc, 5, 0), "LineTo 返回成功");
    checkPixel(surface, 0, 0, kBlack, "LineTo 含起点");
    checkPixel(surface, 4, 0, kBlack, "LineTo 含终点前一像素");
    // 终点不画：这是 Win32 文档语义，也是"连续 LineTo 不重复写端点"的前提。
    checkPixel(surface, 5, 0, kBackground, "LineTo 不含终点");

    POINT position{-1, -1};
    check(GetCurrentPositionEx(surface.dc, &position), "GetCurrentPositionEx 返回成功");
    checkInt(5, position.x, "LineTo 之后当前位置前进到终点 x");
    checkInt(0, position.y, "LineTo 之后当前位置前进到终点 y");

    // MoveToEx 回传的是**移动前**的坐标。
    POINT moved{-1, -1};
    check(MoveToEx(surface.dc, 2, 7, &moved), "第二次 MoveToEx 成功");
    checkInt(5, moved.x, "MoveToEx 回传移动前的 x");
    checkInt(0, moved.y, "MoveToEx 回传移动前的 y");

    // 斜线：4 邻域连通且不少画（(0,0)→(2,2) 应对角 3 个像素，终点除外）。
    reset(surface);
    MoveToEx(surface.dc, 0, 0, nullptr);
    LineTo(surface.dc, 3, 3);
    checkPixel(surface, 0, 0, kBlack, "斜线起点");
    checkPixel(surface, 1, 1, kBlack, "斜线中段");
    checkPixel(surface, 2, 2, kBlack, "斜线末段（终点前）");
    checkPixel(surface, 3, 3, kBackground, "斜线不含终点");

    // PS_NULL：不画，但当前位置仍然前进。
    reset(surface);
    HPEN nullPen = CreatePen(PS_NULL, 1, RGB(0, 0, 0));
    SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(nullPen));
    MoveToEx(surface.dc, 0, 0, nullptr);
    LineTo(surface.dc, 4, 0);
    checkPixel(surface, 2, 0, kBackground, "PS_NULL 不画像素");
    GetCurrentPositionEx(surface.dc, &position);
    checkInt(4, position.x, "PS_NULL 下当前位置照样前进");
    DeleteObject(reinterpret_cast<HGDIOBJ>(nullPen));

    SelectObject(surface.dc, oldPen);
    DeleteObject(reinterpret_cast<HGDIOBJ>(pen));
}

void testPolylineAndPolygon() {
    Surface surface;
    check(surface.create(), "表面创建（折线 / 多边形）");
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(pen));
    HGDIOBJ oldBrush = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(brush));

    POINT line[3] = {{0, 0}, {0, 5}, {5, 5}};
    check(Polyline(surface.dc, line, 3), "Polyline 返回成功");
    checkPixel(surface, 0, 0, kBlack, "折线起点");
    checkPixel(surface, 0, 4, kBlack, "第一段倒数第二点");
    checkPixel(surface, 0, 5, kBlack, "拐点被画（第二段起点）");
    checkPixel(surface, 4, 5, kBlack, "第二段倒数第二点");
    checkPixel(surface, 5, 5, kBackground, "折线不含最后一点");

    // 多边形：闭合轮廓 + ALTERNATE 填充。
    reset(surface);
    POINT triangle[3] = {{0, 0}, {8, 0}, {0, 8}};
    check(Polygon(surface.dc, triangle, 3), "Polygon 返回成功");
    checkPixel(surface, 0, 0, kBlack, "多边形顶点被描边（闭合边含起点）");
    checkPixel(surface, 1, 1, kWhite, "多边形内部被填充");
    checkPixel(surface, 7, 7, kBackground, "多边形外部不填");
    // 斜边 x+y=8 上的像素属于轮廓，不是"外部"——第一版把 (4,4) 当成外部，
    // 是错的期望值而不是错的实现。
    checkPixel(surface, 4, 4, kBlack, "斜边上的像素属于轮廓");

    // 少于三个点：不画，但返回成功。
    check(Polygon(surface.dc, triangle, 2), "两个点的 Polygon 返回成功");

    SelectObject(surface.dc, oldPen);
    SelectObject(surface.dc, oldBrush);
    DeleteObject(reinterpret_cast<HGDIOBJ>(pen));
    DeleteObject(reinterpret_cast<HGDIOBJ>(brush));
}

void testEllipse() {
    Surface surface;
    check(surface.create(), "表面创建（Ellipse）");
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(pen));
    HGDIOBJ oldBrush = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(brush));
    check(Ellipse(surface.dc, 0, 0, 10, 10), "Ellipse 返回成功");

    // 四角必须真的是空的——这正是"拿矩形近似椭圆区域"会多放行的地方。
    checkPixel(surface, 0, 0, kBackground, "椭圆左上角为空");
    checkPixel(surface, 9, 0, kBackground, "椭圆右上角为空");
    checkPixel(surface, 0, 9, kBackground, "椭圆左下角为空");
    checkPixel(surface, 9, 9, kBackground, "椭圆右下角为空");
    checkPixel(surface, 5, 0, kBlack, "椭圆顶点为描边");
    checkPixel(surface, 0, 5, kBlack, "椭圆最左为描边");
    checkPixel(surface, 5, 5, kWhite, "椭圆中心为填充");
    // 行 0 的可见区间是 [3,7)：逐行区间来自 ellipseRowSpan，与区域共用同一函数。
    checkPixel(surface, 2, 0, kBackground, "椭圆行 0 起点左侧为空");
    checkPixel(surface, 7, 0, kBackground, "椭圆行 0 终点右侧为空");

    // 椭圆区域与椭圆图元必须给出同一几何：两处各写一套浮点就会差一像素，
    // 而两者互不比较，谁都不会报错。
    HRGN region = CreateEllipticRgn(0, 0, 10, 10);
    check(region != nullptr, "CreateEllipticRgn 创建成功");
    if (region != nullptr) {
        checkInt(TRUE, PtInRegion(region, 5, 5) ? TRUE : FALSE, "椭圆区域含中心");
        checkInt(FALSE, PtInRegion(region, 0, 0) ? TRUE : FALSE, "椭圆区域不含左上角");
        checkInt(TRUE, PtInRegion(region, 3, 0) ? TRUE : FALSE, "椭圆区域含行 0 的首个像素");
        checkInt(FALSE, PtInRegion(region, 2, 0) ? TRUE : FALSE, "椭圆区域不含行 0 之前一像素");
        RECT box{-1, -1, -1, -1};
        // 椭圆区域在 Win32 里本来就是 COMPLEXREGION（条带表逐行不同高/宽），
        // 这里返回 3 才与 Windows 一致。第一版期望 2，是拿"矩形区域"的直觉套上去。
        checkInt(3, GetRgnBox(region, &box), "椭圆区域为 COMPLEXREGION(3)");
        checkInt(0, box.left, "椭圆区域外接框 left");
        checkInt(10, box.right, "椭圆区域外接框 right");
        checkInt(0, box.top, "椭圆区域外接框 top");
        checkInt(10, box.bottom, "椭圆区域外接框 bottom");
        DeleteObject(reinterpret_cast<HGDIOBJ>(region));
    }

    SelectObject(surface.dc, oldPen);
    SelectObject(surface.dc, oldBrush);
    DeleteObject(reinterpret_cast<HGDIOBJ>(pen));
    DeleteObject(reinterpret_cast<HGDIOBJ>(brush));
}

void testRoundRect() {
    Surface surface;
    check(surface.create(), "表面创建（RoundRect）");
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(pen));
    HGDIOBJ oldBrush = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(brush));
    check(RoundRect(surface.dc, 0, 0, 20, 20, 8, 8), "RoundRect 返回成功");

    checkPixel(surface, 0, 0, kBackground, "圆角矩形左上角被削掉");
    checkPixel(surface, 19, 0, kBackground, "圆角矩形右上角被削掉");
    checkPixel(surface, 10, 10, kWhite, "圆角矩形中心为填充");
    checkPixel(surface, 10, 0, kBlack, "圆角矩形上边中段为描边");
    checkPixel(surface, 0, 10, kBlack, "圆角矩形左边中段为描边");
    checkPixel(surface, 2, 0, kBlack, "圆角矩形上边起点（角椭圆与直边相接处）");

    SelectObject(surface.dc, oldPen);
    SelectObject(surface.dc, oldBrush);
    DeleteObject(reinterpret_cast<HGDIOBJ>(pen));
    DeleteObject(reinterpret_cast<HGDIOBJ>(brush));
}

void testMapping() {
    Surface surface;
    check(surface.create(), "表面创建（映射）");
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));

    // MM_ANISOTROPIC + windowExt=20, viewportExt=10：逻辑坐标缩一半。
    SetMapMode(surface.dc, MM_ANISOTROPIC);
    SetWindowExtEx(surface.dc, 20, 20, nullptr);
    SetViewportExtEx(surface.dc, 10, 10, nullptr);
    const RECT rect{0, 0, 8, 8};
    checkInt(1, FillRect(surface.dc, &rect, brush), "映射下 FillRect 返回成功");
    checkPixel(surface, 3, 3, kWhite, "缩放后的矩形含 (3,3)");
    checkPixel(surface, 4, 4, kBackground, "缩放后的矩形不含 (4,4)（8/2=4）");

    // 视口原点平移：(2,1) + 逻辑 (0,0)-(2,2) → 设备 (2,1)-(4,3)。
    reset(surface);
    SetViewportOrgEx(surface.dc, 2, 1, nullptr);
    const RECT small{0, 0, 2, 2};
    checkInt(1, FillRect(surface.dc, &small, brush), "视口原点下 FillRect 返回成功");
    checkPixel(surface, 2, 1, kWhite, "视口原点平移生效（左上）");
    checkPixel(surface, 3, 2, kWhite, "视口原点平移后仍在矩形内");
    checkPixel(surface, 4, 3, kBackground, "视口原点平移后右下半开");
    checkPixel(surface, 1, 1, kBackground, "视口原点平移后左侧不画");

    // SetMapMode 必须真的改变行为：同一矩形在 MM_TEXT 下位置不同。
    reset(surface);
    checkInt(1, FillRect(surface.dc, &rect, brush), "MM_TEXT 下 FillRect 返回成功");
    checkPixel(surface, 7, 7, kWhite, "MM_TEXT 下矩形不缩放");

    DeleteObject(reinterpret_cast<HGDIOBJ>(brush));
}

void testRop2XorRestores() {
    Surface surface;
    check(surface.create(), "表面创建（ROP2）");
    // 笔用红色：黑色在 XOR 下是恒等运算（0x112233 ^ 0x000000 不变），
    // 用黑笔写这条断言会让"根本没画"和"画对了"给出同一个结果。
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(pen));
    HGDIOBJ oldBrush = SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(brush));

    SetROP2(surface.dc, R2_XORPEN);
    check(Rectangle(surface.dc, 2, 2, 12, 12), "XOR 第一遍 Rectangle 成功");
    // 半开边界上 XOR：填充区与描边区不相交，所以每个像素恰好被写一次。
    // 目标 alpha 字节保留（0xFF），这是 applyRop2 的约定。
    checkPixel(surface, 5, 5, 0xFFEEDDCCu, "XOR 填充 = 背景 ^ 画刷色");
    checkPixel(surface, 2, 5, 0xFFEE2233u, "XOR 描边 = 背景 ^ 笔色");

    check(Rectangle(surface.dc, 2, 2, 12, 12), "XOR 第二遍 Rectangle 成功");
    // 这条断言是"填充与描边不重叠"的唯一硬证据：若有任何像素被写两次，
    // 第二遍之后那块区域就回不到背景色，而"擦干净"正是项目用 XOR 的目的。
    checkWholeSurface(surface, kBackground, "XOR 画两遍后整块表面逐字节还原");

    SetROP2(surface.dc, R2_COPYPEN);
    SelectObject(surface.dc, oldPen);
    SelectObject(surface.dc, oldBrush);
    DeleteObject(reinterpret_cast<HGDIOBJ>(pen));
    DeleteObject(reinterpret_cast<HGDIOBJ>(brush));
}

void testAlphaBlend() {
    Surface surface;
    check(surface.create(), "表面创建（AlphaBlend）");
    Surface source;
    check(source.create(1, 1), "1x1 源表面创建（AlphaBlend）");
    source.bits[0] = kWhite;

    // 目标全黑，源 1x1 白，常量 alpha 128：
    //   (255*128 + 0*127 + 127)/255 = 128
    // 这正是 RuntimeShell 的超级武器侧栏遮罩用法（1x1 拉伸 + 常量 alpha + 黑源）。
    surface.fillAll(kBlack);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 128, 0};
    check(GdiAlphaBlend(surface.dc, 0, 0, 4, 4, source.dc, 0, 0, 1, 1, blend), "常量 alpha 混合成功");
    checkPixel(surface, 0, 0, 0xFF808080u, "常量 alpha=128 的混合结果");
    checkPixel(surface, 3, 3, 0xFF808080u, "1x1 源被拉伸覆盖整个目标矩形");
    checkPixel(surface, 4, 0, kBlack, "混合矩形右下半开");

    // 目标白、源黑：255 - 128 ≈ 127。
    surface.fillAll(kWhite);
    source.bits[0] = kBlack;
    check(GdiAlphaBlend(surface.dc, 0, 0, 2, 2, source.dc, 0, 0, 1, 1, blend), "黑源混合成功");
    checkPixel(surface, 0, 0, 0xFF7F7F7Fu, "黑源 alpha=128 的混合结果");

    // 预乘 alpha：源 = 预乘后的半透明白 (128,128,128,128)，常量 alpha 255。
    // 直通公式会得到 (128*255 + 0*0)/255 = 128 —— 与预乘公式的错误实现
    // (128*128+0*127)/255 = 64 相差一倍，这条断言就是分水岭。
    surface.fillAll(kBlack);
    source.bits[0] = 0x80808080u;
    BLENDFUNCTION premultiplied{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    check(GdiAlphaBlend(surface.dc, 0, 0, 1, 1, source.dc, 0, 0, 1, 1, premultiplied),
          "预乘 alpha 混合成功");
    checkPixel(surface, 0, 0, 0xFF808080u, "预乘源直接叠加到黑底");

    // 裁剪必须对混合生效（RuntimeShell 的遮罩就在裁剪区里画）。
    surface.fillAll(kBlack);
    IntersectClipRect(surface.dc, 0, 0, 2, 4);
    check(GdiAlphaBlend(surface.dc, 0, 0, 4, 4, source.dc, 0, 0, 1, 1, premultiplied),
          "裁剪下混合成功");
    checkPixel(surface, 1, 1, 0xFF808080u, "裁剪内被混合");
    checkPixel(surface, 2, 1, kBlack, "裁剪外不被混合");

    // 全透明：不写像素，返回成功。
    SelectClipRgn(surface.dc, nullptr);
    surface.fillAll(kBlack);
    BLENDFUNCTION transparent{AC_SRC_OVER, 0, 0, 0};
    check(GdiAlphaBlend(surface.dc, 0, 0, 2, 2, source.dc, 0, 0, 1, 1, transparent),
          "alpha=0 返回成功");
    checkPixel(surface, 0, 0, kBlack, "alpha=0 不写像素");
}

void testClipDrivesComplexRegion() {
    // 复杂裁剪（两条条带）必须对图元生效——这正是 GetClipBox 三态存在的意义：
    // 三条快路径靠它决定让不让路，若像素路径不读 clip，判据就毫无作用。
    Surface surface;
    check(surface.create(), "表面创建（复杂裁剪）");
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    HRGN upper = CreateRectRgn(0, 0, 6, 4);
    HRGN lower = CreateRectRgn(10, 20, 16, 24);
    HRGN combined = CreateRectRgn(0, 0, 1, 1);
    checkInt(3, CombineRgn(combined, upper, lower, RGN_OR), "两条条带的组合区为 COMPLEXREGION(3)");
    checkInt(3, SelectClipRgn(surface.dc, combined), "选中复杂裁剪后返回 COMPLEXREGION(3)");
    DeleteObject(reinterpret_cast<HGDIOBJ>(upper));
    DeleteObject(reinterpret_cast<HGDIOBJ>(lower));
    DeleteObject(reinterpret_cast<HGDIOBJ>(combined));

    const RECT rect{0, 0, 40, 32};
    checkInt(1, FillRect(surface.dc, &rect, brush), "复杂裁剪下 FillRect 返回成功");
    checkPixel(surface, 3, 2, kWhite, "第一条条带被填充");
    checkPixel(surface, 3, 10, kBackground, "两条条带之间不被填充");
    checkPixel(surface, 12, 22, kWhite, "第二条条带被填充");
    checkPixel(surface, 20, 22, kBackground, "第二条条带右侧不被填充");

    DeleteObject(reinterpret_cast<HGDIOBJ>(brush));
}

}  // namespace

int main() {
    testFillRectBoundaries();
    testFillRectClip();
    testFrameRectUsesBrush();
    testRectangle();
    testLineSemantics();
    testPolylineAndPolygon();
    testEllipse();
    testRoundRect();
    testMapping();
    testRop2XorRestores();
    testAlphaBlend();
    testClipDrivesComplexRegion();

    std::printf("Draw primitives: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
