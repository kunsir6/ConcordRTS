// Q-05: Cocoa 呈现层实现（Objective-C++，ARC）。
//
// 时序上必须与 compat 的消息循环配合而不是取代它：
//   · GetMessageW / DispatchMessageW 仍然由 compat 层驱动（S7 的消息队列是真的）；
//   · 这个 TU 只负责两件事：把 NSWindow 建出来、把每帧像素贴上去。
// 所以它不是"另一个事件循环"，而是一个纯粹的显示终端。
//
// 每帧都重建 CGImage 而不是缓存：CompatWindow::surface 是**同一块** DIB 内存，
// GDI 每帧往里覆盖写。缓存 CGImage 会让它一直指向那块被改写的内存，
// 而 CoreAnimation 的 layer.contents 需要一个稳定的图像对象 —— 缓存等于
// 让屏幕上永远显示最后一帧和第一帧的混合体。

#include "RuntimeAppleWindowSurface.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CoreAnimation.h>

#include <CoreGraphics/CoreGraphics.h>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

NSWindow* gWindow = nil;
NSView* gView = nil;
bool gApplicationReady = false;

// 32bpp DIB 的字节序是 B,G,R,X（小端），所以 CGImage 用
// kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst：
//   · Little 让 CG 按 B,G,R 顺序读字节；
//   · NoneSkipFirst 跳过第 4 字节（GDI 不写 alpha，留 0，如果按
//     PremultipliedLast 解释，整幅画面会变成全透明——屏幕上什么都没有，
//     而且看起来像是"贴图失败"）。
// 这个组合与 S6 的 GDI+ PNG 导出用的是同一套，那次已经用解回验证过通道次序。
// 两个枚举属于不同的匿名 enum，直接位或会触发
// -Wdeprecated-anon-enum-enum-conversion（S6 那次也踩过同一个）。
// 先各自转 uint32_t 再合成，语义不变、警告消失。
constexpr CGBitmapInfo kSurfaceBitmapInfo = static_cast<CGBitmapInfo>(
    static_cast<std::uint32_t>(kCGBitmapByteOrder32Little)
    | static_cast<std::uint32_t>(kCGImageAlphaNoneSkipFirst));

}  // namespace

namespace concord_compat::apple {

void present(const Surface& surface) {
    if (surface.pixels == nullptr || surface.width <= 0 || surface.height <= 0) return;

    @autoreleasepool {
        if (!gApplicationReady) {
            // NSApplication 必须先于任何 NSWindow 创建。activationPolicy=Regular
            // 才能出现在 Dock 里并接收键盘焦点；Accessory 会让窗口浮着但收不到键。
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            // 没有 mainMenu 时 AppKit 会打印警告，而且 Cmd-Q 不工作。
            NSMenu* menuBar = [[NSMenu alloc] init];
            [NSApp setMainMenu:menuBar];
            gApplicationReady = true;
        }

        if (gWindow == nil) {
            const NSRect contentRect =
                NSMakeRect(0, 0, surface.width, surface.height);
            const NSWindowStyleMask style = NSWindowStyleMaskTitled
                | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
            gWindow = [[NSWindow alloc] initWithContentRect:contentRect
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
            // releasedWhenClosed=NO：窗口关闭后我们仍要能重用它，
            // 否则第二次 ShowWindow 会往一个已释放的对象发消息。
            gWindow.releasedWhenClosed = NO;
            const char* title = surface.titleUtf8;
            gWindow.title = (title != nullptr && title[0] != '\0')
                ? [NSString stringWithUTF8String:title]
                : @"ConcordRTS";

            gView = [[NSView alloc] initWithFrame:contentRect];
            gView.wantsLayer = YES;
            // Resize 让画面随窗口拉伸，而不是居中留黑边——游戏窗口是
            // "把客户区铺满"的语义，不是"按原始尺寸展示图片"。
            gView.layer.contentsGravity = kCAGravityResize;
            gView.layer.magnificationFilter = kCAFilterNearest;
            gWindow.contentView = gView;

            [gWindow center];
            [gWindow makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            // 一次性取证：仅靠"进程没崩"无法区分"present 没被调用"和
            // "调用了但窗口没显示"。这行让两者可分辨，且只在建窗口时打一次。
            std::fprintf(stderr, "[cocoa] NSWindow created %dx%d title=%s visible=%d\n",
                         surface.width, surface.height, title != nullptr ? title : "",
                         surface.visible ? 1 : 0);
        }

        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        const std::size_t byteCount = static_cast<std::size_t>(surface.bytesPerRow)
            * static_cast<std::size_t>(surface.height);
        // 不拷贝（第三个参数传 nullptr）：DIB 的内存归 CompatWindow 所有，
        // 生命周期比这个 CGImage 长；拷贝会让每帧多一次 2.6MB memcpy。
        CGDataProviderRef provider =
            CGDataProviderCreateWithData(nullptr, surface.pixels, byteCount, nullptr);
        CGImageRef image = CGImageCreate(
            static_cast<std::size_t>(surface.width),
            static_cast<std::size_t>(surface.height),
            8, 32,
            static_cast<std::size_t>(surface.bytesPerRow),
            colorSpace,
            kSurfaceBitmapInfo,
            provider,
            nullptr, false, kCGRenderingIntentDefault);
        if (image != nullptr) {
            gView.layer.contents = (__bridge id)image;
            CGImageRelease(image);
            static bool announcedFirstFrame = false;
            if (!announcedFirstFrame) {
                announcedFirstFrame = true;
                std::fprintf(stderr, "[cocoa] first frame attached to layer.contents\n");
            }
        }
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(colorSpace);

        if (surface.visible) {
            if (gWindow.isMiniaturized) [gWindow deminiaturize:nil];
            if (!gWindow.isVisible) [gWindow makeKeyAndOrderFront:nil];
        } else {
            // SW_HIDE / 最小化：orderOut 而不是 close，
            // 关掉会让窗口对象进入"已关闭"状态、再显示需要额外复位。
            if (gWindow.isVisible) [gWindow orderOut:nil];
        }
    }
}

void shutdown() {
    @autoreleasepool {
        if (gWindow != nil) {
            [gWindow orderOut:nil];
            gWindow = nil;
        }
        gView = nil;
        gApplicationReady = false;
    }
}

}  // namespace concord_compat::apple
