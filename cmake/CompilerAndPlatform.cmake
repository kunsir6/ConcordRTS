# with the same container element layout.
set(CONCORD_RULES_ABI_REVISION 2026092202)
include("${CMAKE_CURRENT_LIST_DIR}/CoreCompilerProfiles.cmake")
if(CONCORD_PORTABLE_CORE)
    include("${CMAKE_CURRENT_LIST_DIR}/CoreCompilerAndPlatform.cmake")
    return()
endif()

# macOS platform axis.  Not taken on Windows, so PlatformApple.cmake is never
# read there and cannot affect the existing build in any way.
if(APPLE)
    include("${CMAKE_CURRENT_LIST_DIR}/PlatformApple.cmake")
endif()

target_compile_definitions(
    concord_rules_dump PRIVATE
    CONCORD_RULES_ABI_REVISION=${CONCORD_RULES_ABI_REVISION}
)
concord_runtime_abi_definition(concord_runtime)

if(CONCORD_MSVC_FRONTEND)
    concord_core_fixture_options(concord_map_data_smoke)
    concord_core_fixture_options(concord_game_world_deterministic_hash_smoke)
    concord_target_compile_options(concord_campaign_progress_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(
        concord_campaign_map_selection_smoke PRIVATE /utf-8 /FS
    )
    concord_target_compile_options(
        concord_runtime_campaign_movie_resolver_smoke PRIVATE /utf-8 /FS
    )
    concord_target_compile_options(
        concord_campaign_presentation_decoder_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(concord_rules_special_features_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_rules_pad_aircraft_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_rules_production_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_rules_lookup_dispatch_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_rules_overlay_identity_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_rules_crate_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(
        concord_aircraft_production_matcher_smoke PRIVATE /utf-8 /FS
    )
    concord_target_compile_options(concord_particle_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_garrison_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_subcell_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_subcell_selective_batch_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_repair_depot_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_factory_delivery_state_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_grinding_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_gate_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_world_audio_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(
        concord_audio_sample_resolver_edition_smoke PRIVATE /utf-8 /FS
    )
    concord_target_compile_options(concord_power_audio_feedback_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_lighting_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_production_lanes_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_production_timing_smoke PRIVATE /utf-8 /FS)
    concord_core_fixture_options(concord_runtime_atomic_file_smoke)
    concord_target_compile_options(concord_runtime_map_package_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_spy_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_airstrike_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_death_weapon_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_psychic_status_system_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(concord_special_mechanisms_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(
        concord_special_building_system_smoke PRIVATE /utf-8 /FS
    )
    concord_target_compile_options(
        concord_hospital_armory_mod_compat_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive- /wd4189 /wd4996
    )
    concord_target_compile_options(concord_cooperative_scenario_smoke PRIVATE /utf-8 /FS)
    concord_target_compile_options(
        concord_cooperative_session_system_smoke PRIVATE /utf-8 /FS
    )
    concord_target_compile_options(
        concord_multiplayer_session_system_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_core_fixture_options(concord_lan_transport_smoke)
    concord_target_compile_options(
        concord_multiplayer_protocol_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_core_fixture_options(concord_multiplayer_gameplay_command_smoke)
    concord_core_fixture_options(concord_multiplayer_lockstep_smoke)
    concord_target_compile_options(
        concord_multiplayer_weak_network_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_multiplayer_lan_flow_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_core_fixture_options(concord_multiplayer_match_setup_smoke)
    concord_core_fixture_options(concord_multiplayer_snapshot_transfer_smoke)
    concord_core_fixture_options(concord_multiplayer_compatibility_fingerprint_smoke)
    concord_target_compile_options(
        concord_multiplayer_launch_config_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_multiplayer_lobby_model_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_multiplayer_runtime_bridge_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_multiplayer_two_process_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_multiplayer_lobby_flow_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_multiplayer_player_intent_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_connectivity_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_coordinator_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_matchmaking_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_lobby_flow_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_tcp_tunnel_proxy_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_tls_transport_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_service_transport_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_service_host_core
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_service
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_service_host_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_service_lobby_e2e
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_service_host_config_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_internet_multiplayer_menu_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive-
    )
    concord_target_compile_options(
        concord_multiplayer_menu_smoke
        PRIVATE /utf-8 /FS /W4 /WX /permissive- /wd4189
    )
    concord_target_compile_options(
        concord_rules_dump PRIVATE
        /utf-8
        /FS
        $<$<CONFIG:Debug>:/RTC1>
    )
    # Runtime rendering is dominated by large CPU pixel loops.  Keeping those
    # loops at /Od makes the normal Visual Studio Debug launch more than an
    # order of magnitude slower than Release and masks real gameplay costs.
    # Preserve debug symbols/assertions while compiling the playable runtime
    # with optimized code in Debug as well.
    # This target compiles its own C++ sources and links only platform import
    # libraries. Use the production CRT in every configuration as well: the
    # Debug heap otherwise adds bookkeeping to the hot simulation allocations.
    # Keep the existing Debug symbols/assertions and iterator ABI setting.
    concord_runtime_compile_options(concord_runtime)
endif()

concord_runtime_crt_profile(concord_runtime)
if (WIN32)
    # Explorer/shortcut launches should open only the game window.
    set_property(TARGET concord_runtime PROPERTY WIN32_EXECUTABLE TRUE)
    target_link_libraries(
        concord_runtime PRIVATE winmm ws2_32 bcrypt secur32 crypt32 comdlg32 xaudio2 ole32 xapobase
    )
    target_link_libraries(concord_multiplayer_menu_smoke PRIVATE bcrypt)
    target_link_libraries(concord_lan_transport_smoke PRIVATE ws2_32)
    target_link_libraries(concord_multiplayer_protocol_smoke PRIVATE ws2_32)
    target_link_libraries(concord_multiplayer_lan_flow_smoke PRIVATE ws2_32)
    target_link_libraries(
        concord_multiplayer_runtime_bridge_smoke PRIVATE ws2_32
    )
    target_link_libraries(
        concord_multiplayer_two_process_smoke PRIVATE ws2_32
    )
    target_link_libraries(
        concord_multiplayer_lobby_flow_smoke PRIVATE ws2_32
    )
    target_link_libraries(
        concord_internet_lobby_flow_smoke PRIVATE ws2_32 bcrypt
    )
    target_link_libraries(
        concord_internet_tcp_tunnel_proxy_smoke PRIVATE ws2_32 bcrypt
    )
    target_link_libraries(
        concord_internet_tls_transport_smoke
        PRIVATE ws2_32 secur32 crypt32 bcrypt advapi32
    )
    target_link_libraries(
        concord_internet_service_transport_smoke
        PRIVATE ws2_32 secur32 crypt32 bcrypt advapi32
    )
    target_link_libraries(
        concord_internet_service_host_core
        PUBLIC ws2_32 secur32 crypt32 bcrypt advapi32
    )
    target_link_libraries(
        concord_internet_multiplayer_menu_smoke PRIVATE bcrypt
    )
elseif(APPLE)
    # macOS needs no import libraries to replace the Win32 list above.  Frameworks
    # are linked on demand through concord_apple_link_frameworks() at the point
    # where the code that needs them is introduced, so nothing is pulled in early.
    # Print the resolved capability set, because the most common mistake while a
    # port is in progress is believing a backend is enabled when its switch is OFF.
    message(
        STATUS
        "concord_apple: sdl3-window=${CONCORD_APPLE_SDL3_WINDOW} sdl3-audio=${CONCORD_APPLE_SDL3_AUDIO} software-present=${CONCORD_APPLE_SOFTWARE_PRESENT} metal-present=${CONCORD_APPLE_METAL_PRESENT}"
    )
    # S1: point the include path at the declaration-only Win32 shim before this
    # point's targets are created, so `#include <windows.h>` resolves on macOS
    # without touching a single source file.
    concord_apple_enable_win32_compat()
    # S2: the shim now has real object code behind part of its declarations.
    # Attach it to the runtime target so those symbols resolve at link time, and
    # build the compat layer's own pixel-level test binary alongside.
    if(TARGET concord_runtime)
        concord_apple_add_win32_compat_sources(concord_runtime)
    endif()
    concord_apple_add_win32_compat_selftest()
    # P4-01a：macOS 原生音频底座（WAVE 解码 + AudioQueue 输出流）。
    # 放在 APPLE 分支末尾，既不触碰上面的 if(WIN32) 分支，也不影响 Windows 的
    # 任何目标——Windows 侧这个文件根本不会被 include。
    include("${CMAKE_CURRENT_LIST_DIR}/AppleAudioContracts.cmake")
    # T-03：宽字符运行期语义（含 BMP 外码点不丢字符）。
    include("${CMAKE_CURRENT_LIST_DIR}/AppleTextContracts.cmake")
    # S8b：两进程 smoke 是**唯一没有 #if 保护**的 Win32 调用方（产品路径全在
    # #ifdef _WIN32 里），它需要的 24 个 Win32 函数全由 compat 层提供。
    # 必须挂在这条 APPLE 分支里：DevelopmentTargets.cmake 在 CompilerAndPlatform
    # 之前执行，在那里调用 concord_apple_add_win32_compat_sources 会因为"函数
    # 尚未定义"被 if(COMMAND) 静默跳过——症状是 smoke 永远只能 -fsyntax-only。
    if(TARGET concord_multiplayer_two_process_smoke)
        concord_apple_add_win32_compat_sources(concord_multiplayer_two_process_smoke)
    endif()
    # ARM-02：窗口父子语义契约只调 Win32（CreateWindowExW 等），Windows 侧靠真 SDK，
    # macOS 侧靠 compat 层。它同样由 DevelopmentTargets.cmake 创建，那时 compat 辅助
    # 函数尚未定义，所以只能在这里挂源清单——否则链接期报 `_CreateWindowExW` 未定义。
    if(TARGET concord_window_parent_win32_contract)
        concord_apple_add_win32_compat_sources(concord_window_parent_win32_contract)
    endif()
endif()


include("${CMAKE_CURRENT_LIST_DIR}/ComputerTacticsCodecContracts.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/SuperWeaponDiagnosticLibraries.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/LoadTransactionContracts.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ComputerAiTriggerHashContracts.cmake")

concord_finalize_compiler_contract()
