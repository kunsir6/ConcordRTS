// compat/win32 自测 · DC 状态与查询（S3-1，A 段 25 个符号）
//
// 这一族没有一个函数会画像素，所以断言的重点不是"画得对不对"，而是：
//   1. 初值是否等于 Win32 文档值。三条快路径（RuntimeSelectedPathRaster /
//      RuntimeSelectionFrameRaster / RuntimeRadarPixelStore）都靠逐项比对 DC
//      状态来决定要不要走快路，初值给错 → 快路永远关闭 → 所有"快慢两路一致"
//      的契约仍然全绿。这是**静默降级**，比崩溃难查得多。
//   2. 常量本身是否写对。下面用**字面量**断言（`GetROP2(dc) == 13` 而不是
//      `== R2_COPYPEN`）：拿常量自比时，头文件把常量抄错也照样通过。
//   3. GetObjectW 的返回值是否等于 sizeof。调用点写的是
//      `GetObjectW(...) == sizeof(DIBSECTION)`，返回别的数字等于语义不对。
//
// 运行：
//   c++ -std=c++20 -I compat/win32/include \
//       compat/win32/src/RuntimeWin32CompatGdi.cpp \
//       compat/win32/src/RuntimeWin32CompatMisc.cpp \
//       compat/win32/selftest/CompatDcStateSemantics.cpp -o /tmp/dcs && /tmp/dcs

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

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

struct Surface {
    static constexpr int width = 64;
    static constexpr int height = 40;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void* bits = nullptr;

    bool create(bool topDown = true) {
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = topDown ? -height : height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr) return false;
        previous = SelectObject(dc, reinterpret_cast<HGDIOBJ>(bitmap));
        return true;
    }

    ~Surface() {
        if (dc != nullptr) {
            if (previous != nullptr) SelectObject(dc, previous);
            DeleteDC(dc);
        }
        if (bitmap != nullptr) DeleteObject(reinterpret_cast<HGDIOBJ>(bitmap));
    }
};

// ---------------------------------------------------------------------------
// 1. 常量字面量：头层抄错会在这里被抓住，而不是在"快路永远关闭"上
// ---------------------------------------------------------------------------
void testConstantLiterals() {
    checkInt(13, R2_COPYPEN, "R2_COPYPEN 是 13");
    checkInt(7, R2_XORPEN, "R2_XORPEN 是 7");
    checkInt(1, MM_TEXT, "MM_TEXT 是 1");
    checkInt(8, MM_ANISOTROPIC, "MM_ANISOTROPIC 是 8");
    checkInt(1, GM_COMPATIBLE, "GM_COMPATIBLE 是 1");
    checkInt(2, GM_ADVANCED, "GM_ADVANCED 是 2");
    checkInt(1, AD_COUNTERCLOCKWISE, "AD_COUNTERCLOCKWISE 是 1（不是 0）");
    checkInt(2, AD_CLOCKWISE, "AD_CLOCKWISE 是 2");
    checkInt(0, LAYOUT_LTR, "LAYOUT_LTR 是 0");
    checkInt(1, LAYOUT_RTL, "LAYOUT_RTL 是 1");
    checkInt(1, NULLREGION, "NULLREGION 是 1");
    checkInt(2, SIMPLEREGION, "SIMPLEREGION 是 2");
    checkInt(3, COMPLEXREGION, "COMPLEXREGION 是 3");
    checkInt(7, OBJ_BITMAP, "OBJ_BITMAP 是 7");
    checkInt(6, OBJ_FONT, "OBJ_FONT 是 6");
    checkInt(2, OBJ_BRUSH, "OBJ_BRUSH 是 2");
    checkInt(1, OBJ_PEN, "OBJ_PEN 是 1");
    checkInt(12, OBJ_ENHMETADC, "OBJ_ENHMETADC 是 12");
    checkInt(0, BS_SOLID, "BS_SOLID 是 0");
    checkInt(1, BS_NULL, "BS_NULL 是 1");
    checkInt(16, static_cast<long long>(sizeof(LOGPEN)), "LOGPEN 是 16 字节");
    checkInt(16, static_cast<long long>(sizeof(LOGBRUSH)), "LOGBRUSH 是 16 字节");
    checkInt(32, static_cast<long long>(sizeof(BITMAP)), "BITMAP 是 32 字节（LP64）");
    checkInt(104, static_cast<long long>(sizeof(DIBSECTION)), "DIBSECTION 是 104 字节（LP64）");
}

