#!/usr/bin/env python3
"""The developers' pictures of the Collection Style Launcher's developers page (launcher/assets/developers).

    tools/make-developer-pictures.py NAME=PICTURE ... [--out launcher/assets/developers]

For each developer (fridinator, theclub654, menaman123, thematuu, nagaa95) and the picture its owner supplied:
  <name>.png             the picture, square, 512 x 512
  <name>-silhouette.png  its subject as a black shape with alpha (cut out of the picture itself, so it lines up with
                         the picture when the page cross-fades between the two), with a few plain prop shapes around it
                         that hint at what the person did (drawn here: a gear and a spanner, stars, a laptop, a puzzle
                         piece and a branch line, a handheld and a chip)

How each subject is told from its background is written per picture below (a plain wall, a flat grey, the picture's
own alpha, a starry sky, a pale square). Needs Pillow and NumPy.
"""
import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

SIZE = 512
SCALE = 4  # the props are drawn larger and reduced, for smooth edges


def largest(mask):
    """The largest connected part of a mask (4-neighbours), holes filled."""
    h, w = mask.shape
    labels = np.zeros((h, w), np.int32)
    best, best_size, current = 0, 0, 0
    for y0, x0 in zip(*np.nonzero(mask)):
        if labels[y0, x0]:
            continue
        current += 1
        stack, size = [(y0, x0)], 0
        labels[y0, x0] = current
        while stack:
            y, x = stack.pop()
            size += 1
            for ny, nx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
                if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not labels[ny, nx]:
                    labels[ny, nx] = current
                    stack.append((ny, nx))
        if size > best_size:
            best, best_size = current, size
    return fill_holes(labels == best)


def fill_holes(mask):
    """Everything not reachable from the border through the background belongs to the shape."""
    h, w = mask.shape
    outside = np.zeros((h, w), bool)
    stack = [(y, x) for y in range(h) for x in (0, w - 1)] + [(y, x) for x in range(w) for y in (0, h - 1)]
    while stack:
        y, x = stack.pop()
        if 0 <= y < h and 0 <= x < w and not mask[y, x] and not outside[y, x]:
            outside[y, x] = True
            stack += [(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)]
    return ~outside


def smooth(mask, grow=0):
    """A mask as a soft-edged 8-bit image (closing small gaps; grow > 0 widens it by that many pixels)."""
    image = Image.fromarray((mask * 255).astype(np.uint8))
    image = image.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(5))
    if grow:
        image = image.filter(ImageFilter.MaxFilter(2 * grow + 1))
    return image.filter(ImageFilter.GaussianBlur(1.2)).point(lambda v: 255 if v > 150 else 0 if v < 90 else int((v - 90) * 255 / 60))


def subject(name, picture):
    """The subject's mask (bool array) of a 512 x 512 RGBA picture."""
    a = np.asarray(picture).astype(np.int32)
    r, g, b, alpha = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
    top, low = np.maximum(np.maximum(r, g), b), np.minimum(np.minimum(r, g), b)
    spread = top - low
    if name == "fridinator":
        # a photograph in front of a pale wall: the wall is bright and colourless, the people are not
        wall = (top > 170) & (spread < 26)
        return largest(~wall)
    if name == "theclub654":
        # a drawn portrait on flat grey (with a faint copy of it at the left, a shade off the grey): what is far
        # enough from the grey
        grey = np.median(a[:40, -40:, :3].reshape(-1, 3), axis=0)
        far = np.abs(a[..., :3] - grey).sum(axis=2) > 70
        return largest(far)
    if name == "menaman123":
        # the emblem has its own alpha; its bright parts (the eyes, the muzzle) are left open, so the mask reads
        return (alpha > 128) & ~((top > 150) & (b > 120))
    if name == "thematuu":
        # a drawn figure on a purple sky with stars: the sky is purple (red and blue over green, not bright), the
        # figure is brown, skin, clear blue, white and black
        sky = (b > g + 18) & (r > g) & (top < 190) & ~((b > 150) & (r < 110))
        stars = (low > 150) & (spread < 60)
        eyes = (low > 170) & (spread < 70)
        return largest(~(sky | stars)) & ~eyes
    if name == "nagaa95":
        # a pattern of green squares on a pale square
        return (g > r + 25) & (g > b + 25)
    raise SystemExit(f"no rule for {name}")


