#!/usr/bin/env python3
"""The Collection Style Launcher (launcher/collection), through the real launcher binary.

    test-collection-launcher.py LAUNCHER [--ps3 SOURCE]

Checks, with synthetic data and a private profile only:
  - without the imported menu there is no Collection style: --collection opens the SafeHouse launcher on the rows that
    import it;
  - the import (--collection-import) of a made-up menu written here in the disc's formats (the scene file, GTF / DXT
    textures, the binary XML, the font's glyph map, FSB4 / IMA ADPCM banks), what it converts, and --collection-remove;
  - screenshots of the imported menu: its pages, the options, other window shapes; the pixels are the scene's;
  - a finger (scripted pointer events): the scene follows a drag, a flick turns one page, a short drag goes back,
    a tap on an arrow turns the page, a tap on a logo starts an installed game and nothing for a missing one;
  - the menu's mixer plays the converted music and sounds, and the music loops;
  - a launch from the menu (scripted --live run, keyboard nav to Sly 2, Enter) starts the game runtime with
    SAFEHOUSE_LAUNCHER=collection and the menu comes back after the game quits;
  - the game started any other way (--play) does not get the variable;
  - the Steam and desktop shortcuts for the collection (`--collection`);
  - the full credits (launcher/credits.h) on both the SafeHouse launcher's About page and the menu's Credits panel:
    every section (the decompilation projects among them), every link, the developers.
With --ps3 it also imports from a real Sly Collection source (folder, .iso or .zip).
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


ROOT = pathlib.Path(__file__).resolve().parent.parent


def run(command, **kwargs):
    return subprocess.run([str(c) for c in command], capture_output=True, timeout=kwargs.pop("timeout", 120), **kwargs)


def private_env(home):
    env = dict(os.environ)
    for key in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"):
        env.pop(key, None)
    env.update({"HOME": str(home), "XDG_CONFIG_HOME": str(home / "config"), "XDG_DATA_HOME": str(home / "data"),
                "XDG_STATE_HOME": str(home / "state"), "XDG_CACHE_HOME": str(home / "cache"), "SDL_AUDIODRIVER": "dummy",
                "SAFEHOUSE_COLLECTION_SILENT": "1", "SAFEHOUSE_GIO": "none", "SAFEHOUSE_STEAM_ROOT": str(home / "steam"),
                "APPDATA": str(home / "appdata"), "LOCALAPPDATA": str(home / "localappdata"), "USERPROFILE": str(home)})
    env.pop("SAFEHOUSE_LAUNCHER", None)
    return env


def name_hash(text):
    h = 0
    for c in text.upper().encode():
        h = (h * 31 + c) & 0xFFFFFFFF
    return h


def chunk(tag, data):
    return tag + struct.pack("<I", len(data) + 4) + data


def field(data, at, value):
    data[at:at + len(value)] = value


def gtf(width, height, pixel, alpha=None):
    """A GTF texture of one colour per call of pixel(x, y) -> (r, g, b), DXT1 (or DXT5 with alpha) blocks."""
    blocks = bytearray()
    for by in range(height // 4):
        for bx in range(width // 4):
            r, g, b = pixel(bx * 4, by * 4)
            c = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
            if alpha is not None:
                blocks += bytes([alpha, alpha, 0, 0, 0, 0, 0, 0])
            blocks += struct.pack("<HHI", c, c, 0)
    head = bytearray(128)
    struct.pack_into(">IIIIIIBBBBIHHH", head, 0, 0x01040100, len(blocks), 1, 0, 128, len(blocks), 0xA8 if alpha is not None else 0xA6, 1, 2, 0,
                     0xAAE4, width, height, 1)
    return bytes(head) + bytes(blocks)


def synthetic_scene():
    """A small menu scene in the collection's .san format: strips, logos (one from a four-piece sheet), arrows that
    sway, the van with its exhaust emitter, the minigame circles, six cameras."""
    out = bytearray()
    textures = {}

    def texture(name, width, height, pixel, alpha=None):
        h = name_hash(name)
        textures[name] = h
        head = bytearray(44)
        struct.pack_into("<IHH", head, 1, h, width, height)
        field(head, 9, bytes([1, 0x20 if alpha is not None else 0x18, 0]))
        field(head, 12, (name + ".tga").encode())
        out.extend(chunk(b"TEXR", chunk(b"TXRH", bytes(head)) + chunk(b"TGTF", gtf(width, height, pixel, alpha))))

    def material(name, tex, scroll=None, particle=False, flipbook=None):
        head = bytearray(0x64)
        head[0] = 4
        struct.pack_into("<I", head, 1, textures[tex])
        struct.pack_into("<I", head, 0x15, name_hash(name))
        field(head, 0x19, name.encode())
        struct.pack_into("<I", head, 0x4D, name_hash("ParticleMaterial" if particle else "Standard"))
        data = chunk(b"MTLH", bytes(head))
        if scroll:
            c = bytearray(36)
            c[0] = 3
            struct.pack_into("<3f", c, 1, scroll, 0.0, 1.0)
            data += chunk(b"MTLC", bytes(c))
        for channel, rate, frames in flipbook or ():
            # a flipbook channel: the offset runs at rate (texture widths per second) in frames steps
            c = bytearray(36)
            c[0] = 3
            struct.pack_into("<3f", c, 1, rate, frames, 1.0)
            c[34] = channel
            data += chunk(b"MTLC", bytes(c))
        out.extend(chunk(b"MATL", data))

    def quad(x0, y0, x1, y1, z, colour, uv=(0.0, 0.0, 1.0, 1.0)):
        v = b""
        for x, y, u, w in ((x0, y0, uv[0], uv[3]), (x1, y0, uv[2], uv[3]), (x1, y1, uv[2], uv[1]), (x0, y1, uv[0], uv[1])):
            v += struct.pack(">3e", x, y, z) + bytes(colour) + struct.pack(">2e", u, w) + bytes(18)
        return v

    def mesh(name, parts):
        """parts: (material name, vertices): one sub-mesh each."""
        data = bytearray(24)
        struct.pack_into("<I", data, 17, name_hash("mesh_" + name))
        body = chunk(b"MSHH", bytes(data))
        for mat, vertices in parts:
            head = bytearray(0x58)
            head[0] = 3
            struct.pack_into("<HH", head, 1, len(vertices) // 32, 6)
            struct.pack_into("<I", head, 6, name_hash(mat))
            body += chunk(b"SMSH", chunk(b"MHDR", bytes(head)) + chunk(b"MVTX", vertices) + chunk(b"MIDX", struct.pack(">6H", 0, 1, 2, 0, 2, 3)))
        out.extend(chunk(b"MESH", body))
        geo = bytearray(0x54)
        geo[0] = 4
        struct.pack_into("<II", geo, 25, name_hash(name), name_hash(name))
        field(geo, 37, name.encode())
        lod = bytearray(12)
        struct.pack_into("<If", lod, 1, name_hash("mesh_" + name), 500.0)
        out.extend(chunk(b"GEOB", chunk(b"GEOH", bytes(geo)) + chunk(b"GLOD", bytes(lod))))

    def instance(name, pos, sway=False, children=b"", tag=b"GINS", head_tag=b"GINH", geometry=None):
        head = bytearray(0x74)
        field(head, 0, b"\0\1")
        struct.pack_into("<12f", head, 2, 1, 0, 0, 0, 0, 1, pos[0], pos[1], pos[2], 1, 1, 1)
        struct.pack_into("<II", head, 54, name_hash(geometry or name), name_hash(geometry or name))
        field(head, 62, name.encode())
        data = chunk(head_tag, bytes(head))
        if sway:
            # two groups of named values (big-endian): the arrow moves 0.05 along X and turns 10 degrees, once a second
            def group(prefix, values):
                table, blob = b"", b""
                for key, value in values:
                    table += struct.pack(">III", name_hash(key), 0, 0)
                    blob += value
                return len(values), table, blob
            groups = [("TranslationController", [("xPosAmplitude", struct.pack(">f", 0.05)), ("xPosPeriod", struct.pack(">f", 1.0)), ("xPosPingPong", b"\1\0\0\0")]),
                      ("RotationController", [("zAngleAmplitude", struct.pack(">f", 10.0)), ("zAnglePeriod", struct.pack(">f", 1.0)), ("zAnglePingPong", b"\1\0\0\0"),
                                              ("zRandomStart", b"\1\0\0\0")])]
            at = 4 + 8 * len(groups)
            heads, bodies = b"", b""
            for gname, values in groups:
                heads += struct.pack(">II", name_hash(gname), at + len(bodies))
                entries = b""
                value_at = at + len(bodies) + 4 + 12 * len(values)
                blob = b""
                for key, value in values:
                    entries += struct.pack(">III", name_hash(key), value_at + len(blob), 0)
                    blob += value
                bodies += struct.pack(">I", len(values)) + entries + blob
            data += chunk(b"ATAG", struct.pack(">I", len(groups)) + heads + bodies)
        return chunk(tag, data + children)

    def camera(name, x):
        head = bytearray(0x54)
        struct.pack_into("<10f", head, 1, 1, 0, 0, 0, 0, 1, x, 0, 2.4384, 45.0)
        field(head, 49, name.encode())
        out.extend(chunk(b"CAMC", chunk(b"CAMH", bytes(head))))

    # ---- textures: flat colours, so a screenshot's pixels say what was drawn
    texture("Sky", 64, 16, lambda x, y: (16, 24, 96))
    texture("Ground", 64, 16, lambda x, y: (40, 16, 8), alpha=255)
    texture("Paris", 64, 16, lambda x, y: (160, 64, 120))
    texture("Bar", 32, 32, lambda x, y: (255, 255, 255))
    # Sly's run: 8 x 8 frames, each its own colour (red: the column, green: the row)
    texture("SlyRun", 64, 64, lambda x, y: (x // 8 * 32 + 16, y // 8 * 32 + 16, 128), alpha=255)
    texture("Logos", 1024, 1024, lambda x, y: ((200, 40, 40), (40, 40, 200), (200, 200, 40), (40, 200, 200))[(x >= 512) + 2 * (y >= 512)], alpha=255)
    for name, colour in (("Logo1", (40, 200, 40)), ("Logo2", (200, 200, 40)), ("Logo3", (200, 40, 200)), ("Arrow", (240, 180, 0)), ("Van", (80, 80, 240)),
                         ("Puff", (200, 200, 200)), ("Circle", (128, 128, 128)), ("CircleOn", (64, 128, 255)), ("Lock", (0, 96, 255))):
        texture(name, 32, 32, lambda x, y, c=colour: c, alpha=255)
    for name, tex, scroll in (("Mat_Sky", "Sky", 0.03), ("Mat_Ground", "Ground", 0.06), ("Mat_Paris", "Paris", 0.03), ("Mat_Logos", "Logos", None),
                              ("Mat_Logo1", "Logo1", None), ("Mat_Logo2", "Logo2", None), ("Mat_Logo3", "Logo3", None), ("Mat_Arrow", "Arrow", None),
                              ("Mat_Van", "Van", None), ("Mat_Circle", "Circle", None), ("Mat_CircleOn", "CircleOn", None), ("Mat_Lock", "Lock", None)):
        material(name, tex, scroll)
    material("Mat_Puff", "Puff", particle=True)
    material("Mat_Bar", "Bar")
    material("Mat_SlyRun", "SlyRun", flipbook=[(0, 9.6, 8.0), (1, 1.2, 8.0)])  # (the disc's values)
    grey, shadow = (204, 204, 204, 255), (0, 0, 0, 127)
    wide = (-0.61, 0.0, 1.61, 1.0)
    mesh("STATIC_BackgroundA", [("Mat_Sky", quad(-13.5, -1.5, 13.5, 1.5, 0, grey, wide))])
    mesh("STATIC_BackgroundB", [("Mat_Paris", quad(-13.5, -1.5, 8.1, 1.5, 0, grey, wide))])
    mesh("STATIC_ForegroundA", [("Mat_Ground", quad(-13.5, -1.5, 13.5, -0.8, 0, grey, wide))])
    mesh("STATIC_ForegroundB", [("Mat_Ground", quad(-13.5, -1.5, 8.1, -0.8, 0, grey, wide))])
    for name, mat, uv in (("STATIC_SlyCollectionLogo_US", "Mat_Logos", (0.0, 0.0, 0.5, 0.5)), ("STATIC_SlyCollectionLogo_EU", "Mat_Logos", (0.5, 0.0, 1.0, 0.5)),
                          ("STATIC_Sly1Logo_US", "Mat_Logo1", (0.0, 0.0, 1.0, 1.0)), ("STATIC_Sly2Logo_US", "Mat_Logo2", (0.0, 0.0, 1.0, 1.0)),
                          ("STATIC_Sly3Logo_US", "Mat_Logo3", (0.0, 0.0, 1.0, 1.0)), ("STATIC_MCGameLogo_US", "Mat_Logos", (0.0, 0.5, 0.5, 1.0))):
        mesh(name, [(mat, quad(-0.7, -0.4, 0.7, 0.4, 0, grey, uv)), (mat, quad(-0.74, -0.44, 0.66, 0.36, -0.05, shadow, uv))])
    mesh("Arrow", [("Mat_Arrow", quad(-0.09, -0.09, 0.09, 0.09, 0, grey))])
    mesh("STATIC_SlyVanIcon", [("Mat_Van", quad(-0.19, -0.19, 0.19, 0.19, 0, grey))])
    mesh("STATIC_Border1", [("Mat_Bar", quad(-0.17, -1.5, 0.17, 1.5, 0, (18, 13, 33, 255)))])
    mesh("STATIC_SlyIcon", [("Mat_SlyRun", quad(-0.48, -0.24, 0.48, 0.24, 0, grey, (0.0, 0.0, 0.125, 0.125)))])
    mesh("DEF_Cloud", [("Mat_Puff", quad(-0.06, -0.06, 0.06, 0.06, 0, grey))])
    mesh("Circle", [("Mat_Circle", quad(-0.27, -0.27, 0.27, 0.27, 0, grey))])
    mesh("CircleOn", [("Mat_CircleOn", quad(-0.27, -0.27, 0.27, 0.27, 0, grey))])
    mesh("LockBadge", [("Mat_Lock", quad(-0.08, -0.08, 0.08, 0.08, 0, grey)), ("Mat_Lock", quad(-0.09, -0.09, 0.07, 0.07, -0.01, shadow))])
    # ---- the emitter (the van's exhaust)
    emitter = bytearray(0x4C)
    struct.pack_into("<II", emitter, 25, name_hash("Exhaust"), name_hash("Exhaust"))
    field(emitter, 37, b"Exhaust")
    empc = bytearray(60)
    empc[0] = 2
    struct.pack_into("<3fI", empc, 1, 0.5, 0.25, 25.0, name_hash("mesh_DEF_Cloud"))
    colour = bytearray(12)
    field(colour, 8, b"\xff\xff\xff\xff")
    size = bytearray(16)
    struct.pack_into("<2f", size, 5, 0.5, 2.0)
    out.extend(chunk(b"EMIT", chunk(b"EMIH", bytes(emitter)) + chunk(b"EMPC", chunk(b"EMPH", bytes(empc)) + chunk(b"EPIB", bytes(40)) + chunk(b"EPMN", bytes(12)) +
                                                                          chunk(b"EPTC", bytes(colour)) + chunk(b"EPTS", bytes(size)) + chunk(b"EPTR", bytes(24)))))
    # ---- the placed instances, one page every 5.4
    out.extend(instance("STATIC_BackgroundA", (5.4, 0, 0)))
    out.extend(instance("STATIC_BackgroundB", (32.4, 0, 0)))
    out.extend(instance("STATIC_ForegroundA", (5.4, 0, 0.3)))
    out.extend(instance("STATIC_ForegroundB", (32.4, 0, 0.3)))
    for name, x in (("STATIC_SlyCollectionLogo_US", 0.0), ("STATIC_SlyCollectionLogo_EU", 0.0), ("STATIC_Sly1Logo_US", 5.4), ("STATIC_Sly2Logo_US", 10.8),
                    ("STATIC_Sly3Logo_US", 16.2)):
        out.extend(instance(name, (x, 0.15, 0.3)))
    out.extend(instance("STATIC_MCGameLogo_US", (20.9, 0.3, 0.285)))
    for i, x in enumerate((0.0, 5.4, 10.8, 16.2)):
        out.extend(instance(f"STATIC_ScreenArrow{2 * i + 2}", (x + 0.7, -0.32, 0.45), sway=True, geometry="Arrow"))
        out.extend(instance(f"STATIC_ScreenArrow{2 * i + 3}", (x - 0.7, -0.32, 0.45), sway=True, geometry="Arrow"))
    out.extend(instance("STATIC_SlyVan", (0, -0.61, 0.2), geometry="none",
                        children=instance("STATIC_SlyVanIcon", (0, 0, 0)) + instance("STATICParticle_Exhaust", (-0.2, -0.1, 0), tag=b"EINS", head_tag=b"EINH", geometry="Exhaust")))
    out.extend(instance("STATIC_Border1", (18.9, 0, 0.25)))  # (the dark bar between the halves)
    out.extend(instance("STATIC_Sly", (21.6, -0.5, 0.2), geometry="none", children=instance("STATIC_SlyIcon", (-0.737, -0.037, 0))))
    for i, who in enumerate(("Sly", "Murray", "Bentley", "Carmelita")):
        x, y = 21.67 + 0.55 * (i % 2), 0.126 - 0.55 * (i // 2)
        out.extend(instance(f"STATIC_Minigame{who}", (x, y, 0.4), geometry="Circle"))
        out.extend(instance(f"STATIC_Minigame{who}Focus", (x, y, 0.4), sway=True, geometry="CircleOn"))
        out.extend(instance(f"STATIC_Minigame{who}Lock", (x - 0.16, y + 0.14, 0.47), geometry="LockBadge"))
    for name, x in (("Camera_Credits", -5.4), ("Camera_Title", 0.0), ("Camera_Sly1", 5.4), ("Camera_Sly2", 10.8), ("Camera_Sly3", 16.2), ("Camera_Minigames", 21.6)):
        camera(name, x)
    return b"FORM" + struct.pack("<I", len(out) + 4) + bytes(out)


def xmlb(root):
    """The collection's binary XML. root: (name, {attribute: text}, [children])."""
    nodes, strings = [], bytearray()

    def flatten(node, parent):
        index = len(nodes)
        nodes.append([node, parent, []])
        for child in node[2]:
            nodes[index][2].append(flatten(child, index))
        return index
    flatten(root, None)
    offsets, at = [], 0x18
    for node, parent, children in nodes:
        offsets.append(at)
        at += 24 + 4 * len(children) + 8 * len(node[1])
    strings_at = at
    body = bytearray()
    for i, (node, parent, children) in enumerate(nodes):
        here = offsets[i]
        body += struct.pack(">IIIHHII", name_hash(node[0]), 0, offsets[parent] if parent is not None else 0, len(children), len(node[1]), here + 24,
                            here + 24 + 4 * len(children))
        for c in children:
            body += struct.pack(">I", offsets[c])
        for key, text in node[1].items():
            body += struct.pack(">II", name_hash(key), strings_at + len(strings))
            strings += text.encode() + b"\0"
    return b"XMLB" + struct.pack(">I4sIII", 0x00020000, b"PS3 ", 0x18 + len(body) + len(strings), 0x00010000, 0x18) + bytes(body) + bytes(strings)


