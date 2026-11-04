#!/usr/bin/env python3
"""Check that every HLSL cbuffer the client ships has the same layout on Direct3D and under SPIR-V.

The host fills each constant buffer as a C++ struct laid out by HLSL packing (fxc: a float3 or uint2 may sit at any
4-byte offset inside a 16-byte register, packoffset leaves gaps). The Linux build compiles the same HLSL with DXC to
SPIR-V and SPIRV-Cross turns it into a GLSL ES uniform block, which only accepts std140: a vec3 starts on 16, a vec2
on 8, and nothing may leave a gap. A cbuffer where the two disagree reads the wrong bytes on Linux without any
diagnostic from fxc, so this check simulates both packings.

For every file under src/render/shaders and the plugin header include/maicord/sdk/background.hlsli, and for every
combination of the macros its #if conditions name (with and without __spirv__, which DXC defines and fxc does not):

  * with __spirv__ defined, each cbuffer member must sit at the same offset in HLSL packing and in std140, and
    arrays and structs must have the same stride and size;
  * the layout under __spirv__ must occupy the same bytes as the layout without it: a divergent cbuffer is fixed
    inside `#if defined(__spirv__)` by naming the bytes with scalars (float3 -> three floats) and by naming gaps with
    members whose name contains `_unused_`.

Stdlib only.

  check_cbuffer_layout.py              check the shipped shaders (exit 1 on a divergence)
  check_cbuffer_layout.py --list       also print the HLSL layout of every cbuffer
  check_cbuffer_layout.py --self-test  check the checker on built-in good and bad cbuffers
"""
import itertools
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = sorted((ROOT / 'src' / 'render' / 'shaders').glob('*.hlsl')) + [ROOT / 'include' / 'maicord' / 'sdk' / 'background.hlsli']
SPIRV = '__spirv__'
MAXIMUM_MACROS = 7
MAXIMUM_REPORTED = 6


class LayoutError(Exception):
    pass


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def join_continuations(text):
    return re.sub(r'\\\n', ' ', text)


def condition_macros(text):
    """Macros named by #if/#ifdef/#ifndef/#elif lines and macros the file defines itself."""
    named, defined = set(), set()
    for line in text.split('\n'):
        stripped = line.strip()
        match = re.match(r'#\s*(if|ifdef|ifndef|elif)\b(.*)', stripped)
        if match:
            named.update(re.findall(r'[A-Za-z_]\w*', match.group(2)))
        match = re.match(r'#\s*define\s+([A-Za-z_]\w*)', stripped)
        if match:
            defined.add(match.group(1))
    named.discard('defined')
    return named, defined


def evaluate(expression, macros):
    expression = re.sub(r'defined\s*\(\s*(\w+)\s*\)', lambda m: '1' if m.group(1) in macros else '0', expression)
    expression = re.sub(r'defined\s+(\w+)', lambda m: '1' if m.group(1) in macros else '0', expression)

    def value(match):
        name = match.group(0)
        if name in macros:
            return str(macros[name])
        return '0'

    expression = re.sub(r'[A-Za-z_]\w*', value, expression)
    expression = expression.replace('&&', ' and ').replace('||', ' or ')
    expression = re.sub(r'!(?!=)', ' not ', expression)
    if not re.fullmatch(r'[\s\d()<>=!andornt+\-*/%&|]*', expression):
        raise LayoutError('unsupported preprocessor expression: %r' % expression)
    return bool(eval(expression, {'__builtins__': {}}, {}))


