// P4-01a: RIFF/WAVE 解码 + AudioQueue 输出流。
//
// 两个"看起来对、其实是错的"的实现方式被刻意避开：
//  1) 按 RIFF 头里的 size 读数据。很多游戏素材的 size 是错的（0xFFFFFFFF、
//     或写成整段文件长度），照它读会越界或截断。这里一律以**实际字节数**为准。
//  2) 按 fmt 里的 blockAlign 算帧长。blockAlign 写错时帧会错位。这里用
//     channels * bytesPerSample 现算，blockAlign 只作参考。
// 8 位 WAV 是**无符号**（0x80 == 静音），16 位以上是有符号小端——这条弄反了
// 整首歌都会是"噪声但能播"，没有任何返回值能报错。

#include "audio/apple/RuntimeAppleAudioOutput.h"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

namespace concord::audio::apple {
namespace {

constexpr unsigned kBufferFrames = 2048;
constexpr unsigned kBufferCount = 4;

std::uint32_t readU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t readU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8));
}
bool tagIs(const std::uint8_t* p, const char* tag) { return std::memcmp(p, tag, 4) == 0; }

// 设备探测用的空回调：探测队列从不 Start，所以它永远不会被调用。
void AppleAudioOutputProbeCallback(void*, AudioQueueRef, AudioQueueBufferRef) {}

// IMA ADPCM（MS tag 0x11）的步长与索引表。这两张表是格式定义的一部分，
// 不是可以"调"的参数：表错一格，解出来就是能播但音调不对的噪声。
constexpr int kImaStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kImaIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

std::int16_t clampSample(int value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return static_cast<std::int16_t>(value);
}

// 一个 4 位 nibble → 新预测值。返回值同时更新 index（自适应步长）。
std::int16_t imaStep(int nibble, int& predictor, int& index) {
    const int step = kImaStepTable[index];
    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    predictor += (nibble & 8) ? -diff : diff;
    predictor = clampSample(predictor);
    index += kImaIndexTable[nibble & 0xF];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    return static_cast<std::int16_t>(predictor);
}

} // namespace

