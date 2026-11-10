#pragma once
// ============================================================================
// compat/win32/src/RuntimeWin32CompatAudioInternal.h
//
// P4-02：XAudio2 引擎的**自测接缝**。只有两个用途：
//
//   1. createOfflineEngine()：造一个没有输出设备的引擎。真实 XAudio2 没有这个
//      模式，但混音数学必须能在没有声卡的机器上逐样本对账——否则"多条 voice 混
//      起来对不对"这件事就只能靠人耳，而人耳听不出 4 个 LSB 的偏差。
//      产品路径只有在 XAudio2Create() 一个入口，它一定建真设备。
//
//   2. renderInterleaved()：设备回调内部用的那段渲染。自测调用它，拿到的就是
//      硬件路径**逐字节同一条代码**的输出，而不是"另写一份参考实现来对照自己"
//      （那种自洽比较能证明的东西等于零，见 queue.rules.selfconsistency_ceiling）。
//
// 刻意不做的事：不暴露 voice 内部结构、不暴露混音参数。自测唯一能操纵的入口
// 仍然是 xaudio2.h 里的那 10 个方法——它测的是"调用者看到的语义"，不是实现细节。
// ============================================================================

#include <xaudio2.h>

#include <cstdint>

namespace concord::compat::xaudio2 {

// 无设备的引擎。CreateMasteringVoice 仍然必须被调用（它建立主声部与效果链），
// 但不会打开 AudioQueue。没有输出设备的环境里也能跑完混音断言。
IXAudio2* createOfflineEngine();

// 引擎当前实际送给设备的那一段渲染：交错 16 位、48kHz、立体声。
// 返回实际写入的帧数（== frames；引擎永远不会"少写"，沉默就是 0 样本）。
unsigned renderInterleaved(IXAudio2* engine, std::int16_t* out, unsigned frames);

// 主声部的混音格式。设备回调、自测与效果链锁定格式三处必须一致，所以集中在这里。
constexpr unsigned kMixSampleRate = 48000;
constexpr unsigned kMixChannels = 2;

} // namespace concord::compat::xaudio2
