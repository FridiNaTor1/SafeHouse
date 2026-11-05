# SDL_GameControllerDB

`gamecontrollerdb.txt` is an unmodified copy of the community controller
mapping database, used by `platform/gamepad_mappings.h` (game and launcher) so
pads missing from GLFW's built-in table are still recognised as gamepads.

- Source: https://github.com/mdqinc/SDL_GameControllerDB
- Commit: `c1d5289a1f713b30a2c121e9fe6529d39360be0f` (2026-10-02)
- URL: https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/c1d5289a1f713b30a2c121e9fe6529d39360be0f/gamecontrollerdb.txt
- sha256: `e606134678e6b3fdbdec9081289a1f0ba3e25d53b59b2aa3cdfe69bf491ad18f`
- License: zlib, see `licenses/sdl-gamecontrollerdb/LICENSE.txt`

To update, download the file from a newer pinned commit, record the commit and
sha256 here and rebuild. The build embeds it; at runtime only lines for the
current platform are passed to `glfwUpdateGamepadMappings`, followed by any
mappings in `SDL_GAMECONTROLLERCONFIG` (Steam sets this for its virtual pads).