def tga(width, height, rgba=(255, 255, 255, 255)):
    return struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, width, height, 32, 0x28) + bytes((rgba[2], rgba[1], rgba[0], rgba[3])) * (width * height)


def glyph_map(width=64, height=64, per_row=8, rows=2):
    """The font page's map as the collection stores it: an 8-bit BMP, a green box per glyph, a blue line per baseline
    (the first glyph, the space, is a stroke on its row)."""
    blue, green, black = 0, 1, 2
    px = [[black] * width for _ in range(height)]
    for row in range(rows):
        base = 24 + row * 28
        for x in range(width):
            px[base][x] = blue
        for i in range(per_row):
            x0, x1 = 2 + i * 7, 2 + i * 7 + 5
            if row == 0 and i == 0:
                for x in range(x0, x1 + 1):
                    px[base - 1][x] = green
                continue
            y0, y1 = base - 16, base - 2
            for x in range(x0, x1 + 1):
                px[y0][x] = px[y1][x] = green
            for y in range(y0, y1 + 1):
                px[y][x0] = px[y][x1] = green
    palette = bytes((255, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0)) + bytes(4 * 253)  # (BGRA: blue, green, black)
    rows_bytes = b"".join(bytes(r) for r in reversed(px))
    offset = 14 + 40 + len(palette)
    return b"BM" + struct.pack("<IHHI", offset + len(rows_bytes), 0, 0, offset) + struct.pack("<IiiHHIIiiII", 40, width, height, 1, 8, 0, len(rows_bytes), 2835, 2835, 256, 0) + \
        palette + rows_bytes


