#!/usr/bin/env python3
"""Subsets the Noto fonts the launcher merges for letters Atkinson Hyperlegible lacks (launcher/ui/theme.h).

  make-launcher-l10n-fonts.py NOTO_DIR NOTO_CJK_DIR

NOTO_DIR holds NotoSans-Regular.ttf / NotoSans-Bold.ttf (Cyrillic and other letters), NOTO_CJK_DIR holds
NotoSansCJK-Regular.ttc / NotoSansCJK-Bold.ttc (the Japanese face is taken). Writes launcher/assets/l10n/
NotoSans-{Regular,Bold}.subset.ttf and NotoSansCJKjp-{Regular,Bold}.subset.otf with every character launcher/lang/*.json
prints that Atkinson Hyperlegible has not, plus the whole kana and Cyrillic blocks. Run it again after adding or
changing translations (scripts/test-launcher-l10n.py / safehouse --l10n-check say when a glyph is missing).
Needs fontTools (pip install fonttools). All fonts are SIL Open Font License 1.1 (launcher/assets/l10n/OFL.txt).
"""
import json
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTCollection, TTFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "launcher" / "assets" / "l10n"


def used_codepoints():
    cps = set()
    for path in (ROOT / "launcher" / "lang").glob("*.json"):
        data = json.loads(path.read_text(encoding="utf-8"))
        cps.update(ord(c) for c in data["language"]["name"])
        for value in data["strings"].values():
            cps.update(ord(c) for c in value)
    return cps


def atkinson():
    have = set()
    for weight in ("Regular", "Bold"):
        have.update(TTFont(ROOT / "launcher" / "assets" / f"AtkinsonHyperlegible-{weight}.ttf").getBestCmap())
    return have


def write(font, unicodes, target):
    options = subset.Options()
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.notdef_outline = True
    options.hinting = False
    sub = subset.Subsetter(options)
    sub.populate(unicodes=sorted(unicodes))
    sub.subset(font)
    font.save(target)
    print(f"{target.relative_to(ROOT)}: {len(font.getBestCmap())} glyphs, {target.stat().st_size // 1024} KiB")


def main():
    noto, cjk = Path(sys.argv[1]), Path(sys.argv[2])
    OUT.mkdir(parents=True, exist_ok=True)
    wanted = {c for c in used_codepoints() if c >= 0x80} - atkinson()
    wanted |= set(range(0x3040, 0x3100)) | set(range(0x30A0, 0x3100)) | set(range(0x0400, 0x0460))
    for weight in ("Regular", "Bold"):
        write(TTFont(noto / f"NotoSans-{weight}.ttf"), wanted, OUT / f"NotoSans-{weight}.subset.ttf")
        collection = TTCollection(cjk / f"NotoSansCJK-{weight}.ttc")
        face = next(f for f in collection.fonts if "Noto Sans CJK JP" in str(f["name"].getDebugName(1)))
        write(face, wanted, OUT / f"NotoSansCJKjp-{weight}.subset.otf")


if __name__ == "__main__":
    main()
