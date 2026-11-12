# SDL_GameControllerDB

`gamecontrollerdb.txt` is an unmodified copy of the community controller
mapping database, used by `platform/gamepad_mappings.h` (game and launcher) so
pads missing from GLFW's built-in table are still recognised as gamepads.

- Source: https://github.com/mdqinc/SDL_GameControllerDB
- Commit: `c0f11d40b91f57853ce9714c703e119d8243f91c` (2026-11-09, "Add vivo GP2031 A controller mapping"; updated 2026-11-10)
- URL: https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/c0f11d40b91f57853ce9714c703e119d8243f91c/gamecontrollerdb.txt
- sha256: `a2299ef257b68c62b9fcce39fc59b5fb3a5a7068214f766a96c21595b55a11c3`
- License: zlib, see `licenses/sdl-gamecontrollerdb/LICENSE.txt`

To update, download the file from a newer pinned commit, record the commit and
sha256 here and rebuild. The build embeds it; at runtime only lines for the
current platform are passed to `glfwUpdateGamepadMappings`, followed by any
mappings in `SDL_GAMECONTROLLERCONFIG` (Steam sets this for its virtual pads).

GLFW matches a line only by the exact GUID (version included), so
`ensureFallbackMappings()` (same header) also reuses a line for the same bus /
vendor / product with another version, then builds an Xbox-layout mapping from
the evdev codes (Linux) or by index for any pad the database lacks. The licence
file `licenses/sdl-gamecontrollerdb/LICENSE.txt` is unchanged at this commit
(zlib, Copyright (C) 1997-2025 Sam Lantinga / the contributors).
