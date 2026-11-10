// P4-01b：把 P4-01a 的音频底座（解码 + AudioQueue 输出流）接到产品的 5 个音频接缝上，
// 兑现 7 个 create*Win32 工厂符号。Windows 侧一行都不改：本文件只进 Apple 构建图，
// 而它 include 的是 Windows 侧那 5 个**纯接口头**（不含 windows.h），所以签名由编译器
// 逐字校验——符号能对上不是靠手抄声明。
//
// ---------------------------------------------------------------------------
// 三个必须先定的事（P4-01b 的 next_action 里点名的）
// ---------------------------------------------------------------------------
// ① 路径编码：入参是 std::wstring / std::string / std::filesystem::path。
//    macOS 的 wchar_t 是 4 字节（T-02 同源），所以"wstring 里存的是 UTF-32 码点"是
//    常态；但项目里也有把 UTF-16 代理对塞进 wstring 的写法。utf8FromWide() 两种都收：
//    BOM 之外的 D800..DFFF 一律当代理对合并，其余当独立码点。**绝不**用
//    wcstombs / std::wstring_convert：前者按 locale 编 UTF-8（此机 LC_ALL=C 时会编成
//    "?"），后者在 C++17 后已废弃。编不出来就返回空串让调用方失败——路径编错最坏的
//    结果是"打开了一个同名的其他文件"，而不是报错。
//
// ② 会话语义：MCI 的 alias 是**进程级命名**的，所以两个设备实例其实共用同一个槽。
//    Windows 侧用 lease（weak_ptr 比较）保证"被替换掉的旧实例不许关掉新实例"。
//    这里照搬，而且槽位本身也做成进程级静态表——如果每个实例各持一份输出流，那么
//    "第二个实例 open 同一个通道"会变成两路同时出声，那是 MCI 永远不会有的行为。
//
// ③ 全局语义：AsyncSoundDevice 对齐 PlaySound(SND_ASYNC)，是**进程级单路**播放：
//    同一时刻只有一个声音，后一个替换前一个，任何实例的析构都不许停别人。所以它
//    共用一张只有 1 个槽的表，而不是每实例一路。
//
// ---------------------------------------------------------------------------
// 与 Windows 侧的**已知差异**（记录在案，不是遗漏）
// ---------------------------------------------------------------------------
//  · open 与 play 在 MCI 里是两条命令，中间状态是"已打开未播放"。这里用
//    "open = 解码进槽并缓存 PCM，playFromStart = 起 AudioQueue"来对应，所以 open
//    的失败面（文件不存在 / 格式不认识 / 截断）与 MCI open 一致，都在 open 就报。
//  · MCI 的 setaudio volume 是 0..1000（percent*10）；这里直接吃 0..100，由
//    AudioOutput 换成 AudioQueue 的线性增益。**不是** dB，也不是 MCI 的 1/1000。
//  · MCI 的 status mode 有 playing/paused/stopped 三态；本后端没有 pause 通道，
//    只会回 playing / 非 playing 两态（isPlaying 的 optional 只承载"查询失败"）。
//  · 录制型/无设备环境下 AudioOutput::create() 失败 → supported() 返回 false，
//    工厂照常返回设备对象，但 open/play 一律返回 false（不假装成功）。

#include "audio/backend/win32/RuntimeAsyncSoundDeviceWin32.h"
#include "audio/backend/win32/RuntimeLegacyWaveDeviceWin32.h"
#include "audio/events/win32/RuntimeMenuSoundWin32.h"
#include "audio/menu/win32/RuntimeCampaignMapAudioWin32.h"
#include "audio/menu/win32/RuntimeMainMenuMusicWin32.h"
#include "audio/menu/win32/RuntimeSkirmishMusicWin32.h"

