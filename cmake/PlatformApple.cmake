# macOS platform axis.
#
# This module owns everything that is specific to Darwin.  It is included only
# from CompilerAndPlatform.cmake inside an `if(APPLE)` branch, so on Windows it
# is never read and cannot affect the existing build in any way.
#
# Design rule: every capability defaults to OFF.  A backend that is not
# implemented yet must never be silently linked in -- an unimplemented backend
# surviving to link time produces a confusing crash far from its cause, while an
# OFF switch produces a clear "this feature is not available" at the call site.
include_guard(GLOBAL)

# --- capability switches -----------------------------------------------------

set(
    CONCORD_APPLE_SDL3_WINDOW
    OFF
    CACHE BOOL
    "Use the SDL3 window backend on macOS (P3 of the porting plan)"
)
set(
    CONCORD_APPLE_SDL3_AUDIO
    OFF
    CACHE BOOL
    "Use the SDL3 audio backend on macOS (P4 of the porting plan)"
)
set(
    CONCORD_APPLE_SOFTWARE_PRESENT
    ON
    CACHE BOOL
    "Present frames through the CPU SoftwareFrame path (no GPU dependency)"
)
set(
    CONCORD_APPLE_METAL_PRESENT
    OFF
    CACHE BOOL
    "Present frames through Metal instead of the CPU path"
)

# --- helpers -----------------------------------------------------------------

# Remove every Win32 implementation from a source list variable, in place.
#
# The project lists sources explicitly rather than globbing, and each Win32
# implementation lives under a `win32/` directory.  That makes a single regex
# filter sufficient, and it keeps the exclusion rule in exactly one place
# instead of scattering `if(APPLE)` around every source list.
#   usage: concord_apple_strip_win32_sources(<list-variable-name>)
function(concord_apple_strip_win32_sources _capps_list_var)
    if(NOT DEFINED ${_capps_list_var})
        message(
            FATAL_ERROR
            "concord_apple_strip_win32_sources: '${_capps_list_var}' is not a defined source list"
        )
    endif()
    set(_capps_sources ${${_capps_list_var}})
    list(LENGTH _capps_sources _capps_before)
    list(FILTER _capps_sources EXCLUDE REGEX "/win32/")
    list(LENGTH _capps_sources _capps_after)
    math(EXPR _capps_removed "${_capps_before} - ${_capps_after}")
    if(_capps_removed GREATER 0)
        message(
            STATUS
            "concord_apple: dropped ${_capps_removed} Win32 source(s) from '${_capps_list_var}'"
        )
    endif()
    set(${_capps_list_var} "${_capps_sources}" PARENT_SCOPE)
endfunction()

# Link the named Apple frameworks to a target.
#
# Frameworks are listed explicitly by the caller so that this module never
# guesses a dependency that the surrounding code does not actually need.
#   usage: concord_apple_link_frameworks(<target> Cocoa IOKit)
function(concord_apple_link_frameworks _capps_target)
    if(NOT TARGET ${_capps_target})
        message(
            FATAL_ERROR
            "concord_apple_link_frameworks: unknown target '${_capps_target}'"
        )
    endif()
    if(NOT APPLE)
        return()
    endif()
    foreach(_capps_framework IN LISTS ARGN)
        target_link_libraries(${_capps_target} PRIVATE "-framework ${_capps_framework}")
    endforeach()
endfunction()

# --- Win32 compatibility layer (S1) -----------------------------------------
#
# On macOS there is no <windows.h> at all.  Rather than rewriting the ~42
# translation units that reference Win32 symbols, this build adds a local
# include directory that supplies a declaration-only shim.  Source files keep
# their `#include <windows.h>` unchanged; only the search path differs, and it
# differs only on Apple.
#
# Cardinal rule: this directory must never be on the include path of a Windows
# build.  The shim itself also #error's if it is ever compiled with _WIN32
# defined, so a mistaken path addition fails loudly instead of shadowing the
# real SDK headers.
set(
    CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/include"
    CACHE INTERNAL
    "Include directory holding the non-Windows <windows.h> shim"
)

