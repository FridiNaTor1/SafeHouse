#!/usr/bin/env python3
"""Compiles a game's GLSL programs to Metal Shading Language for the GL-on-Metal layer (platform/metal), at build time.

    metal_shaders.py --source REPO --game Sly1/Sly1-Release --output DIR --glslang PATH --spirv-cross PATH [--ios]

Sly 2 and Sly 3 run on Metal with hand-written ports (platform/metal/shaders, matched to the GLSL the game compiles by
its declarations). Sly 1's programs are larger (glob.vert alone is over a thousand lines), so they are translated: each
stage is rewritten into Vulkan GLSL whose resources sit where the layer binds them, compiled to SPIR-V by
glslangValidator and turned into MSL by SPIRV-Cross. The layer then takes the program by its file names
(cane::gl_metal::Program, platform/metal/prebuilt_programs.mm), as the Vulkan renderer takes its SPIR-V
(cmake/vulkan_shaders.py), instead of parsing the GLSL the game hands to glShaderSource.

The layer's conventions (platform/metal/binding_slots.h):
- the loose uniforms: one uniform block at buffer CANE_MTL_UNIFORM_BUFFER_SLOT, the layer's cane_clip (vec4) first, the
  rest at std140 offsets after it (the table below gives each name's offset, size, array count and stride, a struct
  uniform's members as "struct.member" rows, as glGetUniformLocation asks for them);
- the samplers: texture and sampler index = the sampler's slot (0, 1, ... in declaration order), set by glUniform1i's
  unit as on the hand-written ports;
- the storage blocks: GL binding N at buffer CANE_MTL_STORAGE_BUFFER_BASE + N, in the stage that declares them;
- the std140 uniform blocks (GL_UNIFORM_BUFFER binding i): buffer CANE_MTL_UNIFORM_BLOCK_BASE + i;
- a runtime array's length() is the uniform caneLen_<array> the layer fills from the bound range (Metal has no length
  of a buffer argument);
- clip space: y times cane_clip.x (offscreen targets keep OpenGL's rows), z mapped by cane_clip.y / .z (OpenGL's -w..w
  or glClipControl's 0..w to Metal's 0..w); gl_FragCoord's rows as OpenGL's (cane_clip.w: the drawable's height);
- vertex inputs keep their locations as [[attribute(n)]]; the table lists them so a disabled array reads the current
  value (glVertexAttrib*) from a constant buffer, as OpenGL does.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import switch_shaders as sw  # noqa: E402

# (platform/metal/binding_slots.h)
UNIFORM_SLOT = 8
STORAGE_BASE = 9
STORAGE_COUNT = 16
UNIFORM_BLOCK_BASE = 25
UNIFORM_BLOCK_COUNT = 2
TEXTURE_SLOTS = 8
SET_BUFFERS, SET_TEXTURES = 0, 1

# Sly 1 (--game Sly1/Sly1-Release): GL::InitGL's programs, the shadow layers' copy (shadow.cpp) and the debug window's
# Dear ImGui program (platform/render/imgui_gl.cpp), as cmake/vulkan_shaders.py's SLY1_PROGRAMS.
SLY1 = 'game'
SLY1_PROGRAMS = [
    ('glob', 'glob.vert', None, 'glob.frag', SLY1),
    ('celborder', 'celborder.vert', None, 'celborder.frag', SLY1),
    ('dysh', 'dysh.vert', None, 'dysh.frag', SLY1),
    ('geom', 'geom.vert', None, 'geom.frag', SLY1),
    ('blot', 'blot.vert', None, 'blot.frag', SLY1),
    ('blip', 'blip.vert', None, 'blip.frag', SLY1),
    ('screen', 'screen.vert', None, 'screen.frag', SLY1),
    ('shadowalpha', 'shadowalpha.vert', None, 'shadowalpha.frag', SLY1),
    ('cane_imgui', 'imgui.vert', None, 'imgui.frag', 'platform/vulkan/shaders'),
]
STAGES = ('vert', 'frag')
# (platform/metal/glsl_interface.h GlslType)
GLSL_TYPE = {'float': 1, 'vec2': 2, 'vec3': 3, 'vec4': 4, 'int': 5, 'ivec2': 6, 'ivec3': 7, 'ivec4': 8,
             'uint': 9, 'uvec2': 10, 'uvec3': 11, 'uvec4': 12, 'bool': 13, 'mat4': 14,
             'sampler2D': 15, 'sampler2DArray': 16, 'mat3': 17}
INPUT_KIND = {'float': 0, 'int': 1, 'uint': 2}
VERT_INPUT = re.compile(r'layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s+(\w+)\s+(\w+)\s*;')
LENGTH = re.compile(r'\b(\w+)\s*\.\s*length\s*\(\s*\)')


def fail(message):
    raise SystemExit(f'metal_shaders.py: {message}')


def std430(kind, structs):
    """(alignment, size) of a std430 member."""
    if kind in ('float', 'int', 'uint', 'bool'):
        return 4, 4
    m = re.fullmatch(r'([iub]?)vec([234])', kind)
    if m:
        n = int(m.group(2))
        return (8 if n == 2 else 16), 4 * n
    if kind in ('mat3', 'mat4'):
        return 16, 48 if kind == 'mat3' else 64
    if kind in structs:
        offset, alignment = 0, 4
        for fkind, _, count in structs[kind]:
            a, size = std430(fkind, structs)
            stride = sw.align(size, a)
            offset = sw.align(offset, a) + (stride * count if count > 1 else size)
            alignment = max(alignment, a)
        return alignment, sw.align(offset, alignment)
    fail(f'unsupported storage type {kind}')


def block_members(body):
    """A block body's members: (type, name, count or None for a runtime array)."""
    members = []
    for kind, name, count in re.findall(r'\b(\w+)\s+(\w+)\s*(?:\[\s*(\w*)\s*\])?\s*;', body):
        members.append((kind, name, None if count == '' and f'{name}[]' in re.sub(r'\s', '', body) else int(count) if count else 1))
    return members


