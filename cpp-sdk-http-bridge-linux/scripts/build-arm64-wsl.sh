#!/usr/bin/env sh
# 在 x86_64 WSL 中用 Zig 交叉编译兼容 GLIBC 2.27 的 ARM64 Bridge。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARM_SDK_ROOT=${ARM_SDK_ROOT:-"$ROOT/../docs/HCNetSDKV6.1.11.30_build20260805_ArmLinux64_20260807100545"}
SDK_INCLUDE_DIR=${SDK_INCLUDE_DIR:-"$ARM_SDK_ROOT/incCn"}
SDK_LIBRARY_DIR=${SDK_LIBRARY_DIR:-"$ARM_SDK_ROOT/MakeAll"}
ZIG=${ZIG:-/opt/zig-0.15.1/zig}
GLIBC_BASELINE=${GLIBC_BASELINE:-2.27}
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-arm64"}
OUTPUT=${OUTPUT:-"$BUILD_DIR/hik-sdk-http-bridge"}

fail() { printf '%s\n' "Error: $*" >&2; exit 1; }

[ -x "$ZIG" ] || fail "Zig was not found: $ZIG"
[ -r "$SDK_INCLUDE_DIR/HCNetSDK.h" ] || fail "ARM64 HCNetSDK.h was not found: $SDK_INCLUDE_DIR"
[ -r "$SDK_LIBRARY_DIR/libhcnetsdk.so" ] || fail "ARM64 libhcnetsdk.so was not found: $SDK_LIBRARY_DIR"
command -v readelf >/dev/null 2>&1 || fail "readelf is required"
command -v patchelf >/dev/null 2>&1 || fail "patchelf is required"
command -v aarch64-linux-gnu-strip >/dev/null 2>&1 || fail "aarch64-linux-gnu-strip is required"

sdk_machine=$(readelf -h "$SDK_LIBRARY_DIR/libhcnetsdk.so" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
[ "$sdk_machine" = "AArch64" ] || fail "The SDK is not ARM64: $sdk_machine"

mkdir -p "$BUILD_DIR"
"$ZIG" c++ \
  -target "aarch64-linux-gnu.$GLIBC_BASELINE" -mcpu=baseline \
  -std=c++17 -O3 -DNDEBUG \
  -Wall -Wextra -Wpedantic -Wno-unused-parameter -Wno-invalid-utf8 \
  -I"$SDK_INCLUDE_DIR" "$ROOT/src/main.cpp" \
  -L"$SDK_LIBRARY_DIR" -lhcnetsdk -pthread -ldl \
  -o "$OUTPUT"

# ARM SDK 的 libhcnetsdk.so 没有 SONAME；LLD 会把构建机路径写入 NEEDED，发布前改回库名。
readelf -d "$OUTPUT" | sed -n 's/.*Shared library: \[\(.*\/libhcnetsdk\.so\)\].*/\1/p' |
while IFS= read -r needed; do
  [ -z "$needed" ] || patchelf --replace-needed "$needed" libhcnetsdk.so "$OUTPUT"
done
patchelf --set-rpath '$ORIGIN/../sdk:$ORIGIN/../sdk/HCNetSDKCom' "$OUTPUT"
aarch64-linux-gnu-strip "$OUTPUT"

machine=$(readelf -h "$OUTPUT" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
[ "$machine" = "AArch64" ] || fail "The bridge is not ARM64: $machine"
required_glibc=$(
  readelf --version-info "$OUTPUT" 2>/dev/null |
  sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p' | sort -Vu | tail -n 1)
[ -n "$required_glibc" ] || fail "Unable to determine the bridge GLIBC requirement"
[ "$(printf '%s\n%s\n' "$GLIBC_BASELINE" "$required_glibc" | sort -V | head -n 1)" = "$required_glibc" ] ||
  fail "The bridge requires GLIBC $required_glibc, above baseline $GLIBC_BASELINE"

printf 'ARM64 bridge created: %s\nRequired GLIBC: %s\n' "$OUTPUT" "$required_glibc"
