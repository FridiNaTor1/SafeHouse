"""Mesa's multiply-add fusion written out in GLSL source (cmake/vulkan_shaders.py, the vertex and geometry stages).

Mesa's OpenGL drivers fuse a multiply feeding an add into one fused multiply-add (NIR's late algebraic pass: fadd(fmul(a,
b), c) -> ffma(a, b, c), the first operand's product first; a - b is a + -b). RADV does the same, except for what feeds
the position (and the branches deciding it), which it keeps exact (nir_propagate_invariant with invariant_prim): there
the products and sums stay apart, and the vertices land a fraction of a pixel's 1/256 elsewhere than on OpenGL. This
module rewrites the expressions of the vertex and geometry programs with explicit fma() where the OpenGL compiler fuses,
so both renderers compute the same position bits (an explicit fma stays fused when exact).

Rules (the NIR pattern, applied to the source as written): a / CONST is a product with 1 / CONST, a / b a product with
1.0 / b (fdiv is lowered to a multiply by the reciprocal before fusion); in l + r the left operand's product is fused if
it is one, else the right's; in l - r likewise with -r (fneg(fmul(a, b)) fuses as fma(-a, b, l)); x += y is x = x + y.
A product held in a variable is left alone (NIR fuses it only if used once; the programs here have none such).
"""
import re
import struct

TOKEN = re.compile(r'\s+|(?P<tok>[A-Za-z_]\w*|0[xX][0-9a-fA-F]+[uU]?|\d+(?:\.\d*)?[eE][-+]?\d+[fF]?|\d+\.\d*(?:[eE][-+]?\d+)?[fF]?|\d*\.\d+(?:[eE][-+]?\d+)?[fF]?|\d+[uU]?'
                   r'|\+\+|--|&&|\|\||<<|>>|[-+*/%<>=!]=|[-+*/%<>=!&|^?:~(),.\[\]{};])')
ASSIGN = {'=', '+=', '-=', '*=', '/='}


class Parser:
    def __init__(self, toks):
        self.t = toks
        self.i = 0

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else None

    def take(self, want=None):
        tok = self.peek()
        if want is not None and tok != want:
            raise SyntaxError(f'expected {want!r}, got {tok!r} in {" ".join(self.t)}')
        self.i += 1
        return tok

    def expr(self):          # no comma operator: commas separate arguments / declarators
        return self.ternary()

    def ternary(self):
        c = self.binary(0)
        if self.peek() == '?':
            self.take('?')
            a = self.ternary()
            self.take(':')
            b = self.ternary()
            return ('tern', c, a, b)
        return c

    LEVELS = [('||',), ('&&',), ('|',), ('^',), ('&',), ('==', '!='), ('<', '>', '<=', '>='), ('<<', '>>'), ('+', '-'),
              ('*', '/', '%')]

    def binary(self, level):
        if level == len(self.LEVELS):
            return self.unary()
        node = self.binary(level + 1)
        while self.peek() in self.LEVELS[level]:
            op = self.take()
            node = ('bin', op, node, self.binary(level + 1))
        return node

    def unary(self):
        if self.peek() in ('-', '+', '!', '~', '++', '--'):
            op = self.take()
            return ('un', op, self.unary())
        return self.postfix(self.primary())

    def primary(self):
        tok = self.take()
        if tok == '(':
            e = self.expr()
            self.take(')')
            return ('par', e)
        if tok is None:
            raise SyntaxError('unexpected end')
        return ('num', tok) if re.match(r'[\d.]', tok) else ('id', tok)

    def postfix(self, node):
        while True:
            p = self.peek()
            if p == '(' and node[0] == 'id':
                self.take('(')
                args = []
                if self.peek() != ')':
                    args.append(self.expr())
                    while self.peek() == ',':
                        self.take(',')
                        args.append(self.expr())
                self.take(')')
                node = ('call', node[1], args)
            elif p == '[' and self.peek(1) == ']' and node[0] == 'id':   # (an array constructor: vec2[](...))
                self.take('[')
                self.take(']')
                node = ('id', node[1] + '[]')
            elif p == '[':
                self.take('[')
                e = self.expr()
                self.take(']')
                node = ('idx', node, e)
            elif p == '.':
                self.take('.')
                node = ('fld', node, self.take())
            elif p in ('++', '--'):
                node = ('post', self.take(), node)
            else:
                return node


def strip(n):
    while n[0] == 'par':
        n = n[1]
    return n


