// ============================================================================
// compat/win32/include/xaudio2.h
//
// XAudio2 的**声明层**。它不是 Win32 API，是 DirectX SDK 里的一套 COM 接口
// （IXAudio2 / IXAudio2MasteringVoice / IXAudio2SourceVoice + XAudio2Create），
// 所以 S1 的 windows.h 覆盖不到它——这就是为什么 P2-04 曾经被误标为 superseded。
//
// 本层只做一件事：让 src/audio/backend/RuntimeVoiceAudio.cpp 编译通过。
// 接口方法全是纯虚函数，虚调用不需要链接符号；真正需要实现的是工厂
// XAudio2Create —— 它是本轮刻意留下的**链接期缺口**，由 P4-01 的 SDL3 音频
// 后端兑现。理由：假实现一个 "永远创建失败" 的引擎会让 ready()==false 看起来
// 像"设备不可用"，而设备不可用与"没实现"是两件必须分开记录的事。
//
// 与真实 xaudio2.h 的差异（刻意的，全部记录在案）：
//   · 只声明本项目实际用到的 10 个方法，不是完整接口；未声明的方法若被调用会
//     直接编译失败，而不是悄悄走到一个空实现上。
//   · 不引入 IXAudio2VoiceCallback / XAUDIO2_DEBUG_CONFIGURATION 等未使用设施。
//   · 保留真实的方法顺序无关紧要（纯虚函数，调用按名字解析）。
// ============================================================================
#ifndef CONCORD_COMPAT_XAUDIO2_H
#define CONCORD_COMPAT_XAUDIO2_H

#if defined(_WIN32)
#error "compat/win32/include/xaudio2.h must not be on the include path when targeting Windows"
#endif

#include <windows.h>
#include <mmsystem.h>

// ---------------------------------------------------------------------------
// 常量。取值必须与真实 xaudio2.h 一致——写错不会报错，只会让"循环播放"
// 变成"播一次就停"这类安静的行为差异。
// ---------------------------------------------------------------------------
#define XAUDIO2_DEFAULT_PROCESSOR 0x00000000
#define XAUDIO2_END_OF_STREAM 0x0040
#define XAUDIO2_LOOP_INFINITE 255
#define XAUDIO2_VOICE_NOSAMPLESPLAYED 0x0100
#define XAUDIO2_COMMIT_NOW 0
#define XAUDIO2_DEFAULT_CHANNELS 0
#define XAUDIO2_DEFAULT_SAMPLERATE 0
#define XAUDIO2_DEFAULT_FREQ_RATIO 2.0f
#define XAUDIO2_MIN_SAMPLE_RATE 1000
#define XAUDIO2_MAX_SAMPLE_RATE 200000

enum AUDIO_STREAM_CATEGORY {
    AudioCategory_Other = 0,
    AudioCategory_ForegroundOnlyMedia = 1,
    AudioCategory_BackgroundCapableMedia = 2,
    AudioCategory_Communications = 3,
    AudioCategory_Alerts = 4,
    AudioCategory_SoundEffects = 5,
    AudioCategory_GameEffects = 6,
    AudioCategory_GameMedia = 7,
    AudioCategory_GameChat = 8,
    AudioCategory_Speech = 9,
    AudioCategory_Movie = 10,
    AudioCategory_Media = 11
};

// ---------------------------------------------------------------------------
// 结构体
// ---------------------------------------------------------------------------
struct XAUDIO2_BUFFER {
    UINT32 Flags;
    UINT32 AudioBytes;
    const BYTE* pAudioData;
    UINT32 PlayBegin;
    UINT32 PlayLength;
    UINT32 LoopBegin;
    UINT32 LoopLength;
    UINT32 LoopCount;
    void* pContext;
};

struct XAUDIO2_BUFFER_WMA {
    const UINT32* pDecodedPacketCumulativeBytes;
    UINT32 PacketCount;
};

