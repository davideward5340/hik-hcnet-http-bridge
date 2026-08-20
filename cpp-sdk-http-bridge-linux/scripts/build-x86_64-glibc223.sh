#!/usr/bin/env sh
# 在 x86_64 WSL 中用 Zig 编译兼容 GLIBC 2.23 的 x86_64 Bridge。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK_INCLUDE_DIR=${SDK_INCLUDE_DIR:-"$ROOT/vendor/hcnetsdk/include"}
SDK_LIBRARY_DIR=${SDK_LIBRARY_DIR:-"$ROOT/vendor/hcnetsdk/sdk"}
ZIG=${ZIG:-/opt/zig-0.15.1/zig}
GLIBC_BASELINE=${GLIBC_BASELINE:-2.23}
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-x86_64-glibc223"}
OUTPUT=${OUTPUT:-"$BUILD_DIR/hik-sdk-http-bridge"}

fail() { printf '%s\n' "Error: $*" >&2; exit 1; }
[ -x "$ZIG" ] || fail "Zig was not found: $ZIG"
[ -r "$SDK_INCLUDE_DIR/HCNetSDK.h" ] || fail "x86_64 HCNetSDK.h was not found"
[ -r "$SDK_LIBRARY_DIR/libhcnetsdk.so" ] || fail "x86_64 libhcnetsdk.so was not found"
command -v readelf >/dev/null 2>&1 || fail "readelf is required"
command -v patchelf >/dev/null 2>&1 || fail "patchelf is required"

sdk_machine=$(readelf -h "$SDK_LIBRARY_DIR/libhcnetsdk.so" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
[ "$sdk_machine" = "Advanced Micro Devices X86-64" ] || fail "The SDK is not x86_64: $sdk_machine"
mkdir -p "$BUILD_DIR"
"$ZIG" c++ -target "x86_64-linux-gnu.$GLIBC_BASELINE" -mcpu=baseline -std=c++17 -O3 -DNDEBUG \
  -Wall -Wextra -Wpedantic -Wno-unused-parameter -Wno-invalid-utf8 \
  -I"$SDK_INCLUDE_DIR" "$ROOT/src/main.cpp" -L"$SDK_LIBRARY_DIR" -lhcnetsdk -pthread -ldl -o "$OUTPUT"

readelf -d "$OUTPUT" | sed -n 's/.*Shared library: \[\(.*\/libhcnetsdk\.so\)\].*/\1/p' |
while IFS= read -r needed; do
  [ -z "$needed" ] || patchelf --replace-needed "$needed" libhcnetsdk.so "$OUTPUT"
done
patchelf --set-rpath '$ORIGIN/../sdk:$ORIGIN/../sdk/HCNetSDKCom' "$OUTPUT"
strip "$OUTPUT"

machine=$(readelf -h "$OUTPUT" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
[ "$machine" = "Advanced Micro Devices X86-64" ] || fail "The bridge is not x86_64: $machine"
required_glibc=$(readelf --version-info "$OUTPUT" 2>/dev/null | sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p' | sort -Vu | tail -n 1)
[ -n "$required_glibc" ] || fail "Unable to determine the bridge GLIBC requirement"
[ "$(printf '%s\n%s\n' "$GLIBC_BASELINE" "$required_glibc" | sort -V | head -n 1)" = "$required_glibc" ] || fail "The bridge requires GLIBC $required_glibc, above baseline $GLIBC_BASELINE"
printf 'x86_64 bridge created: %s\nRequired GLIBC: %s\n' "$OUTPUT" "$required_glibc"