#include "audio/apple/RuntimeAppleAudioOutput.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace concord::audio {
namespace {

// ---------------------------------------------------------------------------
// 路径编码
// ---------------------------------------------------------------------------
void appendUtf8(std::string& out, std::uint32_t code) {
    if (code <= 0x7F) {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        // 超出 Unicode 范围：用 U+FFFD 而不是丢弃，这样"路径里有坏码位"会变成
        // 一个必然打不开的文件名，而不是悄悄变成另一个能打开的文件名。
        appendUtf8(out, 0xFFFD);
    }
}

bool isHighSurrogate(std::uint32_t value) { return value >= 0xD800 && value <= 0xDBFF; }
bool isLowSurrogate(std::uint32_t value) { return value >= 0xDC00 && value <= 0xDFFF; }

std::string utf8FromWide(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t code = static_cast<std::uint32_t>(text[i]);
        // 4 字节 wchar_t 里也可能塞了 UTF-16 代理对（T-02 的现场）：合并它。
        if (isHighSurrogate(code) && i + 1 < text.size()) {
            const std::uint32_t low = static_cast<std::uint32_t>(text[i + 1]);
            if (isLowSurrogate(low)) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            } else {
                code = 0xFFFD;
            }
        } else if (isLowSurrogate(code)) {
            code = 0xFFFD; // 孤立低位代理，没有可合并的前项
        }
        appendUtf8(out, code);
    }
    return out;
}

// 相对路径在 Windows 侧是按工作目录解析的，这里同样交给系统去解析（不自己拼）。
std::string utf8Path(const std::wstring& nativePath) { return utf8FromWide(nativePath); }

// ---------------------------------------------------------------------------
// P4-01c：产品级"真的出声"取证
// ---------------------------------------------------------------------------
// 默认完全关闭（不读时钟、不打印、不改行为）。CONCORD_AUDIO_TRACE=1 时，装载/起播/
// 停止各打一行到 stderr。为什么需要它：play() 返回 true 只证明"缓冲交给了
// AudioQueue"，无法区分"CoreAudio 真的按时钟在消费"与"交出去之后什么都没发生"。
// 所以起播时记时刻，停止时取设备自报的已消费帧数折算秒数，与墙钟对账
// （rules.play_must_be_timed 的同一判据，只是这次跑在产品路径上而不是自测里）。
bool audioTraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONCORD_AUDIO_TRACE");
        return value != nullptr && *value != '\0' && std::string(value) != "0";
    }();
    return enabled;
}

std::uint64_t audioNowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void traceAudioLine(const char* family, const char* action, const std::string& detail) {
    if (!audioTraceEnabled()) return;
    std::fprintf(stderr, "[audio] %s %s %s\n", family, action, detail.c_str());
}

// ---------------------------------------------------------------------------
// 槽位（对齐 MCI 的进程级别名）
// ---------------------------------------------------------------------------
// 一个槽 = 一个进程级别名 = 至多一路播放。owner 是 lease：只有当前 owner 才许
// 停/关/替换，被顶掉的旧实例连"关掉自己那条"都做不到（它已经不拥有这条槽了）。
struct AudioLease {};

struct AudioSlot {
    std::shared_ptr<AudioLease> owner;
    std::unique_ptr<apple::AudioOutput> output;
    apple::DecodedPcm pcm{};
    bool loaded = false;   // pcm 是否已解码可用
    int volumePercent = 100;
};

// 设备已消费帧数 → 折算秒数（保留三位）。无输出流时按 0 处理。
std::string formatPlayedSeconds(const AudioSlot& slot) {
    if (!slot.output || slot.pcm.sampleRate == 0) return "0.000";
    const double seconds = static_cast<double>(slot.output->framesConsumed())
        / static_cast<double>(slot.pcm.sampleRate);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", seconds);
    return buffer;
}

// 设备是否可用：AudioQueue 探测一次就够。每次都探测会为 supported() 付一次
// AudioQueueNewOutput 的代价（而 supported() 会被绘制路径频繁调用）。
bool audioAvailable() {
    static const bool available = [] {
        std::string error;
        const auto probe = apple::AudioOutput::create(error);
        return probe != nullptr;
    }();
    return available;
}

