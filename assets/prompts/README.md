# Controller prompt atlases

Button icons for the launcher and for in-game prompts (non-PlayStation
families; in-game PlayStation prompts keep the game's original glyphs).

- Source: [Kenney Input Prompts 1.5A](https://kenney.nl/assets/input-prompts)
  by Kenney (www.kenney.nl), CC0 1.0. Licence: `licenses/kenney-input-prompts/License.txt`.
  1.5A (License.txt "Input Prompts (1.5A)", created 11-07-2026) is the newest release on kenney.nl (checked
  2026-10-28: "1.5a, Fixed fonts, improved vectors"); 1.5 added the 2026 Steam Controller, Steam Frame, Valve
  Index and Meta Quest sets.
- Archive: `kenney_input-prompts_1.5.zip`, sha256
  `ac2fcf599080b0f3ba2d174c9474db6df1a0e96ff0662580e2da79a122ab78a1`.
- PlayStation, Xbox, Nintendo Switch and Steam names and symbols are
  trademarks of their owners; their use does not imply endorsement.

## Files

| File | Contents |
|---|---|
| `prompts_xbox.png` | Xbox Series icons (also the fallback for unknown pads) |
| `prompts_playstation.png` | PlayStation icons (launcher; PS3-style Select/Start) |
| `prompts_nintendo.png` | Nintendo Switch icons, positional (south = Switch "B") |
| `prompts_steamdeck.png` | Steam Deck icons (View / Menu from the 2026 controller set: the same symbols, larger) |
| `prompts_steamcontroller.png` | Steam Controller (2015) icons |
| `prompts_steamcontroller2026.png` | Steam Controller (2026) icons: Kenney's 2026 set (`Steam Controller/controller_button_*`: L1 / R1 / L2 / R2, View, Menu), the Steam face buttons and d-pad, the Steam Deck's stick art (the 2026 set has no sticks) |
| `prompts_ps4.png` | DualShock 4 icons (white-symbol face buttons, Share / Options) |
| `prompts_ps5.png` | DualSense icons (white-symbol face buttons, Create / Options) |
| `prompts_gamecube.png` | Nintendo GameCube icons, positional as SDL maps the pad: A south, X east, B west, Y north, Z the right bumper, L / R the triggers, the yellow C-stick as the right stick (its art also stands in for L3 / R3; no left bumper, Select or Home icons: text) |
| `prompts_switch2.png` | Nintendo Switch 2 icons (Kenney's `Nintendo Switch 2` set), positional like the Switch's |
| `prompts_wii.png` | Wii Classic Controller icons, positional; the round B is the Wii U set's (Kenney's Wii B is the Remote's trigger); stick art stands in for the stick clicks |
| `prompts_wiiu.png` | Wii U Pro Controller icons, positional; stick art stands in for the stick clicks |
| `prompts_metaquest.png` | Meta Quest Touch controllers, both hands as one pad: A / B (right) south / east, X / Y (left) west / north, the grips as bumpers, the triggers, the sticks (the left stick's direction art as the d-pad), Menu as Start, Meta as Guide |
| `prompts_valveindex.png` | Valve Index controllers: A / B (Kenney has one pair: the left hand's are told apart by text), the Quest's grip art (Kenney's Index set has none), the triggers, the left trackpad press as Back, the system button |
| `prompts_steamframe.png` | Steam Frame controllers (the Steam Controller's layout in two hands): Kenney's complete Steam Frame set |
| `prompts_vivewand.png` | HTC Vive wands (no Kenney set): Kenney's Index trackpad / trigger art and the Quest's grip / menu art, all CC0: each trackpad's press as that hand's face button, the menu buttons, the trackpads as the sticks |
| `prompts_pico.png` | Pico controllers (no Kenney set): the Quest's art, the same layout |
| `prompts_keyboard.png` | Keycaps (letters, digits, F-keys, punctuation, editing keys) and the mouse (buttons, wheel, movement) |
| `prompts_devices.png` | Controller pictures for Sly 2's pause Controls page: Kenney's Xbox Series, Switch Pro, Steam Controller (2015), PS4 and PS5 silhouettes (Vector set, rasterised by the packer: 8 px per unit, even-odd fill, 4x supersampled), the GameCube, Switch 2 Pro, Wii Classic Controller Pro and Wii U Pro silhouettes, each as a body cell and a filled, dilated underlay cell; the Steam Deck and the 2026 Steam Controller are composed by the packer (`compose_steamdeck`, `compose_steamcontroller2026`): Kenney's silhouettes leave out their thumbsticks, so the Deck is drawn after the real one (d-pad / face buttons and thumbsticks over square trackpads, View / Menu by the screen's top corners, the bumpers and triggers on the top edge) and the 2026 controller is Kenney's `controller_steam_new` with the d-pad / face buttons moved out and a stick added above each trackpad |
| `prompt_atlas_rects.inc` | Icon rects per family and button, used by `platform/button_prompts.h` |
| `prompt_data.cpp` | Embeds the PNGs and `gamecontrollerdb.txt` (built by `cmake/prompts.cmake`) |

Each icon is a 128 px cell from the archive's `Double` set in a 144 px grid
(7 columns): 8 px of transparent padding around every cell repeats the cell's
edge colours, and transparent pixels inside a cell carry the colour of the
nearest visible pixel, so straight-alpha blending with linear filtering and
mipmaps shows no dark or green fringes. Rects cover the 112 px square inside
each cell where the artwork lies.

## Regenerating

The outputs are committed; only regenerate after changing the icon table in
`tools/pack_prompts.py`:

```
curl -L -o kenney_input-prompts_1.5.zip \
  https://kenney.nl/media/pages/assets/input-prompts/8de120163f-1783763952/kenney_input-prompts_1.5.zip
python3 -I tools/pack_prompts.py --zip kenney_input-prompts_1.5.zip          # writes the files
python3 -I tools/pack_prompts.py --zip kenney_input-prompts_1.5.zip --check  # verifies them
```

Without the zip, `--dir` reads an extracted copy of the same release (its top folder holds "Xbox Series", ...).
On 2026-11-10 kenney.nl refused downloads (HTTP 403) and itch.io rate-limited, so the GameCube / Switch 2 / Wii /
Wii U atlases were packed from the 1.5A files in https://github.com/Nekuzaky/input-prompts-unity (commit
`212687087fb247a12537a6df91fcb24500e8ac36`, `Assets/Sprites/input-prompts`; the Unity `.meta` files are ignored):
`python3 -P tools/pack_prompts.py --dir <that folder> --check` reproduced every atlas, the controller pictures and
the rect table committed before byte for byte, so the copy's files are the release's. Re-run with the pinned zip when
it can be downloaded again; `--check` should then pass unchanged.

The sha256 above was checked again on 2026-10-28 (kenney.nl's download links are
tokenised and expire; keep the archive outside the tracked tree, e.g. in `build/`).
The script refuses archives with another sha256, reads the icons straight
from the zip and produces byte-identical output on every run. It needs Pillow
(use `python3 -P` instead of `-I` if Pillow is only installed for your user).
The `button-prompts` test checks that the embedded PNGs match the rect table,
the padding and bleed rules above, and the GL upload; `button-prompts-render`
writes review images to `<build>/prompt-review`.
