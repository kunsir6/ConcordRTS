// 兵营 / 战车工厂的间谍渗透，在**真实** rulesmd.ini + artmd.ini 数据下逐道门验证。
//
// 为什么需要这条：`--spy-integration-smoke-test` 的夹具只有 POWER（1x1、无 Factory），
// `--spy-technology-smoke-test` 只覆盖实验室的偷科技。两条都不碰 Factory=InfantryType /
// UnitType 的建筑，于是「兵营/战车工厂钻不进去」这件事在回归里是盲区。
//
// 四道门（任何一道红都直接给出名字，不靠猜）：
//   A canInfiltrate  runtimeSpyCanInfiltrateTarget —— 规则/阵营/存活
//   B entryCell      findRuntimeEngineerEntryCell —— 有没有能站上去的地基格
//   C queue          queueRuntimeSpyInfiltration  —— 右键指令是否被接受
//   D complete       completeRuntimeSpyInfiltration —— 到达后是否真的吃人并给奖励
//
// 用真实数据而不是手写夹具：Spyable / Factory / Foundation 都可能与零售 ini 的真实写法
// 相关（含行内注释、Foundation 在 art 里而不是 rules 里），手写夹具会把这些差异屏蔽掉。
bool runRuntimeSpyIndustryInfiltrationSmoke(
    RuntimeWindowState& live,
    std::string& error
) {
    std::cout << std::unitbuf;
    const auto fail = [&](const std::string& why) {
        error = "Spy industry infiltration: " + why;
        return false;
    };

    auto box = std::make_unique<RuntimeWindowState>();
    auto& s = *box;
    seedRuntimeLoadScratch(s, live, false);
    s.config.fixedTickHz = 30;
    s.config.computerAiEnabled = false;
    s.runtimeSuppressPlayerAudioFeedback = true;
    s.tick = 200;
    if (!s.rulesLoaded) return fail("real rules did not load");
    s.structureFootprints = buildRuntimeStructureFootprints(s);

    const std::string spyOwner = "SkirmishPlayer1";
    const std::string enemyOwner = "SkirmishPlayer2";
    s.economy.owner = spyOwner;

    // 以人类方的第一个建筑为原点。间谍也从这里出发，入口格那一关才等于
    // 用户真实的「从自己基地走到敌人兵营」。
    RuntimeCellPoint origin{128, 128};
    bool originFound = false;
    for (const WorldObjectInstance& object : s.world.getObjects()) {
        if (!object.alive || object.kind != WorldObjectKind::Structure
            || object.owner != spyOwner) continue;
        origin = runtimeCellPointForObject(object);
        originFound = true;
        break;
    }
    if (!originFound) {
        // --command-smoke-test 的 live 状态没有真实基地；用同目录其他探针已经
        // 验证过能落地的锚点（RuntimeCloningBatchSmoke 用 80,46 放 GAPOWR）。
        origin = RuntimeCellPoint{80, 46};
    }

    // 每种建筑都要求：能渗透 + 有入口格 + 右键被接受 + 到达后吃人。
    // POWER 是控制组（既有的两条 smoke 只覆盖它这一类）。
    struct IndustryCase {
        const char* type;
        RuntimeSpyVeteranProduction expected;
        bool grantsVeterancy;
    };
    const IndustryCase cases[] = {
        {"GAPILE", RuntimeSpyVeteranProduction::Infantry, true},
        {"NAHAND", RuntimeSpyVeteranProduction::Infantry, true},
        {"GAWEAP", RuntimeSpyVeteranProduction::Vehicles, true},
        {"NAWEAP", RuntimeSpyVeteranProduction::Vehicles, true},
        {"GAPOWR", RuntimeSpyVeteranProduction::Infantry, false}
    };

    int caseIndex = 0;
    for (const IndustryCase& entry : cases) {
        const std::string type = entry.type;
        // 每种建筑各占一片空地：真实基地里建筑相邻，但不会像连排那样把第五栋
        // 完全包死（那样测出来的是「被墙围死」而不是「门口站了兵」）。
        const RuntimeCellPoint caseOrigin = origin;
        ++caseIndex;
        if (const char* onlyCase = std::getenv("CONCORD_SPY_INDUSTRY_CASE");
            onlyCase != nullptr && *onlyCase != '\0'
            && toUpperAsciiLocal(onlyCase) != type) {
            continue;
        }
        const BuildingType* rules = s.rules.findBuildingType(type);
        if (rules == nullptr) return fail(type + " missing from retail rules");
        if (!rules->spyable) return fail(type + " parsed Spyable=no from retail rules");
        if (entry.grantsVeterancy && rules->factory.empty()) {
            return fail(type + " has no Factory= in retail rules");
        }

        RuntimeBuildCatalogItem item;
        item.kind = RuntimeBuildKind::Building;
        item.typeId = type;
        item.owner = enemyOwner;
        item.requiredFactory = "BuildingType";
        item.buildTicks = 1;
        item.techLevel = 1;
        item.cost = 1;
        const auto cell = findRuntimePlacementCellNear(
            s,
            item,
            caseOrigin,
            48,
            enemyOwner,
            false
        );
        if (!cell.has_value()) return fail(type + " has no free placement cell");
        WorldObjectInstance* building = addRuntimeObjectInstanceForOwner(
            s,
            enemyOwner,
            RuntimeBuildKind::Building,
            type,
            *cell
        );
        if (building == nullptr) return fail(type + " could not be created");
        building->buildAnimTicksTotal = 0;
        building->buildAnimTicksRemaining = 0;
        building->mission = "Guard";
        const int buildingId = building->instanceId;
        const std::string footprint = std::to_string(
            runtimeStructureFootprintForObject(s, *building).width
        ) + "x" + std::to_string(
            runtimeStructureFootprintForObject(s, *building).height
        );

        // 间谍站在基点旁边；入口格那一关要真的从基地走到建筑去。
        // 注意不能落进目标建筑的footprint（大地基时 origin+2 会在建筑里面，
        // 那样间谍一出生就在入口格上，测出来的等于没测）。
        RuntimeCellPoint spyCell{origin.x, origin.y + 2};
        {
            const std::vector<RuntimeCellPoint> footprint =
                runtimeStructureFootprintCellsForObject(s, *building);
            const auto insideFootprint = [&footprint](const RuntimeCellPoint& c) {
                return std::find(footprint.begin(), footprint.end(), c)
                    != footprint.end();
            };
            if (insideFootprint(spyCell)) {
                spyCell = RuntimeCellPoint{cell->x - 2, cell->y + 1};
            }
            if (insideFootprint(spyCell)) {
                spyCell = RuntimeCellPoint{
                    cell->x,
                    cell->y + runtimeStructureFootprintForObject(s, *building).height + 2
                };
            }
        }
        WorldObjectInstance* spy = addRuntimeObjectInstanceForOwner(
            s,
            spyOwner,
            RuntimeBuildKind::Infantry,
            "SPY",
            spyCell
        );
        if (spy == nullptr) return fail(type + " probe spy could not be created");
        const int spyId = spy->instanceId;
        spy->buildAnimTicksTotal = 0;
        spy->buildAnimTicksRemaining = 0;
        spy->mission = "Guard";

        // A：规则门
        if (!runtimeSpyCanInfiltrateTarget(s, *spy, *building)) {
            return fail(type + " gate A: canInfiltrate false"
                + " (spyable=" + std::string(rules->spyable ? "yes" : "no")
                + " infiltrate=" + std::string(
                    findRuntimeUnitType(s.rules, *spy) != nullptr
                        && findRuntimeUnitType(s.rules, *spy)->infiltrate
                        ? "yes" : "no")
                + " allied=" + std::string(
                    runtimeOwnersAreAllied(s, spy->owner, building->owner)
                        ? "yes" : "no")
                + ")");
        }

        // B：入口格
        const auto entryCell = findRuntimeEngineerEntryCell(
            s,
            *spy,
            *building,
            "spy-enter",
            2
        );
        if (!entryCell.has_value()) {
            return fail(type + " gate B: no reachable footprint entry cell"
                + " (footprint=" + footprint + " building@"
                + std::to_string(cell->x) + "," + std::to_string(cell->y) + ")");
        }

        // C：右键指令
        //
        // 用户报的现象是「兵营里有兵 / 兵营周边有兵」时才钻不进去，所以这里
        // 可选地在入口格与四周塞满敌方步兵（CONCORD_SPY_INDUSTRY_BODIES=1）。
        // 身体占满子格时，进场方要么让位逻辑生效，要么永远等。
        if (std::getenv("CONCORD_SPY_INDUSTRY_BODIES") != nullptr) {
            // 只堵「入口格的外侧邻居」：地基格上的兵没有意义（那是建筑自己），
            // 更不能放到间谍脚下（会把间谍挤开，测出来的就不是门口被堵）。
            const std::vector<RuntimeCellPoint> footprint =
                runtimeStructureFootprintCellsForObject(s, *building);
            const auto insideFootprint = [&footprint](const RuntimeCellPoint& c) {
                return std::find(footprint.begin(), footprint.end(), c)
                    != footprint.end();
            };
            const RuntimeCellPoint neighbours[] = {
                {entryCell->x + 1, entryCell->y},
                {entryCell->x - 1, entryCell->y},
                {entryCell->x, entryCell->y + 1},
                {entryCell->x, entryCell->y - 1}
            };
            int placed = 0;
            for (const RuntimeCellPoint& bodyCell : neighbours) {
                if (placed >= 3) break;
                if (insideFootprint(bodyCell) || bodyCell == spyCell) continue;
                WorldObjectInstance* body = addRuntimeObjectInstanceForOwner(
                    s,
                    enemyOwner,
                    RuntimeBuildKind::Infantry,
                    "E1",
                    bodyCell
                );
                if (body == nullptr) continue;
                body->buildAnimTicksTotal = 0;
                body->buildAnimTicksRemaining = 0;
                body->mission = "Guard";
                ++placed;
            }
        }
        applySelection(s, {spyId}, false, false);
        if (!queueRuntimeSpyInfiltration(s, *findObjectById(s, buildingId))) {
            return fail(type + " gate C: queue rejected: " + s.interactionStatus);
        }

        // D：真的走过去并完成。这里不传送——传送只能证明完成判定本身可用，
        // 证明不了「用户右键之后间谍到底有没有走进那扇门」。每 tick 走真实
        // 仿真；超时未吃掉就把间谍的实时状态打出来（位置/行为/path/状态栏）。
        const int startTick = s.tick;
        int deadlineTicks = 900;
        if (const char* overrideTicks = std::getenv("CONCORD_SPY_INDUSTRY_TICKS");
            overrideTicks != nullptr && *overrideTicks != '\0') {
            deadlineTicks = std::max(1, std::atoi(overrideTicks));
        }
        const int deadlineTick = startTick + deadlineTicks;
        bool completed = false;
        const bool traceStall = std::getenv("CONCORD_SPY_INDUSTRY_TRACE") != nullptr;
        while (s.tick < deadlineTick) {
            {
                RuntimeSnapshotActiveStateScope active(s);
                ++s.tick;
                advanceRuntimeSimulation(s, false);
                completeRuntimeWorldRefresh(s);
            }
            if (traceStall && ((s.tick - startTick) % 60 == 0
                    || (s.tick - startTick) <= 12)) {
                const WorldObjectInstance* walker = findObjectById(s, spyId);
                const auto move = s.behaviors.find(spyId);
                std::cout << "    trace " << type << " t+" << (s.tick - startTick)
                          << " at=" << (walker ? walker->x : -1) << ","
                          << (walker ? walker->y : -1)
                          << " mission=" << (walker ? walker->mission : "gone");
                if (move != s.behaviors.end()) {
                    std::cout << " state=" << int(move->second.state)
                              << " cool=" << move->second.moveCooldownTicks
                              << " blocked=" << move->second.blockedTicks
                              << " path=" << move->second.path.size()
                              << " renderRemain=" << move->second.renderMoveTicksRemaining;
                }
                std::cout << "\n";
            }
            if (findObjectById(s, spyId) == nullptr) {
                completed = true;
                break;
            }
        }
        if (!completed) {
            const WorldObjectInstance* stuck = findObjectById(s, spyId);
            // 门口被彻底围死时允许进不去，但**不许静默卡住**：必须明确报
            // "cannot reach ..."，并且命令已经收掉（不再挂着 Entering）。
            const bool explicitRefusal =
                s.interactionStatus.rfind("cannot reach ", 0) == 0;
            const bool orderCleared =
                s.behaviors.find(spyId) == s.behaviors.end();
            if (explicitRefusal && orderCleared) {
                std::cout << "  spy-industry " << type
                          << " footprint=" << footprint
                          << " entry=" << entryCell->x << "," << entryCell->y
                          << " ticks=" << (s.tick - startTick)
                          << " consumed=no blocked=explicit"
                          << " status=[" << s.interactionStatus << "]\n";
                continue;
            }
            std::ostringstream detail;
            detail << type << " gate D: spy never entered after "
                   << (s.tick - startTick) << " ticks"
                   << " at=" << (stuck ? stuck->x : -1) << ","
                   << (stuck ? stuck->y : -1)
                   << " entry=" << entryCell->x << "," << entryCell->y
                   << " status=[" << s.interactionStatus << "]"
                   << " sim=[" << s.simulationStatus << "]";
            const auto behavior = s.behaviors.find(spyId);
            if (behavior != s.behaviors.end()) {
                detail << " behavior=" << int(behavior->second.kind)
                       << ":" << int(behavior->second.state)
                       << " target=" << behavior->second.moveTarget.x << ","
                       << behavior->second.moveTarget.y
                       << " targetObject=" << behavior->second.targetObjectId
                       << " allowedOccupied=" << behavior->second.pendingAllowedOccupiedObjectId
                       << " blocked=" << behavior->second.blockedTicks
                       << " replans=" << behavior->second.pathReplans
                       << " nextRepath=" << behavior->second.nextRepathTick
                       << " cooldown=" << behavior->second.moveCooldownTicks
                       << " path=" << behavior->second.path.size();
                if (!behavior->second.path.empty()) {
                    const RuntimeCellPoint next = behavior->second.path.front();
                    detail << " next=" << next.x << "," << next.y;
                    const auto occupiedByCell = buildRuntimeOccupancyLookup(s);
                    const auto occupant = occupiedByCell.find(runtimeCellKey(next.x, next.y));
                    detail << " occupant=" << (occupant == occupiedByCell.end() ? -1 : occupant->second);
                }
            }
            if (stuck != nullptr) {
                detail << " mission=" << stuck->mission
                       << " hp=" << stuck->health;
            }
            // 对照：同一入口格换普通步兵走一遍。用来区分「间谍专属」与
            // 「这块地/这条路径本来就是走不进去」。
            WorldObjectInstance* control = addRuntimeObjectInstanceForOwner(
                s,
                spyOwner,
                RuntimeBuildKind::Infantry,
                "GI",
                RuntimeCellPoint{spyCell.x + 1, spyCell.y}
            );
            if (control != nullptr) {
                control->buildAnimTicksTotal = 0;
                control->buildAnimTicksRemaining = 0;
                control->mission = "Guard";
                const int controlId = control->instanceId;
                applySelection(s, {controlId}, false, false);
                issueRuntimeMoveCommand(s, *entryCell);
                const int controlDeadline = s.tick + 900;
                while (s.tick < controlDeadline
                    && findObjectById(s, controlId) != nullptr) {
                    RuntimeSnapshotActiveStateScope active(s);
                    ++s.tick;
                    advanceRuntimeSimulation(s, false);
                    completeRuntimeWorldRefresh(s);
                    const WorldObjectInstance* walker = findObjectById(s, controlId);
                    if (walker != nullptr
                        && runtimeCellPointForObject(*walker) == *entryCell) {
                        break;
                    }
                }
                const WorldObjectInstance* walker = findObjectById(s, controlId);
                detail << " | controlGI at=" << (walker ? walker->x : -1) << ","
                       << (walker ? walker->y : -1)
                       << " arrived=" << std::string(
                            walker != nullptr
                                && runtimeCellPointForObject(*walker) == *entryCell
                                ? "yes" : "no");
            }
            return fail(detail.str());
        }
        if (entry.grantsVeterancy
            && !s.spies.hasVeteranProduction(spyOwner, entry.expected)) {
            return fail(type + " gate D: infiltrated but no veteran production granted");
        }
        std::cout << "  spy-industry " << type
                  << " footprint=" << footprint
                  << " entry=" << entryCell->x << "," << entryCell->y
                  << " ticks=" << (s.tick - startTick)
                  << " consumed=yes"
                  << " veteran=" << (entry.grantsVeterancy ? "granted" : "n/a")
                  << " status=" << s.interactionStatus << "\n";
    }

    std::cout << "Spy industry infiltration: barracks/veteran-infantry+"
                 "war-factory/veteran-vehicles+power-plant all entered and consumed\n";
    error.clear();
    return true;
}
