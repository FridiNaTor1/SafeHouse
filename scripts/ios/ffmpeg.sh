#!/usr/bin/env bash
# Builds the FFmpeg libraries Sly 2 links on iOS (libavcodec, libavutil, libswscale) from the pinned release, with
# the same codecs as the macOS packages (scripts/macos/ffmpeg.sh: MPEG-2 video for the PSS movies, FLAC), LGPL, no
# external libraries; static, so they live inside Sly 2's framework (an iOS app ships no loose dylibs).
#
#   scripts/ios/ffmpeg.sh <prefix> [iphoneos|iphonesimulator]    (default iphoneos; arm64; cached on RECIPE)
set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.1}"
FFMPEG_SHA256="${FFMPEG_SHA256:-733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1}"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
MIN_IOS="${IOS_DEPLOYMENT_TARGET:-15.0}"

PREFIX="${1:?usage: ffmpeg.sh <prefix> [iphoneos|iphonesimulator]}"
SDK="${2:-iphoneos}"
case "$SDK" in
  iphoneos) MIN_FLAG="-miphoneos-version-min=$MIN_IOS" ;;
  iphonesimulator) MIN_FLAG="-mios-simulator-version-min=$MIN_IOS" ;;
  *) echo "unknown SDK $SDK"; exit 2 ;;
esac
SYSROOT="$(xcrun --sdk "$SDK" --show-sdk-path)"
CC="$(xcrun --sdk "$SDK" --find clang)"

CONFIGURE_ARGS=(
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
  --disable-shared --enable-static --enable-pic
  --disable-gpl --disable-nonfree --disable-version3 --disable-debug
  --disable-avdevice --disable-avformat --disable-avfilter --disable-postproc --disable-swresample
  --enable-avcodec --enable-avutil --enable-swscale
  --enable-decoder=mpeg2video --enable-decoder=mpeg1video --enable-parser=mpegvideo
  --enable-decoder=flac --enable-encoder=flac --enable-parser=flac
  --enable-cross-compile --target-os=darwin --arch=aarch64
)
RECIPE="$(printf 'ffmpeg-%s %s %s\n' "$FFMPEG_VERSION" "$SDK" "$MIN_IOS"; printf '%s\n' "${CONFIGURE_ARGS[@]}")"
if [ -f "$PREFIX/RECIPE" ] && [ "$(cat "$PREFIX/RECIPE")" = "$RECIPE" ] && [ -f "$PREFIX/lib/libavcodec.a" ]; then
  echo "ffmpeg ($SDK): cached at $PREFIX"
  exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
echo "ffmpeg ($SDK): downloading $FFMPEG_URL"
curl -fsSL --retry 3 -o "$WORK/ffmpeg.tar.xz" "$FFMPEG_URL"
echo "$FFMPEG_SHA256  $WORK/ffmpeg.tar.xz" | shasum -a 256 -c -
tar -xJf "$WORK/ffmpeg.tar.xz" -C "$WORK"
cd "$WORK/ffmpeg-$FFMPEG_VERSION"
rm -rf "$PREFIX"
echo "ffmpeg ($SDK): configure ($SYSROOT)"
FLAGS="-arch arm64 -isysroot $SYSROOT $MIN_FLAG"
./configure --prefix="$PREFIX" "${CONFIGURE_ARGS[@]}" --cc="$CC" --sysroot="$SYSROOT" \
  --extra-cflags="$FLAGS" --extra-ldflags="$FLAGS" > "$WORK/configure.log" 2>&1 || { tail -n 60 "$WORK/configure.log"; tail -n 60 ffbuild/config.log || true; exit 1; }
echo "ffmpeg ($SDK): build"
make -j"$(sysctl -n hw.ncpu)" > "$WORK/build.log" 2>&1 || { tail -n 60 "$WORK/build.log"; exit 1; }
make install > "$WORK/install.log" 2>&1
printf '%s\n' "$RECIPE" > "$PREFIX/RECIPE"
cp COPYING.LGPLv2.1 "$PREFIX/COPYING.LGPLv2.1"
echo "ffmpeg ($SDK): installed into $PREFIX"
ls -l "$PREFIX/lib"/*.a
