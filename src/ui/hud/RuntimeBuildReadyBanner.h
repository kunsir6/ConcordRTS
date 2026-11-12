#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

// Local presentation only. The banner is a HUD affordance for "结构已经就绪，
// 点侧栏图标进入放置"；它不进存档、不进权威哈希、也不参与回放。
//
// 形状取自"挂在视口顶边的标签"：顶边与视口顶边齐平（左右上角是直角），
// 只有底部两个角做圆角。所以绘制时不能直接用 RoundRect 一次填满——那样
// 四个角都会被削掉。这里的做法是逐行填充：底边往上 radius 行按圆的内缩量
// 收窄，其余行整宽。
inline constexpr std::uint64_t runtimeBuildReadyBannerSlideInMs = 130;
inline constexpr std::uint64_t runtimeBuildReadyBannerHoldMs = 1900;
inline constexpr std::uint64_t runtimeBuildReadyBannerSlideOutMs = 180;
inline constexpr int runtimeBuildReadyBannerHeight = 36;
inline constexpr int runtimeBuildReadyBannerMinimumWidth = 104;
inline constexpr int runtimeBuildReadyBannerHorizontalPadding = 26;
inline constexpr int runtimeBuildReadyBannerCornerRadius = 12;

inline constexpr std::uint64_t runtimeBuildReadyBannerTotalMs() {
    return runtimeBuildReadyBannerSlideInMs
        + runtimeBuildReadyBannerHoldMs
        + runtimeBuildReadyBannerSlideOutMs;
}

struct RuntimeBuildReadyBannerState {
    bool active = false;
    std::uint64_t startedMs = 0;
    // 只用于诊断与后续扩展（例如在角标里写建筑名）；不是权威数据。
    std::string itemKey;

    void start(std::uint64_t nowMs, std::string key) {
        active = true;
        startedMs = nowMs;
        itemKey = std::move(key);
    }

    void clear() {
        active = false;
        startedMs = 0;
        itemKey.clear();
    }
};

// 1 = 完全落下（顶边与视口顶边齐平），0 = 完全藏到视口上方看不见。
inline double runtimeBuildReadyBannerReveal(
    std::uint64_t nowMs,
    const RuntimeBuildReadyBannerState& state
) {
    if (!state.active) return 0.0;
    if (nowMs < state.startedMs) return 1.0;
    const std::uint64_t elapsed = nowMs - state.startedMs;
    if (elapsed >= runtimeBuildReadyBannerTotalMs()) return 0.0;

    const auto easeOutCubic = [](double t) {
        const double clamped = std::clamp(t, 0.0, 1.0);
        const double inverse = 1.0 - clamped;
        return 1.0 - inverse * inverse * inverse;
    };
    if (elapsed < runtimeBuildReadyBannerSlideInMs) {
        return easeOutCubic(
            static_cast<double>(elapsed)
                / static_cast<double>(runtimeBuildReadyBannerSlideInMs)
        );
    }
    if (elapsed < runtimeBuildReadyBannerSlideInMs
        + runtimeBuildReadyBannerHoldMs) {
        return 1.0;
    }
    const std::uint64_t outElapsed = elapsed
        - runtimeBuildReadyBannerSlideInMs
        - runtimeBuildReadyBannerHoldMs;
    return 1.0 - easeOutCubic(
        static_cast<double>(outElapsed)
            / static_cast<double>(runtimeBuildReadyBannerSlideOutMs)
    );
}

struct RuntimeBuildReadyBannerBounds {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int radius = 0;
    bool valid = false;

    int width() const { return right - left; }
    int height() const { return bottom - top; }
};

// 视口宽度不足时按视口收窄；条目文字量很少，正常情况永远命中最小宽度。
inline int runtimeBuildReadyBannerWidth(int textWidth, int viewportWidth) {
    const int available = std::max(0, viewportWidth - 16);
    const int desired = std::max(
        runtimeBuildReadyBannerMinimumWidth,
        std::max(0, textWidth) + runtimeBuildReadyBannerHorizontalPadding * 2
    );
    return std::min(available, desired);
}

// 水平居中于视口；竖直方向按 reveal 从视口上方滑下。
inline RuntimeBuildReadyBannerBounds runtimeBuildReadyBannerBounds(
    int viewportLeft,
    int viewportTop,
    int viewportRight,
    int viewportBottom,
    int textWidth,
    double reveal
) {
    RuntimeBuildReadyBannerBounds bounds;
    const int viewportWidth = viewportRight - viewportLeft;
    const int viewportHeight = viewportBottom - viewportTop;
    const int width = runtimeBuildReadyBannerWidth(textWidth, viewportWidth);
    if (width < 48 || viewportHeight < runtimeBuildReadyBannerHeight + 8) {
        return bounds;
    }
    const int slide = static_cast<int>(std::lround(
        (1.0 - std::clamp(reveal, 0.0, 1.0))
            * static_cast<double>(runtimeBuildReadyBannerHeight)
    ));
    bounds.radius = std::min(
        runtimeBuildReadyBannerCornerRadius,
        std::min(runtimeBuildReadyBannerHeight / 2, width / 2 - 1)
    );
    bounds.left = viewportLeft + (viewportWidth - width) / 2;
    bounds.right = bounds.left + width;
    bounds.top = viewportTop - slide;
    bounds.bottom = bounds.top + runtimeBuildReadyBannerHeight;
    bounds.valid = true;
    return bounds;
}

// 距底边 rowsFromBottom 行（0 = 最底一行）需要左右各收窄多少像素。
// 圆心在 (left+radius, bottom-radius)，所以 offset = radius - sqrt(r^2-(r-dy)^2)。
inline int runtimeBuildReadyBannerRowInset(int rowsFromBottom, int radius) {
    if (radius <= 0 || rowsFromBottom < 0 || rowsFromBottom >= radius) {
        return 0;
    }
    const double depth = static_cast<double>(rowsFromBottom) + 0.5;
    const double centreOffset = static_cast<double>(radius) - depth;
    const double squared = static_cast<double>(radius * radius)
        - centreOffset * centreOffset;
    if (squared <= 0.0) return radius;
    return radius - static_cast<int>(std::floor(std::sqrt(squared)));
}
