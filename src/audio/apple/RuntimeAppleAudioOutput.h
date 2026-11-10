#pragma once
// P4-01a: macOS 侧的音频底座。
//
// 这一层只做两件事：把 RIFF/WAVE 解成 PCM，把 PCM 交给系统输出流。
// 它**不认识**项目的任何设备接口（RuntimeLegacyWaveDevice / MenuMusicDevice /
// AsyncSoundDevice / MenuSoundDevice / CampaignMapAudioDevice）——那 5 个接缝是
// P4-01b 的工厂负责的。这样分开的理由是：解码与设备可以各自用硬编码断言测，
// 而工厂只负责"会话语义"（谁拥有别名、替换时是否关闭上一个、析构要不要停）。
//
// 后端选择：AudioToolbox / AudioQueue（系统自带，零外部依赖）。
// 之所以不是 SDL3：本机无 SDL3 安装亦无源码，且 cmake/Sdl3Window.cmake 明确
// 不允许自动下载；P3-01（SDL3 窗口）已被 superseded，SDL3 不再是共享依赖。
// 若日后要换回 SDL3，只需替换本文件的实现，接口与工厂不动。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace concord::audio::apple {

// 统一表示：交错、16 位有符号、小端。8/24/32 位源在解码时归一到这里。
struct DecodedPcm {
    unsigned sampleRate = 0;
    unsigned channels = 0;
    std::vector<std::int16_t> samples; // interleaved: frame0.L, frame0.R, frame1.L, ...

    std::uint64_t frames() const {
        return channels == 0 ? 0 : static_cast<std::uint64_t>(samples.size()) / channels;
    }
};

// 内存解码。失败一律返回 false 并写 error；**不存在"返回 true 但空缓冲区"**，
// 因为那样的失败会在设备层变成一个静音的、看不出错的成功播放。
bool decodeWav(const std::vector<std::uint8_t>& bytes, DecodedPcm& out, std::string& error);

// 文件解码。读不到文件同样返回 false（不是"当成静音播放"）。
bool decodeWavFile(const std::string& path, DecodedPcm& out, std::string& error);

// 一条输出流。语义是"替换式"：play() 会先停掉并释放上一条流，再从零开始播放。
// 这与被替换的 Win32 实现（MCI alias close→open→play from 0）一致，也是产品
// 每个通道实际的用法——通道只有一个当前声音。
class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    // 设备是否可用（能否建出输出队列）。探测失败要能被上层看见，不能假装支持。
    static std::unique_ptr<AudioOutput> create(std::string& error);

    // pcm 必须来自 decodeWav()；loop 为真时到尾部自动回绕。
    // volumePercent 夹到 0..100，0 表示静音但**仍然占用设备并推进播放**。
    virtual bool play(const DecodedPcm& pcm, bool loop, int volumePercent, std::string& error) = 0;

    // 真实进度：已交给设备并被消费的帧数（回绕时累加）。
    virtual std::uint64_t framesConsumed() const = 0;
    // 队列还在放（末缓冲尚未播完）。停止、播完、被替换都返回 false。
    virtual bool isPlaying() const = 0;
    virtual void stop() = 0;

    // 最近一次 play() 用的格式（供上层判断"格式变了要重开"）。
    virtual unsigned sampleRate() const = 0;
    virtual unsigned channels() const = 0;
};

} // namespace concord::audio::apple
