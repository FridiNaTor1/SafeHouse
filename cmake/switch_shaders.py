#!/usr/bin/env python3
"""Compiles SafeHouse.nro's GLSL offline for deko3d (platform/switch/deko).

    switch_shaders.py --source REPO --output DIR --uam PATH/uam

deko3d has no run-time GLSL compiler, so every program a game builds (its GLSHADER Init / FInit calls, with the
variants it asks for) is compiled here with uam into DKSH and written into DIR/deko_shaders.cpp, which defines the
table platform/switch/deko/deko_shaders.h declares: the programs of the games on SafeHouse's deko3d layer (Sly 2,
Sly 3) and the shared ones, each tagged with its game. (Sly 1 runs on Project Cane's backend:
cmake/switch_shaders_sly1.py.)
With them the GL layer stands in for the driver's linker (platform/switch/deko/gl_deko.cpp):

- the loose uniforms ("uniform mat4 matWorldToClip;") become one std140 block at binding 15 (CaneConstants) with
  explicit offsets; their names (a struct's members as "name.member"), types and offsets are the program's uniform
  table (glGetUniformLocation, glUniform*);
- each sampler gets an explicit binding (glUniform1i picks its texture unit at run time);
- stage interfaces get explicit locations, matched by name between stages as the GL linker does, around the ones the
  GLSL places itself;
- the uniform blocks and storage buffers keep the GLSL's own bindings; the bindings each stage reads are recorded.

A program variant is (vertex, geometry, fragment, defines): the defines are the prefix the game inserts after #version
("#define GEOM 1\\n" for Sly 2's near-plane clip). DEKO3D is defined for every program (the GLSL's own Switch
adjustments). Variants not listed (Sly 2's BINDLESS) fail to build at run time and the game takes its fallback, as on a
driver without the extension. The ImGui renderer's program (deko/shaders/imgui.*) is shared: the launcher, the
installers and the games draw with it.
"""
import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

NATIVE = 'platform/switch/deko/shaders'
SLY2 = 'Sly2-Release'
SLY3 = 'Sly3-Release'

# key, vertex, geometry, fragment, defines, directory
GAMES = {
    # (Sly 1 runs on Project Cane's deko3d backend, its shaders compiled by cmake/switch_shaders_sly1.py)
    'sly2': [
        ('glob', 'glob.vert', None, 'glob.frag', {}, SLY2),
        ('glob_geom', 'glob.vert', 'glob.geom', 'glob.frag', {'GEOM': '1'}, SLY2),
        # (the variants without the fragment discards: render.cpp draws every run that needs neither the alpha test
        # nor DATE with them, so the GPU tests depth and stencil before the fragments)
        ('glob_nokill', 'glob.vert', None, 'glob.frag', {'NOKILL': '1'}, SLY2),
        ('glob_geom_nokill', 'glob.vert', 'glob.geom', 'glob.frag', {'GEOM': '1', 'NOKILL': '1'}, SLY2),
        ('celborder', 'celborder.vert', None, 'celborder.frag', {}, SLY2),
        ('hud', 'hud.vert', None, 'hud.frag', {}, SLY2),
        ('overlay', 'overlay.vert', None, 'overlay.frag', {}, SLY2),
        ('particle', 'particle.vert', None, 'particle.frag', {}, SLY2),
        ('screen', 'screen.vert', None, 'screen.frag', {}, SLY2),
        ('shadowcombine', 'shadowcombine.vert', None, 'shadowcombine.frag', {}, SLY2),
        ('shadowdate', 'shadowcombine.vert', None, 'shadowdate.frag', {}, SLY2),
        ('shadowvol', 'shadowvol.vert', None, 'shadowvol.frag', {}, SLY2),
        ('shadowvol_nokill', 'shadowvol.vert', None, 'shadowvol.frag', {'NOKILL': '1'}, SLY2),
        # (upscaling, fsr.h: FSR 1's two passes)
        ('fsreasu', 'vrfull.vert', None, 'fsreasu.frag', {}, SLY2),
        ('fsrrcas', 'vrfull.vert', None, 'fsrrcas.frag', {}, SLY2),
    ],
    'sly3': [
        ('glob', 'glob.vert', None, 'glob.frag', {}, SLY3),
        ('glob_geom', 'glob.vert', 'glob.geom', 'glob.frag', {'GEOM': '1'}, SLY3),
        ('celborder', 'celborder.vert', None, 'celborder.frag', {}, SLY3),
        ('hud', 'hud.vert', None, 'hud.frag', {}, SLY3),
        ('overlay', 'overlay.vert', None, 'overlay.frag', {}, SLY3),
        ('particle', 'particle.vert', None, 'particle.frag', {}, SLY3),
        ('screen', 'screen.vert', None, 'screen.frag', {}, SLY3),
        ('shadowcombine', 'shadowcombine.vert', None, 'shadowcombine.frag', {}, SLY3),
        ('shadowvol', 'shadowvol.vert', None, 'shadowvol.frag', {}, SLY3),
    ],
}
# (every game's: game '' in the table)
COMMON = [('imgui', 'imgui.vert', None, 'imgui.frag', {}, NATIVE)]

