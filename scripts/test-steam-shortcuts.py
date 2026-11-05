#!/usr/bin/env python3
"""Add to Steam against a fake Steam tree: safehouse --add-to-steam / --steam-status with SAFEHOUSE_STEAM_ROOT.

Usage: test-steam-shortcuts.py SAFEHOUSE [ART_DIR]
Never touches a real Steam folder or profile: HOME and the XDG folders point into a temporary directory too.
"""
import os, pathlib, shutil, struct, subprocess, sys, tempfile, zlib

def parse(data):
    pos = 0
    def cstr():
        nonlocal pos
        end = data.index(b'\0', pos); s = data[pos:end].decode('utf-8'); pos = end + 1; return s
    def children():
        nonlocal pos
        out = []
        while True:
            t = data[pos]; pos += 1
            if t == 8: return out
            k = cstr()
            if t == 0: out.append((k, children()))
            elif t == 1: out.append((k, cstr()))
            elif t == 2: out.append((k, struct.unpack_from('<I', data, pos)[0])); pos += 4
            else: raise ValueError(f'type {t}')
    root = children()
    assert pos == len(data), 'trailing data'
    return root

def write(nodes):
    out = bytearray()
    for k, v in nodes:
        if isinstance(v, list): out += b'\0' + k.encode() + b'\0' + write(v)
        elif isinstance(v, str): out += b'\1' + k.encode() + b'\0' + v.encode() + b'\0'
        else: out += b'\2' + k.encode() + b'\0' + struct.pack('<I', v)
    return bytes(out + b'\x08')

def entries(file):
    root = dict(parse(file.read_bytes()))
    return [dict(e) for _, e in root['shortcuts']]

failures = []
def check(ok, what):
    print(('ok   ' if ok else 'FAIL ') + what)
    if not ok: failures.append(what)