def preprocess(text, toggled):
    """Keep the lines of the active branches; apply #define NAME value. toggled maps macro -> defined (1) or not."""
    macros = {name: 1 for name, on in toggled.items() if on}
    stack = []  # (active, taken, parent_active)
    active = True
    out = []
    for line in text.split('\n'):
        stripped = line.strip()
        match = re.match(r'#\s*(\w+)\s*(.*)', stripped)
        if match:
            directive, rest = match.groups()
            if directive in ('if', 'ifdef', 'ifndef'):
                if directive == 'if':
                    condition = evaluate(rest, macros) if active else False
                elif directive == 'ifdef':
                    condition = rest.split()[0] in macros if active else False
                else:
                    condition = rest.split()[0] not in macros if active else False
                stack.append((active, condition))
                active = active and condition
                continue
            if directive == 'elif':
                parent, taken = stack.pop()
                condition = (not taken) and parent and evaluate(rest, macros)
                stack.append((parent, taken or condition))
                active = parent and condition
                continue
            if directive == 'else':
                parent, taken = stack.pop()
                stack.append((parent, True))
                active = parent and not taken
                continue
            if directive == 'endif':
                parent, _ = stack.pop()
                active = parent
                continue
            if active and directive == 'define':
                parts = rest.split(None, 1)
                if parts and '(' not in parts[0]:
                    value = parts[1].strip() if len(parts) > 1 else '1'
                    try:
                        macros[parts[0]] = int(value, 0)
                    except ValueError:
                        macros[parts[0]] = 1
                continue
            if active and directive == 'undef':
                macros.pop(rest.strip(), None)
                continue
            continue
        if active:
            out.append(line)
    if stack:
        raise LayoutError('unterminated #if')
    return '\n'.join(out), macros


# ---- HLSL types -----------------------------------------------------------------------------------------------

SCALARS = ('float', 'uint', 'int', 'bool', 'dword')
TYPE = re.compile(r'^(float|uint|int|bool|dword)([1-4])?$')


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


class Member:
    def __init__(self, name, kind, count, array, packoffset):
        self.name = name
        self.kind = kind          # 'vector' or the name of a struct
        self.count = count        # components of a vector
        self.array = array        # None or the array length
        self.packoffset = packoffset


def parse_members(body, structs):
    members = []
    for declaration in [part.strip() for part in body.split(';')]:
        if not declaration:
            continue
        declaration = re.sub(r'\b(nointerpolation|row_major|column_major|static|const|precise|unorm|snorm)\b', ' ', declaration)
        match = re.match(r'(\w+)\s+(.*)', declaration, flags=re.S)
        if not match:
            raise LayoutError('cannot parse member: %r' % declaration)
        type_name, rest = match.groups()
        if type_name in structs:
            kind, count = type_name, 0
        else:
            vector = TYPE.match(type_name)
            if not vector:
                raise LayoutError('unsupported cbuffer member type %r (matrices, halves and doubles are not modelled)'
                                  % type_name)
            kind, count = 'vector', int(vector.group(2) or 1)
        depth, start, declarators = 0, 0, []
        for index, character in enumerate(rest):
            if character in '([':
                depth += 1
            elif character in ')]':
                depth -= 1
            elif character == ',' and depth == 0:
                declarators.append(rest[start:index])
                start = index + 1
        declarators.append(rest[start:])
        for declarator in declarators:
            item = re.match(r'\s*(\w+)\s*((?:\[[^\]]*\]\s*)*)(?::\s*packoffset\(\s*c(\d+)(?:\.([xyzw]))?\s*\))?\s*$', declarator)
            if not item:
                raise LayoutError('cannot parse declarator: %r' % declarator.strip())
            name, arrays, register, component = item.groups()
            length = None
            sizes = re.findall(r'\[([^\]]*)\]', arrays)
            if len(sizes) > 1:
                raise LayoutError('multidimensional array %s is not modelled' % name)
            if sizes:
                length = ('expression', sizes[0])
            offset = None
            if register is not None:
                offset = int(register) * 16 + ('xyzw'.index(component) * 4 if component else 0)
            members.append(Member(name, kind, count, length, offset))
    return members


def parse_structs(text, macros):
    structs = {}
    for match in re.finditer(r'\bstruct\s+(\w+)\s*\{([^{}]*)\}\s*;', text):
        structs[match.group(1)] = match.group(2)
    return structs


