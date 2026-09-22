#!/usr/bin/env sh
# 发布包根目录中的 start.sh：自动选择架构，透传所有桥接程序参数。
set -eu
APP_HOME=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
case "$(uname -m)" in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    *) printf '不支持的 CPU 架构：%s\n' "$(uname -m)" >&2; exit 1 ;;
esac
IMAGE="$APP_HOME/hik-sdk-http-bridge-0.9.1.260922-$ARCH-glibc2.23.AppImage"
if [ ! -x "$IMAGE" ]; then
    printf 'AppImage 缺失或不可执行：%s\n' "$IMAGE" >&2
    exit 1
fi
# 无 FUSE 环境也能运行；应用、SDK、静态 FFmpeg 均包含在 AppImage 内。
export APPIMAGE_EXTRACT_AND_RUN=1
exec "$IMAGE" "$@"
