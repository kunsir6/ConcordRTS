// ============================================================================
// compat/win32/src/RuntimeWin32CompatAudio.cpp
//
// P4-02：XAudio2 的**真引擎**——AudioQueue 上的软件混音器。
//
// 上一轮（P4-01b）这里返回 E_NOTIMPL，并写清了理由：空壳引擎会让
// RuntimeVoiceAudio::ready()==true 而每条语音"播放成功但不出声"。这一轮把那个
// 空洞填上，判据仍然是**效果**而不是返回码：
//
//   · 多条 source voice 并行喂 PCM，混到主声部（不是"最后一条顶掉前面所有"）。
//   · SetVolume / SetOutputMatrix 真的作用在样本上（音量 0 必须真的是 0）。
//   · LoopCount 真的回绕（XAUDIO2_LOOP_INFINITE=255 与有限次数是两条路径）。
//   · 采样率不同真的重采样（本项目语音 22050/44100，主声部固定 48000）。
//   · 主声部上的效果链真的被调用（项目的 peak limiter 是 XAPO，只能靠
//     LockForProcess/Process 驱动；不调用它，多路叠加会硬削波）。
//
// 三个"看起来对、其实错"的做法，本文件刻意避开：
//
// 1) 每条 voice 各开一个 AudioQueue。那样两条语音就是"两条独立音轨"而不是"混音"，
//    音量矩阵跨声道相加这种事永远测不出来，而且两个队列的时钟会漂。这里只有
//    **一个**队列：所有 voice 在 48kHz 立体声浮点缓冲里相加，再一次性转换给设备。
//
// 2) 持锁调用 AudioQueueStop()。AudioQueueStop(synchronous) 会等回调返回，而回调
//    正在等混音锁 → 死锁。停设备一律先置 stopping_ 原子位、**不持锁**停队列，
//    回调看到旗就立刻返回。
//
// 3) 把 SamplesPlayed 当"输出帧数"。它是**源采样帧数**：22050 的素材播 1 秒，
//    输出 48000 帧、SamplesPlayed 只涨 22050。混成一谈，"播了多久"的对账会差
//    2 倍以上。
//
// 与真实 XAudio2 的差异（刻意的，全部记录在案）：
//   · 重采样是**线性插值**，不是 XAudio2 的多相滤波器。差异只在音质，不在时长/
//     音量/声像；但这不是保真实现，接真实设备听感时要知道。
//   · 循环只支持"整段循环"（LoopBegin/LoopLength 非零时仍按 playBegin..playEnd
//     处理）。产品只用 XAUDIO2_LOOP_INFINITE 整段循环，这是可达面的全部。
//   · 效果器只认 CXAPOBase 派生对象（项目里唯一的真效果器就是 peak limiter）。
//     别的 IUnknown 效果器会被当成"无操作"但仍报 enabled——本层唯一的撒谎点，
//     已限制在项目不会走到的路径上。
//   · QueryInterface 只答 E_NOINTERFACE：本层没有 IID 表，产品从不查询。
//   · SetOutputMatrix 只接受"源声道 → 2 声道主声部"这一种形状，其它形状返回
//     E_INVALIDARG，而不是悄悄接受后丢掉声道。
// ============================================================================

#include <xaudio2.h>
#include <xapobase.h>

#include "RuntimeWin32CompatAudioInternal.h"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <vector>