# Put the Win32 shim ahead of every other include path.
#
# `include_directories(BEFORE ...)` is directory-scoped, so it reaches every
# target created after this point, including targets in subdirectories added
# later.  Targets that already exist are handled by passing them explicitly.
#   usage: concord_apple_enable_win32_compat([<target> ...])
function(concord_apple_enable_win32_compat)
    if(NOT APPLE)
        return()
    endif()
    set(_cawc_dir "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}")
    if(NOT EXISTS "${_cawc_dir}/windows.h")
        message(
            FATAL_ERROR
            "concord_apple_enable_win32_compat: '${_cawc_dir}/windows.h' is missing; "
            "the macOS build has no Win32 header fallback and would fail on the first "
            "translation unit that includes <windows.h>."
        )
    endif()
    include_directories(BEFORE "${_cawc_dir}")
    foreach(_cawc_target IN LISTS ARGN)
        if(NOT TARGET ${_cawc_target})
            message(
                FATAL_ERROR
                "concord_apple_enable_win32_compat: unknown target '${_cawc_target}'"
            )
        endif()
        target_include_directories(${_cawc_target} BEFORE PRIVATE "${_cawc_dir}")
    endforeach()
    message(
        STATUS
        "concord_apple: win32-compat include dir = ${_cawc_dir}"
    )
endfunction()

# --- Win32 compatibility layer (S2): implementations -------------------------
#
# S1 shipped declarations only, so an unimplemented symbol surfaced as an
# undefined symbol at link time.  S2 starts filling in the families that the
# product build graph actually reaches, which means real object code has to be
# attached to a target.
#
# Only Apple builds get these sources.  Windows keeps compiling against the real
# SDK, so the compat tree is never on a Windows link line.
set(
    CONCORD_APPLE_WIN32_COMPAT_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatGdi.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatMisc.cpp"
    # S10: file I/O (CreateFileW wide + narrow, WriteFile) and the process /
    # thread / encoding / clock / environment families.  These share the handle
    # table and the code-point codec in RuntimeWin32CompatInternal.h, which is
    # why they arrive together rather than one family per round.
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatProcess.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatFile.cpp"
    # P5-05: input / cursor / capture / timer.  These all presuppose a real event
    # loop and window host (S7), so the implementations are honest defaults: the
    # keyboard state reads all-zero, LoadCursorW returns a stable non-NULL pseudo
    # handle for standard IDs, capture is a logical state machine, and timers are
    # register-only stubs.
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatInput.cpp"
    # S4-2: the glyph backend.  CoreText rasterises one code point at a time into
    # a 1-bit mask; compositing, clipping and ROP2 stay in RuntimeWin32CompatGdi.
    # That split is what lets text honour the clip region without the text layer
    # ever touching a pixel.
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatText.cpp"
    # S7: the window / message / input backend.  Pure-offscreen route: HWND is a
    # pointer to a logical CompatWindow (handle-table + magic verified), the
    # message queue is a thread-level object, and the window DC is an offscreen
    # CompatDevice with the client bitmap selected.  No real NSWindow is opened;
    # a Cocoa shell can present the offscreen frame later without changing this
    # object model.
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatUser.cpp"
    # S6: GDI+ PNG export.  The product only ever uses GDI+ to write a 32bpp
    # pixel buffer out as a PNG diagnostic image, so this is the whole surface:
    # startup/shutdown token bookkeeping, a one-codec (PNG) encoder report, and
    # a Bitmap that holds a pixel buffer and saves it via ImageIO.
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatGdiplus.cpp"
    # S11：定时器。P5-05 时 SetTimer/KillTimer 是"登记即成功"的桩，WM_TIMER
    # 一次都不派发；这一份 TU 持有真正的定时器表，User.cpp 在取消息时合成
    # WM_TIMER / WM_PAINT（两者都是"检索时生成"，不进队列）。
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatTimer.cpp"
    # S8：Interlocked 族。这一族**必须真实现**（两进程 smoke 靠它轮询共享内存），
    # 映射到 __atomic_* 内建；空实现只会在 ARM 上暴露成"偶发读到旧值"。
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatAtomic.cpp"
    # P4-02：XAudio2 的真引擎。P4-01b 时这里只是"诚实的 E_NOTIMPL"（未实现多路
    # 混音，返回成功会变成"每条语音都播放成功但不出声"）；现在它是一台真正的
    # 软件混音器：多条 source voice 在 48kHz 立体声浮点缓冲里相加、按音量/输出
    # 矩阵分配、支持 LoopCount 与线性重采样，最后由**一个** AudioQueue 输出。
    # 这个 TU 因此需要 AudioToolbox —— 框架清单就在下面那份 sets 里，随源清单
    # 一起走，任何编译它的目标都不会漏链接。
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatAudio.cpp"
    # S8b: 跨进程事件族（CreateEventW / OpenEventW / SetEvent / ResetEvent /
    # WaitForSingleObject / WaitForMultipleObjects）+ strncpy_s。这些在 windows.h 里
    # 只有声明、compat/src 下 0 实现；tests/multiplayer 的 smoke 是唯一没有 #if
    # 保护的调用方，所以不加进来就是"能 -fsyntax-only、链接必失败"。
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatKernel.cpp"
    # Q-05：Cocoa 呈现层。S7 的窗口是"纯离屏"——对象、WM_PAINT、DIB 全真，
    # 但进程里没有 NSWindow，所以屏幕上什么都没有。这个 .mm 负责把每帧贴上去。
    # 它是本仓库唯一的 Objective-C++ TU（CMakeLists 里 if(APPLE) 才 enable OBJCXX）。
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeAppleWindowSurface.mm"
    # S12：目录变更通知族（FindFirstChangeNotificationW / FindNextChangeNotification /
    # FindCloseChangeNotification）。产品只有 RuntimeMenuResourceWatch 用它，
    # 判断"资源树有没有变、能不能复用已预热的菜单状态"。
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatChangeNotify.cpp"
    CACHE INTERNAL
    "Win32 compatibility implementations compiled into Apple builds"
)

