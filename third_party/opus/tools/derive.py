"""Derives maiopus sources from libopus files for this build configuration.

Two mechanical steps, both reproducible from the upstream tarball:
1. Preprocessor conditionals over configuration macros (DEFINED/UNDEFINED) are
   resolved with three-valued logic; a condition that involves any other macro
   (compiler, platform, x86 SIMD) is kept verbatim.
2. QEXT/fixed-point argument macros are expanded (see EXPANSIONS).

Usage: derive.py FILE... rewrites the files in place.
"""
import re
import sys

DEFINED = {
    'FIXED_POINT': 1, 'DISABLE_FLOAT_API': 1, 'USE_ALLOCA': 1, 'CUSTOM_SUPPORT': 1,
    'ENABLE_HARDENING': 1, 'OVERRIDE_celt_fatal': 1, 'OPUS_BUILD': 1,
}
UNDEFINED = set('''
CUSTOM_MODES CUSTOM_MODES_ONLY ENABLE_OPUS_CUSTOM_API ENABLE_DEEP_PLC ENABLE_DRED DRED_EXPERIMENTAL_VERSION
ENABLE_OSCE ENABLE_OSCE_BWE ENABLE_OSCE_TRAINING_DATA ENABLE_QEXT ENABLE_RES24 ENABLE_ASSERTIONS ENABLE_VALGRIND
FUZZING RESYNTH MEASURE_NORM_MSE FIXED_DEBUG FIXED_DEBUG_ASSERT FIXED_POINT_DEBUG FLOAT_APPROX HAVE_CONFIG_H
VAR_ARRAYS NONTHREADSAFE_PSEUDOSTACK OPUS_CHECK_ASM OPUS_HAVE_RTCD OPUS_ARM_ASM OPUS_ARM_INLINE_ASM
OPUS_ARM_INLINE_EDSP OPUS_ARM_MAY_HAVE_NEON_INTR OPUS_ARM_PRESUME_AARCH64_NEON_INTR HAVE_ARM_NE10 OPUS_XTENSA_LX7
__mips __mips_dsp CONFIG_TI_C54X CONFIG_TI_C55X CONFIG_TI_C6X ENABLE_TI_DSPLIB ENABLE_TI_DSPLIB55 TI_C5X_ASM
TI_C6X_ASM BFIN_ASM EMBEDDED_ARM R5900 DISABLE_UPDATE_DRAFT DISABLE_LACE DISABLE_NOLACE DISABLE_PTR_CHECK
SMALL_FOOTPRINT USE_WEIGHTS_FILE MLP_TRAINING silk_MACRO_COUNT NO_ASSERTS OPUS_WILL_BE_SLOW SKIP_CONFIG_H
DLL_EXPORT ANALYSIS_C
'''.split())

TOKEN = re.compile(r'\s*(?:(0[xX][0-9a-fA-F]+|\d+)[uUlL]*|([A-Za-z_]\w*)|(&&|\|\||==|!=|<=|>=|<<|>>|[!()<>+\-*/%~,?:&|^]))')
UNKNOWN = object()
CR = chr(13)
BACKSLASH = chr(92)
LF = chr(10)