// 一个槽位表 = 一个家族。所有实例共享同一张表；表本身强持有（与 Windows 侧
// "static const auto slot = std::make_shared<...>()" 同构，避免静态析构顺序问题）。
//
// P4-01c 修正：表原先只按容量 N 做模板参数，于是 table<1>() 对 AsyncSound /
// MainMenuMusic / SkirmishMusic 是**同一个静态实例**——播一声菜单音效就会把菜单
// 音乐从槽里顶掉。实测表现：滑入音效起播（0.59s）之后 drok.wav 立刻消失，观测
// 线程的 consumed 从此恒为 12981。Windows 侧这三个族本就是彼此独立的 MCI 设备，
// 所以表必须按族分开，族用空的 tag 类型区分（容量相同的族也不会再撞车）。
struct AsyncSoundFamily {};
struct MenuSoundFamily {};
struct LegacyWaveFamily {};
struct MainMenuMusicFamily {};
struct SkirmishMusicFamily {};
struct CampaignMapAudioFamily {};

template <typename Family, std::size_t N>
struct SlotTable {
    std::mutex mutex;
    std::array<AudioSlot, N> slots{};
};

template <typename Family, std::size_t N>
std::shared_ptr<SlotTable<Family, N>> table() {
    static const auto instance = std::make_shared<SlotTable<Family, N>>();
    return instance;
}

bool ownsSlot(const AudioSlot& slot, const std::shared_ptr<AudioLease>& lease) {
    const auto owner = slot.owner;
    return !owner || owner == lease;
}

// 把"已解码的 PCM 交给设备"。替换式：AudioOutput::play() 自带停旧起新。
bool startSlot(AudioSlot& slot, bool loop) {
    if (!slot.loaded || slot.pcm.frames() == 0) return false;
    std::string error;
    if (!slot.output) {
        slot.output = apple::AudioOutput::create(error);
        if (!slot.output) return false;
    }
    return slot.output->play(slot.pcm, loop, slot.volumePercent, error);
}

// 各家族共同的关闭动作。
void stopSlot(AudioSlot& slot) {
    if (slot.output) slot.output->stop();
}

void resetSlot(AudioSlot& slot, const std::shared_ptr<AudioLease>& lease) {
    if (slot.owner && slot.owner != lease) return; // 不是自己的槽，别动
    if (slot.output) slot.output->stop();
    slot.output.reset();
    slot.pcm = apple::DecodedPcm{};
    slot.loaded = false;
    if (slot.owner == lease) slot.owner.reset();
}

// 解码并装入槽（open 的实质）。失败时槽保持"未装载"，与 MCI open 失败一致。
bool loadSlot(AudioSlot& slot, const std::string& utf8FilePath) {
    apple::DecodedPcm decoded;
    std::string error;
    if (!apple::decodeWavFile(utf8FilePath, decoded, error)) {
        slot.pcm = apple::DecodedPcm{};
        slot.loaded = false;
        return false;
    }
    if (decoded.frames() == 0) { // 解出来是空：不可能"成功播放一个空文件"
        slot.pcm = apple::DecodedPcm{};
        slot.loaded = false;
        return false;
    }
    slot.pcm = std::move(decoded);
    slot.loaded = true;
    return true;
}

