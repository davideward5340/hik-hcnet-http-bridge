# Debian 10、统信 UOS V20 和银河麒麟 V10 的常见 CMake 均可满足 3.13。
cmake_minimum_required(VERSION 3.13)
project(hik_sdk_http_bridge_linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(hik-sdk-http-bridge src/main.cpp)
target_include_directories(hik-sdk-http-bridge PRIVATE vendor/hcnetsdk/include)
target_link_directories(hik-sdk-http-bridge PRIVATE vendor/hcnetsdk/sdk)
target_link_libraries(hik-sdk-http-bridge PRIVATE hcnetsdk pthread dl)
target_compile_options(hik-sdk-http-bridge PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)

# 便携包仍需要宿主 glibc（HCNetSDK 是动态库），但将 libstdc++/libgcc
# 静态合入可减少统信、深度、麒麟等系统上的 C++ 运行库版本差异。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(hik-sdk-http-bridge PRIVATE -static-libgcc -static-libstdc++)
    # GCC 8 的 std::filesystem 仍位于独立的 libstdc++fs 中；统信/麒麟
    # 的旧版开发工具链可能仍使用该版本。
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
        target_link_libraries(hik-sdk-http-bridge PRIVATE stdc++fs)
    endif()
endif()

set_target_properties(hik-sdk-http-bridge PROPERTIES
    BUILD_RPATH "$ORIGIN/../sdk:$ORIGIN/../sdk/HCNetSDKCom"
    INSTALL_RPATH "$ORIGIN/../sdk:$ORIGIN/../sdk/HCNetSDKCom")
