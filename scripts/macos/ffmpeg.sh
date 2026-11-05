#!/usr/bin/env bash
# Builds the FFmpeg libraries the games link (libavcodec, libavutil, libswscale) from the
# pinned release: the MPEG-2 video and FLAC codecs only, shared, LGPL, no external libraries.
#
#   scripts/macos/ffmpeg.sh <prefix>        build into <prefix> (cached when <prefix>/RECIPE matches)
#   scripts/macos/ffmpeg.sh --print-args    the configure arguments, one per line
set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.1}"
FFMPEG_SHA256="${FFMPEG_SHA256:-733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1}"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"

CONFIGURE_ARGS=(
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
  --enable-shared --disable-static --enable-pic
  --disable-gpl --disable-nonfree --disable-version3 --disable-debug
  --disable-avdevice --disable-avformat --disable-avfilter --disable-postproc --disable-swresample
  --enable-avcodec --enable-avutil --enable-swscale
  --enable-decoder=mpeg2video --enable-decoder=mpeg1video --enable-parser=mpegvideo
  --enable-decoder=flac --enable-encoder=flac --enable-parser=flac
)

if [ "${1:-}" = "--print-args" ]; then
  printf '%s\n' "${CONFIGURE_ARGS[@]}"
  exit 0
fi

PREFIX="${1:?usage: ffmpeg.sh <prefix> | --print-args}"
case "$(uname -m)" in
  x86_64) command -v nasm >/dev/null 2>&1 || CONFIGURE_ARGS+=(--disable-x86asm) ;;
esac
RECIPE="$(printf 'ffmpeg-%s\n' "$FFMPEG_VERSION"; printf '%s\n' "${CONFIGURE_ARGS[@]}")"
if [ -f "$PREFIX/RECIPE" ] && [ "$(cat "$PREFIX/RECIPE")" = "$RECIPE" ] && [ -f "$PREFIX/lib/libavcodec.dylib" ]; then
  echo "ffmpeg: cached at $PREFIX"
  exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
echo "ffmpeg: downloading $FFMPEG_URL"
curl -fsSL --retry 3 -o "$WORK/ffmpeg.tar.xz" "$FFMPEG_URL"
echo "$FFMPEG_SHA256  $WORK/ffmpeg.tar.xz" | shasum -a 256 -c -
tar -xJf "$WORK/ffmpeg.tar.xz" -C "$WORK"
cd "$WORK/ffmpeg-$FFMPEG_VERSION"
rm -rf "$PREFIX"
echo "ffmpeg: configure"
./configure --prefix="$PREFIX" "${CONFIGURE_ARGS[@]}" > "$WORK/configure.log" 2>&1 || { tail -n 40 "$WORK/configure.log"; exit 1; }
echo "ffmpeg: build"
make -j"$(sysctl -n hw.ncpu)" > "$WORK/build.log" 2>&1 || { tail -n 40 "$WORK/build.log"; exit 1; }
make install > "$WORK/install.log" 2>&1
printf '%s\n' "$RECIPE" > "$PREFIX/RECIPE"
cp COPYING.LGPLv2.1 "$PREFIX/COPYING.LGPLv2.1"
echo "ffmpeg: installed into $PREFIX"
ls "$PREFIX/lib"/*.dylib
