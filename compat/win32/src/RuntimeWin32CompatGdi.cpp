// Win32 GDI 兼容层：对象管理 + 位图传输两族（S2）。
//
// 定位
// ----
// 这是 compat/win32 的第一批**实现**。S1 只给声明，未实现的符号在链接期报
// undefined symbol；本文件补上产品构建图里真正会跑到的两组能力：
//
//   对象管理：CreateCompatibleDC / CreateCompatibleBitmap / CreateDIBSection /
//             SelectObject / DeleteObject / DeleteDC / GetStockObject /
//             CreateSolidBrush / CreatePen / SaveDC / RestoreDC
//   位图传输：BitBlt / StretchBlt / PatBlt / StretchDIBits / SetDIBitsToDevice /
//             GetPixel / SetPixelV / SetPixel / SetStretchBltMode
//   DC 状态：SetBkMode / SetBkColor / SetTextColor 及其 Get 对应（文本族 S4 复用）
//
// 设计约束来自实测，不是猜的（见 P2-08a-诊断报告.md 与 S1-兼容层头层说明.md）：
//   * 产品构建图里 11 处 CreateDIBSection、5 处 StretchDIBits、1 处
//     SetDIBitsToDevice，**全部**是 biBitCount=32 / biCompression=BI_RGB /
//     biHeight 为负（top-down）。因此本层只支持 32bpp DIB，其余位深与压缩方式
//     一律显式失败并在 stderr 留下说明，绝不静默画错。
//   * BitBlt/StretchBlt/PatBlt 的 ROP 只出现 SRCCOPY，另有 1 处 DSTINVERT。
//     其余 ROP 返回失败并报错——这是刻意的：静默降级成 SRCCOPY 会让画面
//     "看起来对、细节全错"，比直接失败难查得多。
//
// 有意偏离 Win32 的两处（都写在对应函数里）：
//   1. DeleteDC 允许在仍有非库存位图被选入时删除，并把该位图摘下来。真实
//      Windows 会返回 FALSE。这里选宽容，避免调用方漏写 SelectObject 还原
//      时整块帧缓冲泄漏。
//   2. 内存 DC 初始没有位图（真实 Windows 给 1x1 单色位图）。未选位图时所有
//      绘制都失败返回，不会崩溃也不会画到别处。
//
// 线程模型：与 Win32 一致——单个 HDC / GDI 对象不是线程安全的，调用方负责
// 串行化。本项目渲染是单线程，故不加锁（加锁反而会引入 Windows 没有的开销）。

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "RuntimeWin32CompatTextInternal.h"

// 字形后端在自己的命名空间里（它不知道 GDI，也不知道 windows.h）。
namespace text = concord::win32compat::text;

namespace {

constexpr std::uint32_t kObjectMagic = 0x434F424Au;  // 'COBJ'
constexpr std::uint32_t kDeviceMagic = 0x434F4443u;  // 'CODC'
constexpr int kStockSlots = 32;

// 存活对象计数 —— GetGuiResources(..., GR_GDIOBJECTS) 的语义来源。
//
// 只统计本层分配的**动态**对象：stock 对象是静态表里的固定项，Win32 也不把
// 它们算进进程 GDI 计数。类型用普通 int 而非 atomic，与本层"不是线程安全、
// 调用方负责串行化"的线程模型一致（见文件头）。
//
// 这个计数存在的意义：7 个 diagnostics 文件共 31 处 GetGuiResources 调用都在做
// before/after 相等断言。以前它们只能拿到桩值（恒 0 恒等），现在是真的泄漏检测。
int liveObjectCount = 0;

enum class ObjectKind : std::uint32_t {
    Bitmap,
    Pen,
    Brush,
    Font,
    Region
};

struct IntRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

inline bool isEmpty(const IntRect& rect) {
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

inline IntRect intersect(const IntRect& a, const IntRect& b) {
    IntRect result;
    result.left = a.left > b.left ? a.left : b.left;
    result.top = a.top > b.top ? a.top : b.top;
    result.right = a.right < b.right ? a.right : b.right;
    result.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    return result;
}

// ---------------------------------------------------------------------------
// 区域表示（S3-2，B 段）
// ---------------------------------------------------------------------------
//
// 区域用**横向条带表**表示：每个 band 是"若干连续行 × 同一个列区间"。
//
// 为什么不用多边形/边表表示：RGN_DIFF 之后还要做四件事——精确判点、
// 精确求外接矩形、精确区分 SIMPLE/COMPLEX、以及给裁剪光栅器逐行取区间。
// 条带表让这四件事全部变成"逐行整数区间运算"，且结果与运算顺序无关
// （确定性，S3 的硬约束）。多边形只在 CreatePolygonRgn 里出现一次，
// 进来就光栅化成条带，此后不再有曲线/顶点概念。
//
// 代价（必须记住）：丢掉了 Win32 区域"边界含点"的亚像素细节。项目里
// 所有区域都是整数坐标的矩形或多边形，裁剪要的也是像素格，故一致；
// 但若将来有人用小数坐标或拿区域做几何测量，结论会和 Windows 差半个像素。
struct RegionSpan {
    int left = 0;
    int right = 0;
};

struct RegionBand {
    int top = 0;
    int bottom = 1;  // 半开：覆盖行 [top, bottom)
    RegionSpan span;
};

using RegionBands = std::vector<RegionBand>;

// 平面展开的行跨度表：索引 0 对应最上面一行。
using RowSpans = std::vector<RegionSpan>;

// 单次布尔运算允许展开的行数上限。超过就**失败**而不是截断——
// 截断会得到一个"看起来对、少了一块"的区域，比直接报错难查得多。
constexpr int kMaxRegionRows = 16384;

// 一个结构体承载四类 GDI 对象：位图、笔、画刷、字体。字段按 kind 复用。
struct CompatObject {
    std::uint32_t magic = kObjectMagic;
    ObjectKind kind = ObjectKind::Bitmap;
    bool stock = false;
    int selectedCount = 0;

    // 位图（32bpp，内存字节序 B,G,R,A；行内无填充）
    int width = 0;
    int height = 0;
    int stride = 0;
    bool topDown = true;
    std::uint8_t* pixels = nullptr;

    // 位图的两副面孔。CreateDIBSection 出来的叫"DIBSection"，GetObject 能给
    // 出完整 DIBSECTION（含像素指针）；CreateCompatibleBitmap 出来的叫
    // "设备相关位图 (DDB)"，Win32 里它**没有**可寻址的像素数组，GetObject
    // 只能给出 BITMAP 且 bmBits 必须为 nullptr。产品侧
    // CampaignDeviceBitmapContract 正是靠 bmBits==nullptr 来断言"这是设备位图"。
    // 本层两种都用同一块内存实现（否则没法光栅化），差异只体现在 GetObject
    // 的回答上——这是唯一一处"实现比语义更强"，必须记在这里，
    // 否则将来有人照着 GetObject 的结果去 memcpy 就会读到不该暴露的地址。
    bool dibSection = false;
    BITMAPINFOHEADER header{};  // 仅 DIBSection 有效，逐字节保留创建时的入参

    // 笔
    int penStyle = PS_SOLID;
    int penWidth = 1;

    // 画刷 / 通用颜色
    bool hollow = false;
    COLORREF color = 0;

    // 区域（S3-2）。空表有两种含义，靠这个 bool 区分：
    //   liveRegion=false → 不是区域对象（其它 kind 用同一字段无成本）
    //   liveRegion=true 且 bands 为空 → NULLREGION（空区域，什么都裁掉）
    // 与 DC 侧的 hasClip 分开是刻意的：region 对象永远"存在"，只是可能为空。
    bool liveRegion = false;
    RegionBands bands;

    // 字体（P5-06）。LOGFONTW 逐字节保留 CreateFontW 的入参，GetObject(font, ...)
    // 直接回填。只对 kind==Font 有意义，其它 kind 复用这块内存零成本。字高取
    // lfHeight 绝对值（负值 = "字符高度"、正值 = "单元格高度"，见 fontMetricsFor）。
    LOGFONTW fontLog{};
};

// SaveDC/RestoreDC 的快照。漏一项就等于"这条状态不受 Save 保护"，而
// RuntimeRadarPixelStoreContract 一类用例正是"SaveDC → 污染状态 → 观察快路径
// 是否退让 → RestoreDC"，漏掉的字段会让污染在恢复后残留，表现为后续用例
// 莫名其妙地走慢路径。所以 DC 里每个可变字段都必须出现在这里。
struct CompatSavedState {
    HGDIOBJ pen = nullptr;
    HGDIOBJ brush = nullptr;
    HGDIOBJ font = nullptr;
    COLORREF textColor = 0;
    COLORREF bkColor = 0x00FFFFFFu;
    int bkMode = OPAQUE;
    int polyFillMode = ALTERNATE;
    UINT textAlign = TA_LEFT | TA_TOP;  // P5-06：文本对齐状态（初值即 TA_LEFT|TA_TOP == 0）
    POINT position{0, 0};
    int rop2 = R2_COPYPEN;
    int mapMode = MM_TEXT;
    int graphicsMode = GM_COMPATIBLE;
    int arcDirection = AD_COUNTERCLOCKWISE;
    DWORD layout = LAYOUT_LTR;
    POINT windowOrg{0, 0};
    POINT viewportOrg{0, 0};
    SIZE windowExt{1, 1};
    SIZE viewportExt{1, 1};
    COLORREF dcBrushColor = 0x00FFFFFFu;
    COLORREF dcPenColor = 0x00000000u;
    // 裁剪区快照必须**深拷贝**条带表：SaveDC 之后 DC 上还会继续做
    // IntersectClipRect/SelectClipRgn，若只存指针，RestoreDC 拿到的是被改过的
    // 当前状态——表现是"恢复后裁剪没回来"，而所有像素断言仍然自洽（两边都错）。
    bool hasClip = false;
    RegionBands clip;
};

// ---------------------------------------------------------------------------
// 录制型 DC（EMF）
// ---------------------------------------------------------------------------
// 录制型 DC 没有像素表面：bitmap 为空，recording 非空。它与普通 DC 的差别只有
// 一点——绘制指令不是落到像素上，而是被序列化成 EMF 记录。这不只是"另一种输出
// 目标"，也是唯一能审计"一次重绘到底提交了几次整帧拷贝"的手段：
// 截屏看不出中间黑帧，指令流看得出。

constexpr std::uint32_t kMetafileMagic = 0x454D4621u;  // 'EMF!'

// 录制中的累积体。头部与 EOF 在 CloseEnhMetaFile 时才补——那时才知道总字节数
// 与记录数。
struct EmfRecording {
    std::vector<std::uint8_t> body;
    std::uint32_t recordCount = 0;
    bool hasBounds = false;
    RECTL bounds{0, 0, 0, 0};
};

// 已定稿的图元文件。EnumEnhMetaFile 直接按 nSize 串下去回放。
struct EmfMetafile {
    std::uint32_t magic = kMetafileMagic;
    std::vector<std::uint8_t> bytes;
};

struct CompatDevice {
    std::uint32_t magic = kDeviceMagic;
    CompatObject* bitmap = nullptr;  // 借用，不拥有
    EmfRecording* recording = nullptr;  // 非空即为录制型 DC；拥有
    HGDIOBJ pen = nullptr;
    HGDIOBJ brush = nullptr;
    HGDIOBJ font = nullptr;
    COLORREF textColor = 0;
    COLORREF bkColor = 0x00FFFFFFu;
    int bkMode = OPAQUE;
    int stretchMode = COLORONCOLOR;
    int polyFillMode = ALTERNATE;  // 填充模式：ALTERNATE(1) / WINDING(2)
    UINT textAlign = TA_LEFT | TA_TOP;  // P5-06：文本对齐（Get/SetTextAlign 的状态）
    POINT position{0, 0};
    // 以下状态在 S2 时代不存在（当时没有 Get 查询，也没人读）。S3-1 加进来是
    // 因为三条快路径会逐项比对它们：少一项 Get、或者初值给错，快路径就永远
    // 关闭，而所有"快慢两路是否一致"的契约仍然全绿——静默降级，最难查。
    int rop2 = R2_COPYPEN;
    int mapMode = MM_TEXT;
    int graphicsMode = GM_COMPATIBLE;
    int arcDirection = AD_COUNTERCLOCKWISE;
    DWORD layout = LAYOUT_LTR;
    POINT windowOrg{0, 0};
    POINT viewportOrg{0, 0};
    SIZE windowExt{1, 1};
    SIZE viewportExt{1, 1};
    COLORREF dcBrushColor = 0x00FFFFFFu;
    COLORREF dcPenColor = 0x00000000u;
    // 裁剪区（S3-2）。hasClip=false 表示"没有 clip region"，等价于整块表面；
    // hasClip=true 且 clip 为空表示 NULLREGION（全裁掉）。两者必须分开，
    // 否则 IntersectClipRect(dc,0,0,0,0) 会被当成"没有裁剪"而放行全部像素——
    // RuntimeSelectionFrameRasterContract 正是用这一手把画面裁空的。
    bool hasClip = false;
    RegionBands clip;
    std::vector<CompatSavedState> saved;
};

// 把 [start, start + length) 裁到 [0, limit)，就地更新起点与长度。
inline bool clipSpan(int& start, int& length, int limit) {
    if (length <= 0) return false;
    if (start < 0) {
        length += start;
        start = 0;
    }
    if (start + length > limit) {
        length = limit - start;
    }
    return length > 0;
}

// ---------------------------------------------------------------------------
// 失败报告
// ---------------------------------------------------------------------------
// 全部走 stderr 且带统一前缀，便于在千行构建/运行日志里筛出来。刻意不用项目
// 自身的日志设施——这一层可能先于日志系统初始化被调用。

void reportUnsupported(const char* function, const char* detail) {
    std::fprintf(stderr, "[compat/win32] %s: %s\n", function, detail);
}

void reportUnsupportedLong(const char* function, const char* what, long value) {
    std::fprintf(stderr, "[compat/win32] %s: unsupported %s=%ld\n", function, what, value);
}

// ---------------------------------------------------------------------------
// 区域运算（S3-2）
// ---------------------------------------------------------------------------
//
// 全部是纯函数：输入条带表，输出条带表，没有隐藏状态。这样 selftest 可以
// 直接对着 GetRgnBox / PtInRegion 断言，而不必先建 DC。

IntRect regionBounds(const RegionBands& bands) {
    IntRect bounds{};
    if (bands.empty()) return bounds;
    bounds.left = bands[0].span.left;
    bounds.right = bands[0].span.right;
    bounds.top = bands[0].top;
    bounds.bottom = bands[0].bottom;
    for (const RegionBand& band : bands) {
        if (band.span.left < bounds.left) bounds.left = band.span.left;
        if (band.span.right > bounds.right) bounds.right = band.span.right;
        if (band.top < bounds.top) bounds.top = band.top;
        if (band.bottom > bounds.bottom) bounds.bottom = band.bottom;
    }
    return bounds;
}

// 条带表规范化后互不重叠，所以"多于一条"就必然是复杂区域——不需要再
// 检查能否拼成一个矩形。这条性质是整个 COMPLEXREGION 判定的唯一依据，
// compressRows 必须保证它（同一区间的相邻行一定会被合并成一条）。
int regionComplexity(const RegionBands& bands) {
    if (bands.empty()) return NULLREGION;
    if (bands.size() == 1) return SIMPLEREGION;
    return COMPLEXREGION;
}

bool regionContainsPoint(const RegionBands& bands, int x, int y) {
    for (const RegionBand& band : bands) {
        if (y < band.top || y >= band.bottom) continue;
        if (x >= band.span.left && x < band.span.right) return true;
    }
    return false;
}

void setRectBands(RegionBands& bands, int left, int top, int right, int bottom) {
    bands.clear();
    // 与 Win32 一致：右下角小于等于左上角得到**空区域**，不做自动换位。
    if (right <= left || bottom <= top) return;
    bands.push_back(RegionBand{top, bottom, RegionSpan{left, right}});
}

// 行 y 上的区间，已排序、互不重叠。条带本身互不重叠，但同一行可能被
// 左右两块条带覆盖，且条带表的写出顺序不等于 x 顺序，所以必须排序——
// 否则布尔运算的顺序会渗进结果，破坏"同一输入必然同一输出"的约束。
RowSpans spansAtRow(const RegionBands& bands, int y) {
    RowSpans spans;
    for (const RegionBand& band : bands) {
        if (y < band.top || y >= band.bottom) continue;
        spans.push_back(band.span);
    }
    for (std::size_t i = 1; i < spans.size(); ++i) {
        const RegionSpan key = spans[i];
        std::size_t j = i;
        while (j > 0 && spans[j - 1].left > key.left) {
            spans[j] = spans[j - 1];
            --j;
        }
        spans[j] = key;
    }
    RowSpans merged;
    for (const RegionSpan& span : spans) {
        if (!merged.empty() && span.left <= merged.back().right) {
            if (span.right > merged.back().right) merged.back().right = span.right;
            continue;
        }
        merged.push_back(span);
    }
    return merged;
}

RowSpans unionSpans(const RowSpans& a, const RowSpans& b) {
    RowSpans result;
    const auto push = [&result](const RegionSpan& span) {
        if (!result.empty() && span.left <= result.back().right) {
            if (span.right > result.back().right) result.back().right = span.right;
            return;
        }
        result.push_back(span);
    };
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].left <= b[j].left) {
            push(a[i]);
            ++i;
        } else {
            push(b[j]);
            ++j;
        }
    }
    while (i < a.size()) push(a[i++]);
    while (j < b.size()) push(b[j++]);
    return result;
}

RowSpans intersectSpans(const RowSpans& a, const RowSpans& b) {
    RowSpans result;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        const int left = a[i].left > b[j].left ? a[i].left : b[j].left;
        const int right = a[i].right < b[j].right ? a[i].right : b[j].right;
        if (left < right) result.push_back(RegionSpan{left, right});
        if (a[i].right < b[j].right) {
            ++i;
        } else {
            ++j;
        }
    }
    return result;
}

RowSpans subtractSpans(const RowSpans& a, const RowSpans& b) {
    RowSpans result;
    std::size_t j = 0;
    for (const RegionSpan& span : a) {
        while (j < b.size() && b[j].right <= span.left) ++j;
        int cursor = span.left;
        for (std::size_t k = j; k < b.size() && b[k].left < span.right && cursor < span.right; ++k) {
            if (b[k].left > cursor) result.push_back(RegionSpan{cursor, b[k].left});
            if (b[k].right > cursor) cursor = b[k].right;
        }
        if (cursor < span.right) result.push_back(RegionSpan{cursor, span.right});
    }
    return result;
}

// 把逐行区间压回条带：区间相同且上下相邻的行合并成一条。
// 合并是 COMPLEXREGION 判定成立的前提（见 regionComplexity 的注释）。
RegionBands compressRows(const std::vector<RowSpans>& rows, int top) {
    RegionBands finished;
    RegionBands open;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const int y = top + static_cast<int>(index);
        RegionBands carried;
        carried.reserve(rows[index].size() + open.size());
        for (const RegionSpan& span : rows[index]) {
            bool extended = false;
            for (const RegionBand& band : open) {
                if (band.bottom == y && band.span.left == span.left && band.span.right == span.right) {
                    RegionBand grown = band;
                    grown.bottom = y + 1;
                    carried.push_back(grown);
                    extended = true;
                    break;
                }
            }
            if (!extended) carried.push_back(RegionBand{y, y + 1, span});
        }
        for (const RegionBand& band : open) {
            bool carriedOver = false;
            for (const RegionBand& candidate : carried) {
                if (candidate.top == band.top && candidate.span.left == band.span.left &&
                    candidate.span.right == band.span.right) {
                    carriedOver = true;
                    break;
                }
            }
            if (!carriedOver) finished.push_back(band);
        }
        open = carried;
    }
    for (const RegionBand& band : open) finished.push_back(band);
    return finished;
}

