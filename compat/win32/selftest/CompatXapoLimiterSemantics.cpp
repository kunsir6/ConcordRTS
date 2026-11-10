// 峰值限幅器 + CXAPOBase 桩的语义自测。
// 目的不是测 DSP 算法本身（那是项目原有的），而是测**本层给的基类**是否
// 让那段真实 DSP 在两个平台上得到同一结论：格式被认可、限幅确实发生、
// Release 真的销毁对象。
#include <windows.h>
#include <xaudio2.h>
#include "audio/backend/RuntimeAudioPeakLimiter.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
static int checks = 0;

static void check(bool condition, const char* what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FAIL  %s\n", what);
    }
}

static WAVEFORMATEX makeFloatStereo(std::uint32_t rate) {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 8;
    format.nAvgBytesPerSec = rate * 8;
    format.cbSize = 0;
    return format;
}

// WAVEFORMATEXTENSIBLE 形态必须在 offset 18 处落到 Samples —— 这是整条
// "cbSize==22" 判据的前提，也是本层最容易悄悄错的地方。
struct Extensible {
    WAVEFORMATEXTENSIBLE format;
    void init(std::uint32_t rate, const GUID& subformat) {
        std::memset(this, 0, sizeof(*this));
        format.Format = makeFloatStereo(rate);
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = 3;  // FL | FR
        format.SubFormat = subformat;
    }
};