# Frameworks the compat layer needs, on top of everything it gets for free.
#
# CoreText/CoreGraphics are only used by the glyph backend, but any target that
# compiles that translation unit must link them, so the list travels with the
# source list instead of being remembered at each call site.
set(
    CONCORD_APPLE_WIN32_COMPAT_FRAMEWORKS
    CoreText
    CoreGraphics
    CoreFoundation
    # ImageIO is only needed by the GDI+ PNG export (S6), but it travels with the
    # list so every target that compiles that translation unit links it.
    ImageIO
    # P4-02: the XAudio2 engine drives a real AudioQueue. 十二个 compat 自测目标
    # 都编译同一份源清单，框架漏了它们就会在链接期各报一次同样的错。
    AudioToolbox
    # Q-05: NSWindow/NSView/NSApplication 来自 Cocoa，CALayer/contentsGravity
    # 来自 QuartzCore（CoreAnimation）。两者都随源清单走，任何编译那个 .mm 的
    # 目标都不会漏链接。
    Cocoa
    QuartzCore
    # S12: FSEvents（目录变更通知）在 CoreServices 伞框架之下。链接用伞框架，
    # 头用窄头 <FSEvents/FSEvents.h>（嵌套搜索路径见下面的 -F）。
    CoreServices
    CACHE INTERNAL
    "Apple frameworks required by the Win32 compatibility layer"
)

