// compat/win32 自测 · 区域与裁剪（S3-2，B 段 5 个符号）
//
// 这一族最容易"看起来做完了"：
//   * 区域能建、能组合，GetRgnBox 也能给出外接矩形 —— 但复杂度恒答
//     SIMPLEREGION，于是三条快路径在复杂裁剪下照走不误，绕过裁剪写像素。
//   * SelectClipRgn 存的是指针而不是副本 —— 调用点"选中后立刻 DeleteObject"
//     的下一次绘制读到已释放内存。它能编译、能跑一次、断言还全绿。
//   * SaveDC 漏掉裁剪字段 —— 恢复后裁剪残留，后续用例莫名走慢路。
// 所以下面的断言分三类：**硬编码的矩形/复杂度**、**删掉区域之后仍在**、
// **保存/恢复往返**。一律用字面量（`== 3` 而不是 `== COMPLEXREGION`）：
// 拿常量自比时，头文件把常量抄错也照样通过。
//
// 运行：
//   c++ -std=c++20 -I compat/win32/include \
//       compat/win32/src/RuntimeWin32CompatGdi.cpp \
//       compat/win32/src/RuntimeWin32CompatMisc.cpp \
//       compat/win32/selftest/CompatRegionSemantics.cpp -o /tmp/rgn && /tmp/rgn

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

void checkRect(int left, int top, int right, int bottom, const RECT& actual, const char* description) {
    ++gChecks;
    if (actual.left != left || actual.top != top || actual.right != right || actual.bottom != bottom) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected {%d,%d,%d,%d}, got {%ld,%ld,%ld,%ld})\n", description, left, top,
                    right, bottom, static_cast<long>(actual.left), static_cast<long>(actual.top),
                    static_cast<long>(actual.right), static_cast<long>(actual.bottom));
    }
}

struct Surface {
    static constexpr int width = 200;
    static constexpr int height = 160;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void* bits = nullptr;

