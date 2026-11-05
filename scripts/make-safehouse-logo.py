#!/usr/bin/env python3
"""Makes launcher/assets/safehouse-logo.png, the SafeHouse logo (the mask and cane emblem by HoshiYumeHime, DeviantArt)
as the launcher's rail and About page show it, from the artist's original image (1800x2100 RGBA).

Trimmed to its visible pixels, scaled to 512 px high (the launcher draws it at 56-120 px, times the UI scale, with
mipmaps), and given a thin light rim around the silhouette: the art's black outline and the mask's dark navy otherwise
melt into the launcher's dark background (launcher/ui/theme.h color::sidebar / surface).
Usage: make-safehouse-logo.py SOURCE.png [OUTPUT.png]   (default output: launcher/assets/safehouse-logo.png)
"""
import pathlib, sys
from PIL import Image, ImageFilter

root = pathlib.Path(__file__).resolve().parent.parent
source = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else root / 'launcher/assets/safehouse-logo.png'
HEIGHT, PAD, RIM = 512, 12, 11  # output height, transparent margin, rim width (a MaxFilter size, odd)
RIM_COLOR = (226, 236, 255)  # color::text's blue-white, a touch softer than pure white

art = Image.open(source).convert('RGBA')
art = art.crop(art.getchannel('A').point(lambda v: 255 if v > 8 else 0).getbbox())  # near-invisible speckle ignored
inner = HEIGHT - 2 * PAD
art = art.resize((round(art.width * inner / art.height), inner), Image.LANCZOS)
width = art.width + 2 * PAD
alpha = Image.new('L', (width, HEIGHT), 0)
alpha.paste(art.getchannel('A'), (PAD, PAD))
rim = Image.new('RGBA', (width, HEIGHT), RIM_COLOR + (255,))
rim.putalpha(alpha.filter(ImageFilter.MaxFilter(RIM)).filter(ImageFilter.GaussianBlur(1.2)))
logo = Image.new('RGBA', (width, HEIGHT), (0, 0, 0, 0))
logo.alpha_composite(rim)
logo.alpha_composite(art, (PAD, PAD))
logo.save(out, optimize=True)
print(out, logo.size, out.stat().st_size, 'bytes')
