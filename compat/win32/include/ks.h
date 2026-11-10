// ============================================================================
// compat/win32/include/ks.h
//
// Kernel Streaming（KS）的**极小声明层**。
//
// 它存在的唯一理由：src/audio/backend/RuntimeAudioPeakLimiter.h 里
// `#include <ks.h>`。而那个头之所以包含 ks.h，是因为真实 XAPO 的格式描述
// （KSDATAFORMAT_SUBTYPE_*）在 Windows SDK 里定义于 ksmedia.h，而 ksmedia.h
// 又依赖 ks.h 的 KSIDENTIFIER。
//
// 本层只写 KSIDENTIFIER —— 项目实际用到的 0 个 KS 类型，除此之外一个都没有。
// 刻意不写：KSPIN_DESCRIPTOR / KSDATARANGE / KSFILTER_DESCRIPTOR / IOCTL 码。
// 那些一旦写进来就会引诱人去"顺便实现"，而 KS 设备栈在 macOS 上根本没有对应物。
//
// 关于 KSIDENTIFIER 的形态差异：真实 ks.h 用 union 让 Set/Id/Flags 与 GUID
// 的 Data1/Data2 重叠（16 字节整体）。这里写成扁平结构体 —— 布局同为 16 字节，
// 但没有 union 的重叠别名语义。没有任何代码访问 Id/Flags 以外的重叠部分，
// 所以差异不可观测；若将来要读 Set.Data1 之类，必须先回来把 union 补上。
// ============================================================================
#ifndef CONCORD_COMPAT_KS_H
#define CONCORD_COMPAT_KS_H

#if defined(_WIN32)
#error "compat/win32/include/ks.h is the non-Windows shim; it must never be used on Windows builds."
#endif

#include <windows.h>

// 关于形态差异：真实 ks.h 里 KSIDENTIFIER 是一个 union，让 {Id,Flags} 与 GUID
// 的前 8 个字节重叠（整体仍是 16 字节）。这里写成「GUID + 两个访问器」：
// 布局同为 16 字节，但换成匿名 struct 会引入非标准扩展，而写成一个并列的
// 扁平结构体会变成 24 字节 —— 那正是最初写错的地方（static_assert 抓到的）。
// 访问器的字节序假设：Id 覆盖 Data1，Flags 覆盖 Data2|Data3<<16（小端布局），
// 与 x86/arm64 上真实 union 的解释一致。当前没有任何代码读 Id/Flags。
struct KSIDENTIFIER {
    GUID Set;

    ULONG id() const noexcept { return Set.Data1; }
    ULONG flags() const noexcept {
        return static_cast<ULONG>(Set.Data2) | (static_cast<ULONG>(Set.Data3) << 16);
    }
};
using PKIDENTIFIER = KSIDENTIFIER*;
using LPKIDENTIFIER = KSIDENTIFIER*;

static_assert(sizeof(KSIDENTIFIER) == 16, "KSIDENTIFIER is 16 bytes (GUID-sized) in the Windows SDK");

#endif  // CONCORD_COMPAT_KS_H
