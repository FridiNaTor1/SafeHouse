#!/usr/bin/env python3
"""Saves kept in another folder (launcher/backend.h: setSaveRoot, saveRootFor, launcher.json "user_dir"), through the
real launcher in a private HOME.

    test-save-location.py LAUNCHER

Checks: the default is the profile's user folder; --save-dir FOLDER --move copies the saves, settings and mods into
<folder>/SafeHouse Saves, checks them and only then removes the old copies (the launcher's own files stay); the game
gets the new folder as --user-dir; a file of the same name already there is put aside in "Replaced <date>", never
overwritten; --save-dir without --move only switches; a chosen folder that is away leaves the default in use; a
folder inside the current one is refused; back to the default works.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

WINDOWS = os.name == "nt"
SLY1_USA = "d4ba3ce30047ffb2a825ef13d37a48c4df63aa57977157a194f66bd0b859796c"
launcher = Path(sys.argv[1]).resolve()
failures = []


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def tree(d):
    return {p.relative_to(d).as_posix(): p.read_bytes() for p in sorted(d.rglob("*")) if p.is_file()}


with tempfile.TemporaryDirectory(prefix="safehouse-saves-") as t:
    root = Path(t) / "saves café"
    env = {k: v for k, v in os.environ.items() if k != "APPIMAGE"}
    env.update(HOME=str(root / "home"), XDG_CONFIG_HOME=str(root / "config"), XDG_DATA_HOME=str(root / "data"),
               XDG_STATE_HOME=str(root / "state"), XDG_CACHE_HOME=str(root / "cache"), LOCALAPPDATA=str(root / "Local"))
    (root / "home").mkdir(parents=True)
    base = root / "Local/SafeHouse" if WINDOWS else root / "data/SafeHouse"
    config_dir = base / "config" if WINDOWS else root / "config/SafeHouse"
    default = base / "user" if WINDOWS else config_dir
    config_file = config_dir / "launcher.json"

    def run(*args):
        return subprocess.run([str(launcher), *map(str, args)], env=env, capture_output=True, timeout=60)

    def last(r):  # (the folder line comes last, after the move's log)
        lines = r.stdout.decode().strip().splitlines()
        return lines[-1] if lines else ""

    r = run("--save-dir")
    check(r.returncode == 0 and last(r) == str(default) + " (default)", "default: the profile's user folder (%s)" % r.stdout)
    # the player's files (Sly 1 in the root, the later games in sly2/ sly3/), and the launcher's own
    mine = {"Saves/slot1.sav": b"sly1 save" * 50, "Settings.ini": b"[Display]\n", "Debug Mode.ini": b"[Debug]\nDebug Mode=0\n",
            "sly2/Saves/save1": b"sly2 save" * 300, "sly2/Settings.ini": b"[Audio]\n", "sly2/Mods/Textures/a.png": b"png",
            "sly3/Saves/save2": b"sly3 save" * 20}
    for rel, data in mine.items():
        (default / rel).parent.mkdir(parents=True, exist_ok=True)
        (default / rel).write_bytes(data)
    stamp = 1700000000
    os.utime(default / "sly2/Saves/save1", (stamp, stamp))
    (config_dir / "logs").mkdir(parents=True, exist_ok=True)
    (config_dir / "logs/launcher.log").write_text("log")
    drive = root / "Phone Storage"
    drive.mkdir()
    r = run("--save-dir", drive, "--move")
    moved = drive / "SafeHouse Saves"
    check(r.returncode == 0 and last(r) == str(moved), "--save-dir --move: <folder>/SafeHouse Saves (%s)" % r.stderr.decode()[-300:])
    got = {k: v for k, v in tree(moved).items() if k != ".safehouse-saves"}
    check(got == mine, "every file copied, byte for byte (%s)" % sorted(got))
    check(json.loads(config_file.read_text()).get("user_dir") == str(moved), "launcher.json user_dir")
    check(abs((moved / "sly2/Saves/save1").stat().st_mtime - stamp) < 2, "the saves keep their dates")
    left = set(tree(default))
    check(not any(k in left for k in mine), "the old copies are removed (%s)" % sorted(left))
    if not WINDOWS:
        check(config_file.is_file() and (config_dir / "logs/launcher.log").is_file(), "the launcher's own files stay in the profile")
    check((moved / ".safehouse-saves").is_file(), "the folder is marked as a SafeHouse save folder")
    # the game gets the new folder (a fake runtime records its arguments)
    if not WINDOWS:
        inst = base / "installations/sly1"
        inst.mkdir(parents=True)
        (inst / "installation.json").write_text(json.dumps({"schema": 1, "asset_version": 2, "disc_sha256": SLY1_USA, "region": "NTSC-U"}))
        bin_dir = root / "runtime/bin"
        bin_dir.mkdir(parents=True)
        game = bin_dir / "Sly1-Release"
        game.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "' + str(root / "runtime-args.txt") + '"\n')
        game.chmod(0o755)
        cfg = json.loads(config_file.read_text())
        cfg["runtime_dir"] = str(bin_dir)
        config_file.write_text(json.dumps(cfg))
        r = run("--play", "sly1")
        args = (root / "runtime-args.txt").read_text().splitlines() if (root / "runtime-args.txt").exists() else []
        check(r.returncode == 0 and args[2:4] == ["--user-dir", str(moved)], "--play passes the new folder as --user-dir (%s)" % args)
    # picking the parent again, or the folder itself: the same folder (its name / marker)
    r = run("--save-dir", moved)
    check(r.returncode == 0 and last(r) == str(moved), "the SafeHouse Saves folder itself is taken as it is")
    # into a folder that has its own copy of a save: that copy is put aside, never overwritten
    other = root / "Other/SafeHouse Saves"
    (other / "sly2/Saves").mkdir(parents=True)
    (other / "sly2/Saves/save1").write_bytes(b"their save")
    (other / "sly3/Saves").mkdir(parents=True)
    (other / "sly3/Saves/save2").write_bytes(mine["sly3/Saves/save2"])
    r = run("--save-dir", root / "Other", "--move")
    aside = sorted(other.glob("Replaced */sly2/Saves/save1"))
    check(r.returncode == 0 and (other / "sly2/Saves/save1").read_bytes() == mine["sly2/Saves/save1"] and len(aside) == 1
          and aside[0].read_bytes() == b"their save", "a different file of the same name is kept in Replaced <date> (%s)" % r.stderr.decode()[-200:])
    check(not moved.exists(), "the emptied old folder is removed")
    # without --move: only the setting (the saves there are used, the current ones stay)
    third = root / "Third/SafeHouse Saves"
    (third / "Saves").mkdir(parents=True)
    (third / "Saves/slot9.sav").write_bytes(b"copied in by hand")
    r = run("--save-dir", third)
    check(r.returncode == 0 and last(r) == str(third) and (other / "sly2/Saves/save1").is_file(), "--save-dir without --move only switches")
    # the folder is away (a drive taken out): the default is used, the setting is kept
    (root / "Third").rename(root / "Third away")
    r = run("--save-dir")
    check(r.returncode == 0 and "(chosen folder missing)" in r.stdout.decode() and str(default) in r.stdout.decode()
          and json.loads(config_file.read_text()).get("user_dir") == str(third), "a missing folder: the default is used, the choice kept (%s)" % r.stdout)
    (root / "Third away").rename(root / "Third")
    # a folder inside the current one is refused, nothing changes
    r = run("--save-dir", third / "inner", "--move")
    check(r.returncode != 0 and json.loads(config_file.read_text()).get("user_dir") == str(third), "a folder inside the current one is refused")
    # back to the default, moving
    r = run("--save-dir", "default", "--move")
    check(not (default / ".safehouse-saves").exists(), "the default folder gets no marker")
    check(r.returncode == 0 and last(r) == str(default) + " (default)" and (default / "Saves/slot9.sav").read_bytes() == b"copied in by hand"
          and "user_dir" not in json.loads(config_file.read_text()) and not third.exists(), "back to the default with --move (%s)" % r.stderr.decode()[-300:])
if failures:
    print("%d check(s) failed" % len(failures))
    sys.exit(1)
print("Save location passed")
