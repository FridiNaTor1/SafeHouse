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
| `prompts_keyboard.png` | Keycaps (letters, digits, F-keys, punctuation, editing keys) and the mouse (buttons, wheel, movement) |
| `prompts_devices.png` | Controller pictures for Sly 2's pause Controls page: Kenney's Xbox Series, Switch Pro, Steam Controller (2015), PS4 and PS5 silhouettes (Vector set, rasterised by the packer: 8 px per unit, even-odd fill, 4x supersampled), each as a body cell and a filled, dilated underlay cell; the Steam Deck and the 2026 Steam Controller are composed by the packer (`compose_steamdeck`, `compose_steamcontroller2026`): Kenney's silhouettes leave out their thumbsticks, so the Deck is drawn after the real one (d-pad / face buttons and thumbsticks over square trackpads, View / Menu by the screen's top corners, the bumpers and triggers on the top edge) and the 2026 controller is Kenney's `controller_steam_new` with the d-pad / face buttons moved out and a stick added above each trackpad |
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

The sha256 above was checked again on 2026-10-28 (kenney.nl's download links are
tokenised and expire; keep the archive outside the tracked tree, e.g. in `build/`).
The script refuses archives with another sha256, reads the icons straight
from the zip and produces byte-identical output on every run. It needs Pillow
(use `python3 -P` instead of `-I` if Pillow is only installed for your user).
The `button-prompts` test checks that the embedded PNGs match the rect table,
the padding and bleed rules above, and the GL upload; `button-prompts-render`
writes review images to `<build>/prompt-review`.
