#!/usr/bin/env sh
# 使用 AppImage 将已经过兼容基线验证的二进制、完整 SDK 和静态 FFmpeg 封装为单文件。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RELEASE_VERSION=$(sed -n 's/^#define HIK_BRIDGE_VERSION "\(.*\)"/\1/p' "$ROOT/src/version.h")
export VERSION=${VERSION:-$RELEASE_VERSION}
REPO_ROOT=$(CDPATH= cd -- "$ROOT/.." && pwd)
VERSION=$VERSION
ARCH=${ARCH:-x86_64}
if [ "$ARCH" = "aarch64" ]; then
    DEFAULT_BUILD_DIR="$ROOT/build-arm64"
    DEFAULT_FFMPEG="$ROOT/runtime/ffmpeg-aarch64/ffmpeg"
else
    DEFAULT_BUILD_DIR="$ROOT/build-portable"
    DEFAULT_FFMPEG="$ROOT/runtime/ffmpeg/ffmpeg"
fi
BRIDGE_BINARY=${BRIDGE_BINARY:-"$DEFAULT_BUILD_DIR/hik-sdk-http-bridge"}
FFMPEG_BINARY=${FFMPEG_BINARY:-"$DEFAULT_FFMPEG"}
SDK_ROOT=${SDK_ROOT:-"$ROOT/vendor/hcnetsdk/sdk"}
SDK_LICENSE_DIR=${SDK_LICENSE_DIR:-"$ROOT/vendor/licenses"}
FFMPEG_DOC_DIR=${FFMPEG_DOC_DIR:-"$(dirname -- "$FFMPEG_BINARY")"}
COMPAT_GLIBC_MAX=${COMPAT_GLIBC_MAX:-2.28}
APPIMAGETOOL=${APPIMAGETOOL:-"$ROOT/tools/appimagetool-$ARCH.AppImage"}
RUNTIME_FILE=${RUNTIME_FILE:-}
OUTPUT=${OUTPUT:-"$ROOT/dist/hik-sdk-http-bridge-$VERSION-$ARCH.AppImage"}
SDK_VERSION=${SDK_VERSION:-HCNetSDK_V6.1.11.5_linux64}

mkdir -p "$ROOT/dist"
STAGE_ROOT=$(mktemp -d "$ROOT/dist/.appimage-stage.XXXXXX")
APPDIR="$STAGE_ROOT/HikSdkHttpBridge.AppDir"

cleanup() { rm -rf "$STAGE_ROOT"; }
trap cleanup EXIT HUP INT TERM
fail() { printf '%s\n' "Error: $*" >&2; exit 1; }

[ -x "$BRIDGE_BINARY" ] || fail "Validated bridge executable was not found: $BRIDGE_BINARY"
[ -x "$FFMPEG_BINARY" ] || fail "Static FFmpeg was not found: $FFMPEG_BINARY"
[ -x "$APPIMAGETOOL" ] || fail "appimagetool was not found: $APPIMAGETOOL"
[ -d "$SDK_LICENSE_DIR" ] || fail "HCNetSDK license directory is missing: $SDK_LICENSE_DIR"
find "$FFMPEG_DOC_DIR" -maxdepth 1 -type f -name 'LICENSE*' | grep -q . || fail "FFmpeg/x264 licenses are missing: $FFMPEG_DOC_DIR"
[ -r "$SDK_ROOT/libhcnetsdk.so" ] || fail "HCNetSDK is missing: $SDK_ROOT"
command -v readelf >/dev/null 2>&1 || fail "readelf is required"
command -v ldd >/dev/null 2>&1 || fail "ldd is required"

bridge_machine=$(readelf -h "$BRIDGE_BINARY" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
ffmpeg_machine=$(readelf -h "$FFMPEG_BINARY" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
case "$ARCH" in
    x86_64) expected_machine="Advanced Micro Devices X86-64" ;;
    aarch64) expected_machine="AArch64" ;;
    *) fail "Unsupported architecture: $ARCH" ;;
esac
[ "$bridge_machine" = "$expected_machine" ] || fail "The bridge is not a $ARCH ELF: $bridge_machine"
[ "$ffmpeg_machine" = "$expected_machine" ] || fail "FFmpeg is not a $ARCH ELF: $ffmpeg_machine"

ffmpeg_ldd=$(ldd "$FFMPEG_BINARY" 2>&1 || true)
case "$ffmpeg_ldd" in
    *"not a dynamic executable"*|*"statically linked"*) ;;
    *) fail "FFmpeg must be statically linked; ldd output: $ffmpeg_ldd" ;;
esac

