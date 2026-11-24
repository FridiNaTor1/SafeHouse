#!/usr/bin/env python3
"""Build the SafeHouse Android app: native libraries (launcher, extractor, Sly 1, Sly 2, Sly 3) and a debug-signed APK.

Steps: the locked NDK dependencies (bootstrap-dependencies.py, skipped when complete), the native CMake build
(cmake/android.cmake), staging of the JNI libraries and notices under build/android-package/<abi>/, and Gradle's
assembleDebug of android/. No game data, ISO, save or extracted file is ever staged: the player imports their own disc
image in the app. Debug signing is for testing; a store release needs its own signing configuration.

Builds run at low priority (nice 19, idle I/O) with at most --parallel jobs, so the machine stays usable.
"""
import argparse
import os
import re
from pathlib import Path
import hashlib
import shutil
import subprocess
import sys
import urllib.request
import zipfile

NDK_VERSION = '29.0.14206865'
ROOT = Path(__file__).resolve().parents[2]
# The Khronos OpenXR loader for Android (Apache-2.0; licenses/openxr/README.txt): Sly 2's VR mode on a Meta Quest
# (Sly2-Release/vr.cpp opens it at run time). Pinned by checksum.
OPENXR_LOADER_AAR = ('https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.63/'
                     'openxr_loader_for_android-1.1.63.aar')
OPENXR_LOADER_SHA256 = '622419d2f6741c3443a3beb4779af0764318edd01830de967f24c741ebcded73'


def openxr_loader(deps, abi):
    """libopenxr_loader.so for the ABI, from the pinned .aar (downloaded once into build/android-deps/downloads)."""
    archive = deps / 'downloads' / 'openxr_loader_for_android-1.1.63.aar'
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != OPENXR_LOADER_SHA256:
        archive.parent.mkdir(parents=True, exist_ok=True)
        print('+ download ' + OPENXR_LOADER_AAR, flush=True)
        with urllib.request.urlopen(OPENXR_LOADER_AAR, timeout=120) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != OPENXR_LOADER_SHA256:
            raise SystemExit('OpenXR loader archive checksum mismatch')
        archive.write_bytes(data)
    target = deps / abi / 'openxr' / 'libopenxr_loader.so'
    target.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as aar:
        target.write_bytes(aar.read('jni/' + abi + '/libopenxr_loader.so'))
    return target


