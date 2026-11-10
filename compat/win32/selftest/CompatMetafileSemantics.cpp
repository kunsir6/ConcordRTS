// compat/win32 自测 · 增强图元文件（EMF）录制与回放
//
// 这一层的用途不是"画图"，而是**审计**：把一次重绘录成指令流，再回放检查
// "是不是只提交了一次完整帧拷贝"。截屏看不出中间黑帧，指令流看得出。
//
// 因此本文件的重点断言不是"EnumEnhMetaFile 返回 TRUE"，而是：
//   1. 一次正常重绘 → 回放看到恰好 1 条 EMR_BITBLT，且内嵌的像素与源逐字节一致；
//   2. 多做一次拷贝（人为制造中间帧）→ 回放必须看到 2 条，即审计真的能抓到，
//      而不是"永远只有 1 条"的空断言；
//   3. 空录制 → 0 条拷贝，审计判失败（不是判通过）。
//
// 第 2、3 条是本文件存在的理由：一个只打印"全部通过"的桩也能让第 1 条过。
//
// 构建与运行：
//   c++ -std=c++20 -I compat/win32/include \
//       compat/win32/src/RuntimeWin32CompatGdi.cpp \
//       compat/win32/selftest/CompatMetafileSemantics.cpp -o /tmp/ccmf && /tmp/ccmf

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

void checkEqual(long long expected, long long actual, const char* description) {
    ++gChecks;
    if (expected != actual) {
        ++gFailures;
        std::printf("  FAIL  %s  (expected %lld, got %lld)\n", description, expected, actual);
    }
}

// 与 7 个 diagnostics 文件里的写法一致：一张 32bpp top-down DIB + 一个内存 DC。
struct Dib {
    HBITMAP handle = nullptr;
    void* bits = nullptr;
    HDC dc = nullptr;
    int width = 0;
    int height = 0;

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

    void destroy() {
        if (dc != nullptr) DeleteDC(dc);
        if (handle != nullptr) DeleteObject(reinterpret_cast<HGDIOBJ>(handle));
        dc = nullptr;
        handle = nullptr;
    }

    // 填一个可区分的图案：每个像素值都唯一，落错一个字节就能被发现。
    void fillPattern(std::uint32_t seed) {
        auto* pixels = static_cast<std::uint32_t*>(bits);
        for (int i = 0; i < width * height; ++i) {
            pixels[i] = 0xFF000000u | ((seed + static_cast<std::uint32_t>(i) * 2654435761u) & 0x00FFFFFFu);
        }
    }
};

// 与 RuntimeCampaignMenuUi.inl 的 CampaignPaintAudit 同形。刻意照抄，因为要
// 验证的正是"产品代码的那套判据在这套记录流上给出的结论"。
struct PaintAudit {
    POINT size{};
    int copies = 0;
    int footers = 0;
    int headers = 0;
    bool completeFrameOnly = true;
    const ENHMETARECORD* blit = nullptr;
};

int CALLBACK auditPaintRecord(HDC, HANDLETABLE*, const ENHMETARECORD* record, int nObjects,
                              LPARAM data) {
    auto& audit = *reinterpret_cast<PaintAudit*>(data);
    if (nObjects != 0) audit.completeFrameOnly = false;
    if (record->iType == EMR_BITBLT) {
        const auto& copy = *reinterpret_cast<const EMRBITBLT*>(record);
        ++audit.copies;
        audit.blit = record;
        audit.completeFrameOnly = audit.completeFrameOnly && copy.dwRop == SRCCOPY &&
                                  copy.cbBmiSrc > 0 && copy.cbBitsSrc > 0 && copy.xDest == 0 &&
                                  copy.yDest == 0 && copy.cxDest == audit.size.x &&
                                  copy.cyDest == audit.size.y;
    } else if (record->iType == EMR_EOF) {
        ++audit.footers;
    } else if (record->iType == EMR_HEADER) {
        ++audit.headers;
    } else {
        audit.completeFrameOnly = false;
    }
    return 1;
}

int CALLBACK stopAfterFirst(HDC, HANDLETABLE*, const ENHMETARECORD*, int, LPARAM) { return 0; }

