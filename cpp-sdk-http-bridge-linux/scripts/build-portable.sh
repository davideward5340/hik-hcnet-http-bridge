#!/usr/bin/env sh
# 必须在发布基线系统（建议 glibc 2.28 的统信 V20/麒麟 V10 构建机）执行。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-portable"}
COMPAT_GLIBC_MAX=${COMPAT_GLIBC_MAX:-2.28}
BIN="$BUILD_DIR/hik-sdk-http-bridge"

command -v cmake >/dev/null 2>&1 || { echo "缺少 cmake" >&2; exit 1; }
command -v readelf >/dev/null 2>&1 || { echo "缺少 readelf" >&2; exit 1; }

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel

required_glibc=$(
    {
        readelf --version-info "$BIN"
        find "$ROOT/vendor/hcnetsdk/sdk" -type f -name '*.so*' -exec readelf --version-info {} \;
    } 2>/dev/null \
    | sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p' \
    | sort -Vu | tail -n 1)
[ -n "$required_glibc" ] || { echo "无法识别二进制的 GLIBC 依赖" >&2; exit 1; }

if [ "$(printf '%s\n%s\n' "$COMPAT_GLIBC_MAX" "$required_glibc" | sort -V | head -n 1)" != "$required_glibc" ]; then
    cat >&2 <<EOF
构建失败：当前二进制要求 glibc $required_glibc，高于目标基线 $COMPAT_GLIBC_MAX。
请在 glibc $COMPAT_GLIBC_MAX 或更低版本的 x86_64 构建机重新执行本脚本，
例如统信 UOS V20 或银河麒麟 V10 的开发环境；不要用 Ubuntu 24.04 产出发布包。
EOF
    exit 1
fi

printf '构建完成：%s\n所需 glibc：%s\n' "$BIN" "$required_glibc"
