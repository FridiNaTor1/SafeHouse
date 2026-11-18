#!/usr/bin/env python3
"""The one-time move of the profile folders from their old name (projectcane / ProjectCane) to SafeHouse
(platform/user_folders.h), through the real launcher, in a private HOME.

    test-folder-upgrade.py LAUNCHER

Builds a fake old layout (installations of two regions, a source-built runtime, launcher.json with absolute paths,
saves, settings, Mods, a PS3 HD pack, logs, the oldest launchers' state logs) and checks:
  1. a plain upgrade: every file arrives byte for byte (and executable bits), the old folders are gone, the stored
     paths point into the new folders, the launcher finds the installations there; a second start changes nothing;
  2. the new folder already there (an interrupted move): new files win, identical ones are dropped, different old
     versions land in "ProjectCane leftovers", nothing is lost;
  3. the copy path (as on another drive, SAFEHOUSE_TEST_UPGRADE_COPY=1) with an unreadable file: it stays in the old
     folder, which is kept, and the next start finishes the move;
  4. a portable copy never touches the profile folders.
Linux and Windows layouts (macOS never had the old name).
"""
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

WINDOWS = os.name == "nt"
OLD, NEW = ("ProjectCane", "SafeHouse") if WINDOWS else ("projectcane", "SafeHouse")
SLY1_USA = "d4ba3ce30047ffb2a825ef13d37a48c4df63aa57977157a194f66bd0b859796c"
SLY1_PAL = "cfab20b47a0b9ae1c9eb47d278a48704ca8e272505594c6705de1525a7cccb91"

launcher = Path(sys.argv[1]).resolve()
failures = []


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def snapshot(root):
    """{relative path: (sha256, executable)} of every file below root."""
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_symlink():
            out[p.relative_to(root).as_posix()] = ("link", os.readlink(p))
        elif p.is_file():
            out[p.relative_to(root).as_posix()] = (hashlib.sha256(p.read_bytes()).hexdigest(),
                                                   bool(p.stat().st_mode & stat.S_IXUSR) and not WINDOWS)
    return out


class Profile:
    def __init__(self, root):
        self.root = root
        self.env = {k: v for k, v in os.environ.items() if k not in ("APPIMAGE",)}
        self.env.update(HOME=str(root / "home"), XDG_CONFIG_HOME=str(root / "config"), XDG_DATA_HOME=str(root / "data"),
                        XDG_STATE_HOME=str(root / "state"), XDG_CACHE_HOME=str(root / "cache"),
                        LOCALAPPDATA=str(root / "Local"), APPDATA=str(root / "Roaming"), SDL_AUDIODRIVER="dummy")
        (root / "home").mkdir(parents=True, exist_ok=True)
        if WINDOWS:
            base = root / "Local"
            self.old = {"data": base / OLD}
            self.new = {"data": base / NEW}
            self.config = lambda r: r["data"] / "config"
            self.user = lambda r: r["data"] / "user"
            self.logs = lambda r: r["data"] / "logs"
        else:
            self.old = {"data": root / "data" / OLD, "config": root / "config" / OLD, "state": root / "state" / OLD}
            self.new = {k: v.parent / NEW for k, v in self.old.items()}
            self.config = lambda r: r["config"]
            self.user = lambda r: r["config"]
            self.logs = lambda r: r["config"] / "logs"

    def run(self, *args, extra=None):
        env = dict(self.env)
        env.update(extra or {})
        return subprocess.run([str(launcher), *args], env=env, capture_output=True, timeout=120)

    def build_old(self):
        o = self.old
        installs = o["data"] / "installations"
        for name, sha in (("sly1", SLY1_USA), ("sly1-europe", SLY1_PAL)):
            d = installs / name
            (d / "Worlds").mkdir(parents=True)
            (d / "Worlds" / "level.brx").write_bytes(os.urandom(70000))
            (d / "installation.json").write_text(json.dumps({
                "schema": 1, "asset_version": 2, "disc_sha256": sha, "region": "NTSC-U" if name == "sly1" else "PAL",
                "iso_path": str(self.root / "isos" / (name + ".iso"))}))
        runtime = o["data"] / "user/runtime/bin"
        runtime.mkdir(parents=True)
        tool = runtime / ("Sly1-Release.exe" if WINDOWS else "Sly1-Release")
        tool.write_text("#!/bin/sh\nexit 0\n")
        tool.chmod(0o755)
        if not WINDOWS:
            (runtime / "link").symlink_to("Sly1-Release")
        user, config, logs = self.user(o), self.config(o), self.logs(o)
        for folder in (user / "Saves", user / "sly2/Saves", user / "Mods/Textures", user / "sly2/PS3 HD/Textures", logs, config):
            folder.mkdir(parents=True, exist_ok=True)
        (user / "Saves/save1").write_bytes(b"sly1 save \x00\xff" * 100)
        (user / "Saves/system_settings.dat").write_bytes(b"PCANESYS" + bytes(300))
        (user / "sly2/Saves/save3").write_bytes(b"sly2 save" * 1000)
        (user / "sly2/Saves/advanced_controls.json").write_text('{"version": 1}\n')
        (user / "Debug Mode.ini").write_text("[Debug]\nDebug Mode=1\n")
        (user / "Mods/Textures/café é.png").write_bytes(b"\x89PNG mod")
        (user / "sly2/PS3 HD/Textures/a.png").write_bytes(b"\x89PNG hd")
        (user / "sly2/PS3 HD/pack.json").write_text(json.dumps({"source": str(o["data"] / "ps3/PS3_GAME"), "textures": 1}))
        (logs / "launcher.log").write_text("old log\n")
        if not WINDOWS:
            (o["state"] / "logs").mkdir(parents=True)
            (o["state"] / "logs/launcher.log").write_text("oldest log\n")
        (config / "launcher.json").write_text(json.dumps({
            "runtime_dir": str(runtime),
            "iso_path": str(self.root / "isos/sly1.iso"),
            "browse_dir": str(user / "Saves"),
            "region_isos": {"sly1": {"usa": str(self.root / "isos/sly1.iso"), "europe": str(o["data"] / "isos/eu.iso")}},
            "regions": {"sly1": "europe"},
            "last_game": "sly1",
        }))