def array_length(member, macros):
    if member.array is None:
        return None
    expression = member.array[1].strip()
    expression = re.sub(r'[A-Za-z_]\w*', lambda m: str(macros[m.group(0)]) if m.group(0) in macros else m.group(0),
                        expression)
    if not re.fullmatch(r'[\d\s+\-*/()]+', expression):
        raise LayoutError('array length of %s is not a constant: %r' % (member.name, member.array[1]))
    return int(eval(expression, {'__builtins__': {}}, {}))


def hlsl_layout(members, structs, macros, base=0, prefix=''):
    """Leaf (offset, bytes, path) records and the end offset of HLSL packing; offsets are relative to base."""
    leaves, cursor = [], 0
    for member in members:
        length = array_length(member, macros)
        if member.kind == 'vector':
            element = member.count * 4
        else:
            inner = parse_members(structs[member.kind], structs)
            _, element = hlsl_layout(inner, structs, macros)
            element = align_up(element, 16)
        stride = align_up(element, 16)
        if member.packoffset is not None:
            offset = member.packoffset
        elif length is not None or member.kind != 'vector':
            offset = align_up(cursor, 16)
        else:
            offset = cursor
            if offset % 16 + element > 16:
                offset = align_up(offset, 16)
        count = 1 if length is None else length
        for index in range(count):
            at = offset + index * stride
            path = prefix + member.name + ('' if length is None else '[%d]' % index)
            if member.kind == 'vector':
                leaves.append((base + at, element, path))
            else:
                inner_leaves, _ = hlsl_layout(parse_members(structs[member.kind], structs), structs, macros,
                                              base + at, path + '.')
                leaves.extend(inner_leaves)
        if length is None:
            cursor = offset + element
        else:
            cursor = offset + stride * (length - 1) + (element if member.kind == 'vector' else stride)
    return leaves, cursor


def std140_layout(members, structs, macros, base=0, prefix=''):
    """Leaf records, the size and the alignment of std140 packing."""
    leaves, cursor, maximum_alignment = [], 0, 16
    for member in members:
        length = array_length(member, macros)
        if member.kind == 'vector':
            size = member.count * 4
            alignment = {1: 4, 2: 8, 3: 16, 4: 16}[member.count]
            element_size, element_alignment = size, alignment
        else:
            inner = parse_members(structs[member.kind], structs)
            _, struct_size, struct_alignment = std140_layout(inner, structs, macros)
            element_size, element_alignment = struct_size, struct_alignment
        if length is not None:
            element_alignment = align_up(element_alignment, 16)
            stride = align_up(element_size, 16)
            offset = align_up(cursor, 16)
            cursor = offset + stride * length
        else:
            stride = element_size
            offset = align_up(cursor, element_alignment)
            cursor = offset + element_size
        count = 1 if length is None else length
        for index in range(count):
            at = offset + index * stride
            path = prefix + member.name + ('' if length is None else '[%d]' % index)
            if member.kind == 'vector':
                leaves.append((base + at, element_size, path))
            else:
                inner_leaves, _, _ = std140_layout(parse_members(structs[member.kind], structs), structs, macros,
                                                   base + at, path + '.')
                leaves.extend(inner_leaves)
    return leaves, align_up(cursor, maximum_alignment), maximum_alignment


def cbuffers(text):
    result = []
    for match in re.finditer(r'\bcbuffer\s+(\w+)\s*(?::\s*register\(\s*b(\d+)\s*\))?\s*\{([^{}]*)\}\s*;?', text):
        result.append((match.group(1), match.group(2), match.group(3)))
    return result


