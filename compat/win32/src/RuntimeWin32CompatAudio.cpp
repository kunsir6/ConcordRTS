// ============================================================================
// compat/win32/src/RuntimeWin32CompatAudio.cpp
//
// XAudio2 的**实现层**：只兑现一个 C-linkage 符号 XAudio2Create。
//
// 为什么是"诚实的失败"而不是"最小可用引擎"：
//   XAudio2 在本项目里只被 src/audio/backend/RuntimeVoiceAudio.cpp 用来做
//   多路语音（每条语音一个 source voice + 一张输出矩阵，做增益/声道平移）。
//   它在 macOS 上要成立，需要的不是"一个能返回 S_OK 的对象"，而是**真正的混音
//   器**：多条 source voice 并行喂 16 位 PCM、按矩阵混到主声部、支持 LoopCount、
//   支持运行期 SetVolume/SetOutputMatrix。拿一个空壳顶上去，RuntimeVoiceAudio
//   会 ready()==true，然后每一条语音都"播放成功但不出声"——这正好是本项目
//   rules.honest_success_vs_fake_success 判据的反面（判据落在效果上：
//   出声这件事在 mac 上一旦没做，就必须失败）。
//
//   所以这里返回 E_NOTIMPL，并让 RuntimeVoiceAudio::lastError() 把它原样带出去：
//   "设备不可用"（AUDCLNT_E_DEVICE_INVALIDATED 之类）与"这一层还没实现"
//   （E_NOTIMPL）在错误码上是分开的，调用方和日志都能区分。
//
// 与 S9 里 CoInitializeEx 的做法一致的地方：那条能返回 S_OK，是因为"成功⇒必须
// 配对 CoUninitialize"的契约守得住；这条不能，因为"成功⇒会出声"守不住。
//
// 后续：真正的 XAudio2-on-AudioQueue（多路混音）已被登记为独立任务，不并进
// 音频工厂那一轮——它跟"菜单音效可播"没有共享代码路径，混在一起会让两边都
// 半成品。
// ============================================================================

#include <xaudio2.h>

extern "C" HRESULT XAudio2Create(IXAudio2** xaudio2, UINT32 /*flags*/, UINT32 /*processor*/) {
    // 不留下悬垂指针：调用方会把 *xaudio2 交给成员析构去 Release()。
    if (xaudio2 != nullptr) *xaudio2 = nullptr;
    return E_NOTIMPL;
}