int main() {
    IUnknown* raw = nullptr;
    check(SUCCEEDED(createRuntimeAudioPeakLimiter(&raw)) && raw != nullptr,
          "factory returns an object");
    if (raw == nullptr) return 1;

    // 工厂返回的是 CXAPOBase*，通过虚函数走派生类实现。
    auto* effect = dynamic_cast<CXAPOBase*>(raw);
    check(effect != nullptr, "the object really is a CXAPOBase");

    const WAVEFORMATEX floatStereo = makeFloatStereo(48000);
    WAVEFORMATEX* suggested = reinterpret_cast<WAVEFORMATEX*>(0x1);
    check(effect->IsInputFormatSupported(nullptr, &floatStereo, &suggested) == S_OK,
          "32-bit float stereo is accepted as input");
    check(suggested == nullptr, "no alternative format is suggested on success");

    const WAVEFORMATEX mono = [] { auto f = makeFloatStereo(48000); f.nChannels = 1;
                                    f.nBlockAlign = 4; f.nAvgBytesPerSec = 48000 * 4; return f; }();
    check(effect->IsInputFormatSupported(nullptr, &mono, nullptr) == XAPO_E_FORMAT_UNSUPPORTED,
          "mono is rejected");

    const WAVEFORMATEX int16Stereo = [] { auto f = makeFloatStereo(48000);
                                          f.wFormatTag = WAVE_FORMAT_PCM; f.wBitsPerSample = 16;
                                          f.nBlockAlign = 4; f.nAvgBytesPerSec = 48000 * 4; return f; }();
    check(effect->IsInputFormatSupported(nullptr, &int16Stereo, nullptr) == XAPO_E_FORMAT_UNSUPPORTED,
          "16-bit PCM stereo is rejected (this limiter is float-only)");

    const WAVEFORMATEX tooSlow = makeFloatStereo(XAPO_MIN_FRAMERATE - 1);
    check(effect->IsInputFormatSupported(nullptr, &tooSlow, nullptr) == XAPO_E_FORMAT_UNSUPPORTED,
          "below XAPO_MIN_FRAMERATE is rejected");

    // WAVEFORMATEXTENSIBLE：子格式是 IEEE_FLOAT 才通过，PCM 不通过。
    Extensible extensibleFloat;
    extensibleFloat.init(48000, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    check(reinterpret_cast<WAVEFORMATEX*>(&extensibleFloat.format)->cbSize == 22,
          "WAVEFORMATEXTENSIBLE carries cbSize == 22");
    check(effect->IsInputFormatSupported(nullptr, &extensibleFloat.format.Format, nullptr) == S_OK,
          "WAVEFORMATEXTENSIBLE with IEEE_FLOAT subformat is accepted");

    Extensible extensiblePcm;
    extensiblePcm.init(48000, KSDATAFORMAT_SUBTYPE_PCM);
    check(effect->IsInputFormatSupported(nullptr, &extensiblePcm.format.Format, nullptr)
              == XAPO_E_FORMAT_UNSUPPORTED,
          "WAVEFORMATEXTENSIBLE with PCM subformat is rejected");

    XAPO_LOCKFORPROCESS_PARAMETERS lockIn{&floatStereo, 4096};
    XAPO_LOCKFORPROCESS_PARAMETERS lockOut{&floatStereo, 4096};
    check(effect->LockForProcess(1, &lockIn, 1, &lockOut) == S_OK, "LockForProcess succeeds");

    // 1.5 峰值：必须被限到 1.0 以下，且两声道同步衰减（否则左声道削顶右声道不削）。
    std::vector<float> loud{1.5f, -1.5f, 0.75f, -0.75f, 0.5f, 0.5f, 0.25f, -0.25f};
    std::vector<float> out(loud.size(), 0.0f);
    XAPO_PROCESS_BUFFER_PARAMETERS inParams{};
    inParams.pBuffer = loud.data();
    inParams.BufferFlags = XAPO_BUFFER_VALID;
    inParams.ValidFrameCount = static_cast<UINT32>(loud.size() / 2);
    XAPO_PROCESS_BUFFER_PARAMETERS outParams{};
    outParams.pBuffer = out.data();

    effect->Process(1, &inParams, 1, &outParams, TRUE);
    check(outParams.ValidFrameCount == inParams.ValidFrameCount, "frame count is preserved");
    check(outParams.BufferFlags == XAPO_BUFFER_VALID, "buffer flags are propagated");

    float peak = 0.0f;
    for (float sample : out) peak = std::max(peak, std::abs(sample));
    check(peak <= 1.0f, "output peak is limited to <= 1.0");
    check(peak > 0.9f, "the limiter does not squash the signal to silence");
    // 第一帧的左右声道必须同增益：比例与原信号一致（1.5 : -1.5 => 1 : -1）。
    check(std::abs(out[0] + out[1]) < 1e-6f, "both channels share one gain (no per-channel pumping)");
    check(std::abs(out[0]) > 0.0f, "the loud frame is not zeroed");

    // 静音标志应把输出写零，而不是把上一次的增益留在缓冲里。
    std::vector<float> silence(8, 7.0f);
    XAPO_PROCESS_BUFFER_PARAMETERS silentIn{};
    silentIn.pBuffer = silence.data();
    silentIn.BufferFlags = XAPO_BUFFER_SILENT;
    silentIn.ValidFrameCount = 4;
    XAPO_PROCESS_BUFFER_PARAMETERS silentOut{};
    silentOut.pBuffer = silence.data();
    effect->Process(1, &silentIn, 1, &silentOut, TRUE);
    bool allZero = true;
    for (float sample : silence) allZero = allZero && sample == 0.0f;
    check(allZero, "XAPO_BUFFER_SILENT produces zeros");

    // enabled==FALSE 是旁路：逐样本原样输出（这是测试钩子，不是生产路径）。
    std::vector<float> bypass{2.0f, -2.0f};
    XAPO_PROCESS_BUFFER_PARAMETERS bypassIn{};
    bypassIn.pBuffer = bypass.data();
    bypassIn.BufferFlags = XAPO_BUFFER_VALID;
    bypassIn.ValidFrameCount = 1;
    XAPO_PROCESS_BUFFER_PARAMETERS bypassOut{};
    bypassOut.pBuffer = bypass.data();
    effect->Process(1, &bypassIn, 1, &bypassOut, FALSE);
    check(bypass[0] == 2.0f && bypass[1] == -2.0f, "disabled effect passes the signal through");

    // 引用计数：新建时是 1，AddRef 到 2，两次 Release 归零并销毁。
    // 若 CXAPOBase 的 Release 不 delete（最初的设计缺陷），LeakSanitizer 会报泄漏。
    effect->UnlockForProcess();
    const ULONG afterAddRef = raw->AddRef();
    check(afterAddRef == 2, "AddRef returns 2 after the initial reference");
    check(raw->Release() == 1, "first Release leaves one reference");
    check(raw->Release() == 0, "second Release drops to zero");

    // QueryInterface 只认 IUnknown，且必须自增引用计数后由调用方释放。
    IUnknown* second = nullptr;
    check(SUCCEEDED(createRuntimeAudioPeakLimiter(&second)) && second != nullptr,
          "a second object can be created independently");
    if (second != nullptr) {
        void* unknown = nullptr;
        check(second->QueryInterface(IID_IUnknown, &unknown) == S_OK && unknown == second,
              "QueryInterface(IID_IUnknown) returns the same pointer");
        check(second->QueryInterface(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, &unknown) == E_NOINTERFACE,
              "QueryInterface for an unrelated IID fails with E_NOINTERFACE");
        second->Release();  // QI 的那次 AddRef
        second->Release();  // 创建者的那次
    }

    std::printf("  %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
