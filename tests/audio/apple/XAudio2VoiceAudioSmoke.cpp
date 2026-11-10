// ============================================================================
// tests/audio/apple/XAudio2VoiceAudioSmoke.cpp
//
// P4-02 的产品级验收：**产品自己那条路径**（RuntimeVoiceAudio）真的活了。
//
// 混音数学由 XAudio2MixerSmoke 逐样本钉住（那一个用离线引擎、不依赖声卡）。
// 这里测的是另外三件事，任何一件在"空壳引擎"下都不成立：
//
//   1. RuntimeVoiceAudio::ready() == true 且 lastError() == S_OK。
//      P4-01b 之前这里是 E_NOTIMPL / ready()==false —— 那不是设备问题，是没实现。
//   2. 两条 voice **各自**消费自己的 PCM（不同采样率/声道数）：22050 单声道 与
//      44100 立体声同时播，各自的 SamplesPlayed 都在涨。用一个共享播放器是做不到的。
//   3. setMix 的增益/声像真的落到引擎的矩阵上（probe() 读回来对得上），
//      并且 stop/close/queue 的会话语义没被引擎改写。
//
// 构造 RuntimeVoiceAudio 时传 mute=true：这条测试只关心"帧被真的消费"，不关心
// 房间里的声音；mute 走的是主声部音量 0 那条路，正好顺带钉住"verification mute
// 必须绝对静音"（离线自测里那条 0 输出的断言是同一个语义）。
// ============================================================================

#include <windows.h>

#include "audio/spatial/RuntimeSpatialAudio.h"
#include "audio/backend/RuntimeVoiceAudio.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
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

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    put16(bytes, static_cast<std::uint16_t>(value & 0xFFFF));
    put16(bytes, static_cast<std::uint16_t>(value >> 16));
}

// 写一个 PCM WAV。**自己写**而不是找素材：语音路径读的是 PCM（IMA ADPCM 在
// RuntimeVoiceAudio::readPcm 里是明确不支持的），而项目散装素材 152/152 是 ADPCM。
// 拿素材来测只会测到"open 失败"。
bool writeWav(const std::string& path, unsigned rate, unsigned channels, unsigned frames) {
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) * channels);
    for (unsigned frame = 0; frame < frames; ++frame) {
        const double value = 0.05 * std::sin(2.0 * 3.14159265358979 * 440.0 * frame / rate);
        const auto sample = static_cast<std::int16_t>(std::lround(value * 32767.0));
        for (unsigned channel = 0; channel < channels; ++channel) {
            pcm[static_cast<std::size_t>(frame) * channels + channel] =
                static_cast<std::int16_t>(sample + (channel == 1 ? 1 : 0));
        }
    }
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    put32(bytes, static_cast<std::uint32_t>(36 + pcm.size() * 2));
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(bytes, 16);
    put16(bytes, 1); // WAVE_FORMAT_PCM
    put16(bytes, static_cast<std::uint16_t>(channels));
    put32(bytes, rate);
    put32(bytes, rate * channels * 2);
    put16(bytes, static_cast<std::uint16_t>(channels * 2));
    put16(bytes, 16);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    put32(bytes, static_cast<std::uint32_t>(pcm.size() * 2));
    for (std::int16_t sample : pcm) put16(bytes, static_cast<std::uint16_t>(sample));

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

