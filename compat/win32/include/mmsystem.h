#pragma once
// ============================================================================
// ConcordRTS Win32 兼容层 · mmsystem.h（S1）
// 只提供类型 / 常量 / 声明；实现见 S4（音频族）或直接由 TU 退出构建图。
// 守卫见 windows.h 顶部说明：本目录只在非 Windows 生效。
// ============================================================================

#if defined(_WIN32)
#error "compat/win32/include/mmsystem.h is the non-Windows shim; it must never be used on Windows builds."
#endif

#include "windows.h"

// ---------------------------------------------------------------------------
// 定时器分辨率
// ---------------------------------------------------------------------------
struct TIMECAPS {
    UINT wPeriodMin;
    UINT wPeriodMax;
};
using LPTIMECAPS = TIMECAPS*;

#define TIMERR_NOERROR 0
#define TIMERR_NOCANDO 97
#define TIMERR_STRUCT 129
#define MAXPNAMELEN 32
#define MAXERRORLENGTH 256

// ---------------------------------------------------------------------------
// 波形音频
//
// WAVEFORMATEX 在 Windows SDK 里是 **pack(1)** 的（mmreg.h 用 pshpack1.h 包裹），
// 因此 sizeof == 18、没有尾部填充。这里必须照做：不 pack 的话 macOS 上是 20
// （尾部 2 字节补齐），字段偏移碰巧一样，所以"读字段"不会出错，但
//   · sizeof(WAVEFORMATEX) 会对不上；
//   · cbSize==22 这个 WAVEFORMATEXTENSIBLE 判据算不平（18+22=40）；
//   · 把 WAVEFORMATEX* 重解释成 WAVEFORMATEXTENSIBLE* 时 Samples 会落到 20 而不是 18。
// 第三种是**静默错值**：RuntimeAudioPeakLimiter::validFormat 会拿到错位的
// wValidBitsPerSample，把合法格式判成不合法。这个 bug 是 ksmedia.h 里的
// static_assert(sizeof(WAVEFORMATEX)==18) 抓出来的，不是靠读代码看出来的。
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct WAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
};
#pragma pack(pop)
using LPWAVEFORMATEX = WAVEFORMATEX*;
using LPCWAVEFORMATEX = const WAVEFORMATEX*;

static_assert(sizeof(WAVEFORMATEX) == 18,
              "WAVEFORMATEX must be packed to 18 bytes like the Windows SDK");
static_assert(offsetof(WAVEFORMATEX, cbSize) == 16, "cbSize sits at byte 16");

struct WAVEHDR {
    LPSTR lpData;
    DWORD dwBufferLength;
    DWORD dwBytesRecorded;
    DWORD_PTR dwUser;
    DWORD dwFlags;
    DWORD dwLoops;
    WAVEHDR* lpNext;
    DWORD_PTR reserved;
};
using LPWAVEHDR = WAVEHDR*;

#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_IEEE_FLOAT 3
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#define WAVE_MAPPER ((UINT)-1)
#define WHDR_DONE 0x00000001
#define WHDR_PREPARED 0x00000002
#define WHDR_BEGINLOOP 0x00000004
#define WHDR_ENDLOOP 0x00000008
#define WHDR_INQUEUE 0x00000010
#define CALLBACK_NULL 0x00000000
#define CALLBACK_WINDOW 0x00010000
#define CALLBACK_FUNCTION 0x00030000
#define CALLBACK_EVENT 0x00050000
#define TIME_ONESHOT 0x0000
#define TIME_PERIODIC 0x0001
#define TIME_CALLBACK_FUNCTION 0x0000
#define TIME_CALLBACK_EVENT_SET 0x0010
#define TIME_CALLBACK_EVENT_PULSE 0x0020
#define TIME_KILL_SYNCHRONOUS 0x0100
#define SND_SYNC 0x0000
#define SND_ASYNC 0x0001
#define SND_NODEFAULT 0x0002
#define SND_MEMORY 0x0004
#define SND_LOOP 0x0008
#define SND_NOSTOP 0x0010
#define SND_PURGE 0x0040
#define SND_APPLICATION 0x0080
#define SND_ALIAS 0x00010000
#define SND_FILENAME 0x00020000
#define SND_RESOURCE 0x00040004

// MCI 句柄类型与常用命令
using MCIDEVICEID = UINT;
using MCIERROR = MMRESULT;
#define MCI_OPEN 0x0803
#define MCI_CLOSE 0x0804
#define MCI_PLAY 0x0806
#define MCI_STOP 0x0808
#define MCI_STATUS 0x0814
#define MCI_NOTIFY 0x00000001
#define MCI_WAIT 0x00000002
#define MCI_FROM 0x00000004
#define MCI_TO 0x00000008

// MCI 错误码。数值取自 mmsyscom.h（MCIERR_BASE + n），不是自造的：
// 调用方会把这些值原样透传进日志或错误字符串，错一位就成了另一条消息。
#define MCIERR_BASE 256
#define MCIERR_INVALID_DEVICE_NAME (MCIERR_BASE + 7)      // 263
#define MCIERR_UNSUPPORTED_FUNCTION (MCIERR_BASE + 18)    // 274
#define MCIERR_FILE_NOT_FOUND (MCIERR_BASE + 19)          // 275
#define MCIERR_DEVICE_NOT_READY (MCIERR_BASE + 20)        // 276
#define MCIERR_DEVICE_NOT_INSTALLED (MCIERR_BASE + 50)    // 306

extern "C" {

MMRESULT timeBeginPeriod(UINT uPeriod);
MMRESULT timeEndPeriod(UINT uPeriod);
DWORD timeGetTime();
MMRESULT timeGetDevCaps(LPTIMECAPS ptc, UINT cbtc);
MMRESULT timeSetEvent(UINT uDelay, UINT uResolution, void (*lpTimeProc)(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR),
                      DWORD_PTR dwUser, UINT fuEvent);
MMRESULT timeKillEvent(UINT uTimerID);

MMRESULT waveOutOpen(HANDLE* phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
                     DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen);
MMRESULT waveOutClose(HANDLE hwo);
MMRESULT waveOutPrepareHeader(HANDLE hwo, LPWAVEHDR pwh, UINT cbwh);
MMRESULT waveOutUnprepareHeader(HANDLE hwo, LPWAVEHDR pwh, UINT cbwh);
MMRESULT waveOutWrite(HANDLE hwo, LPWAVEHDR pwh, UINT cbwh);
MMRESULT waveOutReset(HANDLE hwo);
MMRESULT waveOutSetVolume(HANDLE hwo, DWORD dwVolume);
MMRESULT waveOutGetVolume(HANDLE hwo, LPDWORD pdwVolume);
MMRESULT waveOutGetNumDevs();

BOOL PlaySoundW(LPCWSTR pszSound, HMODULE hmod, DWORD fdwSound);
BOOL PlaySoundA(LPCSTR pszSound, HMODULE hmod, DWORD fdwSound);

MMRESULT mciSendStringW(LPCWSTR lpstrCommand, LPWSTR lpstrReturnString, UINT uReturnLength, HWND hwndCallback);
MMRESULT mciGetErrorStringW(MMRESULT mcierr, LPWSTR pszText, UINT cchText);
BOOL mciSendCommandW(MCIDEVICEID mciId, UINT uMsg, DWORD_PTR dwParam1, DWORD_PTR dwParam2);
MCIDEVICEID mciGetDeviceIDW(LPCWSTR pszDevice);

} // extern "C"