// 布尔组合。返回 false 表示"算不出来"（行数超限或未知模式），调用方必须
// 报错而不是把空区域当成结果——空区域在 Win32 里是一个合法的、有意义的答案。
bool combineRegions(const RegionBands& a, const RegionBands& b, int mode, RegionBands& out) {
    out.clear();
    if (mode != RGN_AND && mode != RGN_OR && mode != RGN_DIFF && mode != RGN_XOR) return false;
    int top = 0;
    int bottom = 0;
    if (!a.empty() && !b.empty()) {
        const IntRect left = regionBounds(a);
        const IntRect right = regionBounds(b);
        top = left.top < right.top ? left.top : right.top;
        bottom = left.bottom > right.bottom ? left.bottom : right.bottom;
    } else if (!a.empty()) {
        const IntRect left = regionBounds(a);
        top = left.top;
        bottom = left.bottom;
    } else if (!b.empty()) {
        const IntRect right = regionBounds(b);
        top = right.top;
        bottom = right.bottom;
    } else {
        return true;  // 两边都空 → 结果空
    }
    if (bottom - top > kMaxRegionRows) return false;
    std::vector<RowSpans> rows(static_cast<std::size_t>(bottom - top));
    for (int y = top; y < bottom; ++y) {
        const RowSpans left = spansAtRow(a, y);
        const RowSpans right = spansAtRow(b, y);
        RowSpans& slot = rows[static_cast<std::size_t>(y - top)];
        switch (mode) {
            case RGN_AND: slot = intersectSpans(left, right); break;
            case RGN_OR: slot = unionSpans(left, right); break;
            case RGN_DIFF: slot = subtractSpans(left, right); break;
            default: slot = unionSpans(subtractSpans(left, right), subtractSpans(right, left)); break;
        }
    }
    out = compressRows(rows, top);
    return true;
}

// 与一块矩形求交（裁剪区与表面求交、IntersectClipRect 都用它）。
bool intersectRegionRect(const RegionBands& bands, const IntRect& bounds, RegionBands& out) {
    RegionBands frame;
    setRectBands(frame, bounds.left, bounds.top, bounds.right, bounds.bottom);
    return combineRegions(bands, frame, RGN_AND, out);
}

// 多边形 → 条带表（扫描线填充）。
//
// 采样线取每行像素中心 y+0.5；交点按半开判据 `y1 <= scan < y2` 归类，
// 这样顶点恰好落在采样线上时只被算一次（否则奇偶配对会错位一整行）。
// 像素判定同理取中心：像素 x 属于区域 ⇔ x+0.5 ∈ [x0, x1)。
//
// ALTERNATE(奇偶) 与 WINDING(非零环绕) 的差别不是学术问题：RuntimeShell
// 画进度时钟的遮罩用的是 WINDING，同一个多边形在 ALTERNATE 下中间会被挖空。
bool polygonBands(const POINT* points, int count, int fillMode, RegionBands& out) {
    out.clear();
    if (points == nullptr || count < 3) return true;  // 少于三个点 = 空区域，不是失败
    if (fillMode != ALTERNATE && fillMode != WINDING) return false;
    int minY = points[0].y;
    int maxY = points[0].y;
    for (int i = 1; i < count; ++i) {
        if (points[i].y < minY) minY = points[i].y;
        if (points[i].y > maxY) maxY = points[i].y;
    }
    if (maxY - minY > kMaxRegionRows) return false;
    std::vector<RowSpans> rows(static_cast<std::size_t>(maxY - minY));

    for (int y = minY; y < maxY; ++y) {
        const double scan = static_cast<double>(y) + 0.5;
        std::vector<double> crossings;
        std::vector<int> directions;
        for (int i = 0; i < count; ++i) {
            const POINT& from = points[i];
            const POINT& to = points[(i + 1) % count];
            int direction = 0;
            if (from.y <= scan && scan < to.y) {
                direction = 1;
            } else if (to.y <= scan && scan < from.y) {
                direction = -1;
            } else {
                continue;
            }
            const double span = static_cast<double>(to.y) - static_cast<double>(from.y);
            const double x = static_cast<double>(from.x) +
                             (scan - static_cast<double>(from.y)) *
                                 (static_cast<double>(to.x) - static_cast<double>(from.x)) / span;
            std::size_t position = crossings.size();
            crossings.push_back(x);
            directions.push_back(direction);
            while (position > 0 && crossings[position - 1] > x) {
                crossings[position] = crossings[position - 1];
                directions[position] = directions[position - 1];
                --position;
            }
            crossings[position] = x;
            directions[position] = direction;
        }
        if (crossings.size() < 2) continue;

        RowSpans& slot = rows[static_cast<std::size_t>(y - minY)];
        const auto append = [&slot](double from, double to) {
            const int left = static_cast<int>(std::ceil(from - 0.5));
            const int right = static_cast<int>(std::ceil(to - 0.5));
            if (right > left) slot.push_back(RegionSpan{left, right});
        };
        if (fillMode == ALTERNATE) {
            for (std::size_t k = 0; k + 1 < crossings.size(); k += 2) {
                append(crossings[k], crossings[k + 1]);
            }
        } else {
            int winding = 0;
            double start = 0.0;
            for (std::size_t k = 0; k < crossings.size(); ++k) {
                const int previous = winding;
                winding += directions[k];
                if (previous == 0 && winding != 0) {
                    start = crossings[k];
                } else if (previous != 0 && winding == 0) {
                    append(start, crossings[k]);
                }
            }
        }
    }
    out = compressRows(rows, minY);
    return true;
}

// 裁剪区在当前表面下的可见部分。没有 clip region 时等于整块表面。
// 返回 false 表示算不出来（无表面，或行数超限）——调用方必须报 ERROR，
// 不能退回"整块表面"：那会让一个复杂裁剪在失败时静默变成无裁剪。
bool visibleClipBands(const CompatDevice& device, RegionBands& out) {
    out.clear();
    if (device.bitmap == nullptr) return false;
    const IntRect surface{0, 0, device.bitmap->width, device.bitmap->height};
    if (!device.hasClip) {
        setRectBands(out, surface.left, surface.top, surface.right, surface.bottom);
        return true;
    }
    return intersectRegionRect(device.clip, surface, out);
}

// ---------------------------------------------------------------------------
// 像素换算
// ---------------------------------------------------------------------------
// COLORREF：0x00BBGGRR（R 在低 8 位）
// 32bpp DIB 像素：内存字节序 B,G,R,A，按小端读成 uint32 即 0xAARRGGBB

inline COLORREF toColorRef(std::uint32_t pixel) {
    return ((pixel >> 16) & 0xFFu) | (pixel & 0x0000FF00u) | ((pixel & 0xFFu) << 16);
}

inline std::uint32_t toPixel(COLORREF color) {
    return 0xFF000000u | ((color & 0xFFu) << 16) | (color & 0x0000FF00u) | ((color >> 16) & 0xFFu);
}

inline std::uint8_t* rowPointer(CompatObject& bitmap, int y) {
    const int row = bitmap.topDown ? y : (bitmap.height - 1 - y);
    return bitmap.pixels + static_cast<std::ptrdiff_t>(row) * bitmap.stride;
}

inline std::uint32_t readPixel(CompatObject& bitmap, int x, int y) {
    return *reinterpret_cast<const std::uint32_t*>(rowPointer(bitmap, y) + x * 4);
}

inline void writePixel(CompatObject& bitmap, int x, int y, std::uint32_t pixel) {
    *reinterpret_cast<std::uint32_t*>(rowPointer(bitmap, y) + x * 4) = pixel;
}

// DSTINVERT 只翻转 RGB，保留高 8 位。真实 GDI 对 32bpp BI_RGB 不解释 alpha，
// 若把 0xFF 一起取反会得到 0x00，后续任何把该 DIB 当 RGBA 源的路径都会突然变
// 成全透明——这是"能编译、测试过、画面消失"的典型坑，故显式保住 alpha。
constexpr std::uint32_t kRgbMask = 0x00FFFFFFu;

inline std::uint32_t invertRgb(std::uint32_t pixel) {
    return (pixel & 0xFF000000u) | ((~pixel) & kRgbMask);
}

// ---------------------------------------------------------------------------
// 库存对象
// ---------------------------------------------------------------------------
// 真实 GDI 的库存对象是系统预建、句柄为小整数、永不释放。这里用静态存储期的
// 对象，句柄 = 该对象地址，同样稳定、同样不释放。未初始化的槽位 magic 保持 0，
// GetStockObject 会返回 nullptr 而不是一个假对象。

void markStock(CompatObject* table, int index, ObjectKind kind) {
    if (index < 0 || index >= kStockSlots) return;
    table[index].magic = kObjectMagic;
    table[index].kind = kind;
    table[index].stock = true;
}

CompatObject* stockTable() {
    static CompatObject table[kStockSlots];
    static bool ready = false;
    if (!ready) {
        for (CompatObject& entry : table) entry.magic = 0;
        // table 是静态存储期对象，lambda 无需捕获。
        const auto add = [](int index, ObjectKind kind) { markStock(table, index, kind); };
        add(WHITE_BRUSH, ObjectKind::Brush);
        table[WHITE_BRUSH].color = 0x00FFFFFFu;
        add(LTGRAY_BRUSH, ObjectKind::Brush);
        table[LTGRAY_BRUSH].color = 0x00C0C0C0u;
        add(GRAY_BRUSH, ObjectKind::Brush);
        table[GRAY_BRUSH].color = 0x00808080u;
        add(DKGRAY_BRUSH, ObjectKind::Brush);
        table[DKGRAY_BRUSH].color = 0x00404040u;
        add(BLACK_BRUSH, ObjectKind::Brush);
        table[BLACK_BRUSH].color = 0x00000000u;
        add(NULL_BRUSH, ObjectKind::Brush);
        table[NULL_BRUSH].hollow = true;  // 空画刷：不填充
        add(DC_BRUSH, ObjectKind::Brush);
        table[DC_BRUSH].color = 0x00FFFFFFu;

        add(WHITE_PEN, ObjectKind::Pen);
        table[WHITE_PEN].color = 0x00FFFFFFu;
        add(BLACK_PEN, ObjectKind::Pen);
        table[BLACK_PEN].color = 0x00000000u;
        add(NULL_PEN, ObjectKind::Pen);
        table[NULL_PEN].penStyle = PS_NULL;
        add(DC_PEN, ObjectKind::Pen);

        for (int index : {SYSTEM_FONT, DEFAULT_GUI_FONT, ANSI_FIXED_FONT, ANSI_VAR_FONT,
                          DEVICE_DEFAULT_FONT, OEM_FIXED_FONT, SYSTEM_FIXED_FONT}) {
            add(index, ObjectKind::Font);
        }
        ready = true;
    }
    return table;
}

// ---------------------------------------------------------------------------
// 句柄转换与校验
// ---------------------------------------------------------------------------

inline CompatObject* asObject(HGDIOBJ handle) {
    if (handle == nullptr) return nullptr;
    auto* object = static_cast<CompatObject*>(handle);
    return object->magic == kObjectMagic ? object : nullptr;
}

inline CompatDevice* asDevice(HDC handle) {
    if (handle == nullptr) return nullptr;
    auto* device = reinterpret_cast<CompatDevice*>(handle);
    return device->magic == kDeviceMagic ? device : nullptr;
}

inline HGDIOBJ toHandle(CompatObject* object) {
    return object == nullptr ? nullptr : static_cast<HGDIOBJ>(object);
}

// ---------------------------------------------------------------------------
// 位图生命周期
// ---------------------------------------------------------------------------

CompatObject* allocateBitmap(int width, int height, bool topDown) {
    if (width <= 0 || height <= 0) return nullptr;
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    const std::size_t bytes = stride * static_cast<std::size_t>(height);
    // 确定性清零：Windows 不保证新位图内容，但"确定的黑"让像素断言与截图可复现。
    auto* pixels = new (std::nothrow) std::uint8_t[bytes]();
    if (pixels == nullptr) return nullptr;
    auto* object = new (std::nothrow) CompatObject();
    if (object == nullptr) {
        delete[] pixels;
        return nullptr;
    }
    object->kind = ObjectKind::Bitmap;
    object->width = width;
    object->height = height;
    object->stride = static_cast<int>(stride);
    object->topDown = topDown;
    object->pixels = pixels;
    ++liveObjectCount;
    return object;
}

void releaseObject(CompatObject* object) {
    if (object == nullptr || object->stock) return;
    --liveObjectCount;
    if (object->kind == ObjectKind::Bitmap && object->pixels != nullptr) {
        delete[] object->pixels;
        object->pixels = nullptr;
    }
    object->magic = 0;
    delete object;
}

// 目标/源均已在边界内，按 ROP 逐像素合成。source 为空时只做目标运算。
void rasterBlock(CompatObject& dest, const IntRect& target, CompatObject* source, int sourceX, int sourceY,
                 DWORD rop) {
    const int width = target.right - target.left;
    const int height = target.bottom - target.top;
    for (int row = 0; row < height; ++row) {
        auto* destination =
            reinterpret_cast<std::uint32_t*>(rowPointer(dest, target.top + row) + target.left * 4);
        const std::uint32_t* sourcePixels = nullptr;
        if (source != nullptr) {
            sourcePixels = reinterpret_cast<const std::uint32_t*>(
                rowPointer(*source, sourceY + row) + sourceX * 4);
        }
        for (int column = 0; column < width; ++column) {
            const std::uint32_t previous = destination[column];
            std::uint32_t value = previous;
            switch (rop) {
                case SRCCOPY:
                    value = sourcePixels[column];
                    break;
                case DSTINVERT:
                    value = invertRgb(previous);
                    break;
                case BLACKNESS:
                    value = 0xFF000000u;
                    break;
                case WHITENESS:
                    value = 0xFFFFFFFFu;
                    break;
                case SRCAND:
                    value = previous & sourcePixels[column];
                    break;
                case SRCINVERT:
                    value = previous ^ sourcePixels[column];
                    break;
                case SRCPAINT:
                    value = previous | sourcePixels[column];
                    break;
                default:
                    break;
            }
            destination[column] = value;
        }
    }
}

// 自拷贝：行内用 memmove 处理横向重叠，纵向按位移方向选行序，避免写坏未读。
void rasterSelfCopy(CompatObject& bitmap, const IntRect& target, int sourceX, int sourceY) {
    const int width = target.right - target.left;
    const int height = target.bottom - target.top;
    const bool movesDown = target.top > sourceY;
    for (int step = 0; step < height; ++step) {
        const int row = movesDown ? (height - 1 - step) : step;
        const std::uint8_t* from = rowPointer(bitmap, sourceY + row) + sourceX * 4;
        std::uint8_t* to = rowPointer(bitmap, target.top + row) + target.left * 4;
        std::memmove(to, from, static_cast<std::size_t>(width) * 4u);
    }
}

bool isSupportedBlitRop(DWORD rop) {
    return rop == SRCCOPY || rop == DSTINVERT || rop == BLACKNESS || rop == WHITENESS || rop == SRCAND ||
           rop == SRCINVERT || rop == SRCPAINT;
}

// 把外部裸 DIB（BGRA）贴进 DC。scale 为真做最近邻缩放，否则 1:1 拷贝。
int blitExternalDib(HDC hdc, int xDest, int yDest, int destWidth, int destHeight, const void* lpBits,
                    const BITMAPINFO* lpbmi, int xSrc, int ySrc, int srcWidth, int srcHeight, bool scale) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || device->bitmap == nullptr) return 0;
    if (lpBits == nullptr || lpbmi == nullptr) return 0;
    const BITMAPINFOHEADER& header = lpbmi->bmiHeader;
    if (header.biBitCount != 32 || header.biCompression != BI_RGB) {
        std::fprintf(stderr,
                     "[compat/win32] DIB upload: only 32bpp BI_RGB is supported "
                     "(got biBitCount=%u biCompression=%lu)\n",
                     static_cast<unsigned>(header.biBitCount),
                     static_cast<unsigned long>(header.biCompression));
        return 0;
    }
    if (header.biHeight == 0 || header.biWidth <= 0) return 0;

    const int dibWidth = static_cast<int>(header.biWidth);
    const bool topDown = header.biHeight < 0;
    const int dibHeight = topDown ? static_cast<int>(-header.biHeight) : static_cast<int>(header.biHeight);
    const std::size_t dibStride = static_cast<std::size_t>(dibWidth) * 4u;

    // 源窗口：宽高传 0 或负数表示"从 (xSrc, ySrc) 到 DIB 边界"。
    int windowWidth = srcWidth;
    int windowHeight = srcHeight;
    if (windowWidth <= 0) windowWidth = dibWidth - xSrc;
    if (windowHeight <= 0) windowHeight = dibHeight - ySrc;
    int sourceX = xSrc;
    int sourceY = ySrc;
    if (!clipSpan(sourceX, windowWidth, dibWidth)) return 0;
    if (!clipSpan(sourceY, windowHeight, dibHeight)) return 0;

    const int targetWidth = scale ? destWidth : windowWidth;
    const int targetHeight = scale ? destHeight : windowHeight;
    if (targetWidth <= 0 || targetHeight <= 0) return 0;

    CompatObject& dest = *device->bitmap;
    const auto* base = static_cast<const std::uint8_t*>(lpBits);
    const auto dibPixel = [&](int x, int y) -> std::uint32_t {
        const int row = topDown ? y : (dibHeight - 1 - y);
        return *reinterpret_cast<const std::uint32_t*>(base + static_cast<std::size_t>(row) * dibStride + x * 4);
    };

    int scanLinesWritten = 0;
    for (int row = 0; row < targetHeight; ++row) {
        const int targetY = yDest + row;
        if (targetY < 0 || targetY >= dest.height) continue;
        const int sourceRow = scale ? sourceY + (row * windowHeight) / targetHeight : sourceY + row;
        bool wroteRow = false;
        for (int column = 0; column < targetWidth; ++column) {
            const int targetX = xDest + column;
            if (targetX < 0 || targetX >= dest.width) continue;
            const int sourceColumn = scale ? sourceX + (column * windowWidth) / targetWidth : sourceX + column;
            writePixel(dest, targetX, targetY, dibPixel(sourceColumn, sourceRow));
            wroteRow = true;
        }
        if (wroteRow) ++scanLinesWritten;
    }
    return scanLinesWritten;
}

// ---------------------------------------------------------------------------
// EMF 录制
// ---------------------------------------------------------------------------

// 追加一条记录，返回可写区起点。返回值只在下次 append 之前有效（vector 会搬）。
std::uint8_t* emfBeginRecord(EmfRecording& recording, std::size_t bytes) {
    const std::size_t offset = recording.body.size();
    recording.body.resize(offset + bytes, 0);
    ++recording.recordCount;
    return recording.body.data() + offset;
}

void emfAccumulateBounds(EmfRecording& recording, const RECTL& bounds) {
    if (!recording.hasBounds) {
        recording.bounds = bounds;
        recording.hasBounds = true;
        return;
    }
    RECTL& merged = recording.bounds;
    if (bounds.left < merged.left) merged.left = bounds.left;
    if (bounds.top < merged.top) merged.top = bounds.top;
    if (bounds.right > merged.right) merged.right = bounds.right;
    if (bounds.bottom > merged.bottom) merged.bottom = bounds.bottom;
}

