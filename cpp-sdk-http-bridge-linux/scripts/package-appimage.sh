#!/usr/bin/env sh
# 使用 AppImage 将已经过兼容基线验证的二进制、完整 SDK 和静态 FFmpeg 封装为单文件。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=${VERSION:-1.0.0}
ARCH=x86_64
BRIDGE_BINARY=${BRIDGE_BINARY:-"$ROOT/build-portable/hik-sdk-http-bridge"}
FFMPEG_BINARY=${FFMPEG_BINARY:-"$ROOT/runtime/ffmpeg/ffmpeg"}
COMPAT_GLIBC_MAX=${COMPAT_GLIBC_MAX:-2.28}
APPIMAGETOOL=${APPIMAGETOOL:-"$ROOT/tools/appimagetool-$ARCH.AppImage"}
OUTPUT=${OUTPUT:-"$ROOT/dist/hik-sdk-http-bridge-$VERSION-$ARCH.AppImage"}

mkdir -p "$ROOT/dist"
STAGE_ROOT=$(mktemp -d "$ROOT/dist/.appimage-stage.XXXXXX")
APPDIR="$STAGE_ROOT/HikSdkHttpBridge.AppDir"

cleanup() { rm -rf "$STAGE_ROOT"; }
trap cleanup EXIT HUP INT TERM
fail() { printf '%s\n' "错误: $*" >&2; exit 1; }

[ -x "$BRIDGE_BINARY" ] || fail "未找到已通过基线验证的桥接程序：$BRIDGE_BINARY"
[ -x "$FFMPEG_BINARY" ] || fail "未找到静态 FFmpeg：$FFMPEG_BINARY"
[ -x "$APPIMAGETOOL" ] || fail "未找到 appimagetool：$APPIMAGETOOL"
[ -r "$ROOT/runtime/ffmpeg/LICENSE.LGPL-2.1" ] || fail "缺少随 FFmpeg 分发的 LGPL-2.1 许可证"
[ -r "$ROOT/vendor/hcnetsdk/sdk/libhcnetsdk.so" ] || fail "缺少 HCNetSDK"
command -v readelf >/dev/null 2>&1 || fail "缺少 readelf"
command -v ldd >/dev/null 2>&1 || fail "缺少 ldd"

bridge_machine=$(readelf -h "$BRIDGE_BINARY" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
ffmpeg_machine=$(readelf -h "$FFMPEG_BINARY" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
[ "$bridge_machine" = "Advanced Micro Devices X86-64" ] || fail "桥接程序不是 x86_64 ELF：$bridge_machine"
[ "$ffmpeg_machine" = "Advanced Micro Devices X86-64" ] || fail "FFmpeg 不是 x86_64 ELF：$ffmpeg_machine"

ffmpeg_ldd=$(ldd "$FFMPEG_BINARY" 2>&1 || true)
case "$ffmpeg_ldd" in
    *"not a dynamic executable"*|*"statically linked"*) ;;
    *) fail "FFmpeg 必须静态链接，当前 ldd 输出：$ffmpeg_ldd" ;;
esac

required_glibc=$(
    {
        readelf --version-info "$BRIDGE_BINARY"
        find "$ROOT/vendor/hcnetsdk/sdk" -type f -name '*.so*' -exec readelf --version-info {} \;
    } 2>/dev/null \
    | sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p' \
    | sort -Vu | tail -n 1)
[ -n "$required_glibc" ] || fail "无法识别桥接程序的 GLIBC 依赖"
if [ "$(printf '%s\n%s\n' "$COMPAT_GLIBC_MAX" "$required_glibc" | sort -V | head -n 1)" != "$required_glibc" ]; then
    fail "桥接程序要求 glibc $required_glibc，超过目标基线 $COMPAT_GLIBC_MAX；拒绝生成 AppImage。"
fi

mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib/hik/runtime/ffmpeg" "$APPDIR/usr/lib/hik/runtime/lib" "$APPDIR/usr/share/hik/config"
install -m 0755 "$BRIDGE_BINARY" "$APPDIR/usr/bin/hik-sdk-http-bridge"
install -m 0755 "$FFMPEG_BINARY" "$APPDIR/usr/lib/hik/runtime/ffmpeg/ffmpeg"
cp -a "$ROOT/vendor/hcnetsdk/sdk" "$APPDIR/usr/lib/hik/sdk"
install -m 0644 "$ROOT/config/config.portable.json" "$APPDIR/usr/share/hik/config/config.json"
install -m 0755 "$ROOT/appimage/AppRun" "$APPDIR/AppRun"
install -m 0644 "$ROOT/appimage/hik-sdk-http-bridge.desktop" "$APPDIR/hik-sdk-http-bridge.desktop"
install -m 0644 "$ROOT/appimage/hik-sdk-http-bridge.svg" "$APPDIR/hik-sdk-http-bridge.svg"
mkdir -p "$APPDIR/usr/share/doc/hik-sdk-http-bridge/licenses"
cp -a "$ROOT/vendor/licenses/." "$APPDIR/usr/share/doc/hik-sdk-http-bridge/licenses/"
install -m 0644 "$ROOT/runtime/ffmpeg/README.md" "$APPDIR/usr/share/doc/hik-sdk-http-bridge/FFMPEG-BUILD-AND-LICENSES.md"
install -m 0644 "$ROOT/runtime/ffmpeg/LICENSE.LGPL-2.1" "$APPDIR/usr/share/doc/hik-sdk-http-bridge/FFMPEG-LGPL-2.1.txt"
"$FFMPEG_BINARY" -buildconf > "$APPDIR/usr/share/doc/hik-sdk-http-bridge/FFMPEG-BUILDCONF.txt"

cat > "$APPDIR/usr/share/hik/release-manifest.env" <<EOF
PACKAGE_NAME=hik-sdk-http-bridge
VERSION=$VERSION
ARCH=$ARCH
REQUIRED_GLIBC=$required_glibc
FFMPEG_LINKAGE=static
SDK_VERSION=HCNetSDK_V6.1.11.5_linux64
EOF

# Type 2 AppImage 在无 FUSE 的客户机仍可通过 --appimage-extract-and-run 降级执行。
ARCH="$ARCH" APPIMAGE_EXTRACT_AND_RUN=1 "$APPIMAGETOOL" "$APPDIR" "$OUTPUT"
chmod 0755 "$OUTPUT"
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
printf '已生成 AppImage：%s\n校验文件：%s.sha256\n最低 glibc：%s\n' "$OUTPUT" "$OUTPUT" "$required_glibc"
