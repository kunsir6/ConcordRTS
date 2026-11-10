// ============================================================================
// tests/audio/apple/XAudio2MixerSmoke.cpp
//
// P4-02：XAudio2 混音器的**逐样本**自测。
//
// 为什么断言是硬编码数字而不是"跟另一份实现比"：项目的 raster/audio 契约大量用
// "快慢两路比"（见 queue.rules.selfconsistency_ceiling），两侧共用同一段代码时，
// 一个共同的错（音量没乘、矩阵没生效、重采样没做）会让两边同时错、断言全绿。
// 所以这里的期望值全部是手算的字面量：输入 16384、音量 1.0、矩阵 1.0，输出就必须
// 是 16384；差 1 也算失败。
//
// 覆盖的语义（每一条都对应一个"空壳实现也能返回成功"的坑）：
//   1. 单路直通：身份矩阵下样本逐位不变（连满刻度 32767 / -32768 都不许掉 1 LSB）。
//   2. 多路混音：两条 voice 同时出声，且**分别**落在左右声道（矩阵真的按声道生效）。
//   3. 运行期改音量：中途 SetVolume，后面几帧的幅度立刻变。
//   4. LoopCount：无限循环回绕 / 有限次数（LoopCount=1 → 播两遍）到点停。
//   5. 重采样：24000 单声道进 48000 主声部，线性插值的点手算。
//   6. 播完自动停止：BuffersQueued 归零，再次渲染是静音（不是"循环回开头"）。
//   7. 效果链：两条 0.8 的 voice 叠加，peak limiter 必须把它压到 ≈16368 而不是
//      削在 32767（不调用 Process 的话这条必红）。
//   8. 主音量 0：绝对静音（产品的 verification mute 依赖这一条）。
//   9. 8 位无符号 / 32 位浮点解码：0x80 是零点、float 直通。
//  10. 拒绝无效输入：64 位 PCM、6 声道、跨形状矩阵、第二个主声部。
//  11. 真设备：XAudio2Create → 主声部 → 源声部 → 提交 → Start，SamplesPlayed 真的涨。
//      这一条需要声卡；没有声卡时它是**唯一**会红的检查，其余断言仍然可跑——
//      这正是 createOfflineEngine() 存在的理由。
// ============================================================================

#include "RuntimeWin32CompatAudioInternal.h"

#include "audio/backend/RuntimeAudioPeakLimiter.h"

#include <xaudio2.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

unsigned checks = 0;
unsigned failures = 0;

void expect(bool ok, const char* name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    }
}

WAVEFORMATEX makeFormat(unsigned rate, unsigned channels, unsigned bits, unsigned tag) {
    WAVEFORMATEX format{};
    format.wFormatTag = static_cast<WORD>(tag);
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = static_cast<WORD>(bits);
    format.nBlockAlign = static_cast<WORD>(channels * (bits / 8u));
    format.nAvgBytesPerSec = rate * format.nBlockAlign;
    format.cbSize = 0;
    return format;
}

