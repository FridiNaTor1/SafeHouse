#!/usr/bin/env python3
"""SHA256SUMS and SafeHouse-update.json for a folder of release packages.

  scripts/release-manifest.py DIR --version 0.1.42 [--tag v0.1.42] [--commit HASH] [--date 2026-11-05T12:00:00Z]

SafeHouse-update.json is what the launcher's update check reads (launcher/update.h): the version, commit and UTC
build date, every package's file name, size and SHA-256, and which package updates which kind of installation (the
Windows setup's folder is updated from the portable zip, without running the setup again).
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import sys

# package key -> file name pattern (the names scripts/package.py and scripts/macos/package.py write)
PACKAGES = {
    'linux-appimage': 'SafeHouse-Launcher-Linux-x86_64.AppImage',
    'linux-tar': 'SafeHouse-Launcher-Linux-x86_64.tar.gz',
    'linux-aarch64-appimage': 'SafeHouse-Launcher-Linux-aarch64.AppImage',
    'linux-aarch64-tar': 'SafeHouse-Launcher-Linux-aarch64.tar.gz',
    'windows-portable': 'SafeHouse-Launcher-Windows-x86_64-Portable.zip',
    'windows-setup': 'SafeHouse-Launcher-Windows-x86_64-Setup.exe',
    'windows-arm64-portable': 'SafeHouse-Launcher-Windows-arm64-Portable.zip',
    'windows-arm64-setup': 'SafeHouse-Launcher-Windows-arm64-Setup.exe',
    'macos-arm64': 'SafeHouse-Launcher-macOS-arm64.zip',
    'macos-arm64-dmg': 'SafeHouse-Launcher-macOS-arm64.dmg',
    'macos-x86_64': 'SafeHouse-Launcher-macOS-x86_64.zip',
    'switch-nro': 'SafeHouse.nro',
    'android-apk': 'SafeHouse-Android-arm64-v8a.apk',
    'macos-x86_64-dmg': 'SafeHouse-Launcher-macOS-x86_64.dmg',
}
# installation kind (launcher/update.h kindKey) -> package key
UPDATER = {
    'linux-appimage': 'linux-appimage',
    'linux-tar': 'linux-tar',
    'linux-aarch64-appimage': 'linux-aarch64-appimage',
    'linux-aarch64-tar': 'linux-aarch64-tar',
    'windows-portable': 'windows-portable',
    'windows-installed': 'windows-portable',
    'windows-arm64-portable': 'windows-arm64-portable',
    'windows-arm64-installed': 'windows-arm64-portable',
    'macos-arm64': 'macos-arm64',
    'macos-x86_64': 'macos-x86_64',
}
MANIFEST = 'SafeHouse-update.json'


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dir', type=pathlib.Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--tag')
    parser.add_argument('--commit', default='')
    parser.add_argument('--date', default=datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'))
    parser.add_argument('--require', action='append', default=[], help='package key that must be present (repeatable)')
    args = parser.parse_args()
    if not re.fullmatch(r'\d+\.\d+\.\d+', args.version):
        sys.exit(f'release-manifest: version must be x.y.z, got {args.version!r}')
    if not re.fullmatch(r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z', args.date):
        sys.exit(f'release-manifest: date must be YYYY-MM-DDTHH:MM:SSZ, got {args.date!r}')
    folder = args.dir
    packages = {}
    for key, name in PACKAGES.items():
        file = folder / name
        if file.is_file():
            packages[key] = {'file': name, 'size': file.stat().st_size, 'sha256': sha256(file)}
    missing = [key for key in args.require if key not in packages]
    if missing:
        sys.exit('release-manifest: missing packages: ' + ', '.join(missing))
    if not packages:
        sys.exit(f'release-manifest: no packages in {folder}')
    manifest = {
        'format': 1,
        'version': args.version,
        'tag': args.tag or 'v' + args.version,
        'commit': args.commit,
        'date': args.date,
        'packages': packages,
        'updater': {kind: key for kind, key in UPDATER.items() if key in packages},
    }
    (folder / MANIFEST).write_text(json.dumps(manifest, indent=2) + '\n')
    names = sorted(p['file'] for p in packages.values()) + [MANIFEST]
    lines = [f'{sha256(folder / n)}  {n}\n' for n in names]
    (folder / 'SHA256SUMS').write_text(''.join(lines))
    sys.stdout.write(''.join(lines))


if __name__ == '__main__':
    main()