def slots(leaves):
    covered = {}
    for offset, size, path in leaves:
        for slot in range(offset // 4, (offset + size + 3) // 4):
            covered[slot] = path
    return covered


# ---- The check -------------------------------------------------------------------------------------------------

def check_source(label, source, list_layouts=False):
    """Returns (errors, cbuffer count, cbuffers whose SPIR-V layout differs from HLSL)."""
    text = join_continuations(strip_comments(source))
    named, defined = condition_macros(text)
    toggles = sorted((named - defined) | {SPIRV})
    if len(toggles) > MAXIMUM_MACROS:
        raise LayoutError('%s: %d conditional macros; raise MAXIMUM_MACROS' % (label, len(toggles)))
    errors, seen, guarded = [], set(), set()
    for combination in itertools.product((0, 1), repeat=len(toggles)):
        toggled = dict(zip(toggles, combination))
        if not toggled[SPIRV]:
            continue
        spirv_text, macros = preprocess(text, toggled)
        hlsl_toggled = dict(toggled)
        hlsl_toggled[SPIRV] = 0
        hlsl_text, hlsl_macros = preprocess(text, hlsl_toggled)
        spirv_structs, hlsl_structs = parse_structs(spirv_text, macros), parse_structs(hlsl_text, hlsl_macros)
        hlsl_buffers = {name: (register, body) for name, register, body in cbuffers(hlsl_text)}
        for name, register, body in cbuffers(spirv_text):
            tag = '%s: cbuffer %s%s' % (label, name, '' if register is None else ' (b%s)' % register)
            if name not in hlsl_buffers:
                errors.append('%s exists only under %s' % (tag, SPIRV))
                continue
            spirv_members = parse_members(body, spirv_structs)
            hlsl_members = parse_members(hlsl_buffers[name][1], hlsl_structs)
            spirv_hlsl, spirv_end = hlsl_layout(spirv_members, spirv_structs, macros)
            spirv_std, spirv_size, _ = std140_layout(spirv_members, spirv_structs, macros)
            hlsl_leaves, hlsl_end = hlsl_layout(hlsl_members, hlsl_structs, hlsl_macros)
            key = (name, tuple(sorted(spirv_hlsl)), tuple(sorted(hlsl_leaves)))
            if key in seen:
                continue
            seen.add(key)
            if list_layouts and not any(toggled[m] for m in toggles if m != SPIRV):
                print('%s\n  hlsl:   %s' % (tag, ', '.join('%s@%d' % (path, offset) for offset, _, path in hlsl_leaves)))
            std_by_path = {path: offset for offset, _, path in spirv_std}
            for offset, size, path in spirv_hlsl:
                if std_by_path.get(path) != offset:
                    errors.append('%s%s: member %s is at HLSL offset %d but std140 offset %s; under %s name its bytes '
                                  'with scalars and name gaps with _unused_ members'
                                  % (tag, self_macros(toggled), path, offset, std_by_path.get(path), SPIRV))
            if align_up(spirv_end, 16) != spirv_size:
                errors.append('%s%s: HLSL size %d but std140 size %d'
                              % (tag, self_macros(toggled), align_up(spirv_end, 16), spirv_size))
            wanted, got = slots(hlsl_leaves), slots(spirv_hlsl)
            for slot, path in sorted(wanted.items()):
                if slot not in got:
                    errors.append('%s%s: %s at byte %d exists without %s and not under it'
                                  % (tag, self_macros(toggled), path, slot * 4, SPIRV))
            for slot, path in sorted(got.items()):
                if slot not in wanted and '_unused_' not in path:
                    errors.append('%s%s: %s at byte %d is not in the layout without %s (name padding with _unused_)'
                                  % (tag, self_macros(toggled), path, slot * 4, SPIRV))
            if align_up(hlsl_end, 16) != align_up(spirv_end, 16):
                errors.append('%s%s: size is %d under %s but %d without it'
                              % (tag, self_macros(toggled), align_up(spirv_end, 16), SPIRV, align_up(hlsl_end, 16)))
            if sorted(hlsl_leaves) != sorted(spirv_hlsl):
                guarded.add(name)
    return errors, len({name for name, _, _ in seen}), guarded


def self_macros(toggled):
    on = [name for name, value in toggled.items() if value and name != SPIRV]
    return ' [%s]' % ', '.join(on) if on else ''


SELF_TESTS = [
    ('float3 after a float', False, 'cbuffer A : register(b0) { float a; float3 b; };'),
    ('uint2 after a uint', False, 'cbuffer A : register(b0) { uint a; uint2 b; uint c; };'),
    ('float3 inside an array element', False,
     'struct M { float4 c; float t; float3 r; }; cbuffer A : register(b0) { M m[4]; };'),
    ('packoffset gap', False,
     'cbuffer A : register(b0) { float a : packoffset(c0.x); float2 b : packoffset(c1.x); };'),
    ('plain scalars and vectors', True,
     'cbuffer A : register(b0) { float2 a; float2 b; float c; float d; float e; float f; float4 g[2]; uint2 h; };'),
    ('vec4 arrays and a float after a float3', True, 'cbuffer A : register(b0) { float3 a; float b; float4 c[3]; };'),
    ('guarded float3', True,
     'cbuffer A : register(b0) { float a;\n#if defined(__spirv__)\nfloat bx, by, bz;\n#else\nfloat3 b;\n#endif\n};'),
    ('guarded gap', True,
     'cbuffer A : register(b0) {\n#if defined(__spirv__)\nfloat a; float p0_unused_, p1_unused_, p2_unused_; float2 b;'
     ' float2 p3_unused_; float4 c;\n#else\nfloat a : packoffset(c0.x); float2 b : packoffset(c1.x);'
     ' float4 c : packoffset(c2);\n#endif\n};'),
    ('guarded edit that drops a byte', False,
     'cbuffer A : register(b0) { float a;\n#if defined(__spirv__)\nfloat bx, by;\n#else\nfloat3 b;\n#endif\n};'),
    ('padding not named _unused_', False,
     'cbuffer A : register(b0) {\n#if defined(__spirv__)\nfloat a; float p0, p1, p2; float2 b;\n#else\n'
     'float a : packoffset(c0.x); float2 b : packoffset(c1.x);\n#endif\n};'),
]


def self_test():
    failures = 0
    for name, expect_clean, source in SELF_TESTS:
        try:
            errors, _, _ = check_source('self-test', source)
        except LayoutError as error:
            errors = [str(error)]
        if bool(errors) == expect_clean:
            failures += 1
            print('FAIL self-test %s: expected %s, got %s' % (name, 'clean' if expect_clean else 'errors',
                                                              errors or 'clean'))
    if failures:
        return 1
    print('PASS check_cbuffer_layout self-test (%d cases)' % len(SELF_TESTS))
    return 0


def main(argv):
    if '--self-test' in argv:
        return self_test()
    list_layouts = '--list' in argv
    errors, files, buffers, guarded = [], 0, 0, []
    for path in SOURCES:
        label = path.relative_to(ROOT).as_posix()
        try:
            found, count, changed = check_source(label, path.read_text(encoding='utf-8'), list_layouts)
        except LayoutError as error:
            errors.append('%s: %s' % (label, error))
            continue
        files += 1
        buffers += count
        errors.extend(found[:MAXIMUM_REPORTED])
        if len(found) > MAXIMUM_REPORTED:
            errors.append('%s: %d more divergent members' % (label, len(found) - MAXIMUM_REPORTED))
        guarded.extend('%s:%s' % (path.name, name) for name in sorted(changed))
    for error in errors:
        print('FAIL %s' % error)
    if errors:
        return 1
    print('PASS check_cbuffer_layout: %d cbuffers in %d files agree between HLSL packing and std140 '
          '(%d rewritten under %s: %s)' % (buffers, files, len(guarded), SPIRV, ', '.join(guarded) or 'none'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
