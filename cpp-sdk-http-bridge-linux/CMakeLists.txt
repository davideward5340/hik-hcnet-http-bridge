# Debian 10、统信 UOS V20 和银河麒麟 V10 的常见 CMake 均可满足 3.13。
cmake_minimum_required(VERSION 3.13)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(hik_sdk_http_bridge_linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(hik-sdk-http-bridge src/main.cpp)
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The bundled Windows HCNetSDK is Win32; configure with an x86 toolchain")
    endif()
    set(HIK_WIN32_SDK_PACKAGE "" CACHE PATH "Windows HCNetSDK package root")
    set(HIK_WIN32_RUNTIME "" CACHE PATH "Windows HCNetSDK and FFmpeg runtime root")
    if(NOT HIK_WIN32_SDK_PACKAGE OR NOT EXISTS "${HIK_WIN32_SDK_PACKAGE}/头文件/HCNetSDK.h" OR NOT EXISTS "${HIK_WIN32_SDK_PACKAGE}/库文件/HCNetSDK.lib")
        message(FATAL_ERROR "Set HIK_WIN32_SDK_PACKAGE to an authorized Win32 HCNetSDK package containing 头文件/HCNetSDK.h and 库文件/HCNetSDK.lib")
    endif()
    if(NOT HIK_WIN32_RUNTIME OR NOT EXISTS "${HIK_WIN32_RUNTIME}/hcnetsdk" OR NOT EXISTS "${HIK_WIN32_RUNTIME}/ffmpeg")
        message(FATAL_ERROR "Set HIK_WIN32_RUNTIME to a local runtime root containing hcnetsdk/ and ffmpeg/; vendor files are not distributed by this repository")
    endif()
    target_include_directories(hik-sdk-http-bridge PRIVATE "${HIK_WIN32_SDK_PACKAGE}/头文件")
    target_link_directories(hik-sdk-http-bridge PRIVATE "${HIK_WIN32_SDK_PACKAGE}/库文件")
    target_link_libraries(hik-sdk-http-bridge PRIVATE HCNetSDK ws2_32 delayimp)
    target_link_options(hik-sdk-http-bridge PRIVATE /DELAYLOAD:HCNetSDK.dll)
    target_compile_definitions(hik-sdk-http-bridge PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(hik-sdk-http-bridge PRIVATE /W4 /permissive- /utf-8 /EHsc /wd4828)
    set_property(TARGET hik-sdk-http-bridge PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_custom_command(TARGET hik-sdk-http-bridge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${HIK_WIN32_RUNTIME}/hcnetsdk" "$<TARGET_FILE_DIR:hik-sdk-http-bridge>/hcnetsdk"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${HIK_WIN32_RUNTIME}/ffmpeg" "$<TARGET_FILE_DIR:hik-sdk-http-bridge>/ffmpeg"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/config/config.windows.json" "$<TARGET_FILE_DIR:hik-sdk-http-bridge>/config.json")
else()
    target_include_directories(hik-sdk-http-bridge PRIVATE vendor/hcnetsdk/include)
    target_link_directories(hik-sdk-http-bridge PRIVATE vendor/hcnetsdk/sdk)
    target_link_libraries(hik-sdk-http-bridge PRIVATE hcnetsdk pthread dl)
    target_compile_options(hik-sdk-http-bridge PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# 便携包仍需要宿主 glibc（HCNetSDK 是动态库），但将 libstdc++/libgcc
# 静态合入可减少统信、深度、麒麟等系统上的 C++ 运行库版本差异。
if(NOT WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(hik-sdk-http-bridge PRIVATE -static-libgcc -static-libstdc++)
    # GCC 8 的 std::filesystem 仍位于独立的 libstdc++fs 中；统信/麒麟
    # 的旧版开发工具链可能仍使用该版本。
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
        target_link_libraries(hik-sdk-http-bridge PRIVATE stdc++fs)
    endif()
endif()

if(NOT WIN32)
    set_target_properties(hik-sdk-http-bridge PROPERTIES
        BUILD_RPATH "$ORIGIN/../sdk:$ORIGIN/../sdk/HCNetSDKCom"
        INSTALL_RPATH "$ORIGIN/../sdk:$ORIGIN/../sdk/HCNetSDKCom")
endif()