// ---------------------------------------------------------------------------
// ① 进程级异步音效（PlaySound 语义）
// ---------------------------------------------------------------------------
class AsyncSoundApple final : public RuntimeAsyncSoundDevice {
public:
    AsyncSoundApple() : table_(table<AsyncSoundFamily, 1>()) {}
    bool supported() const noexcept override { return audioAvailable(); }
    bool playFile(const std::wstring& nativeFilename) override {
        if (!supported()) return false;
        const auto path = utf8Path(nativeFilename);
        if (path.empty()) return false;
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        // PlaySound(SND_FILENAME|SND_ASYNC) 的语义：换一个声音就顶掉上一个，
        // 而且不需要（也不许）先 stop——直接替换。
        slot.volumePercent = 100;
        slot.owner.reset(); // 异步音效没有租约：谁都可以替换它
        if (!loadSlot(slot, path)) {
            traceAudioLine("async", "open-failed", path);
            return false;
        }
        const bool started = startSlot(slot, false);
        traceAudioLine(
            "async",
            started ? "play" : "play-failed",
            path + " frames=" + std::to_string(slot.pcm.frames())
                + " rate=" + std::to_string(slot.pcm.sampleRate)
                + " ch=" + std::to_string(slot.pcm.channels)
        );
        return started;
    }
    bool stopGlobal() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        stopSlot(slot);
        slot.output.reset();
        slot.pcm = apple::DecodedPcm{};
        slot.loaded = false;
        return true;
    }

private:
    std::shared_ptr<SlotTable<AsyncSoundFamily, 1>> table_;
};

// ---------------------------------------------------------------------------
// ② 菜单音效（32 个独立通道）
// ---------------------------------------------------------------------------
constexpr std::size_t kMenuChannels = 32;

class MenuSoundApple final : public MenuSoundDevice {
public:
    MenuSoundApple() : table_(table<MenuSoundFamily, kMenuChannels>()) {}
    ~MenuSoundApple() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        for (std::size_t channel = 0; channel < kMenuChannels; ++channel) {
            auto& slot = table_->slots[channel];
            if (leases_[channel] && slot.owner == leases_[channel]) resetSlot(slot, leases_[channel]);
        }
    }
    bool supported() const noexcept override { return audioAvailable(); }
    std::optional<bool> isPlaying(std::size_t channel) const override {
        if (channel >= kMenuChannels) return std::nullopt;
        std::lock_guard<std::mutex> guard(table_->mutex);
        const auto& slot = table_->slots[channel];
        if (!ownsSlot(slot, leases_[channel])) return std::nullopt;
        // MCI 里"别名不存在"时 status 命令报错 → nullopt。这里同样：没装载过就没有
        // 可查询的会话，不能回 false（false 的含义是"在放但没声音"）。
        if (!slot.loaded) return std::nullopt;
        return slot.output && slot.output->isPlaying();
    }
    bool open(std::size_t channel, const std::wstring& nativePath) override {
        if (channel >= kMenuChannels || !supported()) return false;
        const auto path = utf8Path(nativePath);
        if (path.empty()) return false;
        const auto lease = std::make_shared<AudioLease>();
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[channel];
        // open 是"替换这条通道"：旧会话（不管是哪个实例的）先停掉再装新的。
        // MCI 的 open 对已存在的别名不报错、直接顶掉，这里等价。
        if (slot.output) slot.output->stop();
        slot.output.reset();
        slot.owner.reset();
        const bool opened = loadSlot(slot, path);
        slot.owner = lease;
        leases_[channel] = lease;
        return opened;
    }
    bool playFromStart(std::size_t channel) override {
        if (channel >= kMenuChannels) return false;
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[channel];
        if (!ownsSlot(slot, leases_[channel])) return false;
        if (!slot.loaded) return false;
        slot.volumePercent = 100;
        return startSlot(slot, false);
    }
    void close(std::size_t channel) override {
        if (channel >= kMenuChannels) return;
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[channel];
        if (!ownsSlot(slot, leases_[channel])) return; // 被顶掉的旧实例关不掉新会话
        resetSlot(slot, leases_[channel]);
        leases_[channel].reset();
    }

private:
    std::shared_ptr<SlotTable<MenuSoundFamily, kMenuChannels>> table_;
    std::array<std::shared_ptr<AudioLease>, kMenuChannels> leases_{};
};

// ---------------------------------------------------------------------------
// ③ 遗留同步播放（EVA / 外壳音乐两条通道，入参是 legacy token + 编码器）
// ---------------------------------------------------------------------------
constexpr std::size_t kLegacyChannels = 2;

