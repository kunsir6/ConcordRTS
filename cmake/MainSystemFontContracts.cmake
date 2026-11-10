# W01701 Main owned system fonts; device types stay in this private provider.
include_guard(GLOBAL)

# S4-3：窗口客户区出字闭环的可执行契约（仅 macOS）。
#
# 为什么用函数 + DEFER 而不是就地 add_executable：本文件由 DevelopmentTargets.cmake
# （CMakeLists.txt 第 23 行）include，早于 CompilerAndPlatform.cmake（第 25 行），
# 所以此处 concord_apple_add_win32_compat_sources() 还没有定义，concord_runtime 也
# 还没建完。把创建动作推迟到目录处理结束，既能把这族目标留在字体族旁边，又不依赖
# include 顺序，也不需要改动 if(WIN32) 分支。
function(concord_add_main_system_font_window_paint)
    if(NOT APPLE)
        return()
    endif()
    if(TARGET concord_main_system_font_window_paint)
        return()
    endif()
    add_executable(
        concord_main_system_font_window_paint
        tests/ui/main_system_font/MainSystemFontWindowPaint.cpp
    )
    target_include_directories(concord_main_system_font_window_paint PRIVATE src)
    target_compile_features(concord_main_system_font_window_paint PRIVATE cxx_std_20)
    # 兼容层的源清单、头搜索路径与框架（CoreText / CoreGraphics / ImageIO）一起挂上。
    # 框架依赖随源清单走是 S4-2 定下的约定，不靠调用点记得逐个补。
    concord_apple_add_win32_compat_sources(concord_main_system_font_window_paint)
    # 被测的就是产品字体类的真实实现，直接用 leaf 本体，不复制一份。
    target_link_libraries(
        concord_main_system_font_window_paint
        PRIVATE concord_main_system_font_win32
    )
endfunction()

add_executable(concord_main_system_font_value_contract tests/ui/main_system_font/MainSystemFontValueContract.cpp)
target_include_directories(concord_main_system_font_value_contract PRIVATE src)
target_compile_features(concord_main_system_font_value_contract PRIVATE cxx_std_20)
target_compile_options(concord_main_system_font_value_contract PRIVATE "$<TARGET_GENEX_EVAL:concord_runtime_match_score_contract,$<TARGET_PROPERTY:concord_runtime_match_score_contract,COMPILE_OPTIONS>>")
target_compile_definitions(concord_main_system_font_value_contract PRIVATE "$<TARGET_GENEX_EVAL:concord_runtime_match_score_contract,$<TARGET_PROPERTY:concord_runtime_match_score_contract,COMPILE_DEFINITIONS>>")
if(CONCORD_MSVC_ABI)
 set_property(TARGET concord_main_system_font_value_contract PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
if(WIN32)
 concord_add_profiled_leaf(concord_main_system_font_win32 src/ui/text/win32/RuntimeMainSystemFontWin32.cpp concord_runtime Runtime)
 concord_add_profiled_leaf(concord_main_system_font_win32_contract_leaf src/ui/text/win32/RuntimeMainSystemFontWin32.cpp concord_runtime_match_score_contract Contract)
 target_link_libraries(concord_runtime PRIVATE concord_main_system_font_win32)
 add_executable(concord_main_system_font_native_contract tests/ui/main_system_font/MainSystemFontNativeContract.cpp)
 target_include_directories(concord_main_system_font_native_contract PRIVATE src)
 target_compile_features(concord_main_system_font_native_contract PRIVATE cxx_std_20)
 target_compile_options(concord_main_system_font_native_contract PRIVATE "$<TARGET_GENEX_EVAL:concord_runtime_match_score_contract,$<TARGET_PROPERTY:concord_runtime_match_score_contract,COMPILE_OPTIONS>>")
 target_compile_definitions(concord_main_system_font_native_contract PRIVATE NOMINMAX "$<TARGET_GENEX_EVAL:concord_runtime_match_score_contract,$<TARGET_PROPERTY:concord_runtime_match_score_contract,COMPILE_DEFINITIONS>>")
 target_link_libraries(concord_main_system_font_native_contract PRIVATE concord_main_system_font_win32_contract_leaf user32 gdi32)
 if(CONCORD_MSVC_ABI)
  set_property(TARGET concord_main_system_font_native_contract PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
 endif()
elseif(APPLE)
 # S4-1: 纯逻辑 + GDI 调用，放回 Apple 构建图（源码零改动），既有 WIN32 分支未动。
 concord_add_profiled_leaf(concord_main_system_font_win32 src/ui/text/win32/RuntimeMainSystemFontWin32.cpp concord_runtime Runtime)
 target_link_libraries(concord_runtime PRIVATE concord_main_system_font_win32)
 # S4-3: 窗口客户区出字闭环（真窗口 → WM_PAINT → BeginPaint → 产品字体类 → 回读）。
 # 推迟到目录处理结束创建，理由见文件顶部的函数注释。
 cmake_language(DEFER CALL concord_add_main_system_font_window_paint)
endif()