// ---------------------------------------------------------------------------
// 2. 新建 DC 的初值：快路径比对的就是这一组
// ---------------------------------------------------------------------------
void testFreshDcDefaults() {
    Surface surface;
    check(surface.create(), "DIB 表面创建成功");
    if (surface.dc == nullptr) return;

    checkInt(13, GetROP2(surface.dc), "默认 ROP2 是 R2_COPYPEN(13)");
    checkInt(1, GetMapMode(surface.dc), "默认映射模式是 MM_TEXT(1)");
    checkInt(1, GetGraphicsMode(surface.dc), "默认图形模式是 GM_COMPATIBLE(1)");
    checkInt(1, GetArcDirection(surface.dc), "默认弧方向是 AD_COUNTERCLOCKWISE(1)");
    checkInt(0, static_cast<long long>(GetLayout(surface.dc)), "默认布局是 LAYOUT_LTR(0)");
    checkInt(0x00FFFFFF, GetDCBrushColor(surface.dc), "DC_BRUSH 初值是白色");
    checkInt(0x00000000, GetDCPenColor(surface.dc), "DC_PEN 初值是黑色");

    POINT origin{-9, -9};
    check(GetViewportOrgEx(surface.dc, &origin) == TRUE, "GetViewportOrgEx 成功");
    checkInt(0, origin.x, "默认视口原点 x=0");
    checkInt(0, origin.y, "默认视口原点 y=0");
    origin = {-9, -9};
    check(GetWindowOrgEx(surface.dc, &origin) == TRUE, "GetWindowOrgEx 成功");
    checkInt(0, origin.x, "默认窗口原点 x=0");
    checkInt(0, origin.y, "默认窗口原点 y=0");

    SIZE extent{-9, -9};
    check(GetViewportExtEx(surface.dc, &extent) == TRUE, "GetViewportExtEx 成功");
    checkInt(1, extent.cx, "默认视口范围 cx=1");
    checkInt(1, extent.cy, "默认视口范围 cy=1");
    extent = {-9, -9};
    check(GetWindowExtEx(surface.dc, &extent) == TRUE, "GetWindowExtEx 成功");
    checkInt(1, extent.cx, "默认窗口范围 cx=1");
    checkInt(1, extent.cy, "默认窗口范围 cy=1");

    POINT position{-9, -9};
    check(GetCurrentPositionEx(surface.dc, &position) == TRUE, "GetCurrentPositionEx 成功");
    checkInt(0, position.x, "默认当前位置 x=0");
    checkInt(0, position.y, "默认当前位置 y=0");

    // 失败路径：空 DC 一律返回各自的错误哨兵，不能返回"看起来正常"的 0 值域内数字。
    checkInt(0, GetROP2(nullptr), "空 DC 的 GetROP2 返回 0");
    checkInt(0, GetMapMode(nullptr), "空 DC 的 GetMapMode 返回 0");
    checkInt(0xFFFFFFFFLL, GetDCBrushColor(nullptr), "空 DC 的 GetDCBrushColor 返回 CLR_INVALID");
    checkInt(0xFFFFFFFFLL, static_cast<long long>(GetLayout(nullptr)), "空 DC 的 GetLayout 返回 GDI_ERROR");
}

