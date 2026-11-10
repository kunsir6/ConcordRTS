// P4-01b 自测：5 个音频接缝 / 7 个工厂在 macOS 上的会话语义。
//
// 这一层能钉住什么、不能钉住什么，先说清楚：
//   · 能钉：路径编码（含 CJK 与非 ASCII 文件名真能打开）、open 的失败面、
//     进程级别名语义（第二个实例顶掉第一个）、被顶掉的旧实例关不掉新会话、
//     PlaySound 的"进程级单路"语义、循环音乐的时间下界。
//   · 不能钉："听上去对不对"。这需要产品跑起来（P4-01c）与 Windows 金标准。
//
// 断言全部写死字面量或可从被测文件本身推出的量；不用"再从实现算一遍"的期望值。

#include "audio/backend/win32/RuntimeAsyncSoundDeviceWin32.h"
#include "audio/backend/win32/RuntimeLegacyWaveDeviceWin32.h"
#include "audio/events/win32/RuntimeMenuSoundWin32.h"
#include "audio/menu/win32/RuntimeCampaignMapAudioWin32.h"
#include "audio/menu/win32/RuntimeMainMenuMusicWin32.h"
#include "audio/menu/win32/RuntimeSkirmishMusicWin32.h"

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

void putU16(Bytes& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void putU32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void putTag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }

// 16 位单声道 PCM 正弦波：频率固定，时长由 frames 决定。
Bytes tone(unsigned rate, unsigned frames, unsigned hz) {
    Bytes data;
    data.reserve(static_cast<std::size_t>(frames) * 2);
    for (unsigned i = 0; i < frames; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * hz * static_cast<double>(i) / rate;
        const auto sample = static_cast<std::int16_t>(std::sin(phase) * 4000.0);
        putU16(data, static_cast<unsigned>(static_cast<std::uint16_t>(sample)));
    }
    return data;
}

Bytes makeWav(unsigned rate, const Bytes& data) {
    Bytes b;
    putTag(b, "RIFF");
    putU32(b, 0);
    putTag(b, "WAVE");
    putTag(b, "fmt ");
    putU32(b, 16);
    putU16(b, 1); // WAVE_FORMAT_PCM
    putU16(b, 1); // mono
    putU32(b, rate);
    putU32(b, rate * 2);
    putU16(b, 2);
    putU16(b, 16);
    putTag(b, "data");
    putU32(b, static_cast<std::uint32_t>(data.size()));
    b.insert(b.end(), data.begin(), data.end());
    // RIFF 长度 = 除前 8 字节之外的全部
    const std::uint32_t riffSize = static_cast<std::uint32_t>(b.size() - 8);
    for (int i = 0; i < 4; ++i) b[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((riffSize >> (8 * i)) & 0xFF);
    return b;
}

std::filesystem::path writeWav(const std::filesystem::path& path, unsigned rate, const Bytes& data) {
    const auto wav = makeWav(rate, data);
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) return {};
    const auto written = std::fwrite(wav.data(), 1, wav.size(), file);
    std::fclose(file);
    if (written != wav.size()) return {};
    return path;
}

std::wstring asciiEncoder(const std::string& token) { return std::wstring(token.begin(), token.end()); }

std::wstring pathEncoder(const std::filesystem::path& path) { return path.wstring(); }

} // namespace

