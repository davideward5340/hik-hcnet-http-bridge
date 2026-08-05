#!/usr/bin/env sh
# 该文件会在便携包根目录中以 start.sh 的名字分发。
set -eu

APP_HOME=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
BIN="$APP_HOME/bin/hik-sdk-http-bridge"
SDK_DIR="$APP_HOME/sdk"
COMPONENT_DIR="$SDK_DIR/HCNetSDKCom"
FFMPEG="$APP_HOME/runtime/ffmpeg/ffmpeg"
DEFAULT_CONFIG="$APP_HOME/config/config.json"
MANIFEST="$APP_HOME/release-manifest.env"

fail() {
    printf '%s\n' "错误: $*" >&2
    exit 1
}

version_at_least() {
    # 参数 1: 实际版本；参数 2: 所需最低版本。
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n 1)" = "$2" ]
}

[ "$(uname -m)" = "x86_64" ] || fail "本包仅支持 x86_64；当前架构为 $(uname -m)。ARM、龙芯、MIPS 等架构须使用对应架构 HCNetSDK 重新构建。"
[ -x "$BIN" ] || fail "缺少或无权执行 $BIN"
[ -x "$FFMPEG" ] || fail "缺少或无权执行随包 FFmpeg：$FFMPEG"
[ -r "$SDK_DIR/libhcnetsdk.so" ] || fail "缺少 HCNetSDK 主库：$SDK_DIR/libhcnetsdk.so"
[ -d "$COMPONENT_DIR" ] || fail "缺少 HCNetSDKCom 组件目录：$COMPONENT_DIR"
[ -r "$DEFAULT_CONFIG" ] || fail "缺少默认配置：$DEFAULT_CONFIG"

required_glibc=2.28
if [ -r "$MANIFEST" ]; then
    manifest_glibc=$(sed -n 's/^REQUIRED_GLIBC=//p' "$MANIFEST" | head -n 1 || true)
    [ -z "$manifest_glibc" ] || required_glibc=$manifest_glibc
fi

glibc_line=$(getconf GNU_LIBC_VERSION 2>/dev/null || true)
case "$glibc_line" in
    glibc\ *) host_glibc=${glibc_line#glibc } ;;
    *) fail "未检测到 glibc。当前包不支持 musl/Alpine Linux。" ;;
esac
version_at_least "$host_glibc" "$required_glibc" || fail "当前 glibc 为 $host_glibc，包至少需要 $required_glibc。请使用更低 glibc 基线构建的发布包。"

mkdir -p "$APP_HOME/logs"
export LD_LIBRARY_PATH="$SDK_DIR:$COMPONENT_DIR:$APP_HOME/runtime/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export HIK_BRIDGE_SDK_DIR="$SDK_DIR"
export HIK_BRIDGE_FFMPEG_PATH="$FFMPEG"
export HIK_BRIDGE_LOG_DIR="$APP_HOME/logs"

if [ "${1:-}" = "--version" ]; then
    exec "$BIN" --version
fi

if [ "${1:-}" = "--config" ]; then
    [ "$#" -ge 2 ] || fail "--config 后必须给出配置文件路径"
    exec "$BIN" "$@"
fi

CONFIG=${HIK_BRIDGE_CONFIG:-$DEFAULT_CONFIG}
if [ -n "${HIK_BRIDGE_LOG_FILE:-}" ]; then
    exec "$BIN" --config "$CONFIG" "$@" >> "$HIK_BRIDGE_LOG_FILE" 2>&1
fi
exec "$BIN" --config "$CONFIG" "$@"