    bool create() {
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
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

RECT regionBox(HRGN region) {
    RECT box{-1, -1, -1, -1};
    GetRgnBox(region, &box);
    return box;
}

// ---------------------------------------------------------------------------
// 1. 常量字面量
// ---------------------------------------------------------------------------
void testConstantLiterals() {
    checkInt(1, RGN_AND, "RGN_AND 是 1");
    checkInt(2, RGN_OR, "RGN_OR 是 2");
    checkInt(3, RGN_XOR, "RGN_XOR 是 3");
    checkInt(4, RGN_DIFF, "RGN_DIFF 是 4");
    checkInt(5, RGN_COPY, "RGN_COPY 是 5");
    checkInt(1, NULLREGION, "NULLREGION 是 1");
    checkInt(2, SIMPLEREGION, "SIMPLEREGION 是 2");
    checkInt(3, COMPLEXREGION, "COMPLEXREGION 是 3");
    checkInt(8, OBJ_REGION, "OBJ_REGION 是 8");
    checkInt(1, ALTERNATE, "ALTERNATE 是 1");
    checkInt(2, WINDING, "WINDING 是 2");
}

// ---------------------------------------------------------------------------
// 2. 矩形区域：半开区间、空区域、句柄类型
// ---------------------------------------------------------------------------
void testRectRegion() {
    HRGN region = CreateRectRgn(10, 20, 40, 60);
    check(region != nullptr, "CreateRectRgn 返回非空句柄");
    if (region == nullptr) return;
    checkInt(8, static_cast<long long>(GetObjectType(reinterpret_cast<HGDIOBJ>(region))),
             "区域的 GetObjectType 是 OBJ_REGION(8)");
    RECT box = regionBox(region);
    checkRect(10, 20, 40, 60, box, "矩形区域外接框等于入参");
    checkInt(2, GetRgnBox(region, &box), "单个矩形的复杂度是 SIMPLEREGION(2)");

    // 半开区间：left/top 在内，right/bottom 在外。这三个点错了任何一个，
    // 所有裁剪边界都会整体偏移一像素，而画面"看起来是对的"。
    check(PtInRegion(region, 10, 20), "左上角 (10,20) 在区域内（闭）");
    check(PtInRegion(region, 39, 59), "右下内侧 (39,59) 在区域内");
    check(!PtInRegion(region, 40, 30), "右边界 x=40 在区域外（开）");
    check(!PtInRegion(region, 30, 60), "下边界 y=60 在区域外（开）");
    check(!PtInRegion(region, 9, 30), "左侧外一点在区域外");

    // 右下角小于左上角 → 空区域（Win32 不自动换位）
    check(SetRectRgn(region, 30, 10, 10, 30), "SetRectRgn 接受倒置矩形");
    checkInt(1, GetRgnBox(region, &box), "倒置矩形得到 NULLREGION(1)");
    checkRect(0, 0, 0, 0, box, "空区域的外接框是零矩形");
    check(!PtInRegion(region, 15, 20), "空区域不含任何点");

    check(SetRectRgn(region, 5, 5, 15, 15), "SetRectRgn 换成一个正常矩形");
    checkInt(2, GetRgnBox(region, &box), "换回单个矩形后又是 SIMPLEREGION(2)");
    check(PtInRegion(region, 14, 14), "SetRectRgn 之后点位正确");

    RECT source{100, 100, 120, 130};
    HRGN indirect = CreateRectRgnIndirect(&source);
    check(indirect != nullptr, "CreateRectRgnIndirect 返回非空");
    if (indirect != nullptr) {
        const RECT indirectBox = regionBox(indirect);
        checkRect(100, 100, 120, 130, indirectBox, "CreateRectRgnIndirect 与外接框一致");
        DeleteObject(indirect);
    }
    DeleteObject(region);
}

// ---------------------------------------------------------------------------
// 3. 布尔组合：RGN_OR / RGN_AND / RGN_DIFF / RGN_XOR / RGN_COPY
// ---------------------------------------------------------------------------
//
// 每个组合都用**新建**的两个区域：RGN_* 会就地改写 dst，复用同一对句柄
// 会让后一条断言实际测的是上一条的结果（这一版最初就写错了，5 条断言
// 因此假失败——错的期望值比错的实现更危险）。
struct Pair {
    HRGN left = nullptr;
    HRGN right = nullptr;
    HRGN scratch = nullptr;
    bool ready() {
        left = CreateRectRgn(10, 10, 30, 30);
        right = CreateRectRgn(60, 50, 80, 80);
        scratch = CreateRectRgn(0, 0, 1, 1);
        return left != nullptr && right != nullptr && scratch != nullptr;
    }
    ~Pair() {
        if (scratch != nullptr) DeleteObject(scratch);
        if (left != nullptr) DeleteObject(left);
        if (right != nullptr) DeleteObject(right);
    }
};

void testCombine() {
    {
        // RGN_OR 两块不连通 → COMPLEXREGION。这是整个 S3-2 的核心断言：
        // 若这里返回 2，三条快路径会在复杂裁剪下继续写像素。
        Pair pair;
        check(pair.ready(), "OR 用两个矩形区域创建成功");
        if (pair.ready()) {
            checkInt(3, CombineRgn(pair.left, pair.left, pair.right, RGN_OR),
                     "两个不连通矩形 OR 之后是 COMPLEXREGION(3)");
            const RECT box = regionBox(pair.left);
            checkRect(10, 10, 80, 80, box, "OR 的外接框是两块的外包围");
            check(PtInRegion(pair.left, 15, 15), "OR 含左块内部点");
            check(PtInRegion(pair.left, 65, 55), "OR 含右块内部点");
            check(!PtInRegion(pair.left, 40, 30), "OR 不含两块之间的空隙");
            check(!PtInRegion(pair.left, 15, 35), "OR 不含左块下方的行（条带按行记录）");
        }
    }
    {
        Pair pair;
        check(pair.ready(), "AND/XOR 用两个矩形区域创建成功");
        if (pair.ready()) {
            checkInt(1, CombineRgn(pair.scratch, pair.left, pair.right, RGN_AND),
                     "不相交的两块 AND 之后是 NULLREGION(1)");
            RECT box{};
            checkInt(1, GetRgnBox(pair.scratch, &box), "AND 结果的外接框是零矩形");
            checkRect(0, 0, 0, 0, box, "AND 空结果的外接框全零");

            checkInt(2, CombineRgn(pair.scratch, pair.left, pair.left, RGN_AND),
                     "自己 AND 自己仍是原来的单个矩形（SIMPLEREGION(2)）");

            // RGN_COPY 拷的是 src1（单个矩形 → 仍然是 SIMPLEREGION）。
            // 这里如果用 3 去断言，测的其实是"copy 了 OR 的结果"，而 src1
            // 在这个用例里从未被 OR 过。
            checkInt(2, CombineRgn(pair.scratch, pair.left, pair.right, RGN_COPY),
                     "RGN_COPY 拷贝 src1（单矩形），复杂度 2");
            checkRect(10, 10, 30, 30, regionBox(pair.scratch), "RGN_COPY 之后外接框等于 src1");

            checkInt(2, CombineRgn(pair.scratch, pair.left, pair.right, RGN_DIFF),
                     "两块不相交时 src1 - src2 仍是原来那个矩形");
            check(PtInRegion(pair.scratch, 15, 15), "DIFF 保留 src1 的像素");
            check(!PtInRegion(pair.scratch, 65, 55), "DIFF 不含 src2（本来就不相交）");
            checkRect(10, 10, 30, 30, regionBox(pair.scratch), "DIFF 之后外接框等于 src1");

            checkInt(3, CombineRgn(pair.scratch, pair.left, pair.right, RGN_XOR),
                     "不相交时 XOR 等价 OR，复杂度 3");
            check(PtInRegion(pair.scratch, 65, 55), "XOR 含右块");
            check(!PtInRegion(pair.scratch, 40, 30), "XOR 不含空隙");

            // 真减法：从大块里挖掉重叠的小块 → 四条带 → COMPLEXREGION
            HRGN big = CreateRectRgn(0, 0, 40, 40);
            HRGN hole = CreateRectRgn(10, 10, 20, 20);
            if (big != nullptr && hole != nullptr) {
                checkInt(3, CombineRgn(big, big, hole, RGN_DIFF),
                         "从大块里挖掉一块得到 COMPLEXREGION(3)");
                check(!PtInRegion(big, 15, 15), "挖掉的部分不在区域里");
                check(PtInRegion(big, 5, 15), "洞左侧仍在区域里");
                check(PtInRegion(big, 15, 5), "洞上方仍在区域里");
                checkRect(0, 0, 40, 40, regionBox(big), "挖洞后外接框不变");
            }
            if (big != nullptr) DeleteObject(big);
            if (hole != nullptr) DeleteObject(hole);
        }
    }
    {
        Pair pair;
        if (pair.ready()) {
            // 非法模式必须报 RGN_ERROR，而不是"什么都不做还返回成功"
            checkInt(0, CombineRgn(pair.scratch, pair.left, pair.right, 99), "未知组合模式返回 RGN_ERROR(0)");
            checkInt(0, CombineRgn(nullptr, pair.left, pair.right, RGN_OR), "空目标句柄返回 RGN_ERROR(0)");
            checkInt(0, CombineRgn(pair.scratch, nullptr, pair.right, RGN_OR), "空源句柄返回 RGN_ERROR(0)");

            // RectInRegion：文档是"矩形的任何部分在区域内即为真"
            RECT inside{12, 12, 20, 20};
            RECT straddling{20, 20, 40, 40};
            RECT outside{100, 100, 110, 110};
            RECT empty{15, 15, 15, 15};
            check(RectInRegion(pair.left, &inside), "完全落在区域内的矩形 RectInRegion 为真");
            check(RectInRegion(pair.left, &straddling), "部分相交的矩形为真（文档语义是'任何部分'）");
            check(!RectInRegion(pair.left, &outside), "完全在外的矩形 RectInRegion 为假");
            check(!RectInRegion(pair.left, &empty), "空矩形没有任何部分，RectInRegion 为假");
        }
    }
}

// ---------------------------------------------------------------------------
// 4. 选入 DC 的裁剪区：必须是**副本**，必须能报出复杂区域
// ---------------------------------------------------------------------------
void testComplexClip() {
    Surface surface;
    check(surface.create(), "DIB 表面创建成功（复杂裁剪用例）");
    if (surface.dc == nullptr) return;

    HRGN first = CreateRectRgn(10, 10, 30, 30);
    HRGN second = CreateRectRgn(60, 50, 80, 80);
    check(first != nullptr && second != nullptr, "复杂裁剪的两个矩形区域创建成功");
    if (first == nullptr || second == nullptr) {
        if (first != nullptr) DeleteObject(first);
        if (second != nullptr) DeleteObject(second);
        return;
    }
    CombineRgn(first, first, second, RGN_OR);
    checkInt(3, SelectClipRgn(surface.dc, first), "选中复杂区域时 SelectClipRgn 返回 COMPLEXREGION(3)");

    RECT box{-1, -1, -1, -1};
    // 核心：GetClipBox 必须答 3。三条快路径都靠它决定"让不让"，恒答 2
    // 就等于宣布"复杂裁剪不存在"，而所有自洽契约照样全绿。
    checkInt(3, GetClipBox(surface.dc, &box), "复杂裁剪时 GetClipBox 返回 COMPLEXREGION(3)");
    checkRect(10, 10, 80, 80, box, "复杂裁剪的外接框是两块的外包围");

    // 调用点（RuntimeSelectionFrameRasterContract:170、RuntimeShell 时钟遮罩）
    // 都是选中后立刻删区域。若 SelectClipRgn 存指针，这里之后就是悬垂读。
    check(DeleteObject(first) != FALSE, "选中后立刻删除区域句柄，删除本身成功");
    check(DeleteObject(second) != FALSE, "删除第二个区域句柄");
    box = RECT{-1, -1, -1, -1};
    checkInt(3, GetClipBox(surface.dc, &box), "区域句柄删除后裁剪仍然有效且仍是 COMPLEXREGION(3)");
    checkRect(10, 10, 80, 80, box, "删除后外接框不变（证明是深拷贝而不是指针）");

    HRGN copied = CreateRectRgn(0, 0, 1, 1);
    check(copied != nullptr, "GetClipRgn 目标区域创建成功");
    if (copied != nullptr) {
        checkInt(3, GetClipRgn(surface.dc, copied), "GetClipRgn 读回复杂裁剪时返回 COMPLEXREGION(3)");
        check(PtInRegion(copied, 15, 15), "读回的裁剪含左块像素");
        check(PtInRegion(copied, 65, 55), "读回的裁剪含右块像素");
        check(!PtInRegion(copied, 40, 30), "读回的裁剪不含空隙");
        DeleteObject(copied);
    }

    // 重置：nullptr 是"没有 clip region"，不是"空区域"
    checkInt(1, SelectClipRgn(surface.dc, nullptr), "SelectClipRgn(nullptr) 返回 NULLREGION(1)");
    box = RECT{-1, -1, -1, -1};
    checkInt(2, GetClipBox(surface.dc, &box), "重置后 GetClipBox 回到 SIMPLEREGION(2)");
    checkRect(0, 0, Surface::width, Surface::height, box, "重置后裁剪框是整块表面");

    HRGN probe = CreateRectRgn(0, 0, 1, 1);
    if (probe != nullptr) {
        checkInt(0, GetClipRgn(surface.dc, probe), "没有 clip region 时 GetClipRgn 返回 0");
        const RECT untouched = regionBox(probe);
        checkRect(0, 0, 1, 1, untouched, "GetClipRgn 返回 0 时不改动目标区域");
        DeleteObject(probe);
    }
    checkInt(-1, GetClipRgn(surface.dc, nullptr), "GetClipRgn 空目标返回 -1");
}

// ---------------------------------------------------------------------------
// 5. IntersectClipRect / ExcludeClipRect：返回值必须与 GetClipBox 一致
// ---------------------------------------------------------------------------
void testClipRect() {
    Surface surface;
    check(surface.create(), "DIB 表面创建成功（裁剪矩形用例）");
    if (surface.dc == nullptr) return;

    RECT box{};
    checkInt(2, IntersectClipRect(surface.dc, 20, 10, 100, 90), "首次相交返回 SIMPLEREGION(2)");
    checkInt(2, GetClipBox(surface.dc, &box), "GetClipBox 与返回值一致");
    checkRect(20, 10, 100, 90, box, "裁剪框等于相交矩形");

    checkInt(2, IntersectClipRect(surface.dc, 40, 30, 60, 50), "嵌套相交后仍是 SIMPLEREGION(2)");
    checkInt(2, GetClipBox(surface.dc, &box), "嵌套相交后 GetClipBox 仍是 2");
    checkRect(40, 30, 60, 50, box, "嵌套相交取交集");

    // 与已有裁剪不相交 → 直接裁空
    checkInt(1, IntersectClipRect(surface.dc, 0, 0, 5, 5), "不相交的相交得到 NULLREGION(1)");
    box = RECT{-1, -1, -1, -1};
    checkInt(1, GetClipBox(surface.dc, &box), "裁空后 GetClipBox 是 NULLREGION(1)");
    checkRect(0, 0, 0, 0, box, "裁空后裁剪框是零矩形");

    // 这对用例是给 RuntimeSelectionFrameRasterContract:46 那个
    // IntersectClipRect(dc,0,0,0,0) 准备的：空矩形必须真的裁空，
    // 而不是被当成"交了个空，什么都不用做"。
    SelectClipRgn(surface.dc, nullptr);
    checkInt(1, IntersectClipRect(surface.dc, 0, 0, 0, 0), "零矩形相交得到 NULLREGION(1)");
    checkInt(1, GetClipBox(surface.dc, &box), "零矩形相交后 GetClipBox 是 1");

    // 超出表面必须被表面夹住，否则快路径会按"裁剪框=入参"算出越界目标
    SelectClipRgn(surface.dc, nullptr);
    checkInt(2, IntersectClipRect(surface.dc, -50, -50, 500, 500), "超大矩形相交返回 SIMPLEREGION(2)");
    checkInt(2, GetClipBox(surface.dc, &box), "超大矩形相交后 GetClipBox 是 2");
    checkRect(0, 0, Surface::width, Surface::height, box, "超大矩形被表面夹住");

    // ExcludeClipRect：从整块表面挖掉一块 → 四条带 → COMPLEXREGION
    SelectClipRgn(surface.dc, nullptr);
    checkInt(3, ExcludeClipRect(surface.dc, 10, 10, 30, 30), "从整块表面挖掉一块得到 COMPLEXREGION(3)");
    checkInt(3, GetClipBox(surface.dc, &box), "挖洞后 GetClipBox 是 COMPLEXREGION(3)");
    checkRect(0, 0, Surface::width, Surface::height, box, "挖洞后外接框仍是整块表面");
    HRGN hole = CreateRectRgn(0, 0, 1, 1);
    if (hole != nullptr) {
        checkInt(3, GetClipRgn(surface.dc, hole), "挖洞后的裁剪是复杂区域");
        check(!PtInRegion(hole, 20, 20), "洞内的点不在裁剪内");
        check(PtInRegion(hole, 5, 5), "洞外的点仍在裁剪内");
        check(PtInRegion(hole, 20, 5), "洞正上方的点仍在裁剪内");
        DeleteObject(hole);
    }
}

// ---------------------------------------------------------------------------
// 6. SaveDC / RestoreDC 必须带上裁剪区
// ---------------------------------------------------------------------------
void testSaveRestore() {
    Surface surface;
    check(surface.create(), "DIB 表面创建成功（保存恢复用例）");
    if (surface.dc == nullptr) return;

    RECT box{};
    SelectClipRgn(surface.dc, nullptr);
    IntersectClipRect(surface.dc, 20, 10, 100, 90);
    const int saved = SaveDC(surface.dc);
    checkInt(1, saved, "SaveDC 返回 1");

    IntersectClipRect(surface.dc, 40, 30, 60, 50);
    checkInt(2, GetClipBox(surface.dc, &box), "保存后修改裁剪，当前是 SIMPLEREGION(2)");
    checkRect(40, 30, 60, 50, box, "保存后修改裁剪，框变小");

    check(RestoreDC(surface.dc, saved) != FALSE, "RestoreDC 成功");
    checkInt(2, GetClipBox(surface.dc, &box), "恢复后裁剪复杂度回到 2");
    checkRect(20, 10, 100, 90, box, "恢复后裁剪框回到保存时的值（漏字段会留成 {40,30,60,50}）");

    // 复杂裁剪的保存/恢复：这条抓到的是"只存了矩形就以为够"的情形
    HRGN first = CreateRectRgn(10, 10, 30, 30);
    HRGN second = CreateRectRgn(60, 50, 80, 80);
    if (first != nullptr && second != nullptr) {
        CombineRgn(first, first, second, RGN_OR);
        const int complexSaved = SaveDC(surface.dc);
        SelectClipRgn(surface.dc, first);
        checkInt(3, GetClipBox(surface.dc, &box), "选中复杂裁剪后 GetClipBox 是 3");
        check(RestoreDC(surface.dc, complexSaved) != FALSE, "复杂裁剪下 RestoreDC 成功");
        checkInt(2, GetClipBox(surface.dc, &box), "恢复后回到保存时的简单裁剪（而不是残留复杂区）");
        checkRect(20, 10, 100, 90, box, "恢复后的裁剪框仍是保存时的值");
        DeleteObject(first);
        DeleteObject(second);
    }

    // 反向：在复杂裁剪上保存，重置，再恢复
    HRGN third = CreateRectRgn(5, 5, 25, 25);
    HRGN fourth = CreateRectRgn(70, 60, 90, 90);
    if (third != nullptr && fourth != nullptr) {
        CombineRgn(third, third, fourth, RGN_OR);
        SelectClipRgn(surface.dc, third);
        const int savedComplex = SaveDC(surface.dc);
        SelectClipRgn(surface.dc, nullptr);
        checkInt(2, GetClipBox(surface.dc, &box), "重置后是整块表面");
        check(RestoreDC(surface.dc, savedComplex) != FALSE, "恢复复杂裁剪成功");
        checkInt(3, GetClipBox(surface.dc, &box), "恢复后重新变回 COMPLEXREGION(3)（漏字段会留在 2）");
        DeleteObject(third);
        DeleteObject(fourth);
    }
}

// ---------------------------------------------------------------------------
// 7. 多边形区域：扫描线填充、ALTERNATE 与 WINDING 的真实差别
// ---------------------------------------------------------------------------
void testPolygon() {
    POINT box4[4] = {{10, 10}, {50, 10}, {50, 40}, {10, 40}};
    HRGN rectangle = CreatePolygonRgn(box4, 4, ALTERNATE);
    check(rectangle != nullptr, "四点矩形多边形创建成功");
    if (rectangle != nullptr) {
        RECT box = regionBox(rectangle);
        checkInt(2, GetRgnBox(rectangle, &box), "四点矩形多边形是 SIMPLEREGION(2)");
        checkRect(10, 10, 50, 40, box, "多边形区域外接框等于顶点包围");
        check(PtInRegion(rectangle, 10, 10), "多边形含左上角像素");
        check(PtInRegion(rectangle, 49, 39), "多边形含右下内侧像素");
        check(!PtInRegion(rectangle, 50, 40), "多边形不含右下边界像素（半开）");
        DeleteObject(rectangle);
    }

    // 交替 / 环绕的真实差别：五角星。
    // 用连续路径画星（顶点顺序 0,2,4,1,3）时，中心点的环绕数是 2：
    //   ALTERNATE（奇偶）→ 中心为空（两条边各贡献一次配对，中心在配对之外）
    //   WINDING（非零）  → 中心为实心
    // 这不是学术细节：RuntimeShell 画进度时钟遮罩用 WINDING，若把它当
    // ALTERNATE 实现，时钟中间会被挖空——画面明显不对，而没有断言会发现。
    // 这条差异用"某个点在内/在外"抓不住，所以同时断言整行的剖面。
    POINT star[5] = {{40, 0}, {16, 72}, {78, 28}, {2, 28}, {64, 72}};
    HRGN evenOdd = CreatePolygonRgn(star, 5, ALTERNATE);
    HRGN winding = CreatePolygonRgn(star, 5, WINDING);
    check(evenOdd != nullptr && winding != nullptr, "五角星（奇偶/环绕）创建成功");
    if (evenOdd != nullptr && winding != nullptr) {
        check(!PtInRegion(evenOdd, 40, 40), "ALTERNATE 下五角星中心为空");
        check(PtInRegion(winding, 40, 40), "WINDING 下五角星中心为实心（环绕数 2）");
        // 同一个点在两种模式下结论相反，才证明 WINDING 分支真的被走到了；
        // 只断言"两模式都填了星臂"的话，把 WINDING 写成 ALTERNATE 也能过。
        check(PtInRegion(evenOdd, 40, 10), "两种模式下顶臂都是实心（画法本身正确）");
        check(PtInRegion(winding, 40, 10), "WINDING 下顶臂也是实心");
        // 外接框比顶点包围小半个像素：像素中心的判定方式决定了尖角所在行
        // （y=0 与 y=71 的两端）没有完整像素宽度，最左/最右列同理。
        // 这不是误差，是"区域按像素格量化"的必然结果，必须钉进断言——
        // 若哪天有人把这里改成拿顶点 min/max 直接当外接框，这里会立刻红。
        const RECT starBox = regionBox(winding);
        checkRect(3, 1, 77, 72, starBox, "五角星外接框=顶点包围内缩半像素（尖角行无整像素宽）");
        DeleteObject(evenOdd);
        DeleteObject(winding);
    }

    // 顶点不足：空区域，但不是失败（Win32 返回合法空句柄）
    POINT two[2] = {{0, 0}, {10, 10}};
    HRGN degenerate = CreatePolygonRgn(two, 2, ALTERNATE);
    check(degenerate != nullptr, "不足三个点返回合法的空区域句柄");
    if (degenerate != nullptr) {
        RECT box{};
        checkInt(1, GetRgnBox(degenerate, &box), "空多边形区域是 NULLREGION(1)");
        DeleteObject(degenerate);
    }

    // 非法填充模式：显式失败，不猜
    check(CreatePolygonRgn(star, 5, 7) == nullptr, "未知填充模式返回空句柄");
    // 椭圆区域在 S3-3 兑现（此前是"显式失败，不拿矩形近似"）。这里只钉住
    // "四角真的是空的"这一件事——它是矩形近似唯一会漏掉的差别：
    // 近似会把 (0,0) 放进来，而正确实现不会。
    HRGN ellipse = CreateEllipticRgn(0, 0, 10, 10);
    check(ellipse != nullptr, "椭圆区域创建成功（S3-3 兑现）");
    if (ellipse != nullptr) {
        check(PtInRegion(ellipse, 0, 0) == FALSE, "椭圆区域不含左上角（不是矩形近似）");
        check(PtInRegion(ellipse, 5, 5) == TRUE, "椭圆区域含中心");
        check(PtInRegion(ellipse, 0, 5) == TRUE, "椭圆区域含最左中点");
        DeleteObject(ellipse);
    }
    // 退化椭圆（右下不大于左上）与矩形区域一致：空区域，不是失败。
    HRGN degenerateEllipse = CreateEllipticRgn(4, 4, 4, 4);
    check(degenerateEllipse != nullptr, "退化椭圆区域返回空区域而不是空句柄");
    if (degenerateEllipse != nullptr) {
        RECT degenerateBox{};
        checkInt(1, GetRgnBox(degenerateEllipse, &degenerateBox), "退化椭圆区域是 NULLREGION(1)");
        DeleteObject(degenerateEllipse);
    }
}

// ---------------------------------------------------------------------------
// 8. 区域是 GDI 对象：计入 GR_GDIOBJECTS、不可 SelectObject、可删除
// ---------------------------------------------------------------------------
void testObjectAccounting() {
    Surface surface;
    check(surface.create(), "DIB 表面创建成功（对象核算用例）");
    if (surface.dc == nullptr) return;

    // GetCurrentProcess 属 S10（进程/文件族），本轮还没有实现，故传一个空句柄：
    // 本层的 GetGuiResources 明确忽略 hProcess（单一地址空间，调用点恒传当前进程）。
    const HANDLE kPseudoProcess = nullptr;
    const DWORD before = GetGuiResources(kPseudoProcess, GR_GDIOBJECTS);
    HRGN region = CreateRectRgn(0, 0, 10, 10);
    check(region != nullptr, "核算用区域创建成功");
    checkInt(static_cast<long long>(before) + 1, GetGuiResources(kPseudoProcess, GR_GDIOBJECTS),
             "创建区域后 GR_GDIOBJECTS 加 1（区域是真 GDI 对象）");

    // 区域只能通过 SelectClipRgn 选入；SelectObject 必须拒绝，
    // 否则它会被塞进 bitmap/pen 槽位，之后每次绘制读到垃圾。
    check(SelectObject(surface.dc, reinterpret_cast<HGDIOBJ>(region)) == nullptr,
          "SelectObject 拒绝区域（必须先 SelectClipRgn）");

    check(DeleteObject(region) != FALSE, "删除区域成功");
    checkInt(static_cast<long long>(before), GetGuiResources(kPseudoProcess, GR_GDIOBJECTS),
             "删除区域后 GR_GDIOBJECTS 回到原值（无泄漏）");
}

// ---------------------------------------------------------------------------
// 9. 录制型 DC 也能选裁剪区（MapNativeOverlayWin32 的真实用法）
// ---------------------------------------------------------------------------
void testRecordingDcClip() {
    HDC metafile = CreateEnhMetaFileW(nullptr, nullptr, nullptr, nullptr);
    check(metafile != nullptr, "录制型 DC 创建成功（裁剪用例）");
    if (metafile == nullptr) return;

    // MapNativeOverlayWin32.cpp:168 就是这么写的：GetClipRgn 拿不到裁剪时
    // 直接跳过 SelectClipRgn；拿到时必须 != ERROR，否则整条覆盖绘制判失败。
    HRGN probe = CreateRectRgn(0, 0, 0, 0);
    check(probe != nullptr, "录制 DC 用探针区域创建成功");
    if (probe != nullptr) {
        const int existing = GetClipRgn(metafile, probe);
        checkInt(0, existing, "录制型 DC 初始没有 clip region");
        DeleteObject(probe);
    }

    HRGN clip = CreateRectRgn(4, 4, 60, 40);
    check(clip != nullptr, "录制 DC 的裁剪区域创建成功");
    if (clip != nullptr) {
        const int result = SelectClipRgn(metafile, clip);
        check(result != ERROR, "录制型 DC 上 SelectClipRgn 不返回 ERROR");
        checkInt(2, result, "录制型 DC 上单个矩形裁剪返回 SIMPLEREGION(2)");
        DeleteObject(clip);
    }

    checkInt(0, GetClipBox(metafile, nullptr), "录制型 DC 的 GetClipBox 空指针返回 ERROR(0)");
    RECT none{};
    checkInt(0, GetClipBox(metafile, &none), "录制型 DC 没有像素表面，GetClipBox 报 ERROR(0)");

    HENHMETAFILE handle = CloseEnhMetaFile(metafile);
    check(handle != nullptr, "关闭录制 DC 得到图元文件句柄");
    if (handle != nullptr) DeleteEnhMetaFile(handle);
}

}  // namespace

int main() {
    testConstantLiterals();
    testRectRegion();
    testCombine();
    testComplexClip();
    testClipRect();
    testSaveRestore();
    testPolygon();
    testObjectAccounting();
    testRecordingDcClip();

    std::printf("Region / clip: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
