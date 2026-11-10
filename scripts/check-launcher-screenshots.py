#!/usr/bin/env python3
"""Checks the output of scripts/screenshot-launcher.sh (layout, navigation and prompts).

    python3 -I scripts/check-launcher-screenshots.py OUTPUT_DIR [--quick]

Standard library only (a small PNG reader is included), so it runs with -I.
Each NAME.png comes with the NAME.json layout dump written by the launcher's
hidden --dump-layout option.
"""
import hashlib
import json
import pathlib
import struct
import sys
import zlib

PAD_FAMILIES = ["xbox", "ps", "nintendo", "steamdeck", "steamcontroller"]
FOCUS = (0x5E, 0xC0, 0xFF)
LOGO_GOLD = (0xF0, 0xC8, 0x20)  # the cane of the SafeHouse logo (the emblem)
SIZES = ["1280x800", "960x600", "1920x1080", "2560x1440"]
SINGLE = ["game-sly1-notinstalled", "game-sly2-notinstalled", "game-sly2-installed", "game-sly2-ready",
          "game-sly2-installing", "game-sly3", "game-sly4", "game-hackpack", "install-sly1-notinstalled",
          "install-sly1-installed", "install-sly2", "install-progress-sly1", "install-progress-sly2", "about",
          "nav-rail", "nav-rail-down", "nav-enter-page", "nav-open-install", "nav-install-back", "nav-rb",
          "nav-nintendo-enter", "nav-keyboard-down", "nav-settings-row", "install-path", "binding-capture",
          "remembered-game", "nav-right-page", "nav-left-rail", "nav-settings-rows", "sly3-notice",
          "nav-rail-discord", "nav-rail-patreon", "settings-games"] + ["settings-" + c for c in
                                ("display", "graphics", "audio", "controls", "interface", "launcher", "advanced")] + [
          "settings-sly2-" + c for c in ("display", "graphics", "launcher", "advanced")]
# name: (game, view) the layout dump must report.
STATES = {
    "nav-rail": ("sly1", "game"), "nav-rail-down": ("sly2", "game"), "nav-enter-page": ("sly2", "game"),
    "nav-open-install": ("sly2", "install"), "nav-install-back": ("sly2", "game"), "nav-rb": ("sly2", "game"),
    "nav-nintendo-enter": ("sly2", "install"), "nav-keyboard-down": ("sly3", "game"),
    "nav-settings-row": (None, "settings"), "remembered-game": ("sly3", "game"), "about": (None, "about"),
    "game-hackpack": ("hackpack", "game"), "install-sly2": ("sly2", "install"),
    "sly3-notice": ("sly3", "game"), "nav-rail-discord": (None, "about"), "nav-rail-patreon": (None, "about"),
}
# name: label of the item that must have the pad/keyboard focus
FOCUSED = {
    "game-sly1-xbox": "Play", "game-sly2-notinstalled": "Install", "game-sly2-installed": "Verify files",
    "nav-rail-down": "Sly 2: Band of Thieves", "nav-right-page": "Play",
    "nav-left-rail": "Sly Cooper and the Thievius Raccoonus", "nav-enter-page": "Install",
    "nav-keyboard-down": "Sly 3: Honor Among Thieves", "nav-settings-rows": "Game",
    "binding-capture": "Move up",
    # The community links under About: down from About, right to Patreon (right stays in the rail there).
    "nav-rail-discord": "rail-discord", "nav-rail-patreon": "rail-patreon", "sly3-notice": "Play anyway",
}
# name: (labels that must be present, labels that must be absent)
LABELS = {
    "game-sly1-xbox": ({"Play", "Verify files", "Reinstall", "Uninstall", "rail-discord", "rail-patreon"}, {"Install", "sly3-notice"}),
    "game-sly1-notinstalled": ({"Install"}, {"Play"}),
    "game-sly2-notinstalled": ({"Install"}, {"Play"}),
    "game-sly2-installed": ({"In development", "Verify files"}, {"Play", "Install"}),
    "game-sly2-ready": ({"Play"}, {"Install", "In development"}),
    # Sly 3's early-development banner, on its page only; the confirmation before it plays.
    "game-sly3": ({"Install", "sly3-notice"}, {"Play"}), "game-sly4": (set(), {"Play", "Install"}),
    "game-hackpack": (set(), {"Play", "Install"}),
    "install-path": ({"Search"}, set()),
    "sly3-notice": ({"sly3-notice", "Don't show again", "Cancel", "Play anyway"}, set()),
    # (the developers and the full credits of launcher/credits.h; scripts/test-collection-launcher.py checks every one)
    "about": ({"Join us on Discord (ThiefRush)", "Support us on Patreon", "rail-discord", "rail-patreon", "credits-developers",
               "developer FridiNaTor", "credits-decompilation", "credits-trademarks", "credits-licences"}, {"sly3-notice"}),
    "settings-controls": ({"Disable controller input when unfocused"}, {"Always show Original PS layout", "Button prompts", "Debug keys"}),
    "settings-interface": ({"Menu style", "Menu size"}, {"Streamer mode"}),
    "settings-launcher": ({"Launcher size", "Button prompts"}, set()),
    "settings-advanced": ({"Debug mode", "Save logs", "Log folder"}, set()),
}


