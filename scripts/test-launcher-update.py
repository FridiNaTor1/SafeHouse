#!/usr/bin/env python3
"""The launcher's update check and in-place update against a local fake release (launcher/update.h).

  scripts/test-launcher-update.py LAUNCHER
      Linux: a fake release folder (bin/safehouse = LAUNCHER, the SafeHouse link, data/) is updated from a local
      HTTP server: the check, a damaged download (SHA-256), a download cut off midway, a folder that cannot be
      written (the swap is rolled back), an offline check, file:// and the curl program instead of libcurl, a
      successful update (data/ and portable.txt untouched) and an AppImage file replaced in place.

  scripts/test-launcher-update.py serve DIR --version X.Y.Z [--port N] [--date ISO] [--cut]
      Serves the packages in DIR as the latest release (http://127.0.0.1:N/releases/latest) for testing real
      packages: SAFEHOUSE_UPDATE_URL=<that URL> safehouse --update. --cut sends half of each package and closes.

Everything runs with a private HOME / XDG folders; nothing outside a temporary folder is touched.
"""
import argparse
import hashlib
import http.server
import json
import os
import pathlib
import shutil
import socketserver
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading

SOURCE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(SOURCE))
manifest_module = __import__('release-manifest')


class Release:
    """A folder of packages served as GitHub's 'latest release' API answer."""

    def __init__(self, folder, version, date, notes='## Changes\n\n- A test release\n'):
        self.folder, self.version, self.date, self.notes = pathlib.Path(folder), version, date, notes
        self.cut = False
        self.wrong_hash = False
        self.port = 0

    def manifest(self):
        packages = {}
        for key, name in manifest_module.PACKAGES.items():
            file = self.folder / name
            if file.is_file():
                digest = hashlib.sha256(file.read_bytes()).hexdigest()
                if self.wrong_hash:
                    digest = digest[::-1]
                packages[key] = {'file': name, 'size': file.stat().st_size, 'sha256': digest}
        return {'format': 1, 'version': self.version, 'tag': 'v' + self.version, 'commit': 'abcdef12', 'date': self.date,
                'packages': packages, 'updater': {k: v for k, v in manifest_module.UPDATER.items() if v in packages}}

    def api(self, base):
        files = [p['file'] for p in self.manifest()['packages'].values()] + [manifest_module.MANIFEST]
        prefix = base + ('/cut/' if self.cut else '/files/')
        return {'tag_name': 'v' + self.version, 'name': 'SafeHouse ' + self.version, 'body': self.notes,
                'html_url': base + '/page', 'published_at': self.date,
                'assets': [{'name': f, 'browser_download_url': (base + '/files/' if f == manifest_module.MANIFEST else prefix) + f}
                           for f in files]}