def gear(d, cx, cy, radius, teeth=8):
    points = []
    for i in range(teeth * 4):
        angle = math.pi * 2 * i / (teeth * 4)
        rad = radius if i % 4 in (0, 1) else radius * 0.74
        points.append((cx + math.cos(angle + 0.1) * rad, cy + math.sin(angle + 0.1) * rad))
    d.polygon(points, fill=255)
    d.ellipse((cx - radius * 0.3, cy - radius * 0.3, cx + radius * 0.3, cy + radius * 0.3), fill=0)


def spanner(d, x0, y0, x1, y1, width):
    d.line((x0, y0, x1, y1), fill=255, width=int(width))
    for x, y, away in ((x0, y0, (x0 - x1, y0 - y1)), (x1, y1, (x1 - x0, y1 - y0))):
        d.ellipse((x - width * 1.3, y - width * 1.3, x + width * 1.3, y + width * 1.3), fill=255)
        n = math.hypot(*away)
        ox, oy = away[0] / n * width * 1.1, away[1] / n * width * 1.1
        d.ellipse((x + ox - width * 0.6, y + oy - width * 0.6, x + ox + width * 0.6, y + oy + width * 0.6), fill=0)


def star(d, cx, cy, radius, turn=-math.pi / 2):
    points = []
    for i in range(10):
        rad = radius if i % 2 == 0 else radius * 0.42
        points.append((cx + math.cos(turn + math.pi * i / 5) * rad, cy + math.sin(turn + math.pi * i / 5) * rad))
    d.polygon(points, fill=255)


def laptop(d, x, y, w):
    h = w * 0.62
    d.rounded_rectangle((x, y, x + w, y + h), radius=w * 0.06, fill=255)
    d.rectangle((x + w * 0.08, y + h * 0.12, x + w * 0.92, y + h * 0.84), fill=0)
    d.polygon([(x - w * 0.14, y + h * 1.22), (x + w * 1.14, y + h * 1.22), (x + w * 1.0, y + h * 1.02), (x, y + h * 1.02)], fill=255)


def puzzle(d, x, y, s):
    d.rectangle((x, y, x + s, y + s), fill=255)
    k = s * 0.2
    d.ellipse((x + s / 2 - k, y - k * 1.3, x + s / 2 + k, y + k * 0.7), fill=255)        # a knob on top
    d.ellipse((x + s - k * 0.7, y + s / 2 - k, x + s + k * 1.3, y + s / 2 + k), fill=255)  # and on the right
    d.ellipse((x + s / 2 - k, y + s - k * 1.3, x + s / 2 + k, y + s + k * 0.7), fill=0)    # a notch below


def branch(d, x, y, s):
    """Two lines that part and meet again, with dots: work branched off and merged."""
    w = int(s * 0.1)
    d.line((x, y, x, y + s), fill=255, width=w)
    d.line((x, y + s * 0.2, x + s * 0.45, y + s * 0.42, x + s * 0.45, y + s * 0.62, x, y + s * 0.84), fill=255, width=w, joint="curve")
    for px, py in ((x, y), (x, y + s), (x + s * 0.45, y + s * 0.52)):
        d.ellipse((px - w * 1.5, py - w * 1.5, px + w * 1.5, py + w * 1.5), fill=255)