class Expr:
    def __init__(self, text):
        self.tokens = []
        pos = 0
        text = text.strip()
        while pos < len(text):
            m = TOKEN.match(text, pos)
            if not m:
                raise ValueError('cannot tokenize: ' + text)
            if m.group(1):
                self.tokens.append(('num', int(m.group(1), 0)))
            elif m.group(2):
                self.tokens.append(('id', m.group(2)))
            else:
                self.tokens.append(('op', m.group(3)))
            pos = m.end()
            while pos < len(text) and text[pos].isspace():
                pos += 1
        self.i = 0

    def peek(self):
        return self.tokens[self.i] if self.i < len(self.tokens) else (None, None)

    def take(self):
        t = self.peek()
        self.i += 1
        return t

    def parse(self):
        v = self.logor()
        if self.i != len(self.tokens):
            raise ValueError('trailing tokens')
        return v

    def logor(self):
        v = self.logand()
        while self.peek() == ('op', '||'):
            self.take()
            r = self.logand()
            if v is not UNKNOWN and v:
                v = 1
            elif r is not UNKNOWN and r:
                v = 1
            elif v is UNKNOWN or r is UNKNOWN:
                v = UNKNOWN
            else:
                v = 0
        return v

    def logand(self):
        v = self.compare()
        while self.peek() == ('op', '&&'):
            self.take()
            r = self.compare()
            if v is not UNKNOWN and not v:
                v = 0
            elif r is not UNKNOWN and not r:
                v = 0
            elif v is UNKNOWN or r is UNKNOWN:
                v = UNKNOWN
            else:
                v = 1
        return v

    def compare(self):
        v = self.additive()
        while self.peek()[0] == 'op' and self.peek()[1] in ('==', '!=', '<', '>', '<=', '>='):
            op = self.take()[1]
            r = self.additive()
            if v is UNKNOWN or r is UNKNOWN:
                v = UNKNOWN
            else:
                v = int({'==': v == r, '!=': v != r, '<': v < r, '>': v > r, '<=': v <= r, '>=': v >= r}[op])
        return v

    def additive(self):
        v = self.unary()
        while self.peek()[0] == 'op' and self.peek()[1] in ('+', '-', '*', '/', '%', '<<', '>>'):
            op = self.take()[1]
            r = self.unary()
            if v is UNKNOWN or r is UNKNOWN or (op in '/%' and r == 0):
                v = UNKNOWN
            else:
                v = {'+': v + r, '-': v - r, '*': v * r, '/': v // r if r else 0, '%': v % r if r else 0,
                     '<<': v << r, '>>': v >> r}[op]
        return v

    def unary(self):
        kind, value = self.take()
        if kind == 'op' and value == '!':
            v = self.unary()
            return UNKNOWN if v is UNKNOWN else int(not v)
        if kind == 'op' and value == '-':
            v = self.unary()
            return UNKNOWN if v is UNKNOWN else -v
        if kind == 'op' and value == '(':
            v = self.logor()
            if self.take() != ('op', ')'):
                raise ValueError('missing )')
            return v
        if kind == 'num':
            return value
        if kind == 'id' and value == 'defined':
            paren = self.peek() == ('op', '(')
            if paren:
                self.take()
            k, name = self.take()
            if k != 'id':
                raise ValueError('defined without name')
            if paren and self.take() != ('op', ')'):
                raise ValueError('defined missing )')
            if name in DEFINED:
                return 1
            if name in UNDEFINED:
                return 0
            return UNKNOWN
        if kind == 'id':
            if self.peek() == ('op', '('):  # function-like macro call: unknown
                depth = 0
                while True:
                    k, v = self.take()
                    if k is None:
                        raise ValueError('unterminated call')
                    if (k, v) == ('op', '('):
                        depth += 1
                    elif (k, v) == ('op', ')'):
                        depth -= 1
                        if depth == 0:
                            break
                return UNKNOWN
            if value in DEFINED:
                return DEFINED[value]
            if value in UNDEFINED:
                return 0
            return UNKNOWN
        raise ValueError('unexpected token %r' % (value,))


def evaluate(directive, rest):
    if directive == 'ifdef':
        rest = 'defined(' + rest.split()[0] + ')'
    elif directive == 'ifndef':
        rest = '!defined(' + rest.split()[0] + ')'
    try:
        return Expr(rest).parse()
    except ValueError:
        return UNKNOWN  # e.g. __has_builtin(...): kept verbatim


def strip_comments(text, in_comment):
    """Returns (code without comments, still-in-block-comment)."""
    out = []
    i = 0
    while i < len(text):
        if in_comment:
            end = text.find('*/', i)
            if end < 0:
                return ''.join(out), True
            i = end + 2
            in_comment = False
            out.append(' ')
            continue
        c = text[i]
        if text.startswith('/*', i):
            in_comment = True
            i += 2
            continue
        if text.startswith('//', i):
            break
        if c in '"\'':
            j = i + 1
            while j < len(text) and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        out.append(c)
        i += 1
    return ''.join(out), in_comment


DIRECTIVE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$', re.S)