def plain_upgrade(tmp):
    p = Profile(tmp / "plain café")
    p.build_old()
    before = {k: snapshot(v) for k, v in p.old.items()}
    r = p.run("--installs")
    check(r.returncode == 0, "plain: the launcher starts (%s)" % r.stderr.decode(errors="replace").strip()[-300:])
    check(all(not v.exists() for v in p.old.values()), "plain: the old folders are gone")
    for k, v in p.new.items():
        after = snapshot(v)
        missing = [f for f, h in before[k].items() if after.get(f) != h and not f.endswith(("launcher.json", "pack.json", "installation.json"))]
        check(not missing, "plain: %s arrived byte for byte with its executable bits (%s)" % (k, missing[:3]))
    config = json.loads((p.config(p.new) / "launcher.json").read_text())
    runtime = p.new["data"] / "user/runtime/bin"
    check(config["runtime_dir"] == str(runtime), "plain: runtime_dir points into the new folder (%s)" % config["runtime_dir"])
    check(config["browse_dir"] == str(p.user(p.new) / "Saves"), "plain: browse_dir follows the move")
    check(config["iso_path"] == str(p.root / "isos/sly1.iso"), "plain: paths outside the profile are kept")
    check(config["region_isos"]["sly1"]["europe"] == str(p.new["data"] / "isos/eu.iso"), "plain: nested paths are rewritten")
    check(config["regions"] == {"sly1": "europe"} and config["last_game"] == "sly1", "plain: the other settings are kept")
    pack = json.loads((p.user(p.new) / "sly2/PS3 HD/pack.json").read_text())
    check(pack["source"] == str(p.new["data"] / "ps3/PS3_GAME"), "plain: the PS3 HD pack's source follows the move")
    manifest = json.loads((p.new["data"] / "installations/sly1/installation.json").read_text())
    check(manifest["iso_path"] == str(p.root / "isos/sly1.iso") and manifest["disc_sha256"] == SLY1_USA, "plain: manifests keep their content")
    listed = r.stdout.decode().splitlines()
    check(any(l.startswith("sly1 usa ") and l.endswith(str(p.new["data"] / "installations/sly1")) for l in listed) and
          any(l.startswith("sly1 europe * ") and l.endswith(str(p.new["data"] / "installations/sly1-europe")) for l in listed),
          "plain: both regions are found in the new folder, the chosen one still chosen (%s)" % listed)
    record = json.loads((p.config(p.new) / "folder-upgrade.json").read_text())
    check(record["failed"] == 0 and record["renamed"] >= 1 and record["rewritten"] >= 2, "plain: folder-upgrade.json records the move")
    if not WINDOWS:
        check((p.new["state"] / "logs/launcher.log").read_text() == "oldest log\n", "plain: the state folder moved too")
    # a second start: nothing to do, nothing changes
    snap = {k: snapshot(v) for k, v in p.new.items()}
    r = p.run("--installs")
    check(r.returncode == 0 and {k: snapshot(v) for k, v in p.new.items()} == snap and all(not v.exists() for v in p.old.values()),
          "plain: a second start changes nothing")
    # an old launcher started again later re-creates an old folder: the next start merges it in
    stray = p.config(p.old) / "launcher.json"
    stray.parent.mkdir(parents=True)
    stray.write_text('{"last_game": "sly2"}')
    r = p.run("--installs")
    left = p.new["config" if not WINDOWS else "data"] / "ProjectCane leftovers"
    check(r.returncode == 0 and not p.config(p.old).exists() and json.loads((p.config(p.new) / "launcher.json").read_text())["last_game"] == "sly1"
          and any(f.name == "launcher.json" for f in left.rglob("*")), "plain: a later old folder is merged, the current settings win")


