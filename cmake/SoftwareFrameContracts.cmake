include_guard(GLOBAL)
# Public complete-frame contract is pure; native interop stays in its own leaf.
add_subdirectory(tests/rendering/software_frame)
# S38 FramePresenter：Apple 走 Dib 软件呈现；D3D11 仍是 Windows 专属。
if(WIN32 OR APPLE)
    concord_add_profiled_leaf(concord_frame_presenter_win32 src/runtime/presentation/win32/FramePresenterWin32.cpp concord_runtime Runtime)
    target_link_libraries(concord_runtime PRIVATE concord_frame_presenter_win32)
    # S41 地图原生叠加层：纯 CPU 逻辑，不依赖 D3D11/fxc.exe。P5-03 把它从 D3D11
    # 链里拆出（原 include 挂在 D3D11FramePresenter.cmake 末尾，而后者带
    # FATAL_ERROR(fxc.exe)，Apple 绝不能 include 它），改在这里与 FramePresenter 同条件。
    include("${CMAKE_CURRENT_LIST_DIR}/MapNativeOverlayWin32.cmake")
endif()
if(WIN32)
    if(CONCORD_MSVC_FRONTEND)
        concord_target_compile_options(concord_frame_presenter_win32 PRIVATE /W4 /WX)
    endif()
    add_subdirectory(tests/infrastructure/frame_presenter)
    include("${CMAKE_CURRENT_LIST_DIR}/D3D11FramePresenter.cmake")
endif()
