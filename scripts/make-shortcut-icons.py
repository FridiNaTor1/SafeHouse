#!/usr/bin/env python3
"""Makes the icons of the desktop shortcuts and of the game programs (launcher/assets/shortcut/).

<game>-logo.png / <game>-logo.ico: the desktop shortcut's icon, the game's logo (launcher/assets/games/<game>-logo.png)
  trimmed to its visible pixels and centred on a transparent square (the logos are wide: padded, never squashed).
  The PNG (256x256) is embedded in the launcher, which installs it into the user's hicolor icon folder for a Linux
  .desktop shortcut; the .ico (16-256 px) is embedded in safehouse.exe for a Windows .lnk (cmake/launcher.rc.in).
<game>.ico: the Windows icon of Sly1-Release.exe / Sly2-Release.exe (their window and taskbar icon), from the
  SteamGridDB icon bundled for "Add to Steam" (launcher/assets/steam/<game>/icon.png; authors in
  launcher/assets/steam/CREDITS.md), 16-128 px.

The outputs are committed, so building needs no Python. Run again after changing a logo or an icon.
Usage: make-shortcut-icons.py [GAME ...]   (default: sly1 sly2)
"""
import pathlib, sys
from PIL import Image

root = pathlib.Path(__file__).resolve().parent.parent
out_dir = root / 'launcher/assets/shortcut'
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
SIDE, MARGIN = 256, 4  # the square, and the transparent margin left and right of the logo

def report(path, size):
    print(path.relative_to(root), f'{size} px max,', path.stat().st_size, 'bytes')

for game in sys.argv[1:] or ['sly1', 'sly2']:
    # The shortcut icon: the logo, padded to a square.
    logo = Image.open(root / 'launcher/assets/games' / f'{game}-logo.png').convert('RGBA')
    logo = logo.crop(logo.getchannel('A').getbbox())
    scale = min((SIDE - 2 * MARGIN) / logo.width, (SIDE - 2 * MARGIN) / logo.height)
    logo = logo.resize((round(logo.width * scale), round(logo.height * scale)), Image.LANCZOS)
    square = Image.new('RGBA', (SIDE, SIDE), (0, 0, 0, 0))
    square.alpha_composite(logo, ((SIDE - logo.width) // 2, (SIDE - logo.height) // 2))
    png = out_dir / f'{game}-logo.png'
    square.save(png, optimize=True)
    report(png, SIDE)
    ico = out_dir / f'{game}-logo.ico'
    square.save(ico, format='ICO', sizes=[(s, s) for s in ICO_SIZES])
    report(ico, SIDE)
    # The game program's icon: the SteamGridDB icon.
    icon = Image.open(root / 'launcher/assets/steam' / game / 'icon.png').convert('RGBA')
    side = min(icon.size)
    sizes = [(s, s) for s in ICO_SIZES if s <= side]
    ico = out_dir / f'{game}.ico'
    icon.save(ico, format='ICO', sizes=sizes)
    report(ico, sizes[-1][0])
