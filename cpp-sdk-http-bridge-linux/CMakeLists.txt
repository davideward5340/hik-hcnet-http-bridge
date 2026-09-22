# Debian 10、统信 UOS V20 和银河麒麟 V10 的常见 CMake 均可满足 3.13。
cmake_minimum_required(VERSION 3.13)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(hik_sdk_http_bridge_linux LANGUAGES CXX)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h" HIK_VERSION_LINE REGEX "^#define HIK_BRIDGE_VERSION ")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" HIK_PACKAGE_VERSION "${HIK_VERSION_LINE}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/version.json" CONTENT "{\"version\":\"${HIK_PACKAGE_VERSION}\"}\n")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(hik-sdk-http-bridge src/main.cpp)
if(WIN32)
    enable_language(RC)
    target_sources(hik-sdk-http-bridge PRIVATE src/version.rc)
    add_executable(hik-bridge-upgrade src/windows-upgrade.cpp)
    target_compile_options(hik-bridge-upgrade PRIVATE /W4 /utf-8 /EHsc)
    target_link_libraries(hik-bridge-upgrade PRIVATE Advapi32 Shell32 Winhttp Iphlpapi Ws2_32)
    set_property(TARGET hik-bridge-upgrade PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
    target_sources(hik-sdk-http-bridge PRIVATE src/windows-service.cpp)
    target_link_libraries(hik-sdk-http-bridge PRIVATE HCNetSDK ws2_32 delayimp Advapi32)
    target_link_options(hik-sdk-http-bridge PRIVATE /DELAYLOAD:HCNetSDK.dll)
    target_compile_definitions(hik-sdk-http-bridge PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(hik-sdk-http-bridge PRIVATE /W4 /permissive- /utf-8 /EHsc /wd4828)
    set_property(TARGET hik-sdk-http-bridge PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Always stage on a default build, even when only a deployment script changed.
    add_custom_target(windows-package ALL
        DEPENDS hik-sdk-http-bridge hik-bridge-upgrade
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${HIK_WIN32_RUNTIME}/hcnetsdk" "$<TARGET_FILE_DIR:hik-sdk-http-bridge>/hcnetsdk"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${HIK_WIN32_RUNTIME}/ffmpeg" "$<TARGET_FILE_DIR:hik-sdk-http-bridge>/ffmpeg"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeglibs" "$<TARGET_FILE_DIR:hik-sdk-http-bridge>/ffmpeg"
        COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/config/config.windows.json -DDESTINATION=$<TARGET_FILE_DIR:hik-sdk-http-bridge>/config.json -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/stage-config.cmake"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/deploy/windows/run-bridge.cmd" "$<TARGET_FILE_DIR:hik-sdk-http-bridge>/run-bridge.cmd"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/deploy/windows/安装.bat" "$<TARGET_FILE_DIR:hik-sdk-http-bridge>/安装.bat"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/deploy/windows/卸载.bat" "$<TARGET_FILE_DIR:hik-sdk-http-bridge>/卸载.bat"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/deploy/windows/升级.bat" "$<TARGET_FILE_DIR:hik-sdk-http-bridge>/升级.bat"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/deploy/windows/windows-service.cmd" "$<TARGET_FILE_DIR:hik-sdk-http-bridge>/windows-service.cmd"
        VERBATIM)
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

option(HIK_BUILD_TESTS "Build lifecycle regression tests (no NVR required)" OFF)
if(HIK_BUILD_TESTS)
    enable_testing()
    add_executable(hik-lifecycle-tests tests/lifecycle.cpp)
    # The test includes the implementation and exercises real OS resources.
    foreach(property INCLUDE_DIRECTORIES LINK_DIRECTORIES LINK_LIBRARIES COMPILE_OPTIONS COMPILE_DEFINITIONS LINK_OPTIONS)
        get_target_property(value hik-sdk-http-bridge ${property})
        if(value)
            set_property(TARGET hik-lifecycle-tests PROPERTY ${property} "${value}")
        endif()
    endforeach()
    if(WIN32)
        add_executable(hik-upgrade-unit-tests tests/windows-upgrade-unit.cpp)
        add_executable(hik-upgrade-integration-tool src/windows-upgrade.cpp)
        target_compile_definitions(hik-upgrade-integration-tool PRIVATE
            HIK_UPGRADE_NATIVE_SERVICE=L"hikbridge-upgrade-test-native"
            HIK_UPGRADE_MANAGED_SERVICE=L"HikSdkHttpBridge-upgrade-test-managed")
        foreach(upgrade_target hik-upgrade-unit-tests hik-upgrade-integration-tool)
            target_compile_options(${upgrade_target} PRIVATE /W4 /utf-8 /EHsc)
            target_link_libraries(${upgrade_target} PRIVATE Advapi32 Shell32 Winhttp Iphlpapi Ws2_32)
            set_property(TARGET ${upgrade_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endforeach()
        add_test(NAME upgrade-unit COMMAND hik-upgrade-unit-tests "${CMAKE_CURRENT_BINARY_DIR}/upgrade-unit" "$<TARGET_FILE:hik-sdk-http-bridge>")
        add_executable(hik-upgrade-service-fixture src/main.cpp src/windows-service.cpp)
        foreach(property INCLUDE_DIRECTORIES LINK_DIRECTORIES LINK_LIBRARIES COMPILE_OPTIONS COMPILE_DEFINITIONS LINK_OPTIONS)
            get_target_property(value hik-sdk-http-bridge ${property})
            if(value)
                set_property(TARGET hik-upgrade-service-fixture PROPERTY ${property} "${value}")
            endif()
        endforeach()
        target_compile_definitions(hik-upgrade-service-fixture PRIVATE HIK_SERVICE_NAME=L"hikbridge-upgrade-test-native")
        set_property(TARGET hik-upgrade-service-fixture PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        add_executable(hik-cmd-tool-stub tests/cmd-tool-stub.cpp)
        target_compile_options(hik-cmd-tool-stub PRIVATE /utf-8)
        target_sources(hik-lifecycle-tests PRIVATE src/windows-service.cpp)
        set_property(TARGET hik-lifecycle-tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        add_test(NAME lifecycle COMMAND hik-lifecycle-tests "${HIK_WIN32_RUNTIME}/ffmpeg/ffmpeg.exe")
    else()
        find_program(HIK_TEST_FFMPEG ffmpeg)
        if(HIK_TEST_FFMPEG)
            add_test(NAME lifecycle COMMAND hik-lifecycle-tests "${HIK_TEST_FFMPEG}")
        else()
            add_test(NAME lifecycle COMMAND hik-lifecycle-tests)
        endif()
    endif()
    set_tests_properties(lifecycle PROPERTIES TIMEOUT 20)
endif()