// ---------------------------------------------------------------------------
// 3. GetObjectW：返回值必须等于 sizeof，字段必须对得上
// ---------------------------------------------------------------------------
void testGetObjectBitmap() {
    Surface surface;
    check(surface.create(), "DIB 表面创建成功（GetObject 用例）");
    if (surface.dc == nullptr) return;

    const HGDIOBJ current = GetCurrentObject(surface.dc, OBJ_BITMAP);
    check(current == reinterpret_cast<HGDIOBJ>(surface.bitmap), "GetCurrentObject(OBJ_BITMAP) 取回选入的位图");
    checkInt(7, static_cast<long long>(GetObjectType(current)), "GetObjectType 对位图返回 OBJ_BITMAP(7)");

    DIBSECTION section{};
    const int bytes = GetObjectW(current, sizeof(section), &section);
    checkInt(104, bytes, "GetObjectW(dib, sizeof(DIBSECTION)) 返回 sizeof(DIBSECTION)");
    check(section.dsBm.bmBits == surface.bits, "dsBm.bmBits 指向光栅内存");
    checkInt(Surface::width, section.dsBm.bmWidth, "dsBm.bmWidth 与创建宽度一致");
    // 已知与文档的偏差：top-down DIB 的 bmHeight 归一成正数。项目里三处断言
    // 按这个语义写死，且在 Windows 上是绿的（见 S3-诊断报告 第 2 节）。
    checkInt(Surface::height, section.dsBm.bmHeight, "top-down DIB 的 dsBm.bmHeight 归一为正数");
    checkInt(Surface::width * 4, section.dsBm.bmWidthBytes, "dsBm.bmWidthBytes 是 width*4");
    checkInt(1, section.dsBm.bmPlanes, "dsBm.bmPlanes 是 1");
    checkInt(32, section.dsBm.bmBitsPixel, "dsBm.bmBitsPixel 是 32");
    // 反方向的规则：dsBmih 逐字节保留创建时入参，负的 biHeight 原样带出。
    checkInt(static_cast<long long>(sizeof(BITMAPINFOHEADER)), section.dsBmih.biSize, "dsBmih.biSize 是 40");
    checkInt(Surface::width, section.dsBmih.biWidth, "dsBmih.biWidth 保留创建宽度");
    checkInt(-Surface::height, section.dsBmih.biHeight, "dsBmih.biHeight 保留创建时的负值（不归正）");
    checkInt(1, section.dsBmih.biPlanes, "dsBmih.biPlanes 是 1");
    checkInt(32, section.dsBmih.biBitCount, "dsBmih.biBitCount 是 32");
    checkInt(static_cast<long long>(BI_RGB), section.dsBmih.biCompression, "dsBmih.biCompression 是 BI_RGB");
    checkInt(0, static_cast<long long>(section.dsBmih.biClrUsed), "dsBmih.biClrUsed 是 0");
    check(section.dshSection == nullptr, "dshSection 是空（本层不支持文件映射段）");
    checkInt(0, static_cast<long long>(section.dsOffset), "dsOffset 是 0");

    // 只要 sizeof(BITMAP) 时，Win32 只填 BITMAP 部分并返回 sizeof(BITMAP)。
    BITMAP bitmap{};
    memset(&bitmap, 0xAA, sizeof(bitmap));
    const int bitmapBytes = GetObjectW(current, sizeof(bitmap), &bitmap);
    checkInt(32, bitmapBytes, "GetObjectW(dib, sizeof(BITMAP)) 返回 sizeof(BITMAP)");
    check(bitmap.bmBits == surface.bits, "sizeof(BITMAP) 路径下 bmBits 仍是光栅内存");

    // 缓冲区不够 → 0。
    BITMAP tooSmall{};
    checkInt(0, GetObjectW(current, 4, &tooSmall), "缓冲区不足时 GetObjectW 返回 0");
    checkInt(0, GetObjectW(nullptr, sizeof(section), &section), "空对象返回 0");

    // 设备相关位图（DDB）：有 BITMAP、没有像素指针，也不能被当成 DIBSection。
    HBITMAP device = CreateCompatibleBitmap(surface.dc, 8, 6);
    check(device != nullptr, "CreateCompatibleBitmap 成功");
    if (device != nullptr) {
        BITMAP ddb{};
        const int ddbBytes = GetObjectW(reinterpret_cast<HGDIOBJ>(device), sizeof(ddb), &ddb);
        checkInt(32, ddbBytes, "DDB 的 GetObjectW(sizeof(BITMAP)) 返回 sizeof(BITMAP)");
        check(ddb.bmBits == nullptr, "DDB 的 bmBits 必须是 nullptr（设备位图没有可寻址像素）");
        checkInt(8, ddb.bmWidth, "DDB 宽度正确");
        checkInt(6, ddb.bmHeight, "DDB 高度正确");
        // 请求 DIBSECTION 时只能拿回 BITMAP 的字节数，于是调用点的
        // `!= sizeof(DIBSECTION)` 判据会正确地把它拒掉。
        DIBSECTION notASection{};
        const int rejected = GetObjectW(reinterpret_cast<HGDIOBJ>(device), sizeof(notASection), &notASection);
        checkInt(32, rejected, "DDB 请求 DIBSECTION 时只返回 sizeof(BITMAP)（因此被判为非 DIB）");
        check(rejected != static_cast<int>(sizeof(DIBSECTION)), "DDB 不会被误判成 DIBSection");
        DeleteObject(reinterpret_cast<HGDIOBJ>(device));
    }
}