def as_product(n):
    """(a, b) when n is a multiply (or a division, as a multiply by the reciprocal), else None."""
    n = strip(n)
    if n[0] == 'bin' and n[1] == '*':
        return n[2], n[3]
    if n[0] == 'bin' and n[1] == '/':
        d = strip(n[3])
        if d[0] == 'num' and '.' in d[1]:
            v = struct.unpack('<f', struct.pack('<f', 1.0 / float(d[1].rstrip('fF'))))[0]
            return n[2], ('num', repr(v) if 'e' not in repr(v) else '%.9e' % v)
        num = strip(n[2])
        if num[0] == 'num' and re.fullmatch(r'1(\.0*)?[fF]?', num[1]):
            return None   # (1 / b: NIR's frcp, not a product)
        return n[2], ('bin', '/', ('num', '1.0'), n[3])
    return None


def neg(n):
    return ('un', '-', ('par', n))


INT_FIELDS = set()    # struct members with an integer type
INTS = set()          # names with an integer type (fuse_source fills it from the program's declarations)
INT_CALLS = {'int', 'uint', 'ivec2', 'ivec3', 'ivec4', 'uvec2', 'uvec3', 'uvec4', 'floatBitsToUint', 'floatBitsToInt',
             'bitCount', 'findLSB', 'findMSB'}


def is_int(n):
    """An integer-typed expression (integer arithmetic is never fused)."""
    n = strip(n)
    k = n[0]
    if k == 'num':
        return bool(re.fullmatch(r'(0[xX][0-9a-fA-F]+|\d+)[uU]?', n[1]))
    if k == 'id':
        return n[1] in INTS or n[1] in ('gl_VertexIndex', 'gl_InstanceIndex', 'gl_PrimitiveIDIn')
    if k == 'fld':
        return is_int(n[1]) or n[2] in INT_FIELDS
    if k == 'idx':
        return is_int(n[1])
    if k == 'call':
        if n[1] in INT_CALLS:
            return True
        if n[1] in ('min', 'max', 'abs', 'clamp'):
            return any(is_int(a) for a in n[2])
        return False
    if k == 'un':
        return is_int(n[2])
    if k == 'bin':
        return is_int(n[2]) or is_int(n[3])
    if k == 'tern':
        return is_int(n[2])
    return False


def fuse(n):
    """The tree with Mesa's fusion made explicit (children first, as NIR rewrites bottom-up... top-down on adds)."""
    k = n[0]
    if k == 'bin':
        op, l, r = n[1], fuse(n[2]), fuse(n[3])
        if op in ('+', '-') and not is_int(l) and not is_int(r):
            pl, pr = as_product(l), as_product(r)
            rr = r if op == '+' else neg(r)
            if pl:
                return ('call', 'caneFma', [pl[0], pl[1], rr])
            if pr:
                return ('call', 'caneFma', [pr[0] if op == '+' else neg(pr[0]), pr[1], l])
        return ('bin', op, l, r)
    if k == 'un':
        return ('un', n[1], fuse(n[2]))
    if k == 'par':
        return ('par', fuse(n[1]))
    if k == 'tern':
        return ('tern', fuse(n[1]), fuse(n[2]), fuse(n[3]))
    if k == 'call':
        return ('call', n[1], [fuse(a) for a in n[2]])
    if k == 'idx':
        return ('idx', fuse(n[1]), n[2])   # (an index: integer arithmetic)
    if k == 'fld':
        return ('fld', fuse(n[1]), n[2])
    return n


def emit(n):
    k = n[0]
    if k in ('id', 'num'):
        return n[1]
    if k == 'par':
        return '(' + emit(n[1]) + ')'
    if k == 'bin':
        return '(' + emit(n[2]) + ' ' + n[1] + ' ' + emit(n[3]) + ')'
    if k == 'un':
        return '(' + n[1] + emit(n[2]) + ')'
    if k == 'post':
        return emit(n[2]) + n[1]
    if k == 'tern':
        return '(' + emit(n[1]) + ' ? ' + emit(n[2]) + ' : ' + emit(n[3]) + ')'
    if k == 'call':
        return n[1] + '(' + ', '.join(emit(a) for a in n[2]) + ')'
    if k == 'idx':
        return emit(n[1]) + '[' + emit(n[2]) + ']'
    if k == 'fld':
        return emit(n[1]) + '.' + n[2]
    raise ValueError(k)


def fuse_expr(toks):
    p = Parser(toks)
    tree = p.expr()
    if p.i != len(toks):
        raise SyntaxError('trailing tokens: ' + ' '.join(toks[p.i:]))
    return emit(fuse(tree))


def split_top(toks, sep):
    out, cur, depth = [], [], 0
    for t in toks:
        if t in '([':
            depth += 1
        elif t in ')]':
            depth -= 1
        if t == sep and depth == 0:
            out.append(cur)
            cur = []
        else:
            cur.append(t)
    out.append(cur)
    return out


