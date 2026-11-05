#!/usr/bin/env python3
"""The PS3 HD import's source reader (platform/ps3disc.h) through Sly1-Release --ps3-hd-textures, on synthetic data:
the extracted folder, an ISO 9660 image and a .zip of it must read the same files, and the wrong sources must give
the sentences the launcher shows. No game data is used."""
import pathlib, struct, subprocess, sys, tempfile, zipfile

SECTOR = 2048


def both16(v): return struct.pack('<H', v) + struct.pack('>H', v)
def both32(v): return struct.pack('<I', v) + struct.pack('>I', v)


def record(name, lba, size, directory):
    n = name if isinstance(name, bytes) else name.encode()
    body = bytes([0]) + both32(lba) + both32(size) + bytes(7) + bytes([2 if directory else 0, 0, 0]) + both16(1) + bytes([len(n)]) + n
    if len(n) % 2 == 0: body += b'\0'
    return bytes([len(body) + 1]) + body


def iso(files):
    """A minimal ISO 9660 image: PS3_GAME/USRDIR/<game>/<file> for each 'GAME/FILE' -> bytes."""
    dirs = {'': None, 'PS3_GAME': None, 'PS3_GAME/USRDIR': None}
    for path in files:
        dirs['PS3_GAME/USRDIR/' + path.split('/')[0]] = None
    order = sorted(dirs)
    lba = {d: 18 + i for i, d in enumerate(order)}
    nxt = 18 + len(order)
    flba = {}
    for path, data in files.items():
        flba[path] = nxt
        nxt += max(1, (len(data) + SECTOR - 1) // SECTOR)
    image = bytearray(nxt * SECTOR)
    for d in order:
        entries = [record(b'\0', lba[d], SECTOR, True), record(b'\1', lba[d], SECTOR, True)]
        for c in order:
            if c and c.rsplit('/', 1)[0] == d if '/' in c else (c and d == ''):
                entries.append(record(c.rsplit('/', 1)[-1], lba[c], SECTOR, True))
        for path in files:
            if 'PS3_GAME/USRDIR/' + path.split('/')[0] == d:
                entries.append(record(path.split('/')[1] + ';1', flba[path], len(files[path]), False))
        blob = b''.join(entries)
        image[lba[d] * SECTOR:lba[d] * SECTOR + len(blob)] = blob
    for path, data in files.items():
        image[flba[path] * SECTOR:flba[path] * SECTOR + len(data)] = data
    pvd = bytearray(SECTOR)
    pvd[0:6] = b'\x01CD001'
    pvd[6] = 1
    root = record(b'\0', lba[''], SECTOR, True)
    pvd[156:156 + len(root)] = root
    image[16 * SECTOR:17 * SECTOR] = pvd
    image[17 * SECTOR:17 * SECTOR + 6] = b'\xffCD001'
    return bytes(image)


def wac(entries):
    out = struct.pack('<I', len(entries))
    for name, kind, sector, size in entries:
        out += name.encode().ljust(23, b'\0') + kind.encode() + struct.pack('<II', sector, size)
    return out


def main():
    sly1 = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='cane ps3 ') as folder:
        root = pathlib.Path(folder)
        inst = root / 'installation'
        (inst / 'Worlds' / 'A Level').mkdir(parents=True)
        (inst / 'Worlds' / 'A Level' / 'A Level.brx').write_bytes(b'PCBR' + b'\0' * 60)

        def run(source):
            r = subprocess.run([sly1, '--data-dir', str(inst), '--user-dir', str(root / 'user'), '--ps3-hd-textures', str(source), str(root / 'out')],
                               capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=60, env={'SDL_AUDIODRIVER': 'dummy', 'HOME': str(root)})
            errors = [l[len('ps3hd: error: '):] for l in r.stdout.splitlines() if l.startswith('ps3hd: error: ')]
            assert r.returncode != 0 and errors, (source, r.returncode, r.stdout[-2000:], r.stderr[-2000:])
            return errors[-1]

        def expect(source, words):
            message = run(source)
            assert words in message, (source, message)
            return message

        # wrong sources: the sentences the launcher shows
        (root / 'empty').mkdir()
        expect(root / 'empty', 'This folder is not The Sly Collection (PS3)')
        (root / 'notes.txt').write_text('hello')
        with zipfile.ZipFile(root / 'nothing.zip', 'w') as z: z.write(root / 'notes.txt', 'notes.txt')
        expect(root / 'nothing.zip', 'This zip file has no disc image (.iso) in it')
        expect(root / 'notes.txt', 'it has no ISO 9660 file system')
        expect(root / 'missing.iso', 'does not exist')
        other = iso({'SLY3/SLY3.WAL': b'\0' * 100})
        (root / 'other.iso').write_bytes(other)
        expect(root / 'other.iso', 'the disc has no SLY1 / SLY2 game files')

        # a table that is not one: the same message from the image, the zip and the folder
        files = {'SLY1/SLY.WAC': b'\0' * 64, 'SLY1/SLY.WAL': b'\0' * 5000, 'SLY2/SLY2.WAL': b'\0' * 100}
        (root / 'bad.iso').write_bytes(iso(files))
        with zipfile.ZipFile(root / 'bad.zip', 'w', zipfile.ZIP_DEFLATED) as z: z.write(root / 'bad.iso', 'The Sly Collection.iso')
        tree = root / 'tree'
        for path, data in files.items():
            (tree / 'PS3_GAME' / 'USRDIR' / path).parent.mkdir(parents=True, exist_ok=True)
            (tree / 'PS3_GAME' / 'USRDIR' / path).write_bytes(data)
        for source in [root / 'bad.iso', root / 'bad.zip', tree, tree / 'PS3_GAME', tree / 'PS3_GAME' / 'USRDIR']:
            expect(source, 'SLY1/SLY.WAC is not a table of levels')

        # a table with one level the installation does not have: read through to the end (multi-sector, both kinds)
        level = bytes(range(256)) * 20
        files = {'SLY1/SLY.WAC': wac([('a_level', 'W', 2, len(level)), ('a_level_f', 'W', 2, len(level)), ('music', 'M', 0, 10)]),
                 'SLY1/SLY.WAL': b'\x11' * (2 * SECTOR) + level, 'SLY2/SLY2.WAL': b'\0' * 100}
        (root / 'good.iso').write_bytes(iso(files))
        with zipfile.ZipFile(root / 'good.zip', 'w', zipfile.ZIP_DEFLATED) as z: z.write(root / 'good.iso', 'The Sly Collection.iso')
        for source in [root / 'good.iso', root / 'good.zip']:
            expect(source, 'No level of the disc matched')
        assert not (root / 'out' / 'Textures').exists() or not any((root / 'out' / 'Textures').iterdir())
    print('PS3 import source tests passed')


if __name__ == '__main__':
    main()
