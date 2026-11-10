# P4-01a：macOS 音频底座（RIFF/WAVE 解码 + AudioQueue 输出流）。
#
# 这个文件只服务 Apple 构建图，Windows 侧一行都不读——所以它既不会改到任何
# if(WIN32) 分支，也不会给 Windows 增加一个必须维护的目标。
#
# 源文件同时挂在两处：
#   1) concord_runtime —— 产品路径。本轮还没有调用点（5 个工厂是 P4-01b 的事），
#      但先接进来，让"产品级编译这套代码"这件事尽早暴露，而不是等工厂接好才发现
#      某个头在 APPLE 下不成立。
#   2) 自测可执行文件 —— 解码与设备各自的硬编码断言。
#
# AudioToolbox 随源文件一起声明，理由与兼容层的 CoreText 框架一致：谁编译这个
# TU，谁就要链接它，不靠调用点记得补。
include_guard(GLOBAL)

if(NOT APPLE)
    return()
endif()

# 两个源清单分开维护，不只是为了整齐：P4-01a 的解码/输出层自测**不应该**被
# P4-01b 的工厂拖动。上一版把它们合成一个变量，结果 output 自测一编译就连带
# 需要 vtable 与 makeUnsupportedXxx 桩，链接立刻断——"一个目标编了它不该编的
# 文件"这类错误在 cmake 里不会报警，只会在链接期以一个奇怪的未定义符号出现。
set(
    CONCORD_APPLE_AUDIO_CORE_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/../src/audio/apple/RuntimeAppleAudioOutput.cpp"
    CACHE INTERNAL
    "macOS-native decode + AudioQueue output (Apple builds only)"
)

set(
    CONCORD_APPLE_AUDIO_DEVICE_SOURCES
    # P4-01b：5 个产品音频接缝的 7 个工厂。它 include 的是 Windows 侧那 5 个
    # 纯接口头（不含 windows.h），所以签名由编译器逐字校验，不是手抄声明。
    "${CMAKE_CURRENT_LIST_DIR}/../src/audio/apple/RuntimeAppleAudioDevices.cpp"
    CACHE INTERNAL
    "Product audio seam factories for Apple builds"
)

set(
    CONCORD_APPLE_AUDIO_SOURCES
    ${CONCORD_APPLE_AUDIO_CORE_SOURCES}
    ${CONCORD_APPLE_AUDIO_DEVICE_SOURCES}
    CACHE INTERNAL
    "macOS-native audio backend sources (Apple builds only)"
)

set(
    CONCORD_APPLE_AUDIO_FRAMEWORKS
    AudioToolbox
    CACHE INTERNAL
    "Apple frameworks required by the native audio backend"
)

# "能力缺失时的诚实回退"。它们本来就是产品源文件，只是 Windows 侧那 5 个 win32 TU
# 自己兜底（#else 分支返回 unsupported），所以从来没人单独编过这 6 个桩。Apple 侧
# 的工厂把回退放在**调用点**（null 编码器 / null 诊断钩子 / 通道越界），于是这些
# 桩第一次变成真正的链接依赖——这也是它们必须进产品构建图的原因。
set(
    CONCORD_APPLE_AUDIO_FALLBACK_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/../src/audio/backend/RuntimeUnsupportedAsyncSoundDevice.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/audio/backend/RuntimeUnsupportedLegacyWaveDevice.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/audio/events/RuntimeUnsupportedMenuSoundDevice.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/audio/menu/RuntimeUnsupportedMenuMusicDevice.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/audio/menu/RuntimeUnsupportedMainMenuMusicDevice.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/audio/menu/RuntimeUnsupportedCampaignMapAudioDevice.cpp"
    CACHE INTERNAL
    "Unsupported-audio fallbacks required on Apple builds"
)