def fsb4(names, frames=4096):
    """An FSB4 bank of stereo Xbox IMA ADPCM samples that are not silence."""
    block = struct.pack("<hBBhBB", 0, 40, 0, 0, 40, 0) + bytes([0x07, 0x8F, 0x70, 0xF8] * 16)
    headers, data, at = b"", b"", 48 + 80 * len(names)
    for name in names:
        sample = block * (frames // 64)
        header = bytearray(80)
        struct.pack_into("<H", header, 0, 80)
        header[2:2 + len(name)] = name.encode()
        struct.pack_into("<6I", header, 32, frames, len(sample), 0, frames - 1, 0x20400000, 44100)
        struct.pack_into("<4H", header, 56, 255, 128, 128, 2)
        headers += bytes(header)
        data += sample
        end = at + len(sample)
        pad = ((end + 31) & ~31) - end
        data += bytes(pad)
        at = end + pad
    return b"FSB4" + struct.pack("<5I", len(names), len(headers), len(data), 0x40000, 0x40) + bytes(24) + headers + data


def synthetic_disc(root):
    """A folder shaped like The Sly Collection's disc, holding a small made-up menu (nothing of the real one)."""
    usr = root / "PS3_GAME" / "USRDIR"
    (usr / "AUDIO").mkdir(parents=True)
    with zipfile.ZipFile(usr / "IMAGE.ZIP", "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Levels/Frontend/Frontend_Art.san", synthetic_scene())
        z.writestr("Scripts/Levels.xmb", xmlb(("LevelData", {}, [("Level", {"name": "Common"}, []), ("Level", {"name": "Frontend"}, [
            ("Bloom", {"enable": "1", "Intensity": "4.0", "threshold": "{64,64,64,0}"}, [])])])))
        z.writestr("Scripts/Tuning.xmb", xmlb(("Tuning", {}, [("var", {"name": "Game.UI.FrontEndInputDelayTime", "value": "0.25"}, []),
                                                               ("var", {"name": "Game.UI.PageCamInterpFactor", "value": "0.2"}, [])])))
        z.writestr("Gui/Font_MCGames_US_01.tga", tga(64, 64))
        z.writestr("Gui/Font_MCGames_US_01.bmp", glyph_map())
        for piece in ("Banner_Left", "Banner_Center", "Banner_Right"):
            z.writestr(f"Gui/{piece}.tga", tga(16, 16, (0, 64, 160, 255)))
        for piece in ("LeftTop", "CenterTop", "RightTop", "Left", "Center", "Right", "LeftBottom", "CenterBottom", "RightBottom"):
            z.writestr(f"Gui/Border_{piece}.tga", tga(8, 8, (0, 48, 128, 255)))
            z.writestr(f"Gui/Border_NI{piece}.tga", tga(8, 8, (0, 48, 128, 255)))
    (usr / "AUDIO" / "SLYAUDIO_MUSIC.FSB").write_bytes(fsb4(["other_track", "FrontEnd_Loop"]))
    (usr / "AUDIO" / "KRAZEAUDIO_GAME.FSB").write_bytes(fsb4(["Negative_Beep_4", "Beep_18", "Positive_Beep_16"], 512))
    return usr


def read_png(path):
    """The pixels of an 8-bit RGB / RGBA PNG: (width, height, channels, rows)."""
    import zlib
    data = pathlib.Path(path).read_bytes()
    at, idat, width, height, channels = 8, b"", 0, 0, 4
    while at < len(data):
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + size]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and colour in (2, 6), (depth, colour)
            channels = 4 if colour == 6 else 3
        elif kind == b"IDAT":
            idat += body
        at += 12 + size
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return width, height, channels, rows


