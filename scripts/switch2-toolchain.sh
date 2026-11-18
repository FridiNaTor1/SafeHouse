#!/usr/bin/env bash
# Installs ouncebrew's Switch 2 toolchain into a devkitPro installation, for SafeHouse-NX2.nro (cmake/Switch.cmake,
# the switch2-release preset): the ounce-devkit release (libounce, the Switch 2 portlibs), drako3d (built with
# devkitA64 and drakotools' drakodef / drakomme) and the uam2 shader compiler (built with Mesa's NAK backend).
# Everything is pinned below: the devkit by version and SHA-256, the sources by commit.
#
#   scripts/switch2-toolchain.sh <cache>
#
# DEVKITPRO (default /opt/devkitpro) must hold devkitA64, libnx's switch-dev tools and the Switch portlibs (devkitPro's
# devkitpro/devkita64 image). The built files are kept in <cache> (libounce/, portlibs/switch2/, tools/bin/uam2): when
# <cache>/RECIPE matches, nothing is rebuilt and they are only copied into DEVKITPRO.
#
# Building needs a C/C++ compiler, git, curl, flex, bison, ninja, Meson >= 1.7, Python 3 with Mako, PyYAML and
# packaging, Rust >= 1.85 with bindgen (0.71.1 or newer, not 0.72.0 / 0.73.0 / 0.73.1) and cbindgen >= 0.28, libclang,
# and the zlib, zstd, expat and libdrm headers. Mesa's source (uam2's subprojects/mesa.wrap, a release tag) and the Rust
# crates it uses are downloaded by Meson.
set -euo pipefail

OUNCE_DEVKIT_VERSION=20261115b
OUNCE_DEVKIT_SHA256=e6866198bfb44144c8227288c6315772a6a505cacca54b08cd0e6232a84f4729
DRAKOTOOLS_COMMIT=b32fe6e43533e80414f6aa4ad3b899ed077f806c
DRAKO3D_COMMIT=d1f05077667d0eabdd39d8a65483ffcec51807c2
UAM2_COMMIT=7db77e720184d7de389f4fd5c0dd761f7b8b8f90

CACHE="${1:?usage: switch2-toolchain.sh <cache>}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPRO
RECIPE="ounce-devkit $OUNCE_DEVKIT_VERSION $OUNCE_DEVKIT_SHA256
drakotools $DRAKOTOOLS_COMMIT
drako3d $DRAKO3D_COMMIT
uam2 $UAM2_COMMIT"

[ -x "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc" ] && [ -f "$DEVKITPRO/libnx/switch_rules" ] ||
  { echo "switch2-toolchain: no devkitA64 / libnx in $DEVKITPRO" >&2; exit 1; }

install_cache() {
  rm -rf "$DEVKITPRO/libounce" "$DEVKITPRO/portlibs/switch2"
  mkdir -p "$DEVKITPRO/portlibs" "$DEVKITPRO/tools/bin"
  cp -a "$CACHE/libounce" "$DEVKITPRO/libounce"
  cp -a "$CACHE/portlibs/switch2" "$DEVKITPRO/portlibs/switch2"
  cp -a "$CACHE/tools/bin/uam2" "$DEVKITPRO/tools/bin/uam2"
  cp -a "$CACHE/switch2vars.sh" "$CACHE/ounce-devkit-version.txt" "$DEVKITPRO/"
  echo "switch2-toolchain: installed into $DEVKITPRO"
  sed 's/^/  /' "$CACHE/RECIPE"
}

if [ -f "$CACHE/RECIPE" ] && [ "$(cat "$CACHE/RECIPE")" = "$RECIPE" ] && [ -f "$CACHE/libounce/lib/libdrako3d.a" ] &&
   [ -x "$CACHE/tools/bin/uam2" ]; then
  install_cache
  exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
jobs="${SWITCH2_TOOLCHAIN_JOBS:-$(nproc)}"

# the sources at their pinned commits
fetch() {
  git init -q "$work/$1"
  git -C "$work/$1" fetch -q --depth 1 "https://github.com/ouncebrew/$1.git" "$2"
  git -C "$work/$1" checkout -q FETCH_HEAD
}
fetch drakotools "$DRAKOTOOLS_COMMIT"
fetch drako3d "$DRAKO3D_COMMIT"
fetch uam2 "$UAM2_COMMIT"

# the devkit: libounce and the Switch 2 portlibs (its install.sh does the same: an unpack into DEVKITPRO)
curl -fsSL -o "$work/ounce-devkit.tar.xz" \
  "https://github.com/ouncebrew/ounce-devkit/releases/download/devkit-$OUNCE_DEVKIT_VERSION/ounce-devkit-$OUNCE_DEVKIT_VERSION.tar.xz"
echo "$OUNCE_DEVKIT_SHA256  $work/ounce-devkit.tar.xz" | sha256sum -c -
rm -rf "$DEVKITPRO/libounce" "$DEVKITPRO/portlibs/switch2"
tar -xJf "$work/ounce-devkit.tar.xz" -C "$DEVKITPRO"

# drakotools (host): drakodef and drakomme, which drako3d's build runs
meson setup "$work/drakotools-build" "$work/drakotools" --prefix "$work/host" > "$work/drakotools-setup.log" ||
  { cat "$work/drakotools-setup.log"; exit 1; }
ninja -C "$work/drakotools-build" -j "$jobs"
meson install -C "$work/drakotools-build" > /dev/null

# drako3d for the Switch 2 (GA10F), into DEVKITPRO/libounce
PATH="$work/host/bin:$PATH" make -C "$work/drako3d" -j "$jobs" GPU_ARCH=ampere
PATH="$work/host/bin:$PATH" make -C "$work/drako3d" GPU_ARCH=ampere install

# uam2 (host)
meson setup "$work/uam2-build" "$work/uam2" > "$work/uam2-setup.log" || { cat "$work/uam2-setup.log"; exit 1; }
ninja -C "$work/uam2-build" -j "$jobs"
"$work/uam2-build/source/uam2" --version
install -m 755 "$work/uam2-build/source/uam2" "$DEVKITPRO/tools/bin/uam2"

# the cache
rm -rf "$CACHE"
mkdir -p "$CACHE/portlibs" "$CACHE/tools/bin"
cp -a "$DEVKITPRO/libounce" "$CACHE/libounce"
cp -a "$DEVKITPRO/portlibs/switch2" "$CACHE/portlibs/switch2"
cp -a "$DEVKITPRO/tools/bin/uam2" "$CACHE/tools/bin/uam2"
cp -a "$DEVKITPRO/switch2vars.sh" "$DEVKITPRO/ounce-devkit-version.txt" "$CACHE/"
printf '%s\n' "$RECIPE" > "$CACHE/RECIPE"
install_cache
