#!/usr/bin/env sh
# 为 GLIBC 2.23 的 ARM64 系统构建 Bridge，并排除仅用于海康本地播放/语音的 GLIBC 2.27 OpenAL 组件。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARM_SDK_ROOT=${ARM_SDK_ROOT:-"$ROOT/../docs/HCNetSDKV6.1.11.30_build20260805_ArmLinux64_20260807100545"}
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-arm64-glibc223"}
SDK_STAGE=${SDK_STAGE:-"$BUILD_DIR/sdk"}

GLIBC_BASELINE=2.23 BUILD_DIR="$BUILD_DIR" OUTPUT="$BUILD_DIR/hik-sdk-http-bridge" \
  "$ROOT/scripts/build-arm64-wsl.sh"

mkdir -p "$SDK_STAGE"
cp -a "$ARM_SDK_ROOT/MakeAll/." "$SDK_STAGE/"
rm -f \
  "$SDK_STAGE/libopenal.so" \
  "$SDK_STAGE/libopenal.so.1" \
  "$SDK_STAGE/libAudioRender.so" \
  "$SDK_STAGE/libPlayCtrl.so" \
  "$SDK_STAGE/HCNetSDKCom/libopenal.so" \
  "$SDK_STAGE/HCNetSDKCom/libAudioRender.so" \
  "$SDK_STAGE/HCNetSDKCom/libAudioIntercom.so" \
  "$SDK_STAGE/HCNetSDKCom/libHCVoiceTalk.so"

export ARCH=aarch64
export BRIDGE_BINARY="$BUILD_DIR/hik-sdk-http-bridge"
export FFMPEG_BINARY=${FFMPEG_BINARY:-"$ROOT/runtime/ffmpeg-aarch64/ffmpeg"}
export FFMPEG_DOC_DIR=${FFMPEG_DOC_DIR:-"$ROOT/runtime/ffmpeg-aarch64"}
export SDK_ROOT="$SDK_STAGE"
export SDK_LICENSE_DIR=${SDK_LICENSE_DIR:-"$ARM_SDK_ROOT/doc"}
export SDK_VERSION=HCNetSDK_V6.1.11.30_ArmLinux64_no-openal-playctrl-voice
export COMPAT_GLIBC_MAX=2.23
export APPIMAGETOOL=${APPIMAGETOOL:-"$ROOT/tools/appimagetool-x86_64.AppImage"}
export RUNTIME_FILE=${RUNTIME_FILE:-"$ROOT/tools/runtime-aarch64"}
export OUTPUT=${OUTPUT:-"$ROOT/dist/hik-sdk-http-bridge-${VERSION:-1.0.0}-aarch64-glibc2.23.AppImage"}

exec "$ROOT/scripts/package-appimage.sh"
