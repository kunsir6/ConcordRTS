// P4-01a 自测：RIFF/WAVE 解码 + AudioQueue 输出流。
//
// 为什么不用"跑一遍产品再听"来验收：产品级 `concord_runtime` 此刻还链不出来
// （剩 7 个音频工厂符号），而且"能听见"不是可复现的判据。所以这一层钉两件
// 能自动判的事：
//   1) 解码的数字：帧数、样本值、非法输入的失败原因。全部写死字面量，
//      不用"再从实现里算一遍"的期望值——那种断言在实现错时同时错，永远绿。
//   2) 输出流的时间：一段 300ms 的素材，从 play() 到 isPlaying() 转 false 的
//      墙钟必须 ≥250ms。若实现只是把指针交给设备就返回，或者根本没建队列，
//      这个下界立刻掉下来。这是本机上"真的在播"能做到的最强证据。

#include "audio/apple/RuntimeAppleAudioOutput.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check(bool value, const char* why) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL %s\n", why);
    }
}

using Bytes = std::vector<std::uint8_t>;
using namespace concord::audio::apple;

void putU16(Bytes& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void putU32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void putTag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }
void putU16Out(std::FILE* file, unsigned v) {
    const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(v & 0xFF),
                                   static_cast<std::uint8_t>((v >> 8) & 0xFF)};
    std::fwrite(bytes, 1, 2, file);
}
void patchU32(Bytes& b, std::size_t offset, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

// 造一个最小合法 WAV。preData 用来插"data 之前还有别的块"的场景。
Bytes makeWav(unsigned formatTag, unsigned channels, unsigned rate, unsigned bits, const Bytes& data,
              const Bytes& preData = Bytes{}) {
    Bytes b;
    const unsigned frameBytes = channels * bits / 8;
    putTag(b, "RIFF");
    putU32(b, 0);
    putTag(b, "WAVE");
    b.insert(b.end(), preData.begin(), preData.end());
    putTag(b, "fmt ");
    putU32(b, 16);
    putU16(b, formatTag);
    putU16(b, channels);
    putU32(b, rate);
    putU32(b, rate * frameBytes);
    putU16(b, frameBytes);
    putU16(b, bits);
    putTag(b, "data");
    putU32(b, static_cast<std::uint32_t>(data.size()));
    b.insert(b.end(), data.begin(), data.end());
    patchU32(b, 4, static_cast<std::uint32_t>(b.size() - 8));
    return b;
}

// WAVE_FORMAT_EXTENSIBLE：真正的编码在 SubFormat GUID 前两字节。
Bytes makeWavExtensible(unsigned subFormat, unsigned channels, unsigned rate, unsigned bits, const Bytes& data) {
    Bytes b;
    const unsigned frameBytes = channels * bits / 8;
    putTag(b, "RIFF");
    putU32(b, 0);
    putTag(b, "WAVE");
    putTag(b, "fmt ");
    putU32(b, 40);
    putU16(b, 0xFFFE);
    putU16(b, channels);
    putU32(b, rate);
    putU32(b, rate * frameBytes);
    putU16(b, frameBytes);
    putU16(b, bits);
    putU16(b, 22);    // cbSize
    putU16(b, bits);  // validBits
    putU32(b, 0x3);   // channelMask
    putU16(b, subFormat);
    for (int i = 0; i < 14; ++i) b.push_back(0); // GUID 剩余部分：解码只看前两字节
    putTag(b, "data");
    putU32(b, static_cast<std::uint32_t>(data.size()));
    b.insert(b.end(), data.begin(), data.end());
    patchU32(b, 4, static_cast<std::uint32_t>(b.size() - 8));
    return b;
}

// IMA ADPCM（tag 0x11）：fmt 要带 cbSize + samplesPerBlock，data 按块切。
Bytes makeWavAdpcm(unsigned channels, unsigned rate, unsigned blockAlign, unsigned samplesPerBlock,
                   const Bytes& data) {
    Bytes b;
    putTag(b, "RIFF");
    putU32(b, 0);
    putTag(b, "WAVE");
    putTag(b, "fmt ");
    putU32(b, 20);
    putU16(b, 0x11);
    putU16(b, channels);
    putU32(b, rate);
    putU32(b, rate * blockAlign / samplesPerBlock); // byteRate：解码不用，但写对
    putU16(b, blockAlign);
    putU16(b, 4);   // bitsPerSample
    putU16(b, 2);   // cbSize
    putU16(b, samplesPerBlock);
    putTag(b, "data");
    putU32(b, static_cast<std::uint32_t>(data.size()));
    b.insert(b.end(), data.begin(), data.end());
    patchU32(b, 4, static_cast<std::uint32_t>(b.size() - 8));
    return b;
}

// 一个单声道 IMA 块：4 字节块头 + nibble 字节。
Bytes imaBlock(std::int16_t predictor, unsigned stepIndex, const Bytes& nibbles) {
    Bytes b;
    putU16(b, static_cast<unsigned>(static_cast<std::uint16_t>(predictor)));
    b.push_back(static_cast<std::uint8_t>(stepIndex));
    b.push_back(0);
    b.insert(b.end(), nibbles.begin(), nibbles.end());
    return b;
}

// 立体声 IMA 块：两个 4 字节块头，然后**按声道每 4 字节一组**交错。
Bytes imaStereoBlock(std::int16_t leftPred, std::int16_t rightPred, const Bytes& leftNibbles,
                     const Bytes& rightNibbles) {
    Bytes b;
    putU16(b, static_cast<unsigned>(static_cast<std::uint16_t>(leftPred)));
    b.push_back(0);
    b.push_back(0);
    putU16(b, static_cast<unsigned>(static_cast<std::uint16_t>(rightPred)));
    b.push_back(0);
    b.push_back(0);
    b.insert(b.end(), leftNibbles.begin(), leftNibbles.end());
    b.insert(b.end(), rightNibbles.begin(), rightNibbles.end());
    return b;
}

// 交错的 16 位样本 → 小端字节
Bytes pcm16(const std::vector<std::int16_t>& samples) {
    Bytes b;
    for (const auto s : samples) putU16(b, static_cast<unsigned>(static_cast<std::uint16_t>(s)));
    return b;
}

Bytes pcm8(const std::vector<std::uint8_t>& samples) { return samples; }

void decodeChecks() {
    // 1. 16 位单声道：字节序与符号必须原样过来。
    {
        const Bytes file = makeWav(1, 1, 8000, 16, pcm16({0x1234, -2, 0x7FFF}));
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "16-bit mono decodes");
        check(pcm.sampleRate == 8000, "16-bit sample rate is 8000");
        check(pcm.channels == 1, "16-bit mono has 1 channel");
        check(pcm.frames() == 3, "16-bit mono has 3 frames");
        check(pcm.samples[0] == 0x1234, "first sample keeps little-endian value 0x1234");
        check(pcm.samples[1] == -2, "second sample keeps sign (-2)");
        check(pcm.samples[2] == 0x7FFF, "third sample is full scale");
    }
    // 2. 8 位 WAV 是**无符号**：0x80 才是零点。这条反了整首歌是噪声但"能播"。
    {
        const Bytes file = makeWav(1, 1, 22050, 8, pcm8({0x80, 0xFF, 0x00}));
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "8-bit mono decodes");
        check(pcm.samples[0] == 0, "8-bit 0x80 is silence");
        check(pcm.samples[1] == 0x7F00, "8-bit 0xFF is +127/128 full scale");
        check(pcm.samples[2] == -32768, "8-bit 0x00 is negative full scale");
    }
    // 3. 立体声交错顺序
    {
        const Bytes file = makeWav(1, 2, 44100, 16, pcm16({0x0102, 0x0304, 0x0506, 0x0708}));
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "stereo decodes");
        check(pcm.channels == 2, "stereo has 2 channels");
        check(pcm.frames() == 2, "stereo 4 samples is 2 frames");
        check(pcm.samples[1] == 0x0304, "right channel stays second (no L/R swap)");
        check(pcm.samples[3] == 0x0708, "second frame right channel keeps order");
    }
    // 4. data 之前有一个奇数长度的块：必须按偶数字节对齐跳过它。
    {
        Bytes list;
        putTag(list, "LIST");
        putU32(list, 5);
        list.insert(list.end(), {'a', 'b', 'c', 'd', 'e', 0}); // 5 字节 + 1 填充
        const Bytes file = makeWav(1, 1, 8000, 16, pcm16({0x0A0B}), list);
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "odd-sized chunk before data is skipped");
        check(pcm.frames() == 1, "data after odd chunk is still found");
        check(pcm.samples[0] == 0x0A0B, "data payload after odd chunk is intact");
    }
    // 5. data 声明长度大于实际文件：以实际字节为准，绝不越界读。
    {
        Bytes file = makeWav(1, 1, 8000, 16, pcm16({0x1111, 0x2222}));
        patchU32(file, 40, 4000); // data size 谎报 4000 字节
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "oversized data chunk still decodes");
        check(pcm.frames() == 2, "oversized data chunk clamps to 2 real frames");
    }
    // 6. data 长度不是整帧：尾部的半个帧丢掉，不四舍五入。
    {
        Bytes file = makeWav(1, 2, 8000, 16, pcm16({0x1111, 0x2222, 0x3333}));
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "partial trailing frame still decodes");
        check(pcm.frames() == 1, "partial trailing frame is dropped (3 samples -> 1 stereo frame)");
        check(pcm.samples.size() == 2, "partial frame samples are dropped, not padded");
    }
    // 7. 非 RIFF：必须报错，不能"当成静音播放"
    {
        const Bytes file(64, 0x41);
        DecodedPcm pcm;
        std::string error;
        check(!decodeWav(file, pcm, error), "non-RIFF input is rejected");
        check(!error.empty(), "non-RIFF rejection carries a reason");
        check(pcm.frames() == 0, "non-RIFF leaves no phantom frames");
    }
    // 8. 压缩编码（ADPCM tag 2）：明确拒绝，不猜
    {
        const Bytes file = makeWav(2, 1, 22050, 4, Bytes(64, 0));
        DecodedPcm pcm;
        std::string error;
        check(!decodeWav(file, pcm, error), "ADPCM input is rejected");
        check(error.find("codec") != std::string::npos, "ADPCM rejection names the codec");
    }
    // 9. 24 位：取高 16 位（0x400000 的高 16 位是 0x4000）
    {
        const Bytes data{0x00, 0x00, 0x40}; // 0x400000 -> 0x4000
        const Bytes file = makeWav(1, 1, 8000, 24, data);
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "24-bit decodes");
        check(pcm.frames() == 1, "24-bit one frame");
        check(pcm.samples[0] == 0x4000, "24-bit keeps the high 16 bits");
    }
    // 10. 32 位：取高 16 位
    {
        Bytes data;
        putU32(data, 0x00010000);
        const Bytes file = makeWav(1, 1, 8000, 32, data);
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "32-bit decodes");
        check(pcm.samples[0] == 1, "32-bit keeps the high 16 bits");
    }
    // 11. 空 data：报错，不返回空但成功的缓冲
    {
        const Bytes file = makeWav(1, 1, 8000, 16, Bytes{});
        DecodedPcm pcm;
        std::string error;
        check(!decodeWav(file, pcm, error), "empty data chunk is rejected");
        check(pcm.frames() == 0, "empty data chunk yields no frames");
    }
    // 12/13. EXTENSIBLE：子格式 PCM 放行，子格式 ADPCM 拒绝
    {
        const Bytes pcmfile = makeWavExtensible(1, 2, 48000, 16, pcm16({0x1234, 0x5678}));
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(pcmfile, pcm, error), "WAVE_FORMAT_EXTENSIBLE with PCM subformat decodes");
        check(pcm.sampleRate == 48000, "extensible keeps 48000");
        check(pcm.frames() == 1, "extensible one frame");
        const Bytes adpcm = makeWavExtensible(2, 2, 48000, 16, pcm16({0x1234, 0x5678}));
        DecodedPcm pcm2;
        std::string error2;
        check(!decodeWav(adpcm, pcm2, error2), "WAVE_FORMAT_EXTENSIBLE with ADPCM subformat is rejected");
    }
    // 14. IMA ADPCM 的步长/索引推进。nibble 字节 0x55 的低 4 位先出：
    //     第一个 5：step(0)=7 → diff=7>>3(0)+7+7>>2(1)=8 → 样本 8，index=4
    //     第二个 5：step(4)=11 → diff=11>>3(1)+11+11>>2(2)=14 → 样本 22，index=8
    //     这两个数字只有"表对 + 索引真的推进了"才同时成立。
    //     （我第一版把第二项写成 21，是按 7>>2 算的——step 已经变成 11，
    //       右移两位是 2 不是 1。这种错值只能靠逐项手算，算不出来就是没算。）
    {
        const Bytes file = makeWavAdpcm(1, 22050, 5, 3, imaBlock(0, 0, Bytes{0x55}));
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "IMA ADPCM block decodes");
        check(pcm.sampleRate == 22050, "IMA keeps 22050 Hz");
        check(pcm.frames() == 3, "IMA block header sample + 2 nibbles is 3 frames");
        check(pcm.samples[0] == 0, "IMA first frame is the block predictor");
        check(pcm.samples[1] == 8, "IMA first nibble steps to 8 (step table 7)");
        check(pcm.samples[2] == 22, "IMA second nibble steps to 22 (index advanced to step 11)");
    }
    // 15. 符号位：nibble 0xF 是"减最大步长"，不是"加最大步长"。
    {
        const Bytes file = makeWavAdpcm(1, 22050, 5, 3, imaBlock(0, 0, Bytes{0x0F}));
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "IMA negative nibble decodes");
        check(pcm.samples[1] == -11, "IMA nibble 0xF subtracts the full step (0 - 11)");
    }
    // 16. 块头预测值就是每块的第一个样本；多块必须逐块重取块头。
    {
        Bytes data = imaBlock(1000, 0, Bytes{0x00});
        const Bytes second = imaBlock(500, 0, Bytes{0x00});
        data.insert(data.end(), second.begin(), second.end());
        const Bytes file = makeWavAdpcm(1, 22050, 5, 3, data);
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "two IMA blocks decode");
        check(pcm.frames() == 6, "two IMA blocks give 6 frames");
        check(pcm.samples[0] == 1000, "first block starts at its predictor (1000)");
        check(pcm.samples[1] == 1000, "nibble 0 keeps the predictor");
        check(pcm.samples[3] == 500, "second block restarts from its own predictor (500)");
        check(pcm.samples[5] == 500, "second block nibble 0 keeps its predictor");
    }
    // 17. 末块被截断：只解完整的块，尾部半块不能读越界。
    {
        Bytes data = imaBlock(7, 0, Bytes{0x00});
        data.push_back(0x12); // 第二个块只剩 1 字节，连块头都不够
        const Bytes file = makeWavAdpcm(1, 22050, 5, 3, data);
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "truncated final IMA block still decodes the complete one");
        check(pcm.frames() == 3, "truncated final block contributes no frames");
    }
    // 18. blockAlign 小于一个块头：明确拒绝，不按 0 或负数去切。
    {
        const Bytes file = makeWavAdpcm(1, 22050, 2, 3, imaBlock(0, 0, Bytes{0x55}));
        DecodedPcm pcm;
        std::string error;
        check(!decodeWav(file, pcm, error), "IMA blockAlign smaller than one header is rejected");
    }
    // 19. 立体声 nibble 布局：按声道每 4 字节一组交错，不是按帧交错。
    //     这条是从 afconvert 逐样本比对里掉出来的：只测单声道时两种写法结果完全一样，
    //     所以单声道全绿也不能证明立体声对（10 个菜单音乐右声道曾全体错位）。
    //     左声道 nibble 全 0 → 恒等于预测值 1000。
    //     右声道 nibble 全 0xF → 逐次 -11、-30、-63…（步长索引表 indexTable[0xF]=8，
    //       不是 -1：15 的低 3 位是 7，索引每步 +8，所以差值越滚越大）。
    //     这里写的就是这串数值本身——它同时钉住"4 字节分组"和"索引表"两件事。
    {
        const Bytes data = imaStereoBlock(1000, -1000, Bytes(4, 0x00), Bytes(4, 0xFF));
        const Bytes file = makeWavAdpcm(2, 22050, 16, 9, data);
        DecodedPcm pcm;
        std::string error;
        check(decodeWav(file, pcm, error), "stereo IMA block decodes");
        check(pcm.channels == 2, "stereo IMA keeps 2 channels");
        check(pcm.frames() == 9, "stereo IMA block gives 9 frames");
        check(pcm.samples[0] == 1000, "left frame 0 is its predictor");
        check(pcm.samples[2] == 1000, "left nibble 0 keeps 1000");
        check(pcm.samples[16] == 1000, "left frame 8 keeps 1000 (four-byte group boundary)");
        check(pcm.samples[1] == -1000, "right frame 0 is its own predictor (-1000)");
        check(pcm.samples[3] == -1011, "right frame 1 subtracts one step (-1011, not -1000)");
        check(pcm.samples[5] == -1041, "right frame 2 keeps accelerating (-1041)");
        check(pcm.samples[17] == -6431, "right frame 8 has walked to -6431");
    }
}