int main() {
    // 崩溃时不留空白：段错误前最后一条输出就是现场。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace concord::audio;

    const auto root = std::filesystem::temp_directory_path() / "concord_audio_devices_smoke";
    std::error_code ignored;
    std::filesystem::create_directories(root, ignored);
    const auto shortPath = writeWav(root / "short.wav", 22050, tone(22050, 22050 * 3 / 10, 440));   // 300ms
    const auto longPath = writeWav(root / "long.wav", 22050, tone(22050, 22050 * 2, 330));          // 2s
    const auto cjkPath = writeWav(root / L"中文 音效.wav", 22050, tone(22050, 22050 / 4, 880));      // 250ms
    check(!shortPath.empty() && !longPath.empty() && !cjkPath.empty(), "smoke: wav fixtures written");
    const auto missingPath = root / "definitely-missing.wav";

    // ---------------------------------------------------------------- ① 异步音效
    {
        auto device = createAsyncSoundDeviceWin32();
        check(device != nullptr, "async: factory returns a device");
        check(device->supported(), "async: supported on a machine with an output device");
        check(device->playFile(shortPath.wstring()), "async: plays a real file");
        check(!device->playFile(missingPath.wstring()), "async: missing file is a failure, not silence");
        check(device->playFile(shortPath.wstring()), "async: replaces the previous sound");
        // PlaySound 语义是"进程级单路"：另一个实例的 stopGlobal 停的是同一路。
        auto other = createAsyncSoundDeviceWin32();
        check(other->stopGlobal(), "async: stopGlobal succeeds");
        check(device->playFile(cjkPath.wstring()), "async: UTF-8 path with CJK opens");
        check(other->stopGlobal(), "async: stop is visible process-wide");
        // 析构不许停别人：让 other 先死，device 仍然可用。
        other.reset();
        check(device->playFile(shortPath.wstring()), "async: instance destruction never stops a peer");
        check(device->stopGlobal(), "async: final stop");
    }

    // ---------------------------------------------------------------- ② 菜单音效
    {
        auto device = createMenuSoundDeviceWin32();
        check(device != nullptr, "menu: factory returns a device");
        check(device->supported(), "menu: supported");
        check(!device->open(0, missingPath.wstring()), "menu: open of a missing file fails");
        check(!device->playFromStart(0), "menu: play after a failed open fails");
        check(!device->isPlaying(0).has_value(), "menu: unopened channel is not queryable at all");
        check(device->open(0, shortPath.wstring()), "menu: open a real file");
        check(device->isPlaying(0).has_value(), "menu: opened channel is queryable");
        check(device->playFromStart(0), "menu: play from start");
        check(device->isPlaying(0).value(), "menu: reports playing right after start");
        check(!device->open(33, shortPath.wstring()), "menu: channel 33 is out of range");
        check(!device->isPlaying(32).has_value(), "menu: channel 32 is out of range");

        // 进程级别名：第二个实例打开同一通道，第一个立即失去可寻址性。
        auto second = createMenuSoundDeviceWin32();
        check(second->open(0, longPath.wstring()), "menu: second instance opens the same channel");
        check(!device->isPlaying(0).has_value(), "menu: superseded instance loses the alias");
        check(!device->playFromStart(0), "menu: superseded instance cannot start");
        // 被顶掉的旧实例连 close 都不许动新会话。
        device->close(0);
        check(second->isPlaying(0).has_value(), "menu: stale close never tears down the new session");
        check(second->playFromStart(0), "menu: new session still plays after a stale close");
        // 独立通道互不影响。
        check(second->open(7, shortPath.wstring()), "menu: another channel opens independently");
        check(second->playFromStart(7), "menu: another channel plays independently");
        check(second->isPlaying(7).value(), "menu: another channel reports playing");
        second->close(7);
        check(!second->isPlaying(7).has_value(), "menu: closed channel is no longer queryable");
        second->close(0);
        device.reset();
        second.reset();
    }

    // ---------------------------------------------------------------- ③ 遗留同步播放
    {
        auto eva = createLegacyWaveDeviceWin32(RuntimeLegacyWaveChannel::Eva, asciiEncoder);
        auto music = createLegacyWaveDeviceWin32(RuntimeLegacyWaveChannel::ShellMusic, asciiEncoder);
        check(eva != nullptr && music != nullptr, "legacy: both channels build");
        check(eva->supported() && music->supported(), "legacy: supported");
        check(eva->playFromStart(shortPath.string(), 50), "legacy: eva plays at 50%");
        check(eva->isPlaying().has_value(), "legacy: eva is queryable");
        eva->stopAndClose();
        check(!eva->isPlaying().has_value(), "legacy: eva stops and closes");
        check(music->playFromStart(longPath.string(), 100), "legacy: shell music plays");
        music->stopAndClose();
        // 两个通道是两条独立别名：一个关掉不波及另一个。
        check(eva->playFromStart(shortPath.string(), 100), "legacy: eva reopens after music closed");
        check(music->playFromStart(shortPath.string(), 100), "legacy: music independent of eva");
        eva->stopAndClose();
        music->stopAndClose();
        // 编码器为空的工厂必须给"不支持"，不能给一个会假装能播的设备。
        auto noEncoder = createLegacyWaveDeviceWin32(RuntimeLegacyWaveChannel::Eva, nullptr);
        check(noEncoder != nullptr && !noEncoder->supported(), "legacy: null encoder degrades honestly");
        check(!noEncoder->playFromStart(shortPath.string(), 100), "legacy: null encoder never plays");
    }

    // ---------------------------------------------------------------- ④ 主菜单音乐
    {
        auto device = createMainMenuMusicDeviceWin32();
        check(device != nullptr && device->supported(), "main: factory + supported");
        check(!device->open(missingPath), "main: open of a missing file fails");
        check(device->open(shortPath), "main: open a real file");
        device->setVolumePercent(50);
        check(device->playFromStart(), "main: play from start");
        check(device->isPlaying().value(), "main: reports playing");
        device->setVolumePercent(140); // 夹到 100，不许把增益推到 >1
        check(device->isPlaying().has_value(), "main: volume clamp keeps the session alive");
        // 300ms 素材的循环播放：2s 之后必须还在放（循环真的生效）。
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        check(device->isPlaying().value(), "main: loops instead of stopping after one pass");
        device->stop();
        check(!device->isPlaying().value(), "main: stop stops");
        device->close();
        check(!device->isPlaying().has_value(), "main: close makes it unqueryable");
        // 顶掉语义：第二个实例 open 之后，第一个不许再控制。
        auto first = createMainMenuMusicDeviceWin32();
        auto second = createMainMenuMusicDeviceWin32();
        check(first->open(shortPath) && second->open(longPath), "main: two instances open in turn");
        check(!first->isPlaying().has_value(), "main: superseded main instance loses the alias");
        first->stop();
        check(second->playFromStart() && second->isPlaying().value(), "main: new owner keeps playing");
        second->stop();
        second->close();
    }

    // ---------------------------------------------------------------- ⑤ 遭遇战音乐
    {
        auto device = createSkirmishMusicDeviceWin32(pathEncoder);
        check(device != nullptr && device->supported(), "skirmish: factory + supported");
        check(!device->open(missingPath), "skirmish: missing file fails");
        check(device->open(longPath), "skirmish: open a real file");
        check(device->playFromStart(), "skirmish: play from start");
        check(device->isPlaying().value(), "skirmish: reports playing");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        check(device->isPlaying().value(), "skirmish: loops past the first 400ms");
        device->stop();
        check(!device->isPlaying().value(), "skirmish: stop stops");
        device->close();
        auto noEncoder = createSkirmishMusicDeviceWin32(nullptr);
        check(noEncoder != nullptr && !noEncoder->supported(), "skirmish: null encoder degrades honestly");
    }

    // ---------------------------------------------------------------- ⑥ 战役地图音频
    {
        auto voice = createCampaignMapAudioDeviceWin32(CampaignMapAudioChannel::Voice);
        auto effect = createCampaignMapAudioDeviceWin32(CampaignMapAudioChannel::Effect);
        check(voice != nullptr && effect != nullptr, "campaign: both channels build");
        check(voice->supported() && effect->supported(), "campaign: supported");
        check(!voice->play(missingPath, 100), "campaign: missing file fails");
        check(voice->play(shortPath, 80), "campaign: voice plays");
        check(effect->play(cjkPath, 40), "campaign: effect plays with a CJK path");
        voice->stopAndClose();
        // 两条通道是两条别名：关掉语音不波及音效。
        check(effect->play(cjkPath, 40), "campaign: effect unaffected by voice close");
        effect->stopAndClose();
        // 同一条通道再次 play 就是"替换"，不是叠一层。
        check(voice->play(shortPath, 100), "campaign: replay replaces the previous voice");
        check(voice->play(longPath, 100), "campaign: replay replaces again");
        voice->stopAndClose();
    }

    // ---------------------------------------------------------------- ⑦ 诊断接缝
    {
        check(createAsyncSoundDeviceWin32ForDiagnostics(nullptr) != nullptr, "diag: async null hook yields a device");
        check(!createAsyncSoundDeviceWin32ForDiagnostics(nullptr)->supported(), "diag: async null hook is unsupported");
        check(!createMenuSoundDeviceWin32ForDiagnostics(nullptr)->supported(), "diag: menu null hook is unsupported");
        check(!createMainMenuMusicDeviceWin32ForDiagnostics(nullptr)->supported(), "diag: main null hook is unsupported");
        check(!createLegacyWaveDeviceWin32ForDiagnostics(RuntimeLegacyWaveChannel::Eva, asciiEncoder, nullptr)->supported(),
              "diag: legacy null hook is unsupported");
        check(!createCampaignMapAudioDeviceWin32ForDiagnostics(CampaignMapAudioChannel::Voice, nullptr)->supported(),
              "diag: campaign null hook is unsupported");
        // 非空钩子在 macOS 上没有 WinMM 可注入，工厂仍返回真实现（不是空壳）。
        const auto hooked = createMenuSoundDeviceWin32ForDiagnostics([](const wchar_t*, wchar_t*, std::uint32_t) {
            return 0U;
        });
        check(hooked != nullptr && hooked->supported(), "diag: non-null hook on Apple still builds a real device");
    }

    // ------------------------------------------------- ⑧ 跨族独立性（P4-01c 回归）
    // 异步音效 / 主菜单音乐 / 遭遇战音乐 都是"单槽"族，所以它们的槽位表必须彼此
    // 独立：播一声音效不许把音乐顶掉。这条断言的存在理由很具体——槽位表原先只按
    // 容量 N 做模板参数，table<1>() 对这三族是**同一个静态实例**，上面 76 条断言
    // 全绿却漏掉了这个行为；产品上是"滑入音效一响，菜单音乐立刻消失"。
    {
        auto music = createMainMenuMusicDeviceWin32();
        auto async = createAsyncSoundDeviceWin32();
        check(music->open(longPath) && music->playFromStart(), "cross: main music starts");
        check(music->isPlaying().value_or(false), "cross: main music is playing");
        check(async->playFile(shortPath.wstring()), "cross: async sfx plays while music runs");
        // 等待必须**长于** shortPath 的时长（300ms）：如果两族共用槽位，音乐那条
        // 槽会被换成这段 300ms 非循环素材，等它播完再问音乐是否在播，答案必然是
        // 否——这才是"被顶掉"的判据。只等 120ms 会看到音效自己在播，断言假绿。
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        check(music->isPlaying().value_or(false), "cross: async sfx does not disturb main music");
        // 遭遇战音乐与主菜单音乐也是两族：互相都不许顶掉对方。
        auto skirmish = createSkirmishMusicDeviceWin32(pathEncoder);
        check(skirmish->open(shortPath) && skirmish->playFromStart(), "cross: skirmish music starts");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        check(music->isPlaying().value_or(false), "cross: skirmish music does not disturb main music");
        check(skirmish->isPlaying().value_or(false), "cross: main music does not disturb skirmish");
        // 菜单音效族（32 通道）同样不许动音乐。
        auto menuSfx = createMenuSoundDeviceWin32();
        check(
            menuSfx->open(0, shortPath.wstring()) && menuSfx->playFromStart(0),
            "cross: menu sfx plays"
        );
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        check(music->isPlaying().value_or(false), "cross: menu sfx does not disturb main music");
        music->stop();
        music->close();
        skirmish->stop();
        skirmish->close();
        async->stopGlobal();
    }

    // 清理：自测产物留在 /tmp，不动仓库。
    std::filesystem::remove_all(root, ignored);

    std::printf("AudioDevices: checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