STAGES = ('vert', 'geom', 'frag')
CONSTANT_BINDING = 15

# The uniform types the GL layer understands (gl_deko.cpp UniformType).
TYPE_CODES = {
    'float': 0, 'vec2': 1, 'vec3': 2, 'vec4': 3,
    'int': 4, 'ivec2': 5, 'ivec3': 6, 'ivec4': 7,
    'uint': 8, 'uvec2': 9, 'uvec3': 10, 'uvec4': 11,
    'bool': 12, 'mat3': 13, 'mat4': 14,
    'sampler2D': 32, 'sampler2DArray': 33,
}


def fail(message):
    raise SystemExit(f'switch_shaders.py: {message}')


def strip_comments(source):
    # (keeps the line count: a block comment's newlines stay, so uam's line numbers match the input)
    def block(m):
        return '\n' * m.group(0).count('\n')
    source = re.sub(r'/\*.*?\*/', block, source, flags=re.S)
    return re.sub(r'//[^\n]*', '', source)


def preprocess(source, defines):
    """A small C preprocessor: #define (object-like), #undef, #ifdef, #ifndef, #if, #elif, #else, #endif."""
    macros = dict(defines)
    out = []
    stack = []      # (parent_active, a_branch_taken, active)
    active = True

    def expand(line):
        for _ in range(8):
            changed = False
            for name, value in macros.items():
                pattern = r'\b' + re.escape(name) + r'\b'
                if re.search(pattern, line):
                    line = re.sub(pattern, value, line)
                    changed = True
            if not changed:
                break
        return line

    def evaluate(expr):
        expr = re.sub(r'defined\s*\(\s*(\w+)\s*\)', lambda m: '1' if m.group(1) in macros else '0', expr)
        expr = re.sub(r'defined\s+(\w+)', lambda m: '1' if m.group(1) in macros else '0', expr)
        expr = expand(expr)
        expr = re.sub(r'\b[A-Za-z_]\w*\b', '0', expr)  # (unknown identifiers are 0, as in C)
        expr = expr.replace('&&', ' and ').replace('||', ' or ').replace('!', ' not ').replace('not =', '!=')
        try:
            return bool(eval(expr, {'__builtins__': {}}))
        except Exception:
            fail(f'cannot evaluate #if {expr!r}')

    for line in source.splitlines():
        d = re.match(r'\s*#\s*(\w+)\s*(.*)$', line)
        if d:
            directive, rest = d.group(1), d.group(2).strip()
            if directive in ('ifdef', 'ifndef'):
                cond = (rest.split()[0] in macros) if rest else False
                if directive == 'ifndef':
                    cond = not cond
                stack.append((active, cond, active and cond))
                active = active and cond
                out.append('')
                continue
            if directive == 'if':
                cond = evaluate(rest) if active else False
                stack.append((active, cond, active and cond))
                active = active and cond
                out.append('')
                continue
            if directive == 'elif':
                parent, taken, _ = stack.pop()
                cond = (not taken) and parent and evaluate(rest)
                stack.append((parent, taken or cond, parent and cond))
                active = parent and cond
                out.append('')
                continue
            if directive == 'else':
                parent, taken, _ = stack.pop()
                stack.append((parent, True, parent and not taken))
                active = parent and not taken
                out.append('')
                continue
            if directive == 'endif':
                active = stack.pop()[0]
                out.append('')
                continue
            if not active:
                out.append('')
                continue
            if directive == 'define':
                m = re.match(r'(\w+)(?:\s+(.*))?$', rest)
                if not m:
                    fail(f'bad #define {rest!r}')
                if '(' in m.group(1):
                    fail('function-like macros are not supported')
                macros[m.group(1)] = (m.group(2) or '').strip()
                out.append('')
                continue
            if directive == 'undef':
                macros.pop(rest.split()[0], None)
                out.append('')
                continue
            if directive in ('version', 'extension', 'line', 'pragma'):
                out.append('')
                continue
            fail(f'unsupported directive #{directive}')
        out.append(expand(line) if active else '')
    if stack:
        fail('unterminated #if')
    return '\n'.join(out) + '\n'


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