void testGetObjectPenAndBrush() {
    // 笔：RuntimeSelectionFrameRaster 靠 LOGPEN 判"笔是 2px PS_SOLID 且颜色正确"。
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(113, 181, 255));
    check(pen != nullptr, "CreatePen 成功");
    if (pen != nullptr) {
        checkInt(1, static_cast<long long>(GetObjectType(reinterpret_cast<HGDIOBJ>(pen))), "笔的 GetObjectType 是 OBJ_PEN");
        LOGPEN logpen{};
        checkInt(16, GetObjectW(reinterpret_cast<HGDIOBJ>(pen), sizeof(logpen), &logpen), "LOGPEN 返回 sizeof(LOGPEN)");
        checkInt(0, static_cast<long long>(logpen.lopnStyle), "LOGPEN.lopnStyle 是 PS_SOLID(0)");
        checkInt(2, logpen.lopnWidth.x, "LOGPEN.lopnWidth.x 是 2");
        checkInt(0, logpen.lopnWidth.y, "LOGPEN.lopnWidth.y 是 0（化妆笔）");
        checkInt(static_cast<long long>(RGB(113, 181, 255)), static_cast<long long>(logpen.lopnColor),
                 "LOGPEN.lopnColor 与创建颜色一致");
        DeleteObject(reinterpret_cast<HGDIOBJ>(pen));
    }

    // 画刷：SO 与 NULL 两态必须能从 LOGBRUSH 上区分——快路径的判据之一是
    // "当前画刷 == NULL_BRUSH"，画刷的"会不会落笔"全靠 lbStyle。
    HBRUSH brush = CreateSolidBrush(RGB(10, 20, 30));
    check(brush != nullptr, "CreateSolidBrush 成功");
    if (brush != nullptr) {
        checkInt(2, static_cast<long long>(GetObjectType(reinterpret_cast<HGDIOBJ>(brush))), "画刷的 GetObjectType 是 OBJ_BRUSH");
        LOGBRUSH logbrush{};
        checkInt(16, GetObjectW(reinterpret_cast<HGDIOBJ>(brush), sizeof(logbrush), &logbrush), "LOGBRUSH 返回 sizeof(LOGBRUSH)");
        checkInt(0, static_cast<long long>(logbrush.lbStyle), "实心画刷 lbStyle 是 BS_SOLID(0)");
        checkInt(static_cast<long long>(RGB(10, 20, 30)), static_cast<long long>(logbrush.lbColor), "实心画刷颜色正确");
        DeleteObject(reinterpret_cast<HGDIOBJ>(brush));
    }
    LOGBRUSH hollow{};
    const HGDIOBJ nullBrush = GetStockObject(NULL_BRUSH);
    check(nullBrush != nullptr, "NULL_BRUSH 库存对象存在");
    checkInt(16, GetObjectW(nullBrush, sizeof(hollow), &hollow), "NULL_BRUSH 的 LOGBRUSH 返回 sizeof(LOGBRUSH)");
    checkInt(1, static_cast<long long>(hollow.lbStyle), "NULL_BRUSH 的 lbStyle 是 BS_NULL(1)");
    checkInt(2, static_cast<long long>(GetObjectType(nullBrush)), "NULL_BRUSH 的 GetObjectType 是 OBJ_BRUSH");

    // 库存字体：类型对，P5-06 起 GetObject 如实回填 LOGFONTW（库存字体无参数，
    // lfHeight 为 0；CreateFontW 出来的才有真实字高，见 CompatTextSemantics.cpp）。
    const HGDIOBJ font = GetStockObject(DEFAULT_GUI_FONT);
    checkInt(6, static_cast<long long>(GetObjectType(font)), "DEFAULT_GUI_FONT 的 GetObjectType 是 OBJ_FONT(6)");
    LOGFONTW logfont{};
    checkInt(static_cast<long long>(sizeof(LOGFONTW)),
             GetObjectW(font, sizeof(logfont), &logfont),
             "库存字体 GetObject 回填 sizeof(LOGFONTW)（lfHeight 为 0，P5-06 起兑现）");

    // 非法句柄：不能崩、不能返回真值。用一块零填充的小对象模拟"任意非 GDI 指针"，
    // 它的首 4 字节是 0，与 kObjectMagic 不符，因此必须被拒。
    struct NotAnObject {
        std::uint32_t first = 0;
        std::uint32_t second = 0;
    } notAnObject;
    checkInt(0, static_cast<long long>(GetObjectType(reinterpret_cast<HGDIOBJ>(&notAnObject))),
             "非 GDI 对象的 GetObjectType 返回 0");
    check(GetObjectW(reinterpret_cast<HGDIOBJ>(&notAnObject), sizeof(BITMAP), nullptr) == 0,
          "空缓冲区直接被拒");
    checkInt(0, static_cast<long long>(GetObjectType(nullptr)), "空句柄的 GetObjectType 返回 0");
    check(GetCurrentObject(nullptr, OBJ_BITMAP) == nullptr, "空 DC 的 GetCurrentObject 返回空");
}

