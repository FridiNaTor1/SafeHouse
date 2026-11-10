#!/usr/bin/env python3
"""Build locked native Android dependencies with the installed Android NDK.

Builds never use a desktop library for the Android target (SDL2, FAudio and
FFmpeg for the SafeHouse launcher and Sly 2). Sources and successful build identities are
shared between ABI builds; interrupted or changed builds are retried safely.
The target FFmpeg libraries remain shared for LGPL relinking.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import urllib.request

NDK_VERSION = '29.0.14206865'
ABI_TARGETS = {'arm64-v8a': ('aarch64', 'aarch64-linux-android', 'android-arm64'),
               'x86_64': ('x86_64', 'x86_64-linux-android', 'android-x86_64')}


def command(arguments, *, cwd=None, environment=None):
    print('+ ' + ' '.join(map(str, arguments)), flush=True)
    subprocess.run(list(map(str, arguments)), cwd=cwd, env=environment, check=True)


def digest(file):
    checksum = hashlib.sha256()
    with file.open('rb') as data:
        for block in iter(lambda: data.read(1024 * 1024), b''):
            checksum.update(block)
    return checksum.hexdigest()


def source_for(name, dependency, root, caches):
    suffix = '.tar.xz' if dependency['url'].endswith('.tar.xz') else '.tar.gz'
    archive = root / 'downloads' / (name + '-' + dependency['version'] + suffix)
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.is_file() or digest(archive) != dependency['sha256']:
        candidates = [folder / archive.name for folder in caches]
        # Existing project downloads have occasionally used a shorter name.
        candidates += [folder / (name + '.tar.gz') for folder in caches]
        if name == 'sdl2':
            candidates += [folder / ('SDL-' + dependency['version'] + '.tar.gz') for folder in caches]
        match = next((file for file in candidates if file.is_file() and digest(file) == dependency['sha256']), None)
        temporary = archive.with_suffix(archive.suffix + '.part')
        try:
            if match:
                shutil.copy2(match, temporary)
            else:
                with urllib.request.urlopen(dependency['url'], timeout=120) as response, temporary.open('wb') as data:
                    shutil.copyfileobj(response, data)
            if digest(temporary) != dependency['sha256']:
                raise RuntimeError('Downloaded checksum differs from the locked ' + name + ' release')
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    sources = root / 'sources'
    source = sources / dependency['source_directory']
    if not source.is_dir():
        sources.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as files:
            files.extractall(sources, filter='data')
    if not source.is_dir():
        raise RuntimeError('Archive does not contain expected source directory ' + str(source))
    return source


def complete(build, identity, required):
    marker = build / 'safehouse-build.json'
    return marker.is_file() and json.loads(marker.read_text()) == identity and all(file.is_file() for file in required)


def finish(build, identity):
    (build / 'safehouse-build.json').write_text(json.dumps(identity, indent=2) + '\n')


def cmake_build(source, build, install, options, extra, environment):
    args = ['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(install),
            '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
            '-DCMAKE_POLICY_VERSION_MINIMUM=3.5']
    if options:
        args += ['-DCMAKE_TOOLCHAIN_FILE=' + str(options.ndk / 'build/cmake/android.toolchain.cmake'),
                 '-DANDROID_ABI=' + options.abi, '-DANDROID_PLATFORM=android-' + str(options.api),
                 '-DANDROID_STL=c++_shared', '-DCMAKE_C_FLAGS=-ffp-contract=off',
                 '-DCMAKE_CXX_FLAGS=-ffp-contract=off',
                 '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384']
    args += extra
    command(args, environment=environment)
    command(['cmake', '--build', build, '--parallel', str(options.parallel if options else 4)], environment=environment)
    command(['cmake', '--install', build], environment=environment)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=Path(os.environ.get('ANDROID_HOME', Path.home() / 'Android/Sdk')))
    parser.add_argument('--ndk', type=Path)
    parser.add_argument('--abi', choices=ABI_TARGETS, default='arm64-v8a')
    parser.add_argument('--api', type=int, default=26)
    parser.add_argument('--prefix', type=Path, default=Path('build/android-deps'))
    parser.add_argument('--cache', type=Path, action='append', default=[], help='Reuse only checksum-matching locked archives')
    parser.add_argument('--parallel', type=int, default=4)
    parser.add_argument('--download-only', action='store_true')
    options = parser.parse_args(argv)
    if options.api < 26 or options.parallel < 1:
        parser.error('Android API must be at least 26 and parallelism must be positive')
    if platform.system() not in ('Linux', 'Darwin'):
        parser.error('Use Linux, macOS, or WSL to build the POSIX native dependencies')
    options.ndk = (options.ndk or options.sdk / 'ndk' / NDK_VERSION).resolve()
    host_tag = 'linux-x86_64' if platform.system() == 'Linux' else 'darwin-x86_64'
    tool_root = options.ndk / 'toolchains/llvm/prebuilt' / host_tag
    if not (options.ndk / 'build/cmake/android.toolchain.cmake').is_file():
        parser.error('Android NDK is missing: install NDK ' + NDK_VERSION + ' or provide --ndk')
    arch, triple, _ = ABI_TARGETS[options.abi]
    compiler = tool_root / 'bin' / (triple + str(options.api) + '-clang')
    if not compiler.is_file():
        parser.error('NDK has no compiler for this ABI/API: ' + str(compiler))
    for name in ('cmake', 'ninja', 'make', 'perl'):
        if not shutil.which(name):
            parser.error('Required build tool missing: ' + name)
    root = options.prefix.resolve()
    root.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(Path(__file__).with_name('dependencies.json').read_text())
    sources = {name: source_for(name, info, root, options.cache) for name, info in manifest.items()}
    if options.download_only:
        print('Verified Android dependency sources: ' + str(root / 'sources'))
        return
    target = root / options.abi
    install = target / 'install'
    target.mkdir(parents=True, exist_ok=True)
    base_environment = dict(os.environ)
    for key in ('CC', 'CXX', 'AR', 'AS', 'LD', 'RANLIB', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS',
                'CMAKE_TOOLCHAIN_FILE', 'CMAKE_GENERATOR_PLATFORM', 'CMAKE_GENERATOR_TOOLSET',
                'CMAKE_OSX_ARCHITECTURES', 'SDKROOT', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR'):
        base_environment.pop(key, None)
    environment = {**base_environment, 'ANDROID_NDK_ROOT': str(options.ndk),
                   'PATH': str(tool_root / 'bin') + os.pathsep + base_environment.get('PATH', ''),
                   'CC': str(compiler), 'CXX': str(compiler) + '++',
                   'AR': str(tool_root / 'bin/llvm-ar'), 'RANLIB': str(tool_root / 'bin/llvm-ranlib')}
    compiler_version = subprocess.check_output([str(compiler), '--version'], text=True)
    common_identity = {'abi': options.abi, 'api': options.api, 'ndk': str(options.ndk),
                       'compiler': compiler_version, 'format': 1}

    for name in ('ffmpeg', 'sdl2', 'faudio'):
        build = target / (name + '-build')
        identity = {**common_identity, 'dependency': manifest[name]}
        required = {'ffmpeg': [install / ('lib/lib' + lib + '.so') for lib in ('avformat', 'avcodec', 'avutil', 'swscale')],
                    'sdl2': [install / 'lib/libSDL2.so', install / 'lib/cmake/SDL2/SDL2Config.cmake'],
                    'faudio': [install / 'lib/libFAudio.a']}[name]
        if complete(build, identity, required):
            print('Using completed Android ' + options.abi + ' dependency: ' + name, flush=True)
            continue
        # Configuration changes never reuse target objects from a previous ABI/API.
        if build.exists():
            shutil.rmtree(build)
        build.mkdir(parents=True)
        source = sources[name]
        if name == 'ffmpeg':
            args = ['sh', source / 'configure', '--prefix=' + str(install), '--arch=' + arch,
                    '--target-os=android', '--enable-cross-compile', '--host-cc=cc',
                    '--enable-shared', '--disable-static', '--enable-pic', '--disable-autodetect',
                    '--disable-programs', '--disable-doc', '--disable-network', '--disable-avdevice',
                    '--disable-avfilter', '--disable-everything', '--enable-avcodec', '--enable-avformat',
                    '--enable-avutil', '--enable-swscale', '--enable-protocol=file',
                    '--enable-demuxer=mpegvideo,mov', '--enable-decoder=mpeg1video,mpeg2video,h264',
                    '--enable-parser=mpegvideo,h264', '--disable-debug', '--cc=' + str(compiler),
                    '--cxx=' + str(compiler) + '++', '--ar=' + str(tool_root / 'bin/llvm-ar'),
                    '--ranlib=' + str(tool_root / 'bin/llvm-ranlib'), '--nm=' + str(tool_root / 'bin/llvm-nm'),
                    '--strip=' + str(tool_root / 'bin/llvm-strip'), '--extra-cflags=-fPIC -ffp-contract=off',
                    '--extra-ldflags=-Wl,-z,max-page-size=16384']
            if options.abi == 'x86_64':
                args += ['--disable-x86asm']
            command(args, cwd=build, environment=environment)
            command(['make', '-j', str(options.parallel)], cwd=build, environment=environment)
            command(['make', 'install'], cwd=build, environment=environment)
        elif name == 'sdl2':
            cmake_build(source, build, install, options,
                        ['-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TEST=OFF', '-DSDL2_DISABLE_INSTALL=OFF'], base_environment)
        elif name == 'faudio':
            cmake_build(source, build, install, options,
                        ['-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTS=OFF', '-DBUILD_UTILS=OFF', '-DXNASONG=OFF',
                         '-DCMAKE_PREFIX_PATH=' + str(install),
                         '-DSDL2_DIR=' + str(install / 'lib/cmake/SDL2')], base_environment)
        finish(build, identity)
    # Recreate notices even when a completed library is reused. An interrupted
    # earlier run must not leave successful binaries without their notices.
    for name, source in sources.items():
        notices = install / 'licenses' / name
        notices.mkdir(parents=True, exist_ok=True)
        for notice in ('LICENSE', 'LICENSE.txt', 'LICENSE.md', 'COPYING.txt', 'COPYING.LGPLv2.1', 'COPYING.LGPLv3'):
            if (source / notice).is_file():
                shutil.copy2(source / notice, notices / notice)
        (notices / 'SOURCE.json').write_text(json.dumps(manifest[name], indent=2) + '\n')
    metadata = {'abi': options.abi, 'api': options.api, 'ndk': str(options.ndk),
                'CANE_ANDROID_DEPENDENCY_ROOT': str(install),
                'SDL_ANDROID_JAVA_SOURCE': str(sources['sdl2'] / 'android-project/app/src/main/java'),
                'CXX_SHARED_LIBRARY': str(tool_root / 'sysroot/usr/lib' / triple / 'libc++_shared.so')}
    (target / 'dependency-paths.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print('Android dependency paths: ' + str(target / 'dependency-paths.json'))


if __name__ == '__main__':
    main()
