// Win32 兼容层：键盘状态表的最小公共声明（U-07）
//
// 为什么要单独一个头，而不是塞进 RuntimeWin32CompatInternal.h：
//   那个头 `#include <windows.h>`，而**唯一**需要写这张表的非 Win32 侧代码是
//   RuntimeAppleWindowSurface.mm（Objective-C++，`#import <Cocoa/Cocoa.h>`）。
//   windows.h 与 AppKit 在同一个 TU 里同名符号会互撞，实测直接编译失败：
//       conflicting types for 'Polygon' / 'GetCurrentProcess' / 'GetProcessInformation'
//   （windows.h 先声明，HIServices 的 Processes.h 再声明一份）。
//   可行的绕法是"调换包含顺序"，但那样修的是报错顺序，不是冲突本身——换个
//   SDK 版本就又会反过来。真正干净的边界是：**这个 TU 不需要知道 WPARAM 是什么。**
//
// 所以这里只暴露两个概念：
//   · setModifierKeyState(mask)  —— 用**抽象修饰位**覆盖式写入；
//   · setVirtualKeyDown(vk, down) —— 用 Windows 虚拟键码写单键（供 compat 内部用）。
// macOS 的 NSEventModifierFlag 到 VK_* 的映射留在 .cpp 里（那里有 windows.h），
// .mm 只负责把 AppKit 的花样翻成这里的抽象位。任何一侧新增修饰键，都改这一张表。

#ifndef CONCORD_COMPAT_WIN32_KEY_STATE_H
#define CONCORD_COMPAT_WIN32_KEY_STATE_H

namespace concord_compat {

// 抽象修饰位。取值与任何平台的常量都无关，只在本层内部对齐。
enum ModifierKeyFlag : unsigned {
    kModifierShift = 1u << 0,
    kModifierControl = 1u << 1,
    kModifierAlt = 1u << 2,
    kModifierCapsLock = 1u << 3,
};

// GetKeyState 的后端。Windows 上这张表由系统在输入排队/派发时维护；本层没有
// 系统，必须自己维护——否则 GetKeyState 恒 0，所有带修饰键的快捷键会**静默**
// 失效：Ctrl+数字编队、Shift+点击加选、Alt+数字定位、Ctrl+强制攻击……
// 症状不是报错，而是"按了没反应"，很容易被误判成"功能没实现"。
//
// 判据落在解析后的键上，不落在事件类型上：macOS 的修饰键只发 FlagsChanged，
// 不发 KeyDown/KeyUp，所以桥接层必须在两种事件上都同步这张表。
void setVirtualKeyDown(int virtualKey, bool down) noexcept;

// 覆盖式写入修饰键状态：mask 是"当前真实状态快照"，不是增量。
// 用覆盖而不是逐键置位，是为了处理"焦点从别的应用切回来"——那时增量推算不出
// 中间漏掉了哪次抬起。
void setModifierKeyState(unsigned modifierMask) noexcept;

// 失焦/窗口切换时清掉修饰键。否则 Ctrl 会"卡住"——之后所有点击都变成 Ctrl+点击，
// 表现为选择行为莫名其妙（加选/强制攻击）。
void clearModifierKeyDown() noexcept;

}  // namespace concord_compat

#endif  // CONCORD_COMPAT_WIN32_KEY_STATE_H
