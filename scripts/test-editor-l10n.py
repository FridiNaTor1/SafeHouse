#!/usr/bin/env python3
"""The 3D map editor's strings in the launcher's languages (editor/lang/<code>.json; editor/game/editor_lang.inc).

  test-editor-l10n.py [--keys OUT.json]

Collects every string the editor translates: the T("...") / TL("...", ...) literals of editor/game and
Sly2-Release/editor*.cpp, and the tables whose entries go through T() at the drawing (the build palette, the object
types and vis groups, the players, the task states, the brush shapes). Each language must translate every one (same
leading / trailing spaces), keep the "%" format specifiers, and have no key the editor no longer uses; with fontTools
installed, every character must be in Atkinson Hyperlegible or the Noto subsets (launcher/assets/l10n). Exit 1 on a problem.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def keys():
    src = ""
    files = sorted(list((ROOT / "editor" / "game").glob("*.inc")) + list((ROOT / "editor" / "game").glob("*.h"))
                   + list((ROOT / "Sly2-Release").glob("editor*.cpp")))
    for f in files:
        src += f.read_text(encoding="utf-8")
    out, seen = [], set()

    def add(s):
        if s and s not in seen and not s.startswith("##"):
            seen.add(s)
            out.append(s)

    # (adjacent literals are one string: T("a\n" "b"))
    for m in re.finditer(r'\bTL?\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', src):
        k = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
        add(bytes(k, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8") if "\\" in k else k)
    b = (ROOT / "editor" / "game" / "editor_build.inc").read_text(encoding="utf-8")
    for tbl in ("s_abuildTraversal", "s_abuildInteract"):
        i = b.index(tbl + "[] = {")
        j = b.index("};", i)
        for m in re.finditer(r'\{"([^"]*)", 0x[0-9A-F]+, (?:true|false), "([^"]*)", "([^"]*)"\}', b[i:j]):
            for g in m.groups():
                add(g)
    for m in re.finditer(r'\{0x[0-9]+, "([^"]*)"\}|\{[0-9], "([^"]*)"\}', b):
        for g in m.groups():
            if g:
                add(g)
    t = (ROOT / "editor" / "game" / "editor_types.inc").read_text(encoding="utf-8")
    if (ROOT / "editor" / "game" / "editor_objtypes.h").exists():   # (the type tables, shared with the debug overlays)
        t += (ROOT / "editor" / "game" / "editor_objtypes.h").read_text(encoding="utf-8")
    for m in re.finditer(r'\{"([^"]*)", "([^"]*)", VG_', t):
        add(m.group(1))
        add(m.group(2))
    i = t.index("char* SzVisGroup")
    for m in re.finditer(r'return "([^"]*)"', t[i:i + 1500]):
        add(m.group(1))
    for w in ("Sly", "Bentley", "Murray", "Everyone", "Sly and Bentley", "locked", "available", "done and closed", "Progress",
              "Block", "Ramp", "Stairs", "Cylinder", "Crawl tunnel", "Mesh", "Characters", "Gameplay", "Paths / traversal", "Lighting"):
        add(w)
    return out


def glyphs():
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        return None
    have = set()
    for f in ("AtkinsonHyperlegible-Regular.ttf",):
        have.update(TTFont(ROOT / "launcher" / "assets" / f).getBestCmap())
    for f in ("NotoSans-Regular.subset.ttf", "NotoSansCJKjp-Regular.subset.otf"):
        have.update(TTFont(ROOT / "launcher" / "assets" / "l10n" / f).getBestCmap())
    return have


def main():
    ks = keys()
    if len(sys.argv) > 2 and sys.argv[1] == "--keys":
        Path(sys.argv[2]).write_text(json.dumps(ks, ensure_ascii=False, indent=0), encoding="utf-8")
    have = glyphs()
    fail = False
    spec = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?[a-zA-Z]")
    for path in sorted((ROOT / "editor" / "lang").glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        code = data["language"]["code"]
        if code == "en":
            continue
        s = data["strings"]
        # (a string the language leaves as the English counts as translated: names, "Laser", ...)
        missing = [k for k in ks if k not in s and not any(c.isalpha() for c in k) is False and k not in s]
        missing = [k for k in missing if code not in ("",)]
        stale = [k for k in s if k not in ks]
        bad = [k for k, v in s.items() if spec.findall(k) != spec.findall(v)
               or k[:len(k) - len(k.lstrip())] != v[:len(v) - len(v.lstrip())] or k[len(k.rstrip()):] != v[len(v.rstrip()):]]
        glyph = sorted({c for v in s.values() for c in v if ord(c) >= 0x20 and have is not None and ord(c) not in have})
        untranslated = len(missing)
        print(f"{code}: {len(ks) - untranslated}/{len(ks)} strings, {len(stale)} stale, {len(bad)} format / space problems"
              + ("" if have is None else f", {len(glyph)} characters without a glyph"))
        for k in stale[:5]:
            print(f"  stale: {k!r}")
        for k in bad[:5]:
            print(f"  format / spaces: {k!r}")
        if glyph:
            print("  no glyph: " + "".join(glyph[:40]))
        # (strings equal to the English are fine: proper names, "Laser", "Volume"; more than 15% left as the English is not)
        if stale or bad or glyph or untranslated > len(ks) * 0.15:
            fail = True
    print("test-editor-l10n: " + ("FAIL" if fail else "PASS"))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
