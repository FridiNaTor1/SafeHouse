# Building SafeHouse

[Back to the README](../README.md)

Most players should use the packages on the [Releases page](https://github.com/FridiNaTor1/SafeHouse/releases), which
are built by this repository's GitHub Actions workflows (`.github/workflows/`). This page is for building it yourself.

You need CMake 3.22 or newer, a C++20 compiler and Ninja (or Visual Studio 2022 for the launcher and Sly 1 on
Windows). GLFW 3.4, FAudio 25.02 and the SDL 2.30.11 fallback are downloaded at configure time, pinned by SHA-256, so
the first configure needs internet access. Dear ImGui, GLAD, GLM, stb, nlohmann/json, miniz, the Windows FFmpeg build
and the launcher's typeface are in the repository.

## Linux

Ubuntu 22.04 or newer (install `file` and `desktop-file-utils` as well to make AppImages):

```sh
sudo apt install build-essential cmake ninja-build libgl1-mesa-dev libxrandr-dev libxinerama-dev libxcursor-dev \
  libxi-dev libwayland-dev libxkbcommon-dev wayland-protocols libsdl2-dev libavformat-dev libavcodec-dev \
  libavutil-dev libswscale-dev
cmake --preset linux-release
cmake --build --preset linux-release --parallel 4
ctest --test-dir build/linux-release --output-on-failure
```

The programs are in `build/linux-release/bin`: `safehouse` (the launcher), `Sly1-Release`, `Sly2-Release` and the disc
tools. If your Wayland development packages are too old, configure with `-DGLFW_BUILD_WAYLAND=OFF` (X11 / XWayland).

## Windows

The release packages are cross-compiled from Linux with [LLVM/MinGW](https://github.com/mstorsjo/llvm-mingw) (Sly 2
and Sly 3 do not build with MSVC). Point `CANE_MINGW_ROOT` at an LLVM/MinGW toolchain and use the `windows-cross`
preset:

```sh
export CANE_MINGW_ROOT=/path/to/llvm-mingw
cmake --preset windows-cross -DCANE_BUILD_SLY2=ON
cmake --build --preset windows-cross --parallel 4
```

The cross build downloads Microsoft's pinned XAudio2 redistributable. The Visual Studio 2022 preset
(`windows-release`) builds the launcher and Sly 1 natively.

## macOS

`.github/workflows/macos.yml` is the reference: Homebrew's `ninja` and `nasm`, FFmpeg built by `scripts/macos/ffmpeg.sh`,
then

```sh
scripts/macos/ffmpeg.sh "$PWD/build/ffmpeg"
cmake --preset macos-release -DCANE_FFMPEG_ROOT="$PWD/build/ffmpeg"
cmake --build --preset macos-release --parallel 4
```

On macOS the games' OpenGL runs on Metal (`platform/metal`). The macOS build has the launcher and Sly 2.

## Options

| Option | Default | What it does |
| --- | --- | --- |
| `CANE_BUILD_GAME` | ON | Sly 1 (`Sly1-Release`) |
| `CANE_BUILD_SLY2` | ON | Sly 2 (`Sly2-Release`) |
| `CANE_BUILD_SLY3` | OFF | Sly 3 (`Sly3-Release`, early in development) |
| `CANE_BUILD_LAUNCHER` | ON | The launcher |
| `CANE_LTO` | auto | Link-time optimization (on when supported and the machine has at least 6 GB of RAM) |
| `CANE_X86_64_V2` | OFF | Build for x86-64-v2 CPUs (SSE4.2, POPCNT; GCC / Clang) |
| `CANE_FFMPEG_ROOT` | | Another FFmpeg build. The release packages use `scripts/ffmpeg-minimal.sh linux PREFIX` (or `windows`) (the games' codecs only, much smaller than a system FFmpeg); macOS uses `scripts/macos/ffmpeg.sh` |
| `CANE_LAUNCHER_SOURCE_BUILD` | OFF | Development builds: the launcher's **Settings > Advanced > Custom runtime** builds and uses the games from a source checkout |

CMake's `FETCHCONTENT_SOURCE_DIR_GLFW`, `FETCHCONTENT_SOURCE_DIR_FAUDIO` and `FETCHCONTENT_SOURCE_DIR_SDL2` use local
copies of those dependencies instead of downloading them.

## Packages

```sh
python3 scripts/package.py --build build/linux-release                                   # tar.gz
python3 scripts/package.py --build build/linux-release --appimagetool /path/to/appimagetool  # + AppImage
python3 scripts/package.py --build build/windows-cross --windows                         # Setup.exe + Portable.zip
python3 scripts/macos/package.py --build build/macos-release --ffmpeg build/ffmpeg       # SafeHouse.app
```

The packages land in `dist/`. The Windows setup needs NSIS 3 (`apt install nsis`; `--makensis PATH` for another
location; `--windows-package portable` makes only the zip). The setup accepts `/S` (silent) and `/DESKTOP` (desktop
shortcut). Linux packages carry their shared libraries but use the system's C / C++ runtime and graphics drivers, so
build them on the oldest distribution you want to support (the releases use Ubuntu 22.04).

## Command line

The launcher: `--install ISO` (the disc decides the game), `--verify [sly1|sly2|sly3]`, `--play [sly1|sly2|sly3]`,
`--add-to-steam GAME` and `--steam-status`.

The disc installer:

```sh
cane-extractor --iso game.iso --output /path/to/data --non-interactive --progress=jsonl
cane-extractor --iso game.iso --validate-only
cane-extractor --iso game.iso --identify    # which game a disc image is, as JSON
```

The games take `--data-dir PATH` and `--user-dir PATH`; the launcher passes them.

## Tests

`ctest` checks the disc installer's input rejection, the launcher's profile locations, disc identification, the install
and launch contracts, the Steam and desktop shortcuts, the PS3 HD import's disc reader, Sly 2's rumble timing and the
launcher's copy of the settings format. `scripts/test-launcher-games.py` and `scripts/test-extractor.py` can also check
real disc images.