// 把一次位块传输序列化成 EMR_BITBLT / EMR_STRETCHBLT。
//
// 源位图连同 BITMAPINFOHEADER 一起内嵌进记录：EMF 是自包含的指令流，回放时
// 不能再去引用录制期的 GDI 对象——源位图在录制结束后就被删掉了，照句柄回放
// 等于读已释放内存。
//
// 只记录 SRCCOPY。DSTINVERT 需要目标像素参与运算，而录制型 DC 没有像素表面，
// 记下来也无从回放，所以在录制时就失败——而不是写一条回放时才暴露缺数据的记录。
BOOL emfRecordBlit(CompatDevice& device, bool stretch, DWORD rop, int xDest, int yDest,
                   int cxDest, int cyDest, HDC hdcSrc, int xSrc, int ySrc, int cxSrc, int cySrc) {
    const char* name = stretch ? "StretchBlt" : "BitBlt";
    if (rop != SRCCOPY) {
        std::fprintf(stderr,
                     "[compat/win32] %s: a recording DC only records SRCCOPY (rop=0x%08lX)\n", name,
                     static_cast<unsigned long>(rop));
        return FALSE;
    }
    CompatDevice* sourceDevice = asDevice(hdcSrc);
    CompatObject* source = sourceDevice == nullptr ? nullptr : sourceDevice->bitmap;
    if (source == nullptr) {
        std::fprintf(stderr, "[compat/win32] %s: recording a blit needs a source bitmap\n", name);
        return FALSE;
    }
    if (cxDest <= 0 || cyDest <= 0 || cxSrc <= 0 || cySrc <= 0) return FALSE;

    int width = cxSrc;
    int height = cySrc;
    if (!clipSpan(xSrc, width, source->width)) return TRUE;   // 整个源矩形在位图之外
    if (!clipSpan(ySrc, height, source->height)) return TRUE;
    if (width != cxSrc || height != cySrc) {
        // 部分越界：目标尺寸不变而源变小，回放时会拉伸错。不记录，也不假装成功。
        std::fprintf(stderr,
                     "[compat/win32] %s: a partially out-of-bounds source rectangle is not recorded\n",
                     name);
        return FALSE;
    }

    const std::size_t bitsStride = static_cast<std::size_t>(width) * 4u;
    const std::size_t bitsBytes = bitsStride * static_cast<std::size_t>(height);
    const std::size_t headerBytes = stretch ? sizeof(EMRSTRETCHBLT) : sizeof(EMRBITBLT);
    const std::size_t recordBytes = headerBytes + sizeof(BITMAPINFOHEADER) + bitsBytes;

    std::uint8_t* base = emfBeginRecord(*device.recording, recordBytes);
    // EMR_STRETCHBLT 是 EMR_BITBLT 的超集（只多尾部两个字段），公共前缀同形。
    auto* record = reinterpret_cast<EMRBITBLT*>(base);
    record->iType = stretch ? EMR_STRETCHBLT : EMR_BITBLT;
    record->nSize = static_cast<DWORD>(recordBytes);
    // RECTL 沿用 RECT 的右/下开区间约定，与审计侧的 cxDest/cyDest 对得上。
    record->rclBounds = RECTL{xDest, yDest, xDest + cxDest, yDest + cyDest};
    record->xDest = xDest;
    record->yDest = yDest;
    record->cxDest = cxDest;
    record->cyDest = cyDest;
    record->dwRop = rop;
    record->xSrc = xSrc;
    record->ySrc = ySrc;
    record->xformSrc = XFORM{};  // 单位变换
    record->crBkColorSrc = device.bkColor;
    record->iUsageSrc = DIB_RGB_COLORS;
    record->offBmiSrc = static_cast<DWORD>(headerBytes);
    record->cbBmiSrc = static_cast<DWORD>(sizeof(BITMAPINFOHEADER));
    record->offBitsSrc = record->offBmiSrc + record->cbBmiSrc;
    record->cbBitsSrc = static_cast<DWORD>(bitsBytes);
    if (stretch) {
        auto* exact = reinterpret_cast<EMRSTRETCHBLT*>(base);
        exact->cxSrc = cxSrc;
        exact->cySrc = cySrc;
    }

    auto* header = reinterpret_cast<BITMAPINFOHEADER*>(base + record->offBmiSrc);
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = width;
    // 负高度 = top-down，与源位图的内存行序一致：回放不需要翻转，也不会因为
    // 依赖 bottom-up 约定而把记录流写成上下颠倒的位图。
    header->biHeight = -height;
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(bitsBytes);

    std::uint8_t* bits = base + record->offBitsSrc;
    for (int row = 0; row < height; ++row) {
        std::memcpy(bits + static_cast<std::size_t>(row) * bitsStride,
                    rowPointer(*source, ySrc + row) + static_cast<std::ptrdiff_t>(xSrc) * 4,
                    bitsStride);
    }
    emfAccumulateBounds(*device.recording, record->rclBounds);
    return TRUE;
}

// 像素 → 0.01 毫米（EMF 头部的 rclFrame 单位）与微米。按 96 DPI 换算，与
// Win32 在 96 DPI 参考设备上给出的数值一致。
inline LONG pixelsToHundredthsOfMillimeter(LONG pixels) {
    return static_cast<LONG>((static_cast<long long>(pixels) * 2540) / 96);
}

inline LONG pixelsToMicrometers(LONG pixels) {
    return static_cast<LONG>((static_cast<long long>(pixels) * 25400) / 96);
}

// ---------------------------------------------------------------------------
// 图元光栅的公共件（S3-3，C 段）
// ---------------------------------------------------------------------------
//
// 这一族（MoveToEx / LineTo / FillRect / FrameRect / Rectangle / Ellipse /
// RoundRect / Polygon / Polyline / PolyPolyline / GdiAlphaBlend）看起来只是
// "往 DIB 里画像素"，真正难的是三件 S3-1/S3-2 已经埋好坑的事：
//
// 1) 映射必须真做。(x-windowOrg)*viewportExt/windowExt+viewportOrg 不是装饰：
//    RuntimeRadarPixelStoreContract 的 mode 4 把窗口设成表面尺寸、视口设成
//    一半，然后断言快路径退让。忽略映射 = SetMapMode 返回成功但不缩放，而
//    快慢两路用的是同一套 GDI，所有"自洽"断言照样全绿，看不出问题。
//
// 2) 裁剪必须在像素路径上生效。S3-2 把裁剪变成了真状态（可建、可布尔组合、
//    GetClipBox 三态正确），但 BitBlt/PatBlt 至今不读它。RuntimeShell 的
//    进度时钟遮罩是 SelectClipRgn + FillRect，在 mac 上会画成整块方形。
//
// 3) 轮廓与填充不能重叠。R2_XORPEN 在本项目里的用法是"画一遍再画一遍擦掉"
//    （RuntimeSelectedPathRasterContract / RuntimeSelectionFrameRasterContract），
//    同一像素被描边和填充各写一次就会留下残影。故填充区 = 形状内缩 penWidth，
//    描边区 = 形状减去该内缩区，两者严格不相交。
//
// 有意偏离 GDI 的两处（记在这里，因为它们看起来像 bug）：
//   * 描边按**内侧对齐**。GDI 把笔沿路径居中（宽 2 时一半落在形状外），这里
//     全部画在内侧。w==1 时两者逐像素一致；项目里 w>1 只用于焦点边框（2/3），
//     相关断言是快慢两路自洽比较，因此不影响任何判据，但截图会差半个像素。
//   * 形状的轮廓与填充永不重叠（见 3）。GDI 先填后描，重叠处写两次，在
//     R2_XORPEN 下表现为相互抵消。本层选择"每像素只画一次"，因为项目的 XOR
//     用法要的是"能干净擦掉"，不是复刻抵消。

inline bool mirroredMapping(const CompatDevice& device) {
    // 负的窗口/视口范围等于镜像坐标系。Win32 支持，本层不支持——但必须显式
    // 失败而不是默默画空：镜像下"矩形右下角反而在左"会让所有图元静默消失。
    const long long scaleX = static_cast<long long>(device.viewportExt.cx) * device.windowExt.cx;
    const long long scaleY = static_cast<long long>(device.viewportExt.cy) * device.windowExt.cy;
    return scaleX < 0 || scaleY < 0;
}

// 逻辑 → 设备。用 MulDiv 的四舍五入语义（.5 远离零），不用截断：截断在缩小
// 映射下会让整块图形系统性左移半像素，而且两条路径同样偏，断言看不出来。
bool mapPoint(const CompatDevice& device, int x, int y, POINT& out) {
    if (device.windowExt.cx == 0 || device.windowExt.cy == 0) {
        reportUnsupported("mapPoint", "degenerate window extent (division by zero)");
        return false;
    }
    out.x = device.viewportOrg.x +
            MulDiv(x - device.windowOrg.x, device.viewportExt.cx, device.windowExt.cx);
    out.y = device.viewportOrg.y +
            MulDiv(y - device.windowOrg.y, device.viewportExt.cy, device.windowExt.cy);
    return true;
}

// 逻辑矩形 → 设备矩形。返回 false 只表示"这条路径不支持"（镜像映射、退化
// 窗口范围）；右下小于左上（空矩形）返回 true + 空区间，与 Win32 一致：
// 传入反向矩形不画东西，但调用仍然成功。
bool mapRect(const CompatDevice& device, int left, int top, int right, int bottom, IntRect& out) {
    POINT a{};
    POINT b{};
    if (!mapPoint(device, left, top, a) || !mapPoint(device, right, bottom, b)) return false;
    if (mirroredMapping(device)) {
        reportUnsupported("mapRect", "mirrored (negated) coordinate mapping is not implemented");
        return false;
    }
    out = IntRect{a.x, a.y, b.x, b.y};
    return true;
}

inline IntRect inflateRect(const IntRect& rect, int amount) {
    return IntRect{rect.left - amount, rect.top - amount, rect.right + amount, rect.bottom + amount};
}

// ROP2 全 16 种都可实现（都是逐位运算），但只对 RGB 三位生效、保留目标的
// alpha 字节——与 DSTINVERT 里保住 alpha 的理由相同：32bpp BI_RGB 不带 alpha
// 语义，把它一起取反会让后续把该 DIB 当 RGBA 源的路径整片变透明。
bool isSupportedRop2(int rop2) {
    return rop2 >= R2_BLACK && rop2 <= R2_WHITE;
}

std::uint32_t applyRop2(int rop2, std::uint32_t dst, std::uint32_t pen) {
    const std::uint32_t d = dst & kRgbMask;
    const std::uint32_t p = pen & kRgbMask;
    std::uint32_t value = d;
    switch (rop2) {
        case R2_BLACK: value = 0; break;
        case R2_NOTMERGEPEN: value = ~(d | p); break;
        case R2_MASKNOTPEN: value = d & ~p; break;
        case R2_NOTCOPYPEN: value = ~p; break;
        case R2_MASKPENNOT: value = p & ~d; break;
        case R2_NOT: value = ~d; break;
        case R2_XORPEN: value = d ^ p; break;
        case R2_NOTMASKPEN: value = ~(d & p); break;
        case R2_MASKPEN: value = d & p; break;
        case R2_NOTXORPEN: value = ~(d ^ p); break;
        case R2_NOP: value = d; break;
        case R2_MERGENOTPEN: value = d | ~p; break;
        case R2_COPYPEN: value = p; break;
        case R2_MERGEPENNOT: value = p | ~d; break;
        case R2_MERGEPEN: value = d | p; break;
        case R2_WHITE: value = kRgbMask; break;
        default: return dst;
    }
    return (dst & 0xFF000000u) | (value & kRgbMask);
}

// 一切像素写入的唯一出口。存在的理由：裁剪必须只在一个地方实现。若每个图元
// 自己判一遍边界与 clip，迟早有一个漏掉——而漏掉的那个在图元自测里看不出来，
// 只有"整块画面被画满"时才发现。
struct PixelSink {
    CompatObject* bitmap = nullptr;
    const CompatDevice* device = nullptr;

    bool ready() const { return bitmap != nullptr && device != nullptr; }

    // 行 y 上允许写入的区间：表面 ∩ 裁剪区（已排序、互不重叠）。
    void spansForRow(int y, RowSpans& out) const {
        out.clear();
        if (!ready() || y < 0 || y >= bitmap->height) return;
        if (!device->hasClip) {
            out.push_back(RegionSpan{0, bitmap->width});
            return;
        }
        const RowSpans surface{RegionSpan{0, bitmap->width}};
        out = intersectSpans(spansAtRow(device->clip, y), surface);
    }

    void fillSpan(int y, int left, int right, std::uint32_t pen, int rop2) const {
        if (!ready() || right <= left || y < 0 || y >= bitmap->height) return;
        RowSpans spans;
        spansForRow(y, spans);
        if (spans.empty()) return;
        std::uint32_t* row = reinterpret_cast<std::uint32_t*>(rowPointer(*bitmap, y));
        for (const RegionSpan& span : spans) {
            const int from = left > span.left ? left : span.left;
            const int to = right < span.right ? right : span.right;
            for (int x = from; x < to; ++x) row[x] = applyRop2(rop2, row[x], pen);
        }
    }

    void fillRect(const IntRect& rect, std::uint32_t pen, int rop2) const {
        for (int y = rect.top; y < rect.bottom; ++y) fillSpan(y, rect.left, rect.right, pen, rop2);
    }
};

// 每行一个区间。矩形 / 椭圆 / 圆角矩形都是凸的，行交集必然是一个连续区间，
// 所以一张一维表就够；多边形另走条带表（见 Polygon）。
struct ShapeRows {
    int top = 0;
    std::vector<RegionSpan> rows;

    bool span(int y, RegionSpan& out) const {
        if (y < top || y >= top + static_cast<int>(rows.size())) return false;
        const RegionSpan& candidate = rows[static_cast<std::size_t>(y - top)];
        if (candidate.right <= candidate.left) return false;
        out = candidate;
        return true;
    }
};

// 内缩 n 行/像素：每缩一步，丢掉首末两行、每行左右各收 1 像素。
// 凸形状（矩形/椭圆/圆角矩形）的侵蚀可以用这个近似，且对矩形是精确的。
// 只用于"填充区"与"描边区"的划分，不参与几何断言。
ShapeRows erodeShapeRows(const ShapeRows& rows, int inset) {
    ShapeRows result = rows;
    for (int step = 0; step < inset; ++step) {
        if (result.rows.size() <= 2) {
            result.rows.clear();
            break;
        }
        ShapeRows next;
        next.top = result.top + 1;
        next.rows.resize(result.rows.size() - 2);
        for (std::size_t i = 1; i + 1 < result.rows.size(); ++i) {
            RegionSpan span = result.rows[i];
            span.left += 1;
            span.right -= 1;
            next.rows[i - 1] = span;
        }
        result = std::move(next);
    }
    return result;
}

// 椭圆内切于 [left,right) × [top,bottom) 的像素格，逐行给区间。
//
// 判定用像素中心：像素 x 在椭圆内 ⇔ |x+0.5-cx| <= rx*sqrt(1-v²)。
// 同一行只算一次，因此 Ellipse() 的填充、描边与 CreateEllipticRgn 用的是
// **同一个函数**——三处若各写一套浮点，边缘会差一像素，而三者互相不比较，
// 谁都不会报错。
bool ellipseRowSpan(int left, int top, int right, int bottom, int y, RegionSpan& out) {
    out = RegionSpan{0, 0};
    const int width = right - left;
    const int height = bottom - top;
    if (width <= 0 || height <= 0 || y < top || y >= bottom) return false;
    const double cx = static_cast<double>(left) + width / 2.0;
    const double cy = static_cast<double>(top) + height / 2.0;
    const double rx = width / 2.0;
    const double ry = height / 2.0;
    const double v = ((static_cast<double>(y) + 0.5) - cy) / ry;
    if (v <= -1.0 || v >= 1.0) return false;
    const double half = rx * std::sqrt(1.0 - v * v);
    const int first = static_cast<int>(std::ceil(cx - half - 0.5));
    const int last = static_cast<int>(std::floor(cx + half - 0.5));
    if (last < first) return false;
    out = RegionSpan{first, last + 1};
    return true;
}

// 圆角矩形逐行区间。Win32 的 RoundRect 传的是角**直径**，半径是它的一半，
// 且半径超过边长一半时按边长一半截断（否则角椭圆会互相穿插）。
bool roundRectRowSpan(int left, int top, int right, int bottom, int diameterX, int diameterY, int y,
                      RegionSpan& out) {
    out = RegionSpan{0, 0};
    const int width = right - left;
    const int height = bottom - top;
    if (width <= 0 || height <= 0 || y < top || y >= bottom) return false;
    int radiusX = diameterX / 2;
    int radiusY = diameterY / 2;
    if (radiusX > width / 2) radiusX = width / 2;
    if (radiusY > height / 2) radiusY = height / 2;
    if (radiusX <= 0 || radiusY <= 0) {
        out = RegionSpan{left, right};
        return true;
    }
    const int cornerTop = top + radiusY;
    const int cornerBottom = bottom - radiusY;
    if (y >= cornerTop && y < cornerBottom) {
        out = RegionSpan{left, right};
        return true;
    }
    int start = left + radiusX;
    int end = right - radiusX;
    RegionSpan corner{};
    const int ellipseTop = (y < cornerTop) ? top : (bottom - 2 * radiusY);
    if (ellipseRowSpan(left, ellipseTop, left + 2 * radiusX, ellipseTop + 2 * radiusY, y, corner) &&
        corner.left < start) {
        start = corner.left;
    }
    if (ellipseRowSpan(right - 2 * radiusX, ellipseTop, right, ellipseTop + 2 * radiusY, y, corner) &&
        corner.right > end) {
        end = corner.right;
    }
    if (end <= start) return false;
    out = RegionSpan{start, end};
    return true;
}

// 逐行区间表的构造。top/bottom 是**形状真实范围**（决定椭圆几何），
// buildTop/buildBottom 是**裁剪后的表范围**（只为省内存，不影响几何）。
ShapeRows buildShapeRows(int top, int bottom, int buildTop, int buildBottom,
                         bool (*spanAt)(int y, void* context, RegionSpan& out), void* context) {
    ShapeRows rows;
    const int first = top > buildTop ? top : buildTop;
    const int last = bottom < buildBottom ? bottom : buildBottom;
    if (last <= first) return rows;
    rows.top = first;
    rows.rows.resize(static_cast<std::size_t>(last - first));
    for (int y = first; y < last; ++y) {
        RegionSpan span{0, 0};
        if (spanAt(y, context, span)) rows.rows[static_cast<std::size_t>(y - first)] = span;
    }
    return rows;
}

struct EllipseSpanContext {
    int left;
    int top;
    int right;
    int bottom;
};

bool ellipseSpanThunk(int y, void* context, RegionSpan& out) {
    const auto* shape = static_cast<const EllipseSpanContext*>(context);
    return ellipseRowSpan(shape->left, shape->top, shape->right, shape->bottom, y, out);
}

struct RoundRectSpanContext {
    int left;
    int top;
    int right;
    int bottom;
    int diameterX;
    int diameterY;
};

bool roundRectSpanThunk(int y, void* context, RegionSpan& out) {
    const auto* shape = static_cast<const RoundRectSpanContext*>(context);
    return roundRectRowSpan(shape->left, shape->top, shape->right, shape->bottom, shape->diameterX,
                            shape->diameterY, y, out);
}

// 笔/刷取色。DC_PEN / DC_BRUSH 是"颜色由 DC 状态决定"的库存对象，
// 调用点靠 SetDCPenColor/SetDCBrushColor 改色；当普通对象取色会让这两个
// 永远画出黑色/白色，而所有像素断言又是快慢两路自洽比较，照样全绿。
std::uint32_t penPixel(const CompatDevice& device, const CompatObject& pen) {
    if (pen.stock && &pen == &stockTable()[DC_PEN]) return toPixel(device.dcPenColor);
    return toPixel(pen.color);
}

std::uint32_t brushPixel(const CompatDevice& device, const CompatObject& brush) {
    if (brush.stock && &brush == &stockTable()[DC_BRUSH]) return toPixel(device.dcBrushColor);
    return toPixel(brush.color);
}

struct PenState {
    std::uint32_t color = 0;
    int width = 1;
    int style = PS_SOLID;
    bool present = false;
};

PenState resolvePen(const CompatDevice& device) {
    PenState state;
    CompatObject* pen = asObject(device.pen);
    if (pen == nullptr || pen->kind != ObjectKind::Pen) {
        // 没选笔：真实 Win32 的内存 DC 自带一支黑笔，故按 DC_PEN 颜色画。
        state.color = toPixel(device.dcPenColor);
        state.present = true;
        return state;
    }
    state.color = penPixel(device, *pen);
    state.width = pen->penWidth > 0 ? pen->penWidth : 1;
    state.style = pen->penStyle;
    state.present = state.style != PS_NULL;
    return state;
}

struct BrushState {
    std::uint32_t color = 0;
    bool hollow = true;
};

BrushState resolveBrush(const CompatDevice& device) {
    BrushState state;
    CompatObject* brush = asObject(device.brush);
    if (brush == nullptr || brush->kind != ObjectKind::Brush) return state;
    state.hollow = brush->hollow;
    state.color = brushPixel(device, *brush);
    return state;
}

// 笔的虚线节拍。项目只用到 PS_SOLID 与 PS_DOT（43 / 9 处），其余样式按
// 相近的节拍近似——比"当作实线"诚实（调用方要的是视觉分隔，实线会糊成一片），
// 但也不假装与 GDI 的 dash 长度一致。
bool penTickVisible(int style, int step) {
    switch (style) {
        case PS_SOLID: return true;
        case PS_DOT: return (step % 2) == 0;
        case PS_DASH: return (step % 6) < 3;
        case PS_DASHDOT: return (step % 8) < 3 || (step % 8) == 4;
        case PS_DASHDOTDOT: return (step % 10) < 3 || (step % 10) == 4 || (step % 10) == 6;
        default: return true;
    }
}

