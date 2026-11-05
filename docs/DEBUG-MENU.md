# The debug menu and PC Extras

[Back to the README](../README.md)

Sly 2 has a PC debug menu in the style of the Sly 2 prototype's debug menu, with extras the original never had. It is
not part of normal play: turn on **Settings > Advanced > Debug mode** in the launcher (while the game is closed). That
one switch turns on the debug menu, the debug keys below and the games' other debug features; with it off none of them
does anything.

Rows marked **[save]** change your progress (see [Jobs](#jobs) and [Player](#player)). Everything else leaves the save
alone. Back up your saves before experimenting.

<p align="center"><img src="images/debug-menu.jpg" alt="The debug menu" width="720"></p>

## Opening and moving around

- **Open / close:** **F10**, or hold **R1** and press **Select** on a controller.
- The game is **frozen** while the menu is open (music and a dialog line already playing go on).
- **Move:** D-pad, left stick, arrow keys, W / S or the mouse wheel (or point at a row with the mouse).
- **Select:** Cross, Enter, Space or a left click.
- **Back:** Circle, Triangle, Esc, E, Q or a right click. Back from the first page closes the menu.
- The title line shows the port's version.

## The pages

| Page | What it does |
| --- | --- |
| **Warps** | An episode, then any of its worlds (the hub, the interiors, the job worlds), by the game's own names. The world loads as in normal play, at its entrance. |
| **Character** | Makes Sly, Bentley or Murray the player, at the current character's place, among the characters the world has loaded (not from inside the safe house). |
| **PC Extras** | Play as Carmelita, guards and bosses, Sly's tango outfit, and Bentley's RC chopper. See [PC Extras](#pc-extras). |
| **Noclip** | Fly the current character through walls. See [Noclip](#noclip). |
| **Teleport** | Puts the player on one of this world's checkpoints or warp points. |
| **Jobs** | **Complete Current Job [save]** (as F9), or **start** one of this world's jobs the way the game does. |
| **Player** | Infinite health and power (F7), a refill, unlock all gadgets / 9999 coins / all characters (F6) **[save]**, Save Game Now (F5) **[save]**. |
| **Speed** | The game's speed, from 0.1x to 2x. |
| **Display** | Hide the HUD (F11), a performance line (frame rate, world, position), the free camera (F1). |
| **Reset World** | Reloads the current world at its entrance. |

## Noclip

Flies the current character with no collision or gravity; the camera passes through walls too. Health and power stay
full while flying. Turning it off (the menu again) drops the character back into normal physics where it is.

| Controller | Keyboard | Action |
| --- | --- | --- |
| Left stick | W A S D | Move level, relative to the camera |
| Cross / R2 | Space / 3 | Up |
| Circle / L2 | E / 2 | Down |
| R1 held | Shift | 4x faster |
| L1 held | 1 | 4x slower |
| Right stick | Mouse | Camera |

## The free camera

**F1**, or **Display > Free Camera**. The game keeps running while you move the camera.

| Controller | Keyboard / mouse | Action |
| --- | --- | --- |
| Left stick | W A S D | Move along the view / strafe |
| Right stick | Right mouse button held | Look around |
| R2 / L2 | E / Q | Up / down |
| R1 held / L1 held | Left Shift (faster) | Faster / slower |
| Select | F1 | Leave |
| R1 + Select | | Leave and open the debug menu |

## PC Extras

Not in the original game. Each guest is loaded from its own world's files while the current world keeps running (the
first time takes a moment; after that switching is instant). Nothing here is saved; choosing Normal, or turning Debug
mode off, puts everything back.

### Carmelita

**PC Extras > Characters > Carmelita:** her Prologue, Episode 6 (snow) or Episode 2 (tango dress) model, on Sly,
Bentley or Murray. With **Her moves** on (the default) she plays with her own moveset:

| Button | Carmelita |
| --- | --- |
| Left stick | Walk (a slight push walks slowly) and turn |
| R1 held | Run |
| Cross | Jump; Cross again in the air: her double jump with its flip |
| Circle | The interaction in reach (doors, switches, talking), otherwise her high jump |
| Square | Her shock pistol |
| Triangle | Aim mode on / off: she strafes and fires where the camera looks |

With **Her moves** off she follows the player's own moves (ledges, ropes and gadgets show her idle / run / jump).

<p align="center"><img src="images/pc-extras-carmelita.jpg" alt="Carmelita in the aim mode" width="720"></p>

**Sly outfit: Tango** dresses Sly in his ballroom tuxedo with all his usual moves.

### Guards and bosses

**PC Extras > Guards > Episode n** lists every guard type of that episode; **PC Extras > Bosses** has four bosses. The
player keeps Sly's camera, health and HUD, with the guard's model, animations and attacks: **Square** and **Circle** use
the guard's own attacks (strikes, shots, missiles, thrown objects), and their hits knock out other guards with the
game's own hit code. The menu row shows each character's controls.

<p align="center"><img src="images/pc-extras-rajan.jpg" alt="Playing as Rajan" width="720"></p>

### Bentley's RC chopper

**PC Extras > Characters > Bentley's RC chopper** flies the RC chopper from Episode 2's RC Bombing Run in any world.
Choose the row again (or switch character) to get the player back.

| Controller | Keyboard / mouse | RC chopper |
| --- | --- | --- |
| Left stick up / down | W / S | Fly forward / back |
| Left stick left / right | A / D | Turn |
| R2 / L2 | 3 / 2 | Climb / sink |
| Square | Left click | Forward gun (aims at the nearest guard ahead) |
| Cross | Space | Drop a bomb |
| Right stick | Mouse | Orbit the camera |
| R3 | The key bound to R3 | Chase camera / top-down camera |

<p align="center"><img src="images/pc-extras-rc-chopper.jpg" alt="The RC chopper over Paris" width="720"></p>

## Debug keys

With Debug mode on, these keys work in the games:

| Key | Game | What it does |
| --- | --- | --- |
| F3 | Sly 1 | Opens or closes the debug window. |
| F1 | Sly 2 | Toggles the free camera. |
| F2 | Sly 2 | Goes to the Paris hub. |
| F3 | Sly 2 | Goes to the next world. |
| F5 | Sly 2 | Saves to the slot being played (slot 1 if none). |
| F6 | Sly 2 | Unlocks every gadget and power-up, sets 9999 coins and makes every character selectable. |
| F7 | Sly 2 | Toggles infinite health and gadget power. |
| F9 | Sly 2 | **Completes the current job.** |
| F10 | Sly 2 | Opens or closes the debug menu (controller: R1 + Select). |
| F11 | Sly 2 | Hides or shows the HUD, for clean screenshots. |

**F9** is meant for getting past a softlock. It ends the job you are in as if every remaining task had been done: the
game's own "Job Completed!" screen, closing sequence and autosave run, the job is saved as completed and the next jobs
unlock as usual. Outside a job it does nothing. Each use prints the job and task it skipped to the game's log (in
`launcher.log` with **Save logs** on), which is useful in a bug report.

Sly 3 has no debug keys yet.