bool decodeWav(const std::vector<std::uint8_t>& bytes, DecodedPcm& out, std::string& error) {
    out = DecodedPcm{};
    error.clear();
    // 最小可解文件：RIFF(12) + fmt(24) + data(8)。
    if (bytes.size() < 44) {
        error = "wav: file shorter than a minimal header";
        return false;
    }
    if (!tagIs(bytes.data(), "RIFF") || !tagIs(bytes.data() + 8, "WAVE")) {
        error = "wav: not a RIFF/WAVE file";
        return false;
    }

    unsigned channels = 0;
    unsigned sampleRate = 0;
    unsigned bitsPerSample = 0;
    unsigned formatTag = 0;
    bool haveFmt = false;
    std::size_t fmtOffset = 0;
    std::size_t fmtSize = 0;
    std::size_t dataOffset = 0;
    std::size_t dataBytes = 0;
    bool haveData = false;

    // 从 RIFF 头之后**逐块走**，不用 RIFF size：它经常是错的。
    std::size_t cursor = 12;
    while (cursor + 8 <= bytes.size()) {
        const std::uint8_t* header = bytes.data() + cursor;
        const std::size_t chunkSize = readU32(header + 4);
        const std::size_t payload = cursor + 8;
        const std::size_t available = bytes.size() - payload;
        if (tagIs(header, "fmt ")) {
            if (chunkSize < 16 || available < 16) {
                error = "wav: truncated fmt chunk";
                return false;
            }
            formatTag = readU16(bytes.data() + payload);
            channels = readU16(bytes.data() + payload + 2);
            sampleRate = readU32(bytes.data() + payload + 4);
            bitsPerSample = readU16(bytes.data() + payload + 14);
            // WAVE_FORMAT_EXTENSIBLE：真正的编码在 SubFormat GUID 的前两字节。
            if (formatTag == 0xFFFE) {
                if (chunkSize < 40 || available < 40) {
                    error = "wav: truncated WAVE_FORMAT_EXTENSIBLE fmt chunk";
                    return false;
                }
                formatTag = readU16(bytes.data() + payload + 24);
            }
            haveFmt = true;
            fmtOffset = payload;
            fmtSize = std::min<std::size_t>(chunkSize, available);
        } else if (tagIs(header, "data")) {
            if (!haveData) {
                dataOffset = payload;
                // size 大于实际文件时按实际截断——这是本项目素材的常态。
                dataBytes = static_cast<std::size_t>(std::min<std::uint64_t>(chunkSize, available));
                haveData = true;
            }
        }
        // 块按偶数字节对齐：奇数大小的块后面有一个填充字节。
        const std::size_t advance = static_cast<std::size_t>(chunkSize) + (chunkSize & 1u);
        if (advance > available + 1) break; // 块声明越界：停止走路，已拿到的 fmt/data 仍然可用
        cursor = payload + advance;
    }

    if (!haveFmt) {
        error = "wav: no fmt chunk";
        return false;
    }
    if (!haveData) {
        error = "wav: no data chunk";
        return false;
    }
    if (channels == 0 || channels > 8) {
        error = "wav: unsupported channel count " + std::to_string(channels);
        return false;
    }
    if (sampleRate < 4000 || sampleRate > 192000) {
        error = "wav: unsupported sample rate " + std::to_string(sampleRate);
        return false;
    }

    out.sampleRate = sampleRate;
    out.channels = channels;

    // IMA ADPCM：本项目**全部**散装素材都是这个格式（RA2/**/*.WAV 实测 152/152
    // 是 tag 17 / 22050Hz / 单声道 / 4bit）。只做 PCM 等于"接口通了但一句都听不到"，
    // 所以这一支不是可选项。
    if (formatTag == 0x11) {
        if (fmtSize < 20) {
            error = "wav: IMA ADPCM fmt chunk is too short for samplesPerBlock";
            return false;
        }
        const std::size_t blockAlign = readU16(bytes.data() + fmtOffset + 12);
        if (blockAlign < 4 * channels) {
            error = "wav: IMA ADPCM blockAlign " + std::to_string(blockAlign) + " is smaller than one header";
            return false;
        }
        unsigned samplesPerBlock = readU16(bytes.data() + fmtOffset + 18);
        if (samplesPerBlock == 0) {
            // fmt 没写时从 blockAlign 反推：blockAlign = ch*4 + (spb-1)*ch/2
            samplesPerBlock = static_cast<unsigned>((blockAlign - 4 * channels) * 2 / channels + 1);
        }
        std::vector<std::int16_t> samples;
        const std::uint8_t* cursorBytes = bytes.data() + dataOffset;
        std::size_t remaining = dataBytes;
        while (remaining >= 4 * channels) {
            const std::size_t thisBlock = std::min(blockAlign, remaining);
            std::vector<int> predictor(channels), index(channels);
            for (unsigned ch = 0; ch < channels; ++ch) {
                // 块头是每声道 4 字节：int16 预测值 + uint8 步长索引 + 1 保留
                predictor[ch] = static_cast<std::int16_t>(readU16(cursorBytes + ch * 4));
                index[ch] = std::min(88, static_cast<int>(cursorBytes[ch * 4 + 2]));
            }
            for (unsigned ch = 0; ch < channels; ++ch) {
                samples.push_back(static_cast<std::int16_t>(predictor[ch]));
            }
            // 一个块的第 0 个样本就是块头里的预测值，nibble 从第 1 帧开始。
            const std::size_t nibbleBytes = thisBlock - 4 * channels;
            const std::size_t nibbleFrames = nibbleBytes * 2 / channels;
            const std::size_t usableFrames =
                std::min<std::size_t>(nibbleFrames, samplesPerBlock > 0 ? samplesPerBlock - 1 : nibbleFrames);
            for (std::size_t frame = 0; frame < usableFrames; ++frame) {
                for (unsigned ch = 0; ch < channels; ++ch) {
                    // 关键布局（用 afconvert 逐样本比对才发现）：nibble 数据**不是**
                    // 按帧交错，而是按声道每 4 字节为一组交错——
                    // 先 ch0 的 4 字节（8 个 nibble），再 ch1 的 4 字节，如此循环。
                    // 单声道时两种写法结果相同，所以只听单声道素材永远发现不了这个错：
                    // 立体声会让右声道从第 1 帧起全体错位（本项目 10 个菜单音乐全中）。
                    const std::size_t ordinal = frame; // 该声道内的第几个 nibble（0 起）
                    const std::size_t group = ordinal / 8;
                    const std::size_t within = ordinal % 8;
                    const std::size_t byteOffset =
                        4 * channels + group * (4 * channels) + ch * 4 + within / 2;
                    const std::uint8_t packed = cursorBytes[byteOffset];
                    const int nibble = (within & 1) ? (packed >> 4) : (packed & 0xF);
                    samples.push_back(imaStep(nibble, predictor[ch], index[ch]));
                }
            }
            cursorBytes += thisBlock;
            remaining -= thisBlock;
            if (thisBlock < blockAlign) break; // 末块被截断：已解出的样本保留
        }
        if (samples.empty()) {
            error = "wav: IMA ADPCM data holds no complete block";
            return false;
        }
        out.samples = std::move(samples);
        return true;
    }

    if (formatTag != 1) {
        error = "wav: unsupported codec (format tag " + std::to_string(formatTag) + ")";
        return false;
    }
    if (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) {
        error = "wav: unsupported bit depth " + std::to_string(bitsPerSample);
        return false;
    }

    const std::size_t bytesPerSample = bitsPerSample / 8u;
    const std::size_t frameBytes = bytesPerSample * channels; // 现算，不用 blockAlign
    const std::uint64_t frames = dataBytes / frameBytes;      // 尾部不足一帧的字节丢弃
    if (frames == 0) {
        error = "wav: data chunk holds no complete frame";
        return false;
    }

    out.samples.resize(static_cast<std::size_t>(frames) * channels);
    const std::uint8_t* src = bytes.data() + dataOffset;
    for (std::size_t i = 0; i < out.samples.size(); ++i) {
        switch (bitsPerSample) {
        case 8:
            // 无符号：0x80 是零点。
            out.samples[i] = static_cast<std::int16_t>((static_cast<int>(src[i]) - 128) * 256);
            break;
        case 16:
            out.samples[i] = static_cast<std::int16_t>(readU16(src + i * 2));
            break;
        case 24: {
            const std::uint8_t* p = src + i * 3;
            const std::int32_t raw = static_cast<std::int32_t>(
                (static_cast<std::uint32_t>(p[0]) << 8) | (static_cast<std::uint32_t>(p[1]) << 16) |
                (static_cast<std::uint32_t>(p[2]) << 24));
            out.samples[i] = static_cast<std::int16_t>(raw >> 16);
            break;
        }
        default: {
            const std::uint8_t* p = src + i * 4;
            const std::int32_t raw = static_cast<std::int32_t>(readU32(p));
            out.samples[i] = static_cast<std::int16_t>(raw >> 16);
            break;
        }
        }
    }
    return true;
}