namespace {

using concord::compat::xaudio2::kMixChannels;
using concord::compat::xaudio2::kMixSampleRate;

// 设备缓冲 512 帧 = 10.7ms，4 个 = 42.7ms 的总延迟。太小会欠载（回调里还要拿锁），
// 太大会让"按下菜单到出声"的延迟变明显。
constexpr unsigned kDeviceFrames = 512;
constexpr unsigned kDeviceBufferCount = 4;

// XAudio2 的音量上界（真实头文件 XAUDIO2_MAX_VOLUME_LEVEL = 8.0f）。
constexpr float kMaxVolume = 8.0f;
// 单声道源默认铺到左右两声道的系数（XAudio2 与 X3DAUDIO 都用 1/√2）。
constexpr float kMonoPan = 0.70710678f;

double clampVolume(float volume) {
    if (!(volume > 0.0f)) return 0.0; // 顺带挡住 NaN
    return std::min<double>(volume, kMaxVolume);
}

// 浮点 → 16 位。**先乘 32768 再取整**：用 32767 会让满刻度素材掉 1 个 LSB，
// 而"输入 32767 输出 32766"这种偏差在没有像素/样本金标准的项目里极贵。
std::int16_t toInt16(double value) {
    if (value <= -1.0) return -32768;
    if (value >= 1.0) return 32767;
    long rounded = std::lround(value * 32768.0);
    if (rounded > 32767) rounded = 32767;
    if (rounded < -32768) rounded = -32768;
    return static_cast<std::int16_t>(rounded);
}

// 源格式一帧 → 归一化浮点。不支持的格式在 CreateSourceVoice 就被拒了，
// 所以这里没有"猜一个默认分支"的余地。
void decodeFrame(const std::uint8_t* frame, const WAVEFORMATEX& format, double& left, double& right) {
    const unsigned channels = format.nChannels;
    const unsigned bits = format.wBitsPerSample;
    double value[2] = {0.0, 0.0};
    for (unsigned channel = 0; channel < channels && channel < 2; ++channel) {
        if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            float sample = 0.0f;
            std::memcpy(&sample, frame + channel * 4, sizeof(float));
            if (!std::isfinite(sample)) sample = 0.0f; // NaN/Inf 会污染整块混音
            value[channel] = sample;
            continue;
        }
        const std::uint8_t* p = frame + channel * (bits / 8u);
        switch (bits) {
        case 8:
            // 8 位 WAV 是**无符号**（0x80 是零点）。写成有符号会让整条语音变成噪声。
            value[channel] = (static_cast<int>(p[0]) - 128) / 128.0;
            break;
        case 16: {
            const std::int16_t sample = static_cast<std::int16_t>(p[0] | (p[1] << 8));
            value[channel] = sample / 32768.0;
            break;
        }
        case 24: {
            const std::int32_t raw = (static_cast<std::int32_t>(p[0]) << 8) | (static_cast<std::int32_t>(p[1]) << 16) |
                                     (static_cast<std::int32_t>(p[2]) << 24);
            value[channel] = (raw >> 8) / 8388608.0;
            break;
        }
        default: {
            std::int32_t raw = 0;
            std::memcpy(&raw, p, sizeof(raw));
            value[channel] = raw / 2147483648.0;
            break;
        }
        }
    }
    left = value[0];
    right = channels >= 2 ? value[1] : value[0];
}

class Engine;
class SourceVoiceImpl;
class MasteringVoiceImpl;

// ---------------------------------------------------------------------------
// 一条排队的源缓冲。样本在 submit 时就转成归一化浮点**抄一份**：真实 XAudio2
// 引用调用方内存（产品也确实保活到播完），但"引用"意味着任何提前释放都变成读到
// 垃圾而没有任何返回值能报错。抄一份的代价是同样的字节数，换来解码只做一次。
// ---------------------------------------------------------------------------
struct VoiceBuffer {
    std::vector<double> samples; // 交错，源声道数，源采样率
    std::uint64_t playBegin = 0;
    std::uint64_t playEnd = 0;
    std::uint32_t loopsLeft = 0;
    void* context = nullptr;
};

// ---------------------------------------------------------------------------
// 引擎
// ---------------------------------------------------------------------------
class Engine final : public IXAudio2 {
public:
    explicit Engine(bool offline) : offline_(offline) {}
    ~Engine();

    // --- IXAudio2 ----------------------------------------------------------
    HRESULT QueryInterface(const GUID&, void** object) override {
        // 本层没有 IID 表，也没有任何调用点查询。给一个"成功"的接口出去会让
        // 调用方按真实 COM 布局解读我们的对象。
        if (object != nullptr) *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++references_; }
    ULONG Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT CreateSourceVoice(IXAudio2SourceVoice** sourceVoice, const WAVEFORMATEX* format, UINT32, float,
                              void*, const void*, const XAUDIO2_EFFECT_CHAIN*) override;
    HRESULT CreateMasteringVoice(IXAudio2MasteringVoice** masteringVoice, UINT32 inputChannels,
                                 UINT32 inputSampleRate, UINT32, LPCWSTR, const XAUDIO2_EFFECT_CHAIN* effectChain,
                                 AUDIO_STREAM_CATEGORY) override;

    // --- 内部 --------------------------------------------------------------
    std::mutex& mutex() { return mutex_; }
    // 定义放在 MasteringVoiceImpl 之后：这里要对 master_ 做基类转换，
    // 而此刻 MasteringVoiceImpl 还只是个前置声明。
    bool isMasteringVoice(const IXAudio2Voice* voice) const;
    void destroySourceVoice(SourceVoiceImpl* voice);
    void destroyMasteringVoice(MasteringVoiceImpl* voice);