std::size_t legacyIndex(RuntimeLegacyWaveChannel channel) {
    return channel == RuntimeLegacyWaveChannel::Eva ? 0 : 1;
}

class LegacyWaveApple final : public RuntimeLegacyWaveDevice {
public:
    LegacyWaveApple(RuntimeLegacyWaveChannel channel, LegacyWavePathEncoder encode)
        : channel_(channel), encode_(encode), table_(table<LegacyWaveFamily, kLegacyChannels>()),
          index_(legacyIndex(channel)) {}
    ~LegacyWaveApple() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[index_];
        if (lease_ && slot.owner == lease_) resetSlot(slot, lease_);
    }
    bool supported() const noexcept override { return audioAvailable(); }
    bool playFromStart(const std::string& path, int volumePercent) override {
        if (!supported() || !encode_) return false;
        // 保留两条通道原有的先后顺序：Eva 先停后编码，外壳音乐先编码后停。
        // 顺序在 MCI 下影响"编码失败时旧会话是否已被停掉"，这里逐字保留，
        // 免得将来把编码器换成会失败的实现时行为悄悄变了。
        if (channel_ == RuntimeLegacyWaveChannel::Eva) forceRelease();
        const auto widePath = encode_(path);
        if (channel_ == RuntimeLegacyWaveChannel::ShellMusic) forceRelease();
        const auto utf8 = utf8Path(widePath);
        if (utf8.empty()) return false;
        const auto lease = std::make_shared<AudioLease>();
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[index_];
        const bool loaded = loadSlot(slot, utf8);
        slot.owner = lease;
        lease_ = lease;
        if (!loaded) return false;
        slot.volumePercent = std::clamp(volumePercent, 0, 100);
        return startSlot(slot, false);
    }
    std::optional<bool> isPlaying() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        const auto& slot = table_->slots[index_];
        if (!ownsSlot(slot, lease_)) return std::nullopt;
        if (!slot.loaded) return std::nullopt;
        return slot.output && slot.output->isPlaying();
    }
    void stopAndClose() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[index_];
        if (!ownsSlot(slot, lease_)) { lease_.reset(); return; }
        resetSlot(slot, lease_);
        lease_.reset();
    }
    void close() override { stopAndClose(); }

private:
    void forceRelease() {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[index_];
        // 强制释放的语义是"不管谁拥有，这条别名先关掉"——MCI 的 close 命令正是如此。
        if (slot.output) slot.output->stop();
        slot.output.reset();
        slot.pcm = apple::DecodedPcm{};
        slot.loaded = false;
        slot.owner.reset();
        lease_.reset();
    }
    RuntimeLegacyWaveChannel channel_;
    LegacyWavePathEncoder encode_;
    std::shared_ptr<SlotTable<LegacyWaveFamily, kLegacyChannels>> table_;
    std::size_t index_;
    std::shared_ptr<AudioLease> lease_;
};