def process(text):
    lines = text.split('\n')
    out = []
    stack = []  # frames: dict(dead, kept, taken, emit)
    in_comment = False
    i = 0
    resolved = 0

    def emitting():
        return all(f['emit'] and not f['dead'] for f in stack)

    while i < len(lines):
        start = i
        line = lines[i]
        code, comment_after = strip_comments(line, in_comment)
        m = DIRECTIVE.match(code) if not in_comment else None
        if not m:
            in_comment = comment_after
            if emitting():
                out.append(line)
            i += 1
            continue
        # Gather continuation lines and a trailing block comment.
        unit = [line]
        logical = line
        while logical.rstrip(CR).endswith(BACKSLASH) and i + 1 < len(lines):
            i += 1
            unit.append(lines[i])
            logical = logical.rstrip(CR)[:-1] + ' ' + lines[i]
        code, in_comment_end = strip_comments(logical, in_comment)
        while in_comment_end and i + 1 < len(lines):
            i += 1
            unit.append(lines[i])
            logical += '\n' + lines[i]
            code, in_comment_end = strip_comments(logical, in_comment)
        in_comment = False
        m = DIRECTIVE.match(code)
        directive, rest = m.group(1), m.group(2).strip()
        parent = emitting()
        i += 1
        if directive in ('if', 'ifdef', 'ifndef'):
            if not parent:
                stack.append({'dead': True, 'kept': False, 'taken': False, 'emit': False})
                continue
            v = evaluate(directive, rest)
            if v is UNKNOWN:
                stack.append({'dead': False, 'kept': True, 'taken': False, 'emit': True})
                out.extend(unit)
            else:
                resolved += 1
                stack.append({'dead': False, 'kept': False, 'taken': bool(v), 'emit': bool(v)})
        elif directive == 'elif':
            f = stack[-1]
            if f['dead']:
                continue
            if f['taken']:
                f['emit'] = False
                continue
            v = evaluate('if', rest)
            if v is UNKNOWN:
                if f['kept']:
                    out.extend(unit)
                else:
                    first = unit[0]
                    out.append(re.sub(r'#(\s*)elif', r'#\1if', first, count=1))
                    out.extend(unit[1:])
                    f['kept'] = True
                f['emit'] = True
            elif v:
                resolved += 1
                if f['kept']:
                    out.append(re.match(r'^(\s*#\s*)', unit[0]).group(1) + 'else' + (CR if unit[0].endswith(CR) else ''))
                f['taken'] = True
                f['emit'] = True
            else:
                resolved += 1
                f['emit'] = False
        elif directive == 'else':
            f = stack[-1]
            if f['dead']:
                continue
            if f['taken']:
                f['emit'] = False
                continue
            if f['kept']:
                out.extend(unit)
            f['taken'] = True
            f['emit'] = True
        else:  # endif
            f = stack.pop()
            if f['kept'] and not f['dead']:
                out.extend(unit)
    if stack:
        raise ValueError('unbalanced conditionals')
    return '\n'.join(out), resolved


# Function-like macros whose expansion in this configuration is inlined:
# ARG_QEXT(x) -> nothing, ARG_FIXED(x) -> ", x", QEXT_SCALE(x) and
# QEXT_SCALE2(x, s) -> x (parenthesized unless x is a single token).
def first_argument(arg):
    depth = 0
    for i, c in enumerate(arg):
        depth += (c == '(') - (c == ')')
        if c == ',' and depth == 0:
            return arg[:i].strip()
    return arg


def scaled(arg):
    arg = first_argument(arg)
    return arg if re.fullmatch(r'\w+', arg) else '(' + arg + ')'


EXPANSIONS = {'ARG_QEXT': lambda arg: '', 'ARG_FIXED': lambda arg: ', ' + arg,
              'QEXT_SCALE': scaled, 'QEXT_SCALE2': scaled}
KEEP_SPACE = {'QEXT_SCALE', 'QEXT_SCALE2'}
MACRO_CALL = re.compile(r'([ \t]*)\b(ARG_QEXT|ARG_FIXED|QEXT_SCALE2|QEXT_SCALE)\s*\(')
MACRO_DEFINITION = re.compile(r'^[ \t]*#[ \t]*define[ \t]+(ARG_QEXT|ARG_FIXED|QEXT_SCALE2|QEXT_SCALE)\([^)]*\)[^\n]*\n', re.M)


def expand_line(line):
    out = []
    pos = 0
    while True:
        m = MACRO_CALL.search(line, pos)
        if not m:
            break
        depth = 1
        i = m.end()
        while depth:
            if i >= len(line):
                raise ValueError('macro call spans lines: ' + line)
            depth += (line[i] == '(') - (line[i] == ')')
            i += 1
        out.append(line[pos:m.start()])
        if m.group(2) in KEEP_SPACE:
            out.append(m.group(1))
        out.append(EXPANSIONS[m.group(2)](' '.join(line[m.end():i - 1].split())))
        pos = i
    out.append(line[pos:])
    return ''.join(out)


def expand(text):
    text = MACRO_DEFINITION.sub('', text)
    result = []
    for line in text.split(LF):
        expanded = expand_line(line)
        if expanded != line:
            body = expanded.rstrip(CR).strip()
            cr = CR if line.endswith(CR) else ''
            if not body:
                continue
            if body.startswith(')') and result:
                result[-1] = result[-1].rstrip(CR).rstrip() + body + cr
                continue
        result.append(expanded)
    return LF.join(result)


def derive(data):
    """Returns the derived bytes of one upstream source file."""
    text = data.decode('latin-1')
    result, _ = process(text)
    result = expand(result)
    result = re.sub(r'(\r?\n){4,}', lambda m: m.group(1) * 3, result)
    return result.encode('latin-1')


def main():
    for path in sys.argv[1:]:
        with open(path, 'rb') as handle:
            raw = handle.read()
        data = derive(raw)
        if data != raw:
            with open(path, 'wb') as handle:
                handle.write(data)
            print('%s: %d -> %d lines' % (path, raw.count(b'\n'), data.count(b'\n')))


if __name__ == '__main__':
    main()



