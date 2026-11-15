#pragma once
// L10N-03: on-screen text layer (HiDPI).
//
// 为什么需要这一层：帧缓冲是 1x 逻辑分辨率（1024x640），在 Retina 屏上由
// CoreAnimation 整块放大到物理像素。对像素美术这是"像素风放大"，对文字却是
// 灾难——一个 15px 的汉字先被 50% 阈值切成 1-bit，再被整数放大，
// 每一笔都变成 2x2 的硬方块。工具提示「灰熊坦克」就是最明显的样本。
//
// 这里不提高整个帧缓冲的分辨率（那要重做全部 SHP 素材，代价不成比例），
// 只把**系统字体文字**从帧缓冲里摘出来，按 backing 倍数重新光栅到一张独立的
// 透明位图，由 Cocoa 呈现层作为单独的 CALayer 叠在画面上。像素画保持原样，
// 文字拿到真正的亚像素边缘。
//
// 捕获是"要么全有要么全无"的：一旦接管，drawTextRun 不再往 DIB 写文字像素，
// 否则两层会叠出重影。离屏路径（所有 smoke 的 BMP 取证）不经过 present，
// 因此永远走原来的 1x 合成，取证口径不变。

#include <cstdint>
#include <string>
#include <vector>

#include "RuntimeWin32CompatTextInternal.h"

namespace concord::win32compat::textlayer {

// 一条被截获的文字 run，字段是 drawTextRun 当时的全部上下文。
struct Run {
    std::wstring text;
    int x = 0;  // 1x surface 坐标（左上，已含 TextAlign 调整）
    int y = 0;
    int cellWidth = 11;
    int cellHeight = 16;
    text::FaceSpec face;
    std::uint32_t fg = 0xFFFFFFFFu;  // 0xAARRGGBB，来自 toPixel(COLORREF)
    std::uint32_t bg = 0xFF000000u;
    int bkMode = 1;   // TRANSPARENT=1 / OPAQUE=2
    int rop2 = 13;    // R2_COPYPEN
    bool hasClip = false;
    int clipLeft = 0;
    int clipTop = 0;
    int clipRight = 0;
    int clipBottom = 0;
};

// 是否由屏幕合成层接管文字像素。
bool captureEnabled();
void setCaptureEnabled(bool enabled);

// 每帧开头清空记录。记录发生在游戏绘制期间，重建发生在 present 时。
void beginFrame();
void record(const Run& run);
std::size_t runCount();

struct Bitmap {
    const void* pixels = nullptr;  // BGRA，预乘，top-down；无内容时为 nullptr
    int width = 0;
    int height = 0;
    int bytesPerRow = 0;
    unsigned generation = 0;  // 变了才需要重新贴到 layer
};

void setSurfaceSize(int width, int height);
// scale = 屏幕物理像素 / surface 逻辑像素。<= 1.01 视为不需要该层（关闭捕获）。
void setPixelScale(double scale);
double pixelScale();

// 惰性重建：记录与本帧不一致时重画，否则返回上一张。
Bitmap bitmap();

// 自证用：最近一次重建的统计。
struct Stats {
    unsigned rebuilds = 0;
    unsigned runs = 0;
    unsigned glyphs = 0;
    unsigned missing = 0;
    double scale = 0.0;
};
Stats stats();

}  // namespace concord::win32compat::textlayer
