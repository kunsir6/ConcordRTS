cmake_minimum_required(VERSION 3.20)

project(ConcordRTS LANGUAGES CXX)

# Q-05：macOS 的 Cocoa 呈现层是 Objective-C++（.mm），需要显式启用 OBJCXX。
# 只在 Apple 上启用：Windows 的 MSVC 没有 ObjC++ 支持，无条件 enable 会让
# Windows 构建在 configure 阶段就失败——而本项目的立场是"保留多平台编译"。
if(APPLE)
    enable_language(OBJCXX)
endif()

option(CONCORD_PORTABLE_CORE "Build only real production core fixtures, without window/device backends" OFF)

if(CONCORD_PORTABLE_CORE)
    set(_concord_profile_identity portable-core)
else()
    set(_concord_profile_identity product)
endif()
if(DEFINED CONCORD_CONFIGURED_BUILD_PROFILE AND
        NOT CONCORD_CONFIGURED_BUILD_PROFILE STREQUAL _concord_profile_identity)
    message(FATAL_ERROR "Product/core caches must be separate; select a new build directory")
endif()
set(CONCORD_CONFIGURED_BUILD_PROFILE "${_concord_profile_identity}" CACHE INTERNAL "Owned build capability" FORCE)


# Keep this include order: these files share the root directory scope.
# include() preserves existing src/tests paths and target enumeration.
include(cmake/RuntimeSmokeTargets.cmake)
include(cmake/GameTargets.cmake)
include(cmake/DevelopmentTargets.cmake)
include(cmake/VideoBridge.cmake)
include(cmake/CompilerAndPlatform.cmake)
include(cmake/TestsRuntimeRegression.cmake)
include(cmake/TestsUnitAndNetwork.cmake)
include(cmake/TestsAssets.cmake)
include(cmake/MultiplayerIntegration.cmake)
include(cmake/TestsAdditionalRegression.cmake)
