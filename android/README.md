# SafeHouse for Android

The Android app (application id `org.safehouse.android`) packages the SafeHouse launcher, the extractor, Sly 1, Sly 2
and Sly 3 as native libraries on SDL 2.30.11 and OpenGL ES 3.2 (Sly 2 also on Vulkan, Sly 1 on Vulkan only). It is built from the same sources as the desktop programs
(`cmake/android.cmake`); its Java side and the GLFW-on-SDL layer come from ThiefRush's Android port, without the chat
key (SafeHouse is single-player).

No game data is in the APK. The player picks their own Sly 1 / Sly 2 / Sly 3 disc image with Android's document picker (Install >
Browse); the app copies it into its own storage (`files/imports`) and installs from there, as the desktop launcher
does.

## Requirements

- Android 5.0 (API 21) or newer, 64-bit (arm64-v8a; x86_64 for the emulator). The launcher and the installs run on
  every one of them; the games need Vulkan 1.3 (Android 7+ with a recent driver) or OpenGL ES 3.2 with vertex-stage
  storage buffers (Adreno 6xx/7xx: Quest 3/3S, AYN Thor/Odin 2, AYANEO Pocket series, Retroid Pocket 5/Mini; recent
  Mali/Immortalis). A device with less says so when the game starts ("This device cannot run the game: ... OpenGL ES
  3.0 ..."). The launcher alone also runs on ES 3.0.
- About 3 GB for the copied disc image and 5-8 GB for the installed game.

### Older Android (API 21-25, since 2026-11-24)

- No Vulkan before Android 7 (no system loader): the games use OpenGL ES, which old GPUs rarely have at 3.2.
- Java APIs of newer versions are guarded (`Build.VERSION.SDK_INT`; `gradle lintDebug` reports no NewApi issue left in
  SafeHouse's own classes, SDL's API-level classes are only created on their versions); no `java.nio.file`.
- Audio: SDL picks AAudio on Android 8.1+ and OpenSL ES before.
- Storage: the storage permission on Android 6-10 (asked only for "Allow folders anywhere"), granted at install on 5.
- Pinned shortcuts need Android 8 (older home screens get "does not take shortcuts"); the touch pad's font is the
  system's bold face before Android 8; rumble has no strength before Android 8.
- Updates (below) use a PackageInstaller session, Android 5+; "install unknown apps" per app is Android 8+.
- Native code: built for API 21 (`ANDROID_PLATFORM=android-21`, the dependencies too), 16 KB pages.
- Tested on the emulator (Android 6, API 23): the app installs, the launcher runs, Sly 2 installs from its disc image;
  the game stops with the OpenGL ES 3.0 message there (the emulator has no Vulkan before API 24 and ES 3.0 only).

## Updates

Settings > Launcher > Check for updates (and the start-up check) finds the latest release as on the desktop
(`launcher/update.h`). Update now downloads the release's APK (the manifest's `android-apk` package: arm64-v8a) into
`files/cache/safehouse-update/`, checks its SHA-256 against `SafeHouse-update.json`, and hands it to Android's installer
(a PackageInstaller session, `NativeBridge.installPackage`): the first time Android asks to allow SafeHouse to install
apps (its settings page opens), then to confirm the update. The release key signs every APK, so Android installs it
over this one and keeps the games, saves and settings. Debuggable builds also take `http://` addresses
(`SAFEHOUSE_UPDATE_URL` in `files/env.txt`, for tests against a local server).

## Vulkan driver (Adreno, arm64)

Settings > Launcher > Vulkan driver: System driver, or a driver package the player imports (a .zip with `meta.json`
and the driver library, as emulators use them: Mesa Turnip). Every game's Vulkan loader (`platform/vulkan/vk_loader.cpp`)
loads it through libadrenotools (BSD-2-Clause, `Libraries/adrenotools`, credited in THIRD_PARTY_NOTICES.md); the
package lives in `files/drivers/<id>/` (`platform/android/vulkan_driver.h`, `launcher/android_drivers.h`). Fail-safe:
a package that does not load leaves the system driver in use; a game that crashes while starting Vulkan on it (Android's
record of the process) sends the choice back to the system driver, and the launcher says why; VR always uses the
system driver. Shown on Adreno GPUs only. Not tested on a real Adreno yet (the emulator is x86_64: no adrenotools).

## Storage locations