bool decodeWavFile(const std::string& path, DecodedPcm& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "wav: cannot open " + path;
        return false;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file.eof() && file.fail()) {
        error = "wav: read failed for " + path;
        return false;
    }
    return decodeWav(bytes, out, error);
}

namespace {

class AppleAudioOutput final : public AudioOutput {
public:
    ~AppleAudioOutput() override { dispose(); }

    bool play(const DecodedPcm& pcm, bool loop, int volumePercent, std::string& error) override {
        dispose();
        error.clear();
        if (pcm.sampleRate == 0 || pcm.channels == 0 || pcm.frames() == 0) {
            error = "audio: refusing to play an empty buffer";
            return false;
        }
        // 拷一份：调用方常常在 play() 返回后就丢掉 DecodedPcm（MCI 那套语义是
        // "命令返回即交接给设备"），共享引用会变成悬垂读。
        owned_ = pcm;
        cursor_ = 0;
        queued_ = 0;
        consumed_ = 0;
        noMore_ = false;
        finished_ = false;
        loop_ = loop;
        stopping_ = false;
        sampleRate_ = pcm.sampleRate;
        channels_ = pcm.channels;

        AudioStreamBasicDescription asbd{};
        asbd.mSampleRate = static_cast<Float64>(pcm.sampleRate);
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        asbd.mFramesPerPacket = 1;
        asbd.mChannelsPerFrame = pcm.channels;
        asbd.mBitsPerChannel = 16;
        asbd.mBytesPerFrame = pcm.channels * 2;
        asbd.mBytesPerPacket = pcm.channels * 2;

        OSStatus status = AudioQueueNewOutput(&asbd, &AppleAudioOutput::callback, this, nullptr, nullptr, 0,
                                              &queue_);
        if (status != noErr || queue_ == nullptr) {
            queue_ = nullptr;
            error = "audio: AudioQueueNewOutput failed (" + std::to_string(status) + ")";
            return false;
        }
        const float volume = static_cast<float>(std::clamp(volumePercent, 0, 100)) / 100.0f;
        (void)AudioQueueSetParameter(queue_, kAudioQueueParam_Volume, volume);

        const unsigned bufferBytes = kBufferFrames * pcm.channels * 2;
        for (unsigned i = 0; i < kBufferCount; ++i) {
            AudioQueueBufferRef buffer = nullptr;
            status = AudioQueueAllocateBuffer(queue_, bufferBytes, &buffer);
            if (status != noErr || buffer == nullptr) {
                error = "audio: AudioQueueAllocateBuffer failed (" + std::to_string(status) + ")";
                dispose();
                return false;
            }
            fill(buffer);
        }
        status = AudioQueueStart(queue_, nullptr);
        if (status != noErr) {
            error = "audio: AudioQueueStart failed (" + std::to_string(status) + ")";
            dispose();
            return false;
        }
        started_ = true;
        startProbe();
        return true;
    }