    // 渲染一块：所有声部相加 → 效果链 → 主音量 → 16 位。调用方必须已持锁。
    void renderLocked(std::int16_t* out, unsigned frames);
    void renderOffline(std::int16_t* out, unsigned frames) {
        std::lock_guard<std::mutex> guard(mutex_);
        renderLocked(out, frames);
    }

    static void deviceCallback(void* user, AudioQueueRef, AudioQueueBufferRef buffer);
    void onDeviceBuffer(AudioQueueBufferRef buffer);
    HRESULT startDevice();
    void stopDevice();

private:
    void fillDeviceBuffer(AudioQueueBufferRef buffer);

    std::mutex mutex_;
    std::atomic<ULONG> references_{1};
    bool offline_ = false;
    MasteringVoiceImpl* master_ = nullptr;
    std::vector<SourceVoiceImpl*> sources_;
    std::vector<double> mixScratch_;
    AudioQueueRef queue_ = nullptr;
    std::atomic<bool> stopping_{false};
    bool deviceRunning_ = false;
};

// ---------------------------------------------------------------------------
// 源声部
// ---------------------------------------------------------------------------
class SourceVoiceImpl final : public IXAudio2SourceVoice {
    friend class Engine; // 引擎析构时要把 engine_ 摘掉
public:
    SourceVoiceImpl(Engine* engine, const WAVEFORMATEX& format) : engine_(engine), format_(format) {
        matrix_.assign(static_cast<std::size_t>(format_.nChannels) * kMixChannels, 0.0);
        if (format_.nChannels == 1) {
            matrix_[0] = kMonoPan;
            matrix_[1] = kMonoPan;
        } else {
            matrix_[0] = 1.0;
            matrix_[3] = 1.0;
        }
        rateRatio_ = static_cast<double>(format_.nSamplesPerSec) / kMixSampleRate;
    }

