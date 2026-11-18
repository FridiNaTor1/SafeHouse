#!/usr/bin/env python3
"""Sly 2: a language of the shared languages folder on another region's installation (Sly2-Release/langstore.h).

  test-sly2-languages.py SLY2_RELEASE

Needs the user's own game data, so it is skipped (exit code 77) where there is none:
  SLY2_TEST_USA        a USA installation folder or disc image (also checked: SLY2_TEST_EUROPE, the same for Europe)
  SLY2_TEST_LANGUAGES  a languages folder that has Japanese with its Font.bin (the launcher's sly2-languages), and / or
  SLY2_TEST_JAPAN      a Japan installation folder or disc image: Japanese is imported from it into a temporary folder
                       (about 400 MB while the test runs)

Checks, with Japanese chosen on the USA (Europe) data:
- the import writes the Japanese font beside the text;
- --list-languages offers Japanese with its text shown;
- --check-language Japanese: every world's strings are the Japanese ones and every character has a glyph in the font
  it is drawn with (it fails when the text stays English or would draw as missing characters);
- without the Font.bin the game says the text is not shown (--check-language fails, --list-languages says so): the
  check itself notices English text;
- the text and the voices apart (Settings.ini Voices, SLY2_VOICE_LANGUAGE here): English text with Japanese voices
  loads the world's lines from the folder's Japanese and plays a movie's Japanese sound; Japanese text with English
  voices keeps the disc's English lines and sound; --list-languages offers Japanese without its font for the voices
  only.
Everything runs in private HOME / XDG folders.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SKIP = 77


def main():
    exe = str(Path(sys.argv[1]).resolve())
    usa, europe = os.environ.get("SLY2_TEST_USA"), os.environ.get("SLY2_TEST_EUROPE")
    store, japan = os.environ.get("SLY2_TEST_LANGUAGES"), os.environ.get("SLY2_TEST_JAPAN")
    if not usa or not (store or japan):
        print("skipped: set SLY2_TEST_USA and SLY2_TEST_LANGUAGES or SLY2_TEST_JAPAN (the user's own game data)")
        return SKIP
    for p in (usa, europe, store, japan):
        if p and not Path(p).exists():
            print(f"skipped: {p} does not exist")
            return SKIP
    with tempfile.TemporaryDirectory(prefix="sly2-languages-") as tmp:
        tmp = Path(tmp)
        env = dict(os.environ, HOME=str(tmp / "home"), XDG_CONFIG_HOME=str(tmp / "home/.config"),
                   XDG_DATA_HOME=str(tmp / "home/.local/share"), XDG_STATE_HOME=str(tmp / "home/.local/state"),
                   XDG_CACHE_HOME=str(tmp / "home/.cache"), SDL_AUDIODRIVER="dummy")
        env.pop("SLY2_LANGUAGE", None)
        env.pop("SLY2_LANGUAGES", None)
        for key in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"):
            Path(env[key]).mkdir(parents=True, exist_ok=True)

        def data(path):
            return ["--data-dir", path] if Path(path).is_dir() else ["--disc", path]

        def run(args, expect=0, extra=None):
            r = subprocess.run([exe, "--user-dir", str(tmp / "user")] + args, capture_output=True, text=True, env=dict(env, **(extra or {})),
                               stdin=subprocess.DEVNULL, timeout=900, encoding="utf-8", errors="replace")
            if expect is not None and r.returncode != expect:
                sys.stdout.write(r.stdout[-4000:] + r.stderr[-2000:])
                raise SystemExit(f"FAIL: {' '.join(args)}: exit code {r.returncode}, expected {expect}")
            return r

        folders = []
        if japan:
            # the import from the user's Japanese disc: the text, the lines, the movies' sound and the font
            imported = tmp / "imported"
            run(data(japan) + ["--languages-dir", str(imported), "--import-languages", str(imported), "--only-language", "Japanese"])
            for name in ("Text.json", "language.json", "Font.bin"):
                if not (imported / "Japanese" / name).is_file():
                    raise SystemExit(f"FAIL: the import from {japan} wrote no Japanese/{name}")
            if (imported / "Japanese" / "Font.bin").read_bytes()[:8] != b"S2FONT1\0":
                raise SystemExit("FAIL: the imported Font.bin is not a font file")
            folders.append(imported)
        if store:
            if not (Path(store) / "Japanese" / "Font.bin").is_file():
                raise SystemExit(f"FAIL: {store} has no Japanese/Font.bin (import the Japanese disc with this version)")
            folders.append(Path(store))
        # the same Japanese without its font: the text cannot be shown, and the game has to say so
        nofont = tmp / "nofont"
        (nofont / "Japanese").mkdir(parents=True)
        for name in ("Text.json", "language.json"):
            shutil.copy2(folders[0] / "Japanese" / name, nofont / "Japanese" / name)
        try:   # (its voices too: offered for the voices only)
            os.symlink(folders[0] / "Japanese" / "Dialog", nofont / "Japanese" / "Dialog", target_is_directory=True)
            nofont_voices = True
        except OSError:
            nofont_voices = False

        def listed(install, folder):
            out = run(data(install) + ["--languages-dir", str(folder), "--list-languages"]).stdout
            rows = [json.loads(line) for line in out.splitlines() if line.startswith("{")]
            return {row["key"]: row for row in rows}

        for install in filter(None, (usa, europe)):
            for folder in folders:
                row = listed(install, folder).get("Japanese")
                if not row or not row.get("text") or row.get("text_shown") is not True:
                    raise SystemExit(f"FAIL: {install} with {folder}: Japanese is not offered with its text: {row}")
                out = run(data(install) + ["--languages-dir", str(folder), "--check-language", "Japanese"]).stdout
                line = [l for l in out.splitlines() if l.startswith("check-language: Japanese on ")]
                if "check-language: PASS" not in out or not line:
                    raise SystemExit(f"FAIL: {install} with {folder}: {out[-2000:]}")
                print(line[0])
            row = listed(install, nofont).get("Japanese")
            if not row or row.get("text_shown") is not False:
                raise SystemExit(f"FAIL: {install} without the font: --list-languages does not say the text is not shown: {row}")
            out = run(data(install) + ["--languages-dir", str(nofont), "--check-language", "Japanese"], expect=1).stdout
            if "its text is not shown" not in out:
                raise SystemExit(f"FAIL: {install} without the font: {out[-2000:]}")
            print(f"{install}: without the Font.bin the text is reported as not shown")
            if row.get("as_text") is not False or row.get("as_voices") is not nofont_voices:
                raise SystemExit(f"FAIL: {install} without the font: Japanese should be offered for the voices only: {row}")
            # the text and the voices apart
            folder = folders[0]
            row = listed(install, folder).get("Japanese")
            if not row or row.get("as_text") is not True or row.get("as_voices") is not True:
                raise SystemExit(f"FAIL: {install}: Japanese should be offered for the text and the voices: {row}")
            for text, voice, want_lines, want_movie in (("English", "Japanese", "dialog lines in japanese", "the sound of japanese from the languages folder"),
                                                        ("Japanese", "English", "dialog lines in english (the disc's)", "the disc's sound")):
                extra = {"SLY2_LANGUAGE": text, "SLY2_VOICE_LANGUAGE": voice, "SDL_VIDEODRIVER": "dummy"}
                out = run(data(install) + ["--languages-dir", str(folder), "--world", "2", "--headless", "--fixed-dt", "--frames", "30"], extra=extra).stdout
                if want_lines not in out:
                    raise SystemExit(f"FAIL: {install}: {text} text with {voice} voices: no '{want_lines}' at the world's load: {out[-1500:]}")
                out = run(data(install) + ["--languages-dir", str(folder), "--play-movie", "Prologue", "--seconds", "0.5"], extra=extra).stdout
                if want_movie not in out:
                    raise SystemExit(f"FAIL: {install}: {text} text with {voice} voices: the movie's sound is not the voices' ({out[-1500:]})")
                print(f"{install}: {text} text with {voice} voices: the lines and the movie's sound are {voice}")
    print("Sly 2 languages: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
