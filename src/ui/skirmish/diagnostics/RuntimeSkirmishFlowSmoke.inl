// ON-4-02c 切片 1：大厅内走**队列**完成 选图往返。
//
// 与 RuntimeSkirmishCursorSmoke 的分工：
//   那个证明「队列路径在大厅窗口工作」（一次滑块拖动），用的是 SendMessageW 的对照组；
//   本项把真实流程往前走：Lobby --点击 ChooseMap--> MapSelection --点一行地图-->
//   --点 UseMap--> 回 Lobby。全程 PostMessageW 入队 + 真实消息泵取出。
//
// 不在这里点 Start：那会进游戏主循环，smoke 必须自己负责退出（_Exit 纪律），
// 混在同一个函数里会让"选图错"和"开场错"不可分辨。Start 属切片 2。
bool runRuntimeSkirmishFlowSmoke(
    MenuState& state,
    const RuntimeConfig& config,
    const fs::path& outputDirectory,
    std::string& error
) {
    struct FlowGuard {
        MenuState& state;
        RuntimeConfig config;
        HWND host = nullptr;
        HWND child = nullptr;
        ~FlowGuard() {
            if (child != nullptr && IsWindow(child)) DestroyWindow(child);
            if (host != nullptr && IsWindow(host)) DestroyWindow(host);
            resetMenuForEntry(state, config);
        }
    } guard{state, config};

    const auto fail = [&](const std::string& reason) {
        error = "skirmish flow: " + reason;
        return false;
    };

    std::error_code directoryError;
    fs::create_directories(outputDirectory, directoryError);

    resetMenuForEntry(state, config);
    guard.host = CreateWindowExW(0, L"STATIC", L"Skirmish flow smoke",
        WS_POPUP | WS_CLIPCHILDREN, 0, 0, kBaseWidth, kBaseHeight,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (guard.host == nullptr) return fail("hidden host creation failed");
    guard.child = createShellSkirmishWindow(state, config, guard.host);
    if (guard.child == nullptr) return fail("hidden shell creation failed");

    const auto render = [&]() {
        RECT client{};
        GetClientRect(guard.child, &client);
        computeShellViewport(state, client.right, client.bottom);
        HDC dc = GetDC(nullptr);
        if (dc == nullptr) return false;
        const bool ok = ensureShellBackBuffer(state, dc)
            && (!state.backDirty || renderShellBackBuffer(state));
        ReleaseDC(nullptr, dc);
        return ok;
    };

    // 只入队、不直接调窗口过程：与 cursor smoke 里那段队列投递同一口径。
    const auto pumpQueue = [&](int milliseconds) {
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) return;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!IsWindow(guard.child)) return;
            MsgWaitForMultipleObjectsEx(0, nullptr, 4, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (GetTickCount64() < deadline && IsWindow(guard.child));
    };
    const auto waitFor = [&](auto&& predicate, int milliseconds) {
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
        while (!predicate() && GetTickCount64() < deadline && IsWindow(guard.child)) {
            pumpQueue(10);
        }
        return predicate();
    };
    // 基座坐标 → 客户区坐标：与 cursor smoke 用同一套缩放（不是 1:1）。
    const auto toClient = [&](POINT base) {
        return POINT{state.viewLeft + base.x * state.viewWidth / kBaseWidth,
                     state.viewTop + base.y * state.viewHeight / kBaseHeight};
    };
    const auto clickAt = [&](POINT client) {
        PostMessageW(guard.child, WM_MOUSEMOVE, 0, MAKELPARAM(client.x, client.y));
        pumpQueue(20);
        PostMessageW(guard.child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(client.x, client.y));
        pumpQueue(20);
        PostMessageW(guard.child, WM_LBUTTONUP, 0, MAKELPARAM(client.x, client.y));
    };
    // HitRegion::rect 是 RuntimeInputPixelRect（int32 四边），不是 Win32 RECT。
    const auto centreOf = [](const RuntimeInputPixelRect& rect) {
        return POINT{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    };
    // 第 n 个指定 kind 的命中区。列表类区域（Map 每行一个）必须按序号取，
    // 不能用 find_if 拿第一个——那样"换一张图"永远点在同一行上。
    const auto nthHitOfKind = [&](HitKind kind, std::size_t nth) -> const HitRegion* {
        std::size_t seen = 0;
        for (const auto& hit : state.hits) {
            if (hit.target.kind != kind) continue;
            if (seen == nth) return &hit;
            ++seen;
        }
        return nullptr;
    };

    if (!render()) return fail("shell render failed");

    // ---- 起点：Lobby ----
    if (state.page != SkirmishPage::Lobby) return fail("entry page is not Lobby");
    const auto* chooseMap = nthHitOfKind(HitKind::ChooseMap, 0);
    if (chooseMap == nullptr) return fail("Lobby does not expose ChooseMap");
    clickAt(toClient(centreOf(chooseMap->rect)));

    if (!waitFor([&] { return state.page == SkirmishPage::MapSelection; }, 5000))
        return fail("posted click on ChooseMap did not open MapSelection");
    if (!render()) return fail("map selection render failed");
    if (!waitFor([&] { return !state.hits.empty(); }, 3000))
        return fail("map selection produced no hit regions");

    // ---- 地图选择页：换一张图 ----
    const int beforeMap = state.selectedMap;
    // 选一个与当前不同的行：第一个 Map 行若正好是当前项，就取第二行。
    const std::size_t rows = static_cast<std::size_t>(std::count_if(
        state.hits.begin(), state.hits.end(),
        [](const HitRegion& hit) { return hit.target.kind == HitKind::Map; }));
    if (rows == 0) return fail("map selection exposes no Map rows");
    std::size_t target = 0;
    const auto* firstRow = nthHitOfKind(HitKind::Map, 0);
    const auto* secondRow = nthHitOfKind(HitKind::Map, 1);
    // 行序与地图索引不保证一一对应，所以用"点了之后 selectedMap 变了"来判，
    // 而不是假设第 n 行就是第 n 张图。
    if (secondRow != nullptr && firstRow != nullptr
        && state.selectedMap == 0) {
        target = 1;  // 第一行很可能就是索引 0，换第二行避免"点了个没变化"
    }
    const auto* row = nthHitOfKind(HitKind::Map, target);
    if (row == nullptr) return fail("target map row disappeared");
    clickAt(toClient(centreOf(row->rect)));
    if (!waitFor([&] { return state.selectedMap != beforeMap; }, 5000))
        return fail("posted click on a map row did not change selectedMap (rows="
            + std::to_string(rows) + " before=" + std::to_string(beforeMap) + ")");
    const int chosenMap = state.selectedMap;
    if (chosenMap < 0) return fail("selectedMap is invalid after the click");

    // ---- 回 Lobby ----
    const auto* useMap = nthHitOfKind(HitKind::UseMap, 0);
    if (useMap == nullptr) return fail("MapSelection does not expose UseMap");
    clickAt(toClient(centreOf(useMap->rect)));
    if (!waitFor([&] { return state.page == SkirmishPage::Lobby; }, 5000))
        return fail("posted click on UseMap did not return to Lobby");
    if (state.selectedMap != chosenMap)
        return fail("returning to Lobby lost the chosen map");

    // ---- 切片 2：点 Start 发起开局 ----
    //
    // 实测口径（诊断一次拿到的）：点 Start 之后 closing=1、childAlive=0、map 保持。
    // 也就是说 Start 的语义是「关闭大厅窗口并把结果交回上层」，
    // **不在这里**进入加载页/游戏内——那两步由 RuntimeMain 在窗口关闭后启动。
    // 所以本切片的断言只能是"开场被发起且参数没丢"，
    // 加载页与游戏内首帧属 ON-4-02d（跨 TU）。
    const auto* start = nthHitOfKind(HitKind::Start, 0);
    if (start == nullptr) return fail("Lobby does not expose Start");
    clickAt(toClient(centreOf(start->rect)));

    if (!waitFor([&] { return state.closing; }, 5000))
        return fail("posted click on Start did not begin closing the lobby");
    if (state.selectedMap != chosenMap)
        return fail("Start cleared the chosen map before handing off (map="
            + std::to_string(state.selectedMap) + " expected=" + std::to_string(chosenMap) + ")");
    // 窗口必须真的退场：只 closing 而窗口还在，等于"点了没反应"的另一种形态。
    if (!waitFor([&] { return !IsWindow(guard.child); }, 5000))
        return fail("lobby window is still alive after Start (closing flag without destruction)");

    std::cout << "Skirmish flow: checks=6 failures=0"
              << " pages=Lobby->MapSelection->Lobby->Start"
              << " rows=" << rows
              << " mapBefore=" << beforeMap
              << " mapChosen=" << chosenMap
              << " closing=1 windowGone=1\n";
    return true;
}