// ---------------------------------------------------------------------------
// ④ 主菜单音乐（单通道 + 独立音量命令）
// ---------------------------------------------------------------------------
class MainMenuMusicApple final : public MainMenuMusicDevice {
public:
    MainMenuMusicApple() : table_(table<MainMenuMusicFamily, 1>()) {}
    ~MainMenuMusicApple() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        if (owns_ && slot.owner == lease_) resetSlot(slot, lease_);
    }
    bool supported() const noexcept override { return audioAvailable(); }
    bool open(const std::filesystem::path& path) override {
        if (!supported()) return false;
        const auto utf8 = utf8Path(path.wstring());
        if (utf8.empty()) return false;
        const auto lease = std::make_shared<AudioLease>();
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        // 原始 Main 的顺序是 preclose → open：先无条件停掉上一条，再装新的。
        if (slot.output) slot.output->stop();
        slot.output.reset();
        slot.owner.reset();
        const bool loaded = loadSlot(slot, utf8);
        slot.owner = lease;
        lease_ = lease;
        owns_ = true;
        slot.volumePercent = volume_;
        traceAudioLine(
            "main-menu-music",
            loaded ? "open" : "open-failed",
            utf8 + " frames=" + std::to_string(slot.pcm.frames())
                + " rate=" + std::to_string(slot.pcm.sampleRate)
                + " ch=" + std::to_string(slot.pcm.channels)
        );
        return loaded;
    }
    void setVolumePercent(int percent) override {
        volume_ = std::clamp(percent, 0, 100);
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        if (!ownsSlot(slot, lease_)) return;
        // 原始命令的失败被刻意忽略；这里同样不因为音量设不上而报失败。
        slot.volumePercent = volume_;
        if (slot.loaded && slot.output && slot.output->isPlaying()) {
            // 正在放的时候改音量：AudioQueue 没有运行期改增益的接缝，重新起一遍
            // （从 0 开始）会明显"跳回开头"，所以只在下次 play 生效。
            return;
        }
    }
    bool playFromStart() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        if (!ownsSlot(slot, lease_)) return false;
        if (!slot.loaded) return false;
        slot.volumePercent = volume_;
        const bool started = startSlot(slot, true); // 菜单音乐是循环播放
        if (started) playStartedMs_ = audioNowMs();
        traceAudioLine(
            "main-menu-music",
            started ? "play" : "play-failed",
            "loop=1 vol=" + std::to_string(volume_)
                + " frames=" + std::to_string(slot.pcm.frames())
                + " rate=" + std::to_string(slot.pcm.sampleRate)
        );
        return started;
    }
    std::optional<bool> isPlaying() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        const auto& slot = table_->slots[0];
        if (!ownsSlot(slot, lease_)) return std::nullopt;
        if (!slot.loaded) return std::nullopt;
        if (audioTraceEnabled() && playStartedMs_ != 0) {
            // 菜单循环每秒调一次这里（ensureMusicPlaying 的探针间隔），正好当
            // "设备到底在不在按时钟消费"的采样点：playedSeconds 与 wallMs 同步
            // 增长、比值≈1，才能证明是 CoreAudio 真的在出声，而不是缓冲交出去
            // 之后躺在那里没人取。
            const std::uint64_t now = audioNowMs();
            if (now >= lastProbeMs_ + 1000ULL) {
                lastProbeMs_ = now;
                traceAudioLine(
                    "main-menu-music",
                    "probe",
                    "playedSeconds=" + formatPlayedSeconds(slot)
                        + " wallMs=" + std::to_string(now - playStartedMs_)
                        + " isPlaying="
                        + (slot.output && slot.output->isPlaying() ? "1" : "0")
                );
            }
        }
        return slot.output && slot.output->isPlaying();
    }
    void stop() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        if (ownsSlot(slot, lease_)) stopSlot(slot);
        if (audioTraceEnabled() && playStartedMs_ != 0) {
            traceAudioLine(
                "main-menu-music",
                "stop",
                "playedSeconds=" + formatPlayedSeconds(slot)
                    + " wallMs=" + std::to_string(audioNowMs() - playStartedMs_)
            );
            playStartedMs_ = 0;
        }
    }
    void close() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        if (!ownsSlot(slot, lease_)) { owns_ = false; lease_.reset(); return; }
        resetSlot(slot, lease_);
        owns_ = false;
        lease_.reset();
    }

private:
    std::shared_ptr<SlotTable<MainMenuMusicFamily, 1>> table_;
    std::shared_ptr<AudioLease> lease_;
    bool owns_ = false;
    int volume_ = 100;
    std::uint64_t playStartedMs_ = 0;
    std::uint64_t lastProbeMs_ = 0;
};

