#!/usr/bin/env python3
"""Desktop shortcuts (Linux) against a fake HOME: safehouse --add-desktop-shortcut / --desktop-shortcut-status.

Usage: test-desktop-shortcuts.py SAFEHOUSE [ART_DIR]
HOME, XDG_CONFIG_HOME, XDG_DATA_HOME and XDG_STATE_HOME point into a temporary directory and `gio` is a fake that
records its arguments, so the user's desktop, menu and gvfs metadata are never touched.
"""
import os, pathlib, shutil, stat, subprocess, sys, tempfile

failures = []
def check(ok, what):
    print(('ok   ' if ok else 'FAIL ') + what)
    if not ok: failures.append(what)

def entry(file):
    out = {}
    lines = file.read_text().splitlines()
    assert lines[0] == '[Desktop Entry]', lines[0]
    for line in lines[1:]:
        k, _, v = line.partition('=')
        out[k] = v
    return out

def unescape_string(v):  # the Desktop Entry string escapes
    out, i = '', 0
    while i < len(v):
        if v[i] == '\\' and i + 1 < len(v):
            out += {'s': ' ', 'n': '\n', 't': '\t', 'r': '\r', '\\': '\\'}.get(v[i + 1], '\\' + v[i + 1]); i += 2
        else: out += v[i]; i += 1
    return out

def exec_argv(value):  # the spec's Exec rules: string escapes, then quoting, then %%
    s, argv, cur, i, quoted, has = unescape_string(value), [], '', 0, False, False
    while i < len(s):
        c = s[i]
        if quoted:
            if c == '\\' and i + 1 < len(s) and s[i + 1] in '"`$\\': cur += s[i + 1]; i += 2; continue
            if c == '"': quoted = False
            else: cur += c
        elif c == '"': quoted = has = True
        elif c == ' ':
            if cur or has: argv.append(cur)
            cur, has = '', False
        else: cur += c
        i += 1
    assert not quoted, 'unterminated quote'
    if cur or has: argv.append(cur)
    return [a.replace('%%', '%') for a in argv]