void stampPen(const PixelSink& sink, int x, int y, std::uint32_t color, int width, int rop2) {
    if (width <= 1) {
        sink.fillSpan(y, x, x + 1, color, rop2);
        return;
    }
    const int offset = (width - 1) / 2;
    for (int row = 0; row < width; ++row) {
        sink.fillSpan(y - offset + row, x - offset, x - offset + width, color, rop2);
    }
}

// Bresenham。includeFirst/includeLast 是必需的，不是可选项：
// LineTo 与 Polyline 的终点**不画**（Win32 文档语义），而起点在后续线段里
// 必须是"已画过"的那一个，否则折线的每个拐点会被写两次——在 R2_XORPEN 下
// 拐点会变成空洞，在 R2_COPYPEN 下看不出来。
void strokeLine(const CompatDevice& device, POINT from, POINT to, bool includeFirst, bool includeLast,
                const PenState& pen, int rop2) {
    PixelSink sink{device.bitmap, &device};
    if (!sink.ready()) return;
    int x = from.x;
    int y = from.y;
    const int dx = to.x >= from.x ? to.x - from.x : from.x - to.x;
    const int dy = to.y >= from.y ? to.y - from.y : from.y - to.y;
    const int stepX = from.x < to.x ? 1 : (from.x > to.x ? -1 : 0);
    const int stepY = from.y < to.y ? 1 : (from.y > to.y ? -1 : 0);
    int error = dx - dy;
    int step = 0;
    for (;;) {
        const bool last = (x == to.x && y == to.y);
        const bool wanted = (includeFirst || step != 0) && (includeLast || !last);
        if (wanted && penTickVisible(pen.style, step)) {
            stampPen(sink, x, y, pen.color, pen.width, rop2);
        }
        if (last) break;
        const int twice = error * 2;
        if (twice > -dy) {
            error -= dy;
            x += stepX;
        }
        if (twice < dx) {
            error += dx;
            y += stepY;
        }
        ++step;
    }
}

// 形状的填充 + 描边。填充区 = 形状内缩 penWidth，描边区 = 形状减去内缩区，
// 两者不相交（见文件头第 3 条）。
void paintShapeRows(const CompatDevice& device, const ShapeRows& rows, const PenState& pen,
                    const BrushState& brush, int rop2) {
    PixelSink sink{device.bitmap, &device};
    if (!sink.ready()) return;

    const int fillInset = pen.present ? pen.width : 1;
    if (!brush.hollow) {
        const ShapeRows inner = erodeShapeRows(rows, fillInset);
        for (int index = 0; index < static_cast<int>(inner.rows.size()); ++index) {
            const int y = inner.top + index;
            const RegionSpan& span = inner.rows[static_cast<std::size_t>(index)];
            sink.fillSpan(y, span.left, span.right, brush.color, rop2);
        }
    }
    if (!pen.present) return;

    const ShapeRows inner = erodeShapeRows(rows, pen.width);
    for (int index = 0; index < static_cast<int>(rows.rows.size()); ++index) {
        const int y = rows.top + index;
        RegionSpan self{};
        if (!rows.span(y, self)) continue;
        RegionSpan hole{};
        if (!inner.span(y, hole)) {
            sink.fillSpan(y, self.left, self.right, pen.color, rop2);
            continue;
        }
        sink.fillSpan(y, self.left, hole.left, pen.color, rop2);
        sink.fillSpan(y, hole.right, self.right, pen.color, rop2);
    }
}

// ---------------------------------------------------------------------------
// 文本（P5-06 度量 / S4-2 真字形）
// ---------------------------------------------------------------------------
// 三段分工，边界是刻意划的：
//   · 度量 fontMetricsFor —— 唯一一处"逻辑像素 → 单元格"的映射，仍是 5:7（窄）
//     与 1:1（东亚宽）的固定栅格。GetTextExtentPoint32W / TextOutW / DrawTextW
//     全部走它，所以"先量后画"永远一致（P5-06 的断言守的就是这条）。
//   · 字形 RuntimeWin32CompatText.cpp —— CoreText 光栅成 1bit mask，只知道
//     码点和单元格，不知道裁剪、ROP2、背景模式。
//   · 合成（本文件 drawTextRun）—— 裁剪、ROP2、背景模式全部留在 PixelSink，
//     与其他图元共用同一条像素路径。字形层不碰像素，像素层不认识字体。
//
// 缺字形时仍画空心框（原来的"豆腐块"）。它现在恢复了本意："这个码点在系统字体
// 里确实没有字形"，而不是"文本功能还没接进来"。两者在屏幕上一样，含义不同。

struct FontMetrics {
    int height = 16;  // 单元格高度（像素）
    int width = 11;   // 单元格宽度（像素）
};

// 字体度量：字高取 lfHeight 绝对值（负值 = "字符高度"、正值 = "单元格高度"，
// 两者在本层同义：单元格就是字符盒）；字宽取 lfWidth 绝对值，未指定（0）时按
// 5:7 固定比例。库存字体（DEFAULT_GUI_FONT 等）没有 LOGFONTW，回落到 16px。
FontMetrics fontMetricsFor(const CompatObject* font) {
    FontMetrics m;
    int h = (font != nullptr && font->fontLog.lfHeight != 0) ? font->fontLog.lfHeight : 16;
    m.height = h < 0 ? -h : h;
    if (m.height <= 0) m.height = 16;
    int w = (font != nullptr && font->fontLog.lfWidth != 0) ? font->fontLog.lfWidth : 0;
    m.width = w < 0 ? -w : w;
    if (m.width <= 0) m.width = (m.height * 5 + 3) / 7;  // round(height * 5 / 7)
    if (m.width <= 0) m.width = 1;
    return m;
}

// LOGFONTW 里字形后端需要的三项：族名、字重、斜体。
text::FaceSpec faceSpecFor(const CompatObject* font) {
    text::FaceSpec spec;
    if (font == nullptr) return spec;
    spec.face.assign(font->fontLog.lfFaceName);  // NUL 终止，wchar_t[LF_FACESIZE]
    spec.weight = font->fontLog.lfWeight != 0 ? static_cast<int>(font->fontLog.lfWeight) : FW_NORMAL;
    spec.italic = font->fontLog.lfItalic != 0;
    return spec;
}

// 缺字形：1 像素空心框，占满整个前进框。
void buildMissingGlyphMask(int width, int height, text::GlyphMask& mask) {
    mask.width = width;
    mask.height = height;
    mask.ink.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    if (width <= 0 || height <= 0) return;
    for (int column = 0; column < width; ++column) {
        mask.ink[static_cast<std::size_t>(column)] = 1;
        mask.ink[static_cast<std::size_t>(height - 1) * static_cast<std::size_t>(width)
            + static_cast<std::size_t>(column)] = 1;
    }
    for (int row = 0; row < height; ++row) {
        mask.ink[static_cast<std::size_t>(row) * static_cast<std::size_t>(width)] = 1;
        mask.ink[static_cast<std::size_t>(row) * static_cast<std::size_t>(width)
            + static_cast<std::size_t>(width - 1)] = 1;
    }
}

// 把一个字形的 mask 合成到表面。
//
// 背景先铺满整行（OPAQUE），再按行把 ink 的连续段一次性 fillSpan——逐像素写会
// 让 XOR 用例变慢十倍，而"XOR 画两遍逐字节还原"正是文本层唯一的硬证据。
void compositeGlyphRun(PixelSink& sink, int left, int top, const text::GlyphMask& mask,
                       std::uint32_t fg, std::uint32_t bg, int bkMode, int rop2,
                       const RECT* clip) {
    for (int row = 0; row < mask.height; ++row) {
        const int rowY = top + row;
        int backgroundLeft = left;
        int backgroundRight = left + mask.width;
        if (clip != nullptr) {
            if (rowY < clip->top || rowY >= clip->bottom) continue;
            if (backgroundLeft < clip->left) backgroundLeft = clip->left;
            if (backgroundRight > clip->right) backgroundRight = clip->right;
        }
        if (bkMode == OPAQUE && backgroundRight > backgroundLeft) {
            sink.fillSpan(rowY, backgroundLeft, backgroundRight, bg, R2_COPYPEN);
        }
        int runStart = -1;
        for (int column = 0; column <= mask.width; ++column) {
            bool ink = column < mask.width && mask.at(column, row) != 0;
            if (ink && clip != nullptr) {
                const int pixelX = left + column;
                ink = pixelX >= clip->left && pixelX < clip->right;
            }
            if (ink && runStart < 0) {
                runStart = column;
            } else if (!ink && runStart >= 0) {
                sink.fillSpan(rowY, left + runStart, left + column, fg, rop2);
                runStart = -1;
            }
        }
    }
}

// 画一串字形。dx 非空时按逐字符步进（ExtTextOut 的字符间距数组），否则用字形的
// 前进宽度；clip 非空时（ETO_CLIPPED）只画矩形内部。
void drawTextRun(CompatDevice* device, int x, int y, const wchar_t* text, int count,
                 const int* dx, const RECT* clip) {
    if (device == nullptr || device->bitmap == nullptr || text == nullptr || count <= 0) return;
    const CompatObject* font = asObject(device->font);
    const FontMetrics m = fontMetricsFor(font);
    const text::FaceSpec spec = faceSpecFor(font);
    const std::uint32_t fg = toPixel(device->textColor);
    const std::uint32_t bg = toPixel(device->bkColor);
    PixelSink sink{device->bitmap, device};
    int penX = x;
    for (int index = 0; index < count;) {
        const char32_t codePoint = text::decodeCodePoint(text, index, count);
        const int units = text::codePointLength(text, index, count);
        text::GlyphMask mask;
        if (!text::rasterizeGlyph(codePoint, spec, m.width, m.height, mask)) {
            buildMissingGlyphMask(text::glyphAdvance(codePoint, m.width, m.height), m.height, mask);
        }
        compositeGlyphRun(sink, penX, y, mask, fg, bg, device->bkMode, device->rop2, clip);
        const int step = (dx != nullptr && dx[index] > 0) ? dx[index] : mask.width;
        penX += step;
        index += units;
    }
}

// 宽串的字符数；c == -1 表示"到 NUL 为止"（DrawText 的语义）。
int wideCount(const wchar_t* text, int c) {
    if (text == nullptr) return 0;
    if (c >= 0) return c;
    const wchar_t* p = text;
    while (*p != 0) ++p;
    return static_cast<int>(p - text);
}

// A 串的字符数；c == -1 表示"到 NUL 为止"。产品侧 A 串调用点全是 ASCII
// （building label 的 size()），字节数 == 字符数，故这里直接取字节长度当字符数。
int ansiCount(const char* text, int c) {
    if (text == nullptr) return 0;
    if (c >= 0) return c;
    return static_cast<int>(std::strlen(text));
}

}  // namespace

// ===========================================================================
// 对外接口（必须 extern "C"，与 S1 头里的声明保持同一链接名）
// ===========================================================================
extern "C" {

// ---------------------------------------------------------------------------
// 对象管理
// ---------------------------------------------------------------------------

HDC CreateCompatibleDC(HDC hdc) {
    // 参数只是"目标设备的色彩能力"提示；本层只有 32bpp BGRA 一种格式，忽略。
    (void)hdc;
    auto* device = new (std::nothrow) CompatDevice();
    if (device == nullptr) return nullptr;
    device->pen = toHandle(&stockTable()[BLACK_PEN]);
    device->brush = toHandle(&stockTable()[WHITE_BRUSH]);
    device->font = toHandle(&stockTable()[DEFAULT_GUI_FONT]);
    return reinterpret_cast<HDC>(device);
}

HBITMAP CreateCompatibleBitmap(HDC hdc, int cx, int cy) {
    (void)hdc;
    return reinterpret_cast<HBITMAP>(allocateBitmap(cx, cy, true));
}

HBITMAP CreateDIBSection(HDC hdc, const BITMAPINFO* pbmi, UINT usage, void** ppvBits, HANDLE hSection,
                         DWORD offset) {
    (void)hdc;
    if (ppvBits != nullptr) *ppvBits = nullptr;
    if (pbmi == nullptr) {
        reportUnsupported("CreateDIBSection", "null BITMAPINFO");
        return nullptr;
    }
    if (hSection != nullptr || offset != 0) {
        reportUnsupported("CreateDIBSection", "file-mapped sections are not supported");
        return nullptr;
    }
    const BITMAPINFOHEADER& header = pbmi->bmiHeader;
    if (header.biPlanes != 1) {
        reportUnsupportedLong("CreateDIBSection", "biPlanes", header.biPlanes);
        return nullptr;
    }
    if (header.biBitCount != 32 || header.biCompression != BI_RGB) {
        std::fprintf(stderr,
                     "[compat/win32] CreateDIBSection: only 32bpp BI_RGB is supported "
                     "(got biBitCount=%u biCompression=%lu)\n",
                     static_cast<unsigned>(header.biBitCount),
                     static_cast<unsigned long>(header.biCompression));
        return nullptr;
    }
    if (usage != DIB_RGB_COLORS) {
        reportUnsupportedLong("CreateDIBSection", "usage", static_cast<long>(usage));
        return nullptr;
    }
    if (header.biHeight == 0 || header.biWidth <= 0) {
        reportUnsupported("CreateDIBSection", "degenerate extent");
        return nullptr;
    }
    const bool topDown = header.biHeight < 0;
    const int width = static_cast<int>(header.biWidth);
    const int height = topDown ? static_cast<int>(-header.biHeight) : static_cast<int>(header.biHeight);
    CompatObject* object = allocateBitmap(width, height, topDown);
    if (object == nullptr) {
        reportUnsupported("CreateDIBSection", "out of memory");
        return nullptr;
    }
    // 从这一刻起它是 DIBSection：GetObject 会给出完整 DIBSECTION 与像素指针。
    // header 逐字节保留入参（含负的 biHeight），因为产品侧会拿
    // dsBmih.biHeight 的符号做交叉校验（虽然它允许正负两解）。
    object->dibSection = true;
    object->header = header;
    if (ppvBits != nullptr) {
        // Win32 语义：返回指向"内存中第一行"的指针。自底向上位图的第一行是图像
        // 末行，故此处原样返回 pixels，不能改成逻辑首行。
        *ppvBits = object->pixels;
    }
    return reinterpret_cast<HBITMAP>(object);
}

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h) {
    CompatDevice* device = asDevice(hdc);
    CompatObject* object = asObject(h);
    if (device == nullptr || object == nullptr) return nullptr;

    switch (object->kind) {
        case ObjectKind::Bitmap: {
            CompatObject* previous = device->bitmap;
            if (previous != nullptr && previous->selectedCount > 0) previous->selectedCount -= 1;
            device->bitmap = object;
            object->selectedCount += 1;
            return toHandle(previous);
        }
        case ObjectKind::Pen: {
            const HGDIOBJ previous = device->pen;
            if (CompatObject* old = asObject(previous); old != nullptr && old->selectedCount > 0) {
                old->selectedCount -= 1;
            }
            device->pen = h;
            object->selectedCount += 1;
            return previous;
        }
        case ObjectKind::Brush: {
            const HGDIOBJ previous = device->brush;
            if (CompatObject* old = asObject(previous); old != nullptr && old->selectedCount > 0) {
                old->selectedCount -= 1;
            }
            device->brush = h;
            object->selectedCount += 1;
            return previous;
        }
        case ObjectKind::Font: {
            const HGDIOBJ previous = device->font;
            if (CompatObject* old = asObject(previous); old != nullptr && old->selectedCount > 0) {
                old->selectedCount -= 1;
            }
            device->font = h;
            object->selectedCount += 1;
            return previous;
        }
        case ObjectKind::Region:
            // 区域不是可 SelectObject 的对象：Win32 要求用 SelectClipRgn。
            // 放行会把区域塞进 bitmap/pen 槽位，之后每一次绘制都读到垃圾。
            std::fprintf(stderr, "[compat/win32] SelectObject: a region must be selected with SelectClipRgn\n");
            return nullptr;
    }
    return nullptr;
}

BOOL DeleteObject(HGDIOBJ ho) {
    CompatObject* object = asObject(ho);
    if (object == nullptr) return FALSE;
    // 库存对象不可删除；删了句柄依然有效——与 Windows 一致。
    if (object->stock) return FALSE;
    // 仍被选入 DC 的对象删除失败，调用方必须先还原（项目内代码都遵循这个写法）。
    if (object->selectedCount > 0) return FALSE;
    releaseObject(object);
    return TRUE;
}

BOOL DeleteDC(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return FALSE;
    // 未 CloseEnhMetaFile 就删录制 DC，等于丢弃这份图元文件——与 Windows 一致。
    delete device->recording;
    device->recording = nullptr;
    // 有意偏离：真实 Windows 在仍有非库存对象被选入时返回 FALSE。这里选宽容——
    // 摘掉位图并释放 DC，避免调用方漏写还原时整块帧缓冲泄漏。
    if (device->bitmap != nullptr && device->bitmap->selectedCount > 0) {
        device->bitmap->selectedCount -= 1;
    }
    device->magic = 0;
    delete device;
    return TRUE;
}

// ---------------------------------------------------------------------------
// 增强图元文件（EMF）
// ---------------------------------------------------------------------------

HDC CreateEnhMetaFileW(HDC hdcRef, LPCWSTR lpFilename, const RECT* lpRect, LPCWSTR lpDescription) {
    // hdcRef 只影响坐标映射的参考设备；本层记录的是设备坐标，不需要它。
    (void)hdcRef;
    (void)lpRect;
    (void)lpDescription;
    if (lpFilename != nullptr) {
        // 真实 Windows 会在 CloseEnhMetaFile 时把记录写进这个文件。本层只保留在
        // 内存里——假装写了文件会让"文件存在"这类断言在 mac 上静默通过。
        reportUnsupported("CreateEnhMetaFileW",
                          "writing the metafile to a file is not supported (pass nullptr)");
        return nullptr;
    }
    auto* recording = new (std::nothrow) EmfRecording();
    if (recording == nullptr) return nullptr;
    auto* device = new (std::nothrow) CompatDevice();
    if (device == nullptr) {
        delete recording;
        return nullptr;
    }
    device->pen = toHandle(&stockTable()[BLACK_PEN]);
    device->brush = toHandle(&stockTable()[WHITE_BRUSH]);
    device->font = toHandle(&stockTable()[DEFAULT_GUI_FONT]);
    device->recording = recording;
    return reinterpret_cast<HDC>(device);
}

HENHMETAFILE CloseEnhMetaFile(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || device->recording == nullptr) return nullptr;
    EmfRecording* recording = device->recording;
    device->recording = nullptr;
    // 与 Windows 一致：CloseEnhMetaFile 同时销毁录制 DC。
    device->magic = 0;
    delete device;

    // EMR_EOF 的真实记录体是 5 个 DWORD：iType, nSize, nPalEntries, offPalEntries, nSizeLast。
    constexpr std::size_t kEofBytes = 5 * sizeof(DWORD);
    const std::size_t total = sizeof(ENHMETAHEADER) + recording->body.size() + kEofBytes;
    auto* metafile = new (std::nothrow) EmfMetafile();
    if (metafile == nullptr) {
        delete recording;
        return nullptr;
    }
    metafile->bytes.assign(total, 0);
    std::uint8_t* out = metafile->bytes.data();
    if (!recording->body.empty()) {
        std::memcpy(out + sizeof(ENHMETAHEADER), recording->body.data(), recording->body.size());
    }
    auto* eof = reinterpret_cast<DWORD*>(out + sizeof(ENHMETAHEADER) + recording->body.size());
    eof[0] = EMR_EOF;
    eof[1] = static_cast<DWORD>(kEofBytes);
    eof[2] = 0;  // nPalEntries
    eof[3] = 0;  // offPalEntries
    eof[4] = static_cast<DWORD>(kEofBytes);

    const RECTL bounds = recording->hasBounds ? recording->bounds : RECTL{0, 0, 0, 0};
    ENHMETAHEADER header{};
    header.iType = EMR_HEADER;
    header.nSize = sizeof(ENHMETAHEADER);
    header.rclBounds = bounds;
    header.rclFrame = RECTL{pixelsToHundredthsOfMillimeter(bounds.left),
                            pixelsToHundredthsOfMillimeter(bounds.top),
                            pixelsToHundredthsOfMillimeter(bounds.right),
                            pixelsToHundredthsOfMillimeter(bounds.bottom)};
    header.dSignature = ENHMETA_SIGNATURE;
    header.nVersion = 0x00010000;
    header.nBytes = static_cast<DWORD>(total);
    header.nRecords = recording->recordCount + 2;  // + 头部 + EOF
    // 记录流里没有任何 GDI 对象句柄（位图数据是内嵌的），所以句柄数为 0；
    // 回调收到的 nObjects 也必然是 0。
    header.nHandles = 0;
    header.szlDevice = SIZEL{bounds.right - bounds.left, bounds.bottom - bounds.top};
    header.szlMillimeters = SIZEL{pixelsToHundredthsOfMillimeter(bounds.right - bounds.left) / 100,
                                  pixelsToHundredthsOfMillimeter(bounds.bottom - bounds.top) / 100};
    header.szlMicrometers = SIZEL{pixelsToMicrometers(bounds.right - bounds.left),
                                  pixelsToMicrometers(bounds.bottom - bounds.top)};
    std::memcpy(out, &header, sizeof(header));

    delete recording;
    return metafile;
}