Settings > Games: "Install games to" offers the app's own folders on the phone storage and SD cards
(`Context.getExternalFilesDirs`: no permission, removed with the app) besides the private folder; "Allow folders
anywhere…" asks for All files access (Android 11+; the storage permission on 6-10), after which the storage volumes'
roots, any folder ("Choose a folder…") and a disc images folder work as on the desktop. The games read their
installation with plain file access, so a Storage Access Framework folder cannot hold one; disc images picked with
the document picker are still copied in. Existing installations stay where they are; "Move … to the install folder"
moves one (the launcher's move: a rename on the same volume, else a copy that is checked before the original is
removed; tested on the emulator from the private folder to the app's folder on the storage, and the game played from there). Saves and settings stay in the
private folder.

## Why a game stopped (2026-11-24)

The launcher names the real reason when a game ends without its exit code: the game activity's own reason (its
window closed, the launcher's process ended) or Android's record of the process (ApplicationExitInfo, Android 11+:
"native crash, signal 11", "low memory", "killed (signal 9)"), not the game's last log line. Vulkan's start marker
(`platform/renderer_choice.h`) counts only a crash or a hang during Vulkan's start: a game that Android or the player
closed while Vulkan was starting is no failure. After a failed start Auto uses OpenGL ES for 3 starts (then 6, 12) and
tries Vulkan again by itself, and at once after an update. The OpenXR loader is not touched on a device without an
OpenXR runtime (no more five loader errors in a phone's log).

## Quest 2

The manifest lists the Quest 2 (`com.oculus.supportedDevices`), nothing requires more than it has (Vulkan is optional;
VR falls back to OpenGL ES, which the Adreno 650 has). Its runtime names it "Oculus Quest2", which turns on a lower
profile (`Sly2-Release/vr.cpp FLowEndHeadset`): 72 Hz, the eyes at 80 % of the recommended size (dynamic eye resolution
goes down from there), MSAA off. `SLY2N_VR_PROFILE=quest2` forces it on another headset for tests. Not tested on a
Quest 2.

## Files

Everything lives in the app's private storage (`/data/user/0/org.safehouse.android/files`), which the desktop builds'
folder rules map onto (HOME and the XDG folders point into it):

| Folder | Contents |
| --- | --- |
| `config/SafeHouse/` | launcher.json, Settings.ini, saves, logs, mods |
| `share/SafeHouse/installations/sly1/` | Sly 1: the installed game (PCBRX levels, music, movies) |
| `share/SafeHouse/installations/sly2/` | the installed (converted) game |
| `share/SafeHouse/installations/sly3/` | Sly 3: the disc image copy and its world parts |
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
when the game quits). Settings > Collection Style Launcher can pin one for that menu too (once the menu is imported
from your Sly Collection disc, on the same page). Home screens that do not
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

### Mali-G615 MC6 / r44p1

The RG 477M's ARM proprietary r44p1 driver uses the Vulkan attribute-based vertex path by default:
vertex pulling combined with the near-plane geometry stage can fault this GPU. Other tile-GPU
optimizations remain enabled, and `SLY2N_VKPULL=1` can explicitly retest the pulled path. The geometry
shader keeps near-plane clipping and affine color/fog interpolation, using one emission loop to avoid
a separate shader-compiler crash. This is a workaround for the tested GPU/driver, not a claim that all
Mali drivers need it. The OpenGL ES fallback still requires vertex-stage storage buffers.

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
| Sly 1 (`libSly1-Release.so`), Sly 3 (`libSly3-Release.so`) | `FlatGameActivity` (a flat window, also on the Quest) | `:game` |
| extractor (`libcane-extractor.so`) | `ExtractorService` | `:extract` |
| the games' tools (Sly 2's `--convert`, `--export-textures`, `--ps3-hd-textures`; Sly 3's `--convert-worlds`; Sly 1's `--ps3-hd-textures`) | `RuntimeToolService` | `:tool` |

## Sly 1

`libSly1-Release.so` is the game from the `Sly1/` submodule (the ProjectCane fork), built by `cmake/sly1.cmake`'s
`cane_sly1_game` in `cmake/android.cmake` (`-DCANE_BUILD_SLY1=OFF` leaves it out). Its install runs in the extractor's
own process (the level tools as a library, `cmake/sly1_pcbrx.cmake`; every region's disc). It renders on Vulkan only:
its OpenGL 4.3 renderer has no OpenGL ES path, so it runs on SafeHouse's OpenGL-on-Vulkan layer
(`platform/vulkan/gl_vulkan.cpp`, `platform/vulkan/vk_game.cpp`; a device without Vulkan gets an error message), with
its per-frame streams persistently mapped (`Options::persistentMaps`). Sound is upstream's XAudio2-on-FAudio over SDL,
movies FFmpeg. It is a flat window everywhere, also on the Quest (`FlatGameActivity`); the on-screen pad follows the
game (the fork's `safehouse/touch.cpp`: play, menus, cutscenes and movies). A new profile renders at 720 lines, as the
other games here.

## Sly 3

`libSly3-Release.so` is built from `Sly3-Release/` the same way as Sly 2's library (`cmake/android.cmake`,
`-DCANE_BUILD_SLY3=OFF` leaves it out; +6.7 MB APK, +14 MB installed). The launcher starts it like Sly 2 (the intent's
`program` names the library GameActivity / RuntimeToolService load). It renders on OpenGL ES 3.2; its Vulkan renderer
on Android comes with the Sly 3 Vulkan work. It has the same on-screen pad (layouts from its menus, movies and
scenes, hold the screen to skip a scene), vibration and controller prompts as Sly 2.

On a Meta Quest Sly 3 is a flat 2D panel (`FlatGameActivity`, declared without the VR category), not the immersive
part of the app, because its VR mode is not ready: it does not ask for an OpenXR session (`Sly3-Release/main.cpp`).
VR for Sly 3 would need Sly 2's `vr.cpp` path checked against Sly 3's renderer and HUD (the eye passes, the Flat
Screen / Immersive modes, the Touch layout and prompts), the Vulkan renderer for the Quest's performance, and then
the start through `GameActivity` as Sly 2.