// 把源位图整幅拷进录制 DC，模拟"提交完整帧"。
bool recordOneFrame(Dib& source, HENHMETAFILE& out, int copies = 1) {
    HDC recording = CreateEnhMetaFileW(nullptr, nullptr, nullptr, nullptr);
    if (recording == nullptr) return false;
    for (int i = 0; i < copies; ++i) {
        if (!BitBlt(recording, 0, 0, source.width, source.height, source.dc, 0, 0, SRCCOPY)) {
            CloseEnhMetaFile(recording);
            return false;
        }
    }
    out = CloseEnhMetaFile(recording);
    return out != nullptr;
}

// ---------------------------------------------------------------------------
// 1. 单帧：回放看到恰好一条 EMR_BITBLT，内嵌像素与源逐字节一致
// ---------------------------------------------------------------------------
void testSingleFrame() {
    std::printf("[1] 单帧录制与回放\n");
    Dib source;
    if (!source.create(800, 600)) {
        check(false, "构造 800x600 源位图");
        return;
    }
    source.fillPattern(12345);

    HENHMETAFILE frame = nullptr;
    check(recordOneFrame(source, frame), "录制一帧并 CloseEnhMetaFile");

    PaintAudit audit{};
    audit.size = POINT{800, 600};
    if (frame != nullptr) {
        check(EnumEnhMetaFile(nullptr, frame, auditPaintRecord, &audit, nullptr) == TRUE,
              "EnumEnhMetaFile 返回 TRUE");
        checkEqual(1, audit.headers, "记录流里恰好 1 条 EMR_HEADER");
        checkEqual(1, audit.copies, "记录流里恰好 1 条 EMR_BITBLT");
        checkEqual(1, audit.footers, "记录流里恰好 1 条 EMR_EOF");
        check(audit.completeFrameOnly,
              "审计判据（SRCCOPY + 有 BMI/像素 + 目标为整幅 800x600）成立");
    }

    // 记录流必须自带像素：EMF 是自包含的，回放时源位图早就没了。
    if (audit.blit != nullptr) {
        const auto* record = reinterpret_cast<const EMRBITBLT*>(audit.blit);
        const auto* base = reinterpret_cast<const std::uint8_t*>(record);
        const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(base + record->offBmiSrc);
        checkEqual(40, header->biSize, "内嵌 BMI 的 biSize == 40");
        checkEqual(800, header->biWidth, "内嵌 BMI 的 biWidth == 800");
        checkEqual(-600, header->biHeight, "内嵌 BMI 的高度为负（top-down，与源行序一致）");
        checkEqual(32, header->biBitCount, "内嵌 BMI 是 32bpp");
        checkEqual(BI_RGB, header->biCompression, "内嵌 BMI 未压缩");
        checkEqual(800 * 4 * 600, record->cbBitsSrc, "内嵌像素字节数 == 800*600*4");
        checkEqual(record->offBmiSrc + record->cbBmiSrc, record->offBitsSrc,
                   "offBitsSrc 紧接 BMI 之后");

        const auto* recorded = base + record->offBitsSrc;
        const auto* original = static_cast<const std::uint8_t*>(source.bits);
        check(std::memcmp(recorded, original, static_cast<std::size_t>(800 * 4 * 600)) == 0,
              "内嵌像素与源位图逐字节一致");
    }

    // 头部字段自洽：nBytes / nRecords 与实际流长必须对得上，否则真实回放器会在
    // 读到一半时断言失败。
    if (frame != nullptr) {
        std::uint32_t nBytes = 0;
        std::uint32_t nRecords = 0;
        struct HeaderProbe {
            HDC dc;
            std::uint32_t* bytes;
            std::uint32_t* records;
        } probe{nullptr, &nBytes, &nRecords};
        auto readHeader = [](HDC, HANDLETABLE*, const ENHMETARECORD* record, int, LPARAM data) {
            auto& p = *reinterpret_cast<HeaderProbe*>(data);
            if (record->iType == EMR_HEADER) {
                const auto* header = reinterpret_cast<const ENHMETAHEADER*>(record);
                *p.bytes = header->nBytes;
                *p.records = header->nRecords;
                checkEqual(108, header->nSize, "ENHMETAHEADER.nSize == 108");
                checkEqual(0x00010000, header->nVersion, "nVersion == 0x00010000");
                check(header->dSignature == ENHMETA_SIGNATURE, "dSignature == ' EMF'");
                check(header->rclBounds.right == 800 && header->rclBounds.bottom == 600,
                      "rclBounds 覆盖 800x600");
            }
            return 1;
        };
        EnumEnhMetaFile(nullptr, frame, readHeader, &probe, nullptr);
        checkEqual(3, nRecords, "nRecords == 3（头部 + 1 条记录 + EOF）");
        // 108（头部） + 100（EMRBITBLT） + 40（BMI） + 800*600*4（像素） + 20（EOF）
        checkEqual(108 + 100 + 40 + 800 * 600 * 4 + 20, nBytes, "nBytes 与逐段长度之和一致");
    }

    if (frame != nullptr) check(DeleteEnhMetaFile(frame) == TRUE, "DeleteEnhMetaFile 成功");
    check(DeleteEnhMetaFile(frame) == FALSE, "重复 DeleteEnhMetaFile 返回 FALSE");
    source.destroy();
}