// ---------------------------------------------------------------------------
// 4. GetClipBox：没有裁剪区时是"整块位图"，不是 NULLREGION
// ---------------------------------------------------------------------------
void testClipBox() {
    Surface surface;
    check(surface.create(), "DIB 表面创建成功（裁剪用例）");
    if (surface.dc == nullptr) return;

    RECT clip{-9, -9, -9, -9};
    const int complexity = GetClipBox(surface.dc, &clip);
    // 这里必须是 SIMPLEREGION 而不是 NULLREGION：雷达快路径的判据是
    // `GetClipBox(dc,&clip) != SIMPLEREGION` 就拒绝，返回 NULLREGION 会让它
    // 永远走慢路；而 RuntimeSelectedPathRaster 两种都接受。
    checkInt(2, complexity, "新建 DC 的 GetClipBox 返回 SIMPLEREGION(2)");
    checkInt(0, clip.left, "裁剪框 left=0");
    checkInt(0, clip.top, "裁剪框 top=0");
    checkInt(Surface::width, clip.right, "裁剪框 right=位图宽");
    checkInt(Surface::height, clip.bottom, "裁剪框 bottom=位图高");

    // 没有位图的内存 DC：没有像素表面，只能报错——编一个矩形更有害。
    HDC bare = CreateCompatibleDC(nullptr);
    check(bare != nullptr, "空内存 DC 创建成功");
    if (bare != nullptr) {
        RECT none{};
        checkInt(0, GetClipBox(bare, &none), "未选位图的 DC 的 GetClipBox 返回 ERROR(0)");
        DeleteDC(bare);
    }

    // 录制型 DC：同样没有像素表面（帧矩形没被记录），必须报错。
    HDC metafile = CreateEnhMetaFileW(nullptr, nullptr, nullptr, nullptr);
    check(metafile != nullptr, "录制型 DC 创建成功");
    if (metafile != nullptr) {
        RECT none{};
        checkInt(0, GetClipBox(metafile, &none), "录制型 DC 的 GetClipBox 返回 ERROR(0)");
        HENHMETAFILE handle = CloseEnhMetaFile(metafile);
        if (handle != nullptr) DeleteEnhMetaFile(handle);
    }
}

