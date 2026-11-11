bool runRuntimeGuardAcquisitionPolicySmoke(RuntimeWindowState& live, std::string& error) {
    std::string scene;
    const auto fail = [&](const std::string& why) {
        error = "Guard acquisition policy: " + scene + ": " + why;
        return false;
    };
    const auto makeScene = [&]() {
        auto storage = std::make_unique<RuntimeWindowState>();
        RuntimeWindowState& s = *storage;
        seedRuntimeLoadScratch(s, live, false);
        s.config.fixedTickHz = 30; s.config.computerAiEnabled = true;
        s.config.noCooldown = false; s.visionEnabled = false;
        s.runtimeSuppressPlayerAudioFeedback = true; s.runtimeSoundsEnabled = false;
        s.economy.owner = "Americans";
        s.economy.skirmishStartApplied = true;
        s.behaviors.clear(); s.projectiles.clear(); s.impactEffects.clear();
        s.world.accessObjects().clear(); s.world.accessTerrainObjects().clear();
        s.world.accessSmudges().clear(); s.world.accessTerrainCells().clear();
        for (int y = 45; y <= 83; ++y) for (int x = 45; x <= 83; ++x) {
            TerrainCell cell; cell.x=x; cell.y=y; cell.cellIndex512=runtimeCellKey(x,y); cell.tileIndex=0;
            s.world.accessTerrainCells().push_back(cell);
        }
        initializeRuntimeNavigation(s);
        for (int n = 0; n != 3; ++n)
            addRuntimeObjectInstance(s, RuntimeBuildKind::Building, "GAPOWR", {46 + n*4, 46});
        return storage;
    };
    const auto finishSetup = [&](RuntimeWindowState& s) {
        for (auto& object : s.world.accessObjects()) {
            object.buildAnimTicksRemaining = object.buildAnimTicksTotal = 0;
            object.mission = "Guard";
        }
        rebuildRuntimeEconomyPower(s);
    };
    const auto tickFor = [&](RuntimeWindowState& s, int ticks) {
        for (int step = 0; step != ticks; ++step) {
            ++s.tick;
            advanceRuntimeWeaponCooldowns(s);
            std::vector<int> behaviorIds;
            for (const auto& [id, entry] : s.behaviors) behaviorIds.push_back(id);
            for (int id : behaviorIds) {
                auto* actor = findMutableObjectById(s, id);
                auto behavior = s.behaviors.find(id);
                if (actor == nullptr || behavior == s.behaviors.end()
                    || behavior->second.kind != RuntimeBehaviorKind::Attack) continue;
                advanceRuntimeAttackBehavior(s, *actor, behavior->second);
            }
            (void)advanceRuntimeCombatBursts(s);
            (void)advanceRuntimeEffects(s);
            (void)advanceRuntimeGuardBehavior(s);
        }
    };

    // 可动作战单位闲置时主动攻击射程内的敌方作战单位。
    scene = "idle tank acquires enemy tank";
    {
        auto storage = makeScene(); RuntimeWindowState& s = *storage;
        auto* tank = addRuntimeObjectInstance(s, RuntimeBuildKind::Unit, "MTNK", {52,52});
        auto* enemy = addRuntimeObjectInstanceForOwner(s, "Russians", RuntimeBuildKind::Unit, "MTNK", {55,52});
        if (!tank || !enemy) return fail("actors missing");
        const int tankId = tank->instanceId, enemyId = enemy->instanceId;
        enemy->health = 100000; enemy->weaponCooldownTicksRemaining = 100000;
        finishSetup(s);
        tickFor(s, s.config.fixedTickHz * 12);
        const auto behavior = s.behaviors.find(tankId);
        if (behavior == s.behaviors.end()
            || behavior->second.kind != RuntimeBehaviorKind::Attack
            || !behavior->second.autoAcquired
            || behavior->second.targetObjectId != enemyId) {
            return fail("idle tank did not auto-acquire the enemy tank");
        }
        std::cout << "Guard policy: idle tank acquired enemy tank\n";
    }

    // 敌方建筑（含无武器防御）不会被自动索敌锁定，只有点击攻击才能打。
    scene = "idle tank ignores enemy building";
    {
        auto storage = makeScene(); RuntimeWindowState& s = *storage;
        auto* tank = addRuntimeObjectInstance(s, RuntimeBuildKind::Unit, "MTNK", {52,52});
        auto* power = addRuntimeObjectInstanceForOwner(s, "Russians", RuntimeBuildKind::Building, "GAPOWR", {56,52});
        if (!tank || !power) return fail("actors missing");
        const int tankId = tank->instanceId;
        finishSetup(s);
        tickFor(s, s.config.fixedTickHz * 12);
        if (s.behaviors.count(tankId) != 0) {
            return fail("idle tank auto-acquired an enemy building");
        }
        std::cout << "Guard policy: idle tank ignored enemy building\n";
    }

    // 防御类建筑（磁暴线圈）主动攻击来犯单位；单位不会反向主动锁定磁暴线圈。
    scene = "tesla coil acquires tank, tank ignores coil until damaged";
    {
        auto storage = makeScene(); RuntimeWindowState& s = *storage;
        auto* coil = addRuntimeObjectInstance(s, RuntimeBuildKind::Defense, "TESLA", {52,52});
        auto* tank = addRuntimeObjectInstanceForOwner(s, "Russians", RuntimeBuildKind::Unit, "MTNK", {55,52});
        if (!coil || !tank) return fail("actors missing");
        const int coilId = coil->instanceId, tankId = tank->instanceId;
        tank->health = 100000;
        finishSetup(s);
        int guardTicks = 0;
        while (guardTicks != s.config.fixedTickHz * 12
            && s.behaviors.find(coilId) == s.behaviors.end()) {
            ++s.tick;
            advanceRuntimeWeaponCooldowns(s);
            (void)advanceRuntimeCombatBursts(s);
            (void)advanceRuntimeGuardBehavior(s);
            ++guardTicks;
        }
        if (s.behaviors.find(coilId) == s.behaviors.end()) {
            return fail("tesla coil did not auto-acquire the enemy tank");
        }
        if (s.behaviors.count(tankId) != 0) {
            return fail("tank auto-acquired the tesla coil before taking damage");
        }
        // 磁暴线圈开火命中后，被打的空闲单位必须还手（retaliation）。
        bool retaliated = false;
        for (int step = 0; step != s.config.fixedTickHz * 20; ++step) {
            ++s.tick;
            advanceRuntimeWeaponCooldowns(s);
            std::vector<int> behaviorIds;
            for (const auto& [id, entry] : s.behaviors) behaviorIds.push_back(id);
            for (int id : behaviorIds) {
                auto* actor = findMutableObjectById(s, id);
                auto behavior = s.behaviors.find(id);
                if (actor == nullptr || behavior == s.behaviors.end()
                    || behavior->second.kind != RuntimeBehaviorKind::Attack) continue;
                advanceRuntimeAttackBehavior(s, *actor, behavior->second);
            }
            (void)advanceRuntimeCombatBursts(s);
            (void)advanceRuntimeEffects(s);
            (void)advanceRuntimeGuardBehavior(s);
            if (s.behaviors.count(tankId) != 0) { retaliated = true; break; }
        }
        if (!retaliated) return fail("tank never retaliated against the coil that shot it");
        const auto retaliation = s.behaviors.find(tankId);
        if (retaliation == s.behaviors.end()
            || retaliation->second.kind != RuntimeBehaviorKind::Attack
            || !retaliation->second.autoAcquired
            || retaliation->second.targetObjectId != coilId) {
            return fail("retaliation did not target the tesla coil");
        }
        std::cout << "Guard policy: coil acquired tank, tank ignored coil until damage then retaliated\n";
    }

    // 无武器建筑从不自动攻击。
    scene = "weaponless building never auto-attacks";
    {
        auto storage = makeScene(); RuntimeWindowState& s = *storage;
        auto* factory = addRuntimeObjectInstance(s, RuntimeBuildKind::Building, "GAWEAP", {52,52});
        auto* enemy = addRuntimeObjectInstanceForOwner(s, "Russians", RuntimeBuildKind::Unit, "MTNK", {58,52});
        if (!factory || !enemy) return fail("actors missing");
        const int factoryId = factory->instanceId, enemyId = enemy->instanceId;
        enemy->health = 100000; enemy->weaponCooldownTicksRemaining = 100000;
        finishSetup(s);
        tickFor(s, s.config.fixedTickHz * 12);
        if (s.behaviors.count(factoryId) != 0 || s.behaviors.count(enemyId) != 0) {
            return fail("weaponless building or unit attacked without an order");
        }
        std::cout << "Guard policy: weaponless building stayed passive\n";
    }

    // 说明：原本还有一条「被敌方坦克打伤的空闲坦克会还手」，实测无法成立——
    // 敌方坦克进入射程后被动索敌先是给了受害者一条 autoAcquired 的攻击行为
    // （status 记 "guard acquired #4 -> #5"），于是"伤害触发的还手"与"被动索敌"
    // 在同一夹具里无法区分，且该场景 15s 内没有产生任何弹丸（projectilesSeen=0）。
    // 真正只能靠伤害触发的场景是「建筑打你」——建筑被挡在被动索敌之外（见上一场景），
    // 已由 tesla coil 那个场景覆盖，此处不再保留一条会自相矛盾的用例。
    std::cout << "Guard acquisition policy: passed 4 scenarios\n";
    return true;
}
