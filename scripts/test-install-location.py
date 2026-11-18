#!/usr/bin/env python3
"""Games installed in another folder (launcher/backend.h: installRoot, install_paths, moveInstallation), through the
real launcher in a private HOME, with fake installations (an installation.json is what the launcher looks for).

    test-install-location.py LAUNCHER

Checks: the default stays installations/<game>; --install-dir picks <folder>/SafeHouse; installs there are found
(by name and from launcher.json "install_paths", also after the setting is reset); regions side by side across
folders; --move-install moves a region (same drive: a rename; SAFEHOUSE_TEST_UPGRADE_COPY is not involved) and the
game then plays from the new place; moving into a folder that has other files is refused.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

WINDOWS = os.name == "nt"
SLY1_USA = "d4ba3ce30047ffb2a825ef13d37a48c4df63aa57977157a194f66bd0b859796c"
SLY1_PAL = "cfab20b47a0b9ae1c9eb47d278a48704ca8e272505594c6705de1525a7cccb91"
launcher = Path(sys.argv[1]).resolve()
failures = []


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def manifest(d, sha, region):
    d.mkdir(parents=True, exist_ok=True)
    (d / "Worlds").mkdir(exist_ok=True)
    (d / "Worlds/level.brx").write_bytes(b"level" * 1000)
    (d / "installation.json").write_text(json.dumps({"schema": 1, "asset_version": 2, "disc_sha256": sha, "region": region}))


with tempfile.TemporaryDirectory(prefix="safehouse-location-") as t:
    root = Path(t) / "loc café"
    env = {k: v for k, v in os.environ.items() if k != "APPIMAGE"}
    env.update(HOME=str(root / "home"), XDG_CONFIG_HOME=str(root / "config"), XDG_DATA_HOME=str(root / "data"),
               XDG_STATE_HOME=str(root / "state"), XDG_CACHE_HOME=str(root / "cache"), LOCALAPPDATA=str(root / "Local"))
    (root / "home").mkdir(parents=True)
    base = root / "Local/SafeHouse" if WINDOWS else root / "data/SafeHouse"
    config_file = (base / "config" if WINDOWS else root / "config/SafeHouse") / "launcher.json"
    default = base / "installations"

    def run(*args):
        return subprocess.run([str(launcher), *map(str, args)], env=env, capture_output=True, timeout=60)

    def installs():
        r = run("--installs")
        assert r.returncode == 0, r
        return {tuple(l.split()[:2]): l.split(" ", 4)[-1].split(" ", 1)[-1] for l in r.stdout.decode().splitlines()}

    r = run("--install-dir")
    check(r.returncode == 0 and r.stdout.decode().strip() == str(default) + " (default)", "default: installations/ (%s)" % r.stdout)
    drive = root / "Other Drive"
    drive.mkdir()
    r = run("--install-dir", drive)
    check(r.returncode == 0 and r.stdout.decode().strip() == str(drive / "SafeHouse"), "--install-dir: <folder>/SafeHouse (%s)" % r.stdout)
    check(json.loads(config_file.read_text())["install_dir"] == str(drive / "SafeHouse"), "launcher.json install_dir")
    # an installation in the chosen folder is found by its name
    manifest(drive / "SafeHouse/sly1", SLY1_USA, "NTSC-U")
    found = installs()
    check(found.get(("sly1", "usa"), "").endswith(str(drive / "SafeHouse/sly1")), "found in the chosen folder (%s)" % found)
    # another region in the default folder: both listed
    manifest(default / "sly1-europe", SLY1_PAL, "PAL")
    found = installs()
    check(("sly1", "europe") in found and ("sly1", "usa") in found, "regions across folders (%s)" % found)
    # move USA back to the default folder: a rename here; remembered nowhere else
    r = run("--move-install", "sly1", default, "--region", "usa")
    check(r.returncode == 0, "--move-install to the default folder (%s)" % r.stderr.decode()[-300:])
    check(not (drive / "SafeHouse/sly1").exists() and (default / "sly1/Worlds/level.brx").is_file(), "the files moved")
    found = installs()
    check(found.get(("sly1", "usa"), "").endswith(str(default / "sly1")), "found after the move (%s)" % found)
    # and to a third folder: remembered in install_paths, found after the setting is reset
    third = root / "Third"
    r = run("--move-install", "sly1", third, "--region", "europe")
    check(r.returncode == 0 and (third / "SafeHouse/sly1/installation.json").is_file(), "--move-install into <folder>/SafeHouse/sly1 (%s)" % r.stderr.decode()[-300:])
    r = run("--install-dir", "default")
    check(r.returncode == 0 and "(default)" in r.stdout.decode() and "install_dir" not in json.loads(config_file.read_text()), "--install-dir default")
    cfg = json.loads(config_file.read_text())
    check(cfg.get("install_paths", {}).get("sly1") == [str(third / "SafeHouse/sly1")], "install_paths remembers it (%s)" % cfg.get("install_paths"))
    found = installs()
    check(found.get(("sly1", "europe"), "").endswith(str(third / "SafeHouse/sly1")), "found from install_paths (%s)" % found)
    # the drive is away: not listed, still remembered; back: listed again
    (third / "SafeHouse").rename(third / "away")
    found = installs()
    check(("sly1", "europe") not in found and json.loads(config_file.read_text()).get("install_paths"), "missing folder: not listed, kept")
    (third / "away").rename(third / "SafeHouse")
    check(("sly1", "europe") in installs(), "back again: listed")
    # plays from the new place (a fake runtime records its arguments)
    if not WINDOWS:
        bin_dir = root / "runtime/bin"
        bin_dir.mkdir(parents=True)
        game = bin_dir / "Sly1-Release"
        game.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$4/runtime-args.txt"\n')
        game.chmod(0o755)
        cfg = json.loads(config_file.read_text())
        cfg["runtime_dir"] = str(bin_dir)
        config_file.write_text(json.dumps(cfg))
        r = run("--play", "sly1", "--region", "europe")
        user = root / "config/SafeHouse"
        args = (user / "runtime-args.txt").read_text().splitlines() if (user / "runtime-args.txt").exists() else []
        check(r.returncode == 0 and args[:2] == ["--data-dir", str(third / "SafeHouse/sly1")], "--play uses the moved installation (%s)" % args)
    # another drive (the copy path: SAFEHOUSE_TEST_UPGRADE_COPY=1 makes every rename "fail"): copied, checked, switched
    fourth = root / "Fourth"
    before = sorted(p.relative_to(third / "SafeHouse/sly1").as_posix() for p in (third / "SafeHouse/sly1").rglob("*"))
    env["SAFEHOUSE_TEST_UPGRADE_COPY"] = "1"
    r = run("--move-install", "sly1", fourth, "--region", "europe")
    del env["SAFEHOUSE_TEST_UPGRADE_COPY"]
    moved = fourth / "SafeHouse/sly1"
    after = sorted(p.relative_to(moved).as_posix() for p in moved.rglob("*")) if moved.exists() else []
    check(r.returncode == 0 and b'"stage":"move"' in r.stdout and after == before and not (third / "SafeHouse/sly1").exists()
          and not Path(str(moved) + ".staging").exists(), "copy move: copied with progress, old folder removed (%s)" % r.stderr.decode()[-200:])
    check(("sly1", "europe") in installs() and installs()[("sly1", "europe")].endswith(str(moved)), "copy move: found in the new place")
    # refusals: into a folder with other files of that name, or where it already is
    blocked = root / "Blocked/SafeHouse/sly1"
    blocked.mkdir(parents=True)
    (blocked / "mine.txt").write_text("not SafeHouse's")
    (root / "Blocked/SafeHouse/sly1-usa").mkdir()
    (root / "Blocked/SafeHouse/sly1-usa/mine.txt").write_text("also mine")
    r = run("--move-install", "sly1", root / "Blocked", "--region", "usa")
    check(r.returncode != 0 and (blocked / "mine.txt").read_text() == "not SafeHouse's" and (default / "sly1/installation.json").is_file(),
          "a folder with other files is refused, nothing touched (%s)" % r.stderr.decode().strip())
    r = run("--move-install", "sly1", fourth, "--region", "europe")
    check(r.returncode != 0 and (moved / "installation.json").is_file(), "moving to where it is is refused")
if failures:
    print("%d check(s) failed" % len(failures))
    sys.exit(1)
print("Install location passed")
