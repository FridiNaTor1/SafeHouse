#!/usr/bin/env bash
# Builds the small FFmpeg the Linux and Windows packages ship, from the pinned release: shared, LGPL, no external
# libraries, and only what the games use:
#   - MPEG-1/2 video (the movies), with the raw MPEG video / program stream demuxers and the file protocol (Sly 1 opens
#     its .m2v cutscenes through libavformat);
#   - FLAC (European dialog and movie sound; the converter also writes it).
# A system or full FFmpeg pulls in dozens of codec libraries (x265, aom, vpx, codec2, ...), most of the packages' size.
# macOS builds its own copy with scripts/macos/ffmpeg.sh (no libavformat there: Sly 1 isn't built on macOS).
#
#   scripts/ffmpeg-minimal.sh linux   <prefix>   native build for Linux
#   scripts/ffmpeg-minimal.sh windows <prefix>   cross build with LLVM/MinGW (x86_64-w64-mingw32-clang on PATH, or
#                                                CANE_MINGW_ROOT set to the toolchain)
#   scripts/ffmpeg-minimal.sh --print-args       the configure arguments, one per line
# The result is cached: nothing is rebuilt when <prefix>/RECIPE matches.
set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.1}"
FFMPEG_SHA256="${FFMPEG_SHA256:-733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1}"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"

CONFIGURE_ARGS=(
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
  --enable-shared --disable-static --enable-pic
  --disable-gpl --disable-nonfree --disable-version3 --disable-debug
  --disable-avdevice --disable-avfilter --disable-postproc --disable-swresample
  --enable-avcodec --enable-avformat --enable-avutil --enable-swscale
  --enable-decoder=mpeg2video --enable-decoder=mpeg1video --enable-parser=mpegvideo
  --enable-demuxer=mpegvideo --enable-demuxer=mpegps --enable-protocol=file
  --enable-decoder=flac --enable-encoder=flac --enable-parser=flac --enable-demuxer=flac
)

if [ "${1:-}" = "--print-args" ]; then
  printf '%s\n' "${CONFIGURE_ARGS[@]}"
  exit 0
fi

TARGET="${1:?usage: ffmpeg-minimal.sh linux|windows <prefix> | --print-args}"
PREFIX="${2:?usage: ffmpeg-minimal.sh linux|windows <prefix>}"
PREFIX="$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)"
case "$TARGET" in
  linux) CHECK="$PREFIX/lib/libavcodec.so" ;;
  windows)
    if [ -n "${CANE_MINGW_ROOT:-}" ]; then export PATH="$CANE_MINGW_ROOT/bin:$PATH"; fi
    command -v x86_64-w64-mingw32-clang >/dev/null || { echo "ffmpeg: x86_64-w64-mingw32-clang not found (set CANE_MINGW_ROOT)" >&2; exit 1; }
    CONFIGURE_ARGS+=(--enable-cross-compile --target-os=mingw32 --arch=x86_64 --cross-prefix=x86_64-w64-mingw32-
      --cc=x86_64-w64-mingw32-clang --cxx=x86_64-w64-mingw32-clang++ --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm
      --strip=llvm-strip)
    CHECK="$PREFIX/bin/avcodec-61.dll" ;;
  *) echo "ffmpeg: unknown target $TARGET (linux or windows)" >&2; exit 2 ;;
esac
command -v nasm >/dev/null 2>&1 || CONFIGURE_ARGS+=(--disable-x86asm)

RECIPE="$(printf 'ffmpeg-%s %s\n' "$FFMPEG_VERSION" "$TARGET"; printf '%s\n' "${CONFIGURE_ARGS[@]}")"
if [ -f "$PREFIX/RECIPE" ] && [ "$(cat "$PREFIX/RECIPE")" = "$RECIPE" ] && [ -f "$CHECK" ]; then
  echo "ffmpeg: cached at $PREFIX"
  exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
echo "ffmpeg: downloading $FFMPEG_URL"
curl -fsSL --retry 3 -o "$WORK/ffmpeg.tar.xz" "$FFMPEG_URL"
echo "$FFMPEG_SHA256  $WORK/ffmpeg.tar.xz" | sha256sum -c -
tar -xJf "$WORK/ffmpeg.tar.xz" -C "$WORK"
cd "$WORK/ffmpeg-$FFMPEG_VERSION"
find "$PREFIX" -mindepth 1 -delete
echo "ffmpeg: configure ($TARGET)"
./configure --prefix="$PREFIX" "${CONFIGURE_ARGS[@]}" > "$WORK/configure.log" 2>&1 || { tail -n 40 "$WORK/configure.log"; exit 1; }
echo "ffmpeg: build"
make -j"$(nproc)" > "$WORK/build.log" 2>&1 || { tail -n 40 "$WORK/build.log"; exit 1; }
make install > "$WORK/install.log" 2>&1
# The import libraries go beside the .def files in lib/, as in the prebuilt Windows FFmpeg the build used before
# (CMakeLists.txt links Sly 1 from <root>/lib; cmake/ffmpeg.cmake looks there too).
if [ "$TARGET" = windows ]; then mv "$PREFIX"/bin/*.lib "$PREFIX/lib/"; fi
[ -f "$CHECK" ] || { echo "ffmpeg: $CHECK missing after install" >&2; ls -R "$PREFIX" | head -40; exit 1; }
printf '%s\n' "$RECIPE" > "$PREFIX/RECIPE"
cp COPYING.LGPLv2.1 "$PREFIX/COPYING.LGPLv2.1"
echo "ffmpeg: installed into $PREFIX"