struct XAUDIO2_VOICE_STATE {
    void* pCurrentBufferContext;
    UINT32 BuffersQueued;
    UINT64 SamplesPlayed;
};

struct XAUDIO2_EFFECT_DESCRIPTOR {
    IUnknown* pEffect;
    BOOL InitialState;
    UINT32 OutputChannels;
};

struct XAUDIO2_EFFECT_CHAIN {
    UINT32 EffectCount;
    XAUDIO2_EFFECT_DESCRIPTOR* pEffectDescriptor;
};

// ---------------------------------------------------------------------------
// 接口。全部纯虚：派生对象由 XAudio2Create 在 Windows 侧提供；
// 这里只是让调用点的签名有据可依。
// ---------------------------------------------------------------------------
struct IXAudio2Voice {
    virtual HRESULT SetVolume(float volume, UINT32 operationSet = XAUDIO2_COMMIT_NOW) = 0;
    virtual HRESULT GetVolume(float* volume, UINT32 operationSet = XAUDIO2_COMMIT_NOW) = 0;
    virtual HRESULT SetOutputMatrix(IXAudio2Voice* destinationVoice, UINT32 sourceChannels,
                                    UINT32 destinationChannels, const float* levelMatrix,
                                    UINT32 operationSet = XAUDIO2_COMMIT_NOW) = 0;
    virtual HRESULT GetOutputMatrix(IXAudio2Voice* destinationVoice, UINT32 sourceChannels,
                                    UINT32 destinationChannels, float* levelMatrix,
                                    UINT32 operationSet = XAUDIO2_COMMIT_NOW) = 0;
    virtual void DestroyVoice() = 0;
};

struct IXAudio2SourceVoice : IXAudio2Voice {
    virtual HRESULT Start(UINT32 flags = 0, UINT32 operationSet = XAUDIO2_COMMIT_NOW) = 0;
    virtual HRESULT Stop(UINT32 flags = 0, UINT32 operationSet = XAUDIO2_COMMIT_NOW) = 0;
    virtual HRESULT SubmitSourceBuffer(const XAUDIO2_BUFFER* buffer,
                                       const XAUDIO2_BUFFER_WMA* bufferWma = nullptr) = 0;
    virtual HRESULT FlushSourceBuffers() = 0;
    virtual void GetState(XAUDIO2_VOICE_STATE* voiceState, UINT32 flags = 0) = 0;
};

struct IXAudio2MasteringVoice : IXAudio2Voice {
    virtual HRESULT GetEffectState(UINT32 effectIndex, BOOL* enabled) = 0;
};

struct IXAudio2 {
    virtual HRESULT QueryInterface(const GUID& iid, void** object) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
    virtual HRESULT CreateSourceVoice(
        IXAudio2SourceVoice** sourceVoice, const WAVEFORMATEX* sourceFormat, UINT32 flags = 0,
        float maxFrequencyRatio = XAUDIO2_DEFAULT_FREQ_RATIO, void* callback = nullptr,
        const void* sendList = nullptr, const XAUDIO2_EFFECT_CHAIN* effectChain = nullptr) = 0;
    virtual HRESULT CreateMasteringVoice(
        IXAudio2MasteringVoice** masteringVoice, UINT32 inputChannels = XAUDIO2_DEFAULT_CHANNELS,
        UINT32 inputSampleRate = XAUDIO2_DEFAULT_SAMPLERATE, UINT32 flags = 0,
        LPCWSTR deviceId = nullptr, const XAUDIO2_EFFECT_CHAIN* effectChain = nullptr,
        AUDIO_STREAM_CATEGORY streamCategory = AudioCategory_GameEffects) = 0;
};

extern "C" {

// 未实现（刻意）：见文件头。链接时会报 undefined symbol，那是预期结果。
HRESULT XAudio2Create(IXAudio2** xaudio2, UINT32 flags = 0,
                      UINT32 processor = XAUDIO2_DEFAULT_PROCESSOR);

} // extern "C"

#endif // CONCORD_COMPAT_XAUDIO2_H
