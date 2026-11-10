# SafeHouse for Android

The Android app (application id `org.safehouse.android`) packages the SafeHouse launcher, the extractor and Sly 2
as native libraries on SDL 2.30.11 and OpenGL ES 3.2. It is built from the same sources as the desktop programs
(`cmake/android.cmake`); its Java side and the GLFW-on-SDL layer come from ThiefRush's Android port, without the chat
key (SafeHouse is single-player). Sly 1 and Sly 3 are not packaged: the launcher shows them, but only Sly 2 plays.

No game data is in the APK. The player picks their own Sly 2 disc image with Android's document picker (Install >
Browse); the app copies it into its own storage (`files/imports`) and installs from there, as the desktop launcher
does.

## Requirements

- Android 8.0 (API 26) or newer, 64-bit (arm64-v8a; x86_64 for the emulator).
- OpenGL ES 3.2 with vertex-stage storage buffers (Adreno 6xx/7xx: Quest 3/3S, AYN Thor/Odin 2, AYANEO Pocket
  series, Retroid Pocket 5/Mini; recent Mali/Immortalis). The launcher alone also runs on ES 3.1.
- About 3 GB for the copied disc image and 5-8 GB for the installed game.

## Files

Everything lives in the app's private storage (`/data/user/0/org.safehouse.android/files`), which the desktop builds'
folder rules map onto (HOME and the XDG folders point into it):

| Folder | Contents |
| --- | --- |
| `config/SafeHouse/` | launcher.json, Settings.ini, saves, logs, mods |
| `share/SafeHouse/installations/sly2/` | the installed (converted) game |
| `imports/` | disc images copied in through the document picker |

The **SafeHouse** entry in the Files app's sidebar (and other document pickers) shows these folders
(`SafeHouseDocumentsProvider`). Updating the app with the same signing key keeps them; uninstalling removes them.

## Input

- **Touch:** the on-screen pad (ThiefRush's `TouchControls`: stick, camera drag, face buttons, shoulders, D-pad,
  Start/Select; layout editor while paused). It shows while touch is the input and disappears as soon as a
  controller, keyboard or mouse is used (`platform/android/glfw_sdl.cpp useTouchControls`); touching the screen
  brings it back.
- **Controllers:** SDL's Android controller support, with SDL_GameControllerDB and the launcher's/game's normal
  prompt-family detection. Handhelds with built-in controls (AYN Thor/Odin, AYANEO, Retroid) report an Xbox-style
  layout through Android's input system.
- **Mouse and keyboard:** as on the desktop (DeX, Chromebooks, a Bluetooth keyboard).

## Shortcuts

There is no Steam on Android. A game's page has **Add shortcut**: Android asks to pin a home-screen shortcut with the
game's logo that starts the game directly (the app runs the launcher's `--play GAME` without its window and closes
when the game quits). Settings > Collection Style Launcher can pin one for that menu too. Home screens that do not
take pinned shortcuts (some VR / TV launchers) get a message instead.

By default the app's copy of a picked disc image is deleted once the game is installed (Settings > Games
"Keep imported disc images" keeps it; a re-extract after an update then asks for the disc again).

## Desktop mode (Samsung DeX and similar)

The activities are resizable and handle window, density and display changes themselves; in a desktop or freeform
window the landscape lock is dropped (`DesktopMode.java`), and Samsung's `keep_process_alive` hint keeps the game
when the app moves between the phone and the external display.

## Renderer

Sly 2's renderer is desktop OpenGL 4.3+ (GLSL 4.30, storage buffers, a geometry stage, multi-draw-indirect, clip
control). On Android (`platform/android/gles_sly2.cpp`, `Sly2-Release/gl.cpp`, `render.cpp`):

- shaders are translated at load: `#version 320 es`, precision qualifiers, `GL_NV_shader_noperspective_interpolation`
  where present (else perspective-correct colours);
- `glClipControl` is `GL_EXT_clip_control`, `glClearDepth` is `glClearDepthf`;
- without `EXT_base_instance` / `EXT_multi_draw_indirect` (Adreno has neither) each draw is issued on its own with
  the draw index as a uniform (`g_fDrawUniform`, glob.vert `CANE_DRAW_UNIFORM`);
- bindless textures and the persistent ring stay off (they are optional on the desktop too).

## Building

Linux or macOS (WSL on Windows). Needs the Android SDK (platform 36, build-tools), NDK 29.0.14206865, CMake, Ninja,
Make, Perl, Python 3.10+, Gradle 9.6.1 and a JDK 17+.

```sh
python3 scripts/android/build-package.py \
  --sdk ~/Android/Sdk --gradle ~/Documents/tools/gradle-9.6.1/bin/gradle --java-home ~/Documents/tools/jdk-26.0.2
```

It builds the locked dependencies once (`scripts/android/bootstrap-dependencies.py`: SDL2, FAudio, FFmpeg from
`dependencies.json`, checksums verified; `--cache DIR` reuses already-downloaded archives), then the native libraries
(`build/android-arm64/`), stages them under `build/android-package/<abi>/` and runs Gradle's `assembleDebug`. The APK
lands in `dist/android/SafeHouse-Android-arm64-v8a-debug.apk`. Everything runs at low priority (`nice`, idle I/O,
`--parallel 4` by default).

Install and start on a device:

```sh
adb install -r dist/android/SafeHouse-Android-arm64-v8a-debug.apk
adb shell am start -n org.safehouse.android/.LauncherActivity
adb logcat -s SafeHouse SDL SDL/APP      # the launcher's and the game's output
```

Debug builds can capture their own picture (useful on a Quest, whose screencap shows the room, not the panel):
`adb shell run-as org.safehouse.android touch files/capture-request`, then
`adb exec-out run-as org.safehouse.android cat files/captures/<Activity>-<n>.png > shot.png`.

The APK is signed with Android's debug key; a store release needs its own signing configuration.

## Processes

Like ThiefRush, the launcher never fork/execs on Android. `platform/process.h runProcess` asks
`NativeBridge.startChild` to start the program in a private process of the app, with its stdout/stderr returned
through a pipe as on the desktop:

| Program | Android component | Process |
| --- | --- | --- |
| launcher (`libsafehouse.so`) | `LauncherActivity` | main |
| launcher without its window (`--play GAME` from a shortcut, `--collection`) | `DirectActivity` | `:direct` |
| Sly 2 (`libSly2-Release.so`) | `GameActivity` | `:game` |
| extractor (`libcane-extractor.so`) | `ExtractorService` | `:extract` |
| Sly 2's tools (`--convert`, `--export-textures`, `--ps3-hd-textures`) | `RuntimeToolService` | `:tool` |