# S12：FSEvents 是**嵌套**框架（CoreServices.framework/Frameworks/FSEvents.framework），
# 默认框架搜索路径只覆盖顶层，`<FSEvents/FSEvents.h>` 因此找不到。
# 按**文件属性**加 -F 而不是按目标加：同一份源清单被十几个目标编译
# （产品 + 12 个 compat 自测），按目标加必然漏掉几个，症状是"某些自测链接报了
# 一堆 FSEvent 符号未定义"，而产品好好的。
# 注意 CMAKE_OSX_SYSROOT 在"用默认 SDK"时是空串（不是路径），直接拼会得到
# -F/System/Library/... 这种不存在的绝对路径，报"did not find FSEvents.h in
# framework 'FSEvents' (loaded from '/System/Library/...')"——看起来像 SDK 缺失，
# 实际只是拼错了前缀。所以显式问 xcrun 要 SDK 路径。
if(NOT DEFINED CONCORD_APPLE_FSEVENTS_FRAMEWORK_DIR)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _concord_sdk_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_concord_sdk_path AND EXISTS
        "${_concord_sdk_path}/System/Library/Frameworks/CoreServices.framework/Frameworks")
        set(
            CONCORD_APPLE_FSEVENTS_FRAMEWORK_DIR
            "${_concord_sdk_path}/System/Library/Frameworks/CoreServices.framework/Frameworks"
            CACHE INTERNAL "Nested framework search path for FSEvents"
        )
    endif()
endif()
if(CONCORD_APPLE_FSEVENTS_FRAMEWORK_DIR)
    set_source_files_properties(
        "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatChangeNotify.cpp"
        PROPERTIES COMPILE_FLAGS
            "-F${CONCORD_APPLE_FSEVENTS_FRAMEWORK_DIR}"
    )
else()
    message(FATAL_ERROR
        "S12: cannot locate CoreServices.framework/Frameworks for FSEvents headers "
        "(xcrun --show-sdk-path failed).")
endif()

# Q-05：Cocoa 那个 TU 用 ARC。项目其余部分全是 C++（无 ARC 概念），
# 只有这一个文件需要 —— 按文件属性设，不动全局编译选项。
set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeAppleWindowSurface.mm"
    PROPERTIES COMPILE_FLAGS "-fobjc-arc"
)

#   usage: concord_apple_link_win32_compat_frameworks(<target>)
function(concord_apple_link_win32_compat_frameworks _cawlf_target)
    if(NOT APPLE)
        return()
    endif()
    if(NOT TARGET ${_cawlf_target})
        message(
            FATAL_ERROR
            "concord_apple_link_win32_compat_frameworks: unknown target '${_cawlf_target}'"
        )
    endif()
    foreach(_cawlf_framework IN LISTS CONCORD_APPLE_WIN32_COMPAT_FRAMEWORKS)
        target_link_libraries(${_cawlf_target} PRIVATE "-framework ${_cawlf_framework}")
    endforeach()
endfunction()

#   usage: concord_apple_add_win32_compat_sources(<target>)
function(concord_apple_add_win32_compat_sources _cawcs_target)
    if(NOT APPLE)
        return()
    endif()
    if(NOT TARGET ${_cawcs_target})
        message(
            FATAL_ERROR
            "concord_apple_add_win32_compat_sources: unknown target '${_cawcs_target}'"
        )
    endif()
    target_sources(${_cawcs_target} PRIVATE ${CONCORD_APPLE_WIN32_COMPAT_SOURCES})
    target_include_directories(
        ${_cawcs_target}
        BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
    )
    concord_apple_link_win32_compat_frameworks(${_cawcs_target})
endfunction()