def read_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path.name}: not a PNG")
    pos, idat, width = 8, [], None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            if depth != 8 or color not in (2, 6):
                raise ValueError(f"{path.name}: unsupported PNG format")
            channels = 4 if color == 6 else 3
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    rows, previous, offset = [], bytearray(stride), 0
    for _ in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            upleft = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 255
            elif kind == 4:
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else upleft)) & 255
        rows.append(bytes(line))
        previous = line
    return width, height, channels, rows


def pixels(image, box=None):
    width, height, channels, rows = image
    x0, y0, x1, y1 = box or (0, 0, width, height)
    for y in range(max(0, y0), min(height, y1)):
        row = rows[y]
        for x in range(max(0, x0), min(width, x1)):
            yield row[x * channels:x * channels + 3]


def near(rgb, target, tolerance):
    return all(abs(a - b) <= tolerance for a, b in zip(rgb, target))


def crop_hash(image, box):
    digest = hashlib.sha256()
    for rgb in pixels(image, box):
        digest.update(bytes(rgb))
    return digest.hexdigest()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = pathlib.Path(sys.argv[1])
    quick = "--quick" in sys.argv[2:]
    families = ["xbox"] if quick else PAD_FAMILIES + ["keyboard"]
    failures = []
    expected = {f"game-sly1-{f}": (1280, 800) for f in families}
    expected.update({name: (1280, 800) for name in SINGLE})
    for z in SIZES:
        w, h = map(int, z.split("x"))
        for page in ("game-sly1", "game-sly3", "install-sly2", "settings", "about"):
            expected[f"size-{page}-{z}"] = (w, h)
    images, layouts = {}, {}
    for name, size in sorted(expected.items()):
        png, layout = out / f"{name}.png", out / f"{name}.json"
        if not png.exists() or not layout.exists():
            failures.append(f"{name}: missing screenshot or layout")
            continue
        image = read_png(png)
        if (image[0], image[1]) != size:
            failures.append(f"{name}: {image[0]}x{image[1]}, expected {size[0]}x{size[1]}")
            continue
        images[name], layouts[name] = image, json.loads(layout.read_text(encoding="utf-8"))
        # The SafeHouse logo (the emblem, its gold cane) and name sit at the top of the rail ("brand" in the layout).
        width, height = image[0], image[1]
        brand = next((e for e in layouts[name]["entries"] if e["label"] == "brand"), None)
        if not brand or brand["y"] > height * 0.1 or brand["x"] + brand["w"] > width * 0.25:
            failures.append(f"{name}: no SafeHouse logo at the top of the rail ({brand})")
        else:
            box = (int(brand["x"]), int(brand["y"]), int(brand["x"] + brand["h"]), int(brand["y"] + brand["h"]))  # the emblem
            logo = list(pixels(image, box))
            gold = sum(1 for rgb in logo if near(rgb, LOGO_GOLD, 50))
            if gold < 0.03 * len(logo):
                failures.append(f"{name}: no SafeHouse logo in the rail ({gold} logo px)")
    # Footer hints differ per controller family; keyboard shows keycaps.
    if not quick:
        footers = {}
        for family in families:
            name = f"game-sly1-{family}"
            if name in images:
                w, h = images[name][0], images[name][1]
                footers[family] = crop_hash(images[name], (w // 2, h - 44, w, h))
        pads = {footers[f] for f in PAD_FAMILIES if f in footers}
        if len(pads) < len(PAD_FAMILIES) - 1:
            failures.append(f"footer hints look the same for several controller families ({len(pads)} distinct)")
        if "keyboard" in footers and footers["keyboard"] in pads:
            failures.append("keyboard footer matches a controller footer")
    for name, (game, view) in STATES.items():
        layout = layouts.get(name)
        if layout and ((game and layout.get("game") != game) or layout.get("view") != view):
            failures.append(f"{name}: shows {layout.get('game')}/{layout.get('view')}, expected {game}/{view}")
    for name, (present, absent) in LABELS.items():
        if name not in layouts:
            continue
        labels = {e["label"] for e in layouts[name]["entries"]}
        if present - labels:
            failures.append(f"{name}: missing {sorted(present - labels)}")
        if absent & labels:
            failures.append(f"{name}: unexpected {sorted(absent & labels)}")
    for name, label in FOCUSED.items():
        layout = layouts.get(name)
        f = layout and layout.get("focus")
        if not layout:
            continue
        if not f:
            failures.append(f"{name}: nothing focused, expected {label!r}")
            continue
        at = [e["label"] for e in layout["entries"] if abs(e["x"] - f["x"]) < 20 and abs(e["y"] - f["y"]) < 4]
        if label not in at:
            failures.append(f"{name}: focus on {at}, expected {label!r}")
    # Pad shots show the focus ring around the focused item.
    for name, layout in layouts.items():
        if layout.get("family") in (None, "keyboard"):
            continue
        f = layout.get("focus")
        if not f:
            failures.append(f"{name}: nothing has pad focus")
            continue
        x0, y0, x1, y1 = int(f["x"]) - 6, int(f["y"]) - 6, int(f["x"] + f["w"]) + 6, int(f["y"] + f["h"]) + 6
        edge = [rgb for rgb in pixels(images[name], (x0, y0, x1, y0 + 8))]
        edge += [rgb for rgb in pixels(images[name], (x0, y1 - 8, x1, y1))]
        ring = sum(1 for rgb in edge if near(rgb, FOCUS, 40))
        if ring < 0.2 * max(1, x1 - x0):
            failures.append(f"{name}: focus ring not visible")
    for failure in failures:
        print("FAIL", failure)
    print(f"{len(images)} screenshots checked, {len(failures)} problems")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
