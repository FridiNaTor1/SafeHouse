#!/usr/bin/env python3
"""Makes packaging/ios/AppIcon-1024.png, the iOS app icon: the SafeHouse logo (launcher/assets/safehouse-logo.png, the
launcher's own art) centred on the launcher's dark navy (launcher/ui/theme.h), opaque and square as iOS wants it (the
system rounds the corners). scripts/ios/package.py compiles it into the app's asset catalog.
Usage: make-app-icon.py [OUTPUT.png]"""
import pathlib, sys
from PIL import Image

root = pathlib.Path(__file__).resolve().parent.parent.parent
out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'packaging/ios/AppIcon-1024.png'
SIZE, LOGO = 1024, 0.78  # the logo's height as a share of the icon
BACKGROUND = (0x0D, 0x17, 0x2C)  # color::surface
logo = Image.open(root / 'launcher/assets/safehouse-logo.png').convert('RGBA')
height = round(SIZE * LOGO)
logo = logo.resize((round(logo.width * height / logo.height), height), Image.LANCZOS)
icon = Image.new('RGBA', (SIZE, SIZE), BACKGROUND + (255,))
icon.alpha_composite(logo, ((SIZE - logo.width) // 2, (SIZE - logo.height) // 2))
icon.convert('RGB').save(out, optimize=True)
print(out, icon.size, out.stat().st_size, 'bytes')