    // --- IXAudio2Voice -----------------------------------------------------
    HRESULT SetVolume(float volume, UINT32) override {
        // 混音线程（设备回调）会读它，所以走引擎锁。产品确实在播放中改音量。
        std::lock_guard<std::mutex> guard(engine_->mutex());
        volume_ = clampVolume(volume);
        return S_OK;
    }
    HRESULT GetVolume(float* volume, UINT32) override {
        if (volume == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> guard(engine_->mutex());
        *volume = static_cast<float>(volume_);
        return S_OK;
    }
    HRESULT SetOutputMatrix(IXAudio2Voice* destination, UINT32 sourceChannels, UINT32 destinationChannels,
                            const float* levelMatrix, UINT32) override {
        if (destination == nullptr || levelMatrix == nullptr) return E_POINTER;
        if (!engine_->isMasteringVoice(destination)) return E_INVALIDARG;
        if (sourceChannels != format_.nChannels || destinationChannels != kMixChannels) {
            // 形状必须与创建时声明的格式一致。放宽成"取前 N 个"会让多声道素材在
            // 立体声主声部上悄悄丢声道，而调用方看不出任何异常。
            return E_INVALIDARG;
        }
        const std::size_t count = static_cast<std::size_t>(sourceChannels) * destinationChannels;
        std::lock_guard<std::mutex> guard(engine_->mutex());
        matrix_.assign(levelMatrix, levelMatrix + count);
        return S_OK;
    }
    HRESULT GetOutputMatrix(IXAudio2Voice* destination, UINT32 sourceChannels, UINT32 destinationChannels,
                            float* levelMatrix, UINT32) override {
        if (destination == nullptr || levelMatrix == nullptr) return E_POINTER;
        if (!engine_->isMasteringVoice(destination)) return E_INVALIDARG;
        if (sourceChannels != format_.nChannels || destinationChannels != kMixChannels) return E_INVALIDARG;
        std::lock_guard<std::mutex> guard(engine_->mutex());
        std::copy(matrix_.begin(), matrix_.end(), levelMatrix);
        return S_OK;
    }
    void DestroyVoice() override;

    // --- IXAudio2SourceVoice ----------------------------------------------
    HRESULT Start(UINT32, UINT32) override {
        std::lock_guard<std::mutex> guard(engine_->mutex());
        started_ = true;
        // 真实语义：SamplesPlayed 自这次 Start 起算。产品每次 playFromStart 都先
        // Stop+Flush 再 Start，所以"播了多久"读到的永远是这一遍。
        playedPosition_ = 0.0;
        return S_OK;
    }
    HRESULT Stop(UINT32, UINT32) override {
        // Stop 只是"停止处理"，已排队缓冲**仍留在队列里**（真实 XAudio2 如此；
        // 产品正是先 Stop 再 FlushSourceBuffers 来清空）。
        std::lock_guard<std::mutex> guard(engine_->mutex());
        started_ = false;
        return S_OK;
    }
    HRESULT SubmitSourceBuffer(const XAUDIO2_BUFFER* buffer, const XAUDIO2_BUFFER_WMA*) override;
    HRESULT FlushSourceBuffers() override {
        std::lock_guard<std::mutex> guard(engine_->mutex());
        buffers_.clear();
        cursor_ = 0.0;
        return S_OK;
    }
    void GetState(XAUDIO2_VOICE_STATE* state, UINT32 flags) override {
        if (state == nullptr) return;
        std::lock_guard<std::mutex> guard(engine_->mutex());
        state->pCurrentBufferContext = buffers_.empty() ? nullptr : buffers_.front().context;
        state->BuffersQueued = static_cast<UINT32>(buffers_.size());
        // XAUDIO2_VOICE_NOSAMPLESPLAYED 明确表示"别算样本数"。这里不更新而不是
        // 填 0：填 0 会让调用方以为"一个都没播"。
        if ((flags & XAUDIO2_VOICE_NOSAMPLESPLAYED) == 0) {
            state->SamplesPlayed = static_cast<UINT64>(std::floor(playedPosition_));
        }
    }

    // --- 混音内部（调用方必须持引擎锁） ------------------------------------
    // 把本声部在 frames 个输出帧上的贡献**累加**进交错浮点缓冲。多路并行就靠
    // 这个 "+=" 成立：任何一处写成 "=" 都会变成"最后一条 voice 顶掉别人"。
    void mixInto(double* interleaved, unsigned frames) {
        for (unsigned frame = 0; frame < frames; ++frame) {
            double sourceL = 0.0;
            double sourceR = 0.0;
            if (!pullFrame(sourceL, sourceR)) return; // 本声部没数据：后面全部沉默
            double* out = interleaved + static_cast<std::size_t>(frame) * kMixChannels;
            if (format_.nChannels == 1) {
                out[0] += sourceL * volume_ * matrix_[0];
                out[1] += sourceL * volume_ * matrix_[1];
            } else {
                out[0] += sourceL * volume_ * matrix_[0] + sourceR * volume_ * matrix_[2];
                out[1] += sourceL * volume_ * matrix_[1] + sourceR * volume_ * matrix_[3];
            }
        }
    }

private:
    // 取一帧源样本（含线性插值），并把游标前进"一个输出帧对应的源帧数"。
    bool pullFrame(double& left, double& right) {
        for (;;) {
            if (!started_ || buffers_.empty()) return false;
            VoiceBuffer& buffer = buffers_.front();
            if (cursor_ >= static_cast<double>(buffer.playEnd)) {
                if (buffer.loopsLeft > 0) {
                    // 255 = 无限。有限次数每绕一圈减一，减到 0 后走"播完"分支。
                    if (buffer.loopsLeft != XAUDIO2_LOOP_INFINITE) --buffer.loopsLeft;
                    const double overshoot = cursor_ - static_cast<double>(buffer.playEnd);
                    cursor_ = static_cast<double>(buffer.playBegin) + overshoot;
                    if (cursor_ >= static_cast<double>(buffer.playEnd)) cursor_ = static_cast<double>(buffer.playBegin);
                    continue;
                }
                buffers_.pop_front();
                if (buffers_.empty()) {
                    // 缓冲播完且没有后续：声部自动停止（真实 XAudio2 如此，需再次
                    // Start 才继续）。产品靠这个判断"这一遍放完了"。
                    started_ = false;
                    return false;
                }
                cursor_ = static_cast<double>(buffers_.front().playBegin);
                continue;
            }
            const VoiceBuffer& current = buffers_.front();
            const std::uint64_t index = static_cast<std::uint64_t>(std::floor(cursor_));
            const double fraction = cursor_ - static_cast<double>(index);
            const std::uint64_t next = std::min<std::uint64_t>(index + 1, current.playEnd - 1);
            double leftA = 0.0, rightA = 0.0, leftB = 0.0, rightB = 0.0;
            if (!sampleAt(current, index, leftA, rightA)) return false;
            if (!sampleAt(current, next, leftB, rightB)) return false;
            left = leftA + (leftB - leftA) * fraction;
            right = rightA + (rightB - rightA) * fraction;
            cursor_ += rateRatio_;
            playedPosition_ += rateRatio_;
            return true;
        }
    }

    bool sampleAt(const VoiceBuffer& buffer, std::uint64_t frame, double& left, double& right) const {
        const unsigned channels = format_.nChannels;
        const std::size_t offset = static_cast<std::size_t>(frame) * channels;
        if (offset + channels > buffer.samples.size()) return false;
        left = buffer.samples[offset];
        right = channels >= 2 ? buffer.samples[offset + 1] : left;
        return true;
    }

    Engine* engine_ = nullptr;
    WAVEFORMATEX format_{};
    std::deque<VoiceBuffer> buffers_;
    double cursor_ = 0.0;         // 当前缓冲内的源帧位置（可含小数：重采样）
    double playedPosition_ = 0.0; // 自 Start 起消费的源帧数
    double volume_ = 1.0;
    std::vector<double> matrix_;
    double rateRatio_ = 1.0;
    bool started_ = false;
};

// ---------------------------------------------------------------------------
// 主声部：混音汇合点 + 效果链 + 输出设备
// ---------------------------------------------------------------------------
class MasteringVoiceImpl final : public IXAudio2MasteringVoice {
public:
    explicit MasteringVoiceImpl(Engine* engine) : engine_(engine) {}
    ~MasteringVoiceImpl() { releaseEffects(); }

