#!/usr/bin/env python3
"""Compiles Sly 2's GLSL to SPIR-V for its Vulkan renderer (platform/vulkan/gl_vulkan.cpp), at build time.

    vulkan_shaders.py --source REPO --output DIR --glslang PATH/glslangValidator

The Vulkan renderer runs the game's OpenGL calls on Vulkan (the same split as the Switch's deko3d layer), so it needs
every program the game builds (its GLSHADER Init / FInit calls, with the variants it asks for) as SPIR-V, and the
tables the GL layer stands in for the driver's linker with. This script shares cmake/switch_shaders.py's parsing (the
loose uniforms' std140 layout, the stage interfaces matched by name) and writes DIR/vk_shaders.cpp, which defines the
table platform/vulkan/vk_shaders.h declares:

- the loose uniforms become one push-constant block (std140 offsets, after the samplers' slots): glUniform* write the
  program's copy, each draw pushes what changed;
- each sampler becomes a pair of indices into the global descriptor arrays (set 0: every texture and sampler, bound
  once), pushed at offset 8 x its binding: `texture(tex, uv)` reads sampler2D(caneTex2D[i], caneSamplers[j]);
- ARB_bindless_texture's sampler2D(handle) (glob's BINDLESS variant) reads the same arrays with the handle's halves
  (the layer's handles are those indices), nonuniform;
- the storage buffers keep their bindings in set 1 (push descriptors);
- gl_VertexID is gl_VertexIndex (both include the base vertex);
- the same arithmetic as Mesa's GLSL compiler makes of the OpenGL sources, so both renderers compute the same bits: a
  matrix times a vector is summed column by column from the first (Mesa's SPIR-V path sums from the last, with fused
  multiply-adds), normalize() is v * inversesqrt(dot(v, v)) (the SPIR-V path divides by the length), and inverse() of a
  mat3 is Mesa's GLSL built-in (cofactors, then each column / the determinant). The pictures then match bit for bit;
  without it a pixel here and there differs by one (floor() of a product near a whole number).

The variants are switch_shaders.py's plus BINDLESS (the game asks for it with the extension's #extension line, which
the layer reads as the define). VULKAN is defined for every program.

Two views (VR multiview, VK_KHR_multiview: both eyes in one render pass): each stage whose result depends on the eye is
compiled a second time with CANE_MULTIVIEW defined (`spirvMv`; the layer takes it for a pipeline of a two-view pass).
The eye's own values come from the storage block CaneViews (set 1, binding 11), indexed by gl_ViewIndex: the frame
matrix and camera position for every program that reads them (MV_UNIFORMS), the frame copies' texel offsets for the
fragment programs that read texDate / texAlpha (render.cpp keeps both views' copies side by side), and per-program
values (MV_PROGRAM); a stage may also have code of its own under #ifdef CANE_MULTIVIEW. The variant reads exactly
the values the eye-by-eye pass pushes, so both make the same bits.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import switch_shaders as sw  # noqa: E402
import glsl_fuse  # noqa: E402

SLY2 = 'Sly2-Release'
SLY2_ONLY = 'Sly2-Release only'   # (programs of Sly 2's own files, not built for another game's --game: VR's)
_glob = [
    # key suffix, geometry, defines
    ('', None, {}),
    ('_geom', 'glob.geom', {'GEOM': '1'}),
    ('_nokill', None, {'NOKILL': '1'}),
    ('_geom_nokill', 'glob.geom', {'GEOM': '1', 'NOKILL': '1'}),
]
PROGRAMS = []
for suffix, geom, defs in _glob:
    PROGRAMS.append(('glob' + suffix, 'glob.vert', geom, 'glob.frag', dict(defs), SLY2))
    PROGRAMS.append(('glob' + suffix + '_bindless', 'glob.vert', geom, 'glob.frag', dict(defs, BINDLESS='1'), SLY2))
    # (vertex pulling and the one blend pipeline, gl.cpp g_fDrawUniform / g_fBlend1: the Vulkan renderer on Android)
    PROGRAMS.append(('glob' + suffix + '_pull', 'glob.vert', geom, 'glob.frag', dict(defs, CANE_PULL='1', CANE_BLEND1='1'), SLY2))
    PROGRAMS.append(('glob' + suffix + '_bindless_pull', 'glob.vert', geom, 'glob.frag',
                     dict(defs, BINDLESS='1', CANE_PULL='1', CANE_BLEND1='1'), SLY2))
PROGRAMS += [
    ('celborder', 'celborder.vert', None, 'celborder.frag', {}, SLY2),
    ('hud', 'hud.vert', None, 'hud.frag', {}, SLY2),
    ('overlay', 'overlay.vert', None, 'overlay.frag', {}, SLY2),
    ('particle', 'particle.vert', None, 'particle.frag', {}, SLY2),
    ('screen', 'screen.vert', None, 'screen.frag', {}, SLY2),
    ('shadowcombine', 'shadowcombine.vert', None, 'shadowcombine.frag', {}, SLY2),
    ('shadowdate', 'shadowcombine.vert', None, 'shadowdate.frag', {}, SLY2),
    # (the shadow phases' frame alpha read in place: the colour attachment as input attachment, set 1 binding 16; the
    # layer offers them with VK_KHR_dynamic_rendering_local_read)
    ('shadowcombine_fetch', 'shadowcombine.vert', None, 'shadowcombine.frag', {'CANE_FETCH': '1'}, SLY2),
    ('shadowdate_fetch', 'shadowcombine.vert', None, 'shadowdate.frag', {'CANE_FETCH': '1'}, SLY2),
    ('shadowvol', 'shadowvol.vert', None, 'shadowvol.frag', {}, SLY2),
    ('shadowvol_nokill', 'shadowvol.vert', None, 'shadowvol.frag', {'NOKILL': '1'}, SLY2),
    # (VR, vr.cpp: the panel's copy, the comfort vignette, the RC table's aim mark)
    ('vrcopy', 'vrfull.vert', None, 'vrcopy.frag', {}, SLY2_ONLY),
    ('vrcopy_array', 'vrfull.vert', None, 'vrcopy.frag', {'CANE_ARRAY': '1'}, SLY2_ONLY),
    ('vrvignette', 'vrfull.vert', None, 'vrvignette.frag', {}, SLY2_ONLY),
    ('vrmark', 'vrmark.vert', None, 'vrmark.frag', {}, SLY2_ONLY),
]
# (the layer's own: the masked clears' full-frame triangle, platform/vulkan/shaders)
NATIVE = 'platform/vulkan/shaders'
PROGRAMS += [('cane_clear', 'clear.vert', None, 'clear.frag', {}, NATIVE),
             ('cane_present', 'present.vert', None, 'present.frag', {}, NATIVE)]

STAGES = sw.STAGES
NOFUSE = bool(os.environ.get('CANE_VK_NOFUSE'))
SET_TEXTURES, SET_BUFFERS = 0, 1
# (devices with only Vulkan's minimum of 128 push-constant bytes, e.g. Mali, SwiftShader: a program whose block is
# larger also gets a variant that reads the same block, at the same offsets, from a uniform buffer with a dynamic offset
# in set 2; the layer picks the variant at device creation, platform/vulkan/gl_vulkan.cpp)
SET_UNIFORMS = 2
PUSH_MINIMUM = 128
INPUT_KIND = {'float': 0, 'int': 1, 'uint': 2}
# (two views: see the module comment) the eye's values by uniform name, in every program, and per source file
MV_BLOCK = ('layout(std430, binding = 11) readonly buffer CaneViews { mat4 caneViewMat[2]; vec4 caneViewPos[2]; '
            'vec4 caneViewAux[2]; ivec4 caneViewFetch; };\n')
MV_UNIFORMS = {'matWorldToClip': 'caneViewMat[gl_ViewIndex]', 'posCamera': 'caneViewPos[gl_ViewIndex].xyz'}
MV_PROGRAM = {'vrvignette.frag': {'center': 'caneViewAux[gl_ViewIndex].xy', 'scale': 'caneViewAux[gl_ViewIndex].zw'},
              'vrcopy.frag': {'layer': 'int(gl_ViewIndex)'}}
MV_FETCH = re.compile(r'texelFetch\(\s*(texDate|texAlpha)\s*,\s*ivec2\(gl_FragCoord\.xy\)')
VERT_INPUT = re.compile(r'layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s+(\w+)\s+(\w+)\s*;')
SAMPLER_CTOR = {'sampler2D': ('caneTex2D', 'texture2D'), 'sampler2DArray': ('caneTexArray', 'texture2DArray')}

# (Mesa's GLSL lowering, written out: see the module comment)
HELPERS = """vec4 caneMulMV(mat4 m, vec4 v) { return m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3] * v.w; }
vec3 caneMulMV(mat3 m, vec3 v) { return m[0] * v.x + m[1] * v.y + m[2] * v.z; }
vec2 caneNormalize(vec2 v) { return v * inversesqrt(dot(v, v)); }
vec3 caneNormalize(vec3 v) { return v * inversesqrt(dot(v, v)); }
vec4 caneNormalize(vec4 v) { return v * inversesqrt(dot(v, v)); }
mat3 caneInverse(mat3 m)
{
    float f11_22_21_12 = m[1][1] * m[2][2] - m[2][1] * m[1][2];
    float f10_22_20_12 = m[1][0] * m[2][2] - m[2][0] * m[1][2];
    float f10_21_20_11 = m[1][0] * m[2][1] - m[2][0] * m[1][1];
    mat3 adj;
    adj[0][0] = f11_22_21_12;
    adj[1][0] = -f10_22_20_12;
    adj[2][0] = f10_21_20_11;
    adj[0][1] = -(m[0][1] * m[2][2] - m[2][1] * m[0][2]);
    adj[1][1] = m[0][0] * m[2][2] - m[2][0] * m[0][2];
    adj[2][1] = -(m[0][0] * m[2][1] - m[2][0] * m[0][1]);
    adj[0][2] = m[0][1] * m[1][2] - m[1][1] * m[0][2];
    adj[1][2] = -(m[0][0] * m[1][2] - m[1][0] * m[0][2]);
    adj[2][2] = m[0][0] * m[1][1] - m[1][0] * m[0][1];
    float det = m[0][0] * f11_22_21_12 - m[0][1] * f10_22_20_12 + m[0][2] * f10_21_20_11;
    return mat3(adj[0] / det, adj[1] / det, adj[2] / det);
}
"""
TOKEN = re.compile(r'\s+|(?P<tok>[A-Za-z_]\w*|\d+\.?\d*(?:[eE][-+]?\d+)?[uUfF]?|\.\d+(?:[eE][-+]?\d+)?[fF]?|\*=|[-+*/%<>=!&|^]=?|[()\[\]{}.,;?:~])')
KEYWORDS = {'return', 'if', 'else', 'for', 'while', 'do', 'switch', 'case', 'default', 'break', 'continue', 'discard',
            'in', 'out', 'inout', 'const', 'uniform', 'flat', 'noperspective', 'smooth'}
MATRIX_CALLS = {'inverse', 'caneInverse', 'transpose', 'mat2', 'mat3', 'mat4'}


def tokens(src):
    out = []
    for m in TOKEN.finditer(src):
        if m.group('tok'):
            out.append((m.group('tok'), m.start('tok'), m.end('tok')))
    return out


def gl_arithmetic(src, structs, mats):
    """Matrix x vector products, normalize() and inverse() rewritten as Mesa's GLSL compiler lowers them."""
    src = re.sub(r'\bnormalize\s*\(', 'caneNormalize(', src)
    src = re.sub(r'\binverse\s*\(', 'caneInverse(', src)
    fields = {f for body in structs.values() for kind, f, _ in body if kind.startswith('mat')}
    is_ident = lambda t: bool(re.fullmatch(r'[A-Za-z_]\w*', t)) and t not in KEYWORDS
    is_operand = lambda t: is_ident(t) or bool(re.fullmatch(r'[\d.]\S*', t))

    def match(toks, i, step):
        open_, close = ('(', ')') if toks[i][0] in '()' else ('[', ']')
        depth = 0
        while True:
            t = toks[i][0]
            if t == (close if step < 0 else open_):
                depth += 1
            elif t == (open_ if step < 0 else close):
                depth -= 1
                if depth == 0:
                    return i
            i += step

    def is_matrix(toks, a, b):   # the operand toks[a:b]
        t = [x[0] for x in toks[a:b]]
        if len(t) == 1:
            return t[0] in mats
        if t[-1] == ')' and t[0] in MATRIX_CALLS and t[1] == '(':
            return True
        if t[-1] == ']' and t[1] == '[' and match(toks, a + 1, 1) == b - 1:
            return t[0] in mats
        if len(t) >= 3 and t[-2] == '.':
            return t[-1] in fields
        return False

    while True:
        toks = tokens(src)
        done = True
        for k, (t, _, _) in enumerate(toks):
            if t != '*':
                continue
            # the left operand: a postfix expression ending just before the '*'
            i = k - 1
            while i >= 0:
                tt = toks[i][0]
                if tt in (')', ']'):
                    i = match(toks, i, -1) - 1
                    if i >= 0 and (is_ident(toks[i][0]) or toks[i][0] in (')', ']')):
                        continue
                    break
                if is_operand(tt):
                    i -= 1
                    if i >= 0 and toks[i][0] == '.':
                        i -= 1
                        continue
                    break
                break
            a = i + 1
            if a >= k or not is_matrix(toks, a, k):
                continue
            # the right operand: unary signs, a primary, its postfix parts
            j = k + 1
            while toks[j][0] in ('-', '+', '!'):
                j += 1
            if toks[j][0] == '(':
                j = match(toks, j, 1) + 1
            elif is_operand(toks[j][0]):
                j += 1
                if toks[j][0] == '(':
                    j = match(toks, j, 1) + 1
            else:
                fail(f'cannot read the operand after a matrix product: {src[toks[k][1]:toks[k][1] + 40]!r}')
            while toks[j][0] in ('[', '.'):
                j = match(toks, j, 1) + 1 if toks[j][0] == '[' else j + 2
            if is_matrix(toks, k + 1, j):
                continue   # (a matrix product: not lowered here)
            left, right = src[toks[a][1]:toks[k - 1][2]], src[toks[k + 1][1]:toks[j - 1][2]]
            src = src[:toks[a][1]] + f'caneMulMV({left}, {right})' + src[toks[j - 1][2]:]
            done = False
            break
        if done:
            return src