BOOL DeleteEnhMetaFile(HENHMETAFILE hmf) {
    if (hmf == nullptr) return FALSE;
    auto* metafile = static_cast<EmfMetafile*>(hmf);
    if (metafile->magic != kMetafileMagic) return FALSE;
    metafile->magic = 0;
    delete metafile;
    return TRUE;
}

UINT GetEnhMetaFileBits(HENHMETAFILE hmf, UINT cbBuffer, LPBYTE lpbBuffer) {
    auto* metafile = static_cast<EmfMetafile*>(hmf);
    if (metafile == nullptr || metafile->magic != kMetafileMagic) return 0;
    const UINT total = static_cast<UINT>(metafile->bytes.size());
    // Win32 语义：缓冲区不足（或只想查长度）时返回所需字节数，不写。
    if (lpbBuffer == nullptr || cbBuffer < total) return total;
    std::memcpy(lpbBuffer, metafile->bytes.data(), total);
    return total;
}

// 把一条 GdiComment 序列化成 EMR_GDICOMMENT。Data 是变长尾随数组，cbData 给长度，
// 记录体不参与边界合并（私有标记不画东西，也没有 rclBounds）。
BOOL GdiComment(HDC hdc, UINT cbSize, const BYTE* lpData) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || device->recording == nullptr) return FALSE;
    if (cbSize == 0 || lpData == nullptr) return FALSE;
    const std::size_t recordBytes = sizeof(EMR) + sizeof(DWORD) + static_cast<std::size_t>(cbSize);
    std::uint8_t* base = emfBeginRecord(*device->recording, recordBytes);
    auto* record = reinterpret_cast<EMR*>(base);
    record->iType = EMR_GDICOMMENT;
    record->nSize = static_cast<DWORD>(recordBytes);
    auto* cbData = reinterpret_cast<DWORD*>(base + sizeof(EMR));
    *cbData = cbSize;
    std::memcpy(base + sizeof(EMR) + sizeof(DWORD), lpData, cbSize);
    return TRUE;
}

BOOL EnumEnhMetaFile(HDC hdc, HENHMETAFILE hmf, ENHMFENUMPROC lpProc, LPVOID lpParam,
                     const RECT* lpRect) {
    // lpRect 非空时 Windows 会把记录缩放到该矩形。本层不缩放：缩放会让回调里的
    // cxDest/cyDest 与录制值不同，而所有审计都在比对这两个值。显式失败。
    if (lpRect != nullptr) {
        reportUnsupported("EnumEnhMetaFile", "rectangle rescaling is not supported (pass nullptr)");
        return FALSE;
    }
    if (hmf == nullptr || lpProc == nullptr) return FALSE;
    auto* metafile = static_cast<EmfMetafile*>(hmf);
    if (metafile->magic != kMetafileMagic) return FALSE;

    const std::uint8_t* cursor = metafile->bytes.data();
    const std::uint8_t* end = cursor + metafile->bytes.size();
    HANDLETABLE table{};
    table.objectHandle[0] = nullptr;
    while (end - cursor >= static_cast<std::ptrdiff_t>(sizeof(ENHMETARECORD))) {
        const auto* record = reinterpret_cast<const ENHMETARECORD*>(cursor);
        if (record->nSize < sizeof(ENHMETARECORD) ||
            cursor + record->nSize > end) {
            reportUnsupported("EnumEnhMetaFile", "corrupt record stream");
            return FALSE;
        }
        // 回调返回 0 即停止枚举；与 Win32 一致，此情形整体返回 FALSE。
        if (lpProc(hdc, &table, record, 0, reinterpret_cast<LPARAM>(lpParam)) == 0) return FALSE;
        cursor += record->nSize;
    }
    return TRUE;
}

// 回放 EMR_BITBLT / EMR_STRETCHBLT：从内嵌的 BITMAPINFOHEADER + 位图字节重建源
// 位图，再交给 BitBlt/StretchBlt。源位图数据是录制时 emfRecordBlit 逐行原样
// 拷入的（top-down、stride = width*4），这里整块 memcpy 即得同一内存布局。
static BOOL playEmfBlit(CompatDevice* dest, const EMRBITBLT* record, bool stretch) {
    const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(record);
    if (record->offBmiSrc + record->cbBmiSrc > record->nSize ||
        record->offBitsSrc + record->cbBitsSrc > record->nSize) {
        return FALSE;
    }
    const auto* bmi = reinterpret_cast<const BITMAPINFOHEADER*>(base + record->offBmiSrc);
    if (bmi->biSize < sizeof(BITMAPINFOHEADER) || bmi->biBitCount != 32 ||
        bmi->biCompression != BI_RGB) {
        return FALSE;
    }
    const bool topDown = bmi->biHeight < 0;
    const int width = static_cast<int>(bmi->biWidth);
    const int height = topDown ? static_cast<int>(-bmi->biHeight) : static_cast<int>(bmi->biHeight);
    if (width <= 0 || height <= 0) return FALSE;
    const std::size_t bitsBytes = static_cast<std::size_t>(width) * 4u * static_cast<std::size_t>(height);
    if (record->cbBitsSrc < bitsBytes) return FALSE;

    CompatObject* srcBmp = allocateBitmap(width, height, topDown);
    if (srcBmp == nullptr) return FALSE;
    std::memcpy(srcBmp->pixels, base + record->offBitsSrc, bitsBytes);

    auto* srcDev = new (std::nothrow) CompatDevice();
    if (srcDev == nullptr) {
        releaseObject(srcBmp);
        return FALSE;
    }
    srcDev->bitmap = srcBmp;

    BOOL ok;
    if (stretch) {
        const auto* s = reinterpret_cast<const EMRSTRETCHBLT*>(record);
        ok = StretchBlt(reinterpret_cast<HDC>(dest), record->xDest, record->yDest, record->cxDest,
                        record->cyDest, reinterpret_cast<HDC>(srcDev), record->xSrc, record->ySrc, s->cxSrc,
                        s->cySrc, SRCCOPY);
    } else {
        ok = BitBlt(reinterpret_cast<HDC>(dest), record->xDest, record->yDest, record->cxDest,
                    record->cyDest, reinterpret_cast<HDC>(srcDev), record->xSrc, record->ySrc, SRCCOPY);
    }

    srcDev->bitmap = nullptr;  // 源位图不归 DC 所有，避免任何后续引用
    delete srcDev;
    releaseObject(srcBmp);
    return ok;
}

BOOL PlayEnhMetaFileRecord(HDC hdc, LPHANDLETABLE lpHandles, const ENHMETARECORD* lpEnhMetaRecord,
                           UINT nHandles) {
    (void)lpHandles;
    (void)nHandles;
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || lpEnhMetaRecord == nullptr) return FALSE;
    if (lpEnhMetaRecord->nSize < sizeof(ENHMETARECORD)) return FALSE;

    switch (lpEnhMetaRecord->iType) {
    case EMR_HEADER:
    case EMR_EOF:
    case EMR_GDICOMMENT:
        // 头部/结束/私有标记：不改变像素也不改变 DC 状态。GDICOMMENT 由上层回放
        // 回调单独解析（marker 语义），这里兜底成功即可。
        return TRUE;
    case EMR_BITBLT:
        return playEmfBlit(device, reinterpret_cast<const EMRBITBLT*>(lpEnhMetaRecord), false);
    case EMR_STRETCHBLT:
        return playEmfBlit(device, reinterpret_cast<const EMRBITBLT*>(lpEnhMetaRecord), true);
    default:
        // 录制型 DC 目前只产生 GDICOMMENT + BITBLT/STRETCHBLT（图元/文本/alpha/区域
        // 在录制时就被对应函数显式拒绝）。万一遇到未预期的记录，诚实失败而不是
        // 静默跳过——跳过会让回放结果与录制时不一致，而"看着能跑"正是最危险的错。
        std::fprintf(stderr,
                     "[compat/win32] PlayEnhMetaFileRecord: record type %lu is not replayable\n",
                     static_cast<unsigned long>(lpEnhMetaRecord->iType));
        return FALSE;
    }
}

HGDIOBJ GetStockObject(int i) {
    if (i < 0 || i >= kStockSlots) return nullptr;
    CompatObject* table = stockTable();
    return table[i].magic == kObjectMagic ? toHandle(&table[i]) : nullptr;
}

HBRUSH CreateSolidBrush(COLORREF color) {
    auto* object = new (std::nothrow) CompatObject();
    if (object == nullptr) return nullptr;
    object->kind = ObjectKind::Brush;
    object->color = color;
    ++liveObjectCount;
    return reinterpret_cast<HBRUSH>(object);
}

HPEN CreatePen(int iStyle, int cWidth, COLORREF color) {
    auto* object = new (std::nothrow) CompatObject();
    if (object == nullptr) return nullptr;
    object->kind = ObjectKind::Pen;
    object->penStyle = iStyle;
    object->penWidth = cWidth;
    object->color = color;
    ++liveObjectCount;
    return reinterpret_cast<HPEN>(object);
}

int SaveDC(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return 0;
    CompatSavedState state;
    state.pen = device->pen;
    state.brush = device->brush;
    state.font = device->font;
    state.textColor = device->textColor;
    state.bkColor = device->bkColor;
    state.bkMode = device->bkMode;
    state.polyFillMode = device->polyFillMode;
    state.textAlign = device->textAlign;
    state.position = device->position;
    state.rop2 = device->rop2;
    state.mapMode = device->mapMode;
    state.graphicsMode = device->graphicsMode;
    state.arcDirection = device->arcDirection;
    state.layout = device->layout;
    state.windowOrg = device->windowOrg;
    state.viewportOrg = device->viewportOrg;
    state.windowExt = device->windowExt;
    state.viewportExt = device->viewportExt;
    state.dcBrushColor = device->dcBrushColor;
    state.dcPenColor = device->dcPenColor;
    state.hasClip = device->hasClip;
    state.clip = device->clip;
    device->saved.push_back(state);
    return static_cast<int>(device->saved.size());
}

BOOL RestoreDC(HDC hdc, int nSavedDC) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || device->saved.empty()) return FALSE;
    if (nSavedDC < 0) {
        // 负数表示相对栈顶的位置：-1 即最近一次保存。
        nSavedDC = static_cast<int>(device->saved.size()) + nSavedDC + 1;
    }
    if (nSavedDC < 1 || nSavedDC > static_cast<int>(device->saved.size())) return FALSE;
    const CompatSavedState& state = device->saved[static_cast<std::size_t>(nSavedDC - 1)];
    device->pen = state.pen;
    device->brush = state.brush;
    device->font = state.font;
    device->textColor = state.textColor;
    device->bkColor = state.bkColor;
    device->bkMode = state.bkMode;
    device->polyFillMode = state.polyFillMode;
    device->textAlign = state.textAlign;
    device->position = state.position;
    device->rop2 = state.rop2;
    device->mapMode = state.mapMode;
    device->graphicsMode = state.graphicsMode;
    device->arcDirection = state.arcDirection;
    device->layout = state.layout;
    device->windowOrg = state.windowOrg;
    device->viewportOrg = state.viewportOrg;
    device->windowExt = state.windowExt;
    device->viewportExt = state.viewportExt;
    device->dcBrushColor = state.dcBrushColor;
    device->dcPenColor = state.dcPenColor;
    device->hasClip = state.hasClip;
    device->clip = state.clip;
    device->saved.resize(static_cast<std::size_t>(nSavedDC - 1));
    return TRUE;
}

// ---------------------------------------------------------------------------
// DC 状态
// ---------------------------------------------------------------------------

int SetBkMode(HDC hdc, int mode) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return 0;
    const int previous = device->bkMode;
    device->bkMode = mode;
    return previous;
}

int GetBkMode(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? 0 : device->bkMode;
}

COLORREF SetBkColor(HDC hdc, COLORREF color) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return CLR_INVALID;
    const COLORREF previous = device->bkColor;
    device->bkColor = color;
    return previous;
}

COLORREF GetBkColor(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? CLR_INVALID : device->bkColor;
}

COLORREF SetTextColor(HDC hdc, COLORREF color) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return CLR_INVALID;
    const COLORREF previous = device->textColor;
    device->textColor = color;
    return previous;
}

COLORREF GetTextColor(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? CLR_INVALID : device->textColor;
}

int SetStretchBltMode(HDC hdc, int mode) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return 0;
    const int previous = device->stretchMode;
    device->stretchMode = mode;
    return previous;
}

int GetStretchBltMode(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? 0 : device->stretchMode;
}

int SetPolyFillMode(HDC hdc, int mode) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return 0;
    if (mode != ALTERNATE && mode != WINDING) return 0;  // 非法模式：不改变状态
    const int previous = device->polyFillMode;
    device->polyFillMode = mode;
    return previous;
}

int GetPolyFillMode(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? 0 : device->polyFillMode;
}

int SetICMMode(HDC hdc, int mode) {
    // 本层不实现任何色彩管理：所有 DC 都是裸 32bpp 位图，像素原样进出。
    // 查询恒答 ICM_OFF——这正是 RuntimeTerrainViewportFill / RuntimeRadarPixelStore
    // 在绘制前断言的状态（它们要求"没有色彩管理介入"，否则像素金标准不成立）。
    if (asDevice(hdc) == nullptr) return 0;
    if (mode == ICM_QUERY) return ICM_OFF;
    if (mode == ICM_ON) {
        // 开启色彩管理会让后续像素无法与 Windows 逐位对齐，显式拒绝而不是假装成功。
        std::fprintf(stderr, "[compat/win32] SetICMMode(ICM_ON): colour management is not supported\n");
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 位图传输
// ---------------------------------------------------------------------------

BOOL BitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop) {
    CompatDevice* destDevice = asDevice(hdc);
    if (destDevice == nullptr) return FALSE;
    // 录制型 DC 没有像素表面：走记录分支，而不是"没有位图就失败"。
    if (destDevice->recording != nullptr) {
        return emfRecordBlit(*destDevice, false, rop, x, y, cx, cy, hdcSrc, x1, y1, cx, cy);
    }
    if (destDevice->bitmap == nullptr) return FALSE;
    if (!isSupportedBlitRop(rop)) {
        // 显式失败：静默当作 SRCCOPY 会造成"画面像对、合成全错"。
        std::fprintf(stderr, "[compat/win32] BitBlt: unsupported raster operation=0x%08lX\n",
                     static_cast<unsigned long>(rop));
        return FALSE;
    }
    CompatObject& dest = *destDevice->bitmap;
    CompatDevice* sourceDevice = asDevice(hdcSrc);
    CompatObject* source = sourceDevice == nullptr ? nullptr : sourceDevice->bitmap;
    const bool needsSource = rop != DSTINVERT && rop != BLACKNESS && rop != WHITENESS;
    if (needsSource && source == nullptr) return FALSE;

    IntRect target;
    target.left = x;
    target.top = y;
    target.right = x + cx;
    target.bottom = y + cy;
    if (cx < 0) {
        target.left = x + cx;
        target.right = x;
    }
    if (cy < 0) {
        target.top = y + cy;
        target.bottom = y;
    }
    IntRect clipped = intersect(target, IntRect{0, 0, dest.width, dest.height});
    if (isEmpty(clipped)) return TRUE;

    // 源起点随裁剪同步平移，保证裁剪后像素仍一一对应。
    int sourceX = x1 + (clipped.left - target.left);
    int sourceY = y1 + (clipped.top - target.top);
    int width = clipped.right - clipped.left;
    int height = clipped.bottom - clipped.top;

    if (source != nullptr) {
        if (!clipSpan(sourceX, width, source->width)) return TRUE;
        if (!clipSpan(sourceY, height, source->height)) return TRUE;
        clipped.right = clipped.left + width;
        clipped.bottom = clipped.top + height;
    }

    if (source == &dest && rop == SRCCOPY) {
        rasterSelfCopy(dest, clipped, sourceX, sourceY);
        return TRUE;
    }
    rasterBlock(dest, clipped, needsSource ? source : nullptr, sourceX, sourceY, rop);
    return TRUE;
}

BOOL StretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest, HDC hdcSrc, int xSrc, int ySrc,
                int wSrc, int hSrc, DWORD rop) {
    CompatDevice* destDevice = asDevice(hdcDest);
    if (destDevice == nullptr) return FALSE;
    if (destDevice->recording == nullptr && destDevice->bitmap == nullptr) return FALSE;
    if (rop != SRCCOPY && rop != DSTINVERT) {
        std::fprintf(stderr, "[compat/win32] StretchBlt: unsupported raster operation=0x%08lX\n",
                     static_cast<unsigned long>(rop));
        return FALSE;
    }
    if (destDevice->recording != nullptr) {
        // 记录放在 HALFTONE 判定之前：EMR_STRETCHBLT 不带伸缩模式字段，若在这里
        // 放行 HALFTONE，回放时就是一次静默的最近邻放大——画面看起来"能出来"，
        // 但和录制时想要的加权平均不是一回事。故 HALFTONE 先失败。
        if (destDevice->stretchMode == HALFTONE) {
            reportUnsupported("StretchBlt", "HALFTONE stretch mode cannot be recorded");
            return FALSE;
        }
        return emfRecordBlit(*destDevice, true, rop, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc,
                             hSrc);
    }
    CompatDevice* sourceDevice = asDevice(hdcSrc);
    CompatObject* source = sourceDevice == nullptr ? nullptr : sourceDevice->bitmap;
    if (source == nullptr) return FALSE;
    if (wDest <= 0 || hDest <= 0 || wSrc == 0 || hSrc == 0) return FALSE;
    if (destDevice->stretchMode == HALFTONE) {
        // HALFTONE 需要加权平均/抖动，本层不实现；宁可失败也不假装成功。
        reportUnsupported("StretchBlt", "HALFTONE stretch mode is not supported");
        return FALSE;
    }

    CompatObject& dest = *destDevice->bitmap;
    // 最近邻：Windows 的 COLORONCOLOR 语义就是"删像素"，正是本层实现的模式。
    for (int row = 0; row < hDest; ++row) {
        const int targetY = yDest + row;
        if (targetY < 0 || targetY >= dest.height) continue;
        const int sourceY = ySrc + (row * hSrc) / hDest;
        if (sourceY < 0 || sourceY >= source->height) continue;
        for (int column = 0; column < wDest; ++column) {
            const int targetX = xDest + column;
            if (targetX < 0 || targetX >= dest.width) continue;
            const int sourceX = xSrc + (column * wSrc) / wDest;
            if (sourceX < 0 || sourceX >= source->width) continue;
            if (rop == SRCCOPY) {
                writePixel(dest, targetX, targetY, readPixel(*source, sourceX, sourceY));
            } else {
                writePixel(dest, targetX, targetY, invertRgb(readPixel(dest, targetX, targetY)));
            }
        }
    }
    return TRUE;
}

BOOL PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || device->bitmap == nullptr) return FALSE;
    if (rop != BLACKNESS && rop != WHITENESS && rop != DSTINVERT && rop != PATCOPY) {
        std::fprintf(stderr, "[compat/win32] PatBlt: unsupported raster operation=0x%08lX\n",
                     static_cast<unsigned long>(rop));
        return FALSE;
    }
    CompatObject& dest = *device->bitmap;

    std::uint32_t value = 0;
    if (rop == BLACKNESS) {
        value = 0xFF000000u;
    } else if (rop == WHITENESS) {
        value = 0xFFFFFFFFu;
    } else if (rop == PATCOPY) {
        const CompatObject* brush = asObject(device->brush);
        if (brush == nullptr || brush->hollow) return FALSE;
        value = toPixel(brush->color);
    }

    IntRect target;
    target.left = x;
    target.top = y;
    target.right = x + w;
    target.bottom = y + h;
    if (w < 0) {
        target.left = x + w;
        target.right = x;
    }
    if (h < 0) {
        target.top = y + h;
        target.bottom = y;
    }
    const IntRect clipped = intersect(target, IntRect{0, 0, dest.width, dest.height});
    if (isEmpty(clipped)) return TRUE;

    for (int row = clipped.top; row < clipped.bottom; ++row) {
        auto* pixels = reinterpret_cast<std::uint32_t*>(rowPointer(dest, row) + clipped.left * 4);
        for (int column = 0; column < clipped.right - clipped.left; ++column) {
            pixels[column] = (rop == DSTINVERT) ? invertRgb(pixels[column]) : value;
        }
    }
    return TRUE;
}