// 一段 22050Hz 单声道 300ms 的低幅正弦：能听见但很轻，时长固定好算。
Bytes tone300ms(unsigned rate, unsigned millis) {
    const unsigned frames = rate * millis / 1000u;
    std::vector<std::int16_t> samples(frames);
    for (unsigned i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        samples[i] = static_cast<std::int16_t>(std::sin(2.0 * 3.14159265358979 * 440.0 * t) * 600.0);
    }
    return pcm16(samples);
}

void playbackChecks() {
    std::string error;
    std::unique_ptr<AudioOutput> output = AudioOutput::create(error);
    check(output != nullptr, "output device opens on this machine");
    if (!output) {
        std::printf("NOTE audio device unavailable: %s\n", error.c_str());
        return;
    }

    const unsigned rate = 22050;
    const unsigned millis = 300;
    DecodedPcm pcm;
    if (!decodeWav(makeWav(1, 1, rate, 16, tone300ms(rate, millis)), pcm, error)) {
        check(false, "300ms test tone decodes");
        return;
    }
    check(pcm.frames() == 6615, "300ms at 22050Hz is 6615 frames");

    // 2. 真的在播：墙钟下界证明设备确实按实时速率消费了这些帧。
    const auto start = std::chrono::steady_clock::now();
    check(output->play(pcm, false, 30, error), "play() starts the stream");
    check(output->isPlaying(), "isPlaying() is true right after play()");
    const auto deadline = start + std::chrono::seconds(5);
    while (output->isPlaying() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    check(!output->isPlaying(), "isPlaying() turns false at end of stream");
    check(static_cast<std::uint64_t>(output->framesConsumed()) == pcm.frames(),
          "every frame was handed to the device (6615)");
    check(elapsed >= 250, "playback took at least 250ms of wall clock (device really consumed it)");
    std::printf("NOTE played %u frames in %lld ms (nominal %u ms)\n", static_cast<unsigned>(pcm.frames()),
                static_cast<long long>(elapsed), millis);

    // 3. 替换：新格式必须重开队列，且上一次的流不能残留。
    DecodedPcm stereo;
    if (decodeWav(makeWav(1, 2, 8000, 16, pcm16({0x1111, 0x2222, 0x3333, 0x4444})), stereo, error)) {
        check(output->play(stereo, false, 60, error), "second play() replaces the first stream");
        check(output->sampleRate() == 8000, "replacement re-opens at the new sample rate");
        check(output->channels() == 2, "replacement re-opens at the new channel count");
        output->stop();
        check(!output->isPlaying(), "stop() ends the stream");
    } else {
        check(false, "stereo replacement fixture decodes");
    }

    // 4. 循环：会一直播下去（消费帧数越过素材长度），stop() 之后才停。
    DecodedPcm loopPcm;
    if (decodeWav(makeWav(1, 1, rate, 16, tone300ms(rate, 200)), loopPcm, error)) {
        check(output->play(loopPcm, true, 20, error), "looping play() starts");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        check(output->isPlaying(), "looping stream is still running after 500ms of a 200ms clip");
        check(output->framesConsumed() > loopPcm.frames(), "looping stream wrapped and consumed more than one pass");
        output->stop();
        check(!output->isPlaying(), "stop() ends a looping stream");
    } else {
        check(false, "loop fixture decodes");
    }

    // 5. 空缓冲必须拒绝：这是"静默成功"最容易溜进来的入口。
    DecodedPcm empty;
    std::string emptyError;
    check(!output->play(empty, false, 50, emptyError), "empty buffer is refused instead of playing silence");
    check(!emptyError.empty(), "empty buffer refusal carries a reason");
}

// 真实素材：路径由命令行给出。游戏素材在 .mix 里，散装 WAV 只有 Taunts，
// 用它验证"外部真实文件也能解"，而不是只会在自造样本上通过。
void assetChecks(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        std::printf("NOTE no asset path given, skipping real-file check\n");
        return;
    }
    if (!std::filesystem::exists(path)) {
        std::printf("NOTE asset %s not found, skipping real-file check\n", path);
        return;
    }
    DecodedPcm pcm;
    std::string error;
    const bool ok = decodeWavFile(path, pcm, error);
    check(ok, "real game WAV decodes");
    if (!ok) {
        std::printf("NOTE real asset failure: %s\n", error.c_str());
        return;
    }
    // 实测 152/152 个散装素材都是 IMA ADPCM / 22050Hz / 单声道，
    // 这里把这条实测钉住：格式变了就该红，而不是悄悄退化。
    check(pcm.sampleRate == 22050, "real game WAV is 22050 Hz");
    check(pcm.channels == 1, "real game WAV is mono");
    check(pcm.frames() > 0, "real game WAV has frames");
    std::printf("NOTE asset %s: %u Hz, %u ch, %llu frames\n", path, pcm.sampleRate, pcm.channels,
                static_cast<unsigned long long>(pcm.frames()));
}

// 交叉核对用：把解码结果倒成裸 S16LE，交给外部解码器（afconvert）比。
// 自测里的硬编码断言只能证明"我按我理解的表算"，证明不了"和别人的解码一致"。
int dumpPcm(const char* in, const char* out) {
    DecodedPcm pcm;
    std::string error;
    if (!decodeWavFile(in, pcm, error)) {
        std::printf("dump failed: %s\n", error.c_str());
        return 1;
    }
    FILE* file = std::fopen(out, "wb");
    if (file == nullptr) {
        std::printf("dump failed: cannot write %s\n", out);
        return 1;
    }
    for (const auto sample : pcm.samples) putU16Out(file, static_cast<unsigned>(static_cast<std::uint16_t>(sample)));
    std::fclose(file);
    std::printf("%u %u %llu\n", pcm.sampleRate, pcm.channels, static_cast<unsigned long long>(pcm.frames()));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // 输出一旦被缓冲，崩溃时就只剩空白——遇到段错误时这行差别巨大。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 4 && std::string(argv[1]) == "--dump") return dumpPcm(argv[2], argv[3]);
    decodeChecks();
    playbackChecks();
    assetChecks(argc > 1 ? argv[1] : nullptr);
    std::printf("AudioOutput: checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