def fail(message):
    raise SystemExit(f'vulkan_shaders.py: {message}')


def input_kind(kind):
    if kind.startswith('uvec') or kind == 'uint':
        return INPUT_KIND['uint']
    if kind.startswith('ivec') or kind == 'int':
        return INPUT_KIND['int']
    return INPUT_KIND['float']


def build_program(root, key, files, defines, directory, out_dir, glslang):
    sources = {}
    stage_defines = dict(defines, VULKAN='1')
    for stage, name in zip(STAGES, files):
        if name is None:
            continue
        text = (root / directory / name).read_text(encoding='utf-8-sig')
        sources[stage] = sw.preprocess(sw.strip_comments(text), stage_defines)
    structs = {}
    for src in sources.values():
        structs.update(sw.parse_structs(src))
    # (the names with a matrix type: variables, parameters, uniforms, storage arrays)
    mats = set()
    for src in sources.values():
        mats.update(re.findall(r'\bmat[234]\s+(\w+)', src))

    fields, samplers, stage_uniforms, stage_samplers = {}, {}, {}, {}
    for stage, src in sources.items():
        stage_uniforms[stage] = set()
        stage_samplers[stage] = set()
        for kind, name, count in sw.UNIFORM.findall(src):
            if kind.startswith('sampler'):
                if kind not in SAMPLER_CTOR:
                    fail(f'{key}: unsupported sampler type {kind}')
                entry = samplers.setdefault(name, {'type': kind, 'binding': len(samplers), 'stages': 0})
                if entry['type'] != kind:
                    fail(f'{key}: sampler {name} differs between stages')
                entry['stages'] |= 1 << STAGES.index(stage)
                stage_samplers[stage].add(name)
                continue
            n = int(count) if count else 1
            if name in fields and fields[name] != (kind, n):
                fail(f'{key}: uniform {name} differs between stages')
            fields[name] = (kind, n)
            stage_uniforms[stage].add(name)
    # the samplers' index pairs first (8 bytes each), then the constants (std140, from a 16-byte boundary)
    base = sw.align(8 * len(samplers), 16)
    members, rows = [], []
    offset = 0
    for name, (kind, n) in fields.items():
        a, size, stride, sub = sw.std140(kind, structs)
        if n > 1:
            if sub is not None:
                fail(f'{key}: array of struct uniform {name}')
            a, size = 16, stride * n
        offset = sw.align(offset, a)
        members.append({'name': name, 'type': kind, 'offset': base + offset, 'count': n})
        if sub is None:
            rows.append({'name': name, 'type': kind, 'offset': base + offset, 'size': size, 'count': n,
                         'stride': stride if n > 1 else size})
        else:
            for fname, fkind, foff, fsize, fcount, fstride in sub:
                rows.append({'name': f'{name}.{fname}', 'type': fkind, 'offset': base + offset + foff, 'size': fsize,
                             'count': fcount, 'stride': fstride})
        offset += size
    push_size = base + sw.align(offset, 16) if fields else base

    # stage interface locations (switch_shaders.py's rule: the GLSL's own first, the rest matched by name)
    occupied, locations = set(), {}
    for stage, src in sources.items():
        for m in sw.INTERFACE.finditer(src):
            if m.group('loc') is None or (stage == 'vert' and m.group('dir') == 'in') or (stage == 'frag' and m.group('dir') == 'out'):
                continue
            n = sw.slots(m.group('type'), 1 if stage == 'geom' else sw.array_count(m.group('array')), structs)
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
    if 'vert' in sources:
        for loc, kind, _ in VERT_INPUT.findall(sources['vert']):
            inputs[int(loc)] = input_kind(kind)

    def stage_text(stage, src, mv):
        if re.search(r'\bgl_InstanceID\b', src):
            fail(f'{key}.{stage}: gl_InstanceID (Vulkan counts the base instance in) is not handled')
        src = re.sub(r'\bgl_VertexID\b', 'gl_VertexIndex', src)
        src = gl_arithmetic(src, structs, mats)
        src = sw.UNIFORM.sub('', src)
        if mv is not None:
            # (two views: the eye's values from CaneViews; the uniforms stay in the push block, unread)
            for n, v in mv.items():
                src = re.sub(r'\b' + n + r'\b', v, src)
            if stage == 'frag':
                src = MV_FETCH.sub(r'texelFetch(\1, ivec2(gl_FragCoord.xy) + ivec2(caneViewFetch.x * int(gl_ViewIndex), 0)', src)
            if 'caneView' in src:
                src = MV_BLOCK + src

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
                    locations[name] = allocate(sw.slots(kind, 1 if stage == 'geom' else sw.array_count(array), structs))
                loc = locations[name]
            return f'{m.group("lead")}layout(location = {loc}) {qual}{direction} {kind} {name}{array};'
        src = sw.INTERFACE.sub(interface, src)

        # storage blocks into set 1 (their bindings kept)
        storage = 0

        def block(m):
            nonlocal storage
            qualifiers, access, kind, name = m.group(1), m.group(2) or '', m.group(3), m.group(4)
            binding = re.search(r'\bbinding\s*=\s*(\d+)', qualifiers)
            if not binding:
                fail(f'{key}.{stage}: block {name} has no binding')
            if kind != 'buffer':
                fail(f'{key}.{stage}: uniform block {name}: not handled (no Sly 2 program has one)')
            slot = int(binding.group(1))
            if slot >= 16:
                fail(f'{key}.{stage}: storage block {name} binding {slot} above 15')
            storage |= 1 << slot
            return f'layout({qualifiers}, set = {SET_BUFFERS}) {access}{kind} {name} {{'
        src = sw.BLOCK.sub(block, src)

        uses_samplers = bool(stage_samplers[stage]) or 'BINDLESS' in defines
        decl = []
        if uses_samplers:
            decl += [f'layout(set = {SET_TEXTURES}, binding = 0) uniform texture2D caneTex2D[];',
                     f'layout(set = {SET_TEXTURES}, binding = 1) uniform sampler caneSamplers[];',
                     f'layout(set = {SET_TEXTURES}, binding = 2) uniform texture2DArray caneTexArray[];']
        block_members = []
        for name, s in samplers.items():
            if name in stage_samplers[stage]:
                block_members.append(f'    layout(offset = {8 * s["binding"]}) uvec2 caneTex_{s["binding"]};')
        for f in members:
            if f['name'] in stage_uniforms[stage]:
                suffix = f'[{f["count"]}]' if f['count'] > 1 else ''
                block_members.append(f'    layout(offset = {f["offset"]}) {f["type"]} {f["name"]}{suffix};')
        if block_members:
            decl += ['layout(push_constant, std140) uniform CanePush', '{'] + block_members + ['};']
        for name, s in samplers.items():
            if name in stage_samplers[stage]:
                array, _ = SAMPLER_CTOR[s['type']]
                b = s['binding']
                decl.append(f'#define {name} {s["type"]}({array}[caneTex_{b}.x], caneSamplers[caneTex_{b}.y])')
        if 'BINDLESS' in defines:
            # (the layer's texture handles: x the image's slot, y the sampler's)
            src = re.sub(r'\bsampler2D\(\s*(\w+)\s*\)',
                         r'sampler2D(caneTex2D[nonuniformEXT(\1.x)], caneSamplers[nonuniformEXT(\1.y)])', src)
        if 'CANE_FETCH' in defines and stage == 'frag':
            decl.append(f'layout(input_attachment_index = 0, set = {SET_BUFFERS}, binding = 16) uniform subpassInput caneFbFetch;')
        decl.append(glsl_fuse.fma_helpers() + HELPERS)
        if decl:
            func = re.search(r'^[ \t]*(?:void|bool|float|int|uint|vec[234]|ivec[234]|uvec[234]|mat[34]|\w+)\s+\w+\s*\([^;{]*\)\s*\{',
                             src, re.M)
            if not func:
                fail(f'{key}.{stage}: no function to place the declarations before')
            src = src[:func.start()] + '\n'.join(decl) + '\n' + src[func.start():]

        if stage in ('vert', 'geom') and not NOFUSE:
            src = glsl_fuse.fuse_source(src)   # (the OpenGL compiler's fused multiply-adds: cmake/glsl_fuse.py)
        header = '#version 460\n#extension GL_EXT_nonuniform_qualifier : require\n'
        if mv is not None:
            header += '#extension GL_EXT_multiview : require\n'
        return header + src, storage

    def view_source(stage, name):
        """The stage's two-view source (None: the same as the one view's): preprocessed with CANE_MULTIVIEW, the eye's
        values from CaneViews."""
        text = (root / directory / name).read_text(encoding='utf-8-sig')
        src = sw.preprocess(sw.strip_comments(text), dict(stage_defines, CANE_MULTIVIEW='1'))
        changed = src != sources[stage]
        if sorted(sw.UNIFORM.findall(src)) != sorted(sw.UNIFORM.findall(sources[stage])):
            fail(f'{key}.{stage}: CANE_MULTIVIEW changes the uniforms (the push block must stay the same)')
        names = set(sw.UNIFORM.findall(src))
        subst = {n: v for n, v in dict(MV_UNIFORMS, **MV_PROGRAM.get(name, {})).items() if any(u[1] == n for u in names)}
        if subst or (stage == 'frag' and MV_FETCH.search(src)):
            changed = True
        if not changed:
            return None, subst
        return src, subst

    result = {}
    for stage, name in zip(STAGES, files):
        if stage not in sources:
            continue
        text, storage = stage_text(stage, sources[stage], None)

        def compile_text(text, suffix):
            glsl = out_dir / f'{key}{suffix}.{stage}.glsl'
            binary = out_dir / f'{key}{suffix}.{stage}.spv'
            glsl.write_text(text, encoding='utf-8')
            proc = subprocess.run([str(glslang), '-V', '--target-env', 'vulkan1.2', '--quiet', '-S', stage, '-o', str(binary),
                                   str(glsl)], capture_output=True, text=True)
            if proc.returncode != 0:
                sys.stderr.write(proc.stdout + proc.stderr)
                fail(f'glslangValidator failed on {glsl}')
            data = binary.read_bytes()
            if len(data) < 20 or len(data) % 4 or data[:4] != b'\x03\x02\x23\x07':
                fail(f'invalid SPIR-V output: {binary}')
            return data
        result[stage] = {'data': compile_text(text, ''), 'storage': storage, 'ubo': None, 'mv': None, 'storage_mv': storage}
        if push_size > PUSH_MINIMUM:
            push_decl = 'layout(push_constant, std140) uniform CanePush'
            ubo_text = text.replace(push_decl, f'layout(set = {SET_UNIFORMS}, binding = 0, std140) uniform CanePush')
            result[stage]['ubo'] = compile_text(ubo_text, '_ubo')
        mv_src, subst = view_source(stage, name)
        if mv_src is not None:
            mv_text, mv_storage = stage_text(stage, mv_src, subst)
            result[stage]['mv'] = compile_text(mv_text, '_mv')
            result[stage]['storage_mv'] = mv_storage

    sampler_list = sorted(samplers.items(), key=lambda kv: kv[1]['binding'])
    return {'key': key, 'files': files, 'defines': defines, 'rows': rows, 'push_size': push_size,
            'samplers': sampler_list, 'stages': result, 'inputs': inputs}


