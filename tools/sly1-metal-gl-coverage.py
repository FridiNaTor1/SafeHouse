#!/usr/bin/env python3
"""Every OpenGL function Sly 1 calls must be in the GL-on-Metal layer's table (platform/metal/gl_entry_points.mm).

    sly1-metal-gl-coverage.py --source REPO [--list]

On macOS and iOS the game's GL calls go through GLAD to cane::gl_metal::ProcAddress; a function the table lacks loads
as a null pointer and the game crashes at its first call (Sly 2 once lacked glUniform2i that way). There is no Mac in
the development loop, so this runs on every host (ctest sly1-metal-gl-coverage): the GL functions named in Sly 1's
sources (the submodule's Sly1-Release/ and SafeHouse's files it builds on Apple) against the names the table maps.

The scan is by name in the sources, comments removed, so it also counts calls in code Apple does not build (Windows /
deko3d branches): a few more entries than strictly needed, never fewer. NOT_ON_APPLE lists the names whose every
call is in such code, with the reason.
"""
import argparse
import re
import sys
from pathlib import Path

# (name: why Apple never calls it)
NOT_ON_APPLE = {
}
# SafeHouse's files built into Sly1-Release on Apple (cmake/sly1.cmake), beside the submodule's game folder
SAFEHOUSE_FILES = ['platform/render/imgui_gl.cpp', 'platform/render/imgui_gl.h']


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--list', action='store_true', help='print every GL function Sly 1 uses')
    args = parser.parse_args()
    root = args.source

    glad = (root / 'Libraries/include/glad/glad.h').read_text(encoding='utf-8', errors='replace')
    gl_functions = set(re.findall(r'^#define (gl[A-Z]\w*) glad_gl', glad, re.M))
    table_text = (root / 'platform/metal/gl_entry_points.mm').read_text(encoding='utf-8')
    table = set(re.findall(r'CANE_PROC\((gl[A-Z]\w*)\)', table_text))
    if not gl_functions or not table:
        sys.exit('sly1-metal-gl-coverage: could not read glad.h or the Metal table')

    game = root / 'Sly1' / 'Sly1-Release'
    if not game.is_dir():
        sys.exit(f'sly1-metal-gl-coverage: {game} is missing (git submodule update --init)')
    files = [p for p in sorted(game.rglob('*')) if p.suffix in ('.cpp', '.h', '.hpp', '.inl')]
    files += [root / f for f in SAFEHOUSE_FILES if (root / f).exists()]
    used = {}
    for path in files:
        text = strip_comments(path.read_text(encoding='utf-8', errors='replace'))
        # (the calls by name, and glstate's direct GLAD pointers: glad_glIsEnabled, ...)
        for name in re.findall(r'\b(gl[A-Z]\w*)\s*\(', text) + re.findall(r'\bglad_(gl[A-Z]\w*)\b', text):
            if name in gl_functions:
                used.setdefault(name, path.relative_to(root).as_posix())
    if args.list:
        for name in sorted(used):
            print(f'{name}  ({used[name]})')
    missing = sorted(n for n in used if n not in table and n not in NOT_ON_APPLE)
    stale = sorted(n for n in NOT_ON_APPLE if n not in used or n in table)
    for name in missing:
        print(f'MISSING from platform/metal/gl_entry_points.mm: {name} (first use: {used[name]})')
    for name in stale:
        print(f'stale NOT_ON_APPLE entry: {name}')
    print(f'sly1-metal-gl-coverage: {len(used)} GL functions used, {len(table)} in the Metal table, '
          f'{len(missing)} missing')
    sys.exit(1 if missing or stale else 0)


if __name__ == '__main__':
    main()