def run(command, environment=None, cwd=None):
    command = ['nice', '-n', '19', 'ionice', '-c3'] + [str(c) for c in command]
    print('+ ' + ' '.join(command), flush=True)
    subprocess.run(command, check=True, env=environment, cwd=cwd)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--sdk', type=Path, default=Path(os.environ.get('ANDROID_HOME', Path.home() / 'Android/Sdk')))
    parser.add_argument('--abi', choices=('arm64-v8a', 'x86_64'), default='arm64-v8a')
    parser.add_argument('--gradle', type=Path, default=Path.home() / 'Documents/tools/gradle-9.6.1/bin/gradle')
    parser.add_argument('--java-home', type=Path,
                        default=Path(os.environ['JAVA_HOME']) if os.environ.get('JAVA_HOME') else Path.home() / 'Documents/tools/jdk-26.0.2')
    parser.add_argument('--parallel', type=int, default=4)
    parser.add_argument('--cache', type=Path, action='append', default=[],
                        help='folders with checksum-matching dependency archives (bootstrap-dependencies.py --cache)')
    parser.add_argument('--native-only', action='store_true', help='stop after the native build and staging')
    args = parser.parse_args()
    ndk = args.sdk / 'ndk' / NDK_VERSION
    if not (ndk / 'build/cmake/android.toolchain.cmake').is_file():
        parser.error('Android NDK ' + NDK_VERSION + ' is missing under ' + str(args.sdk))
    deps = ROOT / 'build/android-deps'
    install = deps / args.abi / 'install'
    bootstrap = [sys.executable, ROOT / 'scripts/android/bootstrap-dependencies.py', '--sdk', args.sdk, '--abi', args.abi,
                 '--prefix', deps, '--parallel', args.parallel]
    for cache in args.cache:
        bootstrap += ['--cache', cache]
    run(bootstrap)

    # The release version, its UTC build time and commit (CI's SAFEHOUSE_VERSION / SAFEHOUSE_BUILD_DATE /
    # CANE_GIT_HASH) for the launcher (Settings > Launcher, the update check) and Sly 2's debug menu, as on the desktop.
    # Always passed, so a development build after a release build in the same folder is a development build again.
    version = os.environ.get('SAFEHOUSE_VERSION', '')
    if version and not re.fullmatch(r'\d+\.\d+\.\d+', version):
        parser.error('SAFEHOUSE_VERSION must be x.y.z, got ' + version)
    native = ROOT / 'build' / ('android-' + ('arm64' if args.abi == 'arm64-v8a' else 'x86_64'))
    run(['cmake', '-S', ROOT, '-B', native, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_TOOLCHAIN_FILE=' + str(ndk / 'build/cmake/android.toolchain.cmake'), '-DANDROID_ABI=' + args.abi,
         '-DANDROID_PLATFORM=android-21', '-DANDROID_STL=c++_shared', '-DCANE_ANDROID_DEPENDENCY_ROOT=' + str(install),
         '-DSAFEHOUSE_VERSION=' + version, '-DSAFEHOUSE_BUILD_DATE=' + os.environ.get('SAFEHOUSE_BUILD_DATE', ''),
         '-DCANE_GIT_HASH=' + os.environ.get('CANE_GIT_HASH', '')])
    run(['cmake', '--build', native, '--parallel', args.parallel])

    stage = ROOT / 'build/android-package' / args.abi
    jni = stage / 'jniLibs' / args.abi
    if jni.exists():
        shutil.rmtree(jni)
    jni.mkdir(parents=True)
    triple = 'aarch64-linux-android' if args.abi == 'arm64-v8a' else 'x86_64-linux-android'
    libraries = [native / 'lib' / name for name in ('libsafehouse.so', 'libcane-extractor.so', 'libSly2-Release.so')]
    # (Sly 3: in the same app unless the native build was configured with -DCANE_BUILD_SLY3=OFF)
    if (native / 'lib/libSly3-Release.so').is_file() or 'CANE_BUILD_SLY3:BOOL=ON' in (native / 'CMakeCache.txt').read_text():
        libraries.append(native / 'lib/libSly3-Release.so')
    # (Sly 1, from the Sly1/ submodule: likewise, -DCANE_BUILD_SLY1=OFF leaves it out)
    if (native / 'lib/libSly1-Release.so').is_file() or 'CANE_BUILD_SLY1:BOOL=ON' in (native / 'CMakeCache.txt').read_text():
        libraries.append(native / 'lib/libSly1-Release.so')
    # (custom Vulkan drivers, arm64: libadrenotools' hooks, loaded from the native library folder; cmake/android.cmake)
    if args.abi == 'arm64-v8a':
        libraries += [native / 'lib' / name for name in ('libhook_impl.so', 'libmain_hook.so')]
    libraries += [install / 'lib' / name for name in ('libSDL2.so', 'libavcodec.so', 'libavformat.so', 'libavutil.so', 'libswscale.so')]
    libraries.append(ndk / 'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib' / triple / 'libc++_shared.so')
    libraries.append(openxr_loader(deps, args.abi))
    strip = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'
    for library in libraries:
        if not library.is_file():
            raise SystemExit('missing native library: ' + str(library))
        target = jni / library.name
        shutil.copy2(library, target)
        subprocess.run([str(strip), '--strip-unneeded', str(target)], check=True)
    assets = stage / 'assets'
    notices = assets / 'licenses'
    if assets.exists():
        shutil.rmtree(assets)
    shutil.copytree(ROOT / 'licenses', notices)
    shutil.copy2(ROOT / 'THIRD_PARTY_NOTICES.md', notices / 'THIRD_PARTY_NOTICES.md')
    shutil.copy2(ROOT / 'android/SDL-LICENSE.txt', notices / 'SDL-LICENSE.txt')
    for name in ('ffmpeg', 'sdl2', 'faudio'):
        source = install / 'licenses' / name
        if source.is_dir():
            shutil.copytree(source, notices / ('android-' + name), dirs_exist_ok=True)
    print('Staged native libraries: ' + str(jni))
    if args.native_only:
        return

    if not args.gradle.is_file():
        parser.error('Gradle 9.6.1 is missing: --gradle <gradle-9.6.1/bin/gradle>')
    if not (args.java_home / 'bin/java').is_file():
        parser.error('A JDK is missing: set JAVA_HOME or --java-home')
    environment = {**os.environ, 'ANDROID_HOME': str(args.sdk), 'JAVA_HOME': str(args.java_home),
                   'PATH': str(args.java_home / 'bin') + os.pathsep + os.environ.get('PATH', '')}
    gradle_build = stage / 'gradle-app'
    # (SAFEHOUSE_ANDROID_KEYSTORE set: the release APK signed with the SafeHouse key; else the debug APK)
    release = bool(os.environ.get('SAFEHOUSE_ANDROID_KEYSTORE'))
    variant = 'release' if release else 'debug'
    # (the versionName / versionCode: android/app/build.gradle; SAFEHOUSE_BUILD_NUMBER: CI's run number)
    base = re.search(r'^project\(ProjectCane VERSION (\d+\.\d+)\.\d+', (ROOT / 'CMakeLists.txt').read_text(), re.M)
    build_number = os.environ.get('SAFEHOUSE_BUILD_NUMBER', '') or '0'
    if not base or not build_number.isdigit():
        parser.error('no project version in CMakeLists.txt, or SAFEHOUSE_BUILD_NUMBER is not a number')
    run([args.gradle, '-p', ROOT / 'android', 'assembleRelease' if release else 'assembleDebug', '--console=plain',
         '--max-workers=2', '--no-daemon', '--project-cache-dir', stage / 'gradle-project-cache',
         '-PsafehouseAppBuildDir=' + str(gradle_build), '-PsafehouseJniDir=' + str(stage / 'jniLibs'),
         '-PsafehouseAssetsDir=' + str(assets), '-PsafehouseBaseVersion=' + base[1], '-PsafehouseBuildNumber=' + build_number]
        + (['-PsafehouseVersion=' + version] if version else []), environment)
    apk = gradle_build / 'outputs/apk' / variant / ('app-' + variant + '.apk')
    dist = ROOT / 'dist/android'
    dist.mkdir(parents=True, exist_ok=True)
    out = dist / ('SafeHouse-Android-' + args.abi + ('' if release else '-debug') + '.apk')
    shutil.copy2(apk, out)
    # The package's check: every staged native library (Sly 1, 2 and 3 among them) is in the APK, and both game
    # activities are in its code (GameActivity, FlatGameActivity for Sly 1 and Sly 3).
    with zipfile.ZipFile(out) as package:
        names = set(package.namelist())
        missing = ['lib/' + args.abi + '/' + library.name for library in libraries
                   if 'lib/' + args.abi + '/' + library.name not in names]
        code = b''.join(package.read(name) for name in names if re.fullmatch(r'classes\d*\.dex', name))
    missing += [name for name in ('GameActivity', 'FlatGameActivity') if ('Lorg/safehouse/android/' + name + ';').encode() not in code]
    if missing:
        raise SystemExit('the APK misses: ' + ', '.join(missing))
    print('APK: ' + str(out) + ' (' + str(out.stat().st_size) + ' bytes; ' + ', '.join(l.name for l in libraries[:4]) + ', ...)')


if __name__ == '__main__':
    main()