STRUCT = re.compile(r'\bstruct\s+(\w+)\s*\{([^}]*)\}\s*;', re.S)
FIELD = re.compile(r'\b(\w+)\s+(\w+)\s*(?:\[\s*(\w+)\s*\])?\s*;')


def parse_structs(source):
    structs = {}
    for name, body in STRUCT.findall(source):
        structs[name] = [(kind, field, int(count) if count else 1) for kind, field, count in FIELD.findall(body)]
    return structs


def std140(kind, structs):
    """(alignment, size, array stride, fields) of a std140 member; fields: the struct's (name, type, offset)."""
    if kind in ('float', 'int', 'uint', 'bool'):
        return 4, 4, 16, None
    m = re.fullmatch(r'([iub]?)vec([234])', kind)
    if m:
        n = int(m.group(2))
        return (8 if n == 2 else 16), 4 * n, 16, None
    if kind == 'mat3':
        return 16, 48, 48, None
    if kind == 'mat4':
        return 16, 64, 64, None
    if kind in structs:
        offset, fields, alignment = 0, [], 16
        for fkind, fname, count in structs[kind]:
            a, size, stride, sub = std140(fkind, structs)
            if sub is not None:
                fail(f'nested struct {fkind} in a uniform struct')
            if count > 1:
                a, size = 16, stride * count
            offset = align(offset, a)
            fields.append((fname, fkind, offset, size, count, stride if count > 1 else size))
            offset += size
        size = align(offset, 16)
        return alignment, size, size, fields
    fail(f'unsupported uniform type {kind}')


def slots(kind, count, structs):
    """The interface locations a variable takes."""
    if kind in structs:
        n = sum(slots(fkind, fcount, structs) for fkind, _, fcount in structs[kind])
    else:
        n = 4 if kind == 'mat4' else 3 if kind == 'mat3' else 1
    return n * max(1, count)


# (a loose uniform: at a line's start, or after a struct's "};" on the same line)
UNIFORM = re.compile(r'(?:^|(?<=[;}]))[ \t]*uniform\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(?:\[\s*(\w+)\s*\])?\s*;', re.M)
BLOCK = re.compile(r'layout\s*\(([^)]*)\)\s*((?:(?:readonly|writeonly|coherent|volatile|restrict)\s+)*)(uniform|buffer)\s+(\w+)\s*\{', re.S)
INTERFACE = re.compile(
    r'^(?P<lead>[ \t]*)(?:layout\s*\(\s*location\s*=\s*(?P<loc>\d+)\s*\)\s*)?'
    r'(?P<qual>(?:(?:flat|noperspective|smooth|centroid|sample|invariant)\s+)*)'
    r'(?P<dir>in|out)\s+(?P<type>\w+)\s+(?P<name>\w+)\s*(?P<array>\[[^\]]*\])?\s*;', re.M)


def array_count(array):
    token = (array or '').strip('[] ')
    return int(token) if token.isdigit() else 1


