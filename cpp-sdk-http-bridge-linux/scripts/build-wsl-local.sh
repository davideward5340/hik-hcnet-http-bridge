#!/usr/bin/env bash
# Ubuntu WSL 本地验证构建：不安装 CMake，直接使用与 CMakeLists.txt 等价的 g++ 参数。
# 此产物使用当前 WSL 的 glibc，仅用于本机验证；正式信创发布仍须在 glibc 2.28 基线构建。
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-wsl"}
mkdir -p "$BUILD_DIR"

g++ -std=c++17 -O2 -DNDEBUG \
  -Wall -Wextra -Wpedantic -Wno-unused-parameter \
  "$ROOT/src/main.cpp" \
  -I"$ROOT/vendor/hcnetsdk/include" \
  -L"$ROOT/vendor/hcnetsdk/sdk" \
  -Wl,-rpath,'$ORIGIN/../sdk:$ORIGIN/../sdk/HCNetSDKCom' \
  -lhcnetsdk -pthread -ldl -static-libgcc -static-libstdc++ \
  -o "$BUILD_DIR/hik-sdk-http-bridge"

file "$BUILD_DIR/hik-sdk-http-bridge"
readelf -d "$BUILD_DIR/hik-sdk-http-bridge" | grep -E 'NEEDED|RUNPATH|RPATH'