COLORREF GetPixel(HDC hdc, int x, int y) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || device->bitmap == nullptr) return CLR_INVALID;
    CompatObject& bitmap = *device->bitmap;
    if (x < 0 || y < 0 || x >= bitmap.width || y >= bitmap.height) return CLR_INVALID;
    return toColorRef(readPixel(bitmap, x, y));
}

COLORREF SetPixelV(HDC hdc, int x, int y, COLORREF color) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || device->bitmap == nullptr) return CLR_INVALID;
    CompatObject& bitmap = *device->bitmap;
    if (x < 0 || y < 0 || x >= bitmap.width || y >= bitmap.height) return CLR_INVALID;
    writePixel(bitmap, x, y, toPixel(color));
    return color;
}

COLORREF SetPixel(HDC hdc, int x, int y, COLORREF color) {
    return SetPixelV(hdc, x, y, color);
}

int StretchDIBits(HDC hdc, int xDest, int yDest, int DestWidth, int DestHeight, int xSrc, int ySrc, int SrcWidth,
                  int SrcHeight, const void* lpBits, const BITMAPINFO* lpbmi, UINT iUsage, DWORD rop) {
    if (iUsage != DIB_RGB_COLORS) {
        reportUnsupportedLong("StretchDIBits", "usage", static_cast<long>(iUsage));
        return static_cast<int>(GDI_ERROR);
    }
    if (rop != SRCCOPY) {
        std::fprintf(stderr, "[compat/win32] StretchDIBits: unsupported raster operation=0x%08lX\n",
                     static_cast<unsigned long>(rop));
        return static_cast<int>(GDI_ERROR);
    }
    const int lines = blitExternalDib(hdc, xDest, yDest, DestWidth, DestHeight, lpBits, lpbmi, xSrc, ySrc, SrcWidth,
                                      SrcHeight, true);
    // 调用方以 `!= GDI_ERROR` 判成功（RuntimeInternetMenuSkin.cpp:318），因此成功
    // 必须返回扫描线数；一条都没写也算失败。
    return lines > 0 ? lines : static_cast<int>(GDI_ERROR);
}

int SetDIBitsToDevice(HDC hdc, int xDest, int yDest, DWORD w, DWORD h, int xSrc, int ySrc, UINT StartScan,
                      UINT cLines, const void* lpvBits, const BITMAPINFO* lpbmi, UINT ColorUse) {
    // StartScan / cLines 用于"分带传输单个 DIB 的一段行"，本项目只传整图，
    // 且调用点传的是 0 / 全部行，故此处忽略并把 lpvBits 视为完整位图首地址。
    (void)StartScan;
    (void)cLines;
    if (ColorUse != DIB_RGB_COLORS) {
        reportUnsupportedLong("SetDIBitsToDevice", "usage", static_cast<long>(ColorUse));
        return 0;
    }
    return blitExternalDib(hdc, xDest, yDest, static_cast<int>(w), static_cast<int>(h), lpvBits, lpbmi, xSrc, ySrc,
                           static_cast<int>(w), static_cast<int>(h), false);
}

// ---------------------------------------------------------------------------
// DC 状态与查询（S3-1，A 段 25 个符号）
// ---------------------------------------------------------------------------
//
// 这一族里没有一个函数会画像素，但它们决定"像素路径走不走快路"。三条快路径
// （RuntimeSelectedPathRaster::supported / RuntimeSelectionFrameRaster 构造函数 /
// RuntimeRadarPixelStore::isStandardDib）的做法是：先在 DC 上逐项比对状态，
// 任何一项不符就**静默退回**逐图元的慢路径。所以这里最危险的不是崩溃，而是
// 默认值给错——AD_COUNTERCLOCKWISE 是 1（不是 0）、SIMPLEREGION 是 2、
// GM_COMPATIBLE 是 1、R2_COPYPEN 是 13。随手 return 0 会让快路径永远关闭，
// 而现有契约全部"通过"（它们只比较快慢两路是否一致）。因此：
//   * 每个 Get 的初值都按 Win32 文档钉死，并在 selftest 里用**字面量**断言
//     （不是拿常量自比），这样头文件把常量写错时也会被抓到。
//   * 每个 Set 都返回"前一个值"而不是 TRUE —— 调用点普遍写成
//     `const auto old = SetX(...)` 然后还原，返回布尔会让还原变成噪声。

int GetObjectW(HGDIOBJ h, int c, LPVOID pv) {
    CompatObject* object = asObject(h);
    if (object == nullptr || pv == nullptr || c <= 0) return 0;
    switch (object->kind) {
        case ObjectKind::Bitmap: {
            if (c < static_cast<int>(sizeof(BITMAP))) return 0;
            BITMAP bitmap{};
            bitmap.bmType = 0;
            bitmap.bmWidth = object->width;
            // 与文档的已知分歧（见 S3-诊断报告 第 2 节）：top-down DIB 的
            // bmHeight 按**正数**给出。项目里有三处按"GetObject 会把高度归一
            // 成正数"写死（`dib.dsBm.bmHeight != height`，height>0），且这些
            // 断言在 Windows 上是绿的——所以这里迁就实测语义，而不是迁就
            // 文档字面。dsBmih.biHeight 反过来保留创建时的符号（调用点对
            // 正负两解都接受），两条规则分开钉在 selftest 里。
            bitmap.bmHeight = object->height;
            bitmap.bmWidthBytes = object->stride;
            bitmap.bmPlanes = 1;
            bitmap.bmBitsPixel = 32;
            // DDB 没有可寻址像素数组：Win32 文档明写 bmBits 为 NULL。本层内部
            // 两种位图共用一块内存，但对外必须区分，否则
            // CampaignDeviceBitmapContract 的 `bmBits==nullptr` 判据失效。
            bitmap.bmBits = object->dibSection ? object->pixels : nullptr;
            if (!object->dibSection || c < static_cast<int>(sizeof(DIBSECTION))) {
                std::memcpy(pv, &bitmap, sizeof(BITMAP));
                return static_cast<int>(sizeof(BITMAP));
            }
            DIBSECTION section{};
            section.dsBm = bitmap;
            section.dsBmih = object->header;
            section.dsBitfields[0] = 0;
            section.dsBitfields[1] = 0;
            section.dsBitfields[2] = 0;
            section.dshSection = nullptr;
            section.dsOffset = 0;
            std::memcpy(pv, &section, sizeof(DIBSECTION));
            return static_cast<int>(sizeof(DIBSECTION));
        }
        case ObjectKind::Pen: {
            if (c < static_cast<int>(sizeof(LOGPEN))) return 0;
            LOGPEN logpen{};
            logpen.lopnStyle = static_cast<UINT>(object->penStyle);
            logpen.lopnWidth.x = object->penWidth;
            logpen.lopnWidth.y = 0;  // 几何笔的两个方向宽度；本层只造化妆笔
            logpen.lopnColor = object->color;
            std::memcpy(pv, &logpen, sizeof(LOGPEN));
            return static_cast<int>(sizeof(LOGPEN));
        }
        case ObjectKind::Brush: {
            if (c < static_cast<int>(sizeof(LOGBRUSH))) return 0;
            LOGBRUSH logbrush{};
            logbrush.lbStyle = object->hollow ? BS_NULL : BS_SOLID;
            logbrush.lbColor = object->color;
            logbrush.lbHatch = 0;
            std::memcpy(pv, &logbrush, sizeof(LOGBRUSH));
            return static_cast<int>(sizeof(LOGBRUSH));
        }
        case ObjectKind::Font:
            // P5-06：CreateFontW 已把入参完整存进 fontLog，GetObject 如实回填。
            // 库存字体（DEFAULT_GUI_FONT 等）的 fontLog 是零值——此时 lfHeight==0，
            // 调用点若拿它当真实字高会得到"0 像素"；但库存字体本就无参数，如实
            // 回填零值 LOGFONT 比返回 0（失败）更接近 Win32（Win32 的 GetObject
            // 对库存字体同样返回一个系统定义的 LOGFONT）。
            if (c < static_cast<int>(sizeof(LOGFONTW))) return 0;
            std::memcpy(pv, &object->fontLog, sizeof(LOGFONTW));
            return static_cast<int>(sizeof(LOGFONTW));
        case ObjectKind::Region:
            // Win32 的 GetObject 不处理区域（要读区域内容请用 GetRegionData），
            // 本层同样返回 0，而不是把条带表 memcpy 出去。
            return 0;
    }
    return 0;
}

DWORD GetObjectType(HGDIOBJ h) {
    const CompatObject* object = asObject(h);
    if (object == nullptr) return 0;
    switch (object->kind) {
        case ObjectKind::Bitmap: return OBJ_BITMAP;
        case ObjectKind::Pen: return OBJ_PEN;
        case ObjectKind::Brush: return OBJ_BRUSH;
        case ObjectKind::Font: return OBJ_FONT;
        case ObjectKind::Region: return OBJ_REGION;
    }
    return 0;
}

HGDIOBJ GetCurrentObject(HDC hdc, UINT type) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return nullptr;
    switch (type) {
        case OBJ_BITMAP: return toHandle(device->bitmap);
        case OBJ_PEN: return device->pen;
        case OBJ_BRUSH: return device->brush;
        case OBJ_FONT: return device->font;
        default: return nullptr;
    }
}

int GetClipBox(HDC hdc, LPRECT lprect) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || lprect == nullptr) return ERROR;
    if (device->bitmap == nullptr) {
        // 录制型 DC（或还没选位图的内存 DC）没有像素表面，裁剪框应当来自
        // CreateEnhMetaFile 的 rcFrame。本层没保存那个矩形，所以给 ERROR：
        // 0 与 SIMPLEREGION/NULLREGION 都不等，调用方一律走"不支持"分支，
        // 这正是想要的。绝不返回一个编出来的矩形。
        if (device->recording != nullptr) {
            reportUnsupported("GetClipBox", "recording DC has no pixel surface (frame rect not recorded)");
        }
        return ERROR;
    }
    RegionBands visible;
    if (!visibleClipBands(*device, visible)) {
        reportUnsupported("GetClipBox", "clip region could not be intersected with the surface");
        return ERROR;
    }
    const IntRect bounds = regionBounds(visible);
    lprect->left = bounds.left;
    lprect->top = bounds.top;
    lprect->right = bounds.right;
    lprect->bottom = bounds.bottom;
    // 三态是**真实语义**，不再是"恒 SIMPLEREGION"：
    //   NULLREGION(1)    ↔ 裁剪把整块画面裁空（IntersectClipRect(dc,0,0,0,0)）
    //   SIMPLEREGION(2)  ↔ 可见部分是一个矩形（无裁剪、或矩形裁剪）
    //   COMPLEXREGION(3) ↔ 可见部分由多块组成（RGN_OR 出来的不连通区域）
    // 第三条是三条快路径的硬门槛：RuntimeRadarPixelStore / RuntimeTerrainViewportFill
    // 判 `!= SIMPLEREGION` 就退让。若这里仍恒答简单区域，它们会在复杂裁剪下
    // 继续直接写像素——绕过裁剪、画面越界，而所有"快慢两路一致"的契约照样全绿。
    return regionComplexity(visible);
}

int GetROP2(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? 0 : device->rop2;
}

int SetROP2(HDC hdc, int rop2) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || rop2 < R2_BLACK || rop2 > R2_WHITE) return 0;
    const int previous = device->rop2;
    device->rop2 = rop2;
    return previous;
}

int GetMapMode(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? 0 : device->mapMode;
}

int SetMapMode(HDC hdc, int iMode) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || iMode < MM_TEXT || iMode > MM_ANISOTROPIC) return 0;
    const int previous = device->mapMode;
    device->mapMode = iMode;
    return previous;
}

int GetGraphicsMode(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? 0 : device->graphicsMode;
}

int SetGraphicsMode(HDC hdc, int iMode) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || (iMode != GM_COMPATIBLE && iMode != GM_ADVANCED)) return 0;
    const int previous = device->graphicsMode;
    device->graphicsMode = iMode;
    return previous;
}

int GetArcDirection(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? 0 : device->arcDirection;
}

DWORD GetLayout(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? GDI_ERROR : device->layout;
}

DWORD SetLayout(HDC hdc, DWORD dwLayout) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return GDI_ERROR;
    // LAYOUT_LTR 是 0，所以"同时选 LTR 和 RTL"在数值上根本不存在（结果等于
    // 只选 RTL）。真正需要拒绝的是**未知位**：Win32 对非法标志返 GDI_ERROR，
    // 而静默接受会让后续绘制在"以为自己按 RTL 排版"和"实际没生效"之间漂移。
    const DWORD known = static_cast<DWORD>(LAYOUT_LTR | LAYOUT_RTL | LAYOUT_BITMAPORIENTATIONPRESERVED);
    if ((dwLayout & ~known) != 0) return GDI_ERROR;
    const DWORD previous = device->layout;
    device->layout = dwLayout;
    return previous;
}

BOOL GetViewportOrgEx(HDC hdc, LPPOINT lppt) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || lppt == nullptr) return FALSE;
    *lppt = device->viewportOrg;
    return TRUE;
}

BOOL SetViewportOrgEx(HDC hdc, int x, int y, LPPOINT lppt) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return FALSE;
    // Win32 语义：lppt 收的是**旧**原点，不是新原点。写反了调用方在
    // "存旧值→改→还原"的用法里会把自己恢复成刚设的值，等于永不还原。
    if (lppt != nullptr) *lppt = device->viewportOrg;
    device->viewportOrg.x = x;
    device->viewportOrg.y = y;
    return TRUE;
}

BOOL GetWindowOrgEx(HDC hdc, LPPOINT lppt) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || lppt == nullptr) return FALSE;
    *lppt = device->windowOrg;
    return TRUE;
}

BOOL SetWindowOrgEx(HDC hdc, int x, int y, LPPOINT lppt) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return FALSE;
    if (lppt != nullptr) *lppt = device->windowOrg;
    device->windowOrg.x = x;
    device->windowOrg.y = y;
    return TRUE;
}

BOOL GetViewportExtEx(HDC hdc, LPSIZE lpsz) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || lpsz == nullptr) return FALSE;
    *lpsz = device->viewportExt;
    return TRUE;
}

BOOL SetViewportExtEx(HDC hdc, int x, int y, LPSIZE lpsz) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || x == 0 || y == 0) return FALSE;
    if (lpsz != nullptr) *lpsz = device->viewportExt;
    device->viewportExt.cx = x;
    device->viewportExt.cy = y;
    return TRUE;
}

BOOL GetWindowExtEx(HDC hdc, LPSIZE lpsz) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || lpsz == nullptr) return FALSE;
    *lpsz = device->windowExt;
    return TRUE;
}

BOOL SetWindowExtEx(HDC hdc, int x, int y, LPSIZE lpsz) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || x == 0 || y == 0) return FALSE;
    if (lpsz != nullptr) *lpsz = device->windowExt;
    device->windowExt.cx = x;
    device->windowExt.cy = y;
    return TRUE;
}

COLORREF GetDCBrushColor(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? CLR_INVALID : device->dcBrushColor;
}

COLORREF SetDCBrushColor(HDC hdc, COLORREF color) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return CLR_INVALID;
    const COLORREF previous = device->dcBrushColor;
    device->dcBrushColor = color;
    return previous;
}

COLORREF GetDCPenColor(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? CLR_INVALID : device->dcPenColor;
}

COLORREF SetDCPenColor(HDC hdc, COLORREF color) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return CLR_INVALID;
    const COLORREF previous = device->dcPenColor;
    device->dcPenColor = color;
    return previous;
}

int MulDiv(int nNumber, int nNumerator, int nDenominator) {
    if (nDenominator == 0) return -1;  // Win32 返回 -1 并置 ERROR_DIVIDE_BY_ZERO
    // 中间结果必须走 64 位。项目里的用法是 `rect.left + MulDiv(localX, width, 60)`
    // 这类坐标换算，32 位中间量在 2048 宽的画布上就会溢出。
    std::int64_t result = static_cast<std::int64_t>(nNumber) * static_cast<std::int64_t>(nNumerator);
    // Win32 的四舍五入：向最近整数、半数离开零。用 ±denominator/2 实现时
    // 奇数除数的那一半会被截断，这是 Win32 本身的行为，不是本层的简化。
    result += result >= 0 ? nDenominator / 2 : -(nDenominator / 2);
    return static_cast<int>(result / nDenominator);
}

int GetDeviceCaps(HDC hdc, int index) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return 0;
    const CompatObject* bitmap = device->bitmap;
    switch (index) {
    case HORZRES: return bitmap == nullptr ? 0 : bitmap->width;
    case VERTRES: return bitmap == nullptr ? 0 : bitmap->height;
    // 物理尺寸按 96 DPI 参考设备换算（1 英寸 = 96 像素 = 254 毫米/10）。
    // 录制型 DC 没有像素表面，尺寸诚实返回 0——"无表面即无尺寸"。
    case HORZSIZE: return bitmap == nullptr ? 0 : MulDiv(bitmap->width, 254, 960);
    case VERTSIZE: return bitmap == nullptr ? 0 : MulDiv(bitmap->height, 254, 960);
    default: return 0;  // 未支持的查询：Win32 对未知 index 同样返回 0
    }
}

BOOL GdiFlush() {
    // 本层所有绘制都是同步写光栅内存，不存在批处理队列，因此"冲刷"没有
    // 待办事项。返回 TRUE 是诚实的：调用方拿它当"此前所有 GDI 写入都已落到
    // 我能直接读的那块内存里"，这个契约在这里恒成立。
    return TRUE;
}

DWORD GdiSetBatchLimit(DWORD dwLimit) {
    // 批处理本身不存在（见 GdiFlush），但**必须真的存取**这个值：调用点
    // （RuntimeFlakPresentationSmoke）的写法是
    // `{DWORD limit; ~RestoreGdiBatch(){GdiSetBatchLimit(limit);}} b{GdiSetBatchLimit(1)}`，
    // 也就是靠返回值把旧上限存下来。恒返回同一个常数会让还原变成空操作，
    // 而且测不出来——只有"设 1 → 返回 20 → 还原成 20"这种往返才验得出来。
    static DWORD batchLimit = 20;  // Win32 文档给的默认上限
    const DWORD previous = batchLimit;
    batchLimit = dwLimit;
    return previous;
}

BOOL GetCurrentPositionEx(HDC hdc, LPPOINT lpPoint) {
    // C 段（MoveToEx/LineTo）的读一半，提前到这里实现：它只读已有状态，
    // 而光栅契约在画任何东西之前就要用它比较两条路径的当前位置。
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || lpPoint == nullptr) return FALSE;
    *lpPoint = device->position;
    return TRUE;
}

// ---------------------------------------------------------------------------
// 区域对象与裁剪（S3-2，B 段 5 个符号）
// ---------------------------------------------------------------------------
//
// 这一族把 S3-1 的"裁剪只有矩形"升级成真正的区域：矩形 / 多边形 / 布尔组合。
// 它对产品的影响不是"多画了点东西"，而是让一条判据重新有分辨力：复杂裁剪
// 过去恒被报成 SIMPLEREGION，三条快路径于是照走不误。

inline CompatObject* asRegion(HRGN handle) {
    CompatObject* object = asObject(reinterpret_cast<HGDIOBJ>(handle));
    if (object == nullptr || object->kind != ObjectKind::Region || !object->liveRegion) return nullptr;
    return object;
}

HRGN CreateRectRgn(int x1, int y1, int x2, int y2) {
    auto* object = new (std::nothrow) CompatObject();
    if (object == nullptr) return nullptr;
    object->kind = ObjectKind::Region;
    object->liveRegion = true;
    setRectBands(object->bands, x1, y1, x2, y2);
    ++liveObjectCount;
    return reinterpret_cast<HRGN>(object);
}

