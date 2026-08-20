#!/usr/bin/env sh
# 使用项目内 ARM SDK、ARM64 Bridge、静态 FFmpeg 和 ARM64 Runtime 生成单文件 AppImage。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARM_SDK_ROOT=${ARM_SDK_ROOT:-"$ROOT/../docs/HCNetSDKV6.1.11.30_build20260805_ArmLinux64_20260807100545"}

export ARCH=aarch64
export BRIDGE_BINARY=${BRIDGE_BINARY:-"$ROOT/build-arm64/hik-sdk-http-bridge"}
export FFMPEG_BINARY=${FFMPEG_BINARY:-"$ROOT/runtime/ffmpeg-aarch64/ffmpeg"}
export FFMPEG_DOC_DIR=${FFMPEG_DOC_DIR:-"$ROOT/runtime/ffmpeg-aarch64"}
export SDK_ROOT=${SDK_ROOT:-"$ARM_SDK_ROOT/MakeAll"}
export SDK_LICENSE_DIR=${SDK_LICENSE_DIR:-"$ARM_SDK_ROOT/doc"}
export SDK_VERSION=${SDK_VERSION:-HCNetSDK_V6.1.11.30_ArmLinux64}
export COMPAT_GLIBC_MAX=${COMPAT_GLIBC_MAX:-2.27}
export APPIMAGETOOL=${APPIMAGETOOL:-"$ROOT/tools/appimagetool-x86_64.AppImage"}
export RUNTIME_FILE=${RUNTIME_FILE:-"$ROOT/tools/runtime-aarch64"}
export OUTPUT=${OUTPUT:-"$ROOT/dist/hik-sdk-http-bridge-${VERSION:-1.0.0}-aarch64.AppImage"}

exec "$ROOT/scripts/package-appimage.sh"
