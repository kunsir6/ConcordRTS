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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>

namespace {

NSWindow* gWindow = nil;
NSView* gView = nil;
bool gApplicationReady = false;

concord_compat::apple::InputSink gInputSink = nullptr;
void* gInputContext = nullptr;
bool gInputTrace = false;  // CONCORD_INPUT_TRACE=1 时打一行/事件，取证用

// 上一次**呈现**的尺寸/标题。用来判断"这次贴的是不是另一个界面"。
// 为什么不拿 NSWindow 的当前尺寸比对：窗口可以被用户拖动缩放，那时
// 尺寸与 surface 不一致是正常的，不该被我们掰回去。只有"surface 自己变了"
// 才说明换了界面（主菜单 1024x640 → 遭遇战 800x600 → 游戏内视图）。
int gPresentedWidth = 0;
int gPresentedHeight = 0;
std::string gPresentedTitle;

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
            gPresentedWidth = surface.width;
            gPresentedHeight = surface.height;
            gPresentedTitle = title != nullptr ? title : "";
        } else if (surface.width != gPresentedWidth || surface.height != gPresentedHeight) {
            // 换了界面尺寸：必须跟着改，否则小尺寸界面会被 contentsGravity=Resize
            // 拉伸铺满旧窗口——画面"能看"但比例是错的，而且这种错不会报任何东西。
            // 遭遇战菜单是 800x600、主菜单是 1024x640，走的就是这条路。
            gPresentedWidth = surface.width;
            gPresentedHeight = surface.height;
            const NSRect contentRect = NSMakeRect(0, 0, surface.width, surface.height);
            [gWindow setContentSize:contentRect.size];
            gView.frame = contentRect;
            std::fprintf(stderr, "[cocoa] NSWindow resized to %dx%d\n",
                         surface.width, surface.height);
        }

        // 标题只在真的变了时才写（每帧写会打断 AppKit 的标题栏）。
        const char* currentTitle = surface.titleUtf8;
        if (currentTitle != nullptr && currentTitle[0] != '\0'
            && gPresentedTitle != currentTitle) {
            gPresentedTitle = currentTitle;
            gWindow.title = [NSString stringWithUTF8String:currentTitle];
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

void setInputSink(InputSink sink, void* context) {
    gInputSink = sink;
    gInputContext = context;
    static const bool trace = []() {
        const char* value = std::getenv("CONCORD_INPUT_TRACE");
        return value != nullptr && value[0] == '1';
    }();
    gInputTrace = trace;
}

namespace {

// macOS 的硬件键码 → Windows 虚拟键码。只覆盖站点实际会用到的键：
// 菜单靠鼠标，键盘只需要 Esc / Enter / 方向 / 空格 这几个。
// 字母和数字走 characters 分支（见下面），不在这张表里。
int translateKeyCode(unsigned short macKeyCode) {
    switch (macKeyCode) {
        case 0x35: return 0x1B;  // Escape
        case 0x24: return 0x0D;  // Return
        case 0x30: return 0x09;  // Tab
        case 0x33: return 0x08;  // Backspace
        case 0x31: return 0x20;  // Space
        case 0x7B: return 0x25;  // Left
        case 0x7C: return 0x27;  // Right
        case 0x7D: return 0x28;  // Down
        case 0x7E: return 0x26;  // Up
        case 0x73: return 0x24;  // Home
        case 0x77: return 0x23;  // End
        case 0x74: return 0x21;  // PageUp
        case 0x79: return 0x22;  // PageDown
        case 0x75: return 0x2E;  // Delete
        default:   return 0;
    }
}

void emit(NSEvent* event) {
    if (gInputSink == nullptr || gView == nil) return;

    concord_compat::apple::InputEvent out;
    const NSEventType type = event.type;

    switch (type) {
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
            out.kind = concord_compat::apple::InputEvent::Kind::MouseMove;
            break;
        case NSEventTypeLeftMouseDown:   out.kind = concord_compat::apple::InputEvent::Kind::LeftDown; break;
        case NSEventTypeLeftMouseUp:     out.kind = concord_compat::apple::InputEvent::Kind::LeftUp; break;
        case NSEventTypeRightMouseDown:  out.kind = concord_compat::apple::InputEvent::Kind::RightDown; break;
        case NSEventTypeRightMouseUp:    out.kind = concord_compat::apple::InputEvent::Kind::RightUp; break;
        case NSEventTypeOtherMouseDown:  out.kind = concord_compat::apple::InputEvent::Kind::MiddleDown; break;
        case NSEventTypeOtherMouseUp:    out.kind = concord_compat::apple::InputEvent::Kind::MiddleUp; break;
        case NSEventTypeScrollWheel:     out.kind = concord_compat::apple::InputEvent::Kind::Wheel; break;
        case NSEventTypeKeyDown:         out.kind = concord_compat::apple::InputEvent::Kind::KeyDown; break;
        case NSEventTypeKeyUp:           out.kind = concord_compat::apple::InputEvent::Kind::KeyUp; break;
        default: return;  // FlagsChanged / 窗口事件等一律忽略
    }

    // 坐标：AppKit 的原点在左下，客户区 DIB 的原点在左上，所以要翻 y。
    // 不翻的话点击会落在垂直镜像的位置——菜单上点"单人游戏"会命中"退出"。
    if (type != NSEventTypeKeyDown && type != NSEventTypeKeyUp) {
        const NSPoint local = [gView convertPoint:event.locationInWindow fromView:nil];
        out.x = static_cast<int>(std::lround(local.x));
        out.y = static_cast<int>(std::lround(gView.bounds.size.height - local.y));
    }

    if (type == NSEventTypeScrollWheel) {
        // WHEEL_DELTA 是 120。scrollingDeltaY 单位是行，正负与 Windows 相反。
        out.wheelDelta = static_cast<int>(std::lround(-event.scrollingDeltaY * 120.0));
        if (out.wheelDelta == 0) return;
    }

    if (type == NSEventTypeKeyDown || type == NSEventTypeKeyUp) {
        out.keyCode = translateKeyCode(event.keyCode);
        if (out.keyCode == 0) {
            // 字母/数字：用 characters 取可打印字符，取大写 ASCII 当 VK。
            // VK_A..VK_Z 与 VK_0..VK_9 正好等于大写 ASCII。
            NSString* chars = event.charactersIgnoringModifiers;
            if (chars.length > 0) {
                const unichar c = [chars characterAtIndex:0];
                if (c < 0x80) {
                    const char upper = static_cast<char>(std::toupper(c));
                    if ((upper >= 'A' && upper <= 'Z') || (upper >= '0' && upper <= '9')) {
                        out.keyCode = upper;
                    }
                }
            }
        }
        if (out.keyCode == 0) return;
    }

    if (gInputTrace) {
        std::fprintf(stderr, "[input] kind=%d x=%d y=%d key=%d wheel=%d\n",
                     static_cast<int>(out.kind), out.x, out.y, out.keyCode, out.wheelDelta);
    }
    gInputSink(out, gInputContext);
}

}  // namespace

void pumpEvents() {
    if (gInputSink == nullptr || gWindow == nil) return;
    @autoreleasepool {
        // distantPast 让调用立刻返回（不阻塞），把所有排队事件一次排空。
        // 只在窗口是关键窗口/主窗口时取，否则会偷走别的 App 的事件焦点语义。
        for (;;) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event == nil) break;
            // 窗口关闭按钮走标准关闭路径：转成 quit，否则点了红叉没反应。
            if (event.type == NSEventTypeAppKitDefined) continue;
            if (event.window != nil && event.window != gWindow) continue;
            emit(event);
        }
    }
}

}  // namespace concord_compat::apple
