#!/usr/bin/env sh
# 生成适用于 GLIBC 2.23 的 x86_64 AppImage；排除未使用且要求 GLIBC 2.27 的本地播放/语音组件。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-x86_64-glibc223"}
SDK_STAGE=${SDK_STAGE:-"$BUILD_DIR/sdk"}

GLIBC_BASELINE=2.23 BUILD_DIR="$BUILD_DIR" OUTPUT="$BUILD_DIR/hik-sdk-http-bridge" "$ROOT/scripts/build-x86_64-glibc223.sh"
mkdir -p "$SDK_STAGE"
cp -a "$ROOT/vendor/hcnetsdk/sdk/." "$SDK_STAGE/"
rm -f \
  "$SDK_STAGE/libopenal.so.1" \
  "$SDK_STAGE/libAudioRender.so" \
  "$SDK_STAGE/libPlayCtrl.so" \
  "$SDK_STAGE/HCNetSDKCom/libopenal.so" \
  "$SDK_STAGE/HCNetSDKCom/libAudioRender.so" \
  "$SDK_STAGE/HCNetSDKCom/libAudioIntercom.so" \
  "$SDK_STAGE/HCNetSDKCom/libHCVoiceTalk.so"

export ARCH=x86_64
export BRIDGE_BINARY="$BUILD_DIR/hik-sdk-http-bridge"
export FFMPEG_BINARY=${FFMPEG_BINARY:-"$ROOT/runtime/ffmpeg/ffmpeg"}
export FFMPEG_DOC_DIR=${FFMPEG_DOC_DIR:-"$ROOT/runtime/ffmpeg"}
export SDK_ROOT="$SDK_STAGE"
export SDK_LICENSE_DIR=${SDK_LICENSE_DIR:-"$ROOT/vendor/licenses"}
export SDK_VERSION=HCNetSDK_V6.1.11.5_linux64_no-openal-playctrl-voice
export COMPAT_GLIBC_MAX=2.23
export APPIMAGETOOL=${APPIMAGETOOL:-"$ROOT/tools/appimagetool-x86_64.AppImage"}
export RUNTIME_FILE=${RUNTIME_FILE:-"$ROOT/tools/runtime-x86_64"}
export OUTPUT=${OUTPUT:-"$ROOT/dist/hik-sdk-http-bridge-${VERSION:-1.0.0}-x86_64-glibc2.23.AppImage"}
exec "$ROOT/scripts/package-appimage.sh"