// ---------------------------------------------------------------------------
// 5. Setter 往返：返回旧值、非法参数不改状态
// ---------------------------------------------------------------------------
void testSetters() {
    Surface surface;
    check(surface.create(), "DIB 表面创建成功（Setter 用例）");
    if (surface.dc == nullptr) return;

    checkInt(13, SetROP2(surface.dc, R2_XORPEN), "SetROP2 返回旧值 R2_COPYPEN(13)");
    checkInt(7, GetROP2(surface.dc), "SetROP2 之后 GetROP2 是 R2_XORPEN(7)");
    checkInt(7, SetROP2(surface.dc, R2_COPYPEN), "SetROP2 还原时返回 R2_XORPEN(7)");
    checkInt(0, SetROP2(surface.dc, 0), "非法 ROP2（0）被拒");
    checkInt(0, SetROP2(surface.dc, 17), "非法 ROP2（17）被拒");
    checkInt(13, GetROP2(surface.dc), "非法 ROP2 未改变状态");

    checkInt(1, SetMapMode(surface.dc, MM_ANISOTROPIC), "SetMapMode 返回旧值 MM_TEXT(1)");
    checkInt(8, GetMapMode(surface.dc), "SetMapMode(MM_ANISOTROPIC) 之后读回 8");
    checkInt(8, SetMapMode(surface.dc, MM_TEXT), "SetMapMode 还原返回 8");
    checkInt(0, SetMapMode(surface.dc, 9), "非法映射模式（9）被拒");

    checkInt(1, SetGraphicsMode(surface.dc, GM_ADVANCED), "SetGraphicsMode 返回旧值 GM_COMPATIBLE(1)");
    checkInt(2, GetGraphicsMode(surface.dc), "SetGraphicsMode(GM_ADVANCED) 之后读回 2");
    checkInt(2, SetGraphicsMode(surface.dc, GM_COMPATIBLE), "SetGraphicsMode 还原返回 2");
    checkInt(0, SetGraphicsMode(surface.dc, 3), "非法图形模式（3）被拒");

    checkInt(0, static_cast<long long>(SetLayout(surface.dc, LAYOUT_RTL)), "SetLayout 返回旧值 LAYOUT_LTR(0)");
    checkInt(1, static_cast<long long>(GetLayout(surface.dc)), "SetLayout(LAYOUT_RTL) 之后读回 1");
    checkInt(1, static_cast<long long>(SetLayout(surface.dc, LAYOUT_LTR)), "SetLayout 还原返回 1");
    // LAYOUT_LTR 是 0，所以"同时选 LTR 与 RTL"在数值上不可表达（等于只选 RTL），
    // 真正可拒的是未知标志位。
    checkInt(0xFFFFFFFFLL, static_cast<long long>(SetLayout(surface.dc, 0x10)), "未知布局位 → GDI_ERROR");
    checkInt(0, static_cast<long long>(GetLayout(surface.dc)), "非法布局未改变状态");

    POINT before{-9, -9};
    check(SetViewportOrgEx(surface.dc, 7, -3, &before) == TRUE, "SetViewportOrgEx 成功");
    checkInt(0, before.x, "SetViewportOrgEx 的 out 参数是旧原点 x=0");
    checkInt(0, before.y, "SetViewportOrgEx 的 out 参数是旧原点 y=0");
    POINT now{-9, -9};
    GetViewportOrgEx(surface.dc, &now);
    checkInt(7, now.x, "视口原点写入后读回 x=7");
    checkInt(-3, now.y, "视口原点写入后读回 y=-3");
    check(SetViewportOrgEx(surface.dc, 0, 0, nullptr) == TRUE, "out 参数为空时 SetViewportOrgEx 仍成功");

    POINT windowOld{-9, -9};
    check(SetWindowOrgEx(surface.dc, 1, 2, &windowOld) == TRUE, "SetWindowOrgEx 成功");
    checkInt(0, windowOld.x, "SetWindowOrgEx 的 out 参数是旧原点");
    POINT windowNow{-9, -9};
    GetWindowOrgEx(surface.dc, &windowNow);
    checkInt(1, windowNow.x, "窗口原点写入后读回 x=1");
    checkInt(2, windowNow.y, "窗口原点写入后读回 y=2");
    SetWindowOrgEx(surface.dc, 0, 0, nullptr);

    SIZE extOld{-9, -9};
    check(SetViewportExtEx(surface.dc, 32, 20, &extOld) == TRUE, "SetViewportExtEx 成功");
    checkInt(1, extOld.cx, "SetViewportExtEx 的 out 参数是旧范围");
    SIZE extNow{-9, -9};
    GetViewportExtEx(surface.dc, &extNow);
    checkInt(32, extNow.cx, "视口范围写入后读回 cx=32");
    checkInt(20, extNow.cy, "视口范围写入后读回 cy=20");
    check(SetViewportExtEx(surface.dc, 0, 20, nullptr) == FALSE, "零范围被拒");
    SetViewportExtEx(surface.dc, 1, 1, nullptr);

    SIZE windowExtOld{-9, -9};
    check(SetWindowExtEx(surface.dc, 64, 40, &windowExtOld) == TRUE, "SetWindowExtEx 成功");
    checkInt(1, windowExtOld.cx, "SetWindowExtEx 的 out 参数是旧范围");
    SIZE windowExtNow{-9, -9};
    GetWindowExtEx(surface.dc, &windowExtNow);
    checkInt(64, windowExtNow.cx, "窗口范围写入后读回 cx=64");
    checkInt(40, windowExtNow.cy, "窗口范围写入后读回 cy=40");
    SetWindowExtEx(surface.dc, 1, 1, nullptr);

    checkInt(0x00FFFFFF, SetDCBrushColor(surface.dc, RGB(15, 22, 31)), "SetDCBrushColor 返回旧值（白）");
    checkInt(static_cast<long long>(RGB(15, 22, 31)), GetDCBrushColor(surface.dc), "SetDCBrushColor 之后读回新颜色");
    checkInt(0x00000000, SetDCPenColor(surface.dc, RGB(96, 208, 255)), "SetDCPenColor 返回旧值（黑）");
    checkInt(static_cast<long long>(RGB(96, 208, 255)), GetDCPenColor(surface.dc), "SetDCPenColor 之后读回新颜色");
    checkInt(0xFFFFFFFFLL, SetDCBrushColor(nullptr, RGB(1, 2, 3)), "空 DC 的 SetDCBrushColor 返回 CLR_INVALID");
}