// ---------------------------------------------------------------------------
// ⑤ 遭遇战音乐（单通道，路径经编码器）
// ---------------------------------------------------------------------------
class SkirmishMusicApple final : public MenuMusicDevice {
public:
    explicit SkirmishMusicApple(MenuMusicPathEncoder encode)
        : encode_(encode), table_(table<SkirmishMusicFamily, 1>()) {}
    ~SkirmishMusicApple() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        if (owns_ && slot.owner == lease_) resetSlot(slot, lease_);
    }
    bool supported() const noexcept override { return audioAvailable(); }
    bool open(const std::filesystem::path& path) override {
        if (!supported() || !encode_) return false;
        const auto utf8 = utf8Path(encode_(path));
        if (utf8.empty()) return false;
        const auto lease = std::make_shared<AudioLease>();
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        // 原始 Skirmish 是 preclose → open，用的还是硬编码命令串；语义等同替换。
        if (slot.output) slot.output->stop();
        slot.output.reset();
        slot.owner.reset();
        const bool loaded = loadSlot(slot, utf8);
        slot.owner = lease;
        lease_ = lease;
        owns_ = loaded; // 与 Windows 侧一致：只有真的建出会话才持有租约
        slot.volumePercent = 100;
        return loaded;
    }
    bool playFromStart() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        if (!ownsSlot(slot, lease_)) return false;
        if (!slot.loaded) return false;
        return startSlot(slot, true);
    }
    std::optional<bool> isPlaying() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        const auto& slot = table_->slots[0];
        if (!ownsSlot(slot, lease_)) return std::nullopt;
        if (!slot.loaded) return std::nullopt;
        return slot.output && slot.output->isPlaying();
    }
    void stop() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        if (ownsSlot(slot, lease_)) stopSlot(slot);
    }
    void close() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[0];
        if (!ownsSlot(slot, lease_)) { owns_ = false; lease_.reset(); return; }
        resetSlot(slot, lease_);
        owns_ = false;
        lease_.reset();
    }

private:
    MenuMusicPathEncoder encode_;
    std::shared_ptr<SlotTable<SkirmishMusicFamily, 1>> table_;
    std::shared_ptr<AudioLease> lease_;
    bool owns_ = false;
};

// ---------------------------------------------------------------------------
// ⑥ 战役地图音频（语音 / 音效两条通道）
// ---------------------------------------------------------------------------
constexpr std::size_t kCampaignChannels = 2;

std::size_t campaignIndex(CampaignMapAudioChannel channel) {
    return channel == CampaignMapAudioChannel::Voice ? 0 : 1;
}

class CampaignMapAudioApple final : public CampaignMapAudioDevice {
public:
    explicit CampaignMapAudioApple(CampaignMapAudioChannel channel)
        : table_(table<CampaignMapAudioFamily, kCampaignChannels>()), index_(campaignIndex(channel)) {}
    ~CampaignMapAudioApple() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[index_];
        if (owns_ && slot.owner == lease_) resetSlot(slot, lease_);
    }
    bool supported() const noexcept override { return audioAvailable(); }
    bool play(const std::filesystem::path& path, int volumePercent) override {
        if (!supported()) return false;
        const auto utf8 = utf8Path(path.wstring());
        if (utf8.empty()) return false;
        const auto lease = std::make_shared<AudioLease>();
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[index_];
        // 原始 Campaign：先无条件 stop+close，再 open（不带 shareable），
        // 然后 setaudio，最后 play（不带 from 0）。失败即返回，且失败后不留会话。
        if (slot.output) slot.output->stop();
        slot.output.reset();
        slot.owner.reset();
        if (!loadSlot(slot, utf8)) {
            owns_ = false;
            lease_.reset();
            return false;
        }
        slot.owner = lease;
        lease_ = lease;
        owns_ = true;
        slot.volumePercent = std::clamp(volumePercent, 0, 100);
        if (!startSlot(slot, false)) {
            resetSlot(slot, lease_);
            owns_ = false;
            lease_.reset();
            return false;
        }
        return true;
    }
    void stopAndClose() override {
        std::lock_guard<std::mutex> guard(table_->mutex);
        auto& slot = table_->slots[index_];
        if (!ownsSlot(slot, lease_)) { owns_ = false; lease_.reset(); return; }
        resetSlot(slot, lease_);
        owns_ = false;
        lease_.reset();
    }

