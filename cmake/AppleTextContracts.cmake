# T-03：宽字符 / u16string 的**运行期**语义自测。
#
# T-02 只做到"编译面清零"：所有 sizeof(wchar_t) 断言都被 #if 包住、能编过，
# 但没有一条 std::wstring / u16string 路径被执行过。这个目标就是那条运行路径的
# 最小可执行：产品的真实编码函数 + 兼容层的窄宽编解码 + std::filesystem 的宽窄桥接。
#
# 判据全部是"字节/码元个数"这种可复算的数字（4 字节 UTF-8 在这台机器上只产出
# **一个** wchar_t，而不是两个 UTF-16 码元）——因为这类 bug 的表现是"字符没丢但
# 长度不对"，任何只看"能不能编译/能不能显示"的验证都抓不到。
#
# 与 AppleAudioContracts 同样只服务 Apple 构建图：它引用的 compat 头只在非 Windows
# 上存在（windows.h shim 里带 #if defined(_WIN32) #error）。
include_guard(GLOBAL)

if(NOT APPLE)
    return()
endif()

if(NOT TARGET concord_apple_chat_text_encoding_selftest)
    add_executable(
        concord_apple_chat_text_encoding_selftest
        "${CMAKE_CURRENT_LIST_DIR}/../tests/ui/RuntimeChatTextEncodingSmoke.cpp"
        # 产品真实 TU（不是复刻实现）：聊天文本的 UTF-16 → UTF-8 编码就在这里。
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/input/RuntimeChatTextController.cpp"
        # 窄宽编解码（MultiByteToWideChar / WideCharToMultiByte）与它依赖的助手。
        "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatProcess.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src/RuntimeWin32CompatMisc.cpp"
    )
    target_include_directories(
        concord_apple_chat_text_encoding_selftest
        BEFORE PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/include"
            "${CMAKE_CURRENT_LIST_DIR}/../compat/win32/src"
            "${CMAKE_CURRENT_LIST_DIR}/../src"
    )
    target_compile_features(concord_apple_chat_text_encoding_selftest PRIVATE cxx_std_20)
endif()
