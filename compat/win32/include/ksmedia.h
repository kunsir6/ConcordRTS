// ============================================================================
// compat/win32/include/ksmedia.h
//
// KS 的媒体格式描述：WAVEFORMATEXTENSIBLE / KSDATAFORMAT / KSDATAFORMAT_SUBTYPE_*。
//
// 引入原因与本层范围：src/audio/backend/RuntimeAudioPeakLimiter.h 是**真实的
// DSP 实现**（116 行峰值限幅器，纯数学、平台无关），它需要判断
// "输入格式是不是 32-bit float 立体声"，而做这个判断在 Windows SDK 里靠
// WAVEFORMATEXTENSIBLE + KSDATAFORMAT_SUBTYPE_IEEE_FLOAT。所以这几个类型必须
// 有**真实取值**，不能是空壳：SubFormat 的 GUID 写错，validFormat() 会把
// 合法格式判成不合法，限幅器就永远不生效——一种完全静默的失效。
//
// 刻意不写：KSDATARANGE_AUDIO / KSDATAFORMAT_SPECIFIER_* 之外的 KS 数据范围族、
// KSAUDIO_* 属性集。没有一个调用点用到它们。
// ============================================================================
#ifndef CONCORD_COMPAT_KSMEDIA_H
#define CONCORD_COMPAT_KSMEDIA_H

#if defined(_WIN32)
#error "compat/win32/include/ksmedia.h is the non-Windows shim; it must never be used on Windows builds."
#endif

#include <windows.h>
#include <mmsystem.h>
#include <ks.h>

// ---------------------------------------------------------------------------
// WAVEFORMATEXTENSIBLE
//
// 只有 cbSize == 22 时才是合法的 WAVEFORMATEXTENSIBLE（18 + 22 = 40 字节）。
// 调用点正是这么判的（RuntimeAudioPeakLimiter.h 的 validFormat），所以
// sizeof 必须精确 —— 下面有 static_assert 钉住。
// ---------------------------------------------------------------------------
struct WAVEFORMATEXTENSIBLE {
    WAVEFORMATEX Format;
    union {
        WORD wValidBitsPerSample;
        WORD wSamplesPerBlock;
        WORD wReserved;
    } Samples;
    DWORD dwChannelMask;
    GUID SubFormat;
};
using PWAVEFORMATEXTENSIBLE = WAVEFORMATEXTENSIBLE*;
using LPWAVEFORMATEXTENSIBLE = WAVEFORMATEXTENSIBLE*;

static_assert(sizeof(WAVEFORMATEX) == 18, "WAVEFORMATEX is packed to 18 bytes in the Windows SDK");
static_assert(sizeof(WAVEFORMATEXTENSIBLE) == 40,
              "WAVEFORMATEXTENSIBLE is 40 bytes; cbSize must be 22 (40 - 18)");
static_assert(offsetof(WAVEFORMATEXTENSIBLE, SubFormat) == 24,
              "SubFormat sits at byte 24 in the Windows SDK layout");

// ---------------------------------------------------------------------------
// KSDATAFORMAT：KS 数据格式头。项目不构造它，但它是 ksmedia.h 的命名主体，
// 且布局（64 字节）能钉住"GUID 是 16 字节"这件事在 ksmedia 上下文里也成立。
// ---------------------------------------------------------------------------
struct KSDATAFORMAT {
    std::uint32_t FormatSize;
    std::uint32_t Flags;
    std::uint32_t SampleSize;
    std::uint32_t Reserved;
    GUID MajorFormat;
    GUID SubFormat;
    GUID Specifier;
};
using PKSDATAFORMAT = KSDATAFORMAT*;
using LPKSDATAFORMAT = KSDATAFORMAT*;

static_assert(sizeof(KSDATAFORMAT) == 64, "KSDATAFORMAT is 64 bytes in the Windows SDK");

// ---------------------------------------------------------------------------
// 子格式 GUID。取值来自 Windows SDK 的 DEFINE_GUIDSTRUCT 字符串形式，
// 三个都是 "??????-0000-0010-8000-00aa00389b71" 家族，只有 Data1 不同。
// ---------------------------------------------------------------------------
inline constexpr GUID KSDATAFORMAT_SUBTYPE_WAVEFORMATEX{
    0x00000000u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
inline constexpr GUID KSDATAFORMAT_SUBTYPE_PCM{
    0x00000001u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
inline constexpr GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT{
    0x00000003u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// 三者必须互不相等，否则 validFormat() 的 IEEE_FLOAT 判定形同虚设。
// 用运行时比较写成 constexpr 表达式：GUID 是聚合类型，比较函数不是 constexpr，
// 所以这里手写一个只在本文件生效的逐字段比较。
namespace concord_ksmedia_detail {
constexpr bool sameGuid(const GUID& left, const GUID& right) {
    return left.Data1 == right.Data1 && left.Data2 == right.Data2 && left.Data3 == right.Data3
        && left.Data4[0] == right.Data4[0] && left.Data4[1] == right.Data4[1]
        && left.Data4[2] == right.Data4[2] && left.Data4[3] == right.Data4[3]
        && left.Data4[4] == right.Data4[4] && left.Data4[5] == right.Data4[5]
        && left.Data4[6] == right.Data4[6] && left.Data4[7] == right.Data4[7];
}
}  // namespace concord_ksmedia_detail

static_assert(!concord_ksmedia_detail::sameGuid(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
                                                KSDATAFORMAT_SUBTYPE_PCM),
              "IEEE_FLOAT and PCM subformats must differ only-never-equal");
static_assert(concord_ksmedia_detail::sameGuid(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
                                               KSDATAFORMAT_SUBTYPE_IEEE_FLOAT),
              "sameGuid must be reflexive, otherwise the distinctness check above proves nothing");

#endif  // CONCORD_COMPAT_KSMEDIA_H
