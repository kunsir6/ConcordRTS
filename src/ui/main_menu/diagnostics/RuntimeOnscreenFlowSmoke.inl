// ON-4-02a：真实点击驱动「主菜单 → 单人页 → 发起遭遇战转场」。
//
// 与 Q-07 指针导航（RuntimeMainMenuPointerNavigationSmoke.inl）的分工：
//   Q-07：Home ↔ SinglePlayer 往返，证明「投递 → 命中 → 换页」这条链是通的；
//   本项：在它之后**继续往下走一步**，点 Skirmish 并验证菜单真的开始关闭、
//         且返回的结果是 Skirmish —— 也就是"转场被发起"这件事本身。
//
// 边界写清楚：本条**不**进入遭遇战大厅。点完 Skirmish 后菜单窗口进入 closing，
// 遭遇战窗口由上层（RuntimeMain 的菜单循环）启动，属 ON-4-02b。
// 之所以切在这里：再往下一格就跨出 main menu TU 的可见范围，混在一起会让
// "谁该为失败负责"变得不可判。
//
// 所有输入都走 PostMessageW → 真实消息队列 → 窗口过程，与 Q-07 完全同一条路径，
// 不用 SendMessageW（那会绕开 GetMessageW/DispatchMessageW 这一段，而那正是要验的）。
int runRuntimeOnscreenFlowSmoke(const RuntimeConfig& input, const fs::path& outputDirectory) {
    unsigned checks = 0;
    unsigned failures = 0;
    const auto require = [&](bool value, const char* detail) {
        ++checks;
        if (!value) {
            ++failures;
            std::cerr << "Onscreen flow: " << detail << "\n";
        }
    };

    std::error_code directoryError;
    fs::create_directories(outputDirectory, directoryError);
    if (directoryError) {
        std::cerr << "Onscreen flow: cannot create output directory: "
                  << directoryError.message() << "\n";
        return 1;
    }

    prewarmedMainMenuState.reset();
    MenuState state;
    if (!initializeMenu(state, input, true)) {
        std::cerr << "Onscreen flow: original menu resources did not initialize\n";
        return 1;
    }
    // 关声音：本自测判的是导航与转场，音频时钟参与进来会让失败无法归因。
    state.audio.enabled = false;

    constexpr wchar_t kFlowParentClass[] = L"ConcordOnscreenFlowParent";
    WNDCLASSEXW parentClass{};
    parentClass.cbSize = sizeof(parentClass);
    parentClass.lpfnWndProc = DefWindowProcW;
    parentClass.hInstance = GetModuleHandleW(nullptr);
    parentClass.lpszClassName = kFlowParentClass;
    parentClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    require(RegisterClassExW(&parentClass) != 0, "own parent window class registration");
    HWND parent = CreateWindowExW(
        0, kFlowParentClass, L"Owned onscreen flow parent",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        40, 40, 1100, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    require(parent != nullptr, "owned parent window");
    if (parent == nullptr) return 1;
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    HWND page = createMainMenuWindow(state, input, parent);
    require(page != nullptr, "production main menu child window");
    if (page == nullptr) { DestroyWindow(parent); return 1; }
    ShowWindow(page, SW_SHOW);
    UpdateWindow(page);
    require(
        SetWindowPos(page, nullptr, 0, 0, 1024, 640, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
        "child resize to the 1024x640 runtime window"
    );
    UpdateWindow(page);

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
    auto waitUntil = [&](auto&& predicate, int milliseconds) {
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
        while (!predicate() && GetTickCount64() < deadline && IsWindow(page)) pump(15);
        return predicate();
    };
    auto awaitIdle = [&](const char* detail) {
        require(waitUntil([&] { return state.phase == MenuPhase::Idle; }, 5000), detail);
    };
    auto indexOfAction = [&](MenuAction action) {
        const std::vector<ButtonSpec> current = buttons(state);
        for (std::size_t i = 0; i < current.size(); ++i) {
            if (current[i].action == action) return static_cast<int>(i);
        }
        return -1;
    };
    auto snapshot = [&](const fs::path& path, std::vector<unsigned char>& into) {
        state.hover = -1;
        state.mouseVisible = false;
        state.backDirty = true;
        RenderSurface surface{state.backBuffer.dc(), state.composePixels.data()};
        require(renderBase(state, surface), "renderBase on the snapshot frame");
        into = state.composePixels;
        std::string error;
        require(writeBmp(path.string(), state.composePixels.data(), error),
            error.empty() ? "snapshot bmp" : error.c_str());
    };
    auto postMouse = [&](UINT message, WPARAM wParam, int clientX, int clientY) {
        return PostMessageW(page, message, wParam, MAKELPARAM(clientX, clientY)) != FALSE;
    };
    // 点一个按钮并**等它真的生效**，而不是等固定时长。
    // 固定时长在机器忙时会假失败（Q-07 第一版就是这么错的）。
    auto clickButtonAndWait = [&](MenuAction action, auto&& settled, const char* what) {
        const int index = indexOfAction(action);
        require(index >= 0, what);
        if (index < 0) return false;
        const std::pair<int, int> centre =
            pointerNavButtonCentre(state, buttons(state)[static_cast<std::size_t>(index)]);
        const std::pair<int, int> client =
            pointerNavClientPoint(state, centre.first, centre.second);
        require(postMouse(WM_MOUSEMOVE, 0, client.first, client.second),
            (std::string(what) + " move").c_str());
        pump(40);
        require(postMouse(WM_LBUTTONDOWN, MK_LBUTTON, client.first, client.second),
            (std::string(what) + " down").c_str());
        pump(40);
        require(postMouse(WM_LBUTTONUP, 0, client.first, client.second),
            (std::string(what) + " up").c_str());
        return waitUntil(settled, 30000);
    };

    const bool viewportSettled = waitUntil(
        [&] {
            return state.viewLeft == 112 && state.viewTop == 20
                && state.viewWidth == 800 && state.viewHeight == 600;
        }, 3000);
    require(viewportSettled, "1024x640 keeps the 800x600 pixel grid centred at (112,20)");
    awaitIdle("home page settles to Idle before any input");
    require(state.page == MenuPage::Home, "entry page is Home");

    std::vector<unsigned char> homePixels;
    snapshot(outputDirectory / "flow_01_home.bmp", homePixels);

    // ---- 第一跳：主菜单 → 单人页 ----
    const bool toSingle = clickButtonAndWait(
        MenuAction::OpenSinglePlayer,
        [&] { return state.page == MenuPage::SinglePlayer; },
        "home exposes OpenSinglePlayer");
    require(toSingle, "release on SinglePlayer changes the page");
    awaitIdle("SinglePlayer page settles to Idle");
    std::vector<unsigned char> singlePixels;
    snapshot(outputDirectory / "flow_02_single_player.bmp", singlePixels);
    require(pointerNavRegionDiff(homePixels, singlePixels) > 2000,
        "Home vs SinglePlayer differ in the button column");
    require(indexOfAction(MenuAction::Skirmish) >= 0, "SinglePlayer exposes Skirmish");

    // ---- 第二跳：单人页 → 发起遭遇战转场 ----
    // 判据不能只看「点了之后 closing」：那可能只是页面上某个按钮也关窗。
    // 必须同时看到 result==Skirmish 与 pending 清空（产品侧 smoke 用的是同一组三个条件）。
    const bool toSkirmish = clickButtonAndWait(
        MenuAction::Skirmish,
        [&] { return state.closing || state.result == RuntimeMainMenuResult::Skirmish; },
        "SinglePlayer exposes Skirmish");
    require(toSkirmish, "release on Skirmish starts the transition (menu begins closing)");

    // closing 之后主循环可能已经把窗口退了，继续 pump 会打空；等到结果落定即可。
    const bool settled = waitUntil(
        [&] { return state.result == RuntimeMainMenuResult::Skirmish
                    && state.pending == MenuAction::None; },
        10000);
    require(settled, "transition result is Skirmish with no pending action");
    require(state.closing, "menu is closing after the Skirmish transition");

    // 转场发起前的最后一屏：用来证明"走的是遭遇战那条路，不是回 Home"。
    // 注意此时页面已在 SlideOut，hover/pressed 都已清零。
    std::vector<unsigned char> skirmishPendingPixels;
    const bool capturedPending = IsWindow(page) && state.page == MenuPage::SinglePlayer;
    if (capturedPending) {
        snapshot(outputDirectory / "flow_03_skirmish_pending.bmp", skirmishPendingPixels);
    }

    if (IsWindow(parent)) DestroyWindow(parent);
    prewarmedMainMenuState.reset();

    std::cout << "Onscreen flow: checks=" << checks
              << " failures=" << failures
              << " pages=home->single->skirmish"
              << " closing=" << (state.closing ? 1 : 0)
              << " result=" << static_cast<int>(state.result)
              << " pending=" << static_cast<int>(state.pending)
              << " pendingScreenCaptured=" << (capturedPending ? 1 : 0)
              << " bmp=" << outputDirectory.string() << "\n";
    return failures != 0 ? 1 : 0;
}