def main():
    exe = pathlib.Path(sys.argv[1]).resolve()
    art = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else pathlib.Path(__file__).resolve().parent.parent / 'launcher/assets/steam').resolve()
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='safehouse-desktop-'))
    try:
        home = tmp / 'home'
        (home / '.config').mkdir(parents=True)
        (home / '.config/user-dirs.dirs').write_text('# written by xdg-user-dirs-update\nXDG_DOWNLOAD_DIR="$HOME/Downloads"\n'
                                                     'XDG_DESKTOP_DIR="$HOME/Schreib tisch"\n')
        gio_log = tmp / 'gio.log'
        gio = tmp / 'gio'
        gio.write_text(f'#!/bin/sh\nprintf "%s|" "$@" >> "{gio_log}"\necho >> "{gio_log}"\n')
        gio.chmod(0o755)
        env = {k: v for k, v in os.environ.items() if k not in ('APPIMAGE', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_DESKTOP_DIR')}
        env.update({'HOME': str(home), 'XDG_STATE_HOME': str(tmp / 'state'), 'SAFEHOUSE_STEAM_ART': str(art), 'SAFEHOUSE_GIO': str(gio)})
        run = lambda *a, e=env: subprocess.run([str(exe), *a], env=e, capture_output=True, text=True, timeout=60)
        desk, menu = home / 'Schreib tisch', home / '.local/share/applications'
        icon = home / '.local/share/icons/hicolor/256x256/apps/safehouse-sly2.png'
        old_icon = home / '.local/share/icons/hicolor/128x128/apps/safehouse-sly2.png'  # before 2026-11-05: the SteamGridDB icon
        old_icon.parent.mkdir(parents=True); old_icon.write_bytes((art / 'sly2/icon.png').read_bytes())
        logo_icon = pathlib.Path(__file__).resolve().parent.parent / 'launcher/assets/shortcut/sly2-logo.png'

        r = run('--desktop-shortcut-status'); print(r.stdout.strip())
        check(r.returncode == 0 and f'sly2 desktop: {desk}/safehouse-sly2.desktop (none)' in r.stdout, 'status: desktop from user-dirs.dirs, none yet')

        r = run('--add-desktop-shortcut', 'sly2', '--menu'); print(r.stdout.strip(), r.stderr.strip())
        check(r.returncode == 0 and '(created) trusted' in r.stdout, 'add sly2 (desktop + menu)')
        f = desk / 'safehouse-sly2.desktop'
        check(f.is_file() and (menu / 'safehouse-sly2.desktop').is_file(), 'desktop and menu files written')
        if f.is_file():
            e = entry(f)
            argv = exec_argv(e['Exec'])
            check(argv == [str(exe), '--play', 'sly2'], f'Exec runs the launcher: {argv}')
            check(unescape_string(e['Path']) == str(exe.parent), 'Path = the program folder')
            check(e['Type'] == 'Application' and e['Name'] == 'Sly 2: Band of Thieves' and e['Categories'] == 'Game;'
                  and e['Terminal'] == 'false', 'Type, Name, Categories, Terminal')
            check(e['StartupWMClass'] == 'Sly2-Release', 'StartupWMClass = the Sly 2 window class')
            check(e['Icon'] == str(icon) and icon.read_bytes() == logo_icon.read_bytes(), 'icon (the logo, 256x256) written to hicolor and named by path')
            check(not old_icon.exists(), 'the old 128x128 icon removed')
            try:
                from PIL import Image
                with Image.open(icon) as im: check(im.size == (256, 256) and im.mode == 'RGBA', f'icon is a square RGBA PNG: {im.size} {im.mode}')
            except ImportError:
                check(icon.read_bytes()[:8] == b'\x89PNG\r\n\x1a\n', 'icon is a PNG')
            mode = f.stat().st_mode
            check(bool(mode & stat.S_IXUSR) and bool(mode & stat.S_IXOTH) and not mode & stat.S_IWOTH, 'executable, not world-writable')
            check(entry(menu / 'safehouse-sly2.desktop') == e, 'menu copy is the same entry')
            if shutil.which('desktop-file-validate'):
                v = subprocess.run(['desktop-file-validate', str(f)], capture_output=True, text=True)
                check(v.returncode == 0, 'desktop-file-validate: ' + (v.stdout + v.stderr).strip())
        log = gio_log.read_text().splitlines() if gio_log.exists() else []
        check(log == [f'set|{f}|metadata::trusted|true|'], f'gio marks only the desktop file trusted: {log}')

        before = f.stat().st_mtime_ns
        r = run('--add-desktop-shortcut', 'sly2'); print(r.stdout.strip())
        check(r.returncode == 0 and '(up to date)' in r.stdout and f.stat().st_mtime_ns == before, 'adding again: up to date, not rewritten')
        check(sorted(p.name for p in desk.iterdir()) == ['safehouse-sly2.desktop'], 'no duplicate, no temporary files on the desktop')

        # The AppImage: $APPIMAGE is the program, with characters that need quoting.
        image = tmp / 'My Games' / 'Safe$House "x" `y`\\z.AppImage'
        r = run('--add-desktop-shortcut', 'sly2', e={**env, 'APPIMAGE': str(image)}); print(r.stdout.strip())
        check(r.returncode == 0 and '(updated)' in r.stdout, 'moved to an AppImage: updated in place')
        e = entry(f)
        check(exec_argv(e['Exec']) == [str(image), '--play', 'sly2'], f'Exec = $APPIMAGE, quoted: {e["Exec"]}')
        check(unescape_string(e['Path']) == str(image.parent), 'Path = the AppImage folder')
        check(sorted(p.name for p in desk.iterdir()) == ['safehouse-sly2.desktop'], 'still one file')

        r = run('--add-desktop-shortcut', 'sly1', e={**env, 'SAFEHOUSE_GIO': 'none'}); print(r.stdout.strip())
        f1 = desk / 'safehouse-sly1.desktop'
        check(r.returncode == 0 and 'not marked trusted' in r.stdout and f1.stat().st_mode & stat.S_IXUSR, 'sly1 without gio: still executable')
        check(f1.is_file() and entry(f1)['StartupWMClass'] == 'Sly Cooper and the Thievius Raccoonus'
              and entry(f1)['Name'] == 'Sly Cooper and the Thievius Raccoonus', 'sly1: name and window class')
        check(not (menu / 'safehouse-sly1.desktop').exists(), 'sly1: no menu entry without --menu')

        r = run('--add-desktop-shortcut', 'sly3')
        check(r.returncode != 0 and 'not available for Sly 3' in r.stderr and not (desk / 'safehouse-sly3.desktop').exists(), 'sly3 refused')

        # Folder choice: XDG_DESKTOP_DIR from the environment without user-dirs.dirs; ~/Desktop (created) without either,
        # or when user-dirs.dirs says $HOME (no desktop folder).
        (home / '.config/user-dirs.dirs').unlink()
        other = tmp / 'elsewhere/Desk'
        r = run('--add-desktop-shortcut', 'sly1', e={**env, 'XDG_DESKTOP_DIR': str(other)})
        check(r.returncode == 0 and (other / 'safehouse-sly1.desktop').is_file(), 'XDG_DESKTOP_DIR from the environment')
        r = run('--add-desktop-shortcut', 'sly1')
        check(r.returncode == 0 and (home / 'Desktop/safehouse-sly1.desktop').is_file(), '~/Desktop by default (created)')
        (home / '.config/user-dirs.dirs').write_text('XDG_DESKTOP_DIR="$HOME/"\n')
        (home / 'Desktop/safehouse-sly1.desktop').unlink()
        r = run('--add-desktop-shortcut', 'sly1')
        check(r.returncode == 0 and (home / 'Desktop/safehouse-sly1.desktop').is_file() and not (home / 'safehouse-sly1.desktop').exists(),
              'XDG_DESKTOP_DIR=$HOME: ~/Desktop, not the home folder')
        # XDG_DATA_HOME moves the menu and icon.
        data = tmp / 'data'
        r = run('--add-desktop-shortcut', 'sly2', '--menu', e={**env, 'XDG_DATA_HOME': str(data)})
        check(r.returncode == 0 and (data / 'applications/safehouse-sly2.desktop').is_file()
              and (data / 'icons/hicolor/256x256/apps/safehouse-sly2.png').is_file(), 'XDG_DATA_HOME: menu entry and icon there')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f'{len(failures)} failure(s)' if failures else 'all passed')
    return 1 if failures else 0

if __name__ == '__main__':
    sys.exit(main())
