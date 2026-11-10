// ============================================================================
// compat/win32/include/xapobase.h
//
// XAPO 基类（CXAPOBase）的声明层。与 xaudio2.h 同属 DirectX SDK，不是 Win32 API。
//
// 为什么必须实现而不是只声明：
//   src/audio/backend/RuntimeAudioPeakLimiter.h 是一个**真实的 DSP 实现**
//   （class RuntimeAudioPeakLimiter final : public CXAPOBase）。它是纯数学——
//   峰值检测 + 指数释放，与平台无关。它继承 CXAPOBase 只是为了拿到
//   IXAPO 的接口形状。所以这里必须给出**可实例化的**基类，不能是纯虚壳。
//
// 本层实现了什么、以及为什么这样实现：
//   1. 引用计数。RuntimeVoiceAudio.cpp:96 会写 `limiter->Release()` —— 那是
//      COM 的"创建者交出引用"约定（创建时 refcount=1，CreateMasteringVoice
//      接手后 AddRef，创建者再 Release，剩 1 个由 mastering voice 持有）。
//      所以 Release 必须真的在计数归零时销毁对象，否则每建一次引擎泄漏一个限幅器。
//   2. Is*FormatSupported / LockForProcess：返回成功，并把 *supported 置空表示
//      "不需要更接近的替代格式"。这**不是**无脑放行——格式合法性由派生类的
//      validFormat() 先判过才调到基类，基类在这里只回答"没有意见"。
//   3. Process / Reset 保持纯虚：那是效果器自己的算法，基类无权代答。
//
// 刻意不做的（与不实现 XAudio2Create 同一原则）：
//   · 不实现 GetRegistrationProperties（真实基类用 CoTaskMemAlloc 分配，
//     本层没有对应的释放契约，凭空造一个分配/释放配对只会制造不匹配）。
//   · 不实现 UnlockForProcess 的真实资源回收——本层没有持有任何资源。
//   · 不引用任何"音频引擎"概念：格式协商的真实对局方在 Windows 上是 XAudio2
//     运行时，在 macOS 上根本不存在，所以这里的协商语义只能到"没有意见"为止。
//
// 与真实头文件的差异（刻意的）：
//   · CXAPOBase 声明了 virtual 析构。真实 COM 的 IUnknown 没有虚析构，删除必须
//     由叶子类自己的 Release 完成；但派生类 RuntimeAudioPeakLimiter **没有**重写
//     Release（它只重写算法相关的方法），所以销毁责任只能落在基类。基类要正确
//     销毁派生对象就必须有虚析构。取舍：牺牲"逐字节 vtable 对齐"换"引用计数正确"。
//     本层的 vtable 不跨二进制边界，没有第三方按真实布局读取它。
//   · Registration 指针只存不解释（[[maybe_unused]]）：真实基类会拿它应答
//     GetRegistrationProperties 并校验 Flags，本层没有那条路径。
// ============================================================================
#ifndef CONCORD_COMPAT_XAPOBASE_H
#define CONCORD_COMPAT_XAPOBASE_H

#if defined(_WIN32)
#error "compat/win32/include/xapobase.h is the non-Windows shim; it must never be used on Windows builds."
#endif

#include <windows.h>
#include <mmsystem.h>
#include <atomic>

// ---------------------------------------------------------------------------
// HRESULT 设施
// ---------------------------------------------------------------------------
// FACILITY_XAPO 紧邻 FACILITY_XAUDIO2(0x896)，是 Windows SDK 里的固定值。
#ifndef FACILITY_XAPO
#define FACILITY_XAPO 0x897
#endif
// MAKE_HRESULT(SEVERITY_ERROR, FACILITY_XAPO, 0x01) = 0x88970001
#define XAPO_E_FORMAT_UNSUPPORTED ((HRESULT)0x88970001L)
// 真实头文件里还有一个 XAPO_E_NOT_IMPLEMENTED；本层不提供，因为没有任何调用点
// 需要它，多一个未使用的错误码只会让人以为某条路径"已支持"。