HRGN CreateRectRgnIndirect(const RECT* lprect) {
    if (lprect == nullptr) return nullptr;
    return CreateRectRgn(lprect->left, lprect->top, lprect->right, lprect->bottom);
}

HRGN CreatePolygonRgn(const POINT* pptl, int cPoint, int iMode) {
    RegionBands bands;
    if (!polygonBands(pptl, cPoint, iMode, bands)) {
        reportUnsupportedLong("CreatePolygonRgn", "fill mode", static_cast<long>(iMode));
        return nullptr;
    }
    auto* object = new (std::nothrow) CompatObject();
    if (object == nullptr) return nullptr;
    object->kind = ObjectKind::Region;
    object->liveRegion = true;
    object->bands = std::move(bands);
    ++liveObjectCount;
    return reinterpret_cast<HRGN>(object);
}

HRGN CreateEllipticRgn(int x1, int y1, int x2, int y2) {
    // S3-3 兑现。之前显式失败的理由是"拿矩形近似会让四角多放行像素，而调用方
    // 两边用同一个近似，看起来一致"——现在逐行条带由 ellipseRowSpan 给出，与
    // Ellipse() 的填充/描边共用同一个几何函数，三处不会各差一像素。
    if (x2 <= x1 || y2 <= y1) {
        // 与矩形区域一致：右下不大于左上 = 空区域，不是失败。
        return CreateRectRgn(x1, y1, x2, y2);
    }
    if (y2 - y1 > kMaxRegionRows) {
        reportUnsupportedLong("CreateEllipticRgn", "height exceeds region row limit", y2 - y1);
        return nullptr;
    }
    std::vector<RowSpans> rows(static_cast<std::size_t>(y2 - y1));
    for (int y = y1; y < y2; ++y) {
        RegionSpan span{0, 0};
        if (!ellipseRowSpan(x1, y1, x2, y2, y, span)) continue;
        rows[static_cast<std::size_t>(y - y1)].push_back(span);
    }
    auto* object = new (std::nothrow) CompatObject();
    if (object == nullptr) return nullptr;
    object->kind = ObjectKind::Region;
    object->liveRegion = true;
    object->bands = compressRows(rows, y1);
    ++liveObjectCount;
    return reinterpret_cast<HRGN>(object);
}

BOOL SetRectRgn(HRGN hrgn, int left, int top, int right, int bottom) {
    CompatObject* region = asRegion(hrgn);
    if (region == nullptr) return FALSE;
    setRectBands(region->bands, left, top, right, bottom);
    return TRUE;
}

int CombineRgn(HRGN hrgnDst, HRGN hrgnSrc1, HRGN hrgnSrc2, int iMode) {
    CompatObject* destination = asRegion(hrgnDst);
    if (destination == nullptr) return RGN_ERROR;
    if (iMode == RGN_COPY) {
        // RGN_COPY 拷的是 hrgnSrc1（MSDN 原文），不是 src2。
        const CompatObject* source = asRegion(hrgnSrc1);
        if (source == nullptr) return RGN_ERROR;
        RegionBands copy = source->bands;  // 先落临时表：允许 dst == src
        destination->bands = std::move(copy);
        return regionComplexity(destination->bands);
    }
    const CompatObject* left = asRegion(hrgnSrc1);
    const CompatObject* right = asRegion(hrgnSrc2);
    if (left == nullptr || right == nullptr) return RGN_ERROR;
    // 调用点普遍写成 CombineRgn(a, a, b, ...)（RuntimeShell 的时钟遮罩、
    // 两个 selection 契约都是），所以必须先算到临时表再落 dst。
    RegionBands result;
    if (!combineRegions(left->bands, right->bands, iMode, result)) {
        reportUnsupportedLong("CombineRgn", "combine mode", static_cast<long>(iMode));
        return RGN_ERROR;
    }
    destination->bands = std::move(result);
    return regionComplexity(destination->bands);
}

int GetRgnBox(HRGN hrgn, LPRECT lprc) {
    const CompatObject* region = asRegion(hrgn);
    if (region == nullptr || lprc == nullptr) return RGN_ERROR;
    const IntRect bounds = regionBounds(region->bands);
    lprc->left = bounds.left;
    lprc->top = bounds.top;
    lprc->right = bounds.right;
    lprc->bottom = bounds.bottom;
    return regionComplexity(region->bands);
}

BOOL PtInRegion(HRGN hrgn, int x, int y) {
    const CompatObject* region = asRegion(hrgn);
    if (region == nullptr) return FALSE;
    return regionContainsPoint(region->bands, x, y) ? TRUE : FALSE;
}

BOOL PtVisible(HDC hdc, int x, int y) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return FALSE;
    // 无裁剪区 = 整个表面都可见。有裁剪区则判点：NULLREGION（clip 为空）自然全不可见。
    if (!device->hasClip) return TRUE;
    return regionContainsPoint(device->clip, x, y) ? TRUE : FALSE;
}

BOOL RectInRegion(HRGN hrgn, const RECT* lprc) {
    // 文档语义是"矩形的**任何**部分落在区域内即为真"（不是"整个矩形在内"）。
    // 因此用交集非空判定；空矩形没有任何部分，返回 FALSE。
    const CompatObject* region = asRegion(hrgn);
    if (region == nullptr || lprc == nullptr) return FALSE;
    RegionBands frame;
    setRectBands(frame, lprc->left, lprc->top, lprc->right, lprc->bottom);
    if (frame.empty()) return FALSE;
    RegionBands overlap;
    if (!combineRegions(frame, region->bands, RGN_AND, overlap)) return FALSE;
    return overlap.empty() ? FALSE : TRUE;
}

int SelectClipRgn(HDC hdc, HRGN hrgn) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return ERROR;
    if (hrgn == nullptr) {
        // 重置为"没有 clip region"。这与"选了一个空区域"不是一回事：空区域
        // 会把整块画面裁空（见 GetClipBox 的 NULLREGION）。
        device->hasClip = false;
        device->clip.clear();
        return NULLREGION;
    }
    const CompatObject* region = asRegion(hrgn);
    if (region == nullptr) return ERROR;
    // **深拷贝**。调用点普遍是"选中后立刻 DeleteObject"：
    // RuntimeSelectionFrameRasterContract:170 与 RuntimeShell 的时钟遮罩都是
    // 先 SelectClipRgn 再删区域。若这里存指针，下一次绘制读到已释放内存，
    // 表现是"偶尔走快路、偶尔花屏"，而任何自洽断言都发现不了。
    device->clip = region->bands;
    device->hasClip = true;
    if (device->bitmap == nullptr) return regionComplexity(device->clip);
    RegionBands visible;
    if (!visibleClipBands(*device, visible)) return ERROR;
    return regionComplexity(visible);
}

int GetClipRgn(HDC hdc, HRGN hrgn) {
    CompatDevice* device = asDevice(hdc);
    CompatObject* region = asRegion(hrgn);
    if (device == nullptr || region == nullptr) return -1;  // Win32: -1 表示失败
    if (!device->hasClip) return 0;                         // 没有 clip region，hrgn 保持不变
    RegionBands copy = device->clip;
    region->bands = std::move(copy);
    return regionComplexity(region->bands);
}

int IntersectClipRect(HDC hdc, int left, int top, int right, int bottom) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return ERROR;
    RegionBands rect;
    setRectBands(rect, left, top, right, bottom);
    if (!device->hasClip) {
        device->clip = rect;
        device->hasClip = true;
    } else {
        RegionBands result;
        if (!combineRegions(device->clip, rect, RGN_AND, result)) {
            reportUnsupported("IntersectClipRect", "clip region could not be intersected");
            return ERROR;
        }
        device->clip = std::move(result);
    }
    // 返回值是**新的可见裁剪区**复杂度，与 GetClipBox 给出的答案必须一致；
    // 两处若不一致，调用点会按返回值选路、按 GetClipBox 判状态，自相矛盾。
    if (device->bitmap == nullptr) return regionComplexity(device->clip);
    RegionBands visible;
    if (!visibleClipBands(*device, visible)) return ERROR;
    return regionComplexity(visible);
}

int ExcludeClipRect(HDC hdc, int left, int top, int right, int bottom) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return ERROR;
    RegionBands rect;
    setRectBands(rect, left, top, right, bottom);
    if (!device->hasClip) {
        // 等价语义：从整块表面里挖掉这块矩形，所以先把表面装进 clip。
        if (device->bitmap == nullptr) {
            reportUnsupported("ExcludeClipRect", "no surface to exclude from");
            return ERROR;
        }
        setRectBands(device->clip, 0, 0, device->bitmap->width, device->bitmap->height);
        device->hasClip = true;
    }
    RegionBands result;
    if (!combineRegions(device->clip, rect, RGN_DIFF, result)) {
        reportUnsupported("ExcludeClipRect", "clip region could not be reduced");
        return ERROR;
    }
    device->clip = std::move(result);
    if (device->bitmap == nullptr) return regionComplexity(device->clip);
    RegionBands visible;
    if (!visibleClipBands(*device, visible)) return ERROR;
    return regionComplexity(visible);
}

// ---------------------------------------------------------------------------
// 图元光栅（S3-3，C 段 11 个符号）
// ---------------------------------------------------------------------------
//
// 这一族补上之后，"画东西"第一次在 mac 上真的改变了像素。此前所有需要绘制的
// 路径（选择框、时钟遮罩、雷达标记、菜单边框）要么链接失败，要么什么都不画。
//
// 共同约定（每一条都有对应断言，见 CompatDrawSemantics.cpp）：
//   * 矩形类图元的 [left,right)×[top,bottom) 是**右下半开**：right/bottom
//     那一列/行不属于这个矩形。GDI 本身也是这样（Rectangle 的右下边框画在
//     right-1 / bottom-1 上），但很多调用方按闭区间理解，差异正好一格。
//   * LineTo / Polyline **不画终点**；MoveToEx 不改绘制状态，只改当前位置。
//   * 当前位置存**逻辑坐标**（GetCurrentPositionEx 的契约如此），映射在绘制时做。
//   * 所有像素写入经过 PixelSink，裁剪与 ROP2 只在那里实现一次。

BOOL MoveToEx(HDC hdc, int x, int y, LPPOINT lppt) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return FALSE;
    if (lppt != nullptr) {
        // Win32 语义：传出的是**移动前**的位置。写成移动后的位置是最常见的
        // 抄错方式，而且只有"连续两次 MoveToEx"才能发现。
        *lppt = device->position;
    }
    device->position = POINT{x, y};
    return TRUE;
}

BOOL LineTo(HDC hdc, int x, int y) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return FALSE;
    if (device->bitmap == nullptr) {
        reportUnsupported("LineTo", "recording DC has no pixel surface (EMR_LINETO is not recorded)");
        return FALSE;
    }
    if (!isSupportedRop2(device->rop2)) {
        reportUnsupportedLong("LineTo", "raster operation mode", device->rop2);
        return FALSE;
    }
    POINT from{};
    POINT to{};
    if (!mapPoint(*device, device->position.x, device->position.y, from)) return FALSE;
    if (!mapPoint(*device, x, y, to)) return FALSE;
    // 位置先更新：即使笔是 PS_NULL（不画），当前位置也必须前进——GDI 的
    // 当前位置是绘制状态而不是绘制结果。
    device->position = POINT{x, y};
    const PenState pen = resolvePen(*device);
    if (pen.present) strokeLine(*device, from, to, true, false, pen, device->rop2);
    return TRUE;
}

// FillRect / FrameRect / Rectangle 共用的矩形落位：映射 + 与表面求交。
// clipping 只在 PixelSink 里做，这里只负责把巨大矩形裁到表面附近，避免为
// 一个 100000 行的矩形分配一张 100000 项的行表。
static bool rectOnSurface(const CompatDevice& device, const IntRect& target, int margin, IntRect& out) {
    const IntRect surface{0, 0, device.bitmap->width, device.bitmap->height};
    out = intersect(inflateRect(target, margin), surface);
    return !isEmpty(out);
}

int FillRect(HDC hdc, const RECT* lprc, HBRUSH hbr) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || lprc == nullptr) return 0;
    if (device->bitmap == nullptr) {
        // 录制型 DC 上没有 FillRect 可记录：EMF 侧只实现了 BITBLT 两型，而
        // 静默不记录会让产品的帧稳定性审计漏报"出现中间帧"（见 S9 说明）。
        reportUnsupported("FillRect", "recording DC has no pixel surface (EMR_RECTANGLE is not recorded)");
        return 0;
    }
    CompatObject* brush = asObject(reinterpret_cast<HGDIOBJ>(hbr));
    if (brush == nullptr || brush->kind != ObjectKind::Brush) return 0;
    if (brush->hollow) return 1;  // NULL_BRUSH：什么都不填，但语义上成功
    if (!isSupportedRop2(device->rop2)) {
        reportUnsupportedLong("FillRect", "raster operation mode", device->rop2);
        return 0;
    }
    IntRect target{};
    if (!mapRect(*device, lprc->left, lprc->top, lprc->right, lprc->bottom, target)) return 0;
    if (isEmpty(target)) return 1;
    IntRect clipped{};
    if (!rectOnSurface(*device, target, 0, clipped)) return 1;
    PixelSink sink{device->bitmap, device};
    sink.fillRect(clipped, brushPixel(*device, *brush), device->rop2);
    return 1;
}

int FrameRect(HDC hdc, const RECT* lprc, HBRUSH hbr) {
    // 与 FillRect 的差别只有一点：只画那一圈 1 像素的边，且**用画刷的色**
    // （不是笔）。调用点拿它画菜单分隔线与焦点框，若在里面填满会把文字糊掉。
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || lprc == nullptr) return 0;
    if (device->bitmap == nullptr) {
        reportUnsupported("FrameRect", "recording DC has no pixel surface (EMR_RECTANGLE is not recorded)");
        return 0;
    }
    CompatObject* brush = asObject(reinterpret_cast<HGDIOBJ>(hbr));
    if (brush == nullptr || brush->kind != ObjectKind::Brush) return 0;
    if (brush->hollow) return 1;
    if (!isSupportedRop2(device->rop2)) {
        reportUnsupportedLong("FrameRect", "raster operation mode", device->rop2);
        return 0;
    }
    IntRect target{};
    if (!mapRect(*device, lprc->left, lprc->top, lprc->right, lprc->bottom, target)) return 0;
    if (isEmpty(target)) return 1;
    IntRect clipped{};
    if (!rectOnSurface(*device, target, 0, clipped)) return 1;

    PixelSink sink{device->bitmap, device};
    const std::uint32_t color = brushPixel(*device, *brush);
    const int rop2 = device->rop2;
    for (int y = clipped.top; y < clipped.bottom; ++y) {
        if (y == target.top || y == target.bottom - 1) {
            sink.fillSpan(y, clipped.left, clipped.right, color, rop2);
            continue;
        }
        sink.fillSpan(y, target.left, target.left + 1, color, rop2);
        sink.fillSpan(y, target.right - 1, target.right, color, rop2);
    }
    return 1;
}

BOOL Rectangle(HDC hdc, int left, int top, int right, int bottom) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return FALSE;
    if (device->bitmap == nullptr) {
        reportUnsupported("Rectangle", "recording DC has no pixel surface (EMR_RECTANGLE is not recorded)");
        return FALSE;
    }
    if (!isSupportedRop2(device->rop2)) {
        reportUnsupportedLong("Rectangle", "raster operation mode", device->rop2);
        return FALSE;
    }
    IntRect target{};
    if (!mapRect(*device, left, top, right, bottom, target)) return FALSE;
    if (isEmpty(target)) return TRUE;
    const PenState pen = resolvePen(*device);
    const BrushState brush = resolveBrush(*device);
    IntRect extent{};
    if (!rectOnSurface(*device, target, pen.width + 2, extent)) return TRUE;

    ShapeRows rows;
    rows.top = target.top;
    rows.rows.resize(static_cast<std::size_t>(target.bottom - target.top));
    for (RegionSpan& span : rows.rows) span = RegionSpan{target.left, target.right};
    paintShapeRows(*device, rows, pen, brush, device->rop2);
    return TRUE;
}

BOOL Ellipse(HDC hdc, int left, int top, int right, int bottom) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return FALSE;
    if (device->bitmap == nullptr) {
        reportUnsupported("Ellipse", "recording DC has no pixel surface (EMR_ELLIPSE is not recorded)");
        return FALSE;
    }
    if (!isSupportedRop2(device->rop2)) {
        reportUnsupportedLong("Ellipse", "raster operation mode", device->rop2);
        return FALSE;
    }
    IntRect target{};
    if (!mapRect(*device, left, top, right, bottom, target)) return FALSE;
    if (isEmpty(target)) return TRUE;
    const PenState pen = resolvePen(*device);
    const BrushState brush = resolveBrush(*device);
    EllipseSpanContext context{target.left, target.top, target.right, target.bottom};
    const ShapeRows rows = buildShapeRows(target.top, target.bottom,
                                          target.top - (pen.width + 2),
                                          target.bottom + (pen.width + 2), ellipseSpanThunk, &context);
    paintShapeRows(*device, rows, pen, brush, device->rop2);
    return TRUE;
}

BOOL RoundRect(HDC hdc, int left, int top, int right, int bottom, int width, int height) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return FALSE;
    if (device->bitmap == nullptr) {
        reportUnsupported("RoundRect", "recording DC has no pixel surface (EMR_ROUNDRECT is not recorded)");
        return FALSE;
    }
    if (!isSupportedRop2(device->rop2)) {
        reportUnsupportedLong("RoundRect", "raster operation mode", device->rop2);
        return FALSE;
    }
    IntRect target{};
    if (!mapRect(*device, left, top, right, bottom, target)) return FALSE;
    if (isEmpty(target)) return TRUE;
    const PenState pen = resolvePen(*device);
    const BrushState brush = resolveBrush(*device);
    RoundRectSpanContext context{target.left, target.top, target.right, target.bottom, width, height};
    const ShapeRows rows = buildShapeRows(target.top, target.bottom,
                                          target.top - (pen.width + 2),
                                          target.bottom + (pen.width + 2), roundRectSpanThunk, &context);
    paintShapeRows(*device, rows, pen, brush, device->rop2);
    return TRUE;
}

BOOL Polygon(HDC hdc, const POINT* apt, int cpt) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || apt == nullptr) return FALSE;
    if (device->bitmap == nullptr) {
        reportUnsupported("Polygon", "recording DC has no pixel surface (EMR_POLYGON is not recorded)");
        return FALSE;
    }
    if (cpt < 3) return TRUE;  // 少于三个点没有面积，Win32 同样不画
    if (!isSupportedRop2(device->rop2)) {
        reportUnsupportedLong("Polygon", "raster operation mode", device->rop2);
        return FALSE;
    }
    std::vector<POINT> mapped(static_cast<std::size_t>(cpt));
    for (int i = 0; i < cpt; ++i) {
        if (!mapPoint(*device, apt[i].x, apt[i].y, mapped[static_cast<std::size_t>(i)])) return FALSE;
    }
    // 填充模式固定 ALTERNATE（GDI 的默认值）。WINDING 只有 CreatePolygonRgn
    // 的调用点用到，而那条路径在 S3-2 已实现；本项目里 Polygon() 没有一处
    // 依赖非零环绕，故不引入额外的 DC 状态（引入就得同步进 SaveDC 快照）。
    RegionBands bands;
    if (!polygonBands(mapped.data(), cpt, ALTERNATE, bands)) {
        reportUnsupported("Polygon", "polygon could not be rasterized");
        return FALSE;
    }
    const PenState pen = resolvePen(*device);
    const BrushState brush = resolveBrush(*device);
    const int rop2 = device->rop2;
    PixelSink sink{device->bitmap, device};
    if (!brush.hollow) {
        for (const RegionBand& band : bands) {
            for (int y = band.top; y < band.bottom; ++y) {
                sink.fillSpan(y, band.span.left, band.span.right, brush.color, rop2);
            }
        }
    }
    if (pen.present) {
        // 闭合轮廓。最后一条边**包含**起点（首尾相接），与 LineTo 的"不含终点"
        // 是两件事：闭合边必须把起点画上，否则尖角会缺一角。
        for (int i = 0; i < cpt; ++i) {
            const POINT& from = mapped[static_cast<std::size_t>(i)];
            const POINT& to = mapped[static_cast<std::size_t>((i + 1) % cpt)];
            strokeLine(*device, from, to, true, true, pen, rop2);
        }
    }
    return TRUE;
}

