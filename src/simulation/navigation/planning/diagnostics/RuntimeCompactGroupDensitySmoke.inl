// Independent capacity oracle for real stock-rule group commands and arrivals.
// Allocation quality and the eventual occupied cells are measured separately.
bool runRuntimeCompactGroupDensitySmoke(const RuntimeConfig& config, std::string& error) {
    std::cout << std::unitbuf;
    const char* requested = std::getenv("CONCORD_COMPACT_GROUPS_OUTPUT");
    const std::filesystem::path root = requested && *requested ? requested
        : "build/smoke/feature-set-19/06-compact-groups/checks";
    const bool diagnostic = std::getenv("CONCORD_COMPACT_GROUPS_DIAGNOSTIC") != nullptr;
    std::filesystem::create_directories(root);
    std::ofstream units(root / "units.csv"), summaries(root / "groups.csv");
    if (!units || !summaries) { error = "Compact groups: cannot open evidence files"; return false; }
    // CSV layer identifies the command's movement group. The airborne column
    // records the actual final layer; collision checks always use actual state.
    units << "scene,hz,id,type,layer,start_x,start_y,target_x,target_y,final_x,final_y,"
        "render_x,render_y,subcell,subcell_x,subcell_y,airborne,render_active,behavior_state,"
        "settled_tick,stable_ticks,result\n";
    summaries << "scene,hz,layer,units,minimum_cells,minimum_radius,target_cells,target_min_x,"
        "target_min_y,target_max_x,target_max_y,target_radius,target_bbox_area,target_density,"
        "final_cells,final_min_x,final_min_y,final_max_x,final_max_y,final_radius,"
        "final_bbox_area,final_density,render_radius,settled_tick,stable_ticks,result\n";
    auto stock = std::make_unique<RuntimeWindowState>();
    stock->config = config; stock->config.playerSlots.clear();
    stock->config.gameModeRulesOverride.clear();
    stock->report = buildRuntimeStartupReport(stock->config, &stock->world,
        &stock->theater, &stock->art, &stock->rules, &stock->mapFacts);
    if (!stock->report.success) { error = "Compact groups: original resources unavailable"; return false; }
    struct Scenario { const char* name; int tanks; int infantry; int flyers; const char* airType;
        int ships; bool tanksFirst; int hz; };
    const std::array<Scenario, 13> scenarios{{
        {"2MTNK",2,0,0,"",0,false,30}, {"5MTNK",5,0,0,"",0,false,30},
        {"11MTNK",11,0,0,"",0,false,30}, {"12MTNK",12,0,0,"",0,false,30},
        {"12E1_2MTNK_foot_first",2,12,0,"",0,false,30},
        {"12E1_2MTNK_tank_first",2,12,0,"",0,true,30},
        {"3MTNK_3SHAD",3,0,3,"SHAD",0,true,30},
        {"5DEST",0,0,0,"",5,false,30}, {"5SHAD",0,0,5,"SHAD",0,false,30},
        {"5JUMPJET",0,0,5,"JUMPJET",0,false,30}, {"7E1",0,7,0,"",0,false,30},
        {"2MTNK_15Hz",2,0,0,"",0,false,15}, {"2MTNK_60Hz",2,0,0,"",0,false,60}
    }};
    constexpr RuntimeCellPoint goal{32,28};
    // An unobstructed square of Chebyshev radius r has (2r+1)^2 cells.
    // Ground infantry need ceil(n/3) exclusive cells, other movers one each;
    // the air layer has its own capacity. No formation helper supplies this oracle.
    const auto minimumRing = [](int cells) {
        int radius = 0; while ((2*radius+1)*(2*radius+1) < cells) ++radius; return radius;
    };
    struct Bounds {
        std::set<std::pair<int,int>> cells;
        int count=0, minX=100000, minY=100000, maxX=-100000, maxY=-100000, radius=0;
        void add(RuntimeCellPoint cell, RuntimeCellPoint center) {
            ++count; cells.emplace(cell.x,cell.y);
            minX=std::min(minX,cell.x); minY=std::min(minY,cell.y);
            maxX=std::max(maxX,cell.x); maxY=std::max(maxY,cell.y);
            radius=std::max(radius,std::max(std::abs(cell.x-center.x),std::abs(cell.y-center.y)));
        }
        int area() const { return count ? (maxX-minX+1)*(maxY-minY+1) : 0; }
        double density() const { return count ? static_cast<double>(count)/area() : 0.0; }
    };
    int cases=0, failures=0;
    std::string firstFailure;
    for (const auto& scenario : scenarios) {
        ++cases;
        auto owned=std::make_unique<RuntimeWindowState>(); auto& state=*owned;
        RuntimeSnapshotActiveStateScope scope(state);
        state.config=stock->config; state.config.fixedTickHz=scenario.hz;
        state.config.gameMode="Battle";
        state.config.computerAiEnabled=state.config.multiplayerActive=state.config.noCooldown=false;
        state.config.crates=state.config.superweapons=state.config.shortGame=false;
        state.rules=stock->rules; state.art=stock->art; state.theater=stock->theater;
        state.report=stock->report; state.mapFacts=stock->mapFacts;
        state.rulesLoaded=state.artLoaded=state.theaterLoaded=state.mapFactsLoaded=true;
        state.report.map.size.clear(); state.report.map.localSize.clear(); state.report.map.width=-1;
        state.visionEnabled=state.runtimeSoundsEnabled=state.runtimePreferencePersistenceEnabled=false;
        state.runtimeSuppressPlayerAudioFeedback=true; state.economy.owner="Americans";
        state.economy.skirmishStartApplied=true; state.triggers.loaded=state.triggers.bootstrapComplete=true;
        RuntimeHumanHouseAuthority authority; authority.owner=state.economy.owner;
        authority.canonicalOwner=runtimeCanonicalHumanHouseOwner(authority.owner);
        authority.localFacade=true; state.humanHouseAuthorities.push_back(std::move(authority));
        for (int y=0; y<52; ++y) for (int x=0; x<52; ++x) {
            TerrainCell cell; cell.x=x; cell.y=y; cell.cellIndex512=runtimeCellKey(x,y);
            cell.tileIndex=cell.subTileIndex=0; state.world.accessTerrainCells().push_back(cell);
        }
        std::vector<int> ids;
        std::map<int,RuntimeCellPoint> starts;
        std::map<int,std::string> types;
        std::set<int> expectedAir;
        std::vector<std::string> reasons;
        const auto problem=[&](const std::string& reason) {
            if (std::find(reasons.begin(),reasons.end(),reason)==reasons.end()) reasons.push_back(reason);
        };
        const auto spawn=[&](const char* type, int count, bool air) {
            for (int i=0; i<count; ++i) {
                const int index=static_cast<int>(ids.size());
                const RuntimeCellPoint start{6+4*(index%4),8+5*(index/4)};
                auto* object=addRuntimeObjectInstance(state,runtimeBuildKindForUnitTypeId(state.rules,type),type,start);
                if (!object) { problem(std::string("spawn_failed_")+type); continue; }
                object->harvestAutoSearchSuspended=true;
                ids.push_back(object->instanceId); starts.emplace(object->instanceId,start);
                types.emplace(object->instanceId,type); if (air) expectedAir.insert(object->instanceId);
            }
        };
        if (scenario.tanksFirst) { spawn("MTNK",scenario.tanks,false); spawn("E1",scenario.infantry,false); }
        else { spawn("E1",scenario.infantry,false); spawn("MTNK",scenario.tanks,false); }
        spawn(scenario.airType,scenario.flyers,true); spawn("DEST",scenario.ships,false);
        state.terrainByCell=buildRuntimeTerrainLookup(state.world); initializeRuntimeNavigation(state);
        for (auto& [key,info] : state.terrainInfoByCell) {
            (void)key; info=RuntimeTerrainInfo{};
            info.kind=scenario.ships ? RuntimeTerrainKind::Water : RuntimeTerrainKind::Land;
            info.authoredLandType=info.movementLandType=scenario.ships
                ? RuntimeTerrainLandType::Water : RuntimeTerrainLandType::Clear;
            info.water=scenario.ships!=0; info.passable=true; info.cost=10;
        }
        state.pathTerrainCache.valid=state.pathTerrainCache.dynamicValid=false;
        std::map<int,RuntimeCellPoint> destinations;
        state.selectedObjectIds=ids; issueRuntimeMoveCommand(state,goal,&destinations);
        const int expectedCount=scenario.tanks+scenario.infantry+scenario.flyers+scenario.ships;
        if (static_cast<int>(ids.size())!=expectedCount || static_cast<int>(destinations.size())!=expectedCount
            || state.lastCommand.acceptedCount!=expectedCount) problem("command_not_fully_accepted");
        const std::array<int,2> minimumCells{{scenario.tanks+scenario.ships+(scenario.infantry+2)/3,scenario.flyers}};
        std::array<Bounds,2> targets,finals;
        std::array<double,2> renderRadius{{0.0,0.0}};
        for (const auto& [id,cell] : destinations) targets[expectedAir.count(id)?1:0].add(cell,goal);
        for (int layer=0; layer<2; ++layer) if (targets[layer].count
            && targets[layer].radius>minimumRing(minimumCells[layer]))
            problem(layer ? "air_target_not_compact" : "ground_target_not_compact");
        for (int layer=0; layer<2; ++layer) if (minimumCells[layer]
            && !targets[layer].cells.count({goal.x,goal.y}))
            problem(layer ? "air_target_center_unused" : "ground_target_center_unused");
        // Full simulation, including interpolation, reservations, collision and
        // queued movement. A command's target is never substituted for its arrival.
        // 落位归属取证：地面单位一旦停到自己的目标格又被挪走，记下是哪一拍、
        // 被谁（含对方 airborne）挤掉的。没有这段，失败只剩「收得散」这句话，
        // 分不清是分配器给的格就散、还是落位后被别的层抢了格。
        struct PushTrace { int reached=-1; int left=-1; int fromX=0, fromY=0, toX=0, toY=0;
            int blocker=-1; bool blockerAirborne=false; std::string blockerType; };
        std::map<int,PushTrace> pushTraces;
        const auto noteTargetOwnership=[&]() {
            for (const int id : ids) {
                if (expectedAir.count(id)) continue;
                const auto assigned=destinations.find(id); if (assigned==destinations.end()) continue;
                const auto* object=findObjectById(state,id); if (!object || !object->alive) continue;
                auto& trace=pushTraces[id];
                const auto cell=runtimeCellPointForObject(*object);
                if (cell==assigned->second) { if (trace.reached<0) trace.reached=static_cast<int>(state.tick); continue; }
                if (trace.reached<0 || trace.left>=0) continue;
                trace.left=static_cast<int>(state.tick);
                trace.fromX=assigned->second.x; trace.fromY=assigned->second.y;
                trace.toX=cell.x; trace.toY=cell.y;
                for (const int other : ids) {
                    if (other==id) continue;
                    const auto* occupier=findObjectById(state,other);
                    if (!occupier || !occupier->alive) continue;
                    if (runtimeCellPointForObject(*occupier)!=assigned->second) continue;
                    trace.blocker=other; trace.blockerAirborne=occupier->airborne;
                    trace.blockerType=types.at(other); break;
                }
            }
        };
        int settledTick=-1, stableTicks=0;
        bool everMoved=false, settled=false;
        std::map<int,std::tuple<int,int,int,double,double>> settledPositions;
        const auto inspectOccupancy=[&]() {
            std::map<std::tuple<int,int,bool,bool>,std::set<int>> occupied;
            for (const int id : ids) {
                const auto* object=findObjectById(state,id);
                if (!object || !object->alive || object->health<=0) { problem("mover_lost"); continue; }
                auto& slots=occupied[{object->x,object->y,object->onBridge,object->airborne}];
                const bool foot=object->kind==WorldObjectKind::Infantry && !object->airborne;
                if (foot) {
                    if (!RuntimeSubcellSystemState::isActiveSubCell(object->subCell)
                        || slots.count(-1) || !slots.insert(object->subCell).second)
                        problem("ground_subcell_overlap");
                } else {
                    if (!slots.empty()) problem(object->airborne ? "air_cell_overlap" : "ground_cell_overlap");
                    slots.insert(-1);
                }
            }
        };
        inspectOccupancy();
        for (int tick=0; tick<scenario.hz*240; ++tick) {
            ++state.tick; advanceRuntimeSimulation(state); inspectOccupancy(); noteTargetOwnership();
            bool allStopped=static_cast<int>(ids.size())==expectedCount;
            for (const int id : ids) {
                const auto* object=findObjectById(state,id);
                if (!object || !object->alive) { allStopped=false; continue; }
                everMoved=everMoved || runtimeCellPointForObject(*object)!=starts.at(id);
                const auto behavior=state.behaviors.find(id);
                if (object->renderMoveActive || (behavior!=state.behaviors.end()
                    && behavior->second.state!=RuntimeBehaviorState::Complete)) allStopped=false;
            }
            if (!allStopped) {
                if (settledTick>=0) problem("movement_restarted_after_completion");
                stableTicks=0; continue;
            }
            if (settledTick<0) {
                settledTick=static_cast<int>(state.tick);
                for (const int id : ids) {
                    const auto* object=findObjectById(state,id);
                    settledPositions[id]={object->x,object->y,object->subCell,
                        runtimeObjectRenderX(*object),runtimeObjectRenderY(*object)};
                }
            }
            for (const int id : ids) {
                const auto* object=findObjectById(state,id);
                const auto pose=std::make_tuple(object->x,object->y,object->subCell,
                    runtimeObjectRenderX(*object),runtimeObjectRenderY(*object));
                if (pose!=settledPositions.at(id)) problem("settled_pose_changed");
            }
            if (++stableTicks>=scenario.hz*3) { settled=true; break; }
        }
        if (!settled) problem("arrival_timeout");
        if (!everMoved) problem("no_actual_movement");
        for (const int id : ids) {
            const auto* object=findObjectById(state,id); if (!object) continue;
            const int layer=expectedAir.count(id)?1:0;
            finals[layer].add(runtimeCellPointForObject(*object),goal);
            const auto offset=worldObjectSubCellOffset(*object);
            const double x=runtimeObjectRenderX(*object), y=runtimeObjectRenderY(*object);
            renderRadius[layer]=std::max(renderRadius[layer],std::max(std::abs(x+offset.x-goal.x),std::abs(y+offset.y-goal.y)));
            if (std::abs(x-object->x)>1e-6 || std::abs(y-object->y)>1e-6) problem("render_did_not_reach_logical_cell");
            if (runtimeCellPointForObject(*object)==starts.at(id)) problem("mover_never_left_start");
        }
        for (int layer=0; layer<2; ++layer) if (finals[layer].count
            && finals[layer].radius>minimumRing(minimumCells[layer]))
            problem(layer ? "air_final_not_compact" : "ground_final_not_compact");
        // Seven infantry can occupy three cells. Checking only whether any cell
        // holds three people would miss a scattered group after crowded arrival.
        if (scenario.infantry && !scenario.tanks && !scenario.ships
            && finals[0].cells.size()!=static_cast<std::size_t>(minimumCells[0])) problem("infantry_final_cells_not_packed");
        // 只在「地面层确实散到半径外」时才点名。逐格对齐本身不是判据：11MTNK 有 7 台
        // 没停进分配格，但整体仍收在半径内、合法。把「没到分配格」当选判据会把绿场景判红。
        const bool groundScattered = minimumCells[0]!=0 && finals[0].count!=0
            && finals[0].radius>minimumRing(minimumCells[0]);
        if (groundScattered) {
        // 停到目标格又被挤走的，逐个点名（谁挤的、挤到哪、对方当时是否在空中）。
        for (const auto& [id, trace] : pushTraces) {
            if (trace.left<0) continue;
            problem("ground_target_reclaimed: "+types.at(id)+"#"+std::to_string(id)
                +" ("+std::to_string(trace.fromX)+","+std::to_string(trace.fromY)+")@"+std::to_string(trace.reached)
                +"->("+std::to_string(trace.toX)+","+std::to_string(trace.toY)+")@"+std::to_string(trace.left)
                +" by "+(trace.blockerType.empty()?std::string("none"):trace.blockerType)+"#"+std::to_string(trace.blocker)
                +" airborne="+(trace.blockerAirborne?"1":"0"));
        }
        // 压根没到过目标格的，也要点名：它拿到的格现在被谁占着（空着 = 不是抢格，是改派）。
        for (const int id : ids) {
            if (expectedAir.count(id)) continue;
            const auto assigned=destinations.find(id);
            if (assigned==destinations.end()) continue;
            const auto* object=findObjectById(state,id);
            if (!object || !object->alive) continue;
            const auto rest=runtimeCellPointForObject(*object);
            if (rest==assigned->second) continue;
            const auto trace=pushTraces.find(id);
            if (trace!=pushTraces.end() && trace->second.left>=0) continue;
            int holder=-1; bool holderAirborne=false; std::string holderType="none";
            for (const int other : ids) {
                if (other==id) continue;
                const auto* occupier=findObjectById(state,other);
                if (!occupier || !occupier->alive) continue;
                if (runtimeCellPointForObject(*occupier)!=assigned->second) continue;
                holder=other; holderAirborne=occupier->airborne; holderType=types.at(other); break;
            }
            problem("ground_target_unreached: "+types.at(id)+"#"+std::to_string(id)
                +" want("+std::to_string(assigned->second.x)+","+std::to_string(assigned->second.y)+")"
                +" got("+std::to_string(rest.x)+","+std::to_string(rest.y)+")"
                +" holder="+holderType+"#"+std::to_string(holder)
                +" airborne="+(holderAirborne?"1":"0"));
        }
        }
        std::string result="PASS";
        if (!reasons.empty()) {
            result.clear(); for (const auto& reason : reasons) { if (!result.empty()) result+=';'; result+=reason; }
            ++failures; if (firstFailure.empty()) firstFailure=std::string(scenario.name)+": "+result;
        }
        for (const int id : ids) {
            const auto* object=findObjectById(state,id); if (!object) continue;
            const auto assigned=destinations.find(id);
            const RuntimeCellPoint target=assigned==destinations.end()?RuntimeCellPoint{-1,-1}:assigned->second;
            const auto offset=worldObjectSubCellOffset(*object); const auto behavior=state.behaviors.find(id);
            units << scenario.name << ',' << scenario.hz << ',' << id << ',' << types.at(id) << ','
                << (expectedAir.count(id)?"air":"ground") << ',' << starts.at(id).x << ',' << starts.at(id).y
                << ',' << target.x << ',' << target.y << ',' << object->x << ',' << object->y << ',' << std::setprecision(12)
                << runtimeObjectRenderX(*object) << ',' << runtimeObjectRenderY(*object) << ',' << object->subCell
                << ',' << offset.x << ',' << offset.y << ',' << object->airborne << ',' << object->renderMoveActive
                << ',' << (behavior==state.behaviors.end()?-1:static_cast<int>(behavior->second.state))
                << ',' << settledTick << ',' << stableTicks << ',' << result << '\n';
        }
        for (int layer=0; layer<2; ++layer) if (minimumCells[layer]) {
            const auto& a=targets[layer]; const auto& b=finals[layer];
            summaries << scenario.name << ',' << scenario.hz << ',' << (layer?"air":"ground") << ','
                << b.count << ',' << minimumCells[layer] << ',' << minimumRing(minimumCells[layer]) << ','
                << a.cells.size() << ',' << a.minX << ',' << a.minY << ',' << a.maxX << ',' << a.maxY << ','
                << a.radius << ',' << a.area() << ',' << a.density() << ',' << b.cells.size() << ','
                << b.minX << ',' << b.minY << ',' << b.maxX << ',' << b.maxY << ',' << b.radius << ','
                << b.area() << ',' << b.density() << ',' << renderRadius[layer] << ',' << settledTick << ',' << stableTicks << ',' << result << '\n';
        }
        units.flush(); summaries.flush();
        std::cout << "Compact groups: " << scenario.name << " hz=" << scenario.hz
            << " accepted=" << state.lastCommand.acceptedCount << " settled=" << settledTick
            << " stable=" << stableTicks << " groundRadius=" << finals[0].radius
            << " airRadius=" << finals[1].radius << " " << result << '\n';
        if (!reasons.empty() && !diagnostic) break;
    }
    std::cout << "Compact groups: cases=" << cases << " passed=" << cases-failures << " failed=" << failures << '\n';
    if (failures) { error="Compact groups: "+std::to_string(failures)+" failed; "+firstFailure; return false; }
    error.clear(); return true;
}
