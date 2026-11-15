// M-25: 载具穿过友军步兵格。
//
// 用户实测并确认：几个步兵站在一起之后，其他兵种**彻底**过不去（不是减速，
// 是路径根本不存在）。这一条只锁最底层的那一步——「格内只有友军步兵时，
// 载具 mover 认为这一格能不能走」。
//
// 现状（2026-11-14 修复前）：不能走。
//   RuntimeSessionNavigation.cpp 的 passable 分支里，infantryHasSubcell 只对
//   步兵 mover 计算；载具落进 else-if 链的最后一支
//   `!runtimeCanEnterAndCrushCell(...)`，而 runtimeCanEnterAndCrushCell 对
//   同阵营目标恒 false（不压友军）。于是「友军步兵」= 硬阻挡，1 格宽的走廊
//   直接被判成不可通行。
//
// 修复目标（原版语义）：载具可以走进只站着友军步兵的格子，代价更高（拥挤），
// 走到时由运动层把步兵挤开。所以本 smoke 的期望值是 true，修复前它是红的。
//
// 为什么不用「路径规划成功」当判据：路径层会绕路，绕过去了 smoke 也会绿，
// 而用户看到的「明明有空间却过不去」正是绕不过去时的表现。格子级判据才能
// 区分「能过」「绕路」「堵死」三种状态。

bool runRuntimeVehicleInfantryPassThroughSmoke(std::string& error) {
    const auto fail = [&](const std::string& why) {
        // 措辞带 failed：regress 的 pick_evidence 按 *failed* 选证据行，而本冒烟
        // 前面会先打一行 `Failed to open INI file: …2peaks.map`（夹具噪声）——
        // 失败行不含 failed 时，证据行会被那行抢走，known-red 的前缀门禁就钉在
        // 噪声上，真原因换了也照不出来。
        error = "vehicle pass-through failed: " + why;
        return false;
    };
    IniFile ini;
    RulesDatabase rules;
    if (!ini.loadFromString(
            "[VehicleTypes]\n0=CAR\n[InfantryTypes]\n0=MAN\n"
            "[CAR]\nStrength=200\nSpeed=6\nROT=128\nSpeedType=Track\nMovementZone=Normal\n"
            "[MAN]\nStrength=100\nSpeed=6\nROT=128\nSpeedType=Foot\nMovementZone=Normal\n")
        || !rules.loadFromIni(ini)) return fail("fixture rules");

    auto box = std::make_unique<RuntimeWindowState>();
    auto& s = *box;
    s.rules = rules;
    s.rulesLoaded = true;
    s.config.fixedTickHz = 30;
    s.runtimeSoundsEnabled = s.runtimePreferencePersistenceEnabled = false;
    s.runtimeSuppressPlayerAudioFeedback = true;
    s.visionEnabled = false;
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
        TerrainCell c;
        c.x = x; c.y = y; c.cellIndex512 = runtimeCellKey(x, y);
        c.tileIndex = c.subTileIndex = 0;
        s.world.accessTerrainCells().push_back(c);
    }
    initializeRuntimeNavigation(s);
    for (auto& [key, info] : s.terrainInfoByCell) {
        info = RuntimeTerrainInfo{};
        info.authoredLandType = info.movementLandType = RuntimeTerrainLandType::Clear;
    }

    RuntimeSnapshotActiveStateScope active(s);
    const auto add = [&](int id, bool infantry, int x, int y, int slot) {
        WorldObjectInstance a;
        a.instanceId = id;
        a.type = infantry ? "MAN" : "CAR";
        a.kind = infantry ? WorldObjectKind::Infantry : WorldObjectKind::Unit;
        a.owner = "Player";
        a.x = x; a.y = y;
        a.cell = runtimeCellKey(x, y);
        a.health = infantry ? 100 : 200;
        a.alive = a.poweredOperational = true;
        a.subCell = infantry ? slot : -1;
        a.mission = "Guard";
        a.facing = runtimeFacingFromDelta(1, 0);
        s.world.accessObjects().push_back(a);
    };

    // 一格宽的走廊：中央 (12,13) 站着一个友军步兵，其余方向是空地。
    add(1, true, 12, 13, RuntimeSubcellSystemState::activeSubCells()[0]);
    add(2, false, 10, 13, -1);
    ensureRuntimeSubcellsInitialized(s);
    const auto occupancy = buildRuntimeOccupancyLookup(s);

    const WorldObjectInstance* vehicle = findObjectById(s, 2);
    if (vehicle == nullptr) return fail("vehicle missing");
    const bool blockedCellPassable =
        runtimeCellIsPassable(s, s.terrainByCell, occupancy, {12, 13}, *vehicle);

    // 对照组：空地的同一格必须可走，否则说明问题不在步兵而在别的地方。
    const bool emptyCellPassable =
        runtimeCellIsPassable(s, s.terrainByCell, occupancy, {13, 13}, *vehicle);

    std::printf("vehicle pass-through: emptyCell=%d friendlyInfantryCell=%d\n",
                emptyCellPassable ? 1 : 0, blockedCellPassable ? 1 : 0);
    if (!emptyCellPassable) return fail("empty cell not passable (fixture broken)");
    if (!blockedCellPassable) {
        return fail("vehicle refuses a cell occupied only by friendly infantry");
    }
    return true;
}