def c_words(name, data):
    words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
    lines = [f'const uint32_t {name}[] = {{']
    for i in range(0, len(words), 12):
        lines.append(','.join(f'0x{w:08x}u' for w in words[i:i + 12]) + ',')
    lines.append('};')
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--glslang', type=Path, required=True)
    parser.add_argument('--game', default=SLY2, help='the game folder whose GLSL sources are compiled (default Sly2-Release)')
    args = parser.parse_args()

    work = args.output / 'vk_shaders'
    work.mkdir(parents=True, exist_ok=True)
    programs = [build_program(args.source, key, (vert, geom, frag), defines, directory, work, args.glslang)
                for key, vert, geom, frag, defines, directory in
                [(k, v, g, f, d, args.game if dr in (SLY2, SLY2_ONLY) else dr) for k, v, g, f, d, dr in PROGRAMS
                 if dr != SLY2_ONLY or args.game == SLY2]]

    c = ['// Generated by cmake/vulkan_shaders.py from the GLSL sources: do not edit.',
         '#include "vk_shaders.h"', 'namespace CaneVk::Shaders {', 'namespace {']
    entries = []
    for p in programs:
        key = p['key']
        blobs, ubo_blobs, mv_blobs = [], [], []
        for stage in STAGES:
            if stage in p['stages'] and p['stages'][stage]['mv']:
                c.extend(c_words(f'{key}_{stage}_mv', p['stages'][stage]['mv']))
                mv_blobs.append((f'{key}_{stage}_mv', f'sizeof({key}_{stage}_mv)'))
            else:
                mv_blobs.append(('nullptr', '0'))
            if stage in p['stages']:
                c.extend(c_words(f'{key}_{stage}', p['stages'][stage]['data']))
                blobs.append((f'{key}_{stage}', f'sizeof({key}_{stage})'))
            else:
                blobs.append(('nullptr', '0'))
            if stage in p['stages'] and p['stages'][stage]['ubo']:
                c.extend(c_words(f'{key}_{stage}_ubo', p['stages'][stage]['ubo']))
                ubo_blobs.append((f'{key}_{stage}_ubo', f'sizeof({key}_{stage}_ubo)'))
            else:
                ubo_blobs.append(('nullptr', '0'))
        rows = [f'    {{"{f["name"]}", {f["offset"]}, {f["size"]}, {f["count"]}, {f["stride"]}, {sw.TYPE_CODES[f["type"]]}, 0, 0}},'
                for f in p['rows']]
        rows += [f'    {{"{name}", {8 * s["binding"]}, 8, 1, 8, {sw.TYPE_CODES[s["type"]]}, {s["binding"]}, {s["stages"]}}},'
                 for name, s in p['samplers']]
        uniforms = 'nullptr'
        if rows:
            c.append(f'const Uniform {key}_uniforms[] = {{')
            c.extend(rows)
            c.append('};')
            uniforms = f'{key}_uniforms'
        storage = ', '.join(str(p['stages'][s]['storage']) if s in p['stages'] else '0' for s in STAGES)
        storage_mv = ', '.join(str(p['stages'][s]['storage_mv']) if s in p['stages'] else '0' for s in STAGES)
        input_mask = sum(1 << loc for loc in p['inputs'])
        input_kinds = sum(kind << (2 * loc) for loc, kind in p['inputs'].items())
        defines = ';'.join(f'{k}={v}' for k, v in sorted(p['defines'].items()))
        vert, geom, frag = p['files']
        geom_c = f'"{geom}"' if geom else 'nullptr'
        entries.append(f'    {{"{p["key"]}", "{vert}", {geom_c}, "{frag}", "{defines}", '
                       f'{{{", ".join(b[0] for b in blobs)}}}, {{{", ".join(b[1] for b in blobs)}}}, '
                       f'{uniforms}, {len(rows)}, {p["push_size"]}, {{{storage}}}, {len(p["samplers"])}, '
                       f'{input_mask}u, {input_kinds}u, '
                       f'{{{", ".join(b[0] for b in ubo_blobs)}}}, {{{", ".join(b[1] for b in ubo_blobs)}}}, '
                       f'{{{", ".join(b[0] for b in mv_blobs)}}}, {{{", ".join(b[1] for b in mv_blobs)}}}, {{{storage_mv}}}}},')
    c.append('}  // namespace')
    c.append('const Program programs[] = {')
    c.extend(entries)
    c.append('};')
    c.append('const size_t programCount = sizeof(programs) / sizeof(programs[0]);')
    c.append('}')
    text = '\n'.join(c) + '\n'
    out = args.output / 'vk_shaders.cpp'
    if not out.exists() or out.read_text(encoding='utf-8') != text:
        out.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    main()
