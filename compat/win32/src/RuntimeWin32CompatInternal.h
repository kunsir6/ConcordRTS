// Win32 兼容层内部共用件（不进产品源码，只在本层内部与自测之间共享）
//
// 为什么需要这个头：S10 开始，同一件事第一次有**两个**入口——
//   · `GetEnvironmentVariableW` 与 S9 的 `_wgetenv_s` 都是"取环境变量再转宽"；
//   · `MultiByteToWideChar` 与 S9 的本地 `wideFromUtf8` 助手都是"UTF-8 解码"。
// 各写一份的后果不是编译错误，而是**两份实现渐渐走偏**：一边把非法字节替换成
// U+FFFD、另一边截断，于是同一条路径在不同调用点上表现不同，而这种差异只有
// 在喂进非法字节时才会显形。
//
// 所以这里把三件事抬成唯一实现：LastError、句柄表、编解码内核。

#ifndef CONCORD_COMPAT_WIN32_INTERNAL_H
#define CONCORD_COMPAT_WIN32_INTERNAL_H

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace concord_compat {

// ---------------------------------------------------------------------------
// LastError
// ---------------------------------------------------------------------------
// 必须是**线程局部**的。产品里有窗口线程、渲染线程、音频线程同时在跑；全局一个
// DWORD 会让 A 线程读到 B 线程的错误码，症状是"偶发地进了错误的失败分支"——
// 既不能稳定复现，也不会在任何单线程自测里出现。
//
// 另一条 Win32 契约同样是判据：**成功的调用不清 LastError**。所以本层所有成功
// 路径一行都不写它，只有失败路径才 set。若为了"干净"在每个函数入口清 0，
// 调用点惯用的"先做一件可能失败的事，再读 GetLastError"就会读到 0。
DWORD lastError() noexcept;
void setLastError(DWORD code) noexcept;

// errno → Win32 错误码。映射表按**调用点真正会比较的那几个**优先：
//   EEXIST  → ERROR_FILE_EXISTS(80)        AtomicFile 的"换个临时名重试"
//   ENOENT  → ERROR_FILE_NOT_FOUND(2)       "文件不存在"判定
//   EACCES  → ERROR_ACCESS_DENIED(5)
//   EISDIR  → ERROR_ACCESS_DENIED(5)        Windows 上打开目录写是拒绝访问
//   ENOSPC  → ERROR_DISK_FULL(112)
//   EMFILE  → ERROR_TOO_MANY_OPEN_FILES(4)
//   ENAMETOOLONG → ERROR_FILENAME_EXCED_RANGE(206)
// 其余一律 ERROR_GEN_FAILURE(31) ——**不是** ERROR_SUCCESS，也不是 ACCESS_DENIED：
// 把未知错误说成"拒绝访问"会让调用方去检查权限，方向就错了。
void setLastErrorFromErrno(int code) noexcept;

// ---------------------------------------------------------------------------
// 句柄表
// ---------------------------------------------------------------------------
// 本层最终会有三类句柄：文件（S10）、事件/线程/映射（S8）、窗口（S7）。`CloseHandle`
// 收到任意一个 HANDLE 时必须能回答"这是不是我能关的"：
//   · 对文件句柄 → 关掉；
//   · 对 GDI 对象/DC 指针 → 返回 FALSE，**绝不能**把指针当 fd 关掉；
//   · 对野指针 → 返回 FALSE，**绝不能**解引用。
//
// 判据只能是"是否登记在表里"。给每类加 magic 也行，但 magic 要解引用——野指针
// 那一步就已经崩了。查表只比较指针值，不碰它指向的内存，这是唯一安全的做法。
using HandleDestructor = void (*)(void* payload);

bool registerHandle(void* handle, HandleDestructor destroy, const char* kind) noexcept;
// TRUE = 是本层登记过的句柄，已关闭；FALSE = 不是本层的句柄（未做任何事）。
bool closeRegisteredHandle(void* handle) noexcept;
// 这个句柄是不是**某一类**已登记句柄。名字必须对得上——同一个指针被登记成
// "event" 却按 "file" 去解释，等于把一种对象的布局当成另一种。
// 用途：把自己持有的句柄指针转回具体类型之前，先证明它真是这一类的。
bool isRegisteredHandle(void* handle, const char* kind) noexcept;
// 当前存活的登记句柄数。自测用它证明"关干净了"——泄漏在 Windows 上是进程级的
// 慢性病，在这里是自测能一眼看见的数字。
std::size_t liveHandleCount() noexcept;

// ---------------------------------------------------------------------------
// 屏幕 DC（GetDC(NULL) 的后备设备）
// ---------------------------------------------------------------------------
// Win32 里 GetDC(NULL) 返回屏幕 DC，产品大量拿它当 CreateCompatibleDC /
// CreateDIBSection 的参照设备，并用"非空"当成功判据（本仓库 71 处）。返回
// nullptr 会让这些调用点**全部**走进失败分支，而失败信息通常只有一句
// "... creation failed"，看不出根因。
//
// 实现说明见 RuntimeWin32CompatGdi.cpp 末尾：单例、进程级、不计入 GDI 对象数、
// DeleteDC 明确拒绝。width/height 由调用方传入（与 GetSystemMetrics 保持同一套值）。
HDC screenReferenceDc(int width, int height) noexcept;

// ---------------------------------------------------------------------------
// 目录变更通知（S12）
// ---------------------------------------------------------------------------
// "同一种对象、两个入口"再一次出现：创建/复位/关闭在 ChangeNotify.cpp，
// 而等待在 Kernel.cpp 的 WaitForSingleObject 里。这两个函数就是那个接缝——
// 没有它们，Kernel.cpp 只能看见"未知句柄"，于是 WaitForSingleObject 会对
// 变更通知句柄返回 WAIT_FAILED（调用方会把它当成"资源变了"或"没变"的**错误**
// 答案之一，而不是"不支持"）。
bool isChangeNotifyHandle(HANDLE handle);
DWORD waitChangeNotify(HANDLE handle, DWORD milliseconds);

// ---------------------------------------------------------------------------
// 文本编解码
// ---------------------------------------------------------------------------
// 本平台的 wchar_t 是 **32 位（UTF-32）**，Windows 是 16 位（UTF-16）。这不是
// "差不多"的差别：同一个 emoji 在 Windows 是两个 wchar_t，在这里是一个。
// 因此凡是 `result.size()` 参与判据的调用点（缓冲区长度、截断阈值），两边的
// 数字本来就不同——本层保证的是"码点序列一致"，不是"wchar_t 个数一致"。
struct DecodedText {
    std::vector<std::uint32_t> codes;
    // 与 codes 等长。1 = 该码点是由非法输入替换来的 U+FFFD。
    // 严格模式（MB_ERR_INVALID_CHARS / WC_ERR_INVALID_CHARS）据此失败；
    // 非严格模式据此把该位置换成调用方给的默认字符。
    std::vector<std::uint8_t> invalid;

    bool anyInvalid() const noexcept;
};

// 非法判据（与 Windows 一致，不是"能解出来就算"）：
//   · 过长编码（overlong，如 C0 80 表示 U+0000）
//   · 代理区码点（ED A0 80 = U+D800，UTF-8 不允许编码代理）
//   · 超出 U+10FFFF
//   · 截断的多字节序列
// 任何一条命中都算非法——放宽任何一条，都会让"严格模式失败"这条契约失效。
DecodedText decodeUtf8(const char* bytes, std::size_t count);
DecodedText decodeWide(const wchar_t* units, std::size_t count);

void appendUtf8(std::string& out, std::uint32_t code);
void appendWide(std::wstring& out, std::uint32_t code);

// NUL 结尾的快捷形式（S9 的环境变量助手与 S10 的环境变量 API 共用）
std::string utf8FromWideText(const wchar_t* text);
std::wstring wideTextFromUtf8(const char* text);

// ---------------------------------------------------------------------------
// 定时器（S11）
// ---------------------------------------------------------------------------
// 这是"SetTimer 在 Input.cpp、取消息在 User.cpp"造成的**跨 TU 接缝**：定时器
// 必须由注册方与派发方共享同一张表，否则两边各记一份就永远对不上。
//
// 为什么表放在这里而不是直接塞进 User.cpp：SetTimer/KillTimer 属于 P5-05 的
// 输入族，把它们的实现搬走会让"这一族在哪"变得含糊；抬成共享件只是把一个
// 隐式耦合写成显式的。
using TimerProc = void (*)(HWND, UINT, UINT_PTR, DWORD);

struct TimerDue {
    HWND hwnd = nullptr;
    UINT_PTR id = 0;
    TimerProc proc = nullptr;
};

bool timerCreate(HWND hwnd, UINT_PTR id, UINT elapse, TimerProc proc);
bool timerRemove(HWND hwnd, UINT_PTR id);
// 取一条**已到期**的定时器。filter 非空时只匹配该窗口（不匹配的留在表里）。
// consume=false 供 PeekMessage(PM_NOREMOVE) 用：看得见但不消费。
bool timerTakeDue(HWND filter, bool consume, TimerDue& out);
// 距下一次到期还有多少毫秒；没有定时器返回 -1，已到期返回 0。取消息的等待
// 上限用它，否则 16ms 的定时器会被 1ms 轮询白白唤醒 16 次。
long long timerMillisUntilNext();
void timerDropForWindow(HWND hwnd);
std::size_t timerCount();

}  // namespace concord_compat

// 键盘状态（U-07）。刻意**不**把这两个函数写在上面这段里，而是拆去一个不带
// windows.h 的小头：唯一需要调它们的非 Win32 侧代码是 .mm（Cocoa），而
// windows.h 与 AppKit 在同一个 TU 里会撞符号（Polygon / GetCurrentProcess）。
// 见 RuntimeWin32CompatKeyState.h 顶部。
#include "RuntimeWin32CompatKeyState.h"

#endif  // CONCORD_COMPAT_WIN32_INTERNAL_H