// ---------------------------------------------------------------------------
// 帧率上下界。取值与 XAUDIO2_MIN_SAMPLE_RATE / XAUDIO2_MAX_SAMPLE_RATE 相同——
// 不是巧合：XAPO 的帧率边界就是 XAudio2 的采样率边界。
// ---------------------------------------------------------------------------
#define XAPO_MIN_FRAMERATE 1000
#define XAPO_MAX_FRAMERATE 200000

// ---------------------------------------------------------------------------
// 效果器标志位
// ---------------------------------------------------------------------------
enum XAPO_FLAG {
    XAPO_FLAG_INPLACE_REQUIRED = 1,
    XAPO_FLAG_CHANNELS_MUST_MATCH = 2,
    XAPO_FLAG_FRAMERATE_MUST_MATCH = 4,
    XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH = 8,
    XAPO_FLAG_BUFFERCOUNT_MUST_MATCH = 0x10,
    XAPO_FLAG_INPLACE_SUPPORTED = 0x20
};

// 真实 xapobase.h 把这个宏定义为 INPLACE_SUPPORTED|INPLACE_REQUIRED 的组合。
// **诚实标注**：本机没有 Windows SDK 可核对这个数值。它是惰性的——本层的
// CXAPOBase 不读 Flags、不做校验，所以数值偏差不会影响 macOS 行为；但若将来
// 接了真实的 XAudio2 运行时，这个值会参与效果器注册校验，届时必须对 SDK 复核。
#define XAPOBASE_DEFAULT_FLAG (XAPO_FLAG_INPLACE_SUPPORTED | XAPO_FLAG_INPLACE_REQUIRED)

// ---------------------------------------------------------------------------
// 缓冲区标志
// ---------------------------------------------------------------------------
enum XAPO_BUFFER_FLAGS {
    XAPO_BUFFER_SILENT = 0,
    XAPO_BUFFER_VALID = 1
};

// ---------------------------------------------------------------------------
// 结构体。字段顺序与 Windows SDK 一致——聚合初始化按位置绑定，
// 顺序错了不报错，只会把 FriendlyName 写进 CopyrightInfo。
// ---------------------------------------------------------------------------
struct XAPO_REGISTRATION_PROPERTIES {
    CLSID clsid;
    LPCWSTR FriendlyName;
    LPCWSTR CopyrightInfo;
    UINT32 MajorVersion;
    UINT32 MinorVersion;
    UINT32 Flags;
    UINT32 InputLockedFormat;
    UINT32 InputLockedMaxFrameCount;
    UINT32 OutputLockedFormat;
    UINT32 OutputLockedMaxFrameCount;
};

struct XAPO_LOCKFORPROCESS_PARAMETERS {
    const WAVEFORMATEX* pFormat;
    UINT32 MaxFrameCount;
};

struct XAPO_PROCESS_BUFFER_PARAMETERS {
    void* pBuffer;
    XAPO_BUFFER_FLAGS BufferFlags;
    UINT32 ValidFrameCount;
    BOOL DeviceDiscontinuity;
};

// 指针宽度：真实 SDK 里 64 位上 XAPO_LOCKFORPROCESS_PARAMETERS 是 16 字节
// （8 + 4 + 4 padding），XAPO_PROCESS_BUFFER_PARAMETERS 是 24 字节。钉住它们，
// 免得将来有人把 UINT32 换成 size_t 而让数组步长静默改变。
static_assert(sizeof(XAPO_LOCKFORPROCESS_PARAMETERS) == 16,
              "XAPO_LOCKFORPROCESS_PARAMETERS is 16 bytes on 64-bit Windows");
static_assert(sizeof(XAPO_PROCESS_BUFFER_PARAMETERS) == 24,
              "XAPO_PROCESS_BUFFER_PARAMETERS is 24 bytes on 64-bit Windows");