def runtime_arrays(src, structs):
    """{array name: (GL binding, byte offset of the array in its block, element stride)} of the storage blocks."""
    out = {}
    for m in sw.BLOCK.finditer(src):
        if m.group(3) != 'buffer':
            continue
        binding = re.search(r'\bbinding\s*=\s*(\d+)', m.group(1))
        body = src[m.end():src.index('}', m.end())]
        offset = 0
        for kind, name, count in block_members(body):
            a, size = std430(kind, structs)
            stride = sw.align(size, a)
            offset = sw.align(offset, a)
            if count is None:
                out[name] = (int(binding.group(1)), offset, stride)
                break
            offset += stride * count if count > 1 else size
    return out


def build_program(root, key, files, directory, out_dir, glslang, spirv_cross, ios):
    sources = {}
    for stage, name in zip(STAGES, (files[0], files[2])):
        text = (root / directory / name).read_text(encoding='utf-8-sig')
        sources[stage] = sw.preprocess(sw.strip_comments(text), {'METAL': '1'})
    if files[1]:
        fail(f'{key}: Metal has no geometry stage ({files[1]})')
    structs = {}
    for src in sources.values():
        structs.update(sw.parse_structs(src))

    # the loose uniforms (one layout for both stages) and the samplers' slots
    fields, samplers = {}, {}
    for stage, src in sources.items():
        for kind, name, count in sw.UNIFORM.findall(src):
            if kind.startswith('sampler'):
                if kind not in ('sampler2D', 'sampler2DArray'):
                    fail(f'{key}: unsupported sampler type {kind}')
                samplers.setdefault(name, {'type': kind, 'slot': len(samplers)})
                continue
            n = int(count) if count else 1
            if name in fields and fields[name] != (kind, n):
                fail(f'{key}: uniform {name} differs between stages')
            fields[name] = (kind, n)
    if len(samplers) > TEXTURE_SLOTS:
        fail(f'{key}: more than {TEXTURE_SLOTS} samplers')
    lengths = {}
    for stage, src in sources.items():
        arrays = runtime_arrays(src, structs)
        for name in LENGTH.findall(src):
            if name not in arrays:
                fail(f'{key}.{stage}: length() of {name}, which is no storage block\'s runtime array')
            lengths[name] = arrays[name]
            fields.setdefault(f'caneLen_{name}', ('uint', 1))

    rows, members = [], []
    offset = 16   # (cane_clip)
    for name, (kind, n) in fields.items():
        a, size, stride, sub = sw.std140(kind, structs)
        element = size
        if n > 1:
            if sub is not None:
                fail(f'{key}: array of struct uniform {name}')
            a, size = 16, stride * n
        offset = sw.align(offset, a)
        members.append((name, kind, n, offset))
        if sub is None:
            rows.append((name, kind, offset, element, n, stride if n > 1 else element))
        else:
            for fname, fkind, foff, fsize, fcount, fstride in sub:
                rows.append((f'{name}.{fname}', fkind, offset + foff, fsize, fcount, fstride))
        offset += size
    uniform_bytes = sw.align(offset, 16)

    # stage interface locations: the GLSL's own first, the rest matched by name (cmake/vulkan_shaders.py's rule)
    occupied, locations = set(), {}
    for stage, src in sources.items():
        for m in sw.INTERFACE.finditer(src):
            if m.group('loc') is None or (stage == 'vert' and m.group('dir') == 'in') or (stage == 'frag' and m.group('dir') == 'out'):
                continue
            n = sw.slots(m.group('type'), sw.array_count(m.group('array')), structs)
            loc = int(m.group('loc'))
            locations.setdefault(m.group('name'), loc)
            occupied.update(range(loc, loc + n))

    def allocate(n):
        loc = 0
        while any(l in occupied for l in range(loc, loc + n)):
            loc += 1
        occupied.update(range(loc, loc + n))
        return loc

    inputs = {}
    for loc, kind, _ in VERT_INPUT.findall(sources['vert']):
        base = 'uint' if kind.startswith('uvec') or kind == 'uint' else 'int' if kind.startswith('ivec') or kind == 'int' else 'float'
        inputs[int(loc)] = INPUT_KIND[base]

    result = {}
    for stage in STAGES:
        src = sources[stage]
        if re.search(r'\bgl_InstanceID\b', src):
            fail(f'{key}.{stage}: gl_InstanceID is not handled')
        src = re.sub(r'\bgl_VertexID\b', 'gl_VertexIndex', src)
        src = sw.UNIFORM.sub('', src)
        src = LENGTH.sub(lambda m: f'int(caneLen_{m.group(1)})', src)
        uses_frag_coord = bool(re.search(r'\bgl_FragCoord\b', src))
        src = re.sub(r'\bgl_FragCoord\b', 'caneFragCoord()', src)

        def interface(m):
            direction, kind, name, array = m.group('dir'), m.group('type'), m.group('name'), m.group('array') or ''
            qual = m.group('qual') or ''
            if m.group('loc') is not None:
                loc = int(m.group('loc'))
            elif stage == 'vert' and direction == 'in':
                fail(f'{key}: vertex input {name} has no location')
            elif stage == 'frag' and direction == 'out':
                loc = 0
            else:
                if name not in locations:
                    if direction == 'in':
                        fail(f'{key}: {stage} input {name} is written by no earlier stage')
                    locations[name] = allocate(sw.slots(kind, sw.array_count(array), structs))
                loc = locations[name]
            return f'{m.group("lead")}layout(location = {loc}) {qual}{direction} {kind} {name}{array};'
        src = sw.INTERFACE.sub(interface, src)

        storage, blocks = 0, 0
        block_names = {}

        def block(m):
            nonlocal storage, blocks
            qualifiers, access, kind, name = m.group(1), m.group(2) or '', m.group(3), m.group(4)
            binding = re.search(r'\bbinding\s*=\s*(\d+)', qualifiers)
            if not binding:
                fail(f'{key}.{stage}: block {name} has no binding')
            n = int(binding.group(1))
            if kind == 'buffer':
                if n >= STORAGE_COUNT:
                    fail(f'{key}.{stage}: storage block {name} binding {n} above {STORAGE_COUNT - 1}')
                storage |= 1 << n
                slot = STORAGE_BASE + n
            else:
                if n >= UNIFORM_BLOCK_COUNT:
                    fail(f'{key}.{stage}: uniform block {name} binding {n} above {UNIFORM_BLOCK_COUNT - 1}')
                if not re.search(r'\bstd140\b', qualifiers):
                    fail(f'{key}.{stage}: uniform block {name} is not std140')
                blocks |= 1 << n
                block_names[name] = n
                slot = UNIFORM_BLOCK_BASE + n
            qualifiers = re.sub(r'\bbinding\s*=\s*\d+', f'binding = {slot}', qualifiers)
            return f'layout({qualifiers}, set = {SET_BUFFERS}) {access}{kind} {name} {{'
        src = sw.BLOCK.sub(block, src)

        decl = [f'layout(std140, set = {SET_BUFFERS}, binding = {UNIFORM_SLOT}) uniform CaneUniforms', '{',
                '    layout(offset = 0) vec4 caneClip;']
        used = set(re.findall(r'\b\w+\b', src))
        for name, kind, n, off in members:
            suffix = f'[{n}]' if n > 1 else ''
            decl.append(f'    layout(offset = {off}) {kind} {name}{suffix};')
        decl.append('};')
        for name, s in samplers.items():
            if name in used:
                decl.append(f'layout(set = {SET_TEXTURES}, binding = {s["slot"]}) uniform {s["type"]} {name};')
        if uses_frag_coord:
            decl.append('vec4 caneFragCoord() { return vec4(gl_FragCoord.x, caneClip.x > 0.0 ? caneClip.w - gl_FragCoord.y : '
                        'gl_FragCoord.y, gl_FragCoord.zw); }')
        func = re.search(r'^[ \t]*(?:void|bool|float|int|uint|vec[234]|ivec[234]|uvec[234]|mat[34]|\w+)\s+\w+\s*\([^;{]*\)\s*\{',
                         src, re.M)
        if not func:
            fail(f'{key}.{stage}: no function to place the declarations before')
        src = src[:func.start()] + '\n'.join(decl) + '\n' + src[func.start():]
        if stage == 'vert':
            # (the layer's clip space: see the module comment)
            src = re.sub(r'\bvoid\s+main\s*\(\s*(?:void)?\s*\)', 'void caneMain()', src, count=1)
            src += ('void main()\n{\n    caneMain();\n    gl_Position.y *= caneClip.x;\n'
                    '    gl_Position.z = gl_Position.z * caneClip.y + gl_Position.w * caneClip.z;\n}\n')
        text = '#version 460\n' + src
        glsl = out_dir / f'{key}.{stage}.glsl'
        spv = out_dir / f'{key}.{stage}.spv'
        msl = out_dir / f'{key}.{stage}.metal'
        glsl.write_text(text, encoding='utf-8')
        proc = subprocess.run([str(glslang), '-V', '--target-env', 'vulkan1.2', '--quiet', '-S', stage, '-o', str(spv),
                               str(glsl)], capture_output=True, text=True)
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout + proc.stderr)
            fail(f'glslangValidator failed on {glsl}')
        entry = 'vertex_main' if stage == 'vert' else 'fragment_main'
        args = [str(spirv_cross), str(spv), '--msl', '--msl-version', '20100', '--msl-decoration-binding',
                '--rename-entry-point', 'main', entry, stage, '--output', str(msl)]
        if ios:
            args.append('--msl-ios')
        proc = subprocess.run(args, capture_output=True, text=True)
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout + proc.stderr)
            fail(f'spirv-cross failed on {spv}')
        result[stage] = {'msl': msl.read_text(encoding='utf-8'), 'storage': storage, 'blocks': blocks,
                         'block_names': block_names}

    return {'key': key, 'files': files, 'rows': rows, 'uniform_bytes': uniform_bytes,
            'samplers': sorted(samplers.items(), key=lambda kv: kv[1]['slot']), 'stages': result, 'inputs': inputs,
            'lengths': sorted((name, fields_offset(members, f'caneLen_{name}'), *v) for name, v in lengths.items())}


