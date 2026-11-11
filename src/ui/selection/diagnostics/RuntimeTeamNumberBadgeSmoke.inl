bool runRuntimeTeamNumberBadgeSmoke(RuntimeWindowState& live, std::string& error) {
    std::string scene;
    const auto fail = [&](const std::string& why) {
        error = "Team number badge: " + scene + ": " + why;
        return false;
    };
    auto storage = std::make_unique<RuntimeWindowState>();
    RuntimeWindowState& s = *storage;
    seedRuntimeLoadScratch(s, live, false);
    s.config.computerAiEnabled = false; s.visionEnabled = false;
    s.runtimeSuppressPlayerAudioFeedback = true; s.runtimeSoundsEnabled = false;
    s.runtimePreferencePersistenceEnabled = false;
    s.economy.owner = "Americans";
    s.behaviors.clear(); s.projectiles.clear(); s.impactEffects.clear();
    s.world.accessObjects().clear(); s.world.accessTerrainObjects().clear();
    s.world.accessSmudges().clear(); s.world.accessTerrainCells().clear();
    for (int y = 45; y <= 83; ++y) for (int x = 45; x <= 83; ++x) {
        TerrainCell cell; cell.x=x; cell.y=y; cell.cellIndex512=runtimeCellKey(x,y); cell.tileIndex=0;
        s.world.accessTerrainCells().push_back(cell);
    }
    initializeRuntimeNavigation(s);
    auto* tank = addRuntimeObjectInstance(s, RuntimeBuildKind::Unit, "MTNK", {52,52});
    if (!tank) return fail("tank missing");
    const int tankId = tank->instanceId;
    for (auto& object : s.world.accessObjects()) {
        object.buildAnimTicksRemaining = object.buildAnimTicksTotal = 0;
        object.mission = "Guard";
    }
    rebuildRuntimeEconomyPower(s);
    const auto raster = [&](std::vector<std::uint32_t>& pixels) {
        return drawRuntimeStateOffscreenAtCell(
            s, {52,52}, 640, 480, error, "", false, true, -1, &pixels).has_value();
    };

    // 选中 + 属于 Ctrl 编队 1：真实渲染路径必须画出编号角标。
    scene = "badge drawn for selected team member";
    applySelection(s, {tankId}, false, false);
    s.playerLocalCommands.teamCreate(0, {tankId}, [](int) { return true; });
    std::vector<std::uint32_t> withBadge;
    if (!raster(withBadge)) return fail(error);
    if (s.lastTeamNumberBadges != 1) {
        return fail("expected 1 team number badge, got " + std::to_string(s.lastTeamNumberBadges));
    }

    // 同一选中，但不属于任何编队：不画角标，且画面确有可见差异。
    scene = "badge suppressed without team membership";
    s.playerLocalCommands.teams[0].clear();
    std::vector<std::uint32_t> withoutBadge;
    if (!raster(withoutBadge)) return fail(error);
    if (s.lastTeamNumberBadges != 0) {
        return fail("badge drawn although the unit belongs to no team");
    }
    if (withBadge.size() != withoutBadge.size() || withBadge == withoutBadge) {
        return fail("badge produced no visible pixel difference");
    }

    // 编号取最小编组：单位先编入 3 号再编入 1 号，显示必须从 3 变成 1。
    scene = "lowest group number wins";
    s.playerLocalCommands.teamCreate(2, {tankId}, [](int) { return true; });
    std::vector<std::uint32_t> groupThreePixels;
    if (!raster(groupThreePixels)) return fail(error);
    if (s.lastTeamNumberBadges != 1) {
        return fail("group 3 badge missing");
    }
    s.playerLocalCommands.teams[0] = {tankId};
    std::vector<std::uint32_t> groupOnePixels;
    if (!raster(groupOnePixels)) return fail(error);
    if (s.lastTeamNumberBadges != 1) {
        return fail("group 1 badge missing");
    }
    if (groupOnePixels == groupThreePixels) {
        return fail("digit did not follow the lowest assigned group");
    }
    std::cout << "Team number badge: passed selected-member badge, suppression and lowest-group cases\n";
    return true;
}
