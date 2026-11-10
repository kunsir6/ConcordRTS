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

}  // namespace concord_compat::apple

#endif  // CONCORD_COMPAT_APPLE_WINDOW_SURFACE_H
