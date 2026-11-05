#!/usr/bin/env python3
"""Packages a completed macOS build as SafeHouse.app inside a .dmg and a .zip (see .github/workflows/macos.yml)."""
import argparse
import hashlib
import os
import pathlib
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from dylib_bundler import DylibBundler  # noqa: E402

PRODUCT = "SafeHouse"
BUNDLE_ID = "app.safehouse.launcher"
LAUNCHER = "safehouse"
EXECUTABLES = (LAUNCHER, "Sly2-Release", "cane-extractor", "Sly1BrxRebuilder", "Sly1PalExtractor")
GPL_FLAGS = ("--enable-gpl", "--enable-nonfree", "--enable-version3")


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def project_version(source):
    text = (source / "CMakeLists.txt").read_text()
    match = re.search(r"project\(ProjectCane VERSION ([0-9.]+)", text)
    return match.group(1) if match else "0.0.0"


def git_describe(source):
    try:
        return subprocess.run(["git", "-C", str(source), "describe", "--tags", "--always", "--dirty"],
                              check=True, capture_output=True, text=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def write_checksum(path):
    checksum = path.with_name(path.name + ".sha256")
    checksum.write_text(f"{sha256_file(path)}  {path.name}\n")
    return checksum


class AppBundle:
    def __init__(self, root, version, build_id):
        self.root = pathlib.Path(root)
        self.contents = self.root / "Contents"
        self.macos = self.contents / "MacOS"
        self.frameworks = self.contents / "Frameworks"
        self.resources = self.contents / "Resources"
        self.version = version
        self.build_id = build_id

    def create(self, prefix):
        if self.root.exists():
            shutil.rmtree(self.root)
        for folder in (self.macos, self.frameworks, self.resources):
            folder.mkdir(parents=True)
        for name in EXECUTABLES:
            binary = prefix / "bin" / name
            if not binary.is_file():
                raise SystemExit(f"package: {binary} is missing from the install prefix")
            shutil.copy2(binary, self.macos / name)
        for folder in ("share", "licenses"):
            if (prefix / folder).is_dir():
                shutil.copytree(prefix / folder, self.resources / folder, symlinks=True)
        for document in prefix.glob("*.md"):
            shutil.copy2(document, self.resources / document.name)
        (self.resources / "BUILD-ID").write_text(self.build_id + "\n")
        self._write_plist()

    def _write_plist(self):
        info = {
            "CFBundleDevelopmentRegion": "en",
            "CFBundleDisplayName": PRODUCT,
            "CFBundleExecutable": LAUNCHER,
            "CFBundleIconFile": f"{PRODUCT}.icns",
            "CFBundleIdentifier": BUNDLE_ID,
            "CFBundleInfoDictionaryVersion": "6.0",
            "CFBundleName": PRODUCT,
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": self.version,
            "CFBundleVersion": self.version,
            "LSApplicationCategoryType": "public.app-category.games",
            "LSMinimumSystemVersion": "12.0",
            "NSHighResolutionCapable": True,
            "NSSupportsAutomaticGraphicsSwitching": True,
        }
        with open(self.contents / "Info.plist", "wb") as handle:
            plistlib.dump(info, handle)

    def install_icon(self, png):
        with tempfile.TemporaryDirectory() as temporary:
            iconset = pathlib.Path(temporary) / f"{PRODUCT}.iconset"
            iconset.mkdir()
            for size in (16, 32, 128, 256):
                run("sips", "-z", size, size, png, "--out", iconset / f"icon_{size}x{size}.png",
                    stdout=subprocess.DEVNULL)
                run("sips", "-z", size * 2, size * 2, png, "--out", iconset / f"icon_{size}x{size}@2x.png",
                    stdout=subprocess.DEVNULL)
            run("iconutil", "-c", "icns", iconset, "-o", self.resources / f"{PRODUCT}.icns")

    def bundle_libraries(self, search_dirs):
        bundler = DylibBundler(self.frameworks, "@executable_path/../Frameworks", search_dirs)
        for name in EXECUTABLES:
            bundler.bundle_executable(self.macos / name)
        problems = bundler.verify([self.macos / name for name in EXECUTABLES] + list(self.frameworks.iterdir()))
        if problems:
            raise SystemExit("package: unresolved libraries:\n  " + "\n  ".join(problems))
        return sorted(p.name for p in self.frameworks.iterdir())

    def check_ffmpeg_licence(self):
        for library in self.frameworks.glob("libavcodec*.dylib"):
            configuration = subprocess.run(["strings", str(library)], check=True, capture_output=True,
                                           text=True).stdout
            line = next((l for l in configuration.splitlines() if "--prefix=" in l), "")
            if any(flag in line.split() for flag in GPL_FLAGS):
                raise SystemExit(f"package: {library.name} is a GPL/non-free FFmpeg build:\n  {line}")
            (self.resources / "licenses").mkdir(exist_ok=True)
            (self.resources / "licenses" / "ffmpeg-build-configuration.txt").write_text(line + "\n")

    def codesign(self, identity):
        quiet = {"stderr": subprocess.DEVNULL}
        for library in self.frameworks.iterdir():
            run("codesign", "--force", "--sign", identity, library, **quiet)
        for name in EXECUTABLES:
            if name != LAUNCHER:
                run("codesign", "--force", "--sign", identity, self.macos / name, **quiet)
        run("codesign", "--force", "--sign", identity, self.root, **quiet)
        run("codesign", "--verify", "--deep", "--strict", self.root)


def make_dmg(app, output):
    with tempfile.TemporaryDirectory() as temporary:
        stage = pathlib.Path(temporary) / "dmg"
        stage.mkdir()
        shutil.copytree(app, stage / app.name, symlinks=True)
        os.symlink("/Applications", stage / "Applications")
        # hdiutil sometimes fails with "Resource busy" on GitHub's macOS runners; it passes when run again.
        for attempt in range(1, 6):
            output.unlink(missing_ok=True)
            try:
                run("hdiutil", "create", "-quiet", "-volname", PRODUCT, "-srcfolder", stage, "-ov", "-format", "UDZO",
                    output)
                return
            except subprocess.CalledProcessError:
                if attempt == 5:
                    raise
                print(f"hdiutil create failed (attempt {attempt}), retrying", flush=True)
                time.sleep(5 * attempt)


def make_zip(app, output):
    output.unlink(missing_ok=True)
    run("ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", app, output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True, type=pathlib.Path)
    parser.add_argument("--output", default="dist", type=pathlib.Path)
    parser.add_argument("--ffmpeg", type=pathlib.Path, help="prefix of scripts/macos/ffmpeg.sh")
    parser.add_argument("--identity", default=os.environ.get("CODESIGN_IDENTITY", "-"))
    parser.add_argument("--no-dmg", action="store_true")
    args = parser.parse_args()

    source = pathlib.Path(__file__).resolve().parents[2]
    build = args.build.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    arch = platform.machine()
    name = f"{PRODUCT}-Launcher-macOS-{arch}"
    # The release version CI embeds (SAFEHOUSE_VERSION, x.y.z); without it the CMake project version.
    version = os.environ.get("SAFEHOUSE_VERSION") or project_version(source)
    build_id = git_describe(source) or version

    prefix = output / f"{name}-prefix"
    if prefix.exists():
        shutil.rmtree(prefix)
    run("cmake", "--install", build, "--config", "Release", "--prefix", prefix, stdout=subprocess.DEVNULL)

    app = AppBundle(output / f"{PRODUCT}.app", version, build_id)
    app.create(prefix)
    app.install_icon(source / "launcher" / "assets" / "safehouse-icon.png")
    search_dirs = [d for d in build.rglob("*") if d.is_dir() and any(d.glob("*.dylib"))]
    if args.ffmpeg:
        search_dirs.append(args.ffmpeg.resolve() / "lib")
    libraries = app.bundle_libraries(search_dirs)
    app.check_ffmpeg_licence()
    if args.ffmpeg and (args.ffmpeg / "COPYING.LGPLv2.1").is_file():
        shutil.copy2(args.ffmpeg / "COPYING.LGPLv2.1", app.resources / "licenses" / "ffmpeg-COPYING.LGPLv2.1")
    app.codesign(args.identity)
    shutil.rmtree(prefix)

    print(f"{app.root}: {', '.join(libraries)}")
    archive = output / f"{name}.zip"
    make_zip(app.root, archive)
    print(write_checksum(archive).read_text().strip())
    if not args.no_dmg:
        dmg = output / f"{name}.dmg"
        make_dmg(app.root, dmg)
        print(write_checksum(dmg).read_text().strip())


if __name__ == "__main__":
    main()
