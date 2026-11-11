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
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

namespace {

NSWindow* gWindow = nil;
NSView* gView = nil;
bool gApplicationReady = false;

concord_compat::apple::InputSink gInputSink = nullptr;
void* gInputContext = nullptr;
bool gInputTrace = false;  // CONCORD_INPUT_TRACE=1 时打一行/事件，取证用

// U-14：全屏视口回调。全屏的正确语义是"客户区变大、单位尺寸不变"，
// 所以 .mm 要在进入/退出全屏时把新客户区尺寸交给 compat（见头文件说明）。
concord_compat::apple::ResizeSink gResizeSink = nullptr;
void* gResizeContext = nullptr;
// 进全屏之前的窗口内容区尺寸（点）。退出时按它恢复，而不是按当时的表面尺寸——
// 全屏期间表面已经被改成全屏大小了。
int gWindowedWidthBeforeFullScreen = 0;
int gWindowedHeightBeforeFullScreen = 0;
// 自测判据：全屏期间是否至少有一帧「呈现表面尺寸 == 内容区尺寸」。
// 这一条才是"没有拉伸放大"的证据：相等说明游戏按全屏尺寸重绘了 1:1 的画面，
// 不相等说明屏幕上是把一张小图拉伸铺满（单位被放大，看不到更多地图）。
bool gFullScreenCoverageOk = false;
long long gFullScreenCoverageFrames = 0;

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

// L10N-03：真·点击路径的自测开关（CONCORD_CLOSECLICK_SELFTEST=1）。与上面的
// CONCORD_CLOSE_SELFTEST 互补：那边直接调 performClose:，只测 performClose 之后的
// 链路；这边合成一对 LeftMouseDown/Up 投进**真实事件队列**，连 emit() 里的
// 红叉命中判定一起测。红叉点击没反应（用户 2026-11-11 报）就是这一环断了。
bool gCloseClickSelfTestPosted = false;

// L10N-03：红叉按下武装标记。down 在按钮上时吞掉（游戏不能看到标题栏上的
// 点击），up 回到按钮上才触发 performClose:；up 落在按钮外则取消——与
// macOS 系统按钮"按下高亮、抬起在内才生效"的语义一致，也避免游戏收到
// 没有配对 down 的悬空 up。
bool gCloseButtonArmed = false;

// 本轮（U-09 复发）：绿灯按下武装标记，语义与 gCloseButtonArmed 完全相同。
// 起因：U-09 修 collectionBehavior 之后，自测用 performClick: 触发——那是
// 程序化直调 action，**绕过了真实鼠标事件链**（本项目不调 sendEvent，真实
// 点击到不了任何标题栏按钮）。于是「绿灯接在全屏上」只对程序成立，用户点
// 真实鼠标依旧没反应。修法与红叉（L10N-03）同构：事件泵里显式命中。
bool gZoomButtonArmed = false;

// ON-2-04 全屏自测（CONCORD_FULLSCREEN_SELFTEST=1）。
// 全屏切换是**异步**的（有系统动画），不能用"发完 toggleFullScreen 立刻检查"来验，
// 必须在 windowDidEnterFullScreen / windowDidExitFullScreen 回调里逐段推进：
//     present → toggleFullScreen → didEnter（检查一次，再 toggle 退回）→ didExit（检查并收尾）
// 判据是"内容没丢"，不是"窗口变大了"：全屏期间游戏通常不重绘，
// 所以要比对的是 layer.contents 还在不在、CGImage 尺寸是否仍等于呈现尺寸、
// 以及最后一次 present 的内容 hash 是否没变。
bool gFullScreenTest = false;
bool gFullScreenRequested = false;
// 已进入全屏、等待在主循环里发起退回（见 windowDidEnterFullScreen 的注释）。
bool gFullScreenExitPending = false;
// 当前是否处于全屏；以及全屏期间又呈现了多少帧（证明呈现路径没断）。
bool gFullScreenEntered = false;
int gFramesInFullScreen = 0;
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
// U-09：自测要走**用户那条路**——点绿灯，而不是直接调 toggleFullScreen:。
// 两者不等价：toggleFullScreen: 只验"窗口能不能全屏"，点绿灯验的是"绿灯是不是
// 接在全屏上"（用户报障的正是后者）。performClick 之后若若干帧内没有任何
// willEnter，再退回 toggleFullScreen: 并**如实打印**是哪条路生效的。
bool gFullScreenViaButton = true;
bool gFullScreenButtonTried = false;
// 绿灯语义（可全屏 vs 只能缩放）是否已经对了。这是用户报障的那一条本身，
// 所以它算失败，不只是日志。
bool gFullScreenAffordanceOk = false;

// 全屏自测的触发时机。默认第一帧（拿到 input sink 之后立刻）——
// 但"全屏里画面有没有铺满"这条只在**世界画面**上成立：菜单阶段触发，全屏的是
// 1024x640 的菜单壳，之后进世界时 present 会按新表面尺寸 setContentSize，
// 把窗口从全屏里踢出来，测到的其实是窗口态。CONCORD_FULLSCREEN_SELFTEST_DELAY
// 让自测等 N 帧再点绿灯，就能停在世界画面里验。
long long gPresentCount = 0;
// 退出全屏后由下一帧 present 在主循环栈里把窗口恢复成表面尺寸（见 didExit 注释）。
bool gRestoreWindowAfterFullScreen = false;
static long long fullScreenSelftestDelayFrames() {
    static const long long frames = []() {
        const char* value = std::getenv("CONCORD_FULLSCREEN_SELFTEST_DELAY");
        return value != nullptr ? std::atoll(value) : 0LL;
    }();
    return frames;
}

// CONCORD_PRESENT_GEOM=1：每 60 帧打一次窗口/view/layer 的几何。
// 用来回答"进全屏后画面为什么只在左上角"——是 view 没变大、layer 没跟着变大，
// 还是两者都对但 contents 没被拉伸（gravity 被改 / 内容尺寸没变）。
// CONCORD_FULLSCREEN_STAY 的取值：'1' 表示一直停在全屏；N>1 表示在全屏里停留
// N 帧后自动退回（用来验"退出全屏后窗口有没有恢复成表面尺寸"）。
static long long fullScreenStayFrames() {
    static const long long frames = []() {
        const char* value = std::getenv("CONCORD_FULLSCREEN_STAY");
        if (value == nullptr) return 0LL;
        const long long parsed = std::atoll(value);
        return parsed > 1 ? parsed : 0LL;
    }();
    return frames;
}
static bool presentGeomEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("CONCORD_PRESENT_GEOM");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// U-09：鼠标坐标从 view 空间反算回**呈现表面**空间。
//
// 画面是按 contentsGravity=Resize 铺满整个 view 的，所以 view 的尺寸和呈现表面
// 的尺寸是两件事：窗口被放大（用户拖窗口，或系统全屏）之后 view 是 1512x949，
// 而游戏表面仍然是 800x600。原先把 local 坐标直接当客户区坐标交给游戏，
// 只有"view 恰好等于表面"时才对——一旦缩放，点击位置与画面对不上：
// 全屏时点屏幕正中会报 (756,474)，游戏在 800x600 里命中，永远打空。
// （这条在 ON-2-03 放开任意拉伸时就已经存在，只是那时窗口默认尺寸等于表面，
//   没人踩到；全屏把它变成必现，所以在 U-09 一起修。）
// 映射与 CoreAnimation 的 Resize 语义严格互逆：按比例缩放到表面尺寸并夹在
// [0, 表面尺寸-1]，越界的边缘点击不会被丢，而是贴到最外侧一个像素。
// y 同时在这里翻转：AppKit 原点在左下，客户区 DIB 原点在左上，不翻会出现
// 点击垂直镜像（菜单上点"单人游戏"命中"退出"）。
static void mapViewPointToSurface(double viewX, double viewY, int& outX, int& outY) {
    const double viewW = gView != nil ? gView.bounds.size.width : 0.0;
    const double viewH = gView != nil ? gView.bounds.size.height : 0.0;
    const double surfaceW = gPresentedWidth > 0 ? static_cast<double>(gPresentedWidth) : viewW;
    const double surfaceH = gPresentedHeight > 0 ? static_cast<double>(gPresentedHeight) : viewH;
    if (viewW <= 0.0 || viewH <= 0.0 || surfaceW <= 0.0 || surfaceH <= 0.0) {
        outX = 0;
        outY = 0;
        return;
    }
    const double scaleX = surfaceW / viewW;
    const double scaleY = surfaceH / viewH;
    long px = std::lround(viewX * scaleX);
    long py = std::lround((viewH - viewY) * scaleY);
    px = std::min(std::max(px, 0L), static_cast<long>(surfaceW) - 1);
    py = std::min(std::max(py, 0L), static_cast<long>(surfaceH) - 1);
    outX = static_cast<int>(px);
    outY = static_cast<int>(py);
}

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
// U-10 系统指针可见性自证（CONCORD_CURSOR_SELFTEST=1）
//
// 用户报障：「鼠标在 app 框内隐藏了系统图标，移出去就看不到了」。
// 判据用 +[NSCursor currentSystemCursor]——它返回系统**此刻正在显示**的游标：
// 我们把自己的 1×1 空图设上去之后，这里读到的就是 1×1。
//   · 比截图靠谱：截图要把光标画进去得靠 screencapture -C，而且光标位置随时会
//     被真实鼠标移走（实测两次采样之间能差 300px）；
//   · 比 CGCursorIsVisible 准确：那个只反映 CGDisplayHideCursor 那套引用计数，
//     而"把光标换成透明图"在它眼里仍然是"可见"。
// 断言两条：窗口内必须是空图（隐藏生效）；窗口外必须不是空图（移出即恢复）。
// ---------------------------------------------------------------------------
int gCursorSelfTestStage = 0;
// 收敛器的调用计数与最近一次判定（诊断用：区分"没被调用"和"判成不该隐藏"）。
int gCursorSyncCalls = 0;
bool gCursorWantedBlank = false;

// U-11 指针捕获的三态（见 syncGlobalCursorForPointer 后面的长注释）。
enum PointerState { kPointerFree = 0, kPointerCaptured = 1, kPointerReleased = 2 };
int gPointerState = kPointerFree;
// 捕获态的虚拟指针（呈现表面坐标）。硬件指针被冻结，游戏光标靠 delta 推这一个。
double gVirtualPointerX = 0.0;
double gVirtualPointerY = 0.0;
// ⌃⌘ 组合只响应一次（按住不放会连续来 FlagsChanged）。
bool gCaptureChordSeen = false;
// 捕获自检的阶段机（CONCORD_CAPTURE_SELFTEST=1）。
double gCaptureAnchorX = 0.0;
double gCaptureAnchorY = 0.0;
int gCaptureSelfTestStage = 0;
int gCaptureSelfTestTicks = 0;
int gCaptureSelfTestFailures = 0;
int gCursorSelfTestTicks = 0;
int gCursorSelfTestFailures = 0;
int gCursorSelfTestSkips = 0;

// 自检输出双写：stderr（直接跑二进制时能看见）+ /tmp 下的固定文件。
// 后者是因为用 LaunchServices 启动（open）的进程 stderr 直接进 /dev/null，
// 而"窗口是不是 key"恰恰只有在真实启动路径下才有意义——两种启动方式都要能取证。
static void cursorSelfTestLog(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    std::fputs(buffer, stderr);
    std::fflush(stderr);
    static std::FILE* file = std::fopen("/tmp/concord-cursor-selftest.log", "a");
    if (file != nullptr) {
        std::fputs(buffer, file);
        std::fflush(file);
    }
}

static NSString* describeCurrentSystemCursor() {
    NSCursor* cur = [NSCursor currentSystemCursor];
    if (cur == nil) return @"(nil)";
    const NSSize size = cur.image.size;
    return [NSString stringWithFormat:@"%.0fx%.0f", size.width, size.height];
}

// 空光标 = 我们自己造的那个 1×1 透明图（concordInvisibleCursor）。
static bool systemCursorIsBlank() {
    NSCursor* cur = [NSCursor currentSystemCursor];
    if (cur == nil) return false;
    const NSSize size = cur.image.size;
    return size.width <= 1.0 && size.height <= 1.0;
}

// AppKit 的屏幕坐标原点在左下，CGWarpMouseCursorPosition 用左上原点。
static void warpPointerToScreenPoint(double nsX, double nsY) {
    const NSRect screen = [[NSScreen mainScreen] frame];
    CGWarpMouseCursorPosition(CGPointMake(nsX, screen.size.height - nsY));
}

static NSPoint currentPointerScreenPoint() {
    const NSRect screen = [[NSScreen mainScreen] frame];
    const CGPoint p = CGEventGetLocation(CGEventCreate(nullptr));
    return NSMakePoint(p.x, screen.size.height - p.y);
}


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

// U-09：绿灯到底是什么，进程内自证。
//
// 用户报障是"绿灯显示一个 +，不是全屏"。那个图标不是我们画的，是 AppKit 按窗口
// **是否可全屏**挑的：可全屏画展开箭头，只能缩放画 +。判据因此拆成两层：
//
//   ① 形态：collectionBehavior 带 FullScreenPrimary + styleMask 可缩放。
//      这两项是我们能控制的输入，也是 AppKit 决定图标的依据。
//   ② 行为：**点绿灯**真的进了全屏（见 present() 里的 performClick）。
//      这一层比图标本身更硬——只能缩放的窗口，点绿灯永远进不了全屏。
//
// 本来还想直接读绿灯的 AX 子角色（AXFullScreenButton vs AXZoomButton，就是图标
// 的判定源头），但实测在本进程里 `zoom.accessibilitySubrole` 返回 nil：AX 元素
// 是懒创建的，没有 AX 客户端连上来就查不到。外部 AX 查询又走不通（本机
// Accessibility 未授权，System Events 直接 -10004 权限违例）。所以子角色只作为
// 诊断打印，不参与判定；判定靠上面两层。
static bool reportFullScreenAffordance() {
    NSButton* zoom = [gWindow standardWindowButton:NSWindowZoomButton];
    // 用 NSAccessibility 协议方法而不是已弃用的 accessibilityAttributeValue:，
    // 否则每轮构建都带一条 deprecation 警告，久了就没人再看构建输出。
    NSAccessibilitySubrole subrole = nil;
    if (zoom != nil) {
        subrole = zoom.accessibilitySubrole;
    }
    const bool fullScreenBehaviour =
        (gWindow.collectionBehavior & NSWindowCollectionBehaviorFullScreenPrimary) != 0;
    const bool resizable = (gWindow.styleMask & NSWindowStyleMaskResizable) != 0;
    const bool buttonSaysFullScreen =
        subrole != nil && [subrole isEqualToString:@"AXFullScreenButton"];
    const bool buttonSaysZoom =
        subrole != nil && [subrole isEqualToString:@"AXZoomButton"];
    const bool ok = fullScreenBehaviour && resizable;
    std::fprintf(stderr,
        "[fullscreen] affordance collection=fullScreenPrimary:%d resizable:%d"
        " greenButton=%s subrole=%s fullScreenButton:%d zoomButton:%d ok=%d\n",
        fullScreenBehaviour ? 1 : 0,
        resizable ? 1 : 0,
        zoom != nil ? "present" : "missing",
        subrole != nil ? [subrole UTF8String] : "(nil, AX 未接线，仅诊断)",
        buttonSaysFullScreen ? 1 : 0,
        buttonSaysZoom ? 1 : 0,
        ok ? 1 : 0);
    return ok;
}

// U-11：delegate 里要用（失焦放人），声明放在这里免得调用点看不见。
namespace concord_compat::apple {
static void capturePointer();
static void releasePointer();
static NSPoint screenPointForSurfacePoint(double surfaceX, double surfaceY);
static void advanceVirtualPointer(double dx, double dy);
static void ensurePointerAssociationRestored();
}  // namespace concord_compat::apple

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
    // U-11：捕获态下失焦必须放人。否则用户在别的应用里会遇到"鼠标不能动"，
    // 而他根本不知道是被谁抓的。
    if (gPointerState == kPointerCaptured) concord_compat::apple::releasePointer();
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

// 全屏自测的分段推进。只做"检查 + 打日志 + 决定下一步"，不改窗口状态以外的任何东西。
//
// 判据（U-09 修订）：内容还在 + 呈现对象尺寸没变 + 全屏期间确实又呈现过帧。
//
// 为什么去掉原来的"hash 必须等于进入前"：那条判据假设"全屏期间游戏不重绘"，
// 但遭遇战界面一直在动（光标闪烁、背景动画），hash 必然变——实测进出两段都报 ok=0，
// 而画面其实完全正常。把 hash 留在日志里当诊断，不再参与判定。
// 真正要防的是"贴上去的图没了/被换成别的尺寸"，也就是 contents 与 size 两项；
// 再补一条"全屏期间有 present"来证明呈现路径没断（这条在 didExit 才有意义）。
- (void)checkFullScreenState:(const char*)phase {
    CGImageRef attached = (__bridge CGImageRef)gView.layer.contents;
    const bool contentsKept = attached != nullptr;
    const bool sizeKept = contentsKept
        && CGImageGetWidth(attached) == static_cast<std::size_t>(gPresentedWidth)
        && CGImageGetHeight(attached) == static_cast<std::size_t>(gPresentedHeight);
    const bool presentedWhileFullScreen =
        std::strcmp(phase, "entered") == 0 || gFramesInFullScreen > 0;
    // 全屏期间 view 必须和窗口一样大（全屏窗口没有标题栏，frame 就是内容尺寸）。
    // 这一条是 2026-11-11「全屏后画面只在左上角」的判据：present() 曾经在表面
    // 尺寸变化时按表面尺寸 setContentSize + 改 view.frame，窗口仍被系统撑满、
    // view 却被钉回 1024x640，画面缩在左上角、右下全黑。contents 与 size 两项
    // 都"正常"，只有这一条能抓出来。
    const bool windowFullScreen =
        (gWindow.styleMask & NSWindowStyleMaskFullScreen) != 0;
    const bool viewTracksWindow = !windowFullScreen
        || (std::fabs(gView.frame.size.width - gWindow.frame.size.width) < 1.0
            && std::fabs(gView.frame.size.height - gWindow.frame.size.height) < 1.0);
    // U-14：还要求"全屏期间至少出现过一帧 表面尺寸 == 内容区尺寸"。
    // 少了这一条，"画面拉伸铺满"和"客户区真的变大了"在日志上一模一样——
    // 这正是用户第一次报障时判据全部报 ok=1 的原因。
    const bool coverageRequired = std::strcmp(phase, "exited") == 0;
    const bool coverage = !coverageRequired || gFullScreenCoverageOk;
    const bool ok = contentsKept && sizeKept && presentedWhileFullScreen
        && viewTracksWindow && coverage;
    if (!ok) ++gFullScreenFailures;
    std::fprintf(stderr,
        "[fullscreen] %s window=%.0fx%.0f view=%.0fx%.0f contents=%s size=%s"
        " viewTracksWindow=%d coverage=%d"
        " framesInFullScreen=%d hash=%u (before=%u, diagnostic only) ok=%d\n",
        phase,
        gWindow.frame.size.width, gWindow.frame.size.height,
        gView.frame.size.width, gView.frame.size.height,
        contentsKept ? "kept" : "lost",
        sizeKept ? "kept" : "changed",
        viewTracksWindow ? 1 : 0,
        gFullScreenCoverageOk ? 1 : 0,
        gFramesInFullScreen,
        gLastPresentHash, gHashBeforeFullScreen,
        ok ? 1 : 0);
}

- (void)windowWillEnterFullScreen:(NSNotification*)note {
    (void)note;
    // 诊断探针：区分"AppKit 根本没开始切换"和"开始了但动画没走完"。
    // 前者说明 toggleFullScreen 被忽略（窗口/collectionBehavior/run loop），
    // 后者说明状态机在推进，只是没人喂 run loop。
    std::fprintf(stderr, "[fullscreen] willEnter styleMask=0x%lx\n",
                 static_cast<unsigned long>(gWindow.styleMask));
    // U-14：此刻窗口还是窗口态，正是记录"进全屏前客户区多大"的唯一时机。
    const NSRect contentBefore = gView != nil
        ? gView.bounds
        : NSMakeRect(0, 0, gPresentedWidth, gPresentedHeight);
    gWindowedWidthBeforeFullScreen =
        static_cast<int>(std::lround(contentBefore.size.width));
    gWindowedHeightBeforeFullScreen =
        static_cast<int>(std::lround(contentBefore.size.height));
    gFullScreenTransition = true;
    gFullScreenStarted = true;
}

// U-14：全屏 = 更大的客户区，而不是把一张小图放大铺满。
//
// 位置说明：调用点在 windowDidEnterFullScreen 里、`if (!gFullScreenTest) return`
// **之前**——这是产品行为，不是自测行为。compat 收到尺寸后改自己的窗口矩形，
// 游戏下一帧就按新客户区建后缓冲、重算地图视口，于是屏幕上出现的是
// 「同样大小的单位 + 更多地图」，而不是「同一张画面被放大 1.76 倍」。
- (void)applyFullScreenViewport {
    if (gResizeSink == nullptr) {
        // 纯呈现场景（离屏 smoke / 没有接输入）：没有人能重排，保持原样即可。
        return;
    }
    const NSSize content = gView != nil
        ? gView.bounds.size
        : [gWindow contentLayoutRect].size;
    const int width = static_cast<int>(std::lround(content.width));
    const int height = static_cast<int>(std::lround(content.height));
    if (width <= 0 || height <= 0) return;
    gFullScreenCoverageFrames = 0;
    gResizeSink(width, height, gResizeContext);
    std::fprintf(stderr,
        "[fullscreen] viewport -> %dx%d: game client takes the fullscreen size"
        " (units stay 1:1, more map visible)\n", width, height);
}

- (void)windowDidEnterFullScreen:(NSNotification*)note {
    (void)note;
    gFullScreenTransition = false;
    gFullScreenEntered = true;
    [self applyFullScreenViewport];
    if (!gFullScreenTest) return;
    gHashAfterEnter = gLastPresentHash;
    [self checkFullScreenState:"entered"];
    // 全屏之后还能不能操作：view 尺寸变了（1512x949），但游戏表面还是 800x600，
    // 鼠标必须按比例反算回去。这里拿 view 正中当例子——若没缩放，正中的点击会
    // 报成表面的右下角附近，游戏里点什么都点不到。
    {
        const double viewW = gView.bounds.size.width;
        const double viewH = gView.bounds.size.height;
        int mappedX = 0, mappedY = 0;
        mapViewPointToSurface(viewW / 2.0, viewH / 2.0, mappedX, mappedY);
        const int expectX = gPresentedWidth / 2;
        const int expectY = gPresentedHeight / 2;
        const bool mapOk = std::abs(mappedX - expectX) <= 1 && std::abs(mappedY - expectY) <= 1;
        if (!mapOk) ++gFullScreenFailures;
        std::fprintf(stderr,
            "[fullscreen] pointer map view=%.0fx%.0f center->%d,%d expect=%d,%d ok=%d\n",
            viewW, viewH, mappedX, mappedY, expectX, expectY, mapOk ? 1 : 0);
    }
    // 退回**不能**在这里发。
    // 实测：在 windowDidEnterFullScreen 的通知栈里再调一次 toggleFullScreen 会被
    // AppKit 丢掉——日志停在 "entered"，之后既没有 willExit 也没有 didExit，
    // 进程带着一个全屏窗口继续跑完整个 smoke。同一件事在主循环里做就没问题
    // （独立探针里 willEnter→didEnter→willExit→didExit 是连着的）。
    // 所以只置标志，由下一帧 present 在主循环栈里发起退回。
    //
    // CONCORD_FULLSCREEN_STAY=1：停在全屏不退回，给"全屏画面到底铺没铺满"这类
    // 必须用眼睛/抓图看的问题留一个可复现的现场（自测本身只测尺寸，测不出
    // "客户区里那块内容有没有被拉伸铺满"）。
    // CONCORD_FULLSCREEN_STAY：不设 = 进全屏后立刻退回（原行为）；
    // '1' = 一直停在全屏，给"画面到底铺没铺满"这类必须抓图看的问题留现场；
    // N>1 = 在全屏里停 N 帧再退回，用来验退出后窗口有没有恢复成表面尺寸。
    const char* stay = std::getenv("CONCORD_FULLSCREEN_STAY");
    if (stay == nullptr) {
        gFullScreenExitPending = true;
    } else if (stay[0] != '1') {
        // N>1 的场景由 present() 按帧数发起退回；N<=0 视为原行为。
        gFullScreenExitPending = fullScreenStayFrames() == 0;
    }
}

- (void)windowWillExitFullScreen:(NSNotification*)note {
    (void)note;
    gFullScreenTransition = true;
    gFullScreenEntered = false;
}

- (void)windowDidExitFullScreen:(NSNotification*)note {
    (void)note;
    gFullScreenTransition = false;
    // U-14：把客户区恢复成进全屏之前的大小。全屏期间游戏是按全屏尺寸建后缓冲的，
    // 不还原的话窗口退回窗口态、画面却还是全屏尺寸的一张图，被 Resize 压缩回
    // 1024x640——单位又变小了（和"放大"是同一个错误的反面）。
    // 这件事必须先于下面的 present 收尾：present 只负责 NSWindow 内容区尺寸，
    // 客户区尺寸（游戏那边）归 compat 管。
    if (gResizeSink != nullptr) {
        gResizeSink(0, 0, gResizeContext);
        std::fprintf(stderr, "[fullscreen] viewport -> restore %dx%d (windowed)\n",
                     gWindowedWidthBeforeFullScreen, gWindowedHeightBeforeFullScreen);
    }
    // 全屏期间刻意不改窗口几何（见 present() 的注释），所以退出后必须按**当前**
    // 表面尺寸把窗口恢复成窗口态：否则窗口停在屏幕大小，1024x640 的画面被
    // 拉伸成非 4:3。和 toggleFullScreen 一样，这件事不能在通知栈里做——
    // 交给下一帧 present 在主循环栈里收尾。
    gRestoreWindowAfterFullScreen = true;
    if (!gFullScreenTest) return;
    gHashAfterExit = gLastPresentHash;
    [self checkFullScreenState:"exited"];
    // 用户那条路必须自己走通：点了绿灯就进全屏，不能靠后面的 toggleFullScreen 兜底。
    // 若这条是 0，说明绿灯仍然不是"全屏按钮"（退回成缩放了），用户点了还是没用。
    const bool enteredViaButton = gFullScreenStarted && gFullScreenViaButton;
    if (!enteredViaButton) ++gFullScreenFailures;
    std::fprintf(stderr,
        "[fullscreen] done: enter=%u exit=%u before=%u affordance=%d viaButton=%d"
        " failures=%d\n",
        gHashAfterEnter, gHashAfterExit, gHashBeforeFullScreen,
        gFullScreenAffordanceOk ? 1 : 0, enteredViaButton ? 1 : 0,
        gFullScreenFailures);
    // 与 attest 同样的退出纪律：退出码必须带失败数，否则 fail 会被掩盖成成功。
    std::fflush(stderr);
    std::_Exit(gFullScreenFailures == 0 ? 0 : 1);
}
@end

// NSWindow.delegate 是 weak 引用：不在这里持有，delegate 会在 autoreleasepool
// 退出后被回收，红叉就会静默回到"AppKit 自己 orderOut"的老行为。
static ConcordWindowCloseDelegate* gWindowCloseDelegate = nil;

namespace concord_compat::apple {

// U-10：系统指针收敛器（定义在文件后部，pump 与 present 都要用）。
static void syncGlobalCursorForPointer();
// U-11：指针捕获（同上，emit/present/delegate 都要用）。
static void capturePointer();
static void releasePointer();
static void advanceVirtualPointer(double dx, double dy);
static void ensurePointerAssociationRestored();

void present(const Surface& surface) {
    if (surface.pixels == nullptr || surface.width <= 0 || surface.height <= 0) return;
    ++gPresentCount;

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
            // 全屏（U-09 重新定案）。
            //
            // ON-2-04 当初把这一位设成 FullScreenNone，理由是"系统全屏走不完：
            // willEnter 触发了、didEnter 永远不到"。U-09 复测推翻了这个结论：
            // 用独立探针（同一个 AppKit、不跑 [NSApp run]、只出队不 sendEvent、
            // Accessory 策略、2ms 切片）四种组合下 toggleFullScreen 全部
            // willEnter → didEnter → willExit → didExit 走完，frame 变成屏幕尺寸。
            // 也就是说"没有 [NSApp run]"不是障碍。
            //
            // 当时失败的真实前提是：ON-2-04 早于 U-01/U-02 —— 那两轮修之前，
            // 主循环里有大段路径**根本不 pumpEvents**（世界循环纯 PeekMessageW +
            // 裸 sleep_for，大厅循环走 MsgWaitForMultipleObjectsEx），全屏切换的
            // 状态机拿不到 run loop 时间，自然停在半途。现在 pump 已经在
            // GetMessageW / PeekMessageW / MsgWaitForMultipleObjectsEx 三处都接上了。
            //
            // 不用 FullScreenNone 还有第二个理由：那一档下绿灯是"缩放"，图标是
            // 一个「+」——用户看到的就是这个（U-09 报障）。FullScreenPrimary 才是
            // 系统认可的"可全屏"形态，绿灯显示展开箭头，Ctrl+Cmd+F 也能用。
            gWindow.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
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

            // U-11：关联开关是进程级的，异常退出也必须恢复，否则整机鼠标不能动。
            std::atexit(ensurePointerAssociationRestored);
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
        } else if (gRestoreWindowAfterFullScreen) {
            // 刚退出全屏：把窗口恢复成**进全屏之前**的尺寸。
            //
            // 不能拿 gPresented* 当依据（U-14 更正）：全屏期间游戏是按全屏尺寸
            // 重绘的，gPresented* 已经被改成 1800x1130 了，照它恢复等于没退出。
            // 进全屏前的内容区尺寸记在 gWindowed*BeforeFullScreen（willEnter 里）。
            gRestoreWindowAfterFullScreen = false;
            const int width = gWindowedWidthBeforeFullScreen > 0
                ? gWindowedWidthBeforeFullScreen : gPresentedWidth;
            const int height = gWindowedHeightBeforeFullScreen > 0
                ? gWindowedHeightBeforeFullScreen : gPresentedHeight;
            const NSRect contentRect = NSMakeRect(0, 0, width, height);
            [gWindow setContentSize:contentRect.size];
            gView.frame = contentRect;
            // 尺寸基线跟着回到窗口态：否则下一帧 surface 仍是全屏尺寸时会再走
            // 一次"尺寸变了"，把窗口重新撑大。
            gPresentedWidth = width;
            gPresentedHeight = height;
            std::fprintf(stderr, "[cocoa] left fullscreen: window back to %dx%d\n",
                         width, height);
        } else if (surface.width != gPresentedWidth || surface.height != gPresentedHeight) {
            // 换了界面尺寸：必须跟着改，否则小尺寸界面会被 contentsGravity=Resize
            // 拉伸铺满旧窗口——画面"能看"但比例是错的，而且这种错不会报任何东西。
            // 遭遇战菜单是 800x600、主菜单是 1024x640，走的就是这条路。
            //
            // **全屏期间绝不在这里改窗口/view 尺寸**（U-09/U-13 的教训）：
            // 全屏时窗口由系统接管成屏幕大小，这里若按表面尺寸 setContentSize +
            // 改 gView.frame，窗口仍是全屏、view 却被钉成表面尺寸贴左上角，
            // 右下大片黑。
            //
            // U-14 起全屏期间的表面尺寸变化是**正常的**：applyFullScreenViewport
            // 让游戏按全屏客户区重新建后缓冲，表面从 1024x640 变成 1800x1130，
            // 那时 surface 与 view 相等，画面是 1:1（单位不变、地图更多）。
            // 这里只需把基线跟上，别的都不做。
            const bool fullScreen =
                gFullScreenEntered || (gWindow.styleMask & NSWindowStyleMaskFullScreen) != 0;
            gPresentedWidth = surface.width;
            gPresentedHeight = surface.height;
            if (fullScreen) {
                const int viewWidth = static_cast<int>(std::lround(gView.bounds.size.width));
                const int viewHeight = static_cast<int>(std::lround(gView.bounds.size.height));
                // 尺寸相等 = 游戏已经按全屏客户区重绘（U-14 的目标形态）；
                // 不等 = 还在过渡帧，此刻屏幕上是把旧尺寸画面拉伸铺满。
                std::fprintf(stderr,
                    "[cocoa] surface changed to %dx%d while fullscreen: keep window geometry"
                    " view=%dx%d %s\n",
                    surface.width, surface.height, viewWidth, viewHeight,
                    (surface.width == viewWidth && surface.height == viewHeight)
                        ? "1:1" : "still scaled (waiting for repaint at fullscreen size)");
            } else {
                const NSRect contentRect = NSMakeRect(0, 0, surface.width, surface.height);
                [gWindow setContentSize:contentRect.size];
                gView.frame = contentRect;
                std::fprintf(stderr, "[cocoa] NSWindow resized to %dx%d\n",
                             surface.width, surface.height);
            }
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

            if (presentGeomEnabled() && gPresentCount % 60 == 0) {
                const NSRect viewBounds = gView.bounds;
                const CGRect layerBounds = gView.layer.bounds;
                const CGRect layerFrame = gView.layer.frame;
                NSString* gravity = gView.layer.contentsGravity;
                std::fprintf(stderr,
                    "[geom] frame=%lld win=%.0fx%.0f fullscreen=%d view=%.0fx%.0f"
                    " layer.bounds=%.0fx%.0f layer.frame=%.0fx%.0f gravity=%s scale=%.2f"
                    " img=%zux%zu\n",
                    gPresentCount,
                    gWindow.frame.size.width, gWindow.frame.size.height,
                    (gWindow.styleMask & NSWindowStyleMaskFullScreen) ? 1 : 0,
                    viewBounds.size.width, viewBounds.size.height,
                    layerBounds.size.width, layerBounds.size.height,
                    layerFrame.size.width, layerFrame.size.height,
                    gravity != nil ? [gravity UTF8String] : "(nil)",
                    static_cast<double>(gView.layer.contentsScale),
                    CGImageGetWidth(image), CGImageGetHeight(image));
            }

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

        // U-10：系统指针可见性自证。阶段机，每帧推进一步（等 cursor rect 生效
        // 需要几帧；所有等待都落在 present 里，不额外 sleep）。
        if (gCursorSelfTestStage == 0) {
            const char* cursorTest = std::getenv("CONCORD_CURSOR_SELFTEST");
            if (cursorTest != nullptr && cursorTest[0] == '1' && gInputSink != nullptr) {
                gCursorSelfTestStage = 1;
                gCursorSelfTestTicks = 0;
            }
        }
        if (gCursorSelfTestStage > 0) {
            ++gCursorSelfTestTicks;
            const NSRect frame = gWindow.frame;
            const NSRect screen = [[NSScreen mainScreen] frame];
            // 窗口外的一个点：优先取右边，越界取左边，都越界就贴屏幕右侧。
            double outsideX = NSMaxX(frame) + 60.0;
            if (outsideX > NSMaxX(screen) - 40.0) outsideX = NSMinX(frame) - 60.0;
            if (outsideX < NSMinX(screen) + 40.0) outsideX = NSMaxX(screen) - 60.0;
            const double outsideY = NSMidY(frame);

            if (gCursorSelfTestStage == 1 && gCursorSelfTestTicks > 3) {
                gCursorSelfTestTicks = 0;
                gCursorSelfTestStage = 2;
                warpPointerToScreenPoint(NSMidX(frame), NSMidY(frame));
            } else if (gCursorSelfTestStage == 2 && gCursorSelfTestTicks > 3) {
                const NSPoint pointer = currentPointerScreenPoint();
                const bool inside = NSPointInRect(pointer, frame);
                const bool blank = systemCursorIsBlank();
                // 判"在不在我们窗口上方"要用 WindowServer 的窗口序：指针在框内、
                // 但窗口被别的应用盖住时不应该隐藏指针，那种情况记为 skip。
                const NSInteger top =
                    [NSWindow windowNumberAtPoint:pointer belowWindowWithWindowNumber:0];
                const bool overUs = top == gWindow.windowNumber;
                if (!inside || !overUs) {
                    ++gCursorSelfTestSkips;
                } else if (!blank) {
                    ++gCursorSelfTestFailures;
                }
                cursorSelfTestLog(
                    "[cursor] inside pointer=%.0f,%.0f inWindow=%d overUs=%d top=%ld ours=%ld"
                    " cursor=%s hidden=%d sync=%d want=%d ok=%d\n",
                    pointer.x, pointer.y, inside ? 1 : 0, overUs ? 1 : 0,
                    (long)top, (long)gWindow.windowNumber,
                    [describeCurrentSystemCursor() UTF8String],
                    blank ? 1 : 0, gCursorSyncCalls, gCursorWantedBlank ? 1 : 0,
                    (!inside || !overUs || blank) ? 1 : 0);
                gCursorSelfTestTicks = 0;
                gCursorSelfTestStage = 3;
                warpPointerToScreenPoint(outsideX, outsideY);
            } else if (gCursorSelfTestStage == 3 && gCursorSelfTestTicks > 3) {
                const NSPoint pointer = currentPointerScreenPoint();
                const bool outside = !NSPointInRect(pointer, frame);
                const bool blank = systemCursorIsBlank();
                if (!outside) {
                    ++gCursorSelfTestSkips;
                } else if (blank) {
                    // 这就是用户报的那条：人已经离开窗口，系统指针还是那张空图。
                    ++gCursorSelfTestFailures;
                }
                cursorSelfTestLog(
                    "[cursor] outside pointer=%.0f,%.0f outsideWindow=%d cursor=%s hidden=%d ok=%d\n",
                    pointer.x, pointer.y, outside ? 1 : 0,
                    [describeCurrentSystemCursor() UTF8String],
                    blank ? 1 : 0, (!outside || !blank) ? 1 : 0);
                // L10N-02：第三态——标题栏。frame 高出内容的部分就是标题栏 +
                // 边框；拿不到（全屏/无边框，差值 < 8pt）就跳过这一态。
                const double titleBarHeight =
                    frame.size.height - gView.bounds.size.height;
                if (titleBarHeight < 8.0) {
                    ++gCursorSelfTestSkips;
                    cursorSelfTestLog("[cursor] titlebar skip: height=%.1f\n",
                                      titleBarHeight);
                    gCursorSelfTestStage = 5;
                } else {
                    gCursorSelfTestTicks = 0;
                    gCursorSelfTestStage = 4;
                    warpPointerToScreenPoint(
                        NSMidX(frame), NSMaxY(frame) - titleBarHeight / 2.0);
                }
            } else if (gCursorSelfTestStage == 4 && gCursorSelfTestTicks > 3) {
                const NSPoint pointer = currentPointerScreenPoint();
                const double titleBarHeight =
                    frame.size.height - gView.bounds.size.height;
                const bool overTitleBar = NSPointInRect(pointer, frame)
                    && pointer.y >= NSMaxY(frame) - titleBarHeight - 2.0;
                const bool blank = systemCursorIsBlank();
                if (!overTitleBar) {
                    ++gCursorSelfTestSkips;
                } else if (blank) {
                    // 用户 2026-11-11 报的正是这条：指针在标题栏上，系统箭头
                    // 被游戏藏掉了，没法拖动/关闭窗口。
                    ++gCursorSelfTestFailures;
                }
                cursorSelfTestLog(
                    "[cursor] titlebar pointer=%.0f,%.0f overTitleBar=%d titleH=%.1f"
                    " cursor=%s hidden=%d ok=%d\n",
                    pointer.x, pointer.y, overTitleBar ? 1 : 0, titleBarHeight,
                    [describeCurrentSystemCursor() UTF8String],
                    blank ? 1 : 0, (!overTitleBar || !blank) ? 1 : 0);
                cursorSelfTestLog("[cursor] done: failures=%d skips=%d\n",
                                  gCursorSelfTestFailures, gCursorSelfTestSkips);
                gCursorSelfTestStage = 5;
                std::fflush(stderr);
                std::_Exit(gCursorSelfTestFailures == 0 ? 0 : 1);
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

        // L10N-03 自测：真·点击路径。合成一对 LeftMouseDown/Up 投进真实事件
        // 队列，覆盖 emit() 的红叉命中判定 → performClose: → WM_CLOSE 全链。
        if (!gCloseClickSelfTestPosted && gInputSink != nullptr && gWindow != nil) {
            gCloseClickSelfTestPosted = true;
            const char* clickTest = std::getenv("CONCORD_CLOSECLICK_SELFTEST");
            if (clickTest != nullptr && clickTest[0] == '1') {
                NSButton* closeButton =
                    [gWindow standardWindowButton:NSWindowCloseButton];
                if (closeButton != nil) {
                    const NSRect r =
                        [closeButton convertRect:closeButton.bounds toView:nil];
                    const NSPoint center =
                        NSMakePoint(NSMidX(r), NSMidY(r));
                    const double now =
                        [[NSProcessInfo processInfo] systemUptime];
                    NSEvent* down = [NSEvent
                        mouseEventWithType:NSEventTypeLeftMouseDown
                                  location:center
                             modifierFlags:0
                                 timestamp:now
                              windowNumber:gWindow.windowNumber
                                   context:nil
                               eventNumber:0
                                clickCount:1
                                   pressure:1.0];
                    NSEvent* up = [NSEvent
                        mouseEventWithType:NSEventTypeLeftMouseUp
                                  location:center
                             modifierFlags:0
                                 timestamp:now + 0.05
                              windowNumber:gWindow.windowNumber
                                   context:nil
                               eventNumber:1
                                clickCount:1
                                   pressure:0.0];
                    [NSApp postEvent:down atStart:NO];
                    [NSApp postEvent:up atStart:NO];
                    std::fprintf(stderr,
                        "[closebutton] self-test: posted click at (%.0f,%.0f)\n",
                        center.x, center.y);
                } else {
                    std::fprintf(stderr,
                        "[closebutton] self-test: close button unavailable\n");
                }
            }
        }

        // ON-2-04 全屏自测：同样等 sink 就绪（理由同 close selftest），
        // 请求进入全屏后就不再做任何事——后面全靠 delegate 的两个回调推进。
        if (!gFullScreenRequested && gInputSink != nullptr
            && gPresentCount >= fullScreenSelftestDelayFrames()) {
            gFullScreenRequested = true;
            const char* fsTest = std::getenv("CONCORD_FULLSCREEN_SELFTEST");
            if (fsTest != nullptr && fsTest[0] == '1') {
                gFullScreenTest = true;
                gHashBeforeFullScreen = attestHash24(
                    surface.pixels, surface.width, surface.height, surface.bytesPerRow);
                gLastPresentHash = gHashBeforeFullScreen;
                gFullScreenAffordanceOk = reportFullScreenAffordance();
                if (!gFullScreenAffordanceOk) ++gFullScreenFailures;
                gFullScreenButtonTried = true;
                NSButton* zoom = [gWindow standardWindowButton:NSWindowZoomButton];
                std::fprintf(stderr, "[fullscreen] request via green button hash=%u\n",
                             gHashBeforeFullScreen);
                if (zoom != nil) {
                    // 本轮改动：不再 performClick:（程序化直调 action，绕过真实
                    // 鼠标事件链——用户报的正是「真实点击没反应」）。改为把一对
                    // LeftMouseDown/Up 合成事件投进**真实事件队列**，走 pumpEvents
                    // → emit() → 绿灯命中 → toggleFullScreen: 的完整用户路径，
                    // 与红叉自测（CONCORD_CLOSE_SELFTEST）同一套做法。
                    const NSRect r = [zoom convertRect:zoom.bounds toView:nil];
                    const NSPoint center = NSMakePoint(
                        NSMidX(r), NSMidY(r));
                    const NSTimeInterval now = [NSProcessInfo processInfo]
                        .systemUptime;
                    NSEvent* down = [NSEvent
                        mouseEventWithType:NSEventTypeLeftMouseDown
                                  location:center
                             modifierFlags:0
                                 timestamp:now
                              windowNumber:gWindow.windowNumber
                                   context:nil
                               eventNumber:0
                                clickCount:1
                                   pressure:1.0];
                    NSEvent* up = [NSEvent
                        mouseEventWithType:NSEventTypeLeftMouseUp
                                  location:center
                             modifierFlags:0
                                 timestamp:now + 0.05
                              windowNumber:gWindow.windowNumber
                                   context:nil
                               eventNumber:1
                                clickCount:1
                                   pressure:0.0];
                    [NSApp postEvent:down atStart:NO];
                    [NSApp postEvent:up atStart:NO];
                    std::fprintf(stderr,
                        "[fullscreen] posted green-button click at (%.0f,%.0f)\n",
                        center.x, center.y);
                } else {
                    gFullScreenViaButton = false;
                    std::fprintf(stderr,
                        "[fullscreen] no green button, fallback toggleFullScreen\n");
                    [gWindow toggleFullScreen:nil];
                }
            }
        }

        // 全屏期间若游戏自己重绘，把最新帧的 hash 记下来供回调比对。
        // 同一画面重绘 hash 不变，所以判据仍然成立；画面真的变了反而该报警。
        if (gFullScreenTest) {
            if (gFullScreenEntered) ++gFramesInFullScreen;
            // U-14 的核心判据：全屏期间出现「表面尺寸 == 内容区尺寸」的帧，
            // 说明游戏真的按全屏客户区重绘了 1:1 的画面（contentsGravity 无缩放）。
            // 用户第一次报障时结论是"拉伸铺满"——那时这一条永远不成立，
            // 而其它判据全都报 ok，所以必须单独记一条。
            if (gFullScreenEntered) {
                const int viewWidth = static_cast<int>(std::lround(gView.bounds.size.width));
                const int viewHeight = static_cast<int>(std::lround(gView.bounds.size.height));
                if (std::abs(surface.width - viewWidth) <= 1
                    && std::abs(surface.height - viewHeight) <= 1) {
                    ++gFullScreenCoverageFrames;
                    if (!gFullScreenCoverageOk) {
                        gFullScreenCoverageOk = true;
                        std::fprintf(stderr,
                            "[fullscreen] coverage: surface=%dx%d == view=%dx%d"
                            " after %lld frame(s) — 1:1, no scaling\n",
                            surface.width, surface.height, viewWidth, viewHeight,
                            gFullScreenCoverageFrames);
                    }
                }
            }
            gLastPresentHash = attestHash24(
                surface.pixels, surface.width, surface.height, surface.bytesPerRow);
            // 退回在**主循环栈**里发起（不能在 didEnter 通知栈里，见那边注释）。
            const long long stayFrames = fullScreenStayFrames();
            if (stayFrames > 0 && !gFullScreenTransition && !gFullScreenExitPending
                && gFramesInFullScreen >= stayFrames) {
                gFullScreenExitPending = true;
            }
            if (gFullScreenExitPending) {
                gFullScreenExitPending = false;
                std::fprintf(stderr, "[fullscreen] request exit from present()\n");
                [gWindow toggleFullScreen:nil];
            }
            // 兜底一：绿灯合成点击没人响应（真实事件链上的命中判定没生效，
            // 或 toggleFullScreen: 被窗口形态拒绝）。这时改用 toggleFullScreen:
            // 再试一次，把"哪条路生效"如实打出来——若只有兜底生效，说明
            // 用户点的绿灯仍然接不上全屏，要按断链追查。
            if (!gFullScreenStarted && gFullScreenViaButton && ++gFullScreenTicks > 8) {
                gFullScreenViaButton = false;
                std::fprintf(stderr,
                    "[fullscreen] green button produced no willEnter in %d frames,"
                    " falling back to toggleFullScreen\n", gFullScreenTicks);
                gFullScreenTicks = 0;
                [gWindow toggleFullScreen:nil];
            }
            // 兜底二：既没 willEnter 也没有任何回调，说明这个窗口形态在此架构下不存在。
            // 报结论并正常退出（退出码 0），不把它伪装成"卡住"。
            if (!gFullScreenStarted && !gFullScreenViaButton && ++gFullScreenTicks > 60) {
                std::fprintf(stderr,
                    "[fullscreen] unsupported: no windowWillEnterFullScreen after %d frames\n",
                    gFullScreenTicks);
                std::fflush(stderr);
                std::_Exit(0);
            }
        }

        // U-11：指针捕获自证（CONCORD_CAPTURE_SELFTEST=1）。
        // 判据只有一条物理事实：捕获之后 CGWarpMouseCursorPosition 推不动指针，
        // 释放之后又能推动。这比"看日志里有没有打印"硬得多——它证明的是
        // CGAssociateMouseAndMouseCursorPosition 真的生效且真的被恢复了。
        if (gCaptureSelfTestStage == 0) {
            const char* capTest = std::getenv("CONCORD_CAPTURE_SELFTEST");
            if (capTest != nullptr && capTest[0] == '1' && gInputSink != nullptr) {
                gCaptureSelfTestStage = 1;
                gCaptureSelfTestTicks = 0;
            }
        }
        if (gCaptureSelfTestStage > 0) {
            ++gCaptureSelfTestTicks;
            const double expectX = (gPresentedWidth > 0 ? gPresentedWidth : 2) / 2.0 + 37.0;
            const double expectY = (gPresentedHeight > 0 ? gPresentedHeight : 2) / 2.0 + 21.0;
            if (gCaptureSelfTestStage == 1 && gCaptureSelfTestTicks > 3) {
                capturePointer();
                gCaptureSelfTestTicks = 0;
                gCaptureSelfTestStage = 2;
            } else if (gCaptureSelfTestStage == 2 && gCaptureSelfTestTicks > 3) {
                // 捕获态下"指针不动"没法用 warp 验（warp 是显式定位，绕过关联开关）。
                // 能验的是我们自己那一半：delta 进来之后虚拟指针有没有按预期推进、
                // 并且被夹在表面内。这里投一条带 delta 的合成 mouseMoved 事件。
                CGEventRef cg = CGEventCreateMouseEvent(
                    nullptr, kCGEventMouseMoved, CGPointZero, kCGMouseButtonLeft);
                CGEventSetIntegerValueField(cg, kCGMouseEventDeltaX, 37);
                CGEventSetIntegerValueField(cg, kCGMouseEventDeltaY, -21);
                NSEvent* synthetic = [NSEvent eventWithCGEvent:cg];
                CFRelease(cg);
                [NSApp postEvent:synthetic atStart:NO];
                gCaptureSelfTestTicks = 0;
                gCaptureSelfTestStage = 3;
            } else if (gCaptureSelfTestStage == 3 && gCaptureSelfTestTicks > 6) {
                const bool advanced = std::fabs(gVirtualPointerX - expectX) < 1.5
                    && std::fabs(gVirtualPointerY - expectY) < 1.5;
                if (!advanced) ++gCaptureSelfTestFailures;
                cursorSelfTestLog(
                    "[capture] delta virtual=%.0f,%.0f expect=%.0f,%.0f ok=%d\n",
                    gVirtualPointerX, gVirtualPointerY, expectX, expectY,
                    advanced ? 1 : 0);
                // 释放：先把虚拟指针放回表面中心，再做位置断言（避免夹边干扰）
                gVirtualPointerX = (gPresentedWidth > 0 ? gPresentedWidth : 2) / 2.0;
                gVirtualPointerY = (gPresentedHeight > 0 ? gPresentedHeight : 2) / 2.0;
                releasePointer();
                const NSPoint want = screenPointForSurfacePoint(
                    gVirtualPointerX, gVirtualPointerY);
                gCaptureAnchorX = want.x;
                gCaptureAnchorY = want.y;
                gCaptureSelfTestTicks = 0;
                gCaptureSelfTestStage = 4;
            } else if (gCaptureSelfTestStage == 4 && gCaptureSelfTestTicks > 4) {
                const NSPoint now = currentPointerScreenPoint();
                const double dx = std::fabs(now.x - gCaptureAnchorX);
                const double dy = std::fabs(now.y - gCaptureAnchorY);
                // 释放应当把硬件指针"接"到游戏光标处；容差放宽到 4px（窗口/背板取整）
                const bool landed = dx <= 4.0 && dy <= 4.0;
                if (!landed) ++gCaptureSelfTestFailures;
                cursorSelfTestLog(
                    "[capture] release landed=%d want=%.0f,%.0f now=%.0f,%.0f state=%d ok=%d\n",
                    landed ? 1 : 0, gCaptureAnchorX, gCaptureAnchorY, now.x, now.y,
                    gPointerState, landed ? 1 : 0);
                cursorSelfTestLog("[capture] done: failures=%d\n", gCaptureSelfTestFailures);
                std::fflush(stderr);
                // _Exit 不跑 atexit：必须在这里自己恢复，否则自测失败时用户的
                // 鼠标会一直冻着（这正是本轮最怕的失效模式）。
                ensurePointerAssociationRestored();
                std::_Exit(gCaptureSelfTestFailures == 0 ? 0 : 1);
            }
        }

        // U-10：每帧都收敛一次系统指针。放在 present 末尾而不是只靠 pumpEvents——
        // 主菜单在 GetMessageW 上阻塞时，pump 可能几秒才转一次，指针离开窗口后
        // 空图会滞留；present 至少是"画面在动就一定会跑到"的地方。
        syncGlobalCursorForPointer();
    }
}

int onscreenAttestFailures() { return gAttestFailures; }
int onscreenAttestRecords() { return gAttestRecords; }

void shutdown() {
    @autoreleasepool {
        // U-11：收摊前必须把鼠标关联恢复，否则进程退出后整机指针被冻住。
        ensurePointerAssociationRestored();
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

void setResizeSink(ResizeSink sink, void* context) {
    gResizeSink = sink;
    gResizeContext = context;
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

    // U-11：⌃⌘ 组合先于一切处理——它是"把手从游戏里抽出来"的开关，不能先喂给游戏
    // （否则 Ctrl 会被当成强制攻击的修饰键按下去）。
    //   · 捕获态：只要出现 ⌃⌘ 就释放；
    //   · 自由态：⌃⌘G 进入捕获（单按 ⌃⌘ 不抓，避免和系统快捷键抢）。
    {
        const NSEventModifierFlags mods = event.modifierFlags;
        const bool both = (mods & NSEventModifierFlagControl) != 0
            && (mods & NSEventModifierFlagCommand) != 0;
        if (both) {
            if (gPointerState == kPointerCaptured) {
                releasePointer();
                gCaptureChordSeen = true;
                return;
            }
            if (!gCaptureChordSeen && event.type == NSEventTypeKeyDown
                && [[event.charactersIgnoringModifiers lowercaseString] isEqualToString:@"g"]) {
                gCaptureChordSeen = true;
                capturePointer();
                return;
            }
            gCaptureChordSeen = true;
            return;  // 其余 ⌃⌘ 组合仍然吞掉，别让游戏看到 Ctrl 按下
        }
        gCaptureChordSeen = false;
    }

    // L10N-03：标题栏红叉补命中。
    //
    // 本项目从不调用 [NSApp sendEvent:]（见 U-10 注释），AppKit 的按钮点击
    // 响应链从来没被喂过事件——红叉/最小化/缩放点了没反应（用户报的就是
    // 红叉）。不走 sendEvent 大手术，而是在事件泵里对"左键落在关闭按钮上"
    // 做显式命中 → performClose:。后续 windowShouldClose: → Close 事件 →
    // WM_CLOSE → 退出，与 CONCORD_CLOSE_SELFTEST 验证过的链路完全同一条。
    //
    // 只做红叉，不做最小化/缩放：没有 NSApp run 的架构下，窗口缩进 Dock 后
    // 收不到 Dock 图标的恢复点击，补了反而把窗口变成找不回来的死窗。
    if (gWindow != nil
        && (event.type == NSEventTypeLeftMouseDown
            || event.type == NSEventTypeLeftMouseUp)) {
        NSButton* closeButton = [gWindow standardWindowButton:NSWindowCloseButton];
        if (closeButton != nil && closeButton.window == gWindow) {
            const NSRect buttonInWindow =
                [closeButton convertRect:closeButton.bounds toView:nil];
            const NSPoint point = (event.window == gWindow)
                ? event.locationInWindow
                : [gWindow convertPointFromScreen:[NSEvent mouseLocation]];
            const bool inButton = NSPointInRect(point, buttonInWindow);
            if (event.type == NSEventTypeLeftMouseDown) {
                if (inButton) {
                    gCloseButtonArmed = true;
                    return;  // 吞掉：游戏不能看到标题栏上的按下
                }
            } else if (gCloseButtonArmed) {
                gCloseButtonArmed = false;
                if (inButton) {
                    std::fprintf(stderr, "[closebutton] click hit, performClose\n");
                    [gWindow performClose:nil];
                }
                return;  // 配对的 up 一并吞掉，无论是否命中
            }
        }
    }

    // 本轮（U-09 复发）：绿灯补命中——与上面的红叉同一个坑，同一种修法。
    // 命中即 toggleFullScreen:（进入/退出同一个动作，全屏态再点即退回）。
    // 不做最小化按钮，理由同红叉注释：缩进 Dock 后收不到恢复点击。
    if (gWindow != nil
        && (event.type == NSEventTypeLeftMouseDown
            || event.type == NSEventTypeLeftMouseUp)) {
        NSButton* zoomButton = [gWindow standardWindowButton:NSWindowZoomButton];
        if (zoomButton != nil && zoomButton.window == gWindow) {
            const NSRect buttonInWindow =
                [zoomButton convertRect:zoomButton.bounds toView:nil];
            const NSPoint point = (event.window == gWindow)
                ? event.locationInWindow
                : [gWindow convertPointFromScreen:[NSEvent mouseLocation]];
            const bool inButton = NSPointInRect(point, buttonInWindow);
            if (event.type == NSEventTypeLeftMouseDown) {
                if (inButton) {
                    gZoomButtonArmed = true;
                    return;  // 吞掉：游戏不能看到标题栏上的按下
                }
            } else if (gZoomButtonArmed) {
                gZoomButtonArmed = false;
                if (inButton) {
                    std::fprintf(stderr,
                        "[fullscreen] green button click hit, toggleFullScreen\n");
                    [gWindow toggleFullScreen:nil];
                }
                return;  // 配对的 up 一并吞掉，无论是否命中
            }
        }
    }

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

    // U-11：释放态下"点回游戏窗口"就自动再抓（VMware 同款直觉：点哪儿抓哪儿）。
    // 先把虚拟指针落到点击位置，再进捕获——否则会从表面中心跳一下。
    if (gPointerState == kPointerReleased
        && (event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeRightMouseDown)) {
        int clickX = 0, clickY = 0;
        const NSPoint local = [gView convertPoint:event.locationInWindow fromView:nil];
        mapViewPointToSurface(local.x, local.y, clickX, clickY);
        capturePointer();
        gVirtualPointerX = clickX;
        gVirtualPointerY = clickY;
    }

    // 坐标：AppKit 的原点在左下，客户区 DIB 的原点在左上，所以要翻 y。
    // 不翻的话点击会落在垂直镜像的位置——菜单上点"单人游戏"会命中"退出"。
    // 翻转+缩放都收在 mapViewPointToSurface 里（U-09 补缩放，见其注释）。
    //
    // U-11 捕获态例外：硬件指针被冻结，event.locationInWindow 恒为冻结时的那个点，
    // 只有 deltaX/deltaY 还在动。所以捕获时用**虚拟指针**（表面坐标，delta 累积、
    // 夹在表面内）作为游戏光标位置——这正是 Windows 上全屏 RA2 的手感。
    if (type != NSEventTypeKeyDown && type != NSEventTypeKeyUp) {
        if (gPointerState == kPointerCaptured) {
            advanceVirtualPointer(event.deltaX, event.deltaY);
            out.x = static_cast<int>(std::lround(gVirtualPointerX));
            out.y = static_cast<int>(std::lround(gVirtualPointerY));
        } else {
            const NSPoint local = [gView convertPoint:event.locationInWindow fromView:nil];
            mapViewPointToSurface(local.x, local.y, out.x, out.y);
        }
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

// U-10：系统指针"只在窗口内隐藏"的收敛器。
//
// 为什么需要它：U-05 为了让窗口"激活/失焦/orderFront"之后不冒出第二个箭头，
// 在**每个输入事件**上无条件 `[hiddenCursor set]`。而 -[NSCursor set] 是
// **全局**的"当前游标"——谁最后 set 谁说了算，而且不会因为指针离开窗口而自动
// 撤销。于是指针一移出游戏窗口，系统就把那张 1×1 空图继续画在别的应用/桌面上，
// 用户看到的是"鼠标出去了但看不见"（U-10 报障）。
//
// 修法不是删掉这条保险，而是把它变成**有边界的往返**：进入窗口才设空图、
// 离开立刻恢复箭头。判定不用 AppKit 的 isActive/isKeyWindow——那两个标志位在
// 本项目里并不可靠（见下面 sendEvent 的说明），而用 WindowServer 的事实：
// +[NSWindow windowNumberAtPoint:belowWindowWithWindowNumber:] 直接问"这个点最上面
// 是谁的窗口"，等于我们、才隐藏。这样即便窗口被别的应用盖住、或者 AppKit 的标志位
// 还停在旧值，也不会把别人的光标藏掉。
//
// 【U-10 顺带查清的事实】本项目从不调用 [NSApp sendEvent:]，只 nextEventMatchingMask
// 出队自己处理。实测（/tmp/keyprobe.m）：只出队时 NSApp.isActive=0、window.isKeyWindow=0，
// 而且 **cursor rect 根本不生效**（指针在窗口内、也 invalidate 过，系统游标仍是箭头）；
// 同一段代码改成出队后 sendEvent，三个值立刻变成 1/1/空图。
// 也就是说 U-04/U-05 那句"用 cursor rect 而不是 [NSCursor hide]，因为 cursor rect
// 是区域限定的"在实践上没成立过——真正在起作用的，一直是 U-05 补的那句全局 set。
// 这轮不去改事件分发（那是大手术），而是把"区域限定"这件事自己实现出来。
// L10N-02：隐藏区域从"窗口 frame"收窄到"内容区"。
//
// 用户 2026-11-11 报障：鼠标移到标题栏上，系统指针不出现。原因正是下面这个
// 函数用 gWindow.frame 判定——Titled 窗口的 frame 包含标题栏和边框，游戏把
// 那里也当成"窗口内"藏掉了指针。但标题栏是系统 chrome：拖动/关闭/最小化都
// 在那里操作，macOS 的惯例是 chrome 上永远用系统箭头。
//
// 修法：屏幕点转窗口坐标后与 gView.frame（= contentView）求交。标题栏点在
// 窗口坐标里 y ≥ 内容高度，落在 view.frame 之外 → 不隐藏 → 收敛器恢复箭头。
// 全屏时 view 铺满整个窗口（无标题栏），行为与原来完全一致。
static bool pointerIsOverOurWindow() {
    if (gWindow == nil || gView == nil) return false;
    const NSPoint p = [NSEvent mouseLocation];
    if (!NSPointInRect(p, gWindow.frame)) return false;
    const NSInteger top = [NSWindow windowNumberAtPoint:p belowWindowWithWindowNumber:0];
    if (top != gWindow.windowNumber) return false;
    const NSPoint inWindow = [gWindow convertPointFromScreen:p];
    return NSPointInRect(inWindow, gView.frame);
}

static void syncGlobalCursorForPointer() {
    ++gCursorSyncCalls;
    if (gWindow == nil || gView == nil) return;
    NSCursor* blank = concordInvisibleCursor();
    // 释放态（用户按了 ⌃⌘）下不隐藏：那时系统箭头就是他要的那个指针。
    const bool wantBlank = gSystemCursorHidden
        && gPointerState != kPointerReleased
        && pointerIsOverOurWindow();
    gCursorWantedBlank = wantBlank;
    const bool showingBlank = systemCursorIsBlank();
    if (wantBlank && !showingBlank) {
        if (blank != nil) [blank set];
    } else if (!wantBlank && showingBlank) {
        [[NSCursor arrowCursor] set];
    }
}

// ---------------------------------------------------------------------------
// U-11 指针捕获（按需捕获，用户 2026-11-11 决策）
//
// macOS 没有"把光标限制在某个窗口内"的 API，能做"限制"的只有相对模式：
// CGAssociateMouseAndMouseCursorPosition(false) 之后硬件指针**完全不动**，
// 由我们自己的虚拟指针吃 NSEvent.deltaX/deltaY 前进——这就是 VMware/所有 Mac
// 游戏所谓的"抓住鼠标"。所以"指针出不了窗口"的落地形式是"指针停住 + 游戏接管"。
//
// 三态：
//   kPointerFree     默认。指针自由出入，窗口内隐藏系统箭头（U-04/U-10 的常态）。
//   kPointerCaptured ⌃⌘G 进入。硬件指针钉在窗口中心并冻结，游戏光标随鼠标位移走。
//   kPointerReleased ⌃⌘ 释放。硬件指针接回游戏光标所在处并恢复关联，系统箭头可见，
//                    游戏光标停在原地等下一次点击/⌃⌘G。
// 为什么默认不捕获：窗口模式常用于边玩边切应用，一进游戏就抓住会让人以为鼠标坏了；
// 全屏则根本不需要捕获——窗口就是屏幕，指针本来出不去。
// ---------------------------------------------------------------------------
// 表面坐标 → 屏幕坐标（释放时把硬件指针"接"到游戏光标处）
static NSPoint screenPointForSurfacePoint(double surfaceX, double surfaceY) {
    const double surfaceW = gPresentedWidth > 0 ? gPresentedWidth : 1.0;
    const double surfaceH = gPresentedHeight > 0 ? gPresentedHeight : 1.0;
    const double viewW = gView != nil ? gView.bounds.size.width : surfaceW;
    const double viewH = gView != nil ? gView.bounds.size.height : surfaceH;
    const NSPoint local = NSMakePoint(surfaceX * viewW / surfaceW, viewH - surfaceY * viewH / surfaceH);
    const NSPoint inWindow = gView != nil ? [gView convertPoint:local toView:nil] : local;
    return gWindow != nil ? [gWindow convertPointToScreen:inWindow] : inWindow;
}

// 虚拟指针推进（捕获态专用）。dx/dy 是 NSEvent 的 delta（点，y 轴朝上），
// 转到表面坐标要翻 y 并夹在表面内——夹边是刻意的：拖到边上就贴住，不会溢出。
static void advanceVirtualPointer(double dx, double dy) {
    const double surfaceW = gPresentedWidth > 0 ? gPresentedWidth : 1.0;
    const double surfaceH = gPresentedHeight > 0 ? gPresentedHeight : 1.0;
    gVirtualPointerX += dx;
    gVirtualPointerY -= dy;
    if (gVirtualPointerX < 0.0) gVirtualPointerX = 0.0;
    if (gVirtualPointerY < 0.0) gVirtualPointerY = 0.0;
    if (gVirtualPointerX > surfaceW - 1.0) gVirtualPointerX = surfaceW - 1.0;
    if (gVirtualPointerY > surfaceH - 1.0) gVirtualPointerY = surfaceH - 1.0;
}

static void capturePointer() {
    if (gWindow == nil || gPointerState == kPointerCaptured) return;
    // 先把硬件指针钉到窗口中心：随后它就不动了，放在中心最不容易在视觉上"卡在角落"。
    const NSRect frame = gWindow.frame;
    warpPointerToScreenPoint(NSMidX(frame), NSMidY(frame));
    // 虚拟指针从游戏光标当前位置接手（表面中心是安全默认）。
    gVirtualPointerX = (gPresentedWidth > 0 ? gPresentedWidth : 2) / 2.0;
    gVirtualPointerY = (gPresentedHeight > 0 ? gPresentedHeight : 2) / 2.0;
    CGAssociateMouseAndMouseCursorPosition(false);
    gPointerState = kPointerCaptured;
    std::fprintf(stderr, "[capture] captured\n");
}

static void releasePointer() {
    if (gPointerState != kPointerCaptured) return;
    // 顺序很关键：先恢复关联，再把指针挪到游戏光标处，否则挪了也不生效。
    CGAssociateMouseAndMouseCursorPosition(true);
    const NSPoint target = screenPointForSurfacePoint(gVirtualPointerX, gVirtualPointerY);
    warpPointerToScreenPoint(target.x, target.y);
    gPointerState = kPointerReleased;
    std::fprintf(stderr, "[capture] released pointer=%.0f,%.0f\n", target.x, target.y);
}

// 无论如何都要把关联恢复回去：这个开关一旦留在 false，用户的鼠标就"不能动"了
// （只能强杀进程）。所以除了正常释放路径，再挂一条 atexit 兜底。
static void ensurePointerAssociationRestored() {
    if (gPointerState == kPointerCaptured) {
        CGAssociateMouseAndMouseCursorPosition(true);
        gPointerState = kPointerFree;
    }
}

void pumpEvents() {
    if (gInputSink == nullptr || gWindow == nil) return;
    syncGlobalCursorForPointer();
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
            // U-10：这里原来每个事件都无条件 `[hiddenCursor set]`，那是全局设置，
            // 指针离开窗口后不会自动撤销，于是"移出去就看不见指针"。
            // 现在改由 syncGlobalCursorForPointer() 做有边界的往返（见其注释），
            // 每轮 pump 已经调过，这里只负责把事件交给 sink。
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