// ---------------------------------------------------------------------------
// 6. MulDiv：64 位中间量 + Win32 的四舍五入
// ---------------------------------------------------------------------------
void testMulDiv() {
    checkInt(100000, MulDiv(100000, 100000, 100000), "MulDiv 中间量走 64 位（32 位会溢出）");
    checkInt(10, MulDiv(10, 3, 3), "整除");
    checkInt(8, MulDiv(10, 3, 4), "30/4=7.5，半数离开零 → 8");
    checkInt(8, MulDiv(11, 3, 4), "33/4=8.25 → 8");
    checkInt(-1, MulDiv(-1, 2, 3), "-0.67 向零的反方向取整到 -1");
    checkInt(1, MulDiv(1, 2, 3), "0.67 → 1");
    checkInt(-8, MulDiv(-11, 3, 4), "-8.25 → -8");
    checkInt(-1, MulDiv(5, 1, 0), "除数为 0 返回 -1（Win32 语义）");
    checkInt(0, MulDiv(0, 12345, 7), "被乘数为 0 时结果 0");
}

// ---------------------------------------------------------------------------
// 7. GdiFlush / GdiSetBatchLimit：返回值必须能往返
// ---------------------------------------------------------------------------
void testBatchLimit() {
    check(GdiFlush() == TRUE, "GdiFlush 返回 TRUE");
    const DWORD initial = GdiSetBatchLimit(1);
    checkInt(20, static_cast<long long>(initial), "Win32 默认批处理上限是 20");
    checkInt(1, static_cast<long long>(GdiSetBatchLimit(initial)), "GdiSetBatchLimit 返回上一次设置的值");
    // 嵌套：内层先返回 20 并把上限设成 1，外层随即把上限设回 20 并返回 1。
    checkInt(1, static_cast<long long>(GdiSetBatchLimit(GdiSetBatchLimit(1))), "嵌套往返自洽");
    GdiSetBatchLimit(20);
}

// ---------------------------------------------------------------------------
// 8. SaveDC/RestoreDC 必须覆盖 S3-1 新增的每个字段
// ---------------------------------------------------------------------------
void testSaveRestoreCoversNewState() {
    Surface surface;
    check(surface.create(), "DIB 表面创建成功（Save/Restore 用例）");
    if (surface.dc == nullptr) return;

    const int saved = SaveDC(surface.dc);
    check(saved != 0, "SaveDC 成功");
    SetROP2(surface.dc, R2_XORPEN);
    SetMapMode(surface.dc, MM_ANISOTROPIC);
    SetGraphicsMode(surface.dc, GM_ADVANCED);
    SetLayout(surface.dc, LAYOUT_RTL);
    SetViewportOrgEx(surface.dc, 5, 6, nullptr);
    SetWindowOrgEx(surface.dc, 7, 8, nullptr);
    SetViewportExtEx(surface.dc, 2, 2, nullptr);
    SetWindowExtEx(surface.dc, 3, 3, nullptr);
    SetDCBrushColor(surface.dc, RGB(1, 1, 1));
    SetDCPenColor(surface.dc, RGB(2, 2, 2));

    check(RestoreDC(surface.dc, saved) == TRUE, "RestoreDC 成功");
    checkInt(13, GetROP2(surface.dc), "RestoreDC 还原 ROP2");
    checkInt(1, GetMapMode(surface.dc), "RestoreDC 还原映射模式");
    checkInt(1, GetGraphicsMode(surface.dc), "RestoreDC 还原图形模式");
    checkInt(0, static_cast<long long>(GetLayout(surface.dc)), "RestoreDC 还原布局");
    POINT origin{-9, -9};
    GetViewportOrgEx(surface.dc, &origin);
    checkInt(0, origin.x, "RestoreDC 还原视口原点 x");
    checkInt(0, origin.y, "RestoreDC 还原视口原点 y");
    GetWindowOrgEx(surface.dc, &origin);
    checkInt(0, origin.x, "RestoreDC 还原窗口原点 x");
    checkInt(0, origin.y, "RestoreDC 还原窗口原点 y");
    SIZE extent{-9, -9};
    GetViewportExtEx(surface.dc, &extent);
    checkInt(1, extent.cx, "RestoreDC 还原视口范围 cx");
    GetWindowExtEx(surface.dc, &extent);
    checkInt(1, extent.cx, "RestoreDC 还原窗口范围 cx");
    checkInt(0x00FFFFFF, GetDCBrushColor(surface.dc), "RestoreDC 还原 DC_BRUSH 颜色");
    checkInt(0x00000000, GetDCPenColor(surface.dc), "RestoreDC 还原 DC_PEN 颜色");
}

