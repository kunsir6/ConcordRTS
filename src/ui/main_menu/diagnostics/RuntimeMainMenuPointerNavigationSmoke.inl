// Q-07：菜单「指针投递闭环」自测——用真实窗口消息驱动一次点击，验证菜单真的换页。
//
// 为什么需要它：Q-06 把 AppKit 事件翻译成 WM_* 的**规则**钉死了
// （CompatAppleInputSemantics.cpp 26 条），但那条自测只证明「投递函数长得对」。
// 从「消息进队列」到「屏幕上是另一页」之间还有四段全都没被验证过：
//   1. GetMessageW/DispatchMessageW 真的把消息送到 mainMenuWindowProc；
//   2. hitButton 把客户区坐标换算成基座坐标后命中**正确的按钮**（viewLeft/viewTop 偏置、
//      sidebar 列区间、slot 高度三处只要错一处就静默点不中）；
//   3. phase 门禁（SlideIn 期间 hitButton 恒 -1）——点击时机不对就什么都不发生；
//   4. 换页后重绘出的确实是另一屏（按钮集合变了、像素变了）。
//
// 现成的 `--main-menu-ui-smoke-test` 是**直接赋值** state.page 渲染四页（见
// RuntimeMainMenu.cpp 的 featurePagesRendered / singlePlayerPath 段），
// 它绕过了整个输入与状态机，所以换页画得对、点击可能是死的。本自测走完整链路：
//   PostMessageW(WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_LBUTTONUP) → 真实消息循环 → 窗口过程
//   → handleMainMenuLeftDown/Up → activate → performImmediateAction → page 改变
//   → WM_PAINT → renderBase → composePixels → BMP
//
// 断言里的「像素差异」只在一个固定区域（右侧按钮列 x∈[632,800) y∈[199,535)）内统计，
// 而不是整幅：菜单背景有 Bink 动画与告警灯闪烁，全幅差异恒不为零，那样的断言永远绿。
//
// 显式诊断用，不参与产品路径。启用：
//   CONCORD_MAIN_MENU_POINTER_NAV_ONLY=1 \
//   CONCORD_MAIN_MENU_POINTER_NAV_OUTPUT=<目录> \
//   concord_runtime --main-menu-ui-smoke-test <占位 bmp>
//
// 第一版的四条错误期望（保留作教训，别再犯）：
//   · Home 是 **6** 个按钮（5 个槽位 + SDBTM 的 Exit），不是 7 个；
//   · `SetWindowPos` 之后 WM_SIZE 是**排队**投递的，紧接着读 viewLeft/viewTop 还是旧值
//     （0,0），必须先泵到视口就位再算点击坐标——否则用旧偏置算出的坐标在新偏置下
//     会整体左移 112 像素、落到 sidebar 左边之外，表现为「按下了但抬起没反应」；
//   · 投递后固定等 60ms 不够：消息何时被取出取决于主循环节奏，必须「等到状态变化」
//     而不是「等一段固定时间」；
//   · 回到 Home 后 `indexOfAction(OpenHome) == -1`，用 -1 当索引取值是越界 UB
//     （跑起来不一定崩，但断言会假通过：hover==-1 与 backIndex==-1 恰好相等）。
#include "runtime/menu/RuntimeMainMenuController.h"

#include <cstdlib>
#include <utility>

namespace {

// 客户区坐标 = 基座坐标按当前整数缩放放大后加居中偏置。
// 1024x640 下单是 1 倍、偏置 (112,20)；用乘法而不是加法，换分辨率也不会错。
std::pair<int, int> pointerNavClientPoint(const MenuState& state, int baseX, int baseY) {
    const int x = state.viewLeft + baseX * state.viewWidth / kWidth;
    const int y = state.viewTop + baseY * state.viewHeight / kHeight;
    return {x, y};
}

// 按钮中心（基座坐标）。负 slot 是 SDBTM 那个独立按钮，它的纵坐标不由 slot 推导。
std::pair<int, int> pointerNavButtonCentre(
    const RuntimeMainMenuController& state,
    const ButtonSpec& button
) {
    const int slot = resolvedSlot(state, button);
    const int top = slot < 0
        ? buttonTopFor(button)
        : buttonsTop(state) + slot * slotHeight(state);
    return {sidebarX(state) + 30, top + slotHeight(state) / 2};
}

// 只在按钮列里数「不同的像素」（不是字节）：文字/高亮变了才会计数。
std::size_t pointerNavRegionDiff(
    const std::vector<unsigned char>& left,
    const std::vector<unsigned char>& right
) {
    constexpr int x0 = kSidebarLeft;
    constexpr int x1 = kWidth;
    constexpr int y0 = kSidebarTopHeight;
    constexpr int y1 = 535;  // 八个 42 像素槽位结束处；SDBTM 按钮不在这个区域
    if (left.size() != right.size()
        || left.size() != static_cast<std::size_t>(kWidth * kHeight * 4)) {
        return static_cast<std::size_t>(-1);
    }
    std::size_t differing = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * kWidth + x) * 4;
            if (left[at] != right[at]
                || left[at + 1] != right[at + 1]
                || left[at + 2] != right[at + 2]) {
                ++differing;
            }
        }
    }
    return differing;
}

}  // namespace