// ---------------------------------------------------------------------------
// 2. 两帧：审计必须看到 2 条并判失败——证明它是真的在数，不是恒定 1
// ---------------------------------------------------------------------------
void testIntermediateFrameIsDetected() {
    std::printf("[2] 中间帧能被审计发现\n");
    Dib source;
    if (!source.create(320, 240)) {
        check(false, "构造 320x240 源位图");
        return;
    }
    source.fillPattern(777);

    HENHMETAFILE frame = nullptr;
    check(recordOneFrame(source, frame, 2), "连续两次 BitBlt 后 CloseEnhMetaFile");
    if (frame != nullptr) {
        PaintAudit audit{};
        audit.size = POINT{320, 240};
        EnumEnhMetaFile(nullptr, frame, auditPaintRecord, &audit, nullptr);
        checkEqual(2, audit.copies, "两次提交 → 回放看到 2 条 EMR_BITBLT");
        check(audit.copies != 1, "产品侧的 copies != 1 判据会因此报错（即审计有效）");
        DeleteEnhMetaFile(frame);
    }
    source.destroy();
}

// ---------------------------------------------------------------------------
// 3. 空录制：0 条拷贝，审计判失败而不是默认通过
// ---------------------------------------------------------------------------
void testEmptyRecording() {
    std::printf("[3] 空录制\n");
    HDC recording = CreateEnhMetaFileW(nullptr, nullptr, nullptr, nullptr);
    check(recording != nullptr, "CreateEnhMetaFileW 成功");
    HENHMETAFILE frame = CloseEnhMetaFile(recording);
    check(frame != nullptr, "不画任何东西也能 CloseEnhMetaFile");
    if (frame != nullptr) {
        PaintAudit audit{};
        audit.size = POINT{100, 100};
        check(EnumEnhMetaFile(nullptr, frame, auditPaintRecord, &audit, nullptr) == TRUE,
              "空图元文件仍可枚举");
        checkEqual(0, audit.copies, "空录制 → 0 条 EMR_BITBLT");
        checkEqual(2, audit.headers + audit.footers, "空录制只有头部与 EOF 两条记录");
        DeleteEnhMetaFile(frame);
    }
}

// ---------------------------------------------------------------------------
// 4. StretchBlt 记录为 EMR_STRETCHBLT，并带上源尺寸
// ---------------------------------------------------------------------------
void testStretchRecord() {
    std::printf("[4] StretchBlt 记录\n");
    Dib source;
    if (!source.create(64, 48)) {
        check(false, "构造 64x48 源位图");
        return;
    }
    source.fillPattern(4242);

    HDC recording = CreateEnhMetaFileW(nullptr, nullptr, nullptr, nullptr);
    check(recording != nullptr, "CreateEnhMetaFileW 成功");
    check(StretchBlt(recording, 0, 0, 800, 600, source.dc, 0, 0, 64, 48, SRCCOPY) == TRUE,
          "StretchBlt 到录制 DC 成功");

    struct StretchProbe {
        int seen = 0;
        LONG cxSrc = 0;
        LONG cySrc = 0;
        LONG cxDest = 0;
        LONG cyDest = 0;
        DWORD iType = 0;
    } probe;
    auto readStretch = [](HDC, HANDLETABLE*, const ENHMETARECORD* record, int, LPARAM data) {
        auto& p = *reinterpret_cast<StretchProbe*>(data);
        if (record->iType == EMR_STRETCHBLT) {
            const auto* stretch = reinterpret_cast<const EMRSTRETCHBLT*>(record);
            ++p.seen;
            p.iType = record->iType;
            p.cxSrc = stretch->cxSrc;
            p.cySrc = stretch->cySrc;
            p.cxDest = stretch->cxDest;
            p.cyDest = stretch->cyDest;
        }
        return 1;
    };
    HENHMETAFILE frame = CloseEnhMetaFile(recording);
    check(frame != nullptr, "CloseEnhMetaFile 成功");
    if (frame != nullptr) {
        EnumEnhMetaFile(nullptr, frame, readStretch, &probe, nullptr);
        checkEqual(1, probe.seen, "回放看到 1 条 EMR_STRETCHBLT");
        checkEqual(EMR_STRETCHBLT, probe.iType, "记录类型是 EMR_STRETCHBLT 而不是 EMR_BITBLT");
        checkEqual(64, probe.cxSrc, "cxSrc 记录的是源宽 64");
        checkEqual(48, probe.cySrc, "cySrc 记录的是源高 48");
        checkEqual(800, probe.cxDest, "cxDest 记录的是目标宽 800");
        checkEqual(600, probe.cyDest, "cyDest 记录的是目标高 600");
        DeleteEnhMetaFile(frame);
    }
    source.destroy();
}