    // --- IXAudio2Voice -----------------------------------------------------
    HRESULT SetVolume(float volume, UINT32) override {
        std::lock_guard<std::mutex> guard(engine_->mutex());
        volume_ = clampVolume(volume);
        return S_OK;
    }
    HRESULT GetVolume(float* volume, UINT32) override {
        if (volume == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> guard(engine_->mutex());
        *volume = static_cast<float>(volume_);
        return S_OK;
    }
    HRESULT SetOutputMatrix(IXAudio2Voice*, UINT32, UINT32, const float*, UINT32) override {
        // 主声部没有下游声部可混。返回成功等于承认"设了但没用"。
        return E_INVALIDARG;
    }
    HRESULT GetOutputMatrix(IXAudio2Voice*, UINT32, UINT32, float*, UINT32) override { return E_INVALIDARG; }
    void DestroyVoice() override;

    // --- IXAudio2MasteringVoice -------------------------------------------
    HRESULT GetEffectState(UINT32 effectIndex, BOOL* enabled) override {
        if (enabled == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> guard(engine_->mutex());
        if (effectIndex >= effects_.size()) return E_INVALIDARG;
        *enabled = effectEnabled_[effectIndex] ? TRUE : FALSE;
        return S_OK;
    }

    // --- 内部 --------------------------------------------------------------
    HRESULT addEffects(const XAUDIO2_EFFECT_CHAIN* chain) {
        if (chain == nullptr || chain->EffectCount == 0) return S_OK;
        if (chain->pEffectDescriptor == nullptr) return E_POINTER;
        for (UINT32 index = 0; index < chain->EffectCount; ++index) {
            const XAUDIO2_EFFECT_DESCRIPTOR& descriptor = chain->pEffectDescriptor[index];
            if (descriptor.pEffect == nullptr) return E_POINTER;
            descriptor.pEffect->AddRef();
            effects_.push_back(descriptor.pEffect);
            effectEnabled_.push_back(descriptor.InitialState != FALSE);
        }
        return S_OK;
    }

    // 效果链锁进混音格式。CXAPOBase 的 LockForProcess 对派生类做**真实校验**
    // （峰值限幅器要求 32 位浮点立体声且帧率一致），格式给错会返回
    // XAPO_E_FORMAT_UNSUPPORTED——那正是我们想让它报出来的。
    HRESULT lockEffects() {
        WAVEFORMATEX mix{};
        mix.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        mix.nChannels = static_cast<WORD>(kMixChannels);
        mix.nSamplesPerSec = kMixSampleRate;
        mix.wBitsPerSample = 32;
        mix.nBlockAlign = static_cast<WORD>(kMixChannels * 4);
        mix.nAvgBytesPerSec = kMixSampleRate * kMixChannels * 4;
        mix.cbSize = 0;
        XAPO_LOCKFORPROCESS_PARAMETERS parameters{&mix, kDeviceFrames};
        for (IUnknown* effect : effects_) {
            auto* xapo = dynamic_cast<CXAPOBase*>(effect);
            if (xapo == nullptr) continue; // 非 CXAPOBase：本层无法驱动，见文件头
            const HRESULT result = xapo->LockForProcess(1, &parameters, 1, &parameters);
            if (FAILED(result)) return result;
        }
        return S_OK;
    }

    // 调用方必须持引擎锁（渲染路径）。
    void processEffects(double* interleaved, unsigned frames) {
        bool touched = false;
        for (std::size_t index = 0; index < effects_.size(); ++index) {
            if (!effectEnabled_[index]) continue;
            if (dynamic_cast<CXAPOBase*>(effects_[index]) == nullptr) continue;
            if (!touched) {
                // 效果器要 32 位浮点缓冲：只做一次 double→float 拷贝，之后各效果器
                // 就地串联（XAPO_FLAG_INPLACE_REQUIRED 就是这么用的）。
                const std::size_t count = static_cast<std::size_t>(frames) * kMixChannels;
                if (effectScratch_.size() < count) effectScratch_.resize(count);
                for (std::size_t i = 0; i < count; ++i) effectScratch_[i] = static_cast<float>(interleaved[i]);
                touched = true;
            }
            auto* xapo = dynamic_cast<CXAPOBase*>(effects_[index]);
            XAPO_PROCESS_BUFFER_PARAMETERS in{effectScratch_.data(), XAPO_BUFFER_VALID, frames, FALSE};
            XAPO_PROCESS_BUFFER_PARAMETERS out = in;
            xapo->Process(1, &in, 1, &out, TRUE);
        }
        if (!touched) return;
        const std::size_t count = static_cast<std::size_t>(frames) * kMixChannels;
        for (std::size_t i = 0; i < count; ++i) interleaved[i] = effectScratch_[i];
    }

    void releaseEffects() {
        for (IUnknown* effect : effects_) {
            auto* xapo = dynamic_cast<CXAPOBase*>(effect);
            if (xapo != nullptr) xapo->UnlockForProcess();
            effect->Release();
        }
        effects_.clear();
        effectEnabled_.clear();
    }

    double volume() const { return volume_; }

private:
    Engine* engine_ = nullptr;
    std::vector<IUnknown*> effects_;
    std::vector<bool> effectEnabled_;
    std::vector<float> effectScratch_;
    double volume_ = 1.0;
};


bool Engine::isMasteringVoice(const IXAudio2Voice* voice) const {
    return voice != nullptr && static_cast<const IXAudio2Voice*>(master_) == voice;
}

void SourceVoiceImpl::DestroyVoice() {
    Engine* engine = engine_;
    engine_ = nullptr;
    if (engine != nullptr) engine->destroySourceVoice(this); // 内部 delete this
}

void MasteringVoiceImpl::DestroyVoice() {
    Engine* engine = engine_;
    engine_ = nullptr;
    if (engine != nullptr) engine->destroyMasteringVoice(this);
}

Engine::~Engine() {
    stopDevice();
    std::lock_guard<std::mutex> guard(mutex_);
    for (SourceVoiceImpl* voice : sources_) {
        voice->engine_ = nullptr; // 引擎先死：避免析构期回调用到半死的 this
        delete voice;
    }
    sources_.clear();
    delete master_;
    master_ = nullptr;
}

void Engine::destroySourceVoice(SourceVoiceImpl* voice) {
    // 只摘一个声部，**不能**顺手停设备：产品关一条语音时别的语音还在播。
    // 回调渲染时持同一把锁，所以摘除与删除之间不会有人正在用它。
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = std::find(sources_.begin(), sources_.end(), voice);
    if (found != sources_.end()) sources_.erase(found);
    delete voice;
}

void Engine::destroyMasteringVoice(MasteringVoiceImpl* voice) {
    stopDevice(); // 先停设备（不持锁），再摘主声部
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (master_ == voice) master_ = nullptr;
    }
    delete voice;
}

void Engine::deviceCallback(void* user, AudioQueueRef, AudioQueueBufferRef buffer) {
    static_cast<Engine*>(user)->onDeviceBuffer(buffer);
}

void Engine::onDeviceBuffer(AudioQueueBufferRef buffer) {
    // 停止中：一个字节都不写、也不入队，让 AudioQueueStop 立刻返回。
    if (stopping_.load() || queue_ == nullptr) return;
    fillDeviceBuffer(buffer);
}

void Engine::fillDeviceBuffer(AudioQueueBufferRef buffer) {
    auto* samples = static_cast<std::int16_t*>(buffer->mAudioData);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        renderLocked(samples, kDeviceFrames);
    }
    buffer->mAudioDataByteSize = kDeviceFrames * kMixChannels * 2;
    AudioQueueEnqueueBuffer(queue_, buffer, 0, nullptr);
}

HRESULT Engine::startDevice() {
    if (offline_) return S_OK; // 自测接缝：没有设备也能混音
    stopping_ = false;
    AudioStreamBasicDescription format{};
    format.mSampleRate = kMixSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = kMixChannels;
    format.mBitsPerChannel = 16;
    format.mBytesPerFrame = kMixChannels * 2;
    format.mBytesPerPacket = kMixChannels * 2;
    const OSStatus created =
        AudioQueueNewOutput(&format, &Engine::deviceCallback, this, nullptr, nullptr, 0, &queue_);
    if (created != noErr || queue_ == nullptr) {
        queue_ = nullptr;
        // 真实 XAudio2 无设备时 CreateMasteringVoice 失败。返回 E_FAIL 而不是建一个
        // 静音主声部——否则"设备不存在"会伪装成"素材没声音"。
        return E_FAIL;
    }
    const unsigned bytes = kDeviceFrames * kMixChannels * 2;
    for (unsigned index = 0; index < kDeviceBufferCount; ++index) {
        AudioQueueBufferRef buffer = nullptr;
        if (AudioQueueAllocateBuffer(queue_, bytes, &buffer) != noErr || buffer == nullptr) {
            stopDevice();
            return E_OUTOFMEMORY;
        }
        buffer->mAudioDataByteSize = bytes;
        fillDeviceBuffer(buffer);
    }
    if (AudioQueueStart(queue_, nullptr) != noErr) {
        stopDevice();
        return E_FAIL;
    }
    deviceRunning_ = true;
    return S_OK;
}

void Engine::stopDevice() {
    // 先立旗再停队列：回调看到旗立刻返回，AudioQueueStop 才不会去等一个
    // 正在等混音锁的回调（那才是死锁）。这里**不持** mutex_。
    stopping_ = true;
    if (queue_ != nullptr) {
        AudioQueueStop(queue_, true);
        AudioQueueDispose(queue_, true);
        queue_ = nullptr;
    }
    deviceRunning_ = false;
}

HRESULT Engine::CreateSourceVoice(IXAudio2SourceVoice** sourceVoice, const WAVEFORMATEX* format, UINT32, float,
                                 void*, const void*, const XAUDIO2_EFFECT_CHAIN*) {
    if (sourceVoice == nullptr || format == nullptr) return E_POINTER;
    *sourceVoice = nullptr;
    const bool pcm = format->wFormatTag == WAVE_FORMAT_PCM &&
                     (format->wBitsPerSample == 8 || format->wBitsPerSample == 16 ||
                      format->wBitsPerSample == 24 || format->wBitsPerSample == 32);
    const bool ieeeFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32;
    if (!pcm && !ieeeFloat) return E_INVALIDARG;
    if (format->nChannels < 1 || format->nChannels > 2) return E_INVALIDARG;
    if (format->nSamplesPerSec < XAUDIO2_MIN_SAMPLE_RATE || format->nSamplesPerSec > XAUDIO2_MAX_SAMPLE_RATE) {
        return E_INVALIDARG;
    }
    if (format->nBlockAlign != format->nChannels * (format->wBitsPerSample / 8u)) return E_INVALIDARG;

    auto* voice = new (std::nothrow) SourceVoiceImpl(this, *format);
    if (voice == nullptr) return E_OUTOFMEMORY;
    std::lock_guard<std::mutex> guard(mutex_);
    sources_.push_back(voice);
    *sourceVoice = voice;
    return S_OK;
}

HRESULT Engine::CreateMasteringVoice(IXAudio2MasteringVoice** masteringVoice, UINT32 inputChannels,
                                     UINT32 inputSampleRate, UINT32, LPCWSTR, const XAUDIO2_EFFECT_CHAIN* effectChain,
                                     AUDIO_STREAM_CATEGORY) {
    if (masteringVoice == nullptr) return E_POINTER;
    *masteringVoice = nullptr;
    if (master_ != nullptr) return E_INVALIDARG; // 真实 XAudio2 也只允许一个主声部
    // 主声部固定 48kHz 立体声：产品语音素材是 22050/44100，设备这台机器上就是 48k；
    // 让主声部"跟随源格式"会导致每换一条语音就重开一次队列。
    if (inputChannels != 0 && inputChannels != kMixChannels) return E_INVALIDARG;
    if (inputSampleRate != 0 && inputSampleRate != kMixSampleRate) return E_INVALIDARG;

    auto* voice = new (std::nothrow) MasteringVoiceImpl(this);
    if (voice == nullptr) return E_OUTOFMEMORY;
    HRESULT result = voice->addEffects(effectChain);
    if (SUCCEEDED(result)) result = voice->lockEffects();
    if (SUCCEEDED(result)) {
        master_ = voice;
        result = startDevice();
    }
    if (FAILED(result)) {
        master_ = nullptr;
        delete voice;
        return result;
    }
    *masteringVoice = voice;
    return S_OK;
}

void Engine::renderLocked(std::int16_t* out, unsigned frames) {
    if (frames == 0) return;
    const std::size_t count = static_cast<std::size_t>(frames) * kMixChannels;
    if (mixScratch_.size() < count) mixScratch_.resize(count);
    std::fill_n(mixScratch_.begin(), count, 0.0);
    for (SourceVoiceImpl* voice : sources_) voice->mixInto(mixScratch_.data(), frames);
    if (master_ != nullptr) {
        master_->processEffects(mixScratch_.data(), frames);
        // 主音量在效果**之后**：verification mute 必须绝对静音，不能被限幅器的
        // 保持增益"救回来"一个非零样本。
        const double volume = master_->volume();
        if (volume != 1.0) {
            for (std::size_t index = 0; index < count; ++index) mixScratch_[index] *= volume;
        }
    }
    for (std::size_t index = 0; index < count; ++index) out[index] = toInt16(mixScratch_[index]);
}

HRESULT SourceVoiceImpl::SubmitSourceBuffer(const XAUDIO2_BUFFER* buffer, const XAUDIO2_BUFFER_WMA*) {
    if (buffer == nullptr || buffer->pAudioData == nullptr) return E_POINTER;
    const unsigned frameBytes = format_.nBlockAlign;
    if (frameBytes == 0) return E_INVALIDARG;
    if (buffer->AudioBytes == 0 || (buffer->AudioBytes % frameBytes) != 0) return E_INVALIDARG;

    const std::uint64_t totalFrames = buffer->AudioBytes / frameBytes;
    // Play/Loop 是**字节**偏移（真实 XAUDIO2_BUFFER 如此），换算成帧。
    const std::uint64_t playBegin = buffer->PlayBegin / frameBytes;
    const std::uint64_t playLength =
        buffer->PlayLength == 0 ? (totalFrames - std::min(playBegin, totalFrames)) : buffer->PlayLength / frameBytes;
    if (playBegin >= totalFrames) return E_INVALIDARG;
    if (playBegin + playLength > totalFrames) return E_INVALIDARG;

    VoiceBuffer stored;
    stored.playBegin = playBegin;
    stored.playEnd = playBegin + playLength;
    stored.loopsLeft = buffer->LoopCount;
    stored.context = buffer->pContext;
    stored.samples.resize(static_cast<std::size_t>(totalFrames) * format_.nChannels);
    const auto* bytes = buffer->pAudioData;
    for (std::uint64_t frame = 0; frame < totalFrames; ++frame) {
        double left = 0.0;
        double right = 0.0;
        decodeFrame(bytes + frame * frameBytes, format_, left, right);
        stored.samples[static_cast<std::size_t>(frame) * format_.nChannels] = left;
        if (format_.nChannels >= 2) {
            stored.samples[static_cast<std::size_t>(frame) * format_.nChannels + 1] = right;
        }
    }

    std::lock_guard<std::mutex> guard(engine_->mutex());
    const bool first = buffers_.empty();
    buffers_.push_back(std::move(stored));
    // 队列里的第一条从它自己的 playBegin 开始；后续缓冲由 pullFrame 的"换缓冲"
    // 分支接管（连续序列就是这样接起来的）。
    if (first) cursor_ = static_cast<double>(buffers_.front().playBegin);
    return S_OK;
}

} // namespace

namespace concord::compat::xaudio2 {

IXAudio2* createOfflineEngine() { return new (std::nothrow) Engine(true); }

unsigned renderInterleaved(IXAudio2* engine, std::int16_t* out, unsigned frames) {
    auto* real = dynamic_cast<Engine*>(engine);
    if (real == nullptr || out == nullptr) return 0;
    real->renderOffline(out, frames);
    return frames;
}

} // namespace concord::compat::xaudio2

// ---------------------------------------------------------------------------
// 产品入口。XAudio2Create 只有一个：建**真设备**的引擎。离线引擎只存在于
// RuntimeWin32CompatAudioInternal.h 那个自测接缝里，产品代码看不到它。
// ---------------------------------------------------------------------------
extern "C" HRESULT XAudio2Create(IXAudio2** xaudio2, UINT32 /*flags*/, UINT32 /*processor*/) {
    if (xaudio2 == nullptr) return E_POINTER;
    *xaudio2 = nullptr;
    auto* engine = new (std::nothrow) Engine(false);
    if (engine == nullptr) return E_OUTOFMEMORY;
    *xaudio2 = engine;
    return S_OK;
}
