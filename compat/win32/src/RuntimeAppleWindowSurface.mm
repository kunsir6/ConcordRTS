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

// 只拿键盘状态那两个入口，**不**拿 RuntimeWin32CompatInternal.h：那个头会带进
// windows.h，与下面的 Cocoa 同名符号互撞（Polygon / GetCurrentProcess），实测
// 直接编译失败。这个 TU 不需要 WPARAM，也不需要知道 VK_* 的数值。
#include "RuntimeWin32CompatKeyState.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CoreAnimation.h>

#include <CoreGraphics/CoreGraphics.h>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>

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

// ON-1-03: 一次性注入合成 Cmd+Q 的自测开关（CONCORD_QUIT_SELFTEST=1）。
// 真实按键无法在无人值守环境下发出（需要辅助功能授权），但"事件进队列 →
// pumpEvents 取到 → 命中菜单键等价物 → terminate"这条链是可以自动验证的：
// 注入的事件走的就是同一条队列与同一个处理分支，只有事件来源是合成的。
bool gQuitSelfTestPosted = false;

// ON-2-04: 一次性注入"点红叉"的自测开关（CONCORD_CLOSE_SELFTEST=1）。
// 用 performClose: 而不是直接发 Close 事件，是为了让自测走**与真实点击完全一样**
// 的路径：performClose: → windowShouldClose: → Close 事件 → WM_CLOSE → 窗口销毁。
// 直接发事件会跳过 delegate，那就只测了 compat 那一半。
bool gCloseSelfTestDone = false;

// ON-2-04 全屏自测（CONCORD_FULLSCREEN_SELFTEST=1）。
// 全屏切换是**异步**的（有系统动画），不能用"发完 toggleFullScreen 立刻检查"来验，
// 必须在 windowDidEnterFullScreen / windowDidExitFullScreen 回调里逐段推进：
//     present → toggleFullScreen → didEnter（检查一次，再 toggle 退回）→ didExit（检查并收尾）
// 判据是"内容没丢"，不是"窗口变大了"：全屏期间游戏通常不重绘，
// 所以要比对的是 layer.contents 还在不在、CGImage 尺寸是否仍等于呈现尺寸、
// 以及最后一次 present 的内容 hash 是否没变。
bool gFullScreenTest = false;
bool gFullScreenRequested = false;
std::uint32_t gLastPresentHash = 0;
std::uint32_t gHashBeforeFullScreen = 0;
std::uint32_t gHashAfterEnter = 0;
std::uint32_t gHashAfterExit = 0;
int gFullScreenFailures = 0;
// 切换进行中（willEnter 之后、didEnter 之前）。全屏切换是 AppKit 的**动画状态机**，
// 必须靠 run loop 持续喂才能走完。本项目不跑 [NSApp run]，pumpEvents 每轮只给
// 2ms 的 nextEventMatchingMask——实测这就是卡点：willEnter 触发了，走到一半没有
// 后续 run loop 时间，didEnter 永远不到（自测 90s 超时退出 124）。
// 所以在切换期间把这一轮的等待放宽，等状态机走完再恢复 2ms。
bool gFullScreenTransition = false;
// 自测兜底：撤掉 FullScreenPrimary 之后 toggleFullScreen 会**什么都不做**，
// 既没有 willEnter 也没有 didEnter。不设上限的话自测会永远等下去（实测 90s 超时 124，
// 那种"挂住"看起来像缺陷，其实是"这个能力不存在"）。60 帧 ≈ 菜单 4 秒。
bool gFullScreenStarted = false;
int gFullScreenTicks = 0;

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

// ---------------------------------------------------------------------------
// ON-3-01 上屏自证
//
// "present 被调用" 不等于 "屏幕上有这幅画"（ON-2-05 就是贴了一张黑图而日志全绿）。
// 在没有屏幕录制授权的环境里，唯一可信的证据是：把**刚从 layer.contents 读回来**
// 的那张图重新画成位图，与同一帧 DIB 逐像素对账。它证明的是「贴上去的对象 ==
// 渲染出来的帧」，不证明「显示器上真的亮了」——后者只能靠人眼或窗口级抓图
// （ON-3-02 / ON-3-03）。两层证据分开记，别混成一句"上屏成功"。
//
// 两个必须做对的细节：
//   · 只比 B,G,R 三个字节。32bpp BI_RGB 的第 4 字节不是 alpha（S14b 的教训）：
//     GDI 写像素时保留目标原值，CGImage 用 NoneSkipFirst 直接跳过它，两边本来
//     就不该相等，拿它参与 hash 会得到一条永远红的假断言。
//   · **不要翻转 CTM**。CGImage 的数据第 0 行是图像顶行，DrawImage 到
//     (0,0,w,h) 时 CG 会把顶行对齐到 rect 顶部，而 CGBitmapContext 的缓冲第 0
//     行正是顶行——两者已经对齐。本轮实测：不翻转 hash == dibHash，
//     翻转后 0x890043ac != 0x674d476c（整幅上下颠倒）。
//     （别与 rules.cg_bitmap_row_zero_is_top 混淆：那条说的是"按底行解释缓冲
//     会得到翻转的字形"，这里是"画进缓冲时不要额外翻转"。）
int gAttestFailures = 0;
int gAttestRecords = 0;
int gAttestState = 0;  // 0=未探测 1=开 2=关
int gAttestLimit = 1;
int gAttestScaleTest = 0;
bool gScaleTestDone = false;
std::string gAttestDir;
std::FILE* gAttestLog = nullptr;

std::uint32_t attestHash24(const void* pixels, int width, int height, int bytesPerRow) {
    const auto* base = static_cast<const unsigned char*>(pixels);
    std::uint32_t hash = 2166136261u;
    for (int y = 0; y < height; ++y) {
        const unsigned char* row = base
            + static_cast<std::size_t>(y) * static_cast<std::size_t>(bytesPerRow);
        for (int x = 0; x < width; ++x) {
            const unsigned char* pixel = row + static_cast<std::size_t>(x) * 4u;
            hash = (hash ^ pixel[0]) * 16777619u;
            hash = (hash ^ pixel[1]) * 16777619u;
            hash = (hash ^ pixel[2]) * 16777619u;
        }
    }
    return hash;
}