// ---------------------------------------------------------------------------
// 5. 失败路径：宁可返回 FALSE，也不要写一条回放时才暴露缺数据的记录
// ---------------------------------------------------------------------------
void testRecordedFailures() {
    std::printf("[5] 录制型 DC 的失败路径\n");
    Dib source;
    if (!source.create(32, 32)) {
        check(false, "构造 32x32 源位图");
        return;
    }
    source.fillPattern(1);

    // 写文件的 EMF：本层不支持，必须 nullptr 而不是"返回一个句柄但什么都没落盘"。
    check(CreateEnhMetaFileW(nullptr, L"/tmp/concord-compat-selftest.emf", nullptr, nullptr) ==
              nullptr,
          "写文件的 CreateEnhMetaFileW 返回 nullptr");

    HDC recording = CreateEnhMetaFileW(nullptr, nullptr, nullptr, nullptr);
    // DSTINVERT 需要目标像素，录制型 DC 没有像素表面：录制时就失败。
    check(BitBlt(recording, 0, 0, 32, 32, source.dc, 0, 0, DSTINVERT) == FALSE,
          "DSTINVERT 到录制 DC 返回 FALSE");
    // 源 DC 没有选位图。
    HDC emptySource = CreateCompatibleDC(nullptr);
    check(BitBlt(recording, 0, 0, 32, 32, emptySource, 0, 0, SRCCOPY) == FALSE,
          "源 DC 无位图时返回 FALSE");
    DeleteDC(emptySource);
    // SetStretchBltMode(HALFTONE) 后不允许录制伸缩——EMR_STRETCHBLT 不带该模式。
    HDC halftone = CreateEnhMetaFileW(nullptr, nullptr, nullptr, nullptr);
    SetStretchBltMode(halftone, HALFTONE);
    check(StretchBlt(halftone, 0, 0, 32, 32, source.dc, 0, 0, 32, 32, SRCCOPY) == FALSE,
          "HALFTONE 伸缩到录制 DC 返回 FALSE");
    HENHMETAFILE discarded = CloseEnhMetaFile(halftone);
    if (discarded != nullptr) DeleteEnhMetaFile(discarded);

    // 回调返回 0 → 枚举中止，整体返回 FALSE（与 Win32 一致）。
    BitBlt(recording, 0, 0, 32, 32, source.dc, 0, 0, SRCCOPY);
    HENHMETAFILE frame = CloseEnhMetaFile(recording);
    check(frame != nullptr, "CloseEnhMetaFile 成功");
    if (frame != nullptr) {
        check(EnumEnhMetaFile(nullptr, frame, stopAfterFirst, nullptr, nullptr) == FALSE,
              "回调返回 0 时 EnumEnhMetaFile 返回 FALSE");
        check(EnumEnhMetaFile(nullptr, frame, nullptr, nullptr, nullptr) == FALSE,
              "回调为空时返回 FALSE");
        DeleteEnhMetaFile(frame);
    }
    // CloseEnhMetaFile 之后录制 DC 已随图元文件一起销毁。
    check(CloseEnhMetaFile(recording) == nullptr, "重复 CloseEnhMetaFile 返回 nullptr");
    source.destroy();
}

}  // namespace

int main() {
    std::printf("compat/win32 EMF 语义自测（录制 + 回放 + 审计）\n\n");
    testSingleFrame();
    testIntermediateFrameIsDetected();
    testEmptyRecording();
    testStretchRecord();
    testRecordedFailures();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