def main():
    exe = pathlib.Path(sys.argv[1]).resolve()
    art = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else pathlib.Path(__file__).resolve().parent.parent / 'launcher/assets/steam').resolve()
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='safehouse-steam-'))
    try:
        steam = tmp / 'Steam'
        a1, a2 = steam / 'userdata/12345', steam / 'userdata/67890'
        (a1 / 'config').mkdir(parents=True); a2.mkdir(parents=True); (steam / 'userdata/0').mkdir()
        foreign = write([('shortcuts', [('0', [('appid', 0x81234567), ('AppName', 'Other game'), ('Exe', '"/usr/bin/other"'),
                                                ('StartDir', '"/usr/bin"'), ('LaunchOptions', '-x'), ('tags', [('0', 'Favorite')])])])])
        (a1 / 'config/shortcuts.vdf').write_bytes(foreign)
        env = {**os.environ, 'HOME': str(tmp / 'home'), 'XDG_CONFIG_HOME': str(tmp / 'config'), 'XDG_DATA_HOME': str(tmp / 'data'),
               'XDG_STATE_HOME': str(tmp / 'state'), 'SAFEHOUSE_STEAM_ROOT': str(steam), 'SAFEHOUSE_STEAM_ART': str(art)}
        env.pop('APPIMAGE', None)
        run = lambda *a, e=env: subprocess.run([str(exe), *a], env=e, capture_output=True, text=True, timeout=60)

        r = run('--steam-status'); print(r.stdout.strip())
        check(r.returncode == 0 and '2 account(s)' in r.stdout and 'not running' in r.stdout and 'sly2: not in Steam' in r.stdout, 'status before')
        r = run('--add-to-steam', 'sly2'); print(r.stdout.strip(), r.stderr.strip())
        check(r.returncode == 0 and 'added 2' in r.stdout, 'add sly2 to both accounts')
        name = 'Sly 2: Band of Thieves'
        quoted = '"' + str(exe) + '"'
        appid = zlib.crc32((quoted + name).encode()) | 0x80000000
        for account in (a1, a2):
            es = entries(account / 'config/shortcuts.vdf')
            ours = [e for e in es if e.get('AppName') == name]
            check(len(ours) == 1, f'{account.name}: one Sly 2 entry')
            if not ours: continue
            e = ours[0]
            check(e['appid'] == appid and e['Exe'] == quoted and e['LaunchOptions'] == '--play sly2', f'{account.name}: app id {appid}, exe, options')
            check(e['StartDir'] == '"' + str(exe.parent) + '"' and e['AllowOverlay'] == 1, f'{account.name}: start dir, overlay')
            check(dict(e['tags']).get('0') == 'SafeHouse' or 'SafeHouse' in dict(e['tags']).values(), f'{account.name}: SafeHouse tag')
            grid = account / 'config/grid'
            for stem in (f'{appid}p', f'{appid}', f'{appid}_hero', f'{appid}_logo', f'{appid}_icon'):
                check(any((grid / (stem + x)).exists() for x in ('.png', '.jpg')), f'{account.name}: art {stem}')
            check(e['icon'].endswith(f'{appid}_icon.png'), f'{account.name}: icon field')
        es = entries(a1 / 'config/shortcuts.vdf')
        check(es[0]['AppName'] == 'Other game' and es[0]['appid'] == 0x81234567 and dict(es[0]['tags']) == {'0': 'Favorite'}, 'foreign entry kept')
        check((a1 / 'config/shortcuts.vdf').read_bytes().startswith(foreign[:-2]), 'foreign entry kept byte for byte')
        backups = sorted((a1 / 'config').glob('shortcuts.vdf.safehouse-*.bak'))
        check(len(backups) == 2 and all(b.read_bytes() == foreign for b in backups), 'backup of the original file (+ safehouse-original.bak)')
        check(not list((a2 / 'config').glob('*.bak')), 'no backup where there was no file')

        before = (a1 / 'config/shortcuts.vdf').read_bytes()
        r = run('--add-to-steam', 'sly2')
        check(r.returncode == 0 and 'added 0' in r.stdout and (a1 / 'config/shortcuts.vdf').read_bytes() == before, 'adding again: no duplicate, file unchanged')
        custom = a1 / f'config/grid/{appid}p.png'; custom.write_bytes(b'user art')
        run('--add-to-steam', 'sly2')
        check(custom.read_bytes() == b'user art', 'user artwork is not replaced')

        moved = tmp / 'Apps/SafeHouse-x86_64.AppImage'
        r = run('--add-to-steam', 'sly2', e={**env, 'APPIMAGE': str(moved)}); print(r.stdout.strip())
        es = [e for e in entries(a1 / 'config/shortcuts.vdf') if e.get('AppName') == name]
        newid = zlib.crc32(('"' + str(moved) + '"' + name).encode()) | 0x80000000
        check(len(es) == 1 and es[0]['Exe'] == '"' + str(moved) + '"' and es[0]['appid'] == newid and 'updated 2' in r.stdout,
              'moved AppImage: the same entry is updated (no duplicate)')
        check((a1 / f'config/grid/{newid}_hero.jpg').exists(), 'moved AppImage: art under the new app id')
        check((a1 / 'config/shortcuts.vdf.safehouse-original.bak').read_bytes() == foreign, 'the original backup is never overwritten')
        check(len(list((a1 / 'config').glob('shortcuts.vdf.safehouse-2*.bak'))) == 2, 'one backup per change, none overwritten')

        r = run('--add-to-steam', 'sly1'); r3 = run('--add-to-steam', 'sly3')
        es = entries(a1 / 'config/shortcuts.vdf')
        check(r.returncode == 0 and r3.returncode == 0 and len(es) == 4 and len({e['appid'] for e in es}) == 4, 'Sly 1 and Sly 3 added beside it')
        r = run('--steam-status'); print(r.stdout.strip())
        check('sly1: in Steam' in r.stdout and 'sly3: in Steam' in r.stdout, 'status after')

        damaged = a2 / 'config/shortcuts.vdf'
        damaged.write_bytes(b'\0shortcuts\0\0' + b'0\0\x09bad')
        good = (a1 / 'config/shortcuts.vdf').read_bytes()
        r = run('--add-to-steam', 'sly2', e={**env, 'APPIMAGE': str(tmp / 'Other.AppImage')})
        check(r.returncode != 0 and damaged.read_bytes() == b'\0shortcuts\0\0' + b'0\0\x09bad', 'damaged file: error, file untouched')

        if sys.platform.startswith('linux'):
            fake = tmp / 'steam'; shutil.copy('/bin/sleep', fake)
            p = subprocess.Popen([str(fake), '30'])
            try:
                (steam / 'steam.pid').write_text(str(p.pid))
                r = run('--steam-status'); print(r.stdout.strip())
                check(', running' in r.stdout, 'a running Steam is detected')
            finally:
                p.kill(); p.wait()
        r = run('--steam-status', e={**env, 'SAFEHOUSE_STEAM_ROOT': str(tmp / 'nowhere')})
        check('Steam: not found' in r.stdout, 'no Steam: not found')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('FAILED: ' + ', '.join(failures) if failures else 'all Steam shortcut checks passed')
    return 1 if failures else 0

sys.exit(main())