    std::uint64_t framesConsumed() const override { return consumed_.load(); }
    bool isPlaying() const override { return started_ && !finished_.load(); }
    unsigned sampleRate() const override { return sampleRate_; }
    unsigned channels() const override { return channels_; }

    void stop() override {
        started_ = false;
        finished_ = true;
        dispose();
    }

private:
    static void callback(void* user, AudioQueueRef, AudioQueueBufferRef buffer) {
        auto* self = static_cast<AppleAudioOutput*>(user);
        if (self->stopping_.load()) return;
        if (self->queued_ > 0) --self->queued_;
        const unsigned frameBytes = self->owned_.channels * 2;
        if (frameBytes != 0) self->consumed_.fetch_add(buffer->mAudioDataByteSize / frameBytes);
        self->fill(buffer);
        if (self->noMore_ && self->queued_ == 0) self->finished_ = true;
    }

    void fill(AudioQueueBufferRef buffer) {
        const unsigned frameBytes = owned_.channels * 2;
        const std::size_t capacity = frameBytes ? buffer->mAudioDataBytesCapacity / frameBytes : 0;
        const std::uint64_t total = owned_.frames();
        std::size_t written = 0;
        while (written < capacity) {
            if (cursor_ >= total) {
                if (loop_ && total != 0) {
                    cursor_ = 0;
                } else {
                    break;
                }
            }
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(capacity - written, total - cursor_));
            std::memcpy(static_cast<std::uint8_t*>(buffer->mAudioData) + written * frameBytes,
                        owned_.samples.data() + cursor_ * owned_.channels, take * frameBytes);
            written += take;
            cursor_ += take;
        }
        buffer->mAudioDataByteSize = static_cast<UInt32>(written * frameBytes);
        if (written == 0) {
            // 零字节缓冲不能被入队：AudioQueue 会把它当成一个立即完成的缓冲，
            // 于是"播完了"和"从来没数据"看起来一模一样。
            noMore_ = true;
            return;
        }
        if (AudioQueueEnqueueBuffer(queue_, buffer, 0, nullptr) == noErr) {
            ++queued_;
        } else {
            noMore_ = true;
        }
        if (cursor_ >= total && !loop_) noMore_ = true;
    }

    void dispose() {
        stopProbe();
        stopping_ = true;
        if (queue_ != nullptr) {
            AudioQueueStop(queue_, true);
            AudioQueueDispose(queue_, true);
            queue_ = nullptr;
        }
        started_ = false;
    }