void attestEnsureOpen() {
    if (gAttestState != 0) return;
    const char* dir = std::getenv("CONCORD_ONSCREEN_ATTEST_DIR");
    if (dir == nullptr || dir[0] == '\0') { gAttestState = 2; return; }
    gAttestDir = dir;
    const std::string path = gAttestDir + "/onscreen-attest.log";
    // 追加而不是覆盖：不同界面要分进程驱动（主菜单 / 世界 / 指针导航切页），
    // 一个目录里的记录是这几次运行合起来的取证链。
    gAttestLog = std::fopen(path.c_str(), "a");
    if (gAttestLog == nullptr) {
        gAttestState = 2;
        std::fprintf(stderr, "[attest] cannot open %s\n", path.c_str());
        return;
    }
    gAttestState = 1;
    // 默认拿 1 帧就退出：主菜单与世界首帧各跑一次即能覆盖两档界面。
    // 要看指针导航这类多帧序列时把 LIMIT 调大（例如 3 覆盖 首页→单人页→回程）。
    if (const char* limit = std::getenv("CONCORD_ONSCREEN_ATTEST_LIMIT")) {
        const int value = std::atoi(limit);
        if (value > 0) gAttestLimit = value;
    }
    // ON-2-03 验收：把窗口按 1.7 倍缩放，检查 contentsScale 仍跟随 backing scale、
    // 内容没有被重采样、且"上次呈现尺寸"没有被用户缩放污染。
    if (const char* scaleTest = std::getenv("CONCORD_ONSCREEN_ATTEST_SCALE")) {
        gAttestScaleTest = scaleTest[0] == '1';
    }
    std::fseek(gAttestLog, 0, SEEK_END);
    if (std::ftell(gAttestLog) == 0) {
        std::fprintf(gAttestLog,
            "# ON-3-01 present self-attestation: layer.contents re-read, RGB 24-bit FNV-1a vs same-frame DIB\n");
    }
    std::fprintf(stderr, "[attest] log=%s\n", path.c_str());
}