// ---------------------------------------------------------------------------
// 9. 快路径前提条件：把 RuntimeSelectedPathRaster::supported() 的判据逐项照抄
// ---------------------------------------------------------------------------
// 这是 S3-1 的验收条款。真正的光栅器要到 S3-3 才有（LineTo/FillRect），在
// 那之前 RuntimeSelectedPathRasterContract 仍然编译不过；但"状态判据能不能
// 全过"完全可以现在验——它才是 available()==false 的真实原因。
void testFastPathPreconditions() {
    Surface surface;
    check(surface.create(), "DIB 表面创建成功（快路径前提）");
    if (surface.dc == nullptr) return;

    SetBkMode(surface.dc, TRANSPARENT);
    const HGDIOBJ oldBrush = SelectObject(surface.dc, GetStockObject(NULL_BRUSH));

    // 逐条对应 src/ui/selection/RuntimeSelectedPathRaster.h 的 supported()。
    check(GetBkMode(surface.dc) == TRANSPARENT, "supported(): GetBkMode == TRANSPARENT");
    check(GetROP2(surface.dc) == 13, "supported(): GetROP2 == R2_COPYPEN");
    check(GetGraphicsMode(surface.dc) == 1, "supported(): GetGraphicsMode == GM_COMPATIBLE");
    check(GetMapMode(surface.dc) == 1, "supported(): GetMapMode == MM_TEXT");
    check(GetArcDirection(surface.dc) == 1, "supported(): GetArcDirection == AD_COUNTERCLOCKWISE");
    check(GetLayout(surface.dc) == 0, "supported(): GetLayout == 0");
    check(GetCurrentObject(surface.dc, OBJ_BRUSH) == GetStockObject(NULL_BRUSH),
          "supported(): 当前画刷是 NULL_BRUSH");
    POINT window{-9, -9}, viewport{-9, -9};
    check(GetWindowOrgEx(surface.dc, &window) && window.x == 0 && window.y == 0,
          "supported(): 窗口原点为 (0,0)");
    check(GetViewportOrgEx(surface.dc, &viewport) && viewport.x == 0 && viewport.y == 0,
          "supported(): 视口原点为 (0,0)");
    DIBSECTION dib{};
    const HGDIOBJ bitmap = GetCurrentObject(surface.dc, OBJ_BITMAP);
    check(GetObjectW(bitmap, sizeof(dib), &dib) == static_cast<int>(sizeof(dib)),
          "supported(): GetObjectW(DIBSECTION) 长度正确");
    check(dib.dsBm.bmBits == surface.bits, "supported(): dsBm.bmBits 与调用方像素指针一致");
    check(dib.dsBm.bmBitsPixel == 32, "supported(): dsBm.bmBitsPixel == 32");
    check(dib.dsBm.bmPlanes == 1, "supported(): dsBm.bmPlanes == 1");
    check(dib.dsBm.bmWidth == Surface::width, "supported(): dsBm.bmWidth 与实参宽度一致");
    check(dib.dsBm.bmHeight == Surface::height, "supported(): dsBm.bmHeight 为正值且等于实参高度");
    check(static_cast<std::int64_t>(dib.dsBm.bmWidthBytes) == static_cast<std::int64_t>(Surface::width) * 4,
          "supported(): dsBm.bmWidthBytes == width*4");
    RECT clip{};
    const int complexity = GetClipBox(surface.dc, &clip);
    check(complexity == SIMPLEREGION || complexity == NULLREGION, "supported(): GetClipBox 是简单区域或空区域");

    // 破坏任一项都必须让判据失败——否则"全过"这个结论没有分辨力。
    SelectObject(surface.dc, oldBrush);
    check(GetCurrentObject(surface.dc, OBJ_BRUSH) != GetStockObject(NULL_BRUSH),
          "supported(): 换成实心画刷后判据应失配");
    SelectObject(surface.dc, GetStockObject(NULL_BRUSH));
    SetLayout(surface.dc, LAYOUT_RTL);
    check(GetLayout(surface.dc) != 0, "supported(): 设成 RTL 后判据应失配");
    SetLayout(surface.dc, LAYOUT_LTR);
    SetMapMode(surface.dc, MM_ANISOTROPIC);
    check(GetMapMode(surface.dc) != 1, "supported(): 设成 MM_ANISOTROPIC 后判据应失配");
    SetMapMode(surface.dc, MM_TEXT);
    check(GetLayout(surface.dc) == 0 && GetMapMode(surface.dc) == 1, "supported(): 还原后判据重新成立");
}

}  // namespace

int main() {
    std::printf("compat/win32 DC 状态与查询语义自测（S3-1）\n\n");
    testConstantLiterals();
    testFreshDcDefaults();
    testGetObjectBitmap();
    testGetObjectPenAndBrush();
    testClipBox();
    testSetters();
    testMulDiv();
    testBatchLimit();
    testSaveRestoreCoversNewState();
    testFastPathPreconditions();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