def serve(release):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            base = f'http://127.0.0.1:{self.server.server_address[1]}'
            if self.path == '/releases/latest':
                body = json.dumps(release.api(base)).encode()
            elif self.path == '/files/' + manifest_module.MANIFEST:
                body = json.dumps(release.manifest()).encode()
            elif self.path.startswith('/files/') or self.path.startswith('/cut/'):
                file = release.folder / self.path.split('/', 2)[2]
                if not file.is_file():
                    self.send_error(404)
                    return
                body = file.read_bytes()
                if self.path.startswith('/cut/'):
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body[:len(body) // 2])
                    self.wfile.flush()
                    self.close_connection = True
                    return
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = socketserver.ThreadingTCPServer(('127.0.0.1', release.port), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def private_env(root):
    env = {k: v for k, v in os.environ.items() if not k.startswith('XDG_') and k not in ('APPIMAGE', 'APPDIR')}
    env.update(HOME=str(root / 'home'), XDG_CONFIG_HOME=str(root / 'config'), XDG_DATA_HOME=str(root / 'share'),
               XDG_STATE_HOME=str(root / 'state'), XDG_CACHE_HOME=str(root / 'cache'))
    for key in ('HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_STATE_HOME', 'XDG_CACHE_HOME'):
        pathlib.Path(env[key]).mkdir(parents=True, exist_ok=True)
    return env


failures = []


def check(ok, what):
    print(('ok   ' if ok else 'FAIL ') + what, flush=True)
    if not ok:
        failures.append(what)


def run(binary, args, env, timeout=120):
    result = subprocess.run([str(binary), *args], env=env, capture_output=True, text=True, timeout=timeout)
    output = (result.stdout + result.stderr).strip()
    print(f'> {binary.name} {" ".join(args)} (exit {result.returncode})')
    for line in output.splitlines():
        print('    ' + line)
    return result.returncode, output


def make_folder(root, launcher, marker):
    """A release folder like the tar.gz's: bin/safehouse, the SafeHouse link, share/, data/ with a save."""
    root.mkdir(parents=True)
    (root / 'bin').mkdir()
    shutil.copy2(launcher, root / 'bin' / 'safehouse')
    os.symlink('safehouse', root / 'bin' / 'cane-launcher')
    os.symlink('bin/safehouse', root / 'SafeHouse')
    (root / 'share').mkdir()
    (root / 'share' / 'version.txt').write_text(marker + '\n')
    (root / 'README.md').write_text(marker + '\n')
    return root


def folder_tests(launcher, work):
    old = make_folder(work / 'installed' / 'SafeHouse-Launcher-Linux-x86_64', launcher, 'old')
    (old / 'data').mkdir()
    (old / 'data' / 'save.bin').write_bytes(b'my save')
    (old / 'portable.txt').write_text('portable\n')
    (old / 'share' / 'only-old.txt').write_text('kept\n')
    new = make_folder(work / 'pack' / 'SafeHouse-Launcher-Linux-x86_64', launcher, 'new')
    (new / 'share' / 'added.txt').write_text('new file\n')
    (new / 'portable.txt').write_text('from the package\n')
    (new / 'data').mkdir()
    (new / 'data' / 'save.bin').write_bytes(b'package data')
    releases = work / 'release'
    releases.mkdir()
    with tarfile.open(releases / 'SafeHouse-Launcher-Linux-x86_64.tar.gz', 'w:gz') as archive:
        archive.add(new, arcname=new.name)
    release = Release(releases, '0.1.57', '2026-11-17T09:12:44Z')
    server = serve(release)
    url = f'http://127.0.0.1:{server.server_address[1]}/releases/latest'
    env = private_env(work / 'profile')
    env.update(SAFEHOUSE_UPDATE_URL=url, SAFEHOUSE_UPDATE_TEST_VERSION='0.1.45', SAFEHOUSE_UPDATE_TEST_DATE='2026-11-05T19:11:09Z')
    binary = old / 'bin' / 'safehouse'

    def unchanged(what):
        check((old / 'share' / 'version.txt').read_text() == 'old\n' and not (old / 'share' / 'added.txt').exists()
              and (old / 'data' / 'save.bin').read_bytes() == b'my save', what)

    code, out = run(binary, ['--check-update'], env)
    check(code == 0 and 'install linux-tar' in out and 'newer, 12 days' in out and 'latest 0.1.57' in out, 'check: 0.1.57 is newer, 12 days')
    check('package SafeHouse-Launcher-Linux-x86_64.tar.gz' in out, 'check: the tar.gz updates this folder')

    code, out = run(binary, ['--check-update'], {**env, 'SAFEHOUSE_UPDATE_TEST_VERSION': '0.1.57'})
    check(code == 0 and 'up to date' in out, 'check: the same version is up to date')

    code, out = run(binary, ['--check-update'], {**env, 'SAFEHOUSE_UPDATE_URL': 'http://127.0.0.1:9/releases/latest'}, timeout=60)
    check(code == 1 and 'no connection' in out, 'offline: a short error, exit 1')

    release.wrong_hash = True
    code, out = run(binary, ['--update'], env)
    release.wrong_hash = False
    check(code == 1 and 'SHA-256 does not match' in out, 'damaged download: refused')
    unchanged('damaged download: nothing changed')

    release.cut = True
    code, out = run(binary, ['--update'], env)
    release.cut = False
    check(code == 1 and ('interrupted' in out or 'incomplete' in out), 'download cut off: refused')
    unchanged('download cut off: nothing changed')

    (old / 'share').chmod(0o555)
    code, out = run(binary, ['--update'], env)
    (old / 'share').chmod(0o755)
    check(code == 1 and 'previous version was kept' in out, 'folder not writable: the swap is rolled back')
    unchanged('folder not writable: the old files are back')
    check(os.path.islink(old / 'SafeHouse') and os.readlink(old / 'SafeHouse') == 'bin/safehouse', 'rollback: the SafeHouse link is back')

    code, out = run(binary, ['--update'], env)
    check(code == 0 and 'updated to 0.1.57' in out, 'update: exit 0')
    check((old / 'share' / 'version.txt').read_text() == 'new\n' and (old / 'share' / 'added.txt').is_file(), 'update: new files in place')
    check((old / 'share' / 'only-old.txt').is_file(), 'update: files the package does not have are left')
    check((old / 'data' / 'save.bin').read_bytes() == b'my save', 'update: data/ untouched')
    check((old / 'portable.txt').read_text() == 'portable\n', 'update: portable.txt untouched')
    check(os.path.islink(old / 'SafeHouse') and os.path.islink(old / 'bin' / 'cane-launcher'), 'update: links kept as links')
    check(os.access(old / 'bin' / 'safehouse', os.X_OK), 'update: the launcher is executable')
    check((old / '.update' / 'previous' / 'share' / 'version.txt').read_text() == 'old\n', 'update: the old files are kept aside')
    result = json.loads((work / 'profile' / 'config' / 'projectcane' / 'update-result.json').read_text()) \
        if (work / 'profile' / 'config' / 'projectcane' / 'update-result.json').is_file() else {}
    if not result:  # portable.txt: the profile is the folder's data/
        result_file = old / 'data' / 'config' / 'update-result.json'
        result = json.loads(result_file.read_text()) if result_file.is_file() else {}
    check(result.get('ok') is True and result.get('version') == '0.1.57', 'update: the result for the next start')

    # file:// and the curl program (no libcurl)
    file_api = work / 'file-api'
    file_api.mkdir()
    api = release.api('file://' + str(releases.resolve()))
    for asset in api['assets']:
        asset['browser_download_url'] = (releases.resolve() / asset['name']).as_uri()
    (releases / manifest_module.MANIFEST).write_text(json.dumps(release.manifest()))
    (file_api / 'latest.json').write_text(json.dumps(api))
    code, out = run(binary, ['--check-update'], {**env, 'SAFEHOUSE_UPDATE_URL': (file_api / 'latest.json').as_uri()})
    check(code == 0 and 'newer' in out, 'file:// release')
    if shutil.which('curl'):
        code, out = run(binary, ['--check-update'], {**env, 'SAFEHOUSE_UPDATE_NO_LIBCURL': '1'})
        check(code == 0 and 'newer' in out, 'the curl program instead of libcurl')
    server.shutdown()


def appimage_tests(launcher, work):
    folder = work / 'apps'
    folder.mkdir()
    target = folder / 'SafeHouse-Launcher-Linux-x86_64.AppImage'
    target.write_bytes(b'old appimage')
    target.chmod(0o750)
    releases = work / 'release-appimage'
    releases.mkdir()
    (releases / 'SafeHouse-Launcher-Linux-x86_64.AppImage').write_bytes(b'new appimage ' * 1000)
    release = Release(releases, '0.1.57', '2026-11-17T09:12:44Z')
    server = serve(release)
    env = private_env(work / 'profile-appimage')
    env.update(SAFEHOUSE_UPDATE_URL=f'http://127.0.0.1:{server.server_address[1]}/releases/latest',
               SAFEHOUSE_UPDATE_TEST_VERSION='0.1.45', SAFEHOUSE_UPDATE_TEST_DATE='2026-11-05T19:11:09Z', APPIMAGE=str(target))
    inode = target.stat().st_ino
    code, out = run(launcher, ['--update'], env)
    check(code == 0 and 'install linux-appimage' in out, 'AppImage: updated')
    check(target.read_bytes() == b'new appimage ' * 1000, 'AppImage: the new file at $APPIMAGE')
    check(stat.S_IMODE(target.stat().st_mode) == 0o750 and target.stat().st_ino != inode, 'AppImage: replaced by a rename, permissions kept')
    check(not any(p.name.endswith('.part') for p in folder.rglob('*')), 'AppImage: no partial download left')
    server.shutdown()


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'serve':
        parser = argparse.ArgumentParser()
        parser.add_argument('command')
        parser.add_argument('dir', type=pathlib.Path)
        parser.add_argument('--version', required=True)
        parser.add_argument('--date', default='2026-11-17T09:12:44Z')
        parser.add_argument('--port', type=int, default=8765)
        parser.add_argument('--cut', action='store_true')
        parser.add_argument('--wrong-hash', action='store_true')
        args = parser.parse_args()
        release = Release(args.dir, args.version, args.date)
        release.port, release.cut, release.wrong_hash = args.port, args.cut, args.wrong_hash
        server = serve(release)
        print(f'http://127.0.0.1:{server.server_address[1]}/releases/latest', flush=True)
        threading.Event().wait()
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    if not sys.platform.startswith('linux'):
        print('skipped: the update tests run on Linux')
        return
    launcher = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='safehouse-update-') as temporary:
        work = pathlib.Path(temporary)
        folder_tests(launcher, work)
        appimage_tests(launcher, work)
    if failures:
        sys.exit(f'{len(failures)} check(s) failed: ' + '; '.join(failures))
    print('all update checks passed')


if __name__ == '__main__':
    main()