def fields_offset(members, name):
    return next(off for n, _, _, off in members if n == name)


def c_string(text):
    """A C++ raw string literal of the text, in pieces (MSVC limits one literal's size; the others do not mind)."""
    pieces, out = [], []
    for i in range(0, len(text), 8000):
        pieces.append(text[i:i + 8000])
    for p in pieces:
        if ')cane"' in p:
            fail('the MSL contains the raw string delimiter')
        out.append(f'R"cane({p})cane"')
    return '\n'.join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--game', required=True, help='the game folder (Sly1/Sly1-Release)')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--glslang', type=Path, required=True)
    parser.add_argument('--spirv-cross', type=Path, required=True)
    parser.add_argument('--ios', action='store_true', help='MSL for iOS')
    args = parser.parse_args()

    work = args.output / 'metal_shaders'
    work.mkdir(parents=True, exist_ok=True)
    table = [(k, (v, g, f), args.game if d == SLY1 else d) for k, v, g, f, d in SLY1_PROGRAMS]
    programs = [build_program(args.source, key, files, directory, work, args.glslang, args.spirv_cross, args.ios)
                for key, files, directory in table]

    c = ['// Generated by cmake/metal_shaders.py from the GLSL sources: do not edit.',
         '#include "prebuilt_program.h"', 'namespace cane::gl_metal::prebuilt {', 'namespace {']
    entries = []
    for p in programs:
        key = p['key']
        c.append(f'const char {key}_vert[] =\n{c_string(p["stages"]["vert"]["msl"])};')
        c.append(f'const char {key}_frag[] =\n{c_string(p["stages"]["frag"]["msl"])};')
        rows = [f'    {{"{name}", {GLSL_TYPE[kind]}, {off}, {size}, {count}, {stride}, -1}},'
                for name, kind, off, size, count, stride in p['rows']]
        rows += [f'    {{"{name}", {GLSL_TYPE[s["type"]]}, 0, 0, 1, 0, {s["slot"]}}},' for name, s in p['samplers']]
        c.append(f'const Field {key}_fields[] = {{')
        c.extend(rows or ['    {nullptr, 0, 0, 0, 0, 0, -1},'])
        c.append('};')
        lengths = 'nullptr'
        if p['lengths']:
            c.append(f'const Length {key}_lengths[] = {{')
            c.extend(f'    {{{binding}, {off}, {start}, {stride}}},  // {name}' for name, off, binding, start, stride in p['lengths'])
            c.append('};')
            lengths = f'{key}_lengths'
        v, g, f = p['files']
        input_mask = sum(1 << loc for loc in p['inputs'])
        input_kinds = sum(kind << (2 * loc) for loc, kind in p['inputs'].items())
        sv, sf = p['stages']['vert'], p['stages']['frag']
        names = dict(sv['block_names'], **sf['block_names'])
        block_names = ';'.join(f'{n}={b}' for n, b in sorted(names.items(), key=lambda kv: kv[1]))
        entries.append(f'    {{"{key}", "{v}", "{f}", {key}_vert, {key}_frag, {key}_fields, {len(rows)}, {p["uniform_bytes"]}, '
                       f'{sv["storage"]}u, {sf["storage"]}u, {sv["blocks"]}u, {sf["blocks"]}u, "{block_names}", {input_mask}u, {input_kinds}u, '
                       f'{lengths}, {len(p["lengths"])}, {len(p["samplers"])}}},')
    c.append('}  // namespace')
    c.append('const Program programs[] = {')
    c.extend(entries)
    c.append('};')
    c.append('const size_t programCount = sizeof(programs) / sizeof(programs[0]);')
    c.append('}  // namespace cane::gl_metal::prebuilt')
    text = '\n'.join(c) + '\n'
    out = args.output / 'metal_programs.cpp'
    if not out.exists() or out.read_text(encoding='utf-8') != text:
        out.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    main()