def handheld(d, x, y, w):
    h = w * 0.46
    d.rounded_rectangle((x, y, x + w, y + h), radius=h * 0.3, fill=255)
    d.rectangle((x + w * 0.27, y + h * 0.14, x + w * 0.73, y + h * 0.86), fill=0)
    d.ellipse((x + w * 0.08, y + h * 0.3, x + w * 0.19, y + h * 0.3 + w * 0.11), fill=0)
    d.ellipse((x + w * 0.81, y + h * 0.45, x + w * 0.92, y + h * 0.45 + w * 0.11), fill=0)


def chip(d, x, y, s):
    d.rounded_rectangle((x, y, x + s, y + s), radius=s * 0.1, fill=255)
    d.rectangle((x + s * 0.28, y + s * 0.28, x + s * 0.72, y + s * 0.72), fill=0)
    pin = s * 0.09
    for i in range(4):
        t = s * (0.14 + 0.24 * i)
        for box in ((x + t, y - pin * 1.6, x + t + pin, y), (x + t, y + s, x + t + pin, y + s + pin * 1.6),
                    (x - pin * 1.6, y + t, x, y + t + pin), (x + s, y + t, x + s + pin * 1.6, y + t + pin)):
            d.rectangle(box, fill=255)


def props(name):
    """The prop shapes of a developer, as an 8-bit mask of the full square."""
    big = Image.new("L", (SIZE * SCALE, SIZE * SCALE), 0)
    d = ImageDraw.Draw(big)
    u = SIZE * SCALE / 100.0  # (hundredths of the square)
    if name == "fridinator":  # the maintainer: a gear and a spanner
        gear(d, 15 * u, 15 * u, 12 * u)
        spanner(d, 78 * u, 6 * u, 95 * u, 26 * u, 3.0 * u)
    elif name == "theclub654":  # special thanks: stars
        star(d, 85 * u, 14 * u, 12 * u)
        star(d, 96 * u, 36 * u, 5.5 * u, 0.3)
        star(d, 10 * u, 88 * u, 7 * u, -0.6)
    elif name == "menaman123":  # a desktop port: a laptop; a contributor: a branch line
        laptop(d, 70 * u, 76 * u, 24 * u)
        branch(d, 8 * u, 4 * u, 20 * u)
    elif name == "thematuu":  # a contributor: a puzzle piece and a branch line
        puzzle(d, 76 * u, 76 * u, 17 * u)
        branch(d, 90 * u, 6 * u, 20 * u)
    elif name == "nagaa95":  # a handheld port and another processor: a handheld and a chip
        handheld(d, 3 * u, 80 * u, 36 * u)
        chip(d, 8 * u, 36 * u, 16 * u)
    return big.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pictures", nargs="+", help="NAME=PICTURE")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "launcher" / "assets" / "developers")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    yy, xx = np.mgrid[0:SIZE, 0:SIZE]
    disc = (xx - SIZE / 2 + 0.5) ** 2 + (yy - SIZE / 2 + 0.5) ** 2 <= (SIZE * 0.5) ** 2
    for item in args.pictures:
        name, source = item.split("=", 1)
        picture = Image.open(source).convert("RGBA")
        side = min(picture.size)
        left, top = (picture.width - side) // 2, (picture.height - side) // 2
        picture = picture.crop((left, top, left + side, top + side)).resize((SIZE, SIZE), Image.LANCZOS)
        picture.save(args.out / f"{name}.png", optimize=True)
        # the subject inside the disc the picture is shown in, then the props (which may reach past the disc)
        shape = np.asarray(smooth(subject(name, picture) & disc)).astype(np.int32)
        shape = np.maximum(shape * disc, np.asarray(props(name)).astype(np.int32))
        out = np.zeros((SIZE, SIZE, 4), np.uint8)
        out[..., 3] = np.clip(shape, 0, 255)
        Image.fromarray(out, "RGBA").save(args.out / f"{name}-silhouette.png", optimize=True)
        print(name, "->", args.out / f"{name}.png", args.out / f"{name}-silhouette.png")


if __name__ == "__main__":
    main()