static_assert(sizeof(XAPO_REGISTRATION_PROPERTIES) == 64,
              "XAPO_REGISTRATION_PROPERTIES is 64 bytes on 64-bit Windows "
              "(16 clsid + 8 + 8 + 28 payload, padded to 8-byte alignment)");

// ---------------------------------------------------------------------------
// CXAPOBase
// ---------------------------------------------------------------------------
class CXAPOBase : public IUnknown {
public:
    explicit CXAPOBase(const XAPO_REGISTRATION_PROPERTIES* registration) noexcept
        : registration_(registration) {}

    // 见文件头：虚析构是为了让 Release() 能正确销毁派生对象（派生类不重写 Release）。
    virtual ~CXAPOBase() = default;

    HRESULT QueryInterface(const GUID& iid, void** object) noexcept override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (IsEqualGUID(iid, IID_IUnknown)) {
            *object = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        // 只认 IUnknown：本层没有 IXAPO / IPXAPO 的独立 IID，也没有任何代码
        // 按接口查询。返回失败比返回一个"看起来能用的别的接口"安全。
        return E_NOINTERFACE;
    }

    ULONG AddRef() noexcept override { return ++references_; }

    ULONG Release() noexcept override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    // 基类对格式协商"没有意见"：合法性已由派生类的 validFormat() 判过。
    // *supported 置空 = 不需要更接近的替代格式（真实 API 里 S_OK + 非空
    // *ppSupportedFormat 表示"你这个不行，但换成这个就行"）。
    virtual HRESULT IsInputFormatSupported(const WAVEFORMATEX* output,
                                           const WAVEFORMATEX* input,
                                           WAVEFORMATEX** supported) noexcept {
        (void)output;
        (void)input;
        if (supported != nullptr) *supported = nullptr;
        return S_OK;
    }

    virtual HRESULT IsOutputFormatSupported(const WAVEFORMATEX* input,
                                            const WAVEFORMATEX* output,
                                            WAVEFORMATEX** supported) noexcept {
        (void)input;
        (void)output;
        if (supported != nullptr) *supported = nullptr;
        return S_OK;
    }

    // 真实基类在这里分配内部处理状态、校验 in/out 格式一致。本层没有状态可分配，
    // 也没有对局方会来查询"是否已锁定"，所以只回答成功。
    virtual HRESULT LockForProcess(UINT32 inputCount,
                                   const XAPO_LOCKFORPROCESS_PARAMETERS* input,
                                   UINT32 outputCount,
                                   const XAPO_LOCKFORPROCESS_PARAMETERS* output) noexcept {
        (void)inputCount;
        (void)input;
        (void)outputCount;
        (void)output;
        return S_OK;
    }

    virtual void UnlockForProcess() noexcept {}

    // 算法本体归派生类。保持纯虚而不是给个空实现——空实现会让"忘了重写"
    // 变成"处理了但输出全零"，那是比编译错误难查得多的失效。
    virtual void Reset() noexcept = 0;
    virtual void Process(UINT32 inputCount,
                         const XAPO_PROCESS_BUFFER_PARAMETERS* input,
                         UINT32 outputCount,
                         XAPO_PROCESS_BUFFER_PARAMETERS* output,
                         BOOL enabled) noexcept = 0;

protected:
    // 只存不解释：本层不提供 GetRegistrationProperties（分配契约无处兑现，
    // 见文件头）。加 [[maybe_unused]] 是为了说明"这里故意没有读者"，
    // 而不是"忘了用"。
    [[maybe_unused]] const XAPO_REGISTRATION_PROPERTIES* registration_ = nullptr;

private:
    std::atomic<ULONG> references_{1};  // COM 约定：新建对象持有 1 个引用
};

#endif  // CONCORD_COMPAT_XAPOBASE_H