int runMainMenuPointerNavigationSmoke(
    const RuntimeConfig& input,
    const fs::path& outputDirectory
) {
    unsigned checks = 0;
    unsigned failures = 0;
    const auto require = [&](bool value, const char* detail) {
        ++checks;
        if (!value) {
            ++failures;
            std::cerr << "Main pointer nav: " << detail << "\n";
        }
    };

    std::error_code directoryError;
    fs::create_directories(outputDirectory, directoryError);
    if (directoryError) {
        std::cerr << "Main pointer nav: cannot create output directory: "
                  << directoryError.message() << "\n";
        return 1;
    }

    prewarmedMainMenuState.reset();
    MenuState state;
    if (!initializeMenu(state, input, true)) {
        // 失败必须提前返回：继续往下走只会拿空句柄去建窗口，
        // 上层看到的是段错误而不是「资源没起来」。
        std::cerr << "Main pointer nav: original menu resources did not initialize\n";
        return 1;
    }
    // 关声音：本自测只判导航；开着会让 click/slide 音效与音乐时钟参与进来，
    // 断言失败时无法区分是导航错了还是音频卡了。
    state.audio.enabled = false;

    // 自己注册一个窗口类，不用 L"STATIC"：预定义类要靠 compat 的
    // CreateWindowExW 认识它们（Windows 上天然存在），而本仓 2026-11-11 01:50
    // 时还**没有**这张表——用它会让本自测的成败取决于另一处未完成的改动，
    // 表现为「父窗口建不出来」，看起来却像导航坏了。
    constexpr wchar_t kPointerNavParentClass[] = L"ConcordPointerNavParent";
    WNDCLASSEXW parentClass{};
    parentClass.cbSize = sizeof(parentClass);
    parentClass.lpfnWndProc = DefWindowProcW;
    parentClass.hInstance = GetModuleHandleW(nullptr);
    parentClass.lpszClassName = kPointerNavParentClass;
    parentClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    require(RegisterClassExW(&parentClass) != 0, "own parent window class registration");
    HWND parent = CreateWindowExW(
        0, kPointerNavParentClass, L"Owned pointer nav parent",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        40, 40, 1100, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    require(parent != nullptr, "owned parent window");
    if (parent == nullptr) return 1;
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    HWND page = createMainMenuWindow(state, input, parent);
    require(page != nullptr, "production main menu child window");
    if (page == nullptr) {
        DestroyWindow(parent);
        return 1;
    }
    ShowWindow(page, SW_SHOW);
    UpdateWindow(page);
    require(
        SetWindowPos(page, nullptr, 0, 0, 1024, 640, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
        "child resize to the 1024x640 runtime window"
    );
    UpdateWindow(page);

    // 泵消息：真实队列（WM_TIMER/WM_PAINT 由 compat 在该队列空时合成）为主，
    // updateDynamicState 是同一件事的确定性兜底——不兜底就会让自测依赖
    // 「合成定时器恰好在这一轮被取到」的时序，跑两次结果不同。
    auto pump = [&](int milliseconds) {
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) return;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!IsWindow(page)) return;
            updateDynamicState(state);
            MsgWaitForMultipleObjectsEx(0, nullptr, 4, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (GetTickCount64() < deadline && IsWindow(page));
    };
    // 等到条件成立，而不是等固定时长：消息何时被取出取决于主循环节奏，
    // 固定 60ms 在机器忙时会假失败（第一版就是这么错的）。
    auto waitUntil = [&](auto&& predicate, int milliseconds) {
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
        while (!predicate() && GetTickCount64() < deadline && IsWindow(page)) {
            pump(15);
        }
        return predicate();
    };
    auto awaitIdle = [&](const char* detail) {
        // hitButton 在 SlideIn/SlideOut 期间恒返回 -1：这是「点得对但没反应」
        // 最容易被误判成实现错误的地方。
        require(
            waitUntil([&] { return state.phase == MenuPhase::Idle; }, 5000),
            detail
        );
    };
    auto indexOfAction = [&](MenuAction action) {
        const std::vector<ButtonSpec> current = buttons(state);
        for (std::size_t i = 0; i < current.size(); ++i) {
            if (current[i].action == action) return static_cast<int>(i);
        }
        return -1;
    };
    auto renderAndSnapshot = [&](const fs::path& path, std::vector<unsigned char>& into) {
        state.hover = -1;        // 高亮会改像素；快照时统一抹掉
        state.mouseVisible = false;
        state.backDirty = true;
        RenderSurface surface{state.backBuffer.dc(), state.composePixels.data()};
        require(renderBase(state, surface), "renderBase on the snapshot frame");
        into = state.composePixels;
        std::string error;
        require(writeBmp(path.string(), state.composePixels.data(), error),
            error.empty() ? "snapshot bmp" : error.c_str());
    };
    // 用真实消息队列投递，而不是 SendMessageW：SendMessageW 会绕过
    // GetMessageW/DispatchMessageW 这一段——而那正是 Q-06 之后最可疑的一段。
    auto postMouse = [&](UINT message, WPARAM wParam, int clientX, int clientY) {
        return PostMessageW(page, message, wParam, MAKELPARAM(clientX, clientY)) != FALSE;
    };
    // 一次完整点击：移动→按下→抬起，全部只入队，取由主循环负责。
    auto clickAt = [&](int baseX, int baseY, const char* what) {
        const std::pair<int, int> client = pointerNavClientPoint(state, baseX, baseY);
        const std::string prefix = what;
        require(postMouse(WM_MOUSEMOVE, 0, client.first, client.second), (prefix + " move").c_str());
        pump(40);
        require(
            postMouse(WM_LBUTTONDOWN, MK_LBUTTON, client.first, client.second),
            (prefix + " down").c_str()
        );
        pump(40);
        require(
            postMouse(WM_LBUTTONUP, 0, client.first, client.second),
            (prefix + " up").c_str()
        );
    };

    // WM_SIZE 是排队投递的：不等视口就位就取 viewLeft/viewTop，会拿到建窗口时的旧值
    // （0,0），后面所有点击坐标都会整体偏 112 像素而落到 sidebar 之外。
    const bool viewportSettled = waitUntil(
        [&] {
            return state.viewLeft == 112 && state.viewTop == 20
                && state.viewWidth == 800 && state.viewHeight == 600;
        },
        3000
    );
    require(viewportSettled, "1024x640 keeps the exact 800x600 pixel grid centred at (112,20)");
    awaitIdle("home page settles to Idle before any input");

    // ---- 第一屏：Home ----
    require(state.page == MenuPage::Home, "entry page is Home");
    const int openSingleIndex = indexOfAction(MenuAction::OpenSinglePlayer);
    require(openSingleIndex >= 0, "Home exposes OpenSinglePlayer");
    const int homeButtonCount = static_cast<int>(buttons(state).size());
    require(homeButtonCount == 6, "Home has six buttons (five slots + SDBTM Exit)");
    std::vector<unsigned char> homePixels;
    renderAndSnapshot(outputDirectory / "pointer_01_home.bmp", homePixels);

    // 悬停：命中测试把客户区 (viewLeft+base) 换回基座坐标再判区间。
    const std::pair<int, int> singleCentre =
        pointerNavButtonCentre(state, buttons(state)[static_cast<std::size_t>(openSingleIndex)]);
    {
        const std::pair<int, int> client =
            pointerNavClientPoint(state, singleCentre.first, singleCentre.second);
        require(postMouse(WM_MOUSEMOVE, 0, client.first, client.second), "post hover WM_MOUSEMOVE");
        require(
            waitUntil([&] { return state.hover == openSingleIndex; }, 2000),
            "hover lands on the OpenSinglePlayer slot"
        );
        require(state.mouseVisible, "hover makes the menu cursor visible");
    }
    // 反例：空白处不该命中任何按钮（否则后面「点中就换页」证明不了是命中生效）。
    {
        const std::pair<int, int> client = pointerNavClientPoint(state, 100, 100);
        require(postMouse(WM_MOUSEMOVE, 0, client.first, client.second), "post blank WM_MOUSEMOVE");
        pump(40);
        require(state.hover == -1, "blank area hits no button");
    }
    clickAt(100, 100, "blank");
    pump(60);
    require(state.pressed == -1, "blank press arms nothing");
    require(state.page == MenuPage::Home, "blank click keeps the page");

    // ---- 点击 Single Player：真正的换页 ----
    // 先单独按下，验证按下态（capture + pressed），再抬起触发动作。
    {
        const std::pair<int, int> client =
            pointerNavClientPoint(state, singleCentre.first, singleCentre.second);
        require(postMouse(WM_MOUSEMOVE, 0, client.first, client.second), "post move before press");
        pump(40);
        require(postMouse(WM_LBUTTONDOWN, MK_LBUTTON, client.first, client.second), "post down on SinglePlayer");
        require(
            waitUntil([&] { return state.pressed == openSingleIndex; }, 2000),
            "press arms the SinglePlayer slot"
        );
        require(GetCapture() == page, "press takes mouse capture on the real page");
        require(postMouse(WM_LBUTTONUP, 0, client.first, client.second), "post up on SinglePlayer");
    }
    require(
        waitUntil([&] { return state.page == MenuPage::SinglePlayer; }, 3000),
        "release activates SinglePlayer (page changed)"
    );
    require(
        state.phase == MenuPhase::SlideIn,
        "page entry starts the slide-in phase (redraw gate, not an instant swap)"
    );
    awaitIdle("SinglePlayer page settles to Idle");
    require(state.page == MenuPage::SinglePlayer, "SinglePlayer retained after settling");
    require(indexOfAction(MenuAction::Campaign) == 0, "SinglePlayer page starts with Campaign");
    require(indexOfAction(MenuAction::Skirmish) >= 0, "SinglePlayer page exposes Skirmish");
    require(indexOfAction(MenuAction::OpenHome) >= 0, "SinglePlayer page exposes Back");
    require(
        static_cast<int>(buttons(state).size()) != homeButtonCount,
        "the two pages have different button counts (not a repaint of the same list)"
    );
    std::vector<unsigned char> singlePixels;
    renderAndSnapshot(outputDirectory / "pointer_02_single_player.bmp", singlePixels);

    const std::size_t forwardDiff = pointerNavRegionDiff(homePixels, singlePixels);
    require(
        forwardDiff != static_cast<std::size_t>(-1) && forwardDiff > 2000,
        "Home vs SinglePlayer differ in the button column (text/layout actually changed)"
    );

    // ---- 回程：点 Back 回 Home，且回到的是同一屏 ----
    const int backIndex = indexOfAction(MenuAction::OpenHome);
    // -1 不能当索引用：越界取值的 UB 会让后面的断言假通过（第一版踩过）。
    require(backIndex >= 0, "Back index is valid before clicking it");
    if (backIndex >= 0) {
        const std::pair<int, int> backCentre =
            pointerNavButtonCentre(state, buttons(state)[static_cast<std::size_t>(backIndex)]);
        const std::pair<int, int> client =
            pointerNavClientPoint(state, backCentre.first, backCentre.second);
        require(postMouse(WM_MOUSEMOVE, 0, client.first, client.second), "post move to Back");
        require(
            waitUntil([&] { return state.hover == backIndex; }, 2000),
            "hover lands on Back"
        );
        require(postMouse(WM_LBUTTONDOWN, MK_LBUTTON, client.first, client.second), "post down on Back");
        pump(40);
        require(postMouse(WM_LBUTTONUP, 0, client.first, client.second), "post up on Back");
        require(
            waitUntil([&] { return state.page == MenuPage::Home; }, 3000),
            "Back returns to Home"
        );
    }
    awaitIdle("Home page settles to Idle after the round trip");
    std::vector<unsigned char> backPixels;
    renderAndSnapshot(outputDirectory / "pointer_03_back_home.bmp", backPixels);

    const std::size_t returnDiff = pointerNavRegionDiff(homePixels, backPixels);
    require(
        returnDiff != static_cast<std::size_t>(-1) && returnDiff * 4 < forwardDiff,
        "the round trip repaints the original button column (diff an order below the forward one)"
    );

    if (IsWindow(page)) {
        PostMessageW(page, WM_CLOSE, 0, 0);
        pump(120);
    }
    require(!IsWindow(page), "WM_CLOSE retires the page through the real window proc");
    if (IsWindow(parent)) DestroyWindow(parent);
    prewarmedMainMenuState.reset();

    std::cout << "Main pointer nav: checks=" << checks
              << " failures=" << failures
              << " pages=" << static_cast<int>(MenuPage::Home) << "->"
              << static_cast<int>(MenuPage::SinglePlayer) << "->"
              << static_cast<int>(MenuPage::Home)
              << " homeButtons=" << homeButtonCount
              << " forwardDiff=" << forwardDiff
              << " returnDiff=" << returnDiff
              << " bmp=" << outputDirectory.string() << "\n";
    return failures != 0 ? 1 : 0;
}
