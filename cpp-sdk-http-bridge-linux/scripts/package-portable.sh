#!/usr/bin/env sh
# 将已在兼容基线系统上构建的程序、HCNetSDK 和静态 FFmpeg 打成客户机免安装包。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPO_ROOT=$(CDPATH= cd -- "$ROOT/.." && pwd)
VERSION=${VERSION:-1.0.0}
PACKAGE_NAME="hik-sdk-http-bridge-linux-x86_64-$VERSION"
BRIDGE_BINARY=${BRIDGE_BINARY:-"$ROOT/build-portable/hik-sdk-http-bridge"}
FFMPEG_BINARY=${FFMPEG_BINARY:-"$ROOT/runtime/ffmpeg/ffmpeg"}
COMPAT_GLIBC_MAX=${COMPAT_GLIBC_MAX:-2.28}
OUTPUT=${OUTPUT:-"$ROOT/dist/$PACKAGE_NAME.tar.gz"}
mkdir -p "$ROOT/dist"
STAGE_ROOT=$(mktemp -d "$ROOT/dist/.portable-stage.XXXXXX")
PACKAGE_DIR="$STAGE_ROOT/$PACKAGE_NAME"

cleanup() { rm -rf "$STAGE_ROOT"; }
trap cleanup EXIT HUP INT TERM
fail() { printf '%s\n' "错误: $*" >&2; exit 1; }

[ -x "$BRIDGE_BINARY" ] || fail "未找到发布二进制：$BRIDGE_BINARY。请先在 glibc 基线构建机执行 scripts/build-portable.sh。"
[ -x "$FFMPEG_BINARY" ] || fail "未找到静态 FFmpeg：$FFMPEG_BINARY。请按 runtime/ffmpeg/README.md 放置。"
[ -r "$ROOT/runtime/ffmpeg/LICENSE.LGPL-2.1" ] || fail "缺少随 FFmpeg 分发的 LGPL-2.1 许可证"
[ -r "$ROOT/vendor/hcnetsdk/sdk/libhcnetsdk.so" ] || fail "缺少 vendor/hcnetsdk/sdk/libhcnetsdk.so"
command -v readelf >/dev/null 2>&1 || fail "缺少 readelf"
command -v ldd >/dev/null 2>&1 || fail "缺少 ldd"

bridge_machine=$(readelf -h "$BRIDGE_BINARY" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
ffmpeg_machine=$(readelf -h "$FFMPEG_BINARY" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
[ "$bridge_machine" = "Advanced Micro Devices X86-64" ] || fail "桥接程序不是 x86_64 ELF：$bridge_machine"
[ "$ffmpeg_machine" = "Advanced Micro Devices X86-64" ] || fail "FFmpeg 不是 x86_64 ELF：$ffmpeg_machine"

ffmpeg_ldd=$(ldd "$FFMPEG_BINARY" 2>&1 || true)
case "$ffmpeg_ldd" in
    *"not a dynamic executable"*|*"statically linked"*) ;;
    *) fail "FFmpeg 必须是静态 x86_64 二进制，不能依赖客户机共享库。当前 ldd 输出：$ffmpeg_ldd" ;;
esac

required_glibc=$(
    {
        readelf --version-info "$BRIDGE_BINARY"
        find "$ROOT/vendor/hcnetsdk/sdk" -type f -name '*.so*' -exec readelf --version-info {} \;
    } 2>/dev/null \
    | sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p' \
    | sort -Vu | tail -n 1)
[ -n "$required_glibc" ] || fail "无法识别发布二进制的 GLIBC 依赖"
if [ "$(printf '%s\n%s\n' "$COMPAT_GLIBC_MAX" "$required_glibc" | sort -V | head -n 1)" != "$required_glibc" ]; then
    fail "发布二进制要求 glibc $required_glibc，超过目标基线 $COMPAT_GLIBC_MAX；禁止打包。"
fi

mkdir -p "$PACKAGE_DIR/bin" "$PACKAGE_DIR/config" "$PACKAGE_DIR/runtime/ffmpeg" "$PACKAGE_DIR/runtime/lib" "$PACKAGE_DIR/logs" "$PACKAGE_DIR/licenses"
install -m 0755 "$BRIDGE_BINARY" "$PACKAGE_DIR/bin/hik-sdk-http-bridge"
install -m 0755 "$FFMPEG_BINARY" "$PACKAGE_DIR/runtime/ffmpeg/ffmpeg"
cp -a "$ROOT/vendor/hcnetsdk/sdk" "$PACKAGE_DIR/sdk"
install -m 0644 "$ROOT/config/config.portable.json" "$PACKAGE_DIR/config/config.json"
install -m 0755 "$ROOT/scripts/start-portable.sh" "$PACKAGE_DIR/start.sh"
cp -a "$ROOT/vendor/licenses/." "$PACKAGE_DIR/licenses/"
install -m 0644 "$REPO_ROOT/LICENSE" "$PACKAGE_DIR/licenses/PROJECT-APACHE-2.0.txt"
install -m 0644 "$REPO_ROOT/NOTICE" "$PACKAGE_DIR/NOTICE"
install -m 0644 "$ROOT/runtime/ffmpeg/README.md" "$PACKAGE_DIR/licenses/FFMPEG-BUILD-AND-LICENSES.md"
install -m 0644 "$ROOT/runtime/ffmpeg/LICENSE.LGPL-2.1" "$PACKAGE_DIR/licenses/FFMPEG-LGPL-2.1.txt"
"$FFMPEG_BINARY" -buildconf > "$PACKAGE_DIR/licenses/FFMPEG-BUILDCONF.txt"
install -m 0644 "$ROOT/PORTABLE_PACKAGE.md" "$PACKAGE_DIR/README.md"

cat > "$PACKAGE_DIR/release-manifest.env" <<EOF
PACKAGE_NAME=$PACKAGE_NAME
VERSION=$VERSION
ARCH=x86_64
REQUIRED_GLIBC=$required_glibc
FFMPEG_LINKAGE=static
SDK_VERSION=HCNetSDK_V6.1.11.5_linux64
EOF

mkdir -p "$(dirname -- "$OUTPUT")"
tar -C "$STAGE_ROOT" -czf "$OUTPUT" "$PACKAGE_NAME"
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
printf '已生成便携包：%s\n校验文件：%s.sha256\n最低 glibc：%s\n' "$OUTPUT" "$OUTPUT" "$required_glibc"
