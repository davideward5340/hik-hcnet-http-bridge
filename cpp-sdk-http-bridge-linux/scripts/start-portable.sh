#!/usr/bin/env sh
# 该文件会在便携包根目录中以 start.sh 的名字分发。
set -eu

if command -v locale >/dev/null 2>&1 && locale -a 2>/dev/null | grep -Eiq '^C([.]UTF-?8|[.]utf8)$'; then
    export LANG=C.UTF-8
    export LC_ALL=C.UTF-8
fi

APP_HOME=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
BIN="$APP_HOME/bin/hik-sdk-http-bridge"
SDK_DIR="$APP_HOME/sdk"
COMPONENT_DIR="$SDK_DIR/HCNetSDKCom"
FFMPEG="$APP_HOME/runtime/ffmpeg/ffmpeg"
DEFAULT_CONFIG="$APP_HOME/config/config.json"
MANIFEST="$APP_HOME/release-manifest.env"

fail() {
    printf '%s\n' "错误：$*" >&2
    exit 1
}

version_at_least() {
    # 参数 1: 实际版本；参数 2: 所需最低版本。
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n 1)" = "$2" ]
}

[ "$(uname -m)" = "x86_64" ] || fail "本包需要 x86_64，当前架构为 $(uname -m)；其他架构需要匹配的 HCNetSDK。"
[ -x "$BIN" ] || fail "桥接程序缺失或不可执行：$BIN"
[ -x "$FFMPEG" ] || fail "随包 FFmpeg 缺失或不可执行：$FFMPEG"
[ -r "$SDK_DIR/libhcnetsdk.so" ] || fail "HCNetSDK 主库缺失：$SDK_DIR/libhcnetsdk.so"
[ -d "$COMPONENT_DIR" ] || fail "HCNetSDKCom 组件目录缺失：$COMPONENT_DIR"
[ -r "$DEFAULT_CONFIG" ] || fail "默认配置缺失：$DEFAULT_CONFIG"

required_glibc=2.28
if [ -r "$MANIFEST" ]; then
    manifest_glibc=$(sed -n 's/^REQUIRED_GLIBC=//p' "$MANIFEST" | head -n 1 || true)
    [ -z "$manifest_glibc" ] || required_glibc=$manifest_glibc
fi

glibc_line=$(getconf GNU_LIBC_VERSION 2>/dev/null || true)
case "$glibc_line" in
    glibc\ *) host_glibc=${glibc_line#glibc } ;;
    *) fail "未检测到 glibc；本包不支持 musl/Alpine Linux。" ;;
esac
version_at_least "$host_glibc" "$required_glibc" || fail "当前 glibc 为 $host_glibc，本包至少需要 $required_glibc。"

mkdir -p "$APP_HOME/logs"
export LD_LIBRARY_PATH="$SDK_DIR:$COMPONENT_DIR:$APP_HOME/runtime/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export HIK_BRIDGE_SDK_DIR="$SDK_DIR"
export HIK_BRIDGE_FFMPEG_PATH="$FFMPEG"
export HIK_BRIDGE_LOG_DIR="$APP_HOME/logs"

if [ "${1:-}" = "--version" ]; then
    exec "$BIN" --version
fi

if [ "${1:-}" = "--config" ]; then
    [ "$#" -ge 2 ] || fail "--config 后必须提供配置文件路径。"
    exec "$BIN" "$@"
fi

CONFIG=${HIK_BRIDGE_CONFIG:-$DEFAULT_CONFIG}
if [ -n "${HIK_BRIDGE_LOG_FILE:-}" ]; then
    exec "$BIN" --config "$CONFIG" "$@" >> "$HIK_BRIDGE_LOG_FILE" 2>&1
fi
exec "$BIN" --config "$CONFIG" "$@"