def statement(toks):
    """One statement's tokens (no braces), rewritten."""
    if not toks:
        return ''
    head = toks[0]
    if head == 'else':
        return 'else ' + statement(toks[1:])
    if head in ('if', 'while'):
        depth, j = 0, 1
        for j in range(1, len(toks)):
            if toks[j] == '(':
                depth += 1
            elif toks[j] == ')':
                depth -= 1
                if depth == 0:
                    break
        return f'{head} ({fuse_expr(toks[2:j])}) ' + statement(toks[j + 1:])
    if head == 'for' or head in ('break', 'continue', 'discard') or head.startswith('#'):
        return ' '.join(toks)
    if head == 'return':
        return 'return ' + fuse_expr(toks[1:]) if len(toks) > 1 else 'return'
    # an assignment or a declaration (possibly several declarators)
    depth, at = 0, -1
    for j, t in enumerate(toks):
        if t in '([':
            depth += 1
        elif t in ')]':
            depth -= 1
        elif depth == 0 and t in ASSIGN:
            at = j
            break
    if at < 0:
        return ' '.join(toks)   # (a call or a declaration without initializer)
    lhs, op, rest = toks[:at], toks[at], toks[at + 1:]
    parts = split_top(rest, ',')
    if op in ('+=', '-='):
        expr = lhs + [op[0], '('] + parts[0] + [')']
        out = ' '.join(lhs) + ' = ' + fuse_expr(expr)
    else:
        out = ' '.join(lhs) + ' ' + op + ' ' + fuse_expr(parts[0])
    for more in parts[1:]:   # (int a = 1, b = 2: each declarator's initializer)
        name, _, init = more[0], more[1], more[2:]
        out += ', ' + name + ' = ' + fuse_expr(init)
    return out


def fuse_source(src):
    """Every function body of a vertex / geometry program with Mesa's fusion made explicit."""
    int_decl = r'\b(?:u?int|[iu]vec[234]|bool)\s+(\w+)'
    INT_FIELDS.clear()
    for body in re.findall(r'\bstruct\s+\w+\s*\{([^}]*)\}', src):
        INT_FIELDS.update(re.findall(int_decl, body))
    # (the program's global names: outside the structs and function bodies; each function adds its own)
    outside = re.sub(r'\bstruct\s+\w+\s*\{[^}]*\}', '', src)
    depth, flat = 0, []
    for ch in outside:
        depth += ch == '{'
        if depth == 0:
            flat.append(ch)
        depth -= ch == '}'
    globals_ = set(re.findall(int_decl, ''.join(flat)))
    out = []
    pos = 0
    for m in re.finditer(r'^[ \t]*(?:\w+)\s+\w+\s*\([^;{)]*\)\s*\{', src, re.M):
        if m.start() < pos:
            continue
        # the body: matched braces
        depth, j = 0, m.end() - 1
        while True:
            c = src[j]
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append(src[pos:m.end()])
        body = src[m.end():j]
        INTS.clear()
        INTS.update(globals_)
        INTS.update(re.findall(int_decl, m.group(0) + body))
        toks = [x.group('tok') for x in TOKEN.finditer(body) if x.group('tok')]
        lines, cur = [], []
        for t in toks:
            if t in ('{', '}'):
                if cur:
                    lines.append(statement(cur))
                    cur = []
                lines.append(t)
            elif t == ';':
                # (a for header's semicolons stay with it)
                if cur and cur[0] == 'for' and cur.count('(') > cur.count(')'):
                    cur.append(t)
                    continue
                lines.append(statement(cur) + ';')
                cur = []
            else:
                cur.append(t)
        if cur:
            lines.append(statement(cur))
        out.append('\n' + '\n'.join(lines) + '\n')
        pos = j
    out.append(src[pos:])
    return ''.join(out)


def fma_helpers():
    """caneFma: fma() for every mix of scalar and vector operands (GLSL's fma wants one type)."""
    out = ['float caneFma(float a, float b, float c) { return fma(a, b, c); }']
    for n in (2, 3, 4):
        v = f'vec{n}'
        for ta in ('float', v):
            for tb in ('float', v):
                for tc in ('float', v):
                    if ta == tb == tc == 'float':
                        continue
                    args = [x if t == v else f'{v}({x})' for x, t in (('a', ta), ('b', tb), ('c', tc))]
                    out.append(f'{v} caneFma({ta} a, {tb} b, {tc} c) {{ return fma({", ".join(args)}); }}')
    return '\n'.join(out) + '\n'