BOOL Polyline(HDC hdc, const POINT* apt, int cpt) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || apt == nullptr) return FALSE;
    if (device->bitmap == nullptr) {
        reportUnsupported("Polyline", "recording DC has no pixel surface (EMR_POLYLINE is not recorded)");
        return FALSE;
    }
    if (cpt < 2) return TRUE;
    if (!isSupportedRop2(device->rop2)) {
        reportUnsupportedLong("Polyline", "raster operation mode", device->rop2);
        return FALSE;
    }
    std::vector<POINT> mapped(static_cast<std::size_t>(cpt));
    for (int i = 0; i < cpt; ++i) {
        if (!mapPoint(*device, apt[i].x, apt[i].y, mapped[static_cast<std::size_t>(i)])) return FALSE;
    }
    const PenState pen = resolvePen(*device);
    if (!pen.present) return TRUE;
    // 折线不读也不改当前位置（Win32 语义）。终点不画，与 LineTo 一致。
    for (int i = 0; i + 1 < cpt; ++i) {
        strokeLine(*device, mapped[static_cast<std::size_t>(i)], mapped[static_cast<std::size_t>(i + 1)],
                   true, false, pen, device->rop2);
    }
    return TRUE;
}

BOOL PolyPolyline(HDC hdc, const POINT* apt, const DWORD* asz, DWORD csz) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || apt == nullptr || asz == nullptr || csz == 0) return FALSE;
    if (device->bitmap == nullptr) {
        reportUnsupported("PolyPolyline", "recording DC has no pixel surface (EMR_POLYPOLYLINE is not recorded)");
        return FALSE;
    }
    if (!isSupportedRop2(device->rop2)) {
        reportUnsupportedLong("PolyPolyline", "raster operation mode", device->rop2);
        return FALSE;
    }
    const PenState pen = resolvePen(*device);
    const POINT* cursor = apt;
    for (DWORD group = 0; group < csz; ++group) {
        const DWORD count = asz[group];
        std::vector<POINT> mapped(static_cast<std::size_t>(count));
        for (DWORD i = 0; i < count; ++i) {
            if (!mapPoint(*device, cursor[i].x, cursor[i].y, mapped[static_cast<std::size_t>(i)])) return FALSE;
        }
        if (pen.present) {
            for (DWORD i = 0; i + 1 < count; ++i) {
                strokeLine(*device, mapped[static_cast<std::size_t>(i)],
                           mapped[static_cast<std::size_t>(i + 1)], true, false, pen, device->rop2);
            }
        }
        cursor += count;
    }
    return TRUE;
}

// 逐像素混合。GDI 的约定：
//   AlphaFormat==0            → 源是**直通**颜色，整体乘 SourceConstantAlpha
//   AlphaFormat==AC_SRC_ALPHA → 源是**预乘** alpha，逐像素再乘 SourceConstantAlpha
// 后者若按非预乘处理会整片变暗（源色被乘两次 alpha），而"看起来只是偏暗"的
// 缺陷在任何自洽比较里都不会暴露。
static std::uint32_t blendPixel(std::uint32_t dst, std::uint32_t src, int alpha, bool premultiplied) {
    if (alpha <= 0) return dst;
    if (alpha >= 255 && !premultiplied) return (dst & 0xFF000000u) | (src & kRgbMask);
    std::uint32_t result = dst & 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        const std::uint32_t source = (src >> shift) & 0xFFu;
        const std::uint32_t target = (dst >> shift) & 0xFFu;
        std::uint32_t mixed;
        if (premultiplied) {
            const std::uint32_t added = (target * static_cast<std::uint32_t>(255 - alpha) + 127u) / 255u;
            mixed = source + added;
            if (mixed > 255u) mixed = 255u;
        } else {
            mixed = (source * static_cast<std::uint32_t>(alpha) +
                     target * static_cast<std::uint32_t>(255 - alpha) + 127u) / 255u;
        }
        result |= (mixed & 0xFFu) << shift;
    }
    return result;
}

BOOL GdiAlphaBlend(HDC hdcDest, int xDest, int yDest, int wDest, int hDest, HDC hdcSrc, int xSrc, int ySrc,
                   int wSrc, int hSrc, BLENDFUNCTION ftn) {
    CompatDevice* destDevice = asDevice(hdcDest);
    CompatDevice* sourceDevice = asDevice(hdcSrc);
    if (destDevice == nullptr || sourceDevice == nullptr) return FALSE;
    if (destDevice->bitmap == nullptr) {
        reportUnsupported("GdiAlphaBlend", "recording DC has no pixel surface (EMR_ALPHABLEND is not recorded)");
        return FALSE;
    }
    CompatObject* source = sourceDevice->bitmap;
    if (source == nullptr) return FALSE;
    if (ftn.BlendOp != AC_SRC_OVER) {
        reportUnsupportedLong("GdiAlphaBlend", "BlendOp", static_cast<long>(ftn.BlendOp));
        return FALSE;
    }
    if (ftn.BlendFlags != 0) {
        reportUnsupportedLong("GdiAlphaBlend", "BlendFlags", static_cast<long>(ftn.BlendFlags));
        return FALSE;
    }
    if (ftn.AlphaFormat != 0 && ftn.AlphaFormat != AC_SRC_ALPHA) {
        reportUnsupportedLong("GdiAlphaBlend", "AlphaFormat", static_cast<long>(ftn.AlphaFormat));
        return FALSE;
    }
    if (wDest <= 0 || hDest <= 0 || wSrc <= 0 || hSrc <= 0) return FALSE;
    if (ftn.SourceConstantAlpha == 0) return TRUE;  // 全透明：不写像素，语义上成功

    CompatObject& dest = *destDevice->bitmap;
    const bool perPixel = ftn.AlphaFormat == AC_SRC_ALPHA;
    const int constant = static_cast<int>(ftn.SourceConstantAlpha);
    PixelSink sink{&dest, destDevice};
    for (int row = 0; row < hDest; ++row) {
        const int targetY = yDest + row;
        if (targetY < 0 || targetY >= dest.height) continue;
        const int sourceY = ySrc + static_cast<int>((static_cast<long long>(row) * hSrc) / hDest);
        if (sourceY < 0 || sourceY >= source->height) continue;
        RowSpans spans;
        sink.spansForRow(targetY, spans);
        if (spans.empty()) continue;
        std::uint32_t* targetRow = reinterpret_cast<std::uint32_t*>(rowPointer(dest, targetY));
        for (const RegionSpan& span : spans) {
            for (int column = 0; column < wDest; ++column) {
                const int targetX = xDest + column;
                if (targetX < span.left || targetX >= span.right) continue;
                const int sourceX = xSrc + static_cast<int>((static_cast<long long>(column) * wSrc) / wDest);
                if (sourceX < 0 || sourceX >= source->width) continue;
                const std::uint32_t pixel = readPixel(*source, sourceX, sourceY);
                int alpha = constant;
                if (perPixel) {
                    // 预乘源：像素 alpha 与常量 alpha 相乘。
                    alpha = (static_cast<int>((pixel >> 24) & 0xFFu) * constant + 127) / 255;
                }
                targetRow[targetX] = blendPixel(targetRow[targetX], pixel, alpha, perPixel);
            }
        }
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// 进程资源核算（S5b）
// ---------------------------------------------------------------------------
//
// 两个函数都忽略 hProcess：本层的进程视图是单一地址空间，调用点传的恒为
// GetCurrentProcess()。对别的进程报 0 并不比报错更有用，故不假装支持。

DWORD GetGuiResources(HANDLE hProcess, DWORD uiFlags) {
    (void)hProcess;
    if (uiFlags == GR_GDIOBJECTS) return static_cast<DWORD>(liveObjectCount);
    if (uiFlags == GR_USEROBJECTS) {
        // 窗口 / 菜单 / 钩子这些 USER 对象要等 S5 的窗口后端落地才存在。
        // 现在恒 0 —— 调用点的 before/after 相等断言因此恒真，这是已知的
        // 能力缺口，不伪造。
        return 0;
    }
    reportUnsupportedLong("GetGuiResources", "uiFlags", static_cast<long>(uiFlags));
    return 0;
}

BOOL GetProcessHandleCount(HANDLE hProcess, LPDWORD pdwCount) {
    (void)hProcess;
    if (pdwCount == nullptr) return FALSE;
    // Win32 的"句柄数"包含文件、事件、线程等一切内核对象；macOS 上最接近且
    // 可廉价枚举的对应物是 fd 表。语义不完全等价（socket 与 pipe 都算 fd，
    // 而线程不算），但用于"这一轮有没有泄漏"的增量判断是有效的。
    const int limit = ::getdtablesize();
    DWORD count = 0;
    for (int fd = 0; fd < limit; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) ++count;
    }
    *pdwCount = count;
    return TRUE;
}

// ---------------------------------------------------------------------------
// 文本 / 字体（P5-06）
// ---------------------------------------------------------------------------
// 这一族分三层，边界见匿名命名空间里 drawTextRun 的说明：
//   · DC 状态（Get/SetTextAlign）：真实状态，SaveDC/RestoreDC 一并保存。
//   · 度量（CreateFontW / GetTextExtentPoint32W）：真实对象 + 固定栅格度量，
//     让"先量后画"的居中/对齐代码不错位。
//   · 绘制（TextOutW / ExtTextOutA / DrawTextW / DrawTextA）：S4-2 起是**真字形**
//     ——CoreText 光栅 + 1bit mask 合成。缺字形才退回空心框（豆腐块）。

HFONT CreateFontW(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight,
                  DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet,
                  DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily,
                  LPCWSTR pszFaceName) {
    auto* object = new (std::nothrow) CompatObject();
    if (object == nullptr) return nullptr;
    object->kind = ObjectKind::Font;
    object->fontLog.lfHeight = cHeight;
    object->fontLog.lfWidth = cWidth;
    object->fontLog.lfEscapement = cEscapement;
    object->fontLog.lfOrientation = cOrientation;
    object->fontLog.lfWeight = cWeight;
    object->fontLog.lfItalic = static_cast<BYTE>(bItalic != 0);
    object->fontLog.lfUnderline = static_cast<BYTE>(bUnderline != 0);
    object->fontLog.lfStrikeOut = static_cast<BYTE>(bStrikeOut != 0);
    object->fontLog.lfCharSet = static_cast<BYTE>(iCharSet);
    object->fontLog.lfOutPrecision = static_cast<BYTE>(iOutPrecision);
    object->fontLog.lfClipPrecision = static_cast<BYTE>(iClipPrecision);
    object->fontLog.lfQuality = static_cast<BYTE>(iQuality);
    object->fontLog.lfPitchAndFamily = static_cast<BYTE>(iPitchAndFamily);
    object->fontLog.lfFaceName[0] = L'\0';
    if (pszFaceName != nullptr) {
        int n = 0;
        while (n < 31 && pszFaceName[n] != L'\0') {
            object->fontLog.lfFaceName[n] = pszFaceName[n];
            ++n;
        }
        object->fontLog.lfFaceName[n] = L'\0';
    }
    ++liveObjectCount;
    return reinterpret_cast<HFONT>(object);
}

UINT SetTextAlign(HDC hdc, UINT align) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return GDI_ERROR;
    const UINT previous = device->textAlign;
    device->textAlign = align;
    return previous;
}

UINT GetTextAlign(HDC hdc) {
    CompatDevice* device = asDevice(hdc);
    return device == nullptr ? GDI_ERROR : device->textAlign;
}

BOOL GetTextExtentPoint32W(HDC hdc, LPCWSTR lpString, int c, LPSIZE psizl) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || psizl == nullptr || c < 0) return FALSE;
    if (c > 0 && lpString == nullptr) return FALSE;
    const CompatObject* font = asObject(device->font);
    const FontMetrics m = fontMetricsFor(font);
    // 空文本宽度 0，但高度仍是字高（与 Windows 一致，空行不塌陷）。
    psizl->cx = text::runAdvance(lpString, c, m.width, m.height);
    psizl->cy = m.height;
    return TRUE;
}

BOOL TextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || c < 0) return FALSE;
    if (c > 0 && lpString == nullptr) return FALSE;
    // 文本对齐：TA_CENTER / TA_RIGHT 改 x 参照点，TA_BOTTOM 改 y 参照点。
    // 参照点用**整串**前进宽度而不是 字符数 × 字宽——东亚宽字符占 1:1 单元格，
    // 按字符数乘会算短一半（"中文" 是 32 而不是 22）。
    const CompatObject* font = asObject(device->font);
    const FontMetrics m = fontMetricsFor(font);
    const int advance = text::runAdvance(lpString, c, m.width, m.height);
    int ox = x;
    int oy = y;
    if (device->textAlign & TA_CENTER) ox = x - advance / 2;
    else if (device->textAlign & TA_RIGHT) ox = x - advance;
    if (device->textAlign & TA_BOTTOM) oy = y - m.height;
    drawTextRun(device, ox, oy, lpString, c, nullptr, nullptr);
    return TRUE;
}

BOOL ExtTextOutA(HDC hdc, int x, int y, UINT options, const RECT* lprect,
                 LPCSTR lpString, UINT c, const INT* lpDx) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr) return FALSE;
    const int count = static_cast<int>(c);
    if (count > 0 && lpString == nullptr) return FALSE;
    // A 串按 Latin-1 逐字节解释：产品侧调用点全是 ASCII，唯一可能出现的非 ASCII
    // 是 0x80-0xFF 的西文重音，Latin-1 与 CP1252 在这段只差 32 个码位，而项目
    // 没有用到那 32 个。真正的多语言文本都走 W 接口。
    std::vector<wchar_t> wide(static_cast<std::size_t>(count) + 1, 0);
    for (int index = 0; index < count; ++index) {
        wide[static_cast<std::size_t>(index)] =
            static_cast<wchar_t>(static_cast<unsigned char>(lpString[index]));
    }
    // ETO_OPAQUE：先用背景色铺满矩形（不是单元格！GDI 的语义是整块矩形）。
    if ((options & ETO_OPAQUE) != 0 && lprect != nullptr && device->bitmap != nullptr) {
        PixelSink sink{device->bitmap, device};
        sink.fillRect(IntRect{lprect->left, lprect->top, lprect->right, lprect->bottom},
                      toPixel(device->bkColor), R2_COPYPEN);
    }
    // ETO_CLIPPED：文本裁到 lprect。不裁的话 DrawText 之后的文本框会溢出到
    // 相邻控件上，而那个错误在自洽契约里看不出来。
    const RECT* clip = (options & ETO_CLIPPED) != 0 ? lprect : nullptr;
    drawTextRun(device, x, y, wide.data(), count, lpDx, clip);
    return TRUE;
}

int DrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || lprc == nullptr) return 0;
    const int count = wideCount(lpchText, cchText);
    const CompatObject* font = asObject(device->font);
    const FontMetrics m = fontMetricsFor(font);
    const int textWidth = text::runAdvance(lpchText, count, m.width, m.height);
    const int textHeight = m.height;

    if (format & DT_CALCRECT) {
        // 只测量不改画：把 lprc 收敛到文本包围盒（单行）。
        lprc->right = lprc->left + textWidth;
        lprc->bottom = lprc->top + textHeight;
        return textHeight;
    }

    int x = lprc->left;
    if (format & DT_CENTER) x = lprc->left + ((lprc->right - lprc->left) - textWidth) / 2;
    else if (format & DT_RIGHT) x = lprc->right - textWidth;
    int y = lprc->top;
    if (format & DT_VCENTER) y = lprc->top + ((lprc->bottom - lprc->top) - textHeight) / 2;
    else if (format & DT_BOTTOM) y = lprc->bottom - textHeight;

    // DT_WORDBREAK / DT_END_ELLIPSIS / DT_NOCLIP 仍未实现：本层是单行排版，
    // 换行与省略号属真实排版器（要逐词度量 + 二分收缩），留给后续轮次。
    // 已知后果：超长串会在控件右侧溢出，由 surface 边界兜住，不会写坏内存。
    drawTextRun(device, x, y, lpchText, count, nullptr, nullptr);
    return textHeight;
}

int DrawTextA(HDC hdc, LPCSTR lpchText, int cchText, LPRECT lprc, UINT format) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || lprc == nullptr) return 0;
    const int count = ansiCount(lpchText, cchText);
    const CompatObject* font = asObject(device->font);
    const FontMetrics m = fontMetricsFor(font);
    int textWidth = 0;
    for (int index = 0; index < count; ++index) {
        textWidth += text::glyphAdvance(
            static_cast<char32_t>(static_cast<unsigned char>(lpchText[index])), m.width, m.height);
    }
    const int textHeight = m.height;

    if (format & DT_CALCRECT) {
        lprc->right = lprc->left + textWidth;
        lprc->bottom = lprc->top + textHeight;
        return textHeight;
    }

    int x = lprc->left;
    if (format & DT_CENTER) x = lprc->left + ((lprc->right - lprc->left) - textWidth) / 2;
    else if (format & DT_RIGHT) x = lprc->right - textWidth;
    int y = lprc->top;
    if (format & DT_VCENTER) y = lprc->top + ((lprc->bottom - lprc->top) - textHeight) / 2;
    else if (format & DT_BOTTOM) y = lprc->bottom - textHeight;

    std::vector<wchar_t> wide(static_cast<std::size_t>(count) + 1, 0);
    for (int index = 0; index < count; ++index) {
        wide[static_cast<std::size_t>(index)] =
            static_cast<wchar_t>(static_cast<unsigned char>(lpchText[index]));
    }
    drawTextRun(device, x, y, wide.data(), count, nullptr, nullptr);
    return textHeight;
}

DWORD GetGlyphIndicesW(HDC hdc, LPCWSTR lpstr, int c, LPWORD pgi, DWORD fl) {
    CompatDevice* device = asDevice(hdc);
    if (device == nullptr || c < 0) return GDI_ERROR;
    if (c == 0) return 0;
    if (lpstr == nullptr || pgi == nullptr) return GDI_ERROR;
    const CompatObject* font = asObject(device->font);
    const FontMetrics m = fontMetricsFor(font);
    const text::FaceSpec spec = faceSpecFor(font);
    // 现在是**真实**答案：CoreText 的 cmap 查询 + 逐码点字体回退。此前的
    // GDI_ERROR 是"查不出来"，它让 RuntimePlayerDefeatNoticeSmoke 的
    // "字体是否覆盖所需汉字"判据永远报缺字形——那是 mac 上的假象，不是事实。
    // 代理对：高低两个码元给同一个字形索引（Windows 对 UTF-16 也是逐码元输出，
    // 差别在于它会给出两个不同的 surrogate 字形，这里不伪造）。
    for (int index = 0; index < c;) {
        const char32_t codePoint = text::decodeCodePoint(lpstr, index, c);
        const unsigned glyph = text::glyphIndex(codePoint, spec, m.height);
        const int units = text::codePointLength(lpstr, index, c);
        for (int unit = 0; unit < units && index + unit < c; ++unit) {
            pgi[index + unit] = static_cast<WORD>(glyph);
        }
        index += units;
    }
    (void)fl;  // GGI_MARK_NONEXISTING_GLYPHS：本层缺字形恒报 0xFFFF，无需分支
    return static_cast<DWORD>(c);
}

// ---------------------------------------------------------------------------
// DPI 感知（P5-06）
// ---------------------------------------------------------------------------
// 软件帧缓冲不缩放、按原生像素渲染，等价于 Win32 的"每显示器感知"（无 DPI
// 位图虚拟化）。产品侧 MapNativeOverlayWin32::begin 有硬门槛：GetAwarenessFromDpiAwarenessContext
// (GetThreadDpiAwarenessContext()) 必须 == DPI_AWARENESS_PER_MONITOR_AWARE，否则
// native overlay 直接失败。故这里如实返回 PER_MONITOR_AWARE，而非"未感知"默认。

DPI_AWARENESS_CONTEXT GetThreadDpiAwarenessContext() {
    return DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE;
}

DPI_AWARENESS GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT value) {
    if (value == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ||
        value == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) {
        return DPI_AWARENESS_PER_MONITOR_AWARE;
    }
    if (value == DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) return DPI_AWARENESS_SYSTEM_AWARE;
    if (value == DPI_AWARENESS_CONTEXT_UNAWARE) return DPI_AWARENESS_UNAWARE;
    return DPI_AWARENESS_INVALID;
}

}  // extern "C"