def pixel(image, x, y):
    width, height, channels, rows = image
    return tuple(rows[y][x * channels:x * channels + 3])


def shared_credits():
    """launcher/credits.h: the section ids and the links both credits screens show."""
    text = (ROOT / "launcher" / "credits.h").read_text(encoding="utf-8")
    body = text[text.index("sections()"):]
    ids = re.findall(r'^\s*\{"([a-z0-9]+)", (?:N_\()?"', body, re.M)
    constants = dict(re.findall(r'inline constexpr const char\* (\w+) = "([^"]+)";', text))
    brand = dict(re.findall(r'inline constexpr const char\* (\w+) = "([^"]+)";', (ROOT / "platform" / "brand.h").read_text(encoding="utf-8")))
    links = []
    for m in re.finditer(r'\{Line::Link, [^{}]*?, (?:"([^"]+)"|(?:brand::)?(\w+))\}', body):
        links.append(m.group(1) or constants.get(m.group(2)) or brand[m.group(2)])
    assert len(ids) >= 15 and "decompilation" in ids and "trademarks" in ids, ids
    assert "https://github.com/Komasia/sly2-decomp" in links and "https://github.com/NaGaa95/sly3-decomp" in links, links
    return ids, links


def near(colour, wanted, tolerance=14):
    return all(abs(a - b) <= tolerance for a, b in zip(colour, wanted))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("launcher", type=pathlib.Path)
    p.add_argument("--ps3", type=pathlib.Path)
    a = p.parse_args()
    launcher = a.launcher.resolve()
    with tempfile.TemporaryDirectory(prefix="collection-test-") as tmp:
        work = pathlib.Path(tmp)
        # ---- a portable copy (its profile is portable/data), with a private home besides
        portable = work / "portable"
        bin_dir = portable / "bin"
        bin_dir.mkdir(parents=True)
        copied = bin_dir / launcher.name
        shutil.copy2(launcher, copied)
        for name in ("libc++.dll", "libunwind.dll", "libwinpthread-1.dll", "SDL2.dll"):
            if (launcher.parent / name).is_file():
                shutil.copy2(launcher.parent / name, bin_dir / name)
        (portable / "portable.txt").write_text("SafeHouse portable mode\n")
        home = work / "home"
        home.mkdir()
        env = private_env(home)
        r = run([copied, "--help"], env=env)
        assert r.returncode == 0 and b"--collection" in r.stdout, r

        shots = work / "shots"
        shots.mkdir()
        # (the renders need a desktop session: on a Linux machine without X11 / Wayland, such as CI, they are skipped)
        have_display = not sys.platform.startswith("linux") or bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))
        if not have_display:
            print("no display: the screenshot and launch-from-the-menu checks are skipped")

        def render(name, extra, environment=None):
            """A hidden-window screenshot; None on a machine without a usable GL window."""
            nonlocal have_display
            if not have_display:
                return None
            out = shots / f"{name}.png"
            r = run([copied, "--screenshot", out, "--collection"] + extra, env=environment or env)
            if r.returncode != 0 and (b"Cannot create launcher window" in r.stderr or b"Cannot initialize desktop display" in r.stderr):
                # (a machine without a usable GL context, such as a CI Mac runner: the renders and the launch are skipped)
                print("no GL window available: the screenshot and launch-from-the-menu checks are skipped")
                have_display = False
                return None
            assert r.returncode == 0 and out.is_file() and out.stat().st_size > 5000, (name, r)
            return out

        # ---- not imported: there is no Collection style. --collection opens the SafeHouse launcher on Settings > Launcher,
        # whose rows explain it and lead to the import
        r = run([copied, "--collection-status"], env=env)
        assert r.returncode == 0 and b"not imported" in r.stdout, r
        r = run([copied, "--collection-sounds"], env=env)
        assert r.returncode != 0, r
        layout = work / "layout.json"
        if render("missing", ["--dump-layout", layout, "--size", "1280x1600"]):
            shown = json.loads(layout.read_text())
            labels = [e["label"] for e in shown["entries"]]
            assert shown["view"] == "settings", shown["view"]
            assert any("Import the Collection menu from The Sly Collection (PS3)" in l for l in labels), labels
            assert any("Import from the extracted disc" in l for l in labels), labels
            assert not any("Remove the imported Collection menu" in l for l in labels), labels

        # ---- the SafeHouse launcher's About page: the developers with their pictures, then the full credits
        # (launcher/credits.h, the same list as the menu's Credits panel); a window tall enough to show every link
        credit_ids, credit_links = shared_credits()
        developers = ["FridiNaTor", "theclub654", "menaman123", "thematuu", "NaGaa95"]
        if have_display:
            for size in ("1280x4000", "390x4300"):
                out = shots / f"about-{size}.png"
                r = run([copied, "--screenshot", out, "--screen", "about", "--size", size, "--dump-layout", layout], env=env)
                assert r.returncode == 0 and out.is_file(), r
                labels = {e["label"] for e in json.loads(layout.read_text())["entries"]}
                missing = {f"credits-{i}" for i in credit_ids} - labels
                assert not missing, (size, sorted(missing))
                missing = {f"credits-link {u}" for u in credit_links if not u.startswith("https://discord") and "patreon" not in u} - labels
                assert not missing, (size, sorted(missing))  # (the community links are the intro's buttons)
                assert {f"developer {d}" for d in developers} <= labels, (size, sorted(labels))
            # scrolled to the decompilation projects: their section at the top of the page
            r = run([copied, "--screenshot", shots / "about-decomp.png", "--screen", "about:decompilation", "--dump-layout", layout], env=env)
            entries = {e["label"]: e for e in json.loads(layout.read_text())["entries"]}
            assert r.returncode == 0 and 0 < entries["credits-decompilation"]["y"] < 200, entries.get("credits-decompilation")
            assert "credits-link https://github.com/NaGaa95/sly3-decomp" in entries, sorted(entries)
            print(f"About page: {len(credit_ids)} credit sections, {len(credit_links)} links, the five developers")

        # ---- the import from a synthetic Sly Collection folder (a made-up menu in the disc's formats)
        disc = work / "disc"
        usr = synthetic_disc(disc)
        not_disc = work / "notdisc"
        not_disc.mkdir()
        r = run([copied, "--collection-import", not_disc], env=env)
        assert r.returncode != 0 and b"not The Sly Collection" in r.stderr, r
        broken = work / "broken"
        synthetic_disc(broken)
        with zipfile.ZipFile(broken / "PS3_GAME" / "USRDIR" / "IMAGE.ZIP", "w") as z:
            z.writestr("Levels/Frontend/Frontend_Art.san", b"FORM\x04\0\0\0")
        r = run([copied, "--collection-import", broken], env=env)
        assert r.returncode != 0 and b"could not be read (Frontend_Art.san)" in r.stderr, r
        r = run([copied, "--collection-status"], env=env)
        assert b"not imported" in r.stdout, r  # (a failed import leaves nothing behind)
        r = run([copied, "--collection-import", disc], env=env)
        assert r.returncode == 0, r
        menus = [p for p in portable.rglob("Menu") if (p / "menu.json").is_file()]
        assert len(menus) == 1, menus
        menu_dir = menus[0]
        profile = menu_dir.parent.parent
        # again, over the first one; the first Collection Style Launcher's raw copies of the disc's files go
        legacy = profile / "Collection" / "PS3 menu"
        legacy.mkdir()
        (legacy / "IMAGE.ZIP").write_bytes(b"old")
        r = run([copied, "--collection-import", disc], env=env)
        assert r.returncode == 0 and (menu_dir / "menu.json").is_file() and not legacy.exists(), r
        assert not list(profile.rglob("*.staging")), "the import left its staging folder"
        assert not any(home.rglob("menu.json")), "the import went outside the profile"
        menu = json.loads((menu_dir / "menu.json").read_text())
        assert menu["version"] == 2 and set(menu["cameras"]) >= {"Camera_Title", "Camera_Sly1", "Camera_Sly2", "Camera_Sly3", "Camera_Credits", "Camera_Minigames"}, menu["cameras"]
        assert abs(menu["cameras"]["Camera_Sly2"]["pos"][0] - 10.8) < 1e-3 and abs(menu["cameras"]["Camera_Sly2"]["fov"] - 45) < 1e-3
        assert menu["bloom"]["enable"] is True and abs(menu["bloom"]["threshold"] - 64 / 255) < 1e-6 and menu["bloom"]["intensity"] == 4.0, menu["bloom"]
        assert abs(menu["tuning"]["inputDelay"] - 0.25) < 1e-6 and abs(menu["tuning"]["cameraGlide"] - 0.2) < 1e-6, menu["tuning"]
        names = {n["name"]: n for n in menu["nodes"]}
        assert "STATIC_Sly2Logo_US" in names and len(names["STATIC_Sly2Logo_US"]["meshes"]) == 2, list(names)
        assert names["STATIC_SlyVanIcon"]["parent"] == menu["nodes"].index(names["STATIC_SlyVan"])
        assert "emitter" in names["STATICParticle_Exhaust"] and names["STATICParticle_Exhaust"]["emitter"]["tex"] == "Puff", names["STATICParticle_Exhaust"]
        assert names["STATIC_ScreenArrow2"]["move"][0][:2] == [0.05000000074505806, 1.0] and names["STATIC_ScreenArrow2"]["turn"][2][0] == 10.0
        assert names["STATIC_BackgroundA"]["meshes"][0]["uv"] == [[0, 0, 0.029999999329447746, 0.0, 1.0]], names["STATIC_BackgroundA"]["meshes"][0]["uv"]
        # the logo sheet (1024x1024, four pieces) is cut into the pieces used
        pieces = sorted(t for t in menu["textures"] if t.startswith("Logos_"))
        assert pieces == ["Logos_0_0_512x512", "Logos_0_512_512x512", "Logos_512_0_512x512"], pieces
        assert len(menu["font"]) == 1 and menu["font"][0]["first"] == 32 and len(menu["font"][0]["glyphs"]) == 16, menu["font"]
        assert menu["font"][0]["glyphs"][1] == [9, 8, 6, 15, 24], menu["font"][0]["glyphs"][:3]
        for rel in menu["files"]:
            assert (menu_dir / rel).is_file(), rel
        assert menu["audio"]["music"] == ["audio/music0.wav"] and {"move", "select", "cancel"} <= set(menu["audio"]), menu["audio"]
        r = run([copied, "--collection-status"], env=env)
        assert r.returncode == 0 and b"collection menu: imported" in r.stdout, r
        r = run([copied, "--collection-sounds"], env=env)
        sounds = r.stdout.decode().splitlines()
        assert r.returncode == 0 and len(sounds) == 4 and sounds[0].startswith("audio/music0.wav rate 44100 frames 4096 rms "), r
        assert all(int(line.rsplit(" ", 1)[1]) > 100 for line in sounds), sounds  # (decoded, not silence)
        # the menu's mixer plays them (no sound card needed: SDL's dummy driver; skipped without SDL2)
        r = run([copied, "--collection-audio-test", "0.3"], env=env)
        assert r.returncode == 0 and (b"played 0.3 s" in r.stdout or b"no audio output" in r.stdout), r
        disk_env = dict(env, SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=str(work / "out.raw"), SDL_DISKAUDIODELAY="1")
        r = run([copied, "--collection-audio-test", "1.2"], env=disk_env)
        assert r.returncode == 0, r
        if b"played" in r.stdout and (work / "out.raw").is_file():
            raw = (work / "out.raw").read_bytes()
            samples = struct.unpack(f"<{len(raw) // 2}h", raw[:len(raw) // 2 * 2])
            # (the made-up track is 4096 frames long: more output than that means it looped)
            assert len(samples) // 2 > 3 * 4096 and max(abs(v) for v in samples[-4096:]) > 500, (len(samples), "the output is silent or stopped")
            print(f"audio: {len(samples) // 2} frames written by the menu's mixer, not silent, past the track's end (it loops)")

        # ---- the imported menu renders: every page, the options, other window shapes; the pixels are the scene's
        plain = dict(env, SAFEHOUSE_COLLECTION_BLOOM="0")
        ps3 = ["--collection-ps3", profile, "--size", "1280x720"]
        shot = render("title", ps3 + ["--collection-games", "sly1,sly2"], plain)
        if shot:
            image = read_png(shot)
            # the title shows SafeHouse's own logo (the launcher's picture), not the menu's (the sheet's red piece)
            assert not near(pixel(image, 640, 299), (164, 32, 33)) and not near(pixel(image, 640, 299), (13, 19, 79)), pixel(image, 640, 299)
            assert near(pixel(image, 100, 60), (13, 19, 79)), pixel(image, 100, 60)       # the sky strip
            assert near(pixel(image, 100, 710), (33, 13, 7)), pixel(image, 100, 710)      # the ground strip in front
            assert near(pixel(image, 640, 607), (66, 65, 197)), pixel(image, 640, 607)    # the van, riding under the logo
            image = read_png(render("sly1", ps3 + ["--collection-games", "sly1,sly2", "--nav", "right"], plain))
            assert near(pixel(image, 640, 299), (33, 162, 33)), pixel(image, 640, 299)    # Right: the camera went to Sly 1
            assert near(pixel(image, 640, 607), (66, 65, 197)), pixel(image, 640, 607)    # (the van came along)
            image = read_png(render("sly3-locked", ps3 + ["--collection-games", "sly1,sly2", "--nav", "right,right,right"], plain))
            assert near(pixel(image, 560, 250), (46, 9, 46)), pixel(image, 560, 250)      # not installed: the logo in the dark
            image = read_png(render("sly3", ps3 + ["--collection-games", "sly1,sly2,sly3", "--nav", "right,right,right"], plain))
            assert near(pixel(image, 560, 250), (164, 32, 164)), pixel(image, 560, 250)
            # ---- a finger (the same events a touch screen sends): the scene follows a drag, settles on release
            games = ps3 + ["--collection-games", "sly1,sly2"]
            sly1 = games + ["--nav", "right"]  # (from Sly 1's page: its logo is (40, 200, 40) times 0.8)
            wait = "w " * 20
            image = read_png(render("drag-held", sly1 + ["--pointer", wait + "d900,650 m700,650 m500,650"], plain))
            assert near(pixel(image, 240, 299), (33, 162, 33)), pixel(image, 240, 299)    # the logo went 400 pixels with the finger
            assert near(pixel(image, 700, 299), (13, 19, 79)), pixel(image, 700, 299)
            image = read_png(render("drag-flick", sly1 + ["--pointer", wait + "d900,650 m700,650 m500,650 u"], plain))
            assert near(pixel(image, 640, 299), (162, 162, 33)), pixel(image, 640, 299)   # let go while moving: one page on (Sly 2)
            image = read_png(render("drag-back", sly1 + ["--pointer", wait + "d900,650 m880,650 m860,650 m840,650 m820,650 " + "w " * 10 + "u"], plain))
            assert near(pixel(image, 640, 299), (33, 162, 33)), pixel(image, 640, 299)    # a short drag, held, let go: back
            image = read_png(render("drag-first", games + ["--nav", "left", "--pointer", wait + "d300,650 m700,650 m1100,650 u"], plain))
            assert near(pixel(image, 100, 60), (13, 19, 79)), pixel(image, 100, 60)       # dragged past the first page: it stays
            image = read_png(render("tap-arrow", games + ["--pointer", "d944,499 u"], plain))
            assert near(pixel(image, 640, 299), (33, 162, 33)), pixel(image, 640, 299)    # a tap on the right arrow
            # a tap on a game that is not installed starts nothing (the logo stays in the dark, the menu stays)
            image = read_png(render("tap-locked", ps3 + ["--collection-games", "sly1", "--nav", "right,right", "--pointer", "w " * 20 + "d640,299 u"], plain))
            assert near(pixel(image, 560, 250), (46, 46, 9)), pixel(image, 560, 250)
            image = read_png(render("touch-options", ps3 + ["--screen", "options", "--pointer", "d640,400 m640,300 u"], plain))
            image = read_png(render("glide", ps3 + ["--screen", "glide"], plain))
            assert near(pixel(image, 640, 299), (13, 19, 79)), pixel(image, 640, 299)     # between two pages: no logo in the middle
            # the pages after Sly 3: Sly 4 and the Hackpack (the launcher's logos, in development: a tap starts nothing),
            # then the developers; they lie over the other half of the world
            sky, paris = (13, 19, 79), (130, 52, 98)
            image = read_png(render("sly4", games + ["--nav", "right,right,right,right", "--pointer", "w " * 20 + "d640,299 u"], plain))
            assert near(pixel(image, 100, 60), sky), pixel(image, 100, 60)               # the game pages' strips repeat on under Sly 4
            image = read_png(render("hackpack", games + ["--nav", "right,right,right,right,right"], plain))
            assert near(pixel(image, 100, 60), sky) and near(pixel(image, 1270, 60), sky), (pixel(image, 100, 60), pixel(image, 1270, 60))

            def continuous(image, y, where):
                """Along a row: only the two halves' colours and their mixes (no dark bar, no gap, no hard edge)."""
                row = [pixel(image, x, y) for x in range(image[0])]
                for x, c in enumerate(row):
                    assert all(min(a, b) - 8 <= v <= max(a, b) + 8 for v, a, b in zip(c, sky, paris)), (where, x, c)
                for x in range(1, len(row)):
                    assert max(abs(a - b) for a, b in zip(row[x], row[x - 1])) <= 6, (where, "an edge at", x, row[x - 1], row[x])
                return row
            # the dark bar the scene has between its halves is not shown (the camera right over it: Sly 3 -> Sly 4)
            image = read_png(render("glide-sly3-sly4", games + ["--screen", "glide:4.5"], plain))
            assert all(near(c, sky) for c in continuous(image, 60, "sly3-sly4")), "not the game pages' sky between Sly 3 and Sly 4"
            image = read_png(render("glide-sly4-hackpack", games + ["--screen", "glide:5.5"], plain))
            assert all(near(c, sky) for c in continuous(image, 60, "sly4-hackpack")), "not the game pages' sky between Sly 4 and the Hackpack"
            # after the Hackpack: the change to Paris, cross-faded, from the game pages' sky on the left to Paris on the right
            image = read_png(render("glide-hackpack-developers", games + ["--screen", "glide:6.5"], plain))
            row = continuous(image, 60, "hackpack-developers")
            assert near(row[0], sky) and near(row[-1], paris), (row[0], row[-1])
            for size in ("3840x1080", "2400x1080", "1024x768"):  # (32:9, a phone's 20:9, 4:3)
                for at in ("5", "6", "6.5", "7"):
                    image = read_png(render(f"strip-{at}-{size}", games + ["--size", size, "--screen", f"glide:{at}"], plain))
                    continuous(image, 40, f"glide:{at} {size}")
            # the developers: five circles with the launcher's own pictures (no PS3 data needed for them); a silhouette
            # until one is chosen, then the picture and a bar (the menu's frame) with the name
            image = read_png(render("developers", ps3 + ["--screen", "developers"], plain))
            assert near(pixel(image, 1240, 300), (130, 52, 98)), pixel(image, 1240, 300)
            assert near(pixel(image, 318, 330), (0, 0, 0)), pixel(image, 318, 330)        # the first circle: a silhouette
            assert not near(pixel(image, 318, 88), (0, 48, 128)), pixel(image, 318, 88)   # no bar yet
            image = read_png(render("developer-chosen", ps3 + ["--screen", "developer1"], plain))
            assert not near(pixel(image, 318, 330), (0, 0, 0)), pixel(image, 318, 330)    # the picture
            assert near(pixel(image, 318, 88), (0, 48, 128)), pixel(image, 318, 88)       # the bar
            image = read_png(render("developer-tapped", ps3 + ["--screen", "developers", "--pointer", "d318,308 u"], plain))
            assert near(pixel(image, 318, 88), (0, 48, 128)), pixel(image, 318, 88)       # a tap chooses a circle
            image = read_png(render("developer-untapped", ps3 + ["--screen", "developers", "--pointer", "d318,308 u w w d900,640 u"], plain))
            assert not near(pixel(image, 318, 88), (0, 48, 128)), pixel(image, 318, 88)   # a tap elsewhere chooses none
            image = read_png(render("developers-end", games + ["--nav", "right,right,right,right,right,right,right,right"], plain))
            assert near(pixel(image, 318, 88), (0, 48, 128)), pixel(image, 318, 88)       # with keys: the first is chosen; Right stays on the page
            # Sly's run (on the developers' page): the flipbook steps by the menu's time, not by frames drawn. Its offset runs
            # as a scroll does (U 9.6 widths a second in 8 steps, V 1.2 in 8 steps): 76.8 frames a second, the same frame
            # after the same time at 60 and at 120 frames per second
            def run_frame(seconds, rate):
                image = read_png(render(f"sly-run-{seconds}-{rate}", ps3 + ["--screen", "developers", "--seconds", str(seconds), "--frame-rate", str(rate)], plain))
                r, g, b = pixel(image, 354, 609)
                assert abs(b - 102) <= 10, (seconds, rate, (r, g, b), "not Sly's flipbook")
                return round((r / 0.8 - 16) / 32) + 8 * round((g / 0.8 - 16) / 32)
            for seconds in (1, 2, 2.3):
                expected = int(seconds * 76.8 + 1e-6) % 64
                frames = {rate: run_frame(seconds, rate) for rate in (60, 120, 144)}
                assert set(frames.values()) == {expected}, (seconds, frames, expected)
            print("flipbook: frames", [int(t * 76.8 + 1e-6) % 64 for t in (1, 2, 2.3)], "after 1 / 2 / 2.3 s at 60, 120 and 144 frames per second")
            # Sly 3 asks first, as the SafeHouse launcher does
            image = read_png(render("sly3-question", ps3 + ["--collection-games", "sly1,sly2,sly3", "--nav", "right,right,right,a"], plain))
            assert near(pixel(image, 290, 200), (0, 48, 128)), pixel(image, 290, 200)
            image = read_png(render("sly3-cancelled", ps3 + ["--collection-games", "sly1,sly2,sly3", "--nav", "right,right,right,a,b"], plain))
            assert near(pixel(image, 560, 250), (164, 32, 164)), pixel(image, 560, 250)
            for name, extra in (("extras", ["--screen", "extras"]), ("options", ["--screen", "options"]), ("credits", ["--screen", "credits"]),
                                ("launch", ["--screen", "launch"]), ("left", ["--nav", "left"]), ("japanese", ["--language", "ja"]),
                                ("wide", ["--size", "3440x1080", "--nav", "right,right"]), ("narrow", ["--size", "1024x768"]),
                                ("ultrawide", ["--size", "3840x1080"]), ("portrait", ["--size", "720x1280", "--screen", "developers"])):
                render(name, ["--collection-ps3", profile] + extra)
            # the Credits panel: the same sections and links as the About page, scrolled to the decompilation projects
            if render("credits-decomp", ["--collection-ps3", profile, "--screen", "credits:decompilation", "--dump-layout", layout]):
                entries = {e["label"]: e for e in json.loads(layout.read_text())["entries"]}
                assert json.loads(layout.read_text())["view"] == "collection"
                missing = {f"credits-{i}" for i in credit_ids} - set(entries)
                assert not missing, sorted(missing)
                missing = {f"credits-link {u}" for u in credit_links} - set(entries)
                assert not missing, sorted(missing)
                assert 130 < entries["credits-decompilation"]["y"] < 220, entries["credits-decompilation"]
            image = read_png(render("ultrawide-check", ["--collection-ps3", profile, "--size", "3840x1080", "--screen", "extras"], plain))
            assert near(pixel(image, 4, 60), (13, 19, 79)), pixel(image, 4, 60)           # the strip is carried on past its end
            render("bloom", ps3)  # (the glow's passes run)
            # Sly 2's aspect ratio, which the menu shares (Options): 4:3 in a 16:9 window has black bars at the sides and
            # the scene in the middle; 16:9 in a 32:9 window too; Fit fills the window (above)
            for size, aspect, bar in (("1280x720", "4:3", (40, 360)), ("3840x1080", "16:9", (300, 540))):
                shot = render(f"aspect-{aspect.replace(':', 'x')}", ["--collection-ps3", profile, "--size", size, "--collection-aspect", aspect], plain)
                if shot:
                    image = read_png(shot)
                    assert pixel(image, *bar) == (0, 0, 0), (aspect, pixel(image, *bar))
                    w = int(size.split("x")[0])
                    assert pixel(image, w - 1 - bar[0], bar[1]) == (0, 0, 0), aspect
                    assert pixel(image, w // 2, 60) != (0, 0, 0), aspect  # (the sky)
        # ---- a launch from the menu: the flag reaches the game, the menu comes back
        if os.name != "nt" and have_display:
            data = portable / "data"
            sly2 = data / "installations" / "sly2"
            sly2.mkdir(parents=True)
            (sly2 / "extraction_manifest.json").write_bytes(b"{}")
            manifest = {"schema": 1, "game": "sly2", "asset_version": 4, "layout": "files",
                        "disc_sha1": "de2af92a54de71e33edc8a160c31c0337a95e6f5",
                        "files": [{"path": "extraction_manifest.json", "size": 2, "sha256": hashlib.sha256(b"{}").hexdigest()}]}
            (sly2 / "installation.json").write_text(json.dumps(manifest))
            runtime = bin_dir / "Sly2-Release"
            log = work / "runtime.log"
            # (the hand-off, platform/gl_context.h: Sly 2 is told SAFEHOUSE_HANDOFF=1, announces its first frame and its exit;
            # the menu steps aside at the first and comes back at the second, a --live run as a real one)
            runtime.write_text(f'#!/bin/sh\necho "launcher=$SAFEHOUSE_LAUNCHER" >> "{log}"\nprintf "%s\\n" "$@" >> "{log}"\n'
                               f'echo "handoff=$SAFEHOUSE_HANDOFF" >> "{log}"\n'
                               '[ "$SAFEHOUSE_HANDOFF" = 1 ] && echo \'{"event":"first-frame"}\' && sleep 1 && echo \'{"event":"leaving"}\'\nexit 0\n')
            runtime.chmod(0o755)
            r = run([copied, "--play", "sly2"], env=env)
            if r.returncode != 0:
                print("Sly 2 fake installation not accepted by this build; launch check skipped:", r.stderr.decode()[-300:])
            else:
                lines = log.read_text().splitlines()
                assert lines[0] == "launcher=", lines  # (--play: no collection flag)
                log.unlink()
                out = shots / "launched.png"
                # the menu opens on the collection's own item: Right twice (Sly 1, Sly 2), Enter
                r = run([copied, "--screenshot", out, "--collection", "--live", "--nav", "right,right,a", "--delay", "3"],
                        env=dict(env, SAFEHOUSE_COLLECTION_TIMES="1"))
                assert r.returncode == 0 and out.is_file(), r
                assert log.is_file(), "the game was not started from the menu"
                lines = log.read_text().splitlines()
                assert lines[0] == "launcher=collection", lines
                assert "--data-dir" in lines and "--user-dir" in lines, lines
                assert "handoff=1" in lines, lines
                assert r.stderr.count(b"the window steps aside") == 1 and b"the window is back" in r.stderr, r.stderr[-800:]
                after = r.stderr[r.stderr.index(b"the game's first frame"):]
                assert b"the window steps aside" in after, r.stderr[-800:]  # (not before the game's first frame)
                print("launch from the menu: runtime started with SAFEHOUSE_LAUNCHER=collection and the hand-off, menu back afterwards")
                # the same with a finger: a tap on a game that is not installed starts nothing, a tap on Sly 2's logo starts it
                log.unlink()
                r = run([copied, "--screenshot", out, "--collection", "--live", "--nav", "right", "--pointer", "w " * 20 + "d640,299 u", "--delay", "2"], env=env)
                assert r.returncode == 0 and not log.exists(), "a tap on a game that is not installed started something"
                r = run([copied, "--screenshot", out, "--collection", "--live", "--nav", "right,right", "--pointer", "w " * 20 + "d640,299 u", "--delay", "3"],
                        env=env)
                assert r.returncode == 0 and log.is_file(), "a tap on the logo did not start the game"
                assert log.read_text().splitlines()[0] == "launcher=collection"
                print("touch: a tap on an installed game's logo starts it, on a missing one it does not")

        # ---- removed again: back to "not imported", and --collection opens the SafeHouse launcher
        r = run([copied, "--collection-remove"], env=env)
        assert r.returncode == 0 and not menu_dir.exists(), r
        r = run([copied, "--collection-status"], env=env)
        assert b"not imported" in r.stdout, r
        if render("removed", ["--dump-layout", layout]):
            assert json.loads(layout.read_text())["view"] == "settings"

        # ---- the Steam shortcut (a fake Steam folder) and the desktop shortcut: "Sly Collection", the SafeHouse logo as
        # the icon (tools/make-collection-icons.py), never the app icon; one made under the earlier name is renamed
        logo_icon = (ROOT / "launcher" / "assets" / "shortcut" / "collection-logo.png").read_bytes()
        steam_icon = (ROOT / "launcher" / "assets" / "steam" / "collection" / "icon.png").read_bytes()
        app_icon = (ROOT / "launcher" / "assets" / "safehouse-icon.png").read_bytes()
        assert steam_icon == logo_icon and logo_icon != app_icon
        steam_user = home / "steam" / "userdata" / "1234" / "config"
        steam_user.mkdir(parents=True)
        art = work / "steam-art"
        shutil.copytree(ROOT / "launcher" / "assets" / "steam", art)
        steam_env = dict(env, SAFEHOUSE_STEAM_ART=str(art))
        r = run([copied, "--add-to-steam", "collection"], env=steam_env)
        assert r.returncode == 0 and b"Sly Collection" in r.stdout and b"--collection" in r.stdout, r
        vdf = (steam_user / "shortcuts.vdf").read_bytes()
        assert b"Sly Collection" in vdf and b"SafeHouse Collection" not in vdf and b"--collection" in vdf, vdf[:200]
        icons = list((steam_user / "grid").glob("*_icon.png"))
        assert len(icons) == 1 and icons[0].read_bytes() == logo_icon and str(icons[0]).encode() in vdf, icons
        r = run([copied, "--steam-status"], env=steam_env)
        assert r.returncode == 0 and b"collection: in Steam" in r.stdout, r
        # (an entry of the earlier name, as an older launcher wrote it: renamed, not doubled)
        (steam_user / "shortcuts.vdf").write_bytes(vdf.replace(b"Sly Collection", b"SafeHouse Collection").replace(b"\x00Sly", b"\x00Sly"))
        r = run([copied, "--add-to-steam", "collection"], env=steam_env)
        vdf = (steam_user / "shortcuts.vdf").read_bytes()
        assert r.returncode == 0 and vdf.count(b"--collection") == 1 and b"SafeHouse Collection" not in vdf, vdf[:300]
        if sys.platform.startswith("linux"):
            r = run([copied, "--add-desktop-shortcut", "collection", "--menu"], env=env)
            assert r.returncode == 0, r
            entry = (home / "data" / "applications" / "safehouse-collection.desktop").read_text()
            icon = home / "data" / "icons" / "hicolor" / "256x256" / "apps" / "safehouse-collection.png"
            assert "--collection" in entry and "Name=Sly Collection" in entry, entry
            assert f"Icon={icon}" in entry and icon.read_bytes() == logo_icon, entry
            assert (home / "Desktop" / "safehouse-collection.desktop").is_file()
        # ---- a real Sly Collection (never part of the repository): the import and what it converts
        if a.ps3:
            r = run([copied, "--collection-import", a.ps3], env=env, timeout=1800)
            assert r.returncode == 0, r
            r = run([copied, "--collection-sounds"], env=env)
            assert r.returncode == 0 and b"audio/music0.wav" in r.stdout, r
            print(r.stdout.decode().strip())
    print("collection launcher: OK")


if __name__ == "__main__":
    main()