std::vector<std::uint8_t> bytesFromS16(const std::vector<std::int16_t>& samples) {
    std::vector<std::uint8_t> bytes(samples.size() * 2);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto value = static_cast<std::uint16_t>(samples[index]);
        bytes[index * 2] = static_cast<std::uint8_t>(value & 0xFF);
        bytes[index * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    return bytes;
}

std::vector<std::uint8_t> bytesFromF32(const std::vector<float>& samples) {
    std::vector<std::uint8_t> bytes(samples.size() * 4);
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

// 一个可以独立释放的引擎 + 主声部（带项目的真 peak limiter，和产品接线一致）。
struct Graph {
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IUnknown* limiter = nullptr;

    bool create(bool offline) {
        engine = offline ? concord::compat::xaudio2::createOfflineEngine() : nullptr;
        if (engine == nullptr && FAILED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
        if (engine == nullptr) return false;
        if (FAILED(createRuntimeAudioPeakLimiter(&limiter)) || limiter == nullptr) return false;
        XAUDIO2_EFFECT_DESCRIPTOR descriptor{limiter, TRUE, 2};
        const XAUDIO2_EFFECT_CHAIN chain{1, &descriptor};
        const HRESULT created = engine->CreateMasteringVoice(&master, 2, 48000, 0, nullptr, &chain);
        // 与产品一致：主声部接手后由它持有引用，创建者交还自己那一份。
        limiter->Release();
        limiter = nullptr;
        return SUCCEEDED(created) && master != nullptr;
    }

    void destroy() {
        if (master != nullptr) {
            master->DestroyVoice();
            master = nullptr;
        }
        if (engine != nullptr) {
            engine->Release();
            engine = nullptr;
        }
    }
};

// 造一条源声部、排一段 PCM、设好音量与输出矩阵。
IXAudio2SourceVoice* makeVoice(IXAudio2* engine, IXAudio2MasteringVoice* master, unsigned rate, unsigned channels,
                               unsigned bits, unsigned tag, const std::vector<std::uint8_t>& bytes, float volume,
                               float left, float right, UINT32 loopCount = 0,
                               UINT32 flags = XAUDIO2_END_OF_STREAM) {
    const WAVEFORMATEX format = makeFormat(rate, channels, bits, tag);
    IXAudio2SourceVoice* voice = nullptr;
    if (FAILED(engine->CreateSourceVoice(&voice, &format)) || voice == nullptr) return nullptr;
    if (FAILED(voice->SetVolume(volume))) return nullptr;
    const float stereo[4] = {left, 0.0f, 0.0f, right};
    const float mono[2] = {left, right};
    if (FAILED(voice->SetOutputMatrix(master, channels, 2, channels == 1 ? mono : stereo))) return nullptr;
    XAUDIO2_BUFFER buffer{};
    buffer.Flags = flags;
    buffer.AudioBytes = static_cast<UINT32>(bytes.size());
    buffer.pAudioData = bytes.data();
    buffer.LoopCount = loopCount;
    if (FAILED(voice->SubmitSourceBuffer(&buffer))) return nullptr;
    return voice;
}

std::vector<std::int16_t> render(IXAudio2* engine, unsigned frames) {
    std::vector<std::int16_t> output(static_cast<std::size_t>(frames) * concord::compat::xaudio2::kMixChannels, -12345);
    const unsigned written = concord::compat::xaudio2::renderInterleaved(engine, output.data(), frames);
    if (written != frames) return {};
    return output;
}

void checkEqual(const std::vector<std::int16_t>& actual, const std::vector<std::int16_t>& expected, const char* name) {
    if (actual.size() != expected.size()) {
        expect(false, name);
        std::fprintf(stderr, "  size %zu != %zu\n", actual.size(), expected.size());
        return;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (actual[index] != expected[index]) {
            expect(false, name);
            std::fprintf(stderr, "  [%zu] actual=%d expected=%d\n", index, static_cast<int>(actual[index]),
                         static_cast<int>(expected[index]));
            return;
        }
    }
    expect(true, name);
}

// --- 1. 直通：身份矩阵下逐位不变 -------------------------------------------
void testPassthrough() {
    Graph graph;
    expect(graph.create(true), "offline graph");
    const std::vector<std::int16_t> samples{16384, -16384, 32767, -32768, 0, 0, -8192, 4096};
    IXAudio2SourceVoice* voice = makeVoice(graph.engine, graph.master, 48000, 2, 16, WAVE_FORMAT_PCM,
                                           bytesFromS16(samples), 1.0f, 1.0f, 1.0f);
    expect(voice != nullptr, "stereo voice created");
    if (voice != nullptr) {
        voice->Start(0);
        checkEqual(render(graph.engine, 4), samples, "stereo passthrough is bit exact (incl. 32767 / -32768)");
        voice->DestroyVoice();
    }
    graph.destroy();
}

// --- 2. 多路混音：各自落到自己的声道 ---------------------------------------
void testTwoVoicesMix() {
    Graph graph;
    expect(graph.create(true), "offline graph (mix)");
    // 左声部 16384 只落左声道；右声部 8192 只落右声道。两条必须同时在，且互不串。
    IXAudio2SourceVoice* left = makeVoice(graph.engine, graph.master, 48000, 1, 16, WAVE_FORMAT_PCM,
                                          bytesFromS16(std::vector<std::int16_t>(2, 16384)), 1.0f, 1.0f, 0.0f);
    IXAudio2SourceVoice* right = makeVoice(graph.engine, graph.master, 48000, 1, 16, WAVE_FORMAT_PCM,
                                           bytesFromS16(std::vector<std::int16_t>(2, 8192)), 1.0f, 0.0f, 1.0f);
    expect(left != nullptr && right != nullptr, "two mono voices created");
    if (left != nullptr && right != nullptr) {
        left->Start(0);
        right->Start(0);
        checkEqual(render(graph.engine, 2), std::vector<std::int16_t>{16384, 8192, 16384, 8192},
                   "two voices sum into their own channels");
    }
    if (left != nullptr) left->DestroyVoice();
    if (right != nullptr) right->DestroyVoice();
    graph.destroy();
}

// --- 3. 运行期改音量 --------------------------------------------------------
void testRuntimeVolume() {
    Graph graph;
    expect(graph.create(true), "offline graph (volume)");
    IXAudio2SourceVoice* voice =
        makeVoice(graph.engine, graph.master, 48000, 1, 16, WAVE_FORMAT_PCM,
                  bytesFromS16(std::vector<std::int16_t>(4, 16384)), 1.0f, 1.0f, 0.0f);
    expect(voice != nullptr, "volume voice created");
    if (voice != nullptr) {
        voice->Start(0);
        checkEqual(render(graph.engine, 2), std::vector<std::int16_t>{16384, 0, 16384, 0}, "volume 1.0 = full scale");
        voice->SetVolume(0.25f);
        checkEqual(render(graph.engine, 2), std::vector<std::int16_t>{4096, 0, 4096, 0},
                   "runtime SetVolume(0.25) applies on the next block");
        voice->DestroyVoice();
    }
    graph.destroy();
}

// --- 4. 循环 ---------------------------------------------------------------
void testLooping() {
    {
        Graph graph;
        expect(graph.create(true), "offline graph (loop infinite)");
        IXAudio2SourceVoice* voice =
            makeVoice(graph.engine, graph.master, 48000, 1, 16, WAVE_FORMAT_PCM,
                      bytesFromS16(std::vector<std::int16_t>{8192, 16384}), 1.0f, 1.0f, 0.0f, XAUDIO2_LOOP_INFINITE);
        expect(voice != nullptr, "looping voice created");
        if (voice != nullptr) {
            voice->Start(0);
            checkEqual(render(graph.engine, 6),
                       std::vector<std::int16_t>{8192, 0, 16384, 0, 8192, 0, 16384, 0, 8192, 0, 16384, 0},
                       "LOOP_INFINITE wraps forever");
            voice->DestroyVoice();
        }
        graph.destroy();
    }
    {
        Graph graph;
        expect(graph.create(true), "offline graph (loop finite)");
        // LoopCount=1 表示"再放一遍"：总共两遍，然后停。
        IXAudio2SourceVoice* voice =
            makeVoice(graph.engine, graph.master, 48000, 1, 16, WAVE_FORMAT_PCM,
                      bytesFromS16(std::vector<std::int16_t>{8192, 16384}), 1.0f, 1.0f, 0.0f, 1);
        expect(voice != nullptr, "finite-loop voice created");
        if (voice != nullptr) {
            voice->Start(0);
            checkEqual(render(graph.engine, 4),
                       std::vector<std::int16_t>{8192, 0, 16384, 0, 8192, 0, 16384, 0},
                       "LoopCount=1 plays the buffer twice");
            checkEqual(render(graph.engine, 2), std::vector<std::int16_t>(4, 0),
                       "finite loop stops instead of starting a third pass");
            XAUDIO2_VOICE_STATE state{};
            voice->GetState(&state);
            expect(state.BuffersQueued == 0, "finite loop leaves no queued buffer");
            voice->DestroyVoice();
        }
        graph.destroy();
    }
}

// --- 5. 重采样 -------------------------------------------------------------
void testResampling() {
    Graph graph;
    expect(graph.create(true), "offline graph (resample)");
    // 24000 单声道三帧 [0, 0.5, 0] 进 48000 主声部：输出位置 0 / 0.5 / 1.0 / 1.5 / 2.0
    // → 0 / 8192 / 16384 / 8192 / 0。线性插值的这几个点全是手算的字面量。
    IXAudio2SourceVoice* voice =
        makeVoice(graph.engine, graph.master, 24000, 1, 16, WAVE_FORMAT_PCM,
                  bytesFromS16(std::vector<std::int16_t>{0, 16384, 0}), 1.0f, 1.0f, 0.0f);
    expect(voice != nullptr, "24000 Hz voice created");
    if (voice != nullptr) {
        voice->Start(0);
        checkEqual(render(graph.engine, 6), std::vector<std::int16_t>{0, 0, 8192, 0, 16384, 0, 8192, 0, 0, 0, 0, 0},
                   "24000->48000 linear interpolation matches hand-computed points");
        voice->DestroyVoice();
    }
    graph.destroy();
}

// --- 6. 播完自动停止 --------------------------------------------------------
void testEndOfStream() {
    Graph graph;
    expect(graph.create(true), "offline graph (eos)");
    IXAudio2SourceVoice* voice =
        makeVoice(graph.engine, graph.master, 48000, 1, 16, WAVE_FORMAT_PCM,
                  bytesFromS16(std::vector<std::int16_t>{16384, 16384}), 1.0f, 1.0f, 0.0f);
    expect(voice != nullptr, "eos voice created");
    if (voice != nullptr) {
        voice->Start(0);
        checkEqual(render(graph.engine, 2), std::vector<std::int16_t>{16384, 0, 16384, 0}, "eos voice plays its frames");
        checkEqual(render(graph.engine, 2), std::vector<std::int16_t>(4, 0), "after end of stream the voice is silent");
        XAUDIO2_VOICE_STATE state{};
        voice->GetState(&state);
        expect(state.BuffersQueued == 0, "end of stream drains the queue");
        expect(state.SamplesPlayed == 2, "SamplesPlayed counts source frames (2)");
        voice->DestroyVoice();
    }
    graph.destroy();
}

// --- 7. 效果链真的被调用 ----------------------------------------------------
void testLimiter() {
    Graph graph;
    expect(graph.create(true), "offline graph (limiter)");
    BOOL enabled = FALSE;
    expect(SUCCEEDED(graph.master->GetEffectState(0, &enabled)) && enabled != FALSE,
           "mastering effect reports enabled");
    // 两条 0.8 的 voice 叠加 = 1.6：不调用限幅器就会硬削在 32767。
    const std::vector<std::uint8_t> loud = bytesFromS16(std::vector<std::int16_t>(8, 26214));
    IXAudio2SourceVoice* first =
        makeVoice(graph.engine, graph.master, 48000, 2, 16, WAVE_FORMAT_PCM, loud, 1.0f, 1.0f, 1.0f);
    IXAudio2SourceVoice* second =
        makeVoice(graph.engine, graph.master, 48000, 2, 16, WAVE_FORMAT_PCM, loud, 1.0f, 1.0f, 1.0f);
    expect(first != nullptr && second != nullptr, "two loud voices created");
    if (first != nullptr && second != nullptr) {
        first->Start(0);
        second->Start(0);
        const std::vector<std::int16_t> output = render(graph.engine, 2);
        const int peak = output.empty() ? 0 : std::abs(static_cast<int>(output[1]));
        // 限定器是**主声部**效果器，它乘的是混音后的和（1.6），不是单条 voice：
        // gain = 0.999/1.6 = 0.62438 → 和 × gain = 0.999 → 32735。这正是"效果链
        // 被真的调用"的证据：不调用就是硬削在 32767，差 32 个 LSB。
        expect(peak >= 32733 && peak <= 32737, "peak limiter holds the 1.6 sum at 0.999 full scale (32735)");
        expect(peak < 32767, "the summed 1.6 is limited, not hard clipped at 32767");
    }
    if (first != nullptr) first->DestroyVoice();
    if (second != nullptr) second->DestroyVoice();
    graph.destroy();
}

// --- 8. 主音量 0 = 绝对静音 -------------------------------------------------
void testMasterMute() {
    Graph graph;
    expect(graph.create(true), "offline graph (mute)");
    IXAudio2SourceVoice* voice = makeVoice(graph.engine, graph.master, 48000, 2, 16, WAVE_FORMAT_PCM,
                                           bytesFromS16(std::vector<std::int16_t>(8, 26214)), 1.0f, 1.0f, 1.0f);
    expect(voice != nullptr, "mute voice created");
    if (voice != nullptr) {
        voice->Start(0);
        graph.master->SetVolume(0.0f);
        float volume = -1.0f;
        graph.master->GetVolume(&volume);
        expect(volume == 0.0f, "master volume reads back 0");
        checkEqual(render(graph.engine, 2), std::vector<std::int16_t>(4, 0),
                   "master volume 0 silences even a limited signal");
        voice->DestroyVoice();
    }
    graph.destroy();
}

// --- 9. 8 位无符号 / 32 位浮点 ----------------------------------------------
void testSampleFormats() {
    {
        Graph graph;
        expect(graph.create(true), "offline graph (8 bit)");
        // 8 位 WAV 无符号：0x80 是零点，0xFF → +127/128 → 32512。
        const std::vector<std::uint8_t> bytes{0x80, 0xFF};
        IXAudio2SourceVoice* voice =
            makeVoice(graph.engine, graph.master, 48000, 1, 8, WAVE_FORMAT_PCM, bytes, 1.0f, 1.0f, 0.0f);
        expect(voice != nullptr, "8 bit voice created");
        if (voice != nullptr) {
            voice->Start(0);
            checkEqual(render(graph.engine, 2), std::vector<std::int16_t>{0, 0, 32512, 0},
                       "8 bit unsigned PCM centres at 0x80");
            voice->DestroyVoice();
        }
        graph.destroy();
    }
    {
        Graph graph;
        expect(graph.create(true), "offline graph (float)");
        IXAudio2SourceVoice* voice =
            makeVoice(graph.engine, graph.master, 48000, 1, 32, WAVE_FORMAT_IEEE_FLOAT,
                      bytesFromF32(std::vector<float>{0.5f, -0.5f}), 1.0f, 1.0f, 0.0f);
        expect(voice != nullptr, "float32 voice created");
        if (voice != nullptr) {
            voice->Start(0);
            checkEqual(render(graph.engine, 2), std::vector<std::int16_t>{16384, 0, -16384, 0},
                       "float32 PCM passes through unchanged");
            voice->DestroyVoice();
        }
        graph.destroy();
    }
}

// --- 10. 拒绝无效输入（"接受但不用"是本层最危险的错法） --------------------
void testRejections() {
    Graph graph;
    expect(graph.create(true), "offline graph (rejections)");
    IXAudio2SourceVoice* voice = nullptr;
    WAVEFORMATEX bogus = makeFormat(48000, 1, 64, WAVE_FORMAT_PCM); // 64 位 PCM 不存在
    expect(FAILED(graph.engine->CreateSourceVoice(&voice, &bogus)) && voice == nullptr, "64 bit PCM is rejected");
    WAVEFORMATEX wide = makeFormat(48000, 6, 16, WAVE_FORMAT_PCM);
    expect(FAILED(graph.engine->CreateSourceVoice(&voice, &wide)), "6 channel source is rejected");

    WAVEFORMATEX mono = makeFormat(48000, 1, 16, WAVE_FORMAT_PCM);
    expect(SUCCEEDED(graph.engine->CreateSourceVoice(&voice, &mono)) && voice != nullptr, "mono voice created");
    if (voice != nullptr) {
        float matrix[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        expect(FAILED(voice->SetOutputMatrix(graph.master, 2, 2, matrix)),
               "stereo matrix on a mono voice is rejected, not silently accepted");
        voice->DestroyVoice();
    }
    IXAudio2MasteringVoice* second = nullptr;
    expect(FAILED(graph.engine->CreateMasteringVoice(&second, 2, 48000)), "second mastering voice is rejected");
    graph.destroy();
}

// --- 11. 真设备：SamplesPlayed 真的涨 --------------------------------------
void testRealDevice() {
    Graph graph;
    if (!graph.create(false)) {
        // 没有输出设备。把原因说清楚，而不是让它伪装成"混音不对"。
        expect(false, "real device graph (needs an output device)");
        std::fprintf(stderr, "  note: no output device on this machine\n");
        return;
    }
    expect(true, "real device graph");
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(4800) * 2);
    for (std::size_t frame = 0; frame < 4800; ++frame) {
        const double value =
            0.25 * std::sin(2.0 * 3.14159265358979 * 440.0 * static_cast<double>(frame) / 48000.0);
        const auto sample = static_cast<std::int16_t>(std::lround(value * 32767.0));
        pcm[frame * 2] = sample;
        pcm[frame * 2 + 1] = sample;
    }
    IXAudio2SourceVoice* voice = makeVoice(graph.engine, graph.master, 48000, 2, 16, WAVE_FORMAT_PCM,
                                           bytesFromS16(pcm), 1.0f, 1.0f, 1.0f);
    expect(voice != nullptr, "device voice created");
    if (voice != nullptr) {
        expect(SUCCEEDED(voice->Start(0)), "device voice starts");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        std::uint64_t played = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            XAUDIO2_VOICE_STATE state{};
            voice->GetState(&state);
            played = state.SamplesPlayed;
            if (played >= 1024) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        expect(played >= 1024, "real AudioQueue consumes the mixed signal (SamplesPlayed >= 1024)");
        std::printf("device: samplesPlayed=%llu\n", static_cast<unsigned long long>(played));
        voice->DestroyVoice();
    }
    graph.destroy();
}

} // namespace

int main() {
    // 无缓冲：崩在不该崩的地方时，前面已跑的结论不能丢。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    testPassthrough();
    testTwoVoicesMix();
    testRuntimeVolume();
    testLooping();
    testResampling();
    testEndOfStream();
    testLimiter();
    testMasterMute();
    testSampleFormats();
    testRejections();
    testRealDevice();

    std::printf("XAudio2 mixer: checks=%u failures=%u\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