void attestPresent(const concord_compat::apple::Surface& surface) {
    attestEnsureOpen();
    if (gAttestState != 1) return;
    @autoreleasepool {
        CGImageRef attached = (__bridge CGImageRef)gView.layer.contents;
        const bool attachedMatchesSurface = attached != nullptr
            && CGImageGetWidth(attached) == static_cast<std::size_t>(surface.width)
            && CGImageGetHeight(attached) == static_cast<std::size_t>(surface.height);
        std::uint32_t layerHash = 0;
        const std::uint32_t dibHash = attestHash24(
            surface.pixels, surface.width, surface.height, surface.bytesPerRow);
        // 重绘固定**不翻转** CTM（理由见文件头：翻转会让整幅上下颠倒，实测
        // hash 由 0x674d476c 变成 0x890043ac）。另取一条不经重绘的交叉证据：
        // CGImage 的 provider 数据本身。两条都与 dibHash 并列写进日志，
        // 对不上时能立刻分辨是"重绘解释错"还是"贴的根本不是这帧"。
        const auto drawHash = [&]() -> std::uint32_t {
            std::vector<unsigned char> canvas(
                static_cast<std::size_t>(surface.bytesPerRow) * static_cast<std::size_t>(surface.height), 0);
            CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
            CGContextRef context = CGBitmapContextCreate(canvas.data(),
                static_cast<std::size_t>(surface.width), static_cast<std::size_t>(surface.height),
                8, static_cast<std::size_t>(surface.bytesPerRow), colorSpace, kSurfaceBitmapInfo);
            CGColorSpaceRelease(colorSpace);
            if (context == nullptr) return 0xFFFFFFFFu;
            CGContextSetBlendMode(context, kCGBlendModeCopy);
            CGContextDrawImage(context,
                CGRectMake(0, 0, static_cast<CGFloat>(surface.width), static_cast<CGFloat>(surface.height)),
                attached);
            CGContextRelease(context);
            return attestHash24(canvas.data(), surface.width, surface.height, surface.bytesPerRow);
        };
        std::uint32_t providerHash = 0;
        if (attachedMatchesSurface) {
            layerHash = drawHash();
            if (CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(attached))) {
                providerHash = attestHash24(CFDataGetBytePtr(data), surface.width, surface.height,
                    static_cast<int>(CGImageGetBytesPerRow(attached)));
                CFRelease(data);
            }
        }
        const bool match = attachedMatchesSurface && layerHash == dibHash;
        // surface.visible=false 是 SW_HIDE 语义，此时窗口本就不该可见；
        // 只在"要求可见"时断言可见，避免把隐藏帧记成失败。
        const bool visibleOk = !surface.visible || gWindow.isVisible;
        ++gAttestRecords;
        if (!match || !visibleOk) ++gAttestFailures;
        std::fprintf(gAttestLog,
            "idx=%d size=%dx%d title=\"%s\" layerHash=0x%08x dibHash=0x%08x"
            " providerHash=0x%08x match=%s"
            " visible=%d key=%d scale=%.1f frame=%.0fx%.0f layerContents=%s\n",
            gAttestRecords, surface.width, surface.height,
            surface.titleUtf8 != nullptr ? surface.titleUtf8 : "",
            layerHash, dibHash, providerHash, match ? "yes" : "no",
            gWindow.isVisible ? 1 : 0, gWindow.isKeyWindow ? 1 : 0,
            gWindow.backingScaleFactor,
            gWindow.frame.size.width, gWindow.frame.size.height,
            gView.layer.contents != nil ? "set" : "nil");
        std::fflush(gAttestLog);

        // ON-2-03 缩放验收（CONCORD_ONSCREEN_ATTEST_SCALE=1）。程序化改 frame 等价于
        // 用户拖动：变的是 window.frame，surface 不变。四项断言各自对应一个可能的
        // 回归——scale 掉回 1.0（画面糊）、view 没跟着变大（没铺满）、内容被重采样
        // （hash 变）、呈现尺寸被用户缩放污染（下次换界面被掰回）。
        // 只做一次：每帧放大 1.7 倍会让窗口尺寸指数爆炸（1024 → 1741 → 2960 …）。
        // 注意"换界面时 setContentSize 覆盖用户缩放"这一条**没有**在本进程里触发：
        // 各入口的 surface 尺寸在单次运行内是固定的（20 帧全是 800x600），要等
        // ON-3-01b 的多界面脚本才测得到。这里只能钉住它的前提——
        // presentedSizeUnchanged：用户缩放不得污染 gPresentedWidth/Height。
        if (gAttestScaleTest && !gScaleTestDone && attachedMatchesSurface) {
            gScaleTestDone = true;
            const NSRect before = gWindow.frame;
            [gWindow setFrame:NSMakeRect(before.origin.x, before.origin.y,
                before.size.width * 1.7, before.size.height * 1.7) display:YES];
            [gView layoutSubtreeIfNeeded];
            const NSSize viewSize = gView.bounds.size;
            const CGFloat scale = gView.layer.contentsScale;
            const bool scaleOk = std::fabs(scale - gWindow.backingScaleFactor) < 0.001;
            const bool filled = viewSize.width > surface.width * 1.5
                && viewSize.height > surface.height * 1.5;
            const bool contentUnchanged = drawHash() == dibHash;
            const char* gravity = [gView.layer.contentsGravity isEqualToString:kCAGravityResize]
                ? "resize" : "other";
            const bool presentedUnchanged = gPresentedWidth == surface.width
                && gPresentedHeight == surface.height;
            const bool ok = scaleOk && filled && contentUnchanged && presentedUnchanged;
            if (!ok) ++gAttestFailures;
            std::fprintf(gAttestLog,
                "# scale-test frame=%.0fx%.0f view=%.0fx%.0f contentsScale=%.1f"
                " backing=%.1f gravity=%s"
                " contentUnchanged=%s presentedSizeUnchanged=%s result=%s\n",
                gWindow.frame.size.width, gWindow.frame.size.height,
                viewSize.width, viewSize.height, static_cast<double>(scale),
                static_cast<double>(gWindow.backingScaleFactor), gravity,
                contentUnchanged ? "yes" : "no", presentedUnchanged ? "yes" : "no",
                ok ? "pass" : "fail");
            std::fflush(gAttestLog);
        }

        if (gAttestRecords >= gAttestLimit) {
            // 就在 present 里退出，不排队等 pumpEvents：世界视图的主循环不经过
            // 那条路径（实测首帧之后 pumpEvents 不再被调用，进程一直挂着）。
            // _Exit 不跑 static 析构：本项目句柄表/菜单静态对象在退出期有自己的坑
            // （rules.handle_table_outlives_process），取证轮不该引入 SIGABRT。
            // 日志上面已 fflush，记录不会丢。
            // 退出码必须带上失败数：_Exit(0) 会把 match=no 掩盖成"成功"。
            std::fprintf(stderr, "[attest] done: records=%d failures=%d limit=%d\n",
                gAttestRecords, gAttestFailures, gAttestLimit);
            std::_Exit(gAttestFailures == 0 ? 0 : 1);
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// ON-2-04 红叉：把 AppKit 的"关窗口"翻译成一次真正的 Win32 关闭
// ---------------------------------------------------------------------------
// 不实现 windowShouldClose: 时，点红叉的结果是 **AppKit 自己 orderOut**：
// 窗口从屏幕上消失，而 compat 的 GetMessageW 完全不知道这件事（它只认 Win32 消息），
// 于是进程继续跑、主循环继续转——用户看到的就是"点了红叉关不掉"。
//
// 这里拦下来、返回 NO（不让 AppKit 关），改发一个 Close 事件交给 compat 侧投
// WM_CLOSE。之后 WM_CLOSE → DestroyWindow → WM_DESTROY 全是既有的 Win32 链路，
// 与鼠标键盘走同一条通道，边界上不新增任何约定。
//
// 放在匿名 namespace 外面：ObjC 类不能在 C++ namespace 里定义。
// 但它仍然能直接引用 gInputSink / gInputContext——匿名 namespace 的成员
// 在同一个 TU 的文件作用域内是可见的。
@interface ConcordWindowCloseDelegate : NSObject <NSWindowDelegate>
@end

// U-03：游戏窗口必须拿到"激活窗口的那一次点击"。
//
// NSView 默认 acceptsFirstMouse: 返回 NO —— 窗口从非激活变激活时，系统会把这次
// 点击当成"激活动作"吃掉，不派发给 view。对普通文档应用这是体贴，对游戏就是
// "点了没反应，非得再点一下"，而且第二下才开始有反馈，很容易被误判成输入没打通。
// U-05：不可见光标（单例）。
// 不用 +[NSCursor invisibleCursor]：本 SDK 里它不可见（编译报 "no known class
// method for selector 'invisibleCursor'"）。用 1×1 空图自建，各版本 SDK 都稳定。
static NSCursor* concordInvisibleCursor() {
    static NSCursor* cursor = nil;
    if (cursor == nil) {
        NSImage* blank = [[NSImage alloc] initWithSize:NSMakeSize(1.0, 1.0)];
        cursor = [[NSCursor alloc] initWithImage:blank hotSpot:NSZeroPoint];
    }
    return cursor;
}

@interface ConcordGameView : NSView
@end

// U-04：是否在窗口区域隐藏系统指针。默认隐藏——游戏自己画箭头（Windows 上也是
// 只有游戏指针）。声明在 .mm 文件作用域，ObjC 方法可以直接读。
static bool gSystemCursorHidden = true;

@implementation ConcordGameView
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}

// U-04：游戏自绘指针时把系统指针换成不可见光标。
//
// 用 cursor rect 而不是 [NSCursor hide]/unhide：cursor rect 由窗口按区域管理，
// 鼠标移出窗口自动恢复系统箭头，不需要配对调用；[NSCursor hide] 是全局计数，
// 一旦某条路径漏了 unhide（窗口失焦、Cmd+Tab、异常退出），指针会永久消失，
// 用户只能强杀应用。
- (void)resetCursorRects {
    [super resetCursorRects];
    if (!gSystemCursorHidden) return;
    if (concordInvisibleCursor() != nil)
        [self addCursorRect:self.bounds cursor:concordInvisibleCursor()];
}
@end

@implementation ConcordWindowCloseDelegate
// U-05：窗口成为 key 时**重新**挂一次 cursor rect。
//
// 为什么需要这一步：cursor rect 是 AppKit 的窗口级状态，窗口激活/失焦/重新
// orderFront 时可能被整体丢弃，之后若没人 invalidate，AppKit 就退回默认箭头。
// 症状极具迷惑性——菜单阶段正常（那时窗口早已是 key），进世界后（窗口被 resize、
// 重新 orderFront）又冒出系统箭头，看起来像"隐藏策略在世界里失效"。
- (void)windowDidBecomeKey:(NSNotification*)note {
    (void)note;
    if (gWindow == nil || gView == nil) return;
    [gWindow invalidateCursorRectsForView:gView];
}

// U-07：失焦时清掉修饰键。否则按住 Ctrl 用 Cmd+Tab 切走、或点到别的窗口后
// 松开 Ctrl，本进程收不到 KeyUp，Ctrl 会一直"卡住"——之后所有点击都变成
// Ctrl+点击（切换选择/强制攻击），看起来像选择功能坏了。
- (void)windowDidResignKey:(NSNotification*)note {
    (void)note;
    concord_compat::clearModifierKeyDown();
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (gInputSink == nullptr) {
        // 纯呈现、没接输入的场合（离屏 smoke）：没有接收方就退回 AppKit 默认行为，
        // 否则窗口会变成关不掉的死窗口。
        return YES;
    }
    concord_compat::apple::InputEvent event;
    event.kind = concord_compat::apple::InputEvent::Kind::Close;
    gInputSink(event, gInputContext);
    return NO;
}

// ON-2-04 全屏自测的分段推进。两段都只做"检查 + 打日志 + 决定下一步"，
// 不在这里改窗口状态以外的任何东西。
- (void)checkFullScreenState:(const char*)phase {
    CGImageRef attached = (__bridge CGImageRef)gView.layer.contents;
    const bool contentsKept = attached != nullptr;
    const bool sizeKept = contentsKept
        && CGImageGetWidth(attached) == static_cast<std::size_t>(gPresentedWidth)
        && CGImageGetHeight(attached) == static_cast<std::size_t>(gPresentedHeight);
    const bool hashKept = gLastPresentHash == gHashBeforeFullScreen;
    if (!contentsKept || !sizeKept || !hashKept) ++gFullScreenFailures;
    std::fprintf(stderr,
        "[fullscreen] %s window=%.0fx%.0f view=%.0fx%.0f contents=%s size=%s"
        " hash=%u/%u before=%u ok=%d\n",
        phase,
        gWindow.frame.size.width, gWindow.frame.size.height,
        gView.frame.size.width, gView.frame.size.height,
        contentsKept ? "kept" : "lost",
        sizeKept ? "kept" : "changed",
        gLastPresentHash, gHashBeforeFullScreen, gHashBeforeFullScreen,
        (!contentsKept || !sizeKept || !hashKept) ? 0 : 1);
}

- (void)windowWillEnterFullScreen:(NSNotification*)note {
    (void)note;
    // 诊断探针：区分"AppKit 根本没开始切换"和"开始了但动画没走完"。
    // 前者说明 toggleFullScreen 被忽略（窗口/collectionBehavior/run loop），
    // 后者说明状态机在推进，只是没人喂 run loop。
    std::fprintf(stderr, "[fullscreen] willEnter styleMask=0x%lx\n",
                 static_cast<unsigned long>(gWindow.styleMask));
    gFullScreenTransition = true;
    gFullScreenStarted = true;
}

- (void)windowDidEnterFullScreen:(NSNotification*)note {
    (void)note;
    gFullScreenTransition = false;
    if (!gFullScreenTest) return;
    gHashAfterEnter = gLastPresentHash;
    [self checkFullScreenState:"entered"];
    // 立刻退回：自测要的是"进出都不丢"，只停在进入态证明不了一半。
    [gWindow toggleFullScreen:nil];
}

- (void)windowWillExitFullScreen:(NSNotification*)note {
    (void)note;
    gFullScreenTransition = true;
}

- (void)windowDidExitFullScreen:(NSNotification*)note {
    (void)note;
    gFullScreenTransition = false;
    if (!gFullScreenTest) return;
    gHashAfterExit = gLastPresentHash;
    [self checkFullScreenState:"exited"];
    std::fprintf(stderr,
        "[fullscreen] done: enter=%u exit=%u before=%u failures=%d\n",
        gHashAfterEnter, gHashAfterExit, gHashBeforeFullScreen, gFullScreenFailures);
    // 与 attest 同样的退出纪律：退出码必须带失败数，否则 fail 会被掩盖成成功。
    std::fflush(stderr);
    std::_Exit(gFullScreenFailures == 0 ? 0 : 1);
}
@end

// NSWindow.delegate 是 weak 引用：不在这里持有，delegate 会在 autoreleasepool
// 退出后被回收，红叉就会静默回到"AppKit 自己 orderOut"的老行为。
static ConcordWindowCloseDelegate* gWindowCloseDelegate = nil;

namespace concord_compat::apple {

void present(const Surface& surface) {
    if (surface.pixels == nullptr || surface.width <= 0 || surface.height <= 0) return;

    @autoreleasepool {
        if (!gApplicationReady) {
            // NSApplication 必须先于任何 NSWindow 创建。activationPolicy=Regular
            // 才能出现在 Dock 里并接收键盘焦点；Accessory 会让窗口浮着但收不到键。
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            // ON-1-03：菜单栏必须真的有一条 App 菜单，且其中的「退出」项要挂着
            // Cmd+Q 的键等价物。此前这里只 setMainMenu: 了一个空 NSMenu——
            // AppKit 的警告没了，但 Cmd+Q 依然没有对应的菜单项可命中，
            // 于是"按 Cmd+Q 没反应"会被误当成键盘事件没接上。
            NSMenu* menuBar = [[NSMenu alloc] init];
            NSMenuItem* applicationItem = [[NSMenuItem alloc] init];
            [menuBar addItem:applicationItem];
            NSMenu* applicationMenu = [[NSMenu alloc] init];
            NSString* applicationName = [[NSProcessInfo processInfo] processName];
            NSMenuItem* quitItem = [[NSMenuItem alloc]
                initWithTitle:[NSString stringWithFormat:@"Quit %@", applicationName]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
            // 不设 target：走响应链直到 NSApp，terminate: 由 NSApplication 处理，
            // 进程随后以 0 退出。
            [applicationMenu addItem:quitItem];
            [applicationItem setSubmenu:applicationMenu];
            [NSApp setMainMenu:menuBar];
            gApplicationReady = true;
        }

        if (gWindow == nil) {
            const NSRect contentRect =
                NSMakeRect(0, 0, surface.width, surface.height);
            // ON-2-03：Resizable 是用户决策（缩放取"任意拉伸铺满"，不做整数倍 letterbox）。
            // 加了这个位之后，用户拖动窗口改变的是 window.frame，**不改变 surface**，
            // 所以下面那个"surface 变了才 setContentSize"的回写不会被拖动触发——
            // 两者本来就走不同的判据。千万别在 windowDidResize 里补一句回写，
            // 那会把用户拖出来的尺寸掰回上一次呈现的尺寸。
            const NSWindowStyleMask style = NSWindowStyleMaskTitled
                | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                | NSWindowStyleMaskResizable;
            gWindow = [[NSWindow alloc] initWithContentRect:contentRect
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
            // ON-2-04：**刻意不设** NSWindowCollectionBehaviorFullScreenPrimary。
            //
            // 设了之后绿灯会变成"进入全屏"，而本项目的系统全屏是**走不完**的：
            // 实测 windowWillEnterFullScreen 触发了、windowDidEnterFullScreen 永远不到，
            // 哪怕把 pumpEvents 的 run loop 时间从 2ms 放宽到 20ms 也一样。
            // 根因是切换依赖 [NSApp run] 的完整生命周期（Space 切换 + 动画状态机），
            // 而本项目的主循环是 compat 自己的 GetMessageW，从不跑 NSApp run ——
            // 那是整个兼容层的前提，不能为一个窗口形态去动它。
            //
            // 不设这一位时绿灯是 "zoom"（同步 setFrame，纯窗口操作，不需要 run loop）。
            // 但实测 NSWindowCollectionBehaviorDefault **挡不住**：全屏照样开始
            // （willEnter 仍然触发），照样走不完。必须用 FullScreenNone 显式禁止，
            // 否则用户按 Ctrl+Cmd+F 会卡在半途。
            // "系统全屏"因此不可用，已单独记为 ON-2-04b（blocked），见 queue。
            gWindow.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
            // releasedWhenClosed=NO：窗口关闭后我们仍要能重用它，
            // 否则第二次 ShowWindow 会往一个已释放的对象发消息。
            gWindow.releasedWhenClosed = NO;
            // ON-2-04：delegate 必须在 makeKeyAndOrderFront 之前挂上，
            // 否则第一帧窗口已经是 key 了才有"关窗口"的可能。
            if (gWindowCloseDelegate == nil) {
                gWindowCloseDelegate = [[ConcordWindowCloseDelegate alloc] init];
            }
            gWindow.delegate = gWindowCloseDelegate;
            const char* title = surface.titleUtf8;
            // ON-2-05：空白首绘被过滤之后，**第一个真正有内容的窗口通常是主菜单的
            // 子窗口**，而子窗口没有标题（titleUtf8 为空）。此时若退回进程名，用户
            // 看到的就是"ConcordRTS"而不是游戏名——标题栏是唯一立刻可见的交付细节，
            // 不该因为呈现顺序变化而退化。所以空标题时用游戏名兜底。
            gWindow.title = (title != nullptr && title[0] != '\0')
                ? [NSString stringWithUTF8String:title]
                : @"Command & Conquer: Yuri's Revenge";

            gView = [[ConcordGameView alloc] initWithFrame:contentRect];
            gView.wantsLayer = YES;
            // U-03：不打开这一位，AppKit 只在按键按住时才派发 mouseDragged，
            // 平时根本没有 mouseMoved —— 产品的 WM_MOUSEMOVE 就永远不来，
            // 悬停高亮、tooltip、地图边沿滚动全部失灵（点击本身仍能用，因为
            // NSEvent 自带坐标，所以更容易被误判成"只有悬停坏了"）。
            gWindow.acceptsMouseMovedEvents = YES;
            // Resize 让画面随窗口拉伸，而不是居中留黑边——游戏窗口是
            // "把客户区铺满"的语义，不是"按原始尺寸展示图片"。
            gView.layer.contentsGravity = kCAGravityResize;
            gView.layer.magnificationFilter = kCAFilterNearest;
            // ON-2-03：contentsScale 必须跟随 backing scale，否则在 2x 屏上
            // 一张 1024x640 的图像会被当 1x 内容放进 2048x1280 的 backing store，
            // 由 CoreAnimation 再放大一倍——画面能看，但每个游戏像素糊成 4 个。
            // （实测这台机器 backingScaleFactor=2.0 而 layer 默认 1.0。）
            gView.layer.contentsScale = gWindow.backingScaleFactor;
            gWindow.contentView = gView;
            // U-04：系统指针策略。默认隐藏（与 Windows 上"只有游戏指针"一致）；
            // CONCORD_HIDE_SYSTEM_CURSOR=0 可恢复系统箭头，留给"某界面没有自绘指针"。
            // 这里主动失效一次 cursor rect，否则要等用户移动鼠标才生效。
            {
                static const bool hideSystemCursor = []() {
                    const char* value = std::getenv("CONCORD_HIDE_SYSTEM_CURSOR");
                    return value == nullptr || value[0] != '0';
                }();
                gSystemCursorHidden = hideSystemCursor;
                [gWindow invalidateCursorRectsForView:gView];
            }

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

        // ON-2-03 补丁：contentsScale 不能只在建窗口时抓一次。
        //
        // 实测（arm64，无人值守运行，2026-11-11）：同一个进程里
        // `layer.contentsScale=2.0` 而 `window.backingScaleFactor=1.0`，
        // scale-test 因此报 fail。原因就是上面那句只在创建时赋值——窗口的
        // backing 属性会变（系统状态/显示器切换/启动方式都会影响），
        // 而 layer 停在旧值上，两者一旦脱节，画面要么糊要么错。
        //
        // 每帧同步一次，代价只是一个浮点比较。disableActions 压掉隐式动画，
        // 否则改 contentsScale 会带出一次淡入（与 ON-2-05 的每帧贴图同理）。
        const CGFloat backingScale = gWindow.backingScaleFactor;
        if (gView.layer.contentsScale != backingScale) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            gView.layer.contentsScale = backingScale;
            [CATransaction commit];
            std::fprintf(stderr, "[cocoa] contentsScale -> %.1f (backing changed)\n",
                         static_cast<double>(backingScale));
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
            // ON-2-05：显式包一层 CATransaction 并提交。
            //
            // 这是"窗口在、标题对、客户区全黑"的根因。Cocoa 的隐式事务提交挂在
            // NSRunLoop 的 beforeWaiting/exit observer 上，而本项目**从不跑**
            // [NSApp run]——主循环是 compat 自己的 GetMessageW，只在 pumpEvents
            // 里用 2ms 有界超时调 nextEventMatchingMask。那个调用确实会进 run loop
            // （S12 那次 distantPast 阻塞在 mach_msg 就是证据），但 2ms 常常还没
            // 走到 beforeWaiting 就返回了，于是 layer 树改了一直没被提交到
            // render server：日志打出 "first frame attached"，屏幕上一个像素都没有。
            //
            // 显式 commit 立刻把事务推给 render server，不依赖 run loop 的节奏。
            // disableActions 是为了压掉 contents 变更的隐式 0.25s 淡入动画——
            // 游戏每帧都要换内容，带动画会让画面持续落后一到两帧。
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            gView.layer.contents = (__bridge id)image;
            [CATransaction commit];

            // 首帧诊断：一条日志就能区分三种"黑屏"——view 尺寸为 0（没布局）、
            // layer 为 nil（wantsLayer 没生效）、contents 为 nil（事务没提交）。
            // 注意必须在 CGImageRelease 之前读尺寸，否则是 use-after-free。
            static long long presentIndex = 0;
            ++presentIndex;
            if (presentIndex <= 2 || presentIndex == 30) {
                const NSRect viewBounds = gView.bounds;
                const CGRect layerBounds = gView.layer.bounds;
                std::fprintf(
                    stderr,
                    "[cocoa] frame %lld: view=%.0fx%.0f layer=%.0fx%.0f contents=%s"
                    " scale=%.1f win=%.0fx%.0f img=%zux%zu\n",
                    presentIndex,
                    viewBounds.size.width, viewBounds.size.height,
                    layerBounds.size.width, layerBounds.size.height,
                    gView.layer.contents != nil ? "set" : "nil",
                    gWindow.backingScaleFactor,
                    gWindow.frame.size.width, gWindow.frame.size.height,
                    CGImageGetWidth(image), CGImageGetHeight(image));
            }


            // ON-2-05 二分诊断（CONCORD_PRESENT_DIAG=1 才开，生产零开销）：
            //
            // "客户区全黑"有三条互斥的可能，一条日志就能分开：
            //   A. DIB 里本来就是黑的        → px0 全 0
            //   B. CGImage 把 BGRA 解释错了   → px0 有值但 cgProbe 全 0
            //   C. 图像正确但 layer 没被合成 → px0/cgProbe 都有值，屏幕仍黑
            //
            // 另外把 layer 底色刷成洋红：截图里出现洋红说明 layer 渲染链路是通的，
            // 那黑的就是 contents 本身；仍然全黑则说明问题在 NSView/NSWindow 这一层。
            static const bool presentDiag = []() {
                const char* value = std::getenv("CONCORD_PRESENT_DIAG");
                return value != nullptr && value[0] == '1';
            }();
            if (presentDiag && presentIndex <= 2) {
                const auto* bytes = static_cast<const unsigned char*>(surface.pixels);
                std::fprintf(stderr, "[cocoa] dib px0=%02x %02x %02x %02x\n",
                             bytes[0], bytes[1], bytes[2], bytes[3]);
                unsigned char probe[4] = {0, 0, 0, 0};
                CGContextRef probeContext = CGBitmapContextCreate(
                    probe, 1, 1, 8, 4, colorSpace, kSurfaceBitmapInfo);
                if (probeContext != nullptr) {
                    CGContextDrawImage(probeContext, CGRectMake(0, 0, 1, 1), image);
                    CGContextRelease(probeContext);
                    std::fprintf(stderr, "[cocoa] cg  px0=%02x %02x %02x %02x\n",
                                 probe[0], probe[1], probe[2], probe[3]);
                }
                gView.layer.backgroundColor = [NSColor magentaColor].CGColor;
                // 第二帧把 contents 摘掉，只留洋红底：截图上出现洋红就证明
                // layer 渲染链路是通的，黑的是 contents 本身；仍然全黑则说明
                // 问题在 NSView/NSWindow 这一层（view 没进窗口树 / 窗口没被合成）。
                if (presentIndex == 2) {
                    [CATransaction begin];
                    [CATransaction setDisableActions:YES];
                    gView.layer.contents = nil;
                    [CATransaction commit];
                    std::fprintf(stderr, "[cocoa] diag: contents cleared, magenta only\n");
                }
                std::fprintf(stderr, "[cocoa] diag: layer backgroundColor=magenta\n");
            }
            CGImageRelease(image);
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

        // ON-3-01：可见性状态已经更新，此时取证才拿到真实窗口状态。
        // 放在 CGImageRelease 之后也没问题——attestPresent 从 layer.contents
        // 读回图像对象，不复用这一帧的局部 image 引用。
        attestPresent(surface);

        // ON-1-03 自测注入（见 gQuitSelfTestPosted 的注释）。只在窗口建好之后
        // 投一次：performKeyEquivalent 需要 windowNumber 才认键。
        if (!gQuitSelfTestPosted) {
            gQuitSelfTestPosted = true;
            const char* selftest = std::getenv("CONCORD_QUIT_SELFTEST");
            if (selftest != nullptr && selftest[0] == '1') {
                NSEvent* synthetic = [NSEvent
                    keyEventWithType:NSEventTypeKeyDown
                          location:NSZeroPoint
                     modifierFlags:NSEventModifierFlagCommand
                         timestamp:[[NSProcessInfo processInfo] systemUptime]
                      windowNumber:gWindow.windowNumber
                           context:nil
                        characters:@"q"
       charactersIgnoringModifiers:@"q"
                         isARepeat:NO
                           keyCode:0x0C];  // kVK_ANSI_Q
                [NSApp postEvent:synthetic atStart:NO];
                std::fprintf(stderr, "[cocoa] quit self-test: posted synthetic Cmd+Q\n");
            }
        }

        // ON-2-04 自测注入：与 Cmd+Q 的注入并列，但**不要同时开**——两者都会结束进程。
        // 放在 present 末尾：此时本帧的贴图/取证都已完成，销毁窗口不会打断正在进行的绘制。
        //
        // 必须等 gInputSink 就绪：compat 侧的 setInputSink 是在 present **返回之后**
        // 才调用的（它要用刚呈现的那个窗口做 context）。第一次 present 时 sink 还是空，
        // delegate 会走"没人接就放行"的分支——实测就是窗口被 AppKit 关掉、进程继续跑，
        // 而日志里 close self-test 已经打出来了，看着像"注入成功但没效果"。
        if (!gCloseSelfTestDone && gInputSink != nullptr) {
            gCloseSelfTestDone = true;
            const char* closeTest = std::getenv("CONCORD_CLOSE_SELFTEST");
            if (closeTest != nullptr && closeTest[0] == '1') {
                std::fprintf(stderr, "[cocoa] close self-test: performClose\n");
                // 走真实路径：performClose: → windowShouldClose: → Close 事件
                // → compat 投 WM_CLOSE → DestroyWindow → WM_DESTROY → PostQuitMessage。
                [gWindow performClose:nil];
            }
        }

        // ON-2-04 全屏自测：同样等 sink 就绪（理由同 close selftest），
        // 请求进入全屏后就不再做任何事——后面全靠 delegate 的两个回调推进。
        if (!gFullScreenRequested && gInputSink != nullptr) {
            gFullScreenRequested = true;
            const char* fsTest = std::getenv("CONCORD_FULLSCREEN_SELFTEST");
            if (fsTest != nullptr && fsTest[0] == '1') {
                gFullScreenTest = true;
                gHashBeforeFullScreen = attestHash24(
                    surface.pixels, surface.width, surface.height, surface.bytesPerRow);
                gLastPresentHash = gHashBeforeFullScreen;
                std::fprintf(stderr, "[fullscreen] request toggleFullScreen hash=%u\n",
                             gHashBeforeFullScreen);
                [gWindow toggleFullScreen:nil];
            }
        }

        // 全屏期间若游戏自己重绘，把最新帧的 hash 记下来供回调比对。
        // 同一画面重绘 hash 不变，所以判据仍然成立；画面真的变了反而该报警。
        if (gFullScreenTest) {
            gLastPresentHash = attestHash24(
                surface.pixels, surface.width, surface.height, surface.bytesPerRow);
            // 兜底：既没 willEnter 也没有任何回调，说明这个窗口形态在此架构下不存在。
            // 报结论并正常退出（退出码 0），不把它伪装成"卡住"。
            if (!gFullScreenStarted && ++gFullScreenTicks > 60) {
                std::fprintf(stderr,
                    "[fullscreen] unsupported: no windowWillEnterFullScreen after %d frames"
                    " (needs [NSApp run]; see ON-2-04b)\n", gFullScreenTicks);
                std::fflush(stderr);
                std::_Exit(0);
            }
        }
    }
}

int onscreenAttestFailures() { return gAttestFailures; }
int onscreenAttestRecords() { return gAttestRecords; }

void shutdown() {
    @autoreleasepool {
        // 只 flush 不 close：一个 smoke 入口里可能连续跑多个界面，中间会
        // shutdown/重建窗口；关掉文件会让后半个进程静默停止取证。
        // 进程结束由 OS 回收这个 FILE*（与 compat 句柄表同样的取舍）。
        if (gAttestLog != nullptr) std::fflush(gAttestLog);
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
//
// F1..F12（U-07）：不是"顺手补全"，而是绑定表里就有——
//   View1..View4      = 112..115 = VK_F1..VK_F4   （跳视角）
//   SetView1..SetView4= Ctrl+F1..F4                （存视角）
//   Taunt_1..Taunt_8  = 116..123 = VK_F5..VK_F12  （多人语音/嘲讽）
// 这些键在 characters 分支里拿不到可打印字符，表里没有就会被 `keyCode==0` 丢掉，
// 表现为"F 键完全没反应"。注意系统可能把 F1/F2 抢去调亮度，那属于用户设置，不是本层问题。
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
        case 0x7A: return 0x70;  // F1
        case 0x78: return 0x71;  // F2
        case 0x63: return 0x72;  // F3
        case 0x76: return 0x73;  // F4
        case 0x60: return 0x74;  // F5
        case 0x61: return 0x75;  // F6
        case 0x62: return 0x76;  // F7
        case 0x64: return 0x77;  // F8
        case 0x65: return 0x78;  // F9
        case 0x6D: return 0x79;  // F10
        case 0x67: return 0x7A;  // F11
        case 0x6F: return 0x7B;  // F12
        default:   return 0;
    }
}

// U-07：修饰键状态必须从**每一个**事件同步。
//
// macOS 的修饰键不产生 KeyDown/KeyUp，只产生 FlagsChanged；而 Ctrl/Shift/Alt
// 同时又会挂在鼠标事件上。只处理 FlagsChanged 会漏掉"从别的应用的按键状态切
// 回来"这种情况，所以按事件携带的 modifierFlags 全量覆盖一遍——它是当前真实
// 状态的快照，不是增量。
//
// 这里只翻成抽象位；AppKit 常量 → VK_* 的映射在 compat 层内部（那边有 windows.h）。
static void syncModifierState(NSEventModifierFlags flags) {
    unsigned mask = 0;
    if ((flags & NSEventModifierFlagShift) != 0) mask |= concord_compat::kModifierShift;
    if ((flags & NSEventModifierFlagControl) != 0) mask |= concord_compat::kModifierControl;
    if ((flags & NSEventModifierFlagOption) != 0) mask |= concord_compat::kModifierAlt;
    if ((flags & NSEventModifierFlagCapsLock) != 0) mask |= concord_compat::kModifierCapsLock;
    concord_compat::setModifierKeyState(mask);
}

void emit(NSEvent* event) {
    if (gInputSink == nullptr || gView == nil) return;
    // 先同步修饰键，再翻译事件：窗口过程处理这条消息时要读到正确的 Ctrl/Shift。
    syncModifierState(event.modifierFlags);

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
        // **不要**用 [NSDate distantPast] 当"立刻返回"。
        // 文档说它不阻塞，实测会进 AppKit 的事件循环（采样栈：nextEventMatchingMask →
        // _DPSNextEvent → _DPSBlockUntilNextEventMatchingListInMode → mach_msg），
        // 也就是**真阻塞**。后果极隐蔽：compat 的 GetMessageW 在取不到消息时会调
        // pumpEvents 再睡，一旦这里卡住，WM_TIMER 就再也不会派发——产品的定时器
        // 驱动的菜单收尾直接死等（遭遇战 preload smoke 卡在这里，30s 后被杀）。
        //
        // 用"现在 + 2ms"这种**有界**超时：最坏多等 2ms，但绝不会无限挂。
        // 取不到事件就 break，等价于非阻塞；取到就一次排空。
        //
        // 例外：全屏切换期间放宽到 20ms（见 gFullScreenTransition 的注释）。
        // 那段时间需要的不是"取事件"，而是"让 AppKit 的动画状态机有 run loop 可用"，
        // 2ms 的碎片推进走不完，didEnter 永远不来。
        const NSTimeInterval timeout = gFullScreenTransition ? 0.02 : 0.002;
        for (;;) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate dateWithTimeIntervalSinceNow:timeout]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event == nil) break;
            // AppKit 内部事件（NSEventTypeAppKitDefined）不是用户输入，直接丢。
            // 红叉**不在这条路径上**处理：它由 ConcordWindowCloseDelegate 的
            // windowShouldClose: 拦下并发出 Close 事件（ON-2-04）。这一行此前
            // 写着"窗口关闭按钮走标准关闭路径：转成 quit"，那句话是错的——
            // 它的实际作用是丢弃，而 AppKit 自己在没人拦的情况下 orderOut，
            // 于是"点了红叉窗口消失、进程还在"。注释与代码脱节就是这么埋出来的。
            if (event.type == NSEventTypeAppKitDefined) continue;
            if (event.window != nil && event.window != gWindow) continue;
            // ON-1-03：菜单键等价物（Cmd+Q）必须自己派发。本进程没有 [NSApp run]，
            // AppKit 不会替我们跑这一步，事件只会躺在队列里——这正是上一版
            // "Cmd+Q 没反应"的真因（当时还以为要配 mainMenu，其实配了也没人派发）。
            if (event.type == NSEventTypeKeyDown
                && [[NSApp mainMenu] performKeyEquivalent:event]) {
                continue;
            }
            // U-05：每个输入事件都重新压一次不可见光标。
            //
            // cursor rect 是窗口级状态，窗口激活/失焦/重新 orderFront 时可能被
            // 整体丢掉；丢掉之后没人 invalidate 就会退回系统箭头，而用户看到的
            // 就是"世界里又冒出第二个指针"。现场没有可靠的补回时机，所以按
            // 游戏窗口的常规做法：每次事件主动 set 一次（代价可忽略）。
            if (gSystemCursorHidden) {
                NSCursor* hiddenCursor = concordInvisibleCursor();
                if (hiddenCursor != nil) [hiddenCursor set];
            }
            emit(event);
        }
    }
}

}  // namespace concord_compat::apple

namespace concord_compat::apple {

// U-04：系统指针策略。
//
// 默认隐藏：游戏自己画箭头（Windows 上硬件指针本就不显示，玩家只看到一个）。
// CONCORD_HIDE_SYSTEM_CURSOR=0 可关掉——留给"某个界面确实没有自绘指针"的情况，
// 例如加载页万一不画光标，用户至少还能看见系统箭头。
void setSystemCursorHidden(bool hidden) {
    gSystemCursorHidden = hidden;
    @autoreleasepool {
        if (gWindow != nil && gView != nil) {
            // 让窗口重新询问 resetCursorRects，立即生效（不必等鼠标移动）。
            [gWindow invalidateCursorRectsForView:gView];
        }
    }
}

bool systemCursorHidden() { return gSystemCursorHidden; }

}  // namespace concord_compat::apple
