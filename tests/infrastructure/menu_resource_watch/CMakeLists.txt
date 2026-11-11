# The production watch owner is tested without menus, graphics, audio or IPC.
if(CONCORD_MSVC_ABI)
    set(CMAKE_CXX_STANDARD_LIBRARIES "kernel32.lib")
endif()
add_executable(concord_menu_resource_watch_contract
    "${PROJECT_SOURCE_DIR}/tests/infrastructure/MenuResourceWatchContract.cpp"
    "${PROJECT_SOURCE_DIR}/src/ui/resources/RuntimeMenuResourceWatch.cpp")
target_include_directories(concord_menu_resource_watch_contract PRIVATE "${PROJECT_SOURCE_DIR}/src")

# ARM-02: RuntimeMenuResourceWatch.cpp includes <windows.h> on Apple on purpose --
# change notification is served by the compat layer (FSEvents) behind that shim.
# This owner therefore needs the shim headers, the three implementations it calls,
# and the FSEvents/CoreFoundation frameworks.
#
# Everything here is deliberately self-contained instead of calling
# concord_apple_add_win32_compat_sources(): this add_subdirectory() is reached from
# DevelopmentTargets.cmake, which runs before CompilerAndPlatform.cmake includes
# PlatformApple.cmake, so both that function and CONCORD_APPLE_FSEVENTS_FRAMEWORK_DIR
# do not exist yet (the same ordering trap documented in MainSystemFontContracts.cmake).
# The -F path is passed per target rather than per source file for the same reason:
# source file properties are directory-scoped and would not survive this scope.
if(APPLE)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _arm02_sdk_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    target_include_directories(concord_menu_resource_watch_contract PRIVATE
        "${PROJECT_SOURCE_DIR}/compat/win32/include")
    target_sources(concord_menu_resource_watch_contract PRIVATE
        "${PROJECT_SOURCE_DIR}/compat/win32/src/RuntimeWin32CompatChangeNotify.cpp"
        "${PROJECT_SOURCE_DIR}/compat/win32/src/RuntimeWin32CompatKernel.cpp"
        "${PROJECT_SOURCE_DIR}/compat/win32/src/RuntimeWin32CompatProcess.cpp")
    # <FSEvents/FSEvents.h> is a nested framework header: the default framework
    # search path only covers top-level frameworks. CMAKE_OSX_SYSROOT is empty when
    # the default SDK is used, so ask xcrun instead of concatenating.
    if(EXISTS "${_arm02_sdk_path}/System/Library/Frameworks/CoreServices.framework/Frameworks")
        target_compile_options(concord_menu_resource_watch_contract PRIVATE
            "-F${_arm02_sdk_path}/System/Library/Frameworks/CoreServices.framework/Frameworks")
    endif()
    target_link_libraries(concord_menu_resource_watch_contract PRIVATE
        "-framework CoreServices" "-framework CoreFoundation")
    unset(_arm02_sdk_path)
endif()

if(CONCORD_MSVC_FRONTEND)
    concord_target_compile_options(concord_menu_resource_watch_contract PRIVATE /utf-8 /FS /W4 /WX /permissive-)
endif()
