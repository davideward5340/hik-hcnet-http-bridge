#!/usr/bin/env sh
# 在与桥接程序相同的 glibc 基线构建机中执行。
# 使用者需自行从 FFmpeg 官方发布包准备源码目录，并履行许可证义务。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE_DIR=${FFMPEG_SOURCE_DIR:?请设置 FFMPEG_SOURCE_DIR 为已解压的 FFmpeg 官方源码目录}
X264_SOURCE_DIR=${X264_SOURCE_DIR:?请设置 X264_SOURCE_DIR 为已解压的 x264 源码目录；H.265 转 H.264 依赖此组件}
OUTPUT=${OUTPUT:-"$ROOT/runtime/ffmpeg/ffmpeg"}
JOBS=${JOBS:-"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"}
X264_PREFIX=${X264_PREFIX:-"$ROOT/.build/x264"}

[ -x "$SOURCE_DIR/configure" ] || { echo "Invalid FFmpeg source directory: $SOURCE_DIR" >&2; exit 1; }
[ -x "$X264_SOURCE_DIR/configure" ] || { echo "Invalid x264 source directory: $X264_SOURCE_DIR" >&2; exit 1; }
command -v make >/dev/null 2>&1 || { echo "make is required" >&2; exit 1; }
command -v ldd >/dev/null 2>&1 || { echo "ldd is required" >&2; exit 1; }

mkdir -p "$X264_PREFIX"
cd "$X264_SOURCE_DIR"
make distclean >/dev/null 2>&1 || true
./configure --prefix="$X264_PREFIX" --enable-static --disable-opencl --disable-cli --disable-avs --disable-lavf
make -j"$JOBS"
make install

cd "$SOURCE_DIR"
make distclean >/dev/null 2>&1 || true
export PKG_CONFIG_PATH="$X264_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# 浏览器输出统一为 H.264 fMP4：H.264 和 H.265 输入均可播放，H.265 会自动转为 H.264。
# libx264 为 GPL 组件，交付包必须附带 GPL 许可证及对应源码获取方式。
./configure \
  --disable-shared --enable-static --disable-doc --disable-debug --enable-gpl --enable-libx264 --pkg-config-flags="--static" \
  --disable-network --disable-autodetect --disable-x86asm --disable-everything \
  --enable-ffmpeg --enable-protocol=pipe --enable-demuxer=mpegps --enable-muxer=mp4 \
  --enable-parser=h264 --enable-parser=hevc --enable-parser=aac --enable-parser=mpegaudio \
  --enable-decoder=h264 --enable-decoder=hevc --enable-decoder=aac --enable-decoder=ac3 --enable-decoder=mp2 --enable-decoder=mp3 \
  --enable-decoder=pcm_alaw --enable-decoder=pcm_mulaw --enable-decoder=adpcm_g722 \
  --enable-encoder=libx264 --enable-encoder=aac --enable-filter=aresample --enable-filter=scale --enable-filter=format --enable-filter=setpts --enable-swresample --enable-swscale \
  --extra-cflags="-I$X264_PREFIX/include" --extra-ldflags="-L$X264_PREFIX/lib -static"
make -j"$JOBS"

mkdir -p "$(dirname -- "$OUTPUT")"
install -m 0755 ffmpeg "$OUTPUT"
install -m 0644 COPYING.GPLv2 "$(dirname -- "$OUTPUT")/LICENSE.GPLv2"
install -m 0644 "$X264_SOURCE_DIR/COPYING" "$(dirname -- "$OUTPUT")/LICENSE.x264.GPLv2"
ffmpeg_ldd=$(ldd "$OUTPUT" 2>&1 || true)
case "$ffmpeg_ldd" in
    *"not a dynamic executable"*|*"statically linked"*) ;;
    *) echo "The built FFmpeg still depends on shared libraries and was rejected: $ffmpeg_ldd" >&2; exit 1 ;;
esac
"$OUTPUT" -version
"$OUTPUT" -buildconf > "$(dirname -- "$OUTPUT")/ffmpeg-buildconf.txt"
printf 'Static FFmpeg created: %s\n' "$OUTPUT"