def build_program(root, key, files, defines, directory, out_dir, uam):
    sources = {}
    stage_defines = dict(defines, DEKO3D='1')
    for stage, name in zip(STAGES, files):
        if name is None:
            continue
        text = (root / directory / name).read_text(encoding='utf-8-sig')
        sources[stage] = preprocess(strip_comments(text), stage_defines)
    structs = {}
    for src in sources.values():
        structs.update(parse_structs(src))

    # the loose uniforms of all stages, one layout for the program (each stage's block lists its own)
    fields, samplers, stage_has_constants, stage_uniforms = {}, {}, {}, {}
    for stage, src in sources.items():
        stage_has_constants[stage] = False
        stage_uniforms[stage] = set()
        for kind, name, count in UNIFORM.findall(src):
            stage_uniforms[stage].add(name)
            if kind.startswith('sampler'):
                if kind not in TYPE_CODES:
                    fail(f'{key}: unsupported sampler type {kind}')
                entry = samplers.setdefault(name, {'type': kind, 'binding': len(samplers), 'stages': 0})
                if entry['type'] != kind:
                    fail(f'{key}: sampler {name} differs between stages')
                entry['stages'] |= 1 << STAGES.index(stage)
                continue
            n = int(count) if count else 1
            if name in fields and fields[name] != (kind, n):
                fail(f'{key}: uniform {name} differs between stages')
            fields[name] = (kind, n)
            stage_has_constants[stage] = True
    members, rows = [], []
    offset = 0
    for name, (kind, n) in fields.items():
        a, size, stride, sub = std140(kind, structs)
        if n > 1:
            if sub is not None:
                fail(f'{key}: array of struct uniform {name}')
            a, size = 16, stride * n
        offset = align(offset, a)
        members.append({'name': name, 'type': kind, 'offset': offset, 'count': n})
        if sub is None:
            rows.append({'name': name, 'type': kind, 'offset': offset, 'size': size, 'count': n, 'stride': stride if n > 1 else size})
        else:
            for fname, fkind, foff, fsize, fcount, fstride in sub:
                rows.append({'name': f'{name}.{fname}', 'type': fkind, 'offset': offset + foff, 'size': fsize,
                             'count': fcount, 'stride': fstride})
        offset += size
    constant_size = align(offset, 16)

    # stage interface locations: the GLSL's own first, then the others matched by name into the free ones
    occupied, locations = set(), {}
    for stage, src in sources.items():
        for m in INTERFACE.finditer(src):
            if m.group('loc') is None or (stage == 'vert' and m.group('dir') == 'in') or (stage == 'frag' and m.group('dir') == 'out'):
                continue
            n = slots(m.group('type'), 1 if stage == 'geom' else array_count(m.group('array')), structs)
            loc = int(m.group('loc'))
            locations.setdefault(m.group('name'), loc)
            occupied.update(range(loc, loc + n))

    def allocate(n):
        loc = 0
        while any(l in occupied for l in range(loc, loc + n)):
            loc += 1
        occupied.update(range(loc, loc + n))
        return loc

    result, blocks = {}, {}
    for stage in [s for s in STAGES if s in sources]:
        src = sources[stage]
        src = UNIFORM.sub(lambda m: (f'layout(binding = {samplers[m.group(2)]["binding"]}) uniform {m.group(1)} {m.group(2)};'
                                     if m.group(1).startswith('sampler') else ''), src)

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
                    locations[name] = allocate(slots(kind, 1 if stage == 'geom' else array_count(array), structs))
                loc = locations[name]
            return f'{m.group("lead")}layout(location = {loc}) {qual}{direction} {kind} {name}{array};'
        src = INTERFACE.sub(interface, src)

        if members and stage_has_constants.get(stage):
            block = [f'layout(std140, binding = {CONSTANT_BINDING}) uniform CaneConstants', '{']
            for f in members:
                if f['name'] not in stage_uniforms[stage]:
                    continue   # (another stage's: the explicit offsets keep the layout)
                suffix = f'[{f["count"]}]' if f['count'] > 1 else ''
                block.append(f'    layout(offset = {f["offset"]}) {f["type"]} {f["name"]}{suffix};')
            block.append('};')
            func = re.search(r'^[ \t]*(?:void|bool|float|int|uint|vec[234]|ivec[234]|uvec[234]|mat[34]|\w+)\s+\w+\s*\([^;{]*\)\s*\{',
                             src, re.M)
            if not func:
                fail(f'{key}.{stage}: no function to place the uniform block before')
            src = src[:func.start()] + '\n'.join(block) + '\n' + src[func.start():]

        storage = uniform_blocks = 0
        for qualifiers, _, kind, block_name in BLOCK.findall(src):
            binding = re.search(r'\bbinding\s*=\s*(\d+)', qualifiers)
            if not binding:
                fail(f'{key}.{stage}: block {block_name} has no binding')
            slot = int(binding.group(1))
            if kind == 'buffer':
                if slot >= 16:
                    fail(f'{key}.{stage}: storage block {block_name} binding {slot} is above deko3d\'s 15')
                storage |= 1 << slot
            elif block_name != 'CaneConstants':
                if slot >= CONSTANT_BINDING:
                    fail(f'{key}.{stage}: uniform block {block_name} binding {slot} collides with the constants ({CONSTANT_BINDING})')
                uniform_blocks |= 1 << slot
                if blocks.setdefault(block_name, slot) != slot:
                    fail(f'{key}: uniform block {block_name} has two bindings')

        text = '#version 460\n' + src
        glsl = out_dir / f'{key}.{stage}.glsl'
        binary = out_dir / f'{key}.{stage}.dksh'
        glsl.write_text(text, encoding='utf-8')
        proc = subprocess.run([str(uam), '-s', stage, '-o', str(binary), str(glsl)], capture_output=True, text=True)
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout + proc.stderr)
            fail(f'uam failed on {glsl}')
        data = binary.read_bytes()
        if len(data) < 64 or data[:4] != b'DKSH':
            fail(f'invalid DKSH output: {binary}')
        _, header_size, control_size, code_size, program_offset, program_count = struct.unpack_from('<6I', data)
        if header_size < 24 or control_size + code_size > len(data) or program_count != 1 or program_offset + 64 > control_size:
            fail(f'invalid DKSH sections: {binary}')
        scratch = struct.unpack_from('<I', data, program_offset + 20)[0]
        if scratch > 64 * 1024:
            fail(f'{binary}: scratch memory {scratch} above the queue limit')
        result[stage] = {'data': data, 'storage': storage, 'uniforms': uniform_blocks,
                         'constants': bool(members and stage_has_constants.get(stage))}

    sampler_list = sorted(samplers.items(), key=lambda kv: kv[1]['binding'])
    return {'key': key, 'files': files, 'defines': defines, 'rows': rows, 'constant_size': constant_size,
            'samplers': sampler_list, 'stages': result, 'blocks': sorted(blocks.items(), key=lambda kv: kv[1])}


