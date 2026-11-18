#!/usr/bin/env python3
"""The icons of the shortcuts that open the Collection Style Launcher ("Sly Collection"), made from the SafeHouse
logo (launcher/assets/safehouse-logo.png, the mask and cane by HoshiYumeHime) fitted into a transparent square.

  launcher/assets/shortcut/collection-logo.png   256 x 256: the Linux .desktop shortcut's icon (installed into the
                                                 user's hicolor folder) and the Android pinned shortcut's icon
  launcher/assets/shortcut/collection-logo.ico   16-256 px: the Windows .lnk's icon (embedded in safehouse.exe)
  launcher/assets/steam/collection/icon.png      256 x 256: the Steam shortcut's icon
  launcher/assets/shortcut/collection-adaptive.png  432 x 432: the logo inside the middle 66 % (an Android adaptive
                                                 icon's safe zone), for launchers that mask shortcut icons

The outputs are committed, so building needs no Python. Run again after changing the logo. Needs Pillow.
"""
import pathlib
from PIL import Image

root = pathlib.Path(__file__).resolve().parent.parent
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def square(logo, side, fill):
    """The logo, trimmed to its visible pixels, scaled to fill `fill` of the side, centred on a transparent square."""
    logo = logo.crop(logo.getchannel('A').getbbox())
    scale = min(side * fill / logo.width, side * fill / logo.height)
    logo = logo.resize((round(logo.width * scale), round(logo.height * scale)), Image.LANCZOS)
    out = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    out.alpha_composite(logo, ((side - logo.width) // 2, (side - logo.height) // 2))
    return out


def main():
    logo = Image.open(root / 'launcher/assets/safehouse-logo.png').convert('RGBA')
    icon = square(logo, 256, 0.94)
    outputs = {
        root / 'launcher/assets/shortcut/collection-logo.png': icon,
        root / 'launcher/assets/steam/collection/icon.png': icon,
        root / 'launcher/assets/shortcut/collection-adaptive.png': square(logo, 432, 0.62),
    }
    for path, image in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        image.save(path, optimize=True)
        print(path.relative_to(root), f'{image.width} x {image.height},', path.stat().st_size, 'bytes')
    ico = root / 'launcher/assets/shortcut/collection-logo.ico'
    icon.save(ico, format='ICO', sizes=[(s, s) for s in ICO_SIZES])
    print(ico.relative_to(root), f'{", ".join(str(s) for s in ICO_SIZES)} px,', ico.stat().st_size, 'bytes')


if __name__ == '__main__':
    main()
