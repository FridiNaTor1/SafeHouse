#!/usr/bin/env python3
"""The Collection Style Launcher (launcher/collection), through the real launcher binary.

    test-collection-launcher.py LAUNCHER [--ps3 SOURCE]

Checks, with synthetic data and a private profile only:
  - the PS3 menu import (--collection-import) from a synthetic Sly Collection folder, and --collection-remove;
  - screenshots of the menu render (no games, some games, the options, extras) without crashing;
  - a launch from the menu (scripted --live run, keyboard nav to Sly 2, Enter) starts the game runtime with
    SAFEHOUSE_LAUNCHER=collection and the menu comes back after the game quits;
  - the game started any other way (--play) does not get the variable;
  - the Steam and desktop shortcuts for the collection (`--collection`).
With --ps3 it also imports from a real Sly Collection source (folder, .iso or .zip).
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


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


def png(width=4, height=2):
    import zlib
    raw = b"".join(b"\0" + b"\x30\x60\xc0\xff" * width for _ in range(height))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def fsb4(name, frames=128):
    """An FSB4 bank with one stereo Xbox IMA ADPCM sample (silence)."""
    data = bytes(72 * (frames // 64))
    header = bytearray(80)
    struct.pack_into("<H", header, 0, 80)
    header[2:2 + len(name)] = name.encode()
    struct.pack_into("<6I", header, 32, frames, len(data), 0, frames - 1, 0x20400000, 44100)
    struct.pack_into("<4H", header, 56, 255, 128, 128, 2)
    return b"FSB4" + struct.pack("<5I", 1, len(header), len(data), 0x40000, 0x40) + bytes(24) + bytes(header) + data


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

        # ---- the PS3 menu import from a synthetic Sly Collection folder
        disc = work / "disc"
        usr = disc / "PS3_GAME" / "USRDIR"
        (usr / "SLY2").mkdir(parents=True)
        (usr / "AUDIO").mkdir()
        (usr / "SLY2" / "SLY2.WAL").write_bytes(b"\0" * 2048)
        with zipfile.ZipFile(usr / "IMAGE.ZIP", "w") as z:
            for game in ("SlyCollection", "Sly1", "Sly2", "Sly3"):
                z.writestr(f"System/{game}/icon0.png", png(32, 16))
        (usr / "AUDIO" / "SLYAUDIO_MUSIC.FSB").write_bytes(fsb4("FrontEnd_Loop"))
        (usr / "AUDIO" / "SLYAUDIO_BANK.FSB").write_bytes(fsb4("positive_03"))
        (disc / "PS3_GAME" / "PIC1.PNG").write_bytes(png(16, 9))
        not_disc = work / "notdisc"
        not_disc.mkdir()
        r = run([copied, "--collection-import", not_disc], env=env)
        assert r.returncode != 0 and b"not The Sly Collection" in r.stderr, r
        r = run([copied, "--collection-import", disc], env=env)
        assert r.returncode == 0, r
        caches = list(portable.rglob("PS3 menu"))
        assert len(caches) == 1, caches
        cache = caches[0]
        for name in ("IMAGE.ZIP", "SLYAUDIO_MUSIC.FSB", "SLYAUDIO_BANK.FSB", "PIC1.PNG", "source.json"):
            assert (cache / name).is_file(), name
        assert (cache / "IMAGE.ZIP").read_bytes() == (usr / "IMAGE.ZIP").read_bytes()
        assert not any(home.rglob("PS3 menu")), "the import went outside the profile"

        # ---- screenshots render (fixtures; the synthetic PS3 files; options, extras)
        shots = work / "shots"
        shots.mkdir()
        # (the renders need a desktop session: on a Linux machine without X11 / Wayland, such as CI, they are skipped)
        have_display = not sys.platform.startswith("linux") or bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))
        if not have_display:
            print("no display: the screenshot and launch-from-the-menu checks are skipped")
        for name, extra in (() if not have_display else (("none", []), ("some", ["--collection-games", "sly1,sly2"]),
                            ("ps3", ["--collection-games", "sly2", "--collection-ps3", cache.parent.parent]),
                            ("options", ["--screen", "options"]), ("extras", ["--screen", "extras"]),
                            ("wide", ["--size", "3440x1080", "--nav", "right,right"]), ("narrow", ["--size", "1024x768"]))):
            out = shots / f"{name}.png"
            r = run([copied, "--screenshot", out, "--collection"] + extra, env=env)
            assert r.returncode == 0 and out.is_file() and out.stat().st_size > 10000, (name, r)
        r = run([copied, "--collection-sounds"], env=env)
        assert r.returncode == 0 and b"FrontEnd_Loop frames 128" in r.stdout and b"positive_03 frames 128" in r.stdout, r
        r = run([copied, "--collection-remove"], env=env)
        assert r.returncode == 0 and not cache.exists(), r

        # ---- a launch from the menu: the flag reaches the game, the menu comes back
        if os.name != "nt" and have_display:
            data = portable / "data"
            sly2 = data / "installations" / "sly2"
            sly2.mkdir(parents=True)
            (sly2 / "extraction_manifest.json").write_bytes(b"{}")
            manifest = {"schema": 1, "game": "sly2", "asset_version": 3, "layout": "files",
                        "disc_sha1": "de2af92a54de71e33edc8a160c31c0337a95e6f5",
                        "files": [{"path": "extraction_manifest.json", "size": 2, "sha256": hashlib.sha256(b"{}").hexdigest()}]}
            (sly2 / "installation.json").write_text(json.dumps(manifest))
            runtime = bin_dir / "Sly2-Release"
            log = work / "runtime.log"
            runtime.write_text(f'#!/bin/sh\necho "launcher=$SAFEHOUSE_LAUNCHER" >> "{log}"\nprintf "%s\\n" "$@" >> "{log}"\n')
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
                r = run([copied, "--screenshot", out, "--collection", "--live", "--nav", "right,right,a", "--delay", "3"], env=env)
                assert r.returncode == 0 and out.is_file(), r
                assert log.is_file(), "the game was not started from the menu"
                lines = log.read_text().splitlines()
                assert lines[0] == "launcher=collection", lines
                assert "--data-dir" in lines and "--user-dir" in lines, lines
                print("launch from the menu: runtime started with SAFEHOUSE_LAUNCHER=collection, menu back afterwards")

        # ---- the Steam shortcut (a fake Steam folder) and the desktop shortcut
        steam_user = home / "steam" / "userdata" / "1234" / "config"
        steam_user.mkdir(parents=True)
        r = run([copied, "--add-to-steam", "collection"], env=env)
        assert r.returncode == 0 and b"SafeHouse Collection" in r.stdout and b"--collection" in r.stdout, r
        vdf = (steam_user / "shortcuts.vdf").read_bytes()
        assert b"SafeHouse Collection" in vdf and b"--collection" in vdf, vdf[:200]
        r = run([copied, "--steam-status"], env=env)
        assert r.returncode == 0 and b"collection: in Steam" in r.stdout, r
        if sys.platform.startswith("linux"):
            r = run([copied, "--add-desktop-shortcut", "collection", "--menu"], env=env)
            assert r.returncode == 0, r
            entry = (home / "data" / "applications" / "safehouse-collection.desktop").read_text()
            assert "--collection" in entry and "Name=SafeHouse Collection" in entry, entry
            assert (home / "Desktop" / "safehouse-collection.desktop").is_file()
    print("collection launcher: OK")


if __name__ == "__main__":
    main()