private:
    std::shared_ptr<SlotTable<CampaignMapAudioFamily, kCampaignChannels>> table_;
    std::size_t index_;
    std::shared_ptr<AudioLease> lease_;
    bool owns_ = false;
};

} // namespace

// ---------------------------------------------------------------------------
// 工厂（7 个链接缺口符号）
// ---------------------------------------------------------------------------
std::unique_ptr<RuntimeAsyncSoundDevice> createAsyncSoundDeviceWin32() {
    return std::make_unique<AsyncSoundApple>();
}
// 诊断接缝在 Windows 侧注入的是 WinMM 命令串发送器，macOS 上没有对应物：
// 保留符号（避免将来某个 Apple 目标引用它时又变成缺口），但不提供注入能力，
// 传进任何非空调试钩子都不改变行为——它只能是空实现之上的"真实现"。
std::unique_ptr<RuntimeAsyncSoundDevice> createAsyncSoundDeviceWin32ForDiagnostics(AsyncSoundNativeCall call) {
    if (!call) return makeUnsupportedAsyncSoundDevice();
    return std::make_unique<AsyncSoundApple>();
}

std::unique_ptr<RuntimeLegacyWaveDevice> createLegacyWaveDeviceWin32(
    RuntimeLegacyWaveChannel channel, LegacyWavePathEncoder encode) {
    if (!encode || (channel != RuntimeLegacyWaveChannel::Eva && channel != RuntimeLegacyWaveChannel::ShellMusic))
        return makeUnsupportedLegacyWaveDevice();
    return std::make_unique<LegacyWaveApple>(channel, encode);
}
std::unique_ptr<RuntimeLegacyWaveDevice> createLegacyWaveDeviceWin32ForDiagnostics(
    RuntimeLegacyWaveChannel channel, LegacyWavePathEncoder encode, LegacyWaveNativeCommand send) {
    if (!send) return makeUnsupportedLegacyWaveDevice();
    return createLegacyWaveDeviceWin32(channel, encode);
}

std::unique_ptr<MenuSoundDevice> createMenuSoundDeviceWin32() { return std::make_unique<MenuSoundApple>(); }
std::unique_ptr<MenuSoundDevice> createMenuSoundDeviceWin32ForDiagnostics(MenuSoundNativeCommand send) {
    if (!send) return makeUnsupportedMenuSoundDevice();
    return std::make_unique<MenuSoundApple>();
}

std::unique_ptr<MainMenuMusicDevice> createMainMenuMusicDeviceWin32() {
    return std::make_unique<MainMenuMusicApple>();
}
std::unique_ptr<MainMenuMusicDevice> createMainMenuMusicDeviceWin32ForDiagnostics(MainMusicNativeCommand send) {
    if (!send) return makeUnsupportedMainMenuMusicDevice();
    return std::make_unique<MainMenuMusicApple>();
}

std::unique_ptr<MenuMusicDevice> createSkirmishMusicDeviceWin32(MenuMusicPathEncoder encode) {
    if (!encode) return makeUnsupportedMenuMusicDevice();
    return std::make_unique<SkirmishMusicApple>(encode);
}

std::unique_ptr<CampaignMapAudioDevice> createCampaignMapAudioDeviceWin32(CampaignMapAudioChannel channel) {
    if (channel != CampaignMapAudioChannel::Voice && channel != CampaignMapAudioChannel::Effect)
        return makeUnsupportedCampaignMapAudioDevice();
    return std::make_unique<CampaignMapAudioApple>(channel);
}
std::unique_ptr<CampaignMapAudioDevice> createCampaignMapAudioDeviceWin32ForDiagnostics(
    CampaignMapAudioChannel channel, CampaignMapAudioNativeCommand send) {
    if (!send) return makeUnsupportedCampaignMapAudioDevice();
    return createCampaignMapAudioDeviceWin32(channel);
}

} // namespace concord::audio