def existing_new(tmp):
    p = Profile(tmp / "existing")
    p.build_old()
    # the new folder exists already: an identical save, a different settings file, a file only there
    nu, ou = p.user(p.new), p.user(p.old)
    (nu / "Saves").mkdir(parents=True)
    shutil.copy2(ou / "Saves/save1", nu / "Saves/save1")
    (nu / "Saves/system_settings.dat").write_bytes(b"NEWER settings")
    (nu / "Saves/only-new").write_bytes(b"new")
    # an interrupted move: half of the installations already there
    half = p.new["data"] / "installations/sly1-europe"
    shutil.copytree(p.old["data"] / "installations/sly1-europe", half)
    (half / "Worlds/level.brx.safehouse-upgrade").write_bytes(b"partial copy")
    old_settings = (ou / "Saves/system_settings.dat").read_bytes()
    r = p.run("--installs")
    check(r.returncode == 0, "existing: the launcher starts")
    check(all(not v.exists() for v in p.old.values()), "existing: the old folders are gone")
    check((nu / "Saves/system_settings.dat").read_bytes() == b"NEWER settings", "existing: the new folder's version wins")
    left = list((p.config(p.new) if not WINDOWS else p.new["data"]).glob("ProjectCane leftovers/**/system_settings.dat"))
    check(len(left) == 1 and left[0].read_bytes() == old_settings, "existing: the old version is kept in ProjectCane leftovers (%s)" % left)
    check((nu / "Saves/save1").is_file() and not any(p.new["data"].rglob("*.safehouse-upgrade")), "existing: duplicates merged, no partial copies left")
    check((nu / "Saves/only-new").read_bytes() == b"new" and (nu / "sly2/Saves/save3").is_file(), "existing: files from both are there")
    check((half / "Worlds/level.brx").is_file(), "existing: the already moved part stays")


def copy_path(tmp):
    p = Profile(tmp / "copy")
    p.build_old()
    before = {k: snapshot(v) for k, v in p.old.items()}
    locked = p.user(p.old) / "sly2/Saves/save3"
    hook = {"SAFEHOUSE_TEST_UPGRADE_COPY": "1"}
    if not WINDOWS and os.geteuid() != 0:
        locked.chmod(0)
        r = p.run("--installs", extra=hook)
        check(r.returncode == 0, "copy: the launcher starts with an unreadable file")
        check(locked.exists() and not (p.user(p.new) / "sly2/Saves/save3").exists(), "copy: the unreadable file stays in the old folder")
        check((p.user(p.new) / "Saves/save1").is_file(), "copy: everything else moved")
        locked.chmod(0o644)
    r = p.run("--installs", extra=hook)
    check(r.returncode == 0 and all(not v.exists() for v in p.old.values()), "copy: the next start finishes the move")
    for k, v in p.new.items():
        after = snapshot(v)
        missing = [f for f, h in before[k].items() if after.get(f) != h and not f.endswith(("launcher.json", "pack.json", "installation.json"))]
        check(not missing, "copy: %s copied byte for byte (%s)" % (k, missing[:3]))
    config = json.loads((p.config(p.new) / "launcher.json").read_text())
    check(config["runtime_dir"] == str(p.new["data"] / "user/runtime/bin"), "copy: stored paths rewritten")


def portable(tmp):
    p = Profile(tmp / "portable")
    p.build_old()
    before = {k: snapshot(v) for k, v in p.old.items()}
    copy = tmp / "portable/SafeHouse portable/bin"
    copy.mkdir(parents=True)
    shutil.copy2(launcher, copy / launcher.name)
    for dll in launcher.parent.glob("*.dll"):
        shutil.copy2(dll, copy / dll.name)
    (copy.parent / "portable.txt").write_text("SafeHouse portable mode\n")
    r = subprocess.run([str(copy / launcher.name), "--installs"], env=p.env, capture_output=True, timeout=60)
    check(r.returncode == 0 and {k: snapshot(v) for k, v in p.old.items()} == before and not any(v.exists() for v in p.new.values()),
          "portable: the profile folders are not touched")


with tempfile.TemporaryDirectory(prefix="safehouse-upgrade-") as t:
    tmp = Path(t)
    for case in (plain_upgrade, existing_new, copy_path, portable):
        case(tmp)
if failures:
    print("%d check(s) failed" % len(failures))
    sys.exit(1)
print("Folder upgrade passed")