# Build the compat layer's own semantic tests as a plain executable.
#
# Deliberately NOT registered with add_test(): the captured baselines compare
# CTest's `registered` count across platforms, and a macOS-only test would make
# every future capture look like a difference.  The binary is built by the
# normal build, and scripts/verify-win32-compat.sh runs it.
#   usage: concord_apple_add_win32_compat_selftest()
function(concord_apple_add_win32_compat_selftest)
    if(NOT APPLE)
        return()
    endif()
    if(TARGET concord_win32_compat_gdi_selftest)
        return()
    endif()
    add_executable(
        concord_win32_compat_gdi_selftest
        "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatGdiSemantics.cpp"
        ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
    )
    target_include_directories(
        concord_win32_compat_gdi_selftest
        BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
    )

    # Q-06：输入投递闭环。真实点击无法在无人值守环境里验证（不能截图、
    # 不能合成鼠标），但"InputEvent → WM_*"这一段可以钉死，而且它正是最容易
    # **静默**写错的地方：LPARAM 的坐标是两个 16 位字（负数/超界会被截断）、
    # WM_MOUSEWHEEL 的 delta 必须在**高位**字（放低位就永远等于 0）。
    # 这些错了都不会崩，只会让"点上去没反应"。
    if(NOT TARGET concord_apple_input_semantics_selftest)
        add_executable(
            concord_apple_input_semantics_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatAppleInputSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_apple_input_semantics_selftest
            BEFORE PRIVATE
                "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
                # RuntimeAppleWindowSurface.h 在 src/ 下（不是 include/ 下的 shim 头）。
                "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src"
        )
        concord_apple_link_win32_compat_frameworks(concord_apple_input_semantics_selftest)
    endif()

    # Q-10：屏幕 DC（GetDC(NULL)）。这是本仓库出现最多的 GDI 惯用法（71 处），
    # 语义上"必须非空、必须单例、必须能当参照设备"，而这些都是编译器看不见的契约。
    if(NOT TARGET concord_screen_dc_semantics_selftest)
        add_executable(
            concord_screen_dc_semantics_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatScreenDcSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_screen_dc_semantics_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
        concord_apple_link_win32_compat_frameworks(concord_screen_dc_semantics_selftest)
    endif()

    # Q-12：窗口类光标 + DefWindowProc(WM_SETCURSOR)。产品的"编辑框该恢复 I 形
    # 光标"断言直接依赖它，而默认过程不装光标时既不崩也不报错。
    if(NOT TARGET concord_window_cursor_semantics_selftest)
        add_executable(
            concord_window_cursor_semantics_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatWindowCursorSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_window_cursor_semantics_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
        concord_apple_link_win32_compat_frameworks(concord_window_cursor_semantics_selftest)
    endif()

    # S12：目录变更通知（FSEvents 后端）。产品用它判断"资源树有没有变、能不能复用
    # 已预热的菜单状态"，所以"未变更不能假阳性"和"变更后必须保持 signaled"都要钉住。
    if(NOT TARGET concord_change_notify_semantics_selftest)
        add_executable(
            concord_change_notify_semantics_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatChangeNotifySemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_change_notify_semantics_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
        concord_apple_link_win32_compat_frameworks(concord_change_notify_semantics_selftest)
    endif()

    # The XAPO half of the audio story: CXAPOBase is only "done" if the project's
    # real peak limiter (src/audio/backend/RuntimeAudioPeakLimiter.h, pure DSP)
    # still compiles, still limits, and is still destroyed by Release().  That
    # last part is the one no compile check can see: the base class owns the
    # destructor because the limiter never overrides Release itself.
    if(NOT TARGET concord_win32_compat_xapo_selftest)
        add_executable(
            concord_win32_compat_xapo_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatXapoLimiterSemantics.cpp"
        )
        target_include_directories(
            concord_win32_compat_xapo_selftest
            BEFORE PRIVATE
                "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
                "${CMAKE_CURRENT_LIST_DIR}/../src"
        )
    endif()

    # EMF recorder/replayer.  The product's frame-stability audits record one
    # repaint and assert "exactly one full-frame copy"; a recorder that emits no
    # records would make that audit vacuously true.  The test therefore also
    # records two frames and asserts the audit sees two.
    if(NOT TARGET concord_win32_compat_metafile_selftest)
        add_executable(
            concord_win32_compat_metafile_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatMetafileSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_win32_compat_metafile_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
    endif()

    # DC 状态与查询。这一族不画像素，却决定"像素路径走不走快路"：三条快路径
    # 逐项比对 DC 状态，任何一项不符就静默退回慢路径。所以这里断言的是**初值与
    # 字面量**（GetROP2(dc)==13 而不是 ==R2_COPYPEN），因为拿常量自比时头文件
    # 抄错也照样通过。
    if(NOT TARGET concord_win32_compat_dcstate_selftest)
        add_executable(
            concord_win32_compat_dcstate_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatDcStateSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_win32_compat_dcstate_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
    endif()

    # The small families (COM / timer / MCI / MSVC `_s` CRT variants).  Their
    # whole risk is "a surface-level equivalent silently changes behaviour", so
    # this binary asserts the _s semantics rather than the return codes.
    if(NOT TARGET concord_win32_compat_misc_selftest)
        add_executable(
            concord_win32_compat_misc_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatMiscSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_win32_compat_misc_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
    endif()

    # 区域与裁剪（S3-2）。三条快路径的判据是 GetClipBox；若复杂度恒答
    # SIMPLEREGION，复杂裁剪下它们照走不误，而所有"快慢两路一致"的契约照样全绿。
    # 另外调用点普遍"选中裁剪区后立刻 DeleteObject"，所以这里还要验证 DC 拿到
    # 的是副本而不是指针。
    if(NOT TARGET concord_win32_compat_region_selftest)
        add_executable(
            concord_win32_compat_region_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatRegionSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_win32_compat_region_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
    endif()

    # 图元光栅（S3-3）。这一族唯一不能省的是**硬编码像素**：项目的 raster
    # 契约都是"快慢两路比"，两侧共用同一套 GDI，LineTo 差一像素、裁剪被忽略、
    # 映射没生效这三种错法在两侧同时发生，断言照样全绿。所以这里断言的是
    # 字面量（0xFFEEDDCC 而不是 background ^ white），并且专门验证
    # "XOR 画两遍必须逐字节还原"——那是填充与描边不重叠的唯一硬证据。
    if(NOT TARGET concord_win32_compat_draw_selftest)
        add_executable(
            concord_win32_compat_draw_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatDrawSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_win32_compat_draw_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
    endif()

    # S10: files, handles, encoding, clocks.  The assertions that matter here are
    # not "the call returned true" but "the failure carried the right error
    # code" (ERROR_FILE_EXISTS vs ERROR_ACCESS_DENIED drive retry loops) and
    # "CloseHandle refused a GDI pointer instead of closing an unrelated fd".
    if(NOT TARGET concord_win32_compat_file_selftest)
        add_executable(
            concord_win32_compat_file_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatFileSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_win32_compat_file_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
    endif()

    # P5-05: input / cursor / capture / timer.  The risk here is not "does the
    # return code look right" but "is the default honest when there is no window
    # host yet" -- e.g. LoadCursorW must return a non-NULL pseudo handle for
    # standard IDs (a NULL would be read by SetCursor as "hide the cursor"), and
    # DrawFrameControl must actually paint (not "return TRUE while drawing
    # nothing").  Those are exactly what this binary asserts.
    if(NOT TARGET concord_win32_compat_input_selftest)
        add_executable(
            concord_win32_compat_input_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatInputSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_win32_compat_input_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
    endif()

    # P5-06 + S4-2: text / font.  There IS a glyph backend now (CoreText), so the
    # assertions moved from "is it honest" to "is it a glyph": baseline alignment
    # across 'T'/'e', a real descender on 'g', a bottom bar on 'L' (catches a
    # vertically flipped raster), same-call-twice byte equality, and XOR-twice
    # pixel restoration (only possible because the mask is 1 bit).
    if(NOT TARGET concord_win32_compat_text_selftest)
        add_executable(
            concord_win32_compat_text_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatTextSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_win32_compat_text_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
    endif()

    # S7: window / message / hook.  The assertions here are about the logical
    # window object model closing its loop: register → create → WM_NCCREATE
    # stores GWLP_USERDATA → PostMessage/GetMessage/DispatchMessage reach the
    # window proc → WH_CBT sees HCBT_CREATEWND → DestroyWindow drops the handle
    # table count back to zero.  No real window is opened.
    if(NOT TARGET concord_win32_compat_user_selftest)
        add_executable(
            concord_win32_compat_user_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatUserSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_win32_compat_user_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
        # 本自测直接断言句柄表计数（liveHandleCount 证明"窗口关干净了"），
        # 需要看到 RuntimeWin32CompatInternal.h —— 它和实现源同目录。
        target_include_directories(
            concord_win32_compat_user_selftest
            BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src"
        )
    endif()

    # S6: GDI+ PNG export.  The assertions are about the one real chain the
    # product uses — startup, one PNG codec, and a Bitmap::Save that actually
    # writes a PNG whose decoded pixels keep their R/G/B order (no silent BGR
    # swap).  The colour read-back uses ImageIO + CoreGraphics directly.
    if(NOT TARGET concord_win32_compat_gdiplus_selftest)
        add_executable(
            concord_win32_compat_gdiplus_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatGdiplusSemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_win32_compat_gdiplus_selftest
            BEFORE PRIVATE "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
        )
    endif()

    # S11: timer delivery.  The assertion is a **count inside a real GetMessageW
    # loop**, not a return code: the previous stub returned TRUE from SetTimer and
    # the product still reached the menu -- it just never animated.  A stub that
    # answers "registered" while nothing is ever delivered passes every signature
    # check and fails only this one.
    if(NOT TARGET concord_win32_compat_timer_selftest)
        add_executable(
            concord_win32_compat_timer_selftest
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/selftest/CompatTimerDeliverySemantics.cpp"
            ${CONCORD_APPLE_WIN32_COMPAT_SOURCES}
        )
        target_include_directories(
            concord_win32_compat_timer_selftest
            BEFORE PRIVATE
                "${CONCORD_APPLE_WIN32_COMPAT_INCLUDE_DIR}"
                "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src"
        )
    endif()

    # Every compat self-test in this directory compiles the same source list, so
    # every one of them needs the glyph backend's frameworks.  Doing it here, by
    # name pattern, keeps a future self-test from failing to link for a reason
    # that has nothing to do with what it tests.
    get_property(_cawcs_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_cawcs_target IN LISTS _cawcs_all_targets)
        if(_cawcs_target MATCHES "^concord_win32_compat_.*_selftest$")
            concord_apple_link_win32_compat_frameworks(${_cawcs_target})
        endif()
    endforeach()
endfunction()

# Fail the configure step if a Win32 implementation reached a non-Windows build.
#
# This is the guard for the porting plan's cardinal rule -- Windows keeps
# working, macOS is added beside it.  A Win32 translation unit that leaks into
# an Apple build would otherwise surface as `windows.h` not found, which reads
# like a compiler problem rather than the build-graph problem it really is.
#   usage: concord_apple_assert_no_win32_sources(<target>)
function(concord_apple_assert_no_win32_sources _capps_target)
    if(NOT TARGET ${_capps_target})
        message(
            FATAL_ERROR
            "concord_apple_assert_no_win32_sources: unknown target '${_capps_target}'"
        )
    endif()
    get_target_property(_capps_sources ${_capps_target} SOURCES)
    if(NOT _capps_sources)
        return()
    endif()
    set(_capps_leaked "")
    foreach(_capps_source IN LISTS _capps_sources)
        # compat/win32/ 里的是 macOS 侧的 shim 实现，路径同样含 "/win32/"。
        # 不排除它，这条守卫会对着自己的兼容层报"Win32 源码泄漏"——
        # 一个只在第一次被调用时才会暴露的误报，比漏报更浪费时间。
        if(_capps_source MATCHES "/compat/win32/")
            continue()
        endif()
        if(_capps_source MATCHES "/win32/")
            list(APPEND _capps_leaked "${_capps_source}")
        endif()
    endforeach()
    if(_capps_leaked)
        message(
            FATAL_ERROR
            "concord_apple: target '${_capps_target}' still compiles Win32 sources: ${_capps_leaked}"
        )
    endif()
endfunction()

# ---------------------------------------------------------------------------
# ON-1-03: macOS 交付形态（.app bundle）
# ---------------------------------------------------------------------------
#
# 刻意**不用** MACOSX_BUNDLE：那会把已链接的可执行文件搬进
# ConcordRTS.app/Contents/MacOS/ 下面，而仓库里十几套 smoke、scripts/ 里的验证
# 脚本、以及 RUNLOG 里的每一条命令都写着 build-product-mac/concord_runtime。
# 改产物路径等于把既有验证全打断，而这件事本身不带来任何能力。
#
# 所以 bundle 由一个显式目标（concord_app）从已链接的二进制**复制**出来：
# build 目录里的二进制仍是开发形态，.app 是交付形态，两者互不影响。
#
# 用法：cmake --build build-product-mac --target concord_app
# 产出：<build>/app/ConcordRTS.app（ad-hoc 已签名，双击即玩）

set(
    CONCORD_MACOS_APP_NAME
    "ConcordRTS"
    CACHE STRING "macOS bundle name (also the .app file name)"
)
set(
    CONCORD_MACOS_BUNDLE_ID
    "org.concord.rts"
    CACHE STRING "CFBundleIdentifier of the macOS bundle"
)
set(
    CONCORD_MACOS_APP_VERSION
    "0.1.0"
    CACHE STRING "CFBundleShortVersionString of the macOS bundle"
)
set(
    CONCORD_MACOS_MIN_VERSION
    "11.0"
    CACHE STRING "LSMinimumSystemVersion of the macOS bundle"
)
set(
    CONCORD_MACOS_APP_DATA_ROOT
    ""
    CACHE PATH
    "Game data tree baked into Contents/Resources/data-root (local_data/ + RedAlert2/)"
)

# 默认值猜一次：数据是兄弟目录（../ConcordRTS-GameData）或内联目录。
# 猜不到就留空——打包脚本会打印警告，探测器退回兄弟/祖先扫描，
# 比写一个错路径更诚实。
if(NOT CONCORD_MACOS_APP_DATA_ROOT)
    foreach(
        _concord_data_candidate
        "${CMAKE_SOURCE_DIR}/../ConcordRTS-GameData"
        "${CMAKE_SOURCE_DIR}/ConcordRTS-GameData"
    )
        if(
            EXISTS "${_concord_data_candidate}/local_data"
            AND EXISTS "${_concord_data_candidate}/RedAlert2"
        )
            get_filename_component(
                _concord_data_real
                "${_concord_data_candidate}"
                REALPATH
            )
            set(
                CONCORD_MACOS_APP_DATA_ROOT
                "${_concord_data_real}"
                CACHE PATH
                "Game data tree baked into Contents/Resources/data-root (local_data/ + RedAlert2/)"
                FORCE
            )
            break()
        endif()
    endforeach()
endif()

# TARGET 守卫：portable-core 构建（CONCORD_PORTABLE_CORE=ON）在
# CompilerAndPlatform.cmake 里提前 return，不会走到这里；但若将来平台轴被
# 更早引入，这里的守卫能保证"没有产品可执行文件时也不会配置失败"。
if(TARGET concord_runtime)
    set(
        CONCORD_MACOS_APP_DIR
        "${CMAKE_BINARY_DIR}/app"
        CACHE PATH
        "Directory that receives the assembled .app bundle"
    )
    add_custom_target(
        concord_app
        COMMAND
            sh "${CMAKE_SOURCE_DIR}/packaging/make-app-bundle.sh"
            --binary "$<TARGET_FILE:concord_runtime>"
            --output "${CONCORD_MACOS_APP_DIR}"
            --data-root "${CONCORD_MACOS_APP_DATA_ROOT}"
            --version "${CONCORD_MACOS_APP_VERSION}"
            --bundle-id "${CONCORD_MACOS_BUNDLE_ID}"
            --app-name "${CONCORD_MACOS_APP_NAME}"
            --min-macos "${CONCORD_MACOS_MIN_VERSION}"
            --icon-source "${CMAKE_SOURCE_DIR}/packaging/icon-source.png"
        DEPENDS concord_runtime
        COMMENT
            "Assembling ${CONCORD_MACOS_APP_NAME}.app (data root: ${CONCORD_MACOS_APP_DATA_ROOT})"
        VERBATIM
    )
    message(
        STATUS
        "concord_app target: ${CONCORD_MACOS_APP_DIR}/${CONCORD_MACOS_APP_NAME}.app"
    )
endif()