    // ------------------------------------------------------------------
    // P4-01c：产品级"真的在按时钟消费"取证
    // ------------------------------------------------------------------
    // 只在 CONCORD_AUDIO_TRACE=1 时存在（否则不起线程、不读时钟）。
    // 为什么不用产品自己的定时器来采样：菜单循环阻塞在 GetMessageW 上，WM_TIMER
    // 不投递（本轮实测，见 RUNLOG），任何"每秒回调一次"的观测点都不会被调用。
    // 所以观测线程必须由输出流自己持有，而且只读 consumed_ 这个原子量，不改任何
    // 播放行为。判据：playedSec 与 wallMs 同步增长且比值≈1。
    void startProbe() {
        if (!traceEnabled()) return;
        stopProbe();
        {
            std::lock_guard<std::mutex> guard(probeMutex_);
            probeStop_ = false;
        }
        const unsigned rate = sampleRate_;
        probeStartMs_ = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        probe_ = std::thread([this, rate] {
            for (int second = 1;; ++second) {
                std::unique_lock<std::mutex> lock(probeMutex_);
                if (probeCv_.wait_for(
                        lock, std::chrono::milliseconds(1000),
                        [this] { return probeStop_; })) {
                    return;
                }
                lock.unlock();
                const std::uint64_t consumed = consumed_.load();
                const double playedSec = rate == 0 ? 0.0
                    : static_cast<double>(consumed) / static_cast<double>(rate);
                std::fprintf(
                    stderr,
                    "[audio] output probe t=%ds consumed=%llu playedSec=%.2f"
                    " wallMs=%llu rate=%u ch=%u playing=%d\n",
                    second,
                    static_cast<unsigned long long>(consumed),
                    playedSec,
                    static_cast<unsigned long long>(probeStartMs_ == 0 ? 0 : probeElapsedMs()),
                    rate,
                    channels_,
                    started_ && !finished_.load() ? 1 : 0
                );
            }
        });
    }

    void stopProbe() {
        if (!probe_.joinable()) return;
        {
            std::lock_guard<std::mutex> guard(probeMutex_);
            probeStop_ = true;
        }
        probeCv_.notify_all();
        probe_.join();
    }

    static bool traceEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("CONCORD_AUDIO_TRACE");
            return value != nullptr && *value != '\0' && std::string(value) != "0";
        }();
        return enabled;
    }

    std::uint64_t probeElapsedMs() const {
        const std::uint64_t now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        return now >= probeStartMs_ ? now - probeStartMs_ : 0;
    }

    AudioQueueRef queue_ = nullptr;
    DecodedPcm owned_{};
    std::uint64_t cursor_ = 0;
    unsigned queued_ = 0;
    std::atomic<std::uint64_t> consumed_{0};
    std::atomic<bool> finished_{false};
    std::atomic<bool> stopping_{false};
    bool noMore_ = false;
    bool loop_ = false;
    bool started_ = false;
    unsigned sampleRate_ = 0;
    unsigned channels_ = 0;

    std::thread probe_;
    std::mutex probeMutex_;
    std::condition_variable probeCv_;
    bool probeStop_ = false;
    std::uint64_t probeStartMs_ = 0;
};

} // namespace

std::unique_ptr<AudioOutput> AudioOutput::create(std::string& error) {
    error.clear();
    // 真探测一次设备，而不是"构造一个必然成功的对象再在 play() 里失败"。
    // 没有输出设备时（无声卡、CI 容器）必须在 create() 就报出来，否则上层
    // 会把"设备不存在"当成"素材坏了"去查。
    AudioStreamBasicDescription probe{};
    probe.mSampleRate = 44100.0;
    probe.mFormatID = kAudioFormatLinearPCM;
    probe.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    probe.mFramesPerPacket = 1;
    probe.mChannelsPerFrame = 2;
    probe.mBitsPerChannel = 16;
    probe.mBytesPerFrame = 4;
    probe.mBytesPerPacket = 4;
    AudioQueueRef queue = nullptr;
    const OSStatus status =
        AudioQueueNewOutput(&probe, &AppleAudioOutputProbeCallback, nullptr, nullptr, nullptr, 0, &queue);
    if (status != noErr || queue == nullptr) {
        error = "audio: no output device (AudioQueueNewOutput " + std::to_string(status) + ")";
        return nullptr;
    }
    AudioQueueDispose(queue, true);
    std::unique_ptr<AudioOutput> output(new AppleAudioOutput());
    return output;
}

} // namespace concord::audio::apple
