#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

// Retail GameOptions::GameSpeed is a wait count, the reverse of the UI slider.
// 0x55D79E stores it in the frame timer; 0x6C8C40 reads timeGetTime() >> 4.
// These are 16 ms clock quanta, not exact 1/60-second intervals. The
// user-selected faster pacing runs finite settings at 1.5x this retail
// baseline; authored durations still use 15 frames per second.
//
// BUILD-03：最快档不再照抄 retail 的"零等待"哨兵。retail 的零等待意味着
// "一帧只跑一拍"，于是帧率＝机器能力——游戏速度被绑到机器性能上（老机器上
// 5/6 档糊成一片就是这个原因）。现在 6 档＝5 档的 1.5 倍等待（有限值）：
// 七档阶梯严格单调，任何一档都与机器快慢无关。nativeWaitQuanta 仍保留 retail
// 的原始量子数（6 档为 0），只作诊断用，不再影响调度。
struct RuntimeGameSpeedTiming {
    int nativeWaitQuanta = 0;
    double secondsPerTick = 0.0;

    // 只有"零墙钟配额"才算 unlimited；所有档位现在都有有限配额，故恒为 false。
    bool unlimited() const {
        return secondsPerTick <= 0.0;
    }

    double accumulate(double previous, double elapsed, int maximumTicks) const {
        if (unlimited()) return 0.0;
        return std::min(
            std::max(0.0, previous) + std::max(0.0, elapsed),
            secondsPerTick * std::max(0, maximumTicks)
        );
    }

    bool ready(double accumulated, int advanced, int maximumTicks) const {
        // Repeated subtraction must not lose the last tick of an exact
        // catch-up batch solely to a few binary floating-point ULPs. Account
        // for the larger local batch as well as the network's smaller budget.
        const double roundoff = secondsPerTick
            * (16.0 * std::numeric_limits<double>::epsilon()
                * std::max(1, maximumTicks));
        return advanced < maximumTicks
            && (unlimited() || accumulated + roundoff >= secondsPerTick);
    }

    double consume(double accumulated, int ticks = 1) const {
        return unlimited() ? 0.0 : std::max(
            0.0, accumulated - secondsPerTick * std::max(0, ticks)
        );
    }

    // Diagnostic only: the actual scheduler must use secondsPerTick so that
    // rounding, for example 93.75 to 94 Hz, cannot alter simulation speed.
    int roundedTickRate() const {
        if (unlimited()) return 0;
        return static_cast<int>(std::min(
            std::floor(1.0 / secondsPerTick + 0.5),
            static_cast<double>(std::numeric_limits<int>::max())
        ));
    }
};

inline constexpr double runtimeGameSpeedPacingMultiplier = 1.5;

inline RuntimeGameSpeedTiming runtimeGameSpeedTiming(int slider, int fixedTickHz) {
    const int clamped = std::clamp(slider, 0, 6);
    const int waitQuanta = 6 - clamped;
    // 0..5 档按 retail 的 N 个 16ms 量子；6 档把一个量子的等待再除以 1.5，
    // 于是它恰好是 5 档的 1.5 倍快，而不是"无等待"。
    const double effectiveQuanta = clamped >= 6
        ? 1.0 / runtimeGameSpeedPacingMultiplier
        : static_cast<double>(waitQuanta);
    return {
        waitQuanta,
        effectiveQuanta * 0.016 * 15.0
            / static_cast<double>(std::max(1, fixedTickHz))
            / runtimeGameSpeedPacingMultiplier
    };
}