required_glibc=$(
    {
        readelf --version-info "$BRIDGE_BINARY"
        find "$SDK_ROOT" -type f -name '*.so*' -exec readelf --version-info {} \;
    } 2>/dev/null \
    | sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p' \
    | sort -Vu | tail -n 1)
[ -n "$required_glibc" ] || fail "Unable to determine the bridge GLIBC requirement"
if [ "$(printf '%s\n%s\n' "$COMPAT_GLIBC_MAX" "$required_glibc" | sort -V | head -n 1)" != "$required_glibc" ]; then
    fail "The bridge requires glibc $required_glibc, above target baseline $COMPAT_GLIBC_MAX; AppImage generation refused."
fi

mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib/hik/runtime/ffmpeg" "$APPDIR/usr/lib/hik/runtime/lib" "$APPDIR/usr/share/hik/config"
install -m 0755 "$BRIDGE_BINARY" "$APPDIR/usr/bin/hik-sdk-http-bridge"
install -m 0755 "$FFMPEG_BINARY" "$APPDIR/usr/lib/hik/runtime/ffmpeg/ffmpeg"
cp -a "$SDK_ROOT" "$APPDIR/usr/lib/hik/sdk"
install -m 0644 "$ROOT/config/config.portable.json" "$APPDIR/usr/share/hik/config/config.json"
install -m 0755 "$ROOT/appimage/AppRun" "$APPDIR/AppRun"
install -m 0644 "$ROOT/appimage/hik-sdk-http-bridge.desktop" "$APPDIR/hik-sdk-http-bridge.desktop"
install -m 0644 "$ROOT/appimage/hik-sdk-http-bridge.svg" "$APPDIR/hik-sdk-http-bridge.svg"
mkdir -p "$APPDIR/usr/share/doc/hik-sdk-http-bridge/licenses"
install -m 0644 "$REPO_ROOT/LICENSE" "$APPDIR/usr/share/doc/hik-sdk-http-bridge/licenses/PROJECT-APACHE-2.0.txt"
install -m 0644 "$REPO_ROOT/NOTICE" "$APPDIR/usr/share/doc/hik-sdk-http-bridge/NOTICE"
find "$SDK_LICENSE_DIR" -maxdepth 1 -type f -name 'Open Source Software Licenses*.txt' \
    -exec cp -a {} "$APPDIR/usr/share/doc/hik-sdk-http-bridge/licenses/" \;
install -m 0644 "$ROOT/runtime/ffmpeg/README.md" "$APPDIR/usr/share/doc/hik-sdk-http-bridge/FFMPEG-BUILD-AND-LICENSES.md"
find "$FFMPEG_DOC_DIR" -maxdepth 1 -type f -name 'LICENSE*' -exec cp -a {} "$APPDIR/usr/share/doc/hik-sdk-http-bridge/licenses/" \;
if [ -r "$(dirname -- "$FFMPEG_BINARY")/ffmpeg-buildconf.txt" ]; then
    install -m 0644 "$(dirname -- "$FFMPEG_BINARY")/ffmpeg-buildconf.txt" "$APPDIR/usr/share/doc/hik-sdk-http-bridge/FFMPEG-BUILDCONF.txt"
else
    "$FFMPEG_BINARY" -buildconf > "$APPDIR/usr/share/doc/hik-sdk-http-bridge/FFMPEG-BUILDCONF.txt"
fi

cat > "$APPDIR/usr/share/hik/release-manifest.env" <<EOF
PACKAGE_NAME=hik-sdk-http-bridge
VERSION=$VERSION
ARCH=$ARCH
REQUIRED_GLIBC=$required_glibc
FFMPEG_LINKAGE=static
SDK_VERSION=$SDK_VERSION
EOF

# Type 2 AppImage 在无 FUSE 的客户机仍可通过 --appimage-extract-and-run 降级执行。
if [ -n "$RUNTIME_FILE" ]; then
    [ -r "$RUNTIME_FILE" ] || fail "AppImage Runtime was not found: $RUNTIME_FILE"
    ARCH="$ARCH" APPIMAGE_EXTRACT_AND_RUN=1 "$APPIMAGETOOL" --runtime-file "$RUNTIME_FILE" "$APPDIR" "$OUTPUT"
else
    ARCH="$ARCH" APPIMAGE_EXTRACT_AND_RUN=1 "$APPIMAGETOOL" "$APPDIR" "$OUTPUT"
fi
chmod 0755 "$OUTPUT"
(cd "$(dirname -- "$OUTPUT")" && sha256sum "$(basename -- "$OUTPUT")" > "$(basename -- "$OUTPUT").sha256")
printf 'AppImage created: %s\nChecksum file: %s.sha256\nMinimum glibc: %s\n' "$OUTPUT" "$OUTPUT" "$required_glibc"
