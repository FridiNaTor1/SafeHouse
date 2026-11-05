#!/usr/bin/env python3
"""Checks Sly 1's port UI text (Sly1-Release/porttext.cpp) against the fonts it is drawn with.

usage: sly1porttextcheck.py [porttext.cpp] [--pal FONTS.txt] [--jp FONTS.txt] [--kr FONTS.txt]

- every row has 7 strings (English, French, German, Spanish, Italian, Japanese, Korean: porttext.h);
- with --pal / --jp / --kr: every character of the French .. Italian / Japanese / Korean strings is a glyph of that
  disc's text font. A Korean syllable needs its jamo (the Korean fonts hold only the conjoining jamo, font.cpp).
  FONTS.txt is the "font glyphs" lines a run prints with CANE_FONTLIST=1 (the text font is the one with the most
  glyphs), e.g.
    CANE_FONTLIST=1 Sly1-Release --data-dir <installation> --user-dir <private profile> \\
      --smoke-level "A Stealthy Approach" --smoke-test-seconds 20 | grep '^font glyphs' > jpfonts.txt
  The "&2X&." runs are the icon font's and are not checked, nor are printf fields (%d, %s, %.2f).
"""
import re
import sys
from pathlib import Path

LANGS = ["English", "French", "German", "Spanish", "Italian", "Japanese", "Korean"]


def unescape(s):
    return bytes(s, "utf8").decode("unicode_escape").encode("latin1").decode("utf8")


def rows(src):
    body = src[src.index("s_arow[] = {"):src.index("// clang-format on")]
    for m in re.finditer(r"\{(PTX_\w+),\s*\{(.*?)\}\}", body, re.S):
        yield m.group(1), [unescape(s) for s in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))]


def font_glyphs(path):
    best = set()
    for line in Path(path).read_text(encoding="utf8").splitlines():
        glyphs = set(c for c, _ in re.findall(r" (.)\((\d+)\)", line))
        if len(glyphs) > len(best):
            best = glyphs
    return best


def hangul_jamo(c):
    s = ord(c) - 0xAC00
    if not 0 <= s < 11172:
        return None
    jamo = [chr(0x1100 + s // 588), chr(0x1161 + (s % 588) // 28)]
    if s % 28:
        jamo.append(chr(0x11A7 + s % 28))
    return jamo


def missing(text, glyphs):
    text = re.sub(r"&2.&\.", "", text)
    text = re.sub(r"%(\.\d+)?[dsf]|%%", "", text)
    out = set()
    for c in text:
        if c == " ":
            continue
        if c in glyphs:
            continue
        jamo = hangul_jamo(c)
        if jamo and all(j in glyphs for j in jamo):
            continue
        out.add(c)
    return sorted(out)


def main():
    args = sys.argv[1:]
    fonts = {}
    for opt, cols in (("--pal", [1, 2, 3, 4]), ("--jp", [5]), ("--kr", [6])):
        if opt in args:
            i = args.index(opt)
            glyphs = font_glyphs(args[i + 1])
            print(f"{opt}: {len(glyphs)} glyphs")
            for col in cols:
                fonts[col] = glyphs
            del args[i:i + 2]
    path = Path(args[0]) if args else Path(__file__).resolve().parent.parent / "Sly1-Release" / "porttext.cpp"
    bad = n = 0
    for ptx, strs in rows(path.read_text(encoding="utf8")):
        n += 1
        if len(strs) != len(LANGS):
            print(f"{ptx}: {len(strs)} strings")
            bad += 1
            continue
        for col, glyphs in fonts.items():
            miss = missing(strs[col], glyphs)
            if miss:
                print(f"{ptx} {LANGS[col]}: no glyph for {' '.join(miss)} in {strs[col]}")
                bad += 1
    print(f"{n} rows, {bad} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