def c_bytes(name, data):
    lines = [f'alignas(256) const uint8_t {name}[] = {{']
    for i in range(0, len(data), 32):
        lines.append(','.join(str(b) for b in data[i:i + 32]) + ',')
    lines.append('};')
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--uam', type=Path, required=True)
    args = parser.parse_args()

    # (game, program): the games' own, then the shared ones
    programs = []
    for game, table in [(g, GAMES[g]) for g in sorted(GAMES)] + [('', COMMON)]:
        work = args.output / 'deko_shaders' / (game or 'common')
        work.mkdir(parents=True, exist_ok=True)
        programs += [(game, build_program(args.source, key, (vert, geom, frag), defines, directory, work, args.uam))
                     for key, vert, geom, frag, defines, directory in table]

    c = ['// Generated by cmake/switch_shaders.py from the GLSL sources: do not edit.',
         '#include "deko/deko_shaders.h"', 'namespace CaneDeko::Shaders {', 'namespace {']
    entries = []
    for game, p in programs:
        key = f'{game or "common"}_{p["key"]}'
        blobs = []
        for stage in STAGES:
            if stage in p['stages']:
                c.extend(c_bytes(f'{key}_{stage}', p['stages'][stage]['data']))
                blobs.append((f'{key}_{stage}', f'sizeof({key}_{stage})'))
            else:
                blobs.append(('nullptr', '0'))
        rows = [f'    {{"{f["name"]}", {f["offset"]}, {f["size"]}, {f["count"]}, {f["stride"]}, {TYPE_CODES[f["type"]]}, 0, 0}},'
                for f in p['rows']]
        rows += [f'    {{"{name}", 0, 4, 1, 4, {TYPE_CODES[s["type"]]}, {s["binding"]}, {s["stages"]}}},' for name, s in p['samplers']]
        uniforms = 'nullptr'
        if rows:
            c.append(f'const Uniform {key}_uniforms[] = {{')
            c.extend(rows)
            c.append('};')
            uniforms = f'{key}_uniforms'
        blocks = 'nullptr'
        if p['blocks']:
            c.append(f'const Block {key}_blocks[] = {{')
            c.extend(f'    {{"{name}", {binding}}},' for name, binding in p['blocks'])
            c.append('};')
            blocks = f'{key}_blocks'
        mask = lambda field: ', '.join(str(p['stages'][s][field]) if s in p['stages'] else '0' for s in STAGES)
        const_stages = sum((1 << i) for i, s in enumerate(STAGES) if s in p['stages'] and p['stages'][s]['constants'])
        defines = ';'.join(f'{k}={v}' for k, v in sorted(p['defines'].items()))
        vert, geom, frag = p['files']
        geom_c = f'"{geom}"' if geom else 'nullptr'
        entries.append(f'    {{"{game}", "{p["key"]}", "{vert}", {geom_c}, "{frag}", "{defines}", '
                       f'{{{", ".join(b[0] for b in blobs)}}}, {{{", ".join(b[1] for b in blobs)}}}, '
                       f'{uniforms}, {len(rows)}, {p["constant_size"]}, {{{mask("storage")}}}, {{{mask("uniforms")}}}, '
                       f'{const_stages}, {len(p["samplers"])}, {blocks}, {len(p["blocks"])}}},')
    c.append('}  // namespace')
    c.append('const Program programs[] = {')
    c.extend(entries)
    c.append('};')
    c.append('const size_t programCount = sizeof(programs) / sizeof(programs[0]);')
    c.append('}')
    text = '\n'.join(c) + '\n'
    out = args.output / 'deko_shaders.cpp'
    if not out.exists() or out.read_text(encoding='utf-8') != text:
        out.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    main()