function(concord_apple_add_audio_sources _caas_target)
    if(NOT TARGET ${_caas_target})
        message(FATAL_ERROR "concord_apple_add_audio_sources: unknown target '${_caas_target}'")
    endif()
    target_sources(${_caas_target} PRIVATE ${CONCORD_APPLE_AUDIO_SOURCES})
    target_sources(${_caas_target} PRIVATE ${CONCORD_APPLE_AUDIO_FALLBACK_SOURCES})
    target_include_directories(${_caas_target} BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
    foreach(_caas_framework IN LISTS CONCORD_APPLE_AUDIO_FRAMEWORKS)
        target_link_libraries(${_caas_target} PRIVATE "-framework ${_caas_framework}")
    endforeach()
endfunction()

if(TARGET concord_runtime)
    concord_apple_add_audio_sources(concord_runtime)
endif()

if(NOT TARGET concord_apple_audio_output_selftest)
    add_executable(
        concord_apple_audio_output_selftest
        "${CMAKE_CURRENT_LIST_DIR}/../tests/audio/apple/AudioOutputSmoke.cpp"
        ${CONCORD_APPLE_AUDIO_CORE_SOURCES}
    )
    target_include_directories(
        concord_apple_audio_output_selftest
        BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src"
    )
    target_compile_features(concord_apple_audio_output_selftest PRIVATE cxx_std_20)
    foreach(_caas_framework IN LISTS CONCORD_APPLE_AUDIO_FRAMEWORKS)
        target_link_libraries(concord_apple_audio_output_selftest PRIVATE "-framework ${_caas_framework}")
    endforeach()
endif()

# P4-01b：5 个产品音频接缝的会话语义自测。
#
# 这个目标自己编那 6 个 makeUnsupportedXxx 桩，不挂到产品目标上——它们是"能力
# 缺失时的诚实回退"，自测要能在没有产品依赖的情况下证明"工厂宁可选回退也不假装
# 能播"。注意它**不是** concord_apple_audio_output_selftest：那一个只测解码与
# 输出流（P4-01a），这个测的是别名/租约/替换那套会话语义（P4-01b）。
if(NOT TARGET concord_apple_audio_devices_selftest)
    add_executable(
        concord_apple_audio_devices_selftest
        "${CMAKE_CURRENT_LIST_DIR}/../tests/audio/apple/AudioDevicesSmoke.cpp"
        ${CONCORD_APPLE_AUDIO_SOURCES}
        ${CONCORD_APPLE_AUDIO_FALLBACK_SOURCES}
    )
    target_include_directories(
        concord_apple_audio_devices_selftest
        BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src"
    )
    target_compile_features(concord_apple_audio_devices_selftest PRIVATE cxx_std_20)
    foreach(_caas_framework IN LISTS CONCORD_APPLE_AUDIO_FRAMEWORKS)
        target_link_libraries(concord_apple_audio_devices_selftest PRIVATE "-framework ${_caas_framework}")
    endforeach()
endif()

# ---------------------------------------------------------------------------
# P4-02：XAudio2 混音器的两个自测。
#
# 为什么要两个而不是一个：混音数学可以完全脱离声卡验证（离线引擎），而"产品那条
# 路径真的活了"（RuntimeVoiceAudio::ready()）必须要真设备。合成一个目标会让
# "没有声卡的 CI/容器"上跑不了本该跑得动的 52 条逐样本断言；拆开以后，
# 无声卡时只有一个检查会红，而且红得清楚。
#
# 两个目标都**不**碰产品目标：混音器的源文件已经通过
# CONCORD_APPLE_WIN32_COMPAT_SOURCES 进了产品图（和 GDI 的实现同一个位置）。
# ---------------------------------------------------------------------------
set(
    CONCORD_APPLE_XAUDIO2_COMPAT_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/include"
    "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src"
)

if(NOT TARGET concord_apple_xaudio2_mixer_selftest)
    add_executable(
        concord_apple_xaudio2_mixer_selftest
        "${CMAKE_CURRENT_LIST_DIR}/../tests/audio/apple/XAudio2MixerSmoke.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatAudio.cpp"
    )
    target_include_directories(
        concord_apple_xaudio2_mixer_selftest
        BEFORE PRIVATE
            ${CONCORD_APPLE_XAUDIO2_COMPAT_INCLUDE_DIRS}
            "${CMAKE_CURRENT_LIST_DIR}/../src"
    )
    target_compile_features(concord_apple_xaudio2_mixer_selftest PRIVATE cxx_std_20)
    target_link_libraries(concord_apple_xaudio2_mixer_selftest PRIVATE "-framework AudioToolbox")
endif()

if(NOT TARGET concord_apple_xaudio2_voice_selftest)
    # RuntimeVoiceAudio 是**产品**的语音门面，它对 Windows 的依赖只剩
    # CoInitializeEx 与两个编码助手（都在 compat 里）。这里把这三件事一起编进来，
    # 而不是给它加一层测试专用的替身——替身一出现，"ready()==true"就不再是产品
    # 路径上的事实。
    add_executable(
        concord_apple_xaudio2_voice_selftest
        "${CMAKE_CURRENT_LIST_DIR}/../tests/audio/apple/XAudio2VoiceAudioSmoke.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/audio/backend/RuntimeVoiceAudio.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatAudio.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatMisc.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatProcess.cpp"
    )
    target_include_directories(
        concord_apple_xaudio2_voice_selftest
        BEFORE PRIVATE
            ${CONCORD_APPLE_XAUDIO2_COMPAT_INCLUDE_DIRS}
            "${CMAKE_CURRENT_LIST_DIR}/../src"
    )
    target_compile_features(concord_apple_xaudio2_voice_selftest PRIVATE cxx_std_20)
    target_link_libraries(concord_apple_xaudio2_voice_selftest PRIVATE "-framework AudioToolbox")
endif()
