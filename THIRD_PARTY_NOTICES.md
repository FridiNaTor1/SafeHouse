# Third-party dependencies

The source/build uses GLFW 3.4 (zlib/libpng), FAudio 25.02 (zlib), SDL2 (zlib), Dear ImGui and ImGuiFileDialog (MIT), GLAD (MIT/public domain components), GLM (MIT), stb (MIT/public domain), nlohmann/json (MIT), PicoSHA2 1.0.1 (MIT), miniz 3.1.2 (MIT; `Libraries/miniz`, the PS3 HD import reads zipped disc images with it, licence in `licenses/miniz.txt`), FFmpeg, and Atkinson Hyperlegible (SIL Open Font License).

CHD disc images are read with libchdr (BSD-3-Clause; `Libraries/libchdr`, the sources of https://github.com/rtissera/libchdr at 47d7941 with its LZMA SDK (public domain), zstd (BSD), miniz (MIT) and dr_flac (public domain or MIT-0) decoders); its licence is shipped in `licenses/libchdr/`.

The vendored headers retain their copyright/license notices. Downloaded GLFW and FAudio releases retain their license files. The font's OFL is in `launcher/assets/OFL.txt`; PicoSHA2's license is in `platform/picosha2.h`. Windows cross builds use Microsoft.XAudio2.Redist 1.2.13 under its included Microsoft license.

The release packages for Windows and Linux ship a small FFmpeg (LGPL 2.1, shared libraries) built from the unmodified FFmpeg 7.1.1 release by `scripts/ffmpeg-minimal.sh`, with only the MPEG-1/2 video and FLAC codecs; its licence and exact configuration are in `licenses/ffmpeg/` (`COPYING.LGPLv2.1`, `BUILD-CONFIGURATION.txt`), and the source is https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz. Local Windows builds without `CANE_FFMPEG_ROOT` use the LGPL shared build in `Libraries/ffmpeg`. Other Linux builds use the selected system FFmpeg and libraries; their configuration and corresponding license obligations follow that build. The packaging script collects dependent shared libraries from the build environment. Distributor builds must retain applicable notices and provide corresponding sources where their dependency licenses require them.

AppImages embed the MIT-licensed type2-runtime release 20251108, pinned by SHA-256. Its source is https://github.com/AppImage/type2-runtime/tree/dd6cebedcbddde9c82f89b011e8e1d40b6e43868 and its license is shipped in `licenses/appimage-runtime.txt`.

No original Sly Cooper game data is included in release packages.

Sly 2 (`Sly2-Release/`, the native port; called Sly2-Native before 2026-10-28) contains no PS2 emulation code. Its sound engine (`Sly2-Release/snd989.cpp`) follows the 989snd rules of OpenGOAL's reimplementation of 989snd (ISC). The emulated Sly 2 reference used during development, with its vendored rac1-native-game (MIT) and OpenGOAL 989snd (ISC) code, is no longer part of this repository or of the packages.

Sly Cooper and the Thievius Raccoonus port: SafeHouse grew out of ProjectCane by theclub654 (https://github.com/theclub654/ProjectCane), the native PC port of Sly 1 that made this project possible. Its history is kept in this repository.

SafeHouse logo and app icon: the SafeHouse logo (the mask and cane emblem, `launcher/assets/safehouse-logo.png`) and the application icon (the cane, `launcher/assets/safehouse-icon.png`, `safehouse.ico`) are artwork by HoshiYumeHime (DeviantArt).

Game logos: the launcher's game list shows the logos of Sly Cooper and the Thievius Raccoonus, Sly 2: Band of Thieves, Sly 3: Honor Among Thieves, Sly Cooper: Thieves in Time and Bentley's Hackpack (`launcher/assets/games/`, converted from images supplied by the project owner); the desktop shortcuts the launcher makes use the same logos as their icons (`launcher/assets/shortcut/`). These logos and names are trademarks of Sony Interactive Entertainment; the games were developed by Sucker Punch Productions (Sly 1-3) and Sanzaru Games (Thieves in Time, Bentley's Hackpack). They are used only to identify the games. SafeHouse (formerly ProjectCane) is not affiliated with or endorsed by these companies.

Controller prompt icons: Kenney Input Prompts 1.5A (the newest release, created 2026-07-11; archive sha256 `ac2fcf599080b0f3ba2d174c9474db6df1a0e96ff0662580e2da79a122ab78a1`), CC0 1.0 (www.kenney.nl), packed by `tools/pack_prompts.py` into `assets/prompts/`, including its 2026 Steam Controller set. The Steam Deck and 2026 Steam Controller pictures on Sly 2's Controls page are drawn by the packer after Kenney's silhouettes (CC0) with their thumbsticks and trackpads added. The licence file is shipped in `licenses/kenney-input-prompts/`. PlayStation, Xbox, Nintendo Switch and Steam names and symbols are trademarks of their owners; use does not imply endorsement.

Controller mappings: SDL_GameControllerDB (zlib), `Libraries/gamecontrollerdb/`. Its licence is shipped in `licenses/sdl-gamecontrollerdb/`.

The launcher typeface is Atkinson Hyperlegible Regular and Bold by the Braille Institute (SIL Open Font License, `launcher/assets/OFL.txt`).

Windows offline setup is built with NSIS. Its notices are included in `licenses/nsis/copyright`.

Steam library artwork: `launcher/assets/steam/` (installed as `share/safehouse/steam/`) holds images from SteamGridDB (https://www.steamgriddb.com), made by its community artists; each file's author and source page are listed in `launcher/assets/steam/CREDITS.md`. They are copied into Steam's grid folder only when the user chooses "Add to Steam". The pictured characters and logos are trademarks of Sony Interactive Entertainment.
