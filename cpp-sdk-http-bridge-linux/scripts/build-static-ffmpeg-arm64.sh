#!/usr/bin/env sh
# 在 x86_64 Linux/WSL 中交叉编译完整静态 ARM64 FFmpeg + x264。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE_DIR=${FFMPEG_SOURCE_DIR:?请设置 FFMPEG_SOURCE_DIR 为 FFmpeg 官方源码目录}
X264_SOURCE_DIR=${X264_SOURCE_DIR:?请设置 X264_SOURCE_DIR 为 x264 官方源码目录}
OUTPUT_DIR=${OUTPUT_DIR:-"$ROOT/runtime/ffmpeg-aarch64"}
OUTPUT=${OUTPUT:-"$OUTPUT_DIR/ffmpeg"}
X264_PREFIX=${X264_PREFIX:-"$ROOT/.build/x264-aarch64"}
JOBS=${JOBS:-"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"}

[ -x "$SOURCE_DIR/configure" ] || { echo "Invalid FFmpeg source directory: $SOURCE_DIR" >&2; exit 1; }
[ -x "$X264_SOURCE_DIR/configure" ] || { echo "Invalid x264 source directory: $X264_SOURCE_DIR" >&2; exit 1; }
command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || { echo "The ARM64 cross compiler is required" >&2; exit 1; }

mkdir -p "$X264_PREFIX" "$OUTPUT_DIR"
cd "$X264_SOURCE_DIR"
make distclean >/dev/null 2>&1 || true
./configure --host=aarch64-linux --cross-prefix=aarch64-linux-gnu- \
  --prefix="$X264_PREFIX" --enable-static --disable-cli --disable-opencl
make -j"$JOBS"
make install

cd "$SOURCE_DIR"
make distclean >/dev/null 2>&1 || true
PKG_CONFIG_LIBDIR="$X264_PREFIX/lib/pkgconfig" ./configure \
  --arch=aarch64 --target-os=linux --enable-cross-compile --cross-prefix=aarch64-linux-gnu- \
  --cc=aarch64-linux-gnu-gcc --cxx=aarch64-linux-gnu-g++ --ar=aarch64-linux-gnu-ar \
  --ranlib=aarch64-linux-gnu-ranlib --strip=aarch64-linux-gnu-strip \
  --pkg-config=pkg-config --pkg-config-flags=--static \
  --disable-shared --enable-static --disable-doc --disable-debug --disable-network --disable-autodetect --disable-everything \
  --enable-gpl --enable-libx264 --enable-ffmpeg --enable-protocol=pipe --enable-demuxer=mpegps --enable-muxer=mp4 \
  --enable-parser=h264 --enable-parser=hevc --enable-parser=aac --enable-parser=mpegaudio \
  --enable-decoder=h264 --enable-decoder=hevc --enable-decoder=aac --enable-decoder=ac3 --enable-decoder=mp2 --enable-decoder=mp3 \
  --enable-decoder=pcm_alaw --enable-decoder=pcm_mulaw --enable-decoder=adpcm_g722 \
  --enable-encoder=libx264 --enable-encoder=aac \
  --enable-filter=aresample --enable-filter=scale --enable-filter=format --enable-filter=setpts \
  --enable-swresample --enable-swscale \
  --extra-cflags="-I$X264_PREFIX/include -O3" --extra-ldflags="-L$X264_PREFIX/lib -static" --extra-libs="-lpthread -lm"
make -j"$JOBS"

install -m 0755 ffmpeg "$OUTPUT"
install -m 0644 COPYING.GPLv2 "$OUTPUT_DIR/LICENSE.GPLv2"
install -m 0644 "$X264_SOURCE_DIR/COPYING" "$OUTPUT_DIR/LICENSE.x264.GPLv2"
if command -v qemu-aarch64-static >/dev/null 2>&1; then
  qemu-aarch64-static "$OUTPUT" -buildconf > "$OUTPUT_DIR/ffmpeg-buildconf.txt"
fi
file "$OUTPUT"
printf 'ARM64 static FFmpeg created: %s\n' "$OUTPUT"