bool waitForSamples(const RuntimeVoiceAudio& audio, std::uint64_t handle, std::uint64_t target, int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (audio.probe(handle).samplesPlayed >= target) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return audio.probe(handle).samplesPlayed >= target;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    const std::string monoPath = "/tmp/concord_voice_22050_mono.wav";
    const std::string stereoPath = "/tmp/concord_voice_44100_stereo.wav";
    expect(writeWav(monoPath, 22050, 1, 4410), "wrote 22050 mono PCM fixture");
    expect(writeWav(stereoPath, 44100, 2, 8820), "wrote 44100 stereo PCM fixture");

    RuntimeVoiceAudio audio(true);
    expect(audio.ready(), "RuntimeVoiceAudio::ready() with the real XAudio2 engine");
    expect(audio.lastError() == S_OK, "lastError() is S_OK, not E_NOTIMPL");

    // 打不开的文件必须是"失败"，不能是"静音播放成功"。
    expect(!audio.open(9, "/tmp/concord_voice_missing.wav"), "missing file fails to open");
    expect(audio.lastError() == E_INVALIDARG, "missing file reports E_INVALIDARG");
    expect(audio.voiceCount() == 0, "a failed open owns no voice");

    expect(audio.open(1, monoPath), "22050 mono PCM opens");
    expect(audio.open(2, stereoPath), "44100 stereo PCM opens");
    expect(audio.voiceCount() == 2, "two source voices are owned at once");

    const RuntimeVoiceAudioProbe probe = audio.probe(1);
    expect(probe.available, "voice 1 is available");
    expect(probe.channels == 1, "voice 1 keeps its own channel count (1)");
    expect(probe.sampleRate == 22050, "voice 1 keeps its own sample rate (22050)");
    expect(probe.bitsPerSample == 16, "voice 1 keeps its own bit depth (16)");
    expect(probe.frameBytes == 2, "voice 1 keeps its own frame size (2)");
    expect(probe.masteringLimiterEnabled, "mastering limiter is attached and enabled");

    // 增益满档 + 右偏 50 → 左 10^(-1000/2000)=0.3162，右 1.0。矩阵必须原样读回来。
    const RuntimeAudioMix mix = runtimeRetailAudioMix(16384, 12288);
    expect(audio.setMix(1, 16384, 12288), "setMix(16384, 12288) accepted");
    const RuntimeVoiceAudioProbe mixed = audio.probe(1);
    expect(mixed.matrix.size() == 2, "mono voice reports a 1x2 matrix");
    if (mixed.matrix.size() == 2) {
        expect(std::fabs(mixed.matrix[0] - mix.left) < 1e-6f, "matrix[0] is the computed left gain (0.3162)");
        expect(std::fabs(mixed.matrix[1] - mix.right) < 1e-6f, "matrix[1] is the computed right gain (1.0)");
        expect(mixed.matrix[0] < 0.6f && mixed.matrix[1] > 0.99f, "pan is asymmetric, not a constant");
    }
    expect(std::fabs(mixed.volume - mix.volume) < 1e-6f, "voice volume equals the computed gain");

    expect(audio.playFromStart(1, true, false), "voice 1 starts looping");
    expect(audio.playFromStart(2, true, false), "voice 2 starts looping");
    expect(audio.isPlaying(1) == std::optional<bool>(true), "voice 1 reports playing");
    expect(audio.queuedBuffers(1) == 1, "voice 1 owns exactly one queued buffer");

    // 两条 voice 同时被消费：这是"真的多路混音"在产品路径上的唯一硬证据。
    const bool monoAdvanced = waitForSamples(audio, 1, 4096, 3000);
    const bool stereoAdvanced = waitForSamples(audio, 2, 8192, 3000);
    expect(monoAdvanced, "22050 mono voice consumed >= 4096 source frames");
    expect(stereoAdvanced, "44100 stereo voice consumed >= 8192 source frames while the other kept playing");
    const std::uint64_t monoPlayed = audio.probe(1).samplesPlayed;
    const std::uint64_t stereoPlayed = audio.probe(2).samplesPlayed;
    expect(monoPlayed > 0 && stereoPlayed > 0, "both voices report progress");

    expect(audio.stop(1), "voice 1 stops");
    expect(audio.isPlaying(1) == std::optional<bool>(false), "stopped voice reports not playing");
    expect(audio.isPlaying(2) == std::optional<bool>(true), "voice 2 is unaffected by stopping voice 1");

    // 连续序列：同一格式的第二段可以接在后面（产品用它做长素材无缝衔接）。
    expect(audio.open(3, monoPath), "third voice opens for the queue test");
    expect(audio.setMix(3, 16384, 8192), "third voice mix accepted");
    expect(audio.playFromStart(3, false, true), "third voice starts as a continuous sequence");
    expect(audio.queue(3, monoPath), "queue() appends a compatible buffer");
    expect(audio.queuedBuffers(3) >= 2, "queued buffer count reflects the appended buffer");
    expect(!audio.queue(3, stereoPath), "queue() rejects a buffer whose format differs");
    expect(audio.stop(3), "third voice stops");

    audio.close(1);
    audio.close(2);
    audio.close(3);
    expect(audio.voiceCount() == 0, "closing every handle drops all voices");

    std::printf("RuntimeVoiceAudio: checks=%u failures=%u ready=%d monoFrames=%llu stereoFrames=%llu leftGain=%.4f\n",
                checks, failures, audio.ready() ? 1 : 0, static_cast<unsigned long long>(monoPlayed),
                static_cast<unsigned long long>(stereoPlayed), static_cast<double>(mix.left));
    return failures == 0 ? 0 : 1;
}
