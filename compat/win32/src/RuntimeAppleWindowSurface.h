// Q-05: Cocoa 呈现层接口（macOS 真窗口）。
//
// 为什么这个头**不含任何 Win32 类型**：
// `${APPLE}` 侧的 .mm 必须 include <Cocoa/Cocoa.h>，而它把 BOOL 定义成
// signed char、还带 min/max 宏；compat 的 windows.h 把 BOOL 定义成 int。
// 同一个 TU 里两者必冲突，而且报的是"Cannot combine ... BOOL"这种看不出根因的错。
// 所以边界上只传裸指针和 int，UTF-8 标题也由调用方先转好。
//
// 背景：S7 做的是纯离屏窗口模型——窗口对象、WM_PAINT、DIB、PNG 导出全是真的，
// 但进程里没有 NSWindow，所以屏幕上什么都没有（ShowWindow 只翻一个 bool）。
// 这个 TU 就是补上"把已经画好的帧贴到屏幕上"这一层。

#ifndef CONCORD_COMPAT_APPLE_WINDOW_SURFACE_H
#define CONCORD_COMPAT_APPLE_WINDOW_SURFACE_H

namespace concord_compat::apple {

// 客户区一帧的画面描述。像素格式固定为 32bpp、小端 B,G,R,X、自顶向下——
// 与 CompatWindow::surface（CreateDIBSection，top-down 32bpp）完全一致。
struct Surface {
    const void* pixels = nullptr;
    int width = 0;
    int height = 0;
    int bytesPerRow = 0;
    const char* titleUtf8 = nullptr;  // 可空；空则用默认标题
    bool visible = false;             // false 时把窗口藏起来（SW_HIDE / 最小化）
};

// 每帧调用（EndPaint 之后）。第一次调用时懒创建 NSWindow 与 NSView。
// 内部 @autoreleasepool，可安全地在非 AppKit 管理的线程上被调用。
void present(const Surface& surface);

// 关窗口时调用。可重复调用。
void shutdown();

// ON-3-01 上屏自证（CONCORD_ONSCREEN_ATTEST_DIR=<dir> 开启）。
// 每次 present 之后把 layer.contents 的图像读回、重画成位图，与同一帧 DIB 的
// RGB 24 位 FNV-1a 对账，逐条写入 <dir>/onscreen-attest.log。
// 这两个计数供 smoke 入口判定退出码；未开启时恒为 0。
int onscreenAttestRecords();
int onscreenAttestFailures();

// ---------------------------------------------------------------------------
// Q-06: 输入接入
// ---------------------------------------------------------------------------
// 为什么需要回调而不是让 .mm 直接调 PostMessageW：PostMessageW 的参数是
// WPARAM/LPARAM 和 HWND，那些类型来自 compat 的 windows.h，而 .mm 里 include
// windows.h 会和 Cocoa 的 BOOL 冲突（见文件头）。所以边界上定义一套自己的
// 事件描述，由 compat 侧的回调翻译成 WM_* 消息。
struct InputEvent {
    enum class Kind {
        MouseMove, LeftDown, LeftUp, RightDown, RightUp, MiddleDown, MiddleUp,
        Wheel, KeyDown, KeyUp,
        // ON-2-04：用户点了窗口关闭按钮（红叉）。没有坐标也没有键码——
        // 它不是"输入"，但它必须和输入走同一条通道：.mm 拿不到 HWND，
        // 唯一被允许的跨界方式就是这套事件描述（理由见文件头）。
        // compat 侧收到后投 WM_CLOSE，之后的一切与 Windows 上关窗口完全一致。
        Close
    };
    Kind kind = Kind::MouseMove;
    int x = 0;           // 客户区坐标，原点在**左上**（已从 AppKit 左下原点翻转过）
    int y = 0;
    int keyCode = 0;     // Windows 虚拟键码 VK_*
    int wheelDelta = 0;  // 每格 120，与 WM_MOUSEWHEEL 的 WHEEL_DELTA 一致
};

using InputSink = void (*)(const InputEvent& event, void* context);

// 注册接收方。context 由调用方定义（compat 侧传目标窗口）。
void setInputSink(InputSink sink, void* context);

// 把队列里攒下的 AppKit 事件全部取出来、翻译后交给 sink。
// 必须由**消息循环**周期性调用（compat 的 GetMessageW 轮询里）：
// NSApp 的事件分发依赖 NSRunLoop，而本项目的主循环是 compat 自己的
// GetMessageW，不跑 NSRunLoop —— 不主动 nextEventMatchingMask 的话，
// 事件永远待在 AppKit 队列里，窗口看得见但点不动。
void pumpEvents();

// 隐藏/恢复窗口内的**系统**指针（U-04）。
//
// Windows 上游戏把硬件指针关掉、自己画箭头，所以玩家只看到一个指针；
// macOS 默认两者并存，菜单里会出现"系统箭头 + 游戏箭头"重叠。
// 这里用 view 的 cursor rect 把窗口区域内的系统指针换成不可见光标，
// 而不是 [NSCursor hide]：cursor rect 是区域限定的，鼠标移出窗口自动恢复，
// 不需要配对 unhide，也不会因窗口失焦/切换而把指针永久藏掉。
// 默认隐藏；CONCORD_HIDE_SYSTEM_CURSOR=0 可关闭（万一某个界面没有自绘指针）。
//
// U-05 结论：产品那套 SetCursor 请求**不代表**系统指针该不该出现（世界每秒几十次
// 请求 StockArrow，而游戏同时自绘箭头；截图实证 hide=0 时两个箭头并排），
// 所以这里保持"一律隐藏"，不做 SetCursor 镜像。
//
// L10N-02：隐藏区域是**内容区**而非整个窗口 frame——标题栏/边框是系统
// chrome，指针移上去要恢复系统箭头（拖动、关闭、最小化都在那里操作）。
void setSystemCursorHidden(bool hidden);
bool systemCursorHidden();

}  // namespace concord_compat::apple

#endif  // CONCORD_COMPAT_APPLE_WINDOW_SURFACE_H
