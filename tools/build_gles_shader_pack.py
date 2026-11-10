#!/usr/bin/env python3
"""Builds the GLSL ES shader pack of the Linux GPU backend from the hand-written GLSL in src/render/shaders/gles.

The shader list is the maicord_shader(... PACK <name> ...) calls of cmake/generated_sources.cmake, in call order,
which is the order of ShaderId (src/gpu/gles/shader_pack.cpp checks it). Each one is the twin of an HLSL file the
Windows build compiles: src/render/shaders/<x>.hlsl is src/render/shaders/gles/<x>.glsl (rules and macros in the
README beside the GLSL; tools/check_shader_twins.py keeps the pair in step). Per shader:

  <x>.glsl --pruned for the shader's stage and defines, the binding manifest read from its declarations--> GLSL ES 3.10
           --comments and whitespace stripped, the names the shader defines made short--> text
           --one solid deflate stream for all shaders--> pack

No compiler runs here: the text is what the driver compiles (--validate also runs glslangValidator on each text when it
is on PATH). The stream comes from tools/optimal_deflate.py and tools/deflate_cache.txt like every other shipped table
(namespace gles_shaders; MAICORD_UPDATE_DEFLATE_CACHE=1 searches the miss, then commit the cache).

Usage: build_gles_shader_pack.py OUTPUT.inc [--validate] [--dump DIRECTORY]
Writes OUTPUT.inc and maicord_shader_pack_gles.bin beside it (read by #embed). --dump also writes every shader's final
text as DIRECTORY/<name>.<stage>.glsl.
"""
import collections
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

import optimal_deflate
from font_reference.embed_fixture import embed_arrays, write_if_changed

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHADER_LIST = ROOT / 'cmake' / 'generated_sources.cmake'
GLSL_DIRECTORY = ROOT / 'src' / 'render' / 'shaders' / 'gles'
PACKED_FILE = 'maicord_shader_pack_gles.bin'
STAGES = {'vs': 0, 'ps': 1, 'cs': 2}
STAGE_KIND = {'vs': 'vert', 'ps': 'frag', 'cs': 'comp'}
CACHE = optimal_deflate.DeflateCache('gles_shaders')
GLES_VARIANTS = [('colr_layers_narrow_compute', 'colr_layers_compute', ['COLR_SLICES=2'])]

# GlesBindingKind of src/gpu/gles/internal.hpp, and the limits that header and the backend enforce.
UNIFORM_BUFFER, TEXTURE, STORAGE_SRV, STORAGE_UAV, IMAGE = range(5)
NO_SAMPLER = 255
UNIFORM_BINDINGS, PIXEL_UNIFORM_BASE, UNITS, STORAGE_BINDINGS, IMAGE_BINDINGS, RESOURCE_SLOTS = 16, 8, 64, 8, 8, 16


class ShaderError(Exception):
    pass


def shader_list():
    """(name, stage, entry, source path, defines) of every packed shader, in pack order."""
    text = SHADER_LIST.read_text()
    directory = re.search(r'set\(maicord_shaders\s+(\S+)\)', text).group(1)
    shaders = []
    for call in re.finditer(r'maicord_shader\(([^)]*)\)', text):
        words = call.group(1).split()
        # <variable> <header> <profile> <entry> <symbol> <source> [PACK <name>] [<define>...]
        if 'PACK' not in words:
            continue
        pack = words.index('PACK')
        stage = words[2][:2]
        source = words[5].replace('${maicord_shaders}', directory)
        shaders.append((words[pack + 1], stage, words[3], source, words[pack + 2:]))
    # Variants only this backend uses, after the shaders of the ShaderId order: (name, the shader it replaces on a
    # device that cannot run it, defines added to it). colr_layers cuts each pixel's paint plan into COLR_SLICES runs
    # evaluated by one thread each, so its group is 8 x 8 x COLR_SLICES invocations: 512 by default, above the 128 an
    # OpenGL ES 3.1 device has to offer; two slices fit every device and give the same picture.
    for name, base, extra in GLES_VARIANTS:
        original = next(shader for shader in shaders if shader[0] == base)
        shaders.append((name, original[1], original[2], original[3], original[4] + extra))
    names = [shader[0] for shader in shaders]
    if not shaders or len(set(names)) != len(names):
        raise SystemExit('error: cannot read the packed shaders of cmake/generated_sources.cmake')
    return shaders


def twin_of(source):
    """The GLSL twin of an HLSL source path (src/render/shaders/x.hlsl)."""
    return GLSL_DIRECTORY / (pathlib.PurePosixPath(source).stem + '.glsl')


# ---- the source text: comments, annotations, conditionals ---------------------------------------------------------

def strip_comments(text):
    """[(code, annotation)] per line: `//` and `/* */` comments removed; the annotation is the text of a `//!` comment
    (the HLSL register of the declaration on that line) or None."""
    lines, annotations = [], []
    block = False
    for line in text.replace('\r\n', '\n').split('\n'):
        out, at, annotation = [], 0, None
        while at < len(line):
            if block:
                end = line.find('*/', at)
                if end < 0:
                    at = len(line)
                else:
                    block, at = False, end + 2
                    out.append(' ')
                continue
            slash = line.find('/', at)
            if slash < 0 or slash + 1 >= len(line):
                out.append(line[at:])
                break
            out.append(line[at:slash])
            if line[slash + 1] == '/':
                if line.startswith('//!', slash):
                    annotation = line[slash + 3:].strip()
                break
            if line[slash + 1] == '*':
                block, at = True, slash + 2
                continue
            out.append('/')
            at = slash + 1
        lines.append(''.join(out))
        annotations.append(annotation)
    return list(zip(lines, annotations))


def evaluate(expression, macros):
    """The value of a preprocessor condition: defined(X), integer macros, ! && || == != < > <= >= + - and parentheses."""
    def defined(match):
        return '1' if (match.group(1) or match.group(2)) in macros else '0'
    expression = re.sub(r'\bdefined\s*(?:\(\s*(\w+)\s*\)|(\w+))', defined, expression)

    def value(match):
        word = macros.get(match.group(0), '0')
        if not re.fullmatch(r'-?\d+', word):
            raise ShaderError('the macro %s in "#if %s" is not a number' % (match.group(0), expression))
        return word
    expression = re.sub(r'\b[A-Za-z_]\w*\b', value, expression)
    expression = expression.replace('&&', ' and ').replace('||', ' or ')
    expression = re.sub(r'!(?!=)', ' not ', expression)
    if not re.fullmatch(r'[\d\s()<>=!+\-andortn]*', expression):
        raise ShaderError('cannot evaluate "#if %s"' % expression)
    return bool(eval(expression, {'__builtins__': {}}, {}))


def preprocess(text, stage, entry, defines, name):
    """([(code line, annotation)], [#extension lines]) of the shader `name` of `text`: the conditionals evaluated for
    `stage` (STAGE_VS, STAGE_PS, STAGE_CS), the HLSL `entry` (ENTRY_<entry>) and `defines` ("NAME" or "NAME=value"),
    `#define` macros expanded in place. Nothing else survives."""
    macros = {'STAGE_' + stage.upper(): '1', 'ENTRY_' + entry: '1', 'V2F': 'out' if stage == 'vs' else 'in'}
    for define in defines:
        key, _, value = define.partition('=')
        macros[key] = value or '1'
    out, extensions = [], []
    stack = []  # [parent active, a branch was taken, this branch active]
    for number, (code, annotation) in enumerate(strip_comments(text), 1):
        stripped = code.strip()
        active = all(frame[2] for frame in stack)
        if stripped.startswith('#'):
            directive, _, rest = stripped[1:].strip().partition(' ')
            rest = rest.strip()
            where = '%s: line %d' % (name, number)
            if directive in ('if', 'ifdef', 'ifndef'):
                if not active:
                    stack.append([False, True, False])
                    continue
                if directive == 'if':
                    taken = evaluate(rest, macros)
                else:
                    taken = (rest in macros) == (directive == 'ifdef')
                stack.append([True, taken, taken])
            elif directive == 'elif':
                frame = stack[-1]
                frame[2] = frame[0] and not frame[1] and evaluate(rest, macros)
                frame[1] = frame[1] or frame[2]
            elif directive == 'else':
                frame = stack[-1]
                frame[2] = frame[0] and not frame[1]
                frame[1] = True
            elif directive == 'endif':
                stack.pop()
            elif not active:
                continue
            elif directive == 'define':
                key, _, value = rest.partition(' ')
                if '(' in key or not re.fullmatch(r'[A-Za-z_]\w*', key):
                    raise ShaderError('%s: only object-like macros are supported' % where)
                macros[key] = value.strip()
            elif directive == 'undef':
                macros.pop(rest, None)
            elif directive == 'extension':
                extensions.append('#extension ' + rest)
            else:
                raise ShaderError('%s: the directive #%s is not supported' % (where, directive))
            continue
        if active and stripped:
            out.append((stripped, annotation))
    if stack:
        raise ShaderError('%s: an #if is not closed' % name)
    words = [key for key, value in macros.items() if value and not key.startswith(('STAGE_', 'ENTRY_'))]
    if words:
        pattern = re.compile(r'\b(' + '|'.join(sorted(map(re.escape, words), key=len, reverse=True)) + r')\b')
        for _ in range(8):  # a macro may use another; the nesting is shallow
            changed = [(pattern.sub(lambda match: macros[match.group(0)], code), annotation) for code, annotation in out]
            if changed == out:
                break
            out = changed
        else:
            raise ShaderError('%s: macros nest too deeply' % name)
    return out, extensions


# ---- the binding manifest ------------------------------------------------------------------------------------------

LAYOUT = re.compile(r'layout\s*\(([^)]*)\)\s*(.*)')


def parse_layout(parts):
    """{qualifier: value or None} of the inside of layout(...)."""
    result = {}
    for part in parts.split(','):
        key, _, value = part.partition('=')
        result[key.strip()] = int(value) if value.strip().isdigit() else None
    return result


def register(annotation, letter, what):
    """(slot, sampler or None) of an annotation "t0", "t0 s0" or "u2" (free text may follow)."""
    match = re.match(r'%s(\d+)(?:\s+s(\d+))?' % letter, annotation or '')
    if not match:
        raise ShaderError('%s needs a //! %s<slot> annotation' % (what, letter))
    slot = int(match.group(1))
    if slot >= RESOURCE_SLOTS:
        raise ShaderError('%s: register %s is out of range' % (what, annotation))
    return slot, int(match.group(2)) if match.group(2) else None


def binding_rows(lines, stage, name):
    """The manifest rows (kind, slot, binding, sampler) of the declarations in `lines`, in declaration order.

    The GL binding is the declaration's layout(binding = N); the HLSL register it stands for is the block's binding
    (b#: 0..7 for a vertex or compute shader, 8..15 for a pixel shader) or the declaration's //! annotation (t#, u#,
    s#)."""
    rows, units = [], {}
    for code, annotation in lines:
        match = LAYOUT.match(code)
        if not match:
            continue
        qualifiers, rest = parse_layout(match.group(1)), match.group(2)
        binding = qualifiers.get('binding')
        if binding is None:
            continue
        what = '%s: "%s"' % (name, code[:60])
        if re.match(r'uniform\s+\w+\s*\{', rest) and 'std140' in qualifiers:
            slot = binding - (PIXEL_UNIFORM_BASE if stage == 'ps' else 0)
            if slot < 0 or slot >= 8 or binding >= UNIFORM_BINDINGS:
                raise ShaderError('%s: a %s uniform block binds b0..b7 at %d..%d' % (
                    what, 'pixel' if stage == 'ps' else 'vertex or compute', PIXEL_UNIFORM_BASE if stage == 'ps' else 0,
                    (PIXEL_UNIFORM_BASE if stage == 'ps' else 0) + 7))
            rows.append((UNIFORM_BUFFER, slot, binding, NO_SAMPLER))
        elif re.match(r'uniform\s+(?:highp\s+)?sampler2D\s+\w+\s*;', rest):
            slot, sampler = register(annotation, 't', what)
            if binding >= UNITS or units.setdefault(('t', binding), what) != what:
                raise ShaderError('%s: texture unit %d is out of range or used twice' % (what, binding))
            rows.append((TEXTURE, slot, binding, NO_SAMPLER if sampler is None else sampler))
        elif re.match(r'(?:readonly\s+)?buffer\s+\w+\s*\{', rest) and 'std430' in qualifiers:
            srv = rest.startswith('readonly')
            slot, _ = register(annotation, 't' if srv else 'u', what)
            if binding >= STORAGE_BINDINGS or units.setdefault(('s', binding), what) != what:
                raise ShaderError('%s: storage binding %d is out of range or used twice' % (what, binding))
            rows.append((STORAGE_SRV if srv else STORAGE_UAV, slot, binding, NO_SAMPLER))
        elif re.search(r'\bimage2D\b', rest):
            slot, _ = register(annotation, 'u', what)
            if binding >= IMAGE_BINDINGS or units.setdefault(('i', binding), what) != what:
                raise ShaderError('%s: image binding %d is out of range or used twice' % (what, binding))
            rows.append((IMAGE, slot, binding, NO_SAMPLER))
        else:
            raise ShaderError('%s: a declaration with layout(binding) that the pack does not know' % what)
    if stage == 'vs' and any(row[0] not in (UNIFORM_BUFFER, TEXTURE) for row in rows):
        raise ShaderError('%s: a vertex shader binds only uniform blocks and textures' % name)
    return rows


# ---- the minified text ----------------------------------------------------------------------------------------------

TOKEN = re.compile(r'0[xX][0-9a-fA-F]+[uU]?|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[uUfF]?|[A-Za-z_]\w*'
                   r'|<<=|>>=|\+\+|--|<<|>>|&&|\|\||\^\^|[=!<>+\-*/%&|^]=|\S')
WORD = re.compile(r'\w')
# Two characters that make one token when nothing separates them.
JOINS = {'++', '--', '<<', '>>', '&&', '||', '^^', '//', '/*', '*/', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=',
         '==', '!=', '<=', '>='}


def minify(lines):
    """One line of GLSL: the source lines joined, with the spaces that no token boundary needs removed."""
    pieces, previous = [], ''
    for code, _ in lines:
        for token in TOKEN.findall(code):
            if previous and ((WORD.match(previous[-1]) and WORD.match(token[0])) or previous[-1] + token[0] in JOINS):
                pieces.append(' ')
            pieces.append(token)
            previous = token
    return ''.join(pieces)


PREAMBLE = {'vs': '', 'ps': 'precision highp float;precision highp int;', 'cs': ''}


def assemble(shader, validate, work):
    """(GLSL text as ASCII bytes, binding rows) of one packed shader."""
    name, stage, entry, source, defines = shader
    twin = twin_of(source)
    if not twin.is_file():
        raise ShaderError('%s: no GLSL twin %s of %s' % (name, twin.relative_to(ROOT).as_posix(), source))
    lines, extensions = preprocess(twin.read_text(), stage, entry, defines, name)
    rows = binding_rows(lines, stage, name)
    header = '\n'.join(['#version 310 es'] + extensions) + '\n' + PREAMBLE[stage]
    text = (header + minify(lines) + '\n').encode('ascii')
    if validate:
        validator = shutil.which('glslangValidator')
        if validator:
            # The line-preserving text first: its error lines are the pruned source's, the minified text's are not.
            readable = header.count('\n') + 1
            for kind, body in (('src', header + '\n' + '\n'.join(code for code, _ in lines) + '\n'), ('min', None)):
                scratch = pathlib.Path(work) / ('%s.%s.%s' % (name, kind, STAGE_KIND[stage]))
                scratch.write_bytes(text if body is None else body.encode('ascii'))
                result = subprocess.run([validator, str(scratch)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True)
                if result.returncode != 0:
                    report = []
                    for line in result.stdout.strip().splitlines():
                        report.append(line)
                        found = re.search(r'ERROR: 0:(\d+):', line)
                        if found and body is not None and 0 <= int(found.group(1)) - readable - 1 < len(lines):
                            report.append('    ' + lines[int(found.group(1)) - readable - 1][0].strip())
                    raise ShaderError('glslangValidator for %s (%s) failed:\n%s' % (name, kind, '\n'.join(report)))
    return text, rows


# ---- shorter names --------------------------------------------------------------------------------------------------

# What no shader name may be or collide with: the keywords, reserved words, types, qualifiers and built-in functions of
# GLSL ES 3.10 (the names inside layout(...) and the swizzles are found in the text, see shorten_names).
KEPT = frozenset('''
void bool int uint float double half fixed long short unsigned vec2 vec3 vec4 ivec2 ivec3 ivec4 uvec2 uvec3 uvec4 bvec2
bvec3 bvec4 mat2 mat3 mat4 mat2x2 mat2x3 mat2x4 mat3x2 mat3x3 mat3x4 mat4x2 mat4x3 mat4x4 sampler2D isampler2D usampler2D
sampler3D isampler3D usampler3D samplerCube sampler2DArray sampler2DShadow samplerCubeShadow sampler2DMS image2D iimage2D
uimage2D image3D image2DArray imageCube uniform buffer shared layout in out inout flat smooth highp mediump lowp
precision const struct return if else for while do break continue discard switch case default true false main readonly
writeonly coherent volatile restrict centroid invariant precise attribute varying sample patch input output packed
common partition active asm class union enum typedef template this resource goto inline noinline public static extern
external interface superp namespace using sizeof cast filter radians degrees sin cos tan asin acos atan sinh cosh tanh
asinh acosh atanh pow exp log exp2 log2 sqrt inversesqrt abs sign floor trunc round roundEven ceil fract mod modf min
max clamp mix step smoothstep isnan isinf floatBitsToInt floatBitsToUint intBitsToFloat uintBitsToFloat packSnorm2x16
unpackSnorm2x16 packUnorm2x16 unpackUnorm2x16 packHalf2x16 unpackHalf2x16 packUnorm4x8 packSnorm4x8 unpackUnorm4x8
unpackSnorm4x8 length distance dot cross normalize faceforward reflect refract matrixCompMult outerProduct transpose
determinant inverse lessThan lessThanEqual greaterThan greaterThanEqual equal notEqual any all not uaddCarry
usubBorrow umulExtended imulExtended bitfieldExtract bitfieldInsert bitfieldReverse bitCount findLSB findMSB
textureSize texture textureProj textureLod textureOffset texelFetch texelFetchOffset textureProjOffset
textureLodOffset textureProjLod textureProjLodOffset textureGrad textureGradOffset textureProjGrad
textureProjGradOffset textureGather imageLoad imageStore imageSize imageAtomicAdd imageAtomicMin imageAtomicMax
imageAtomicAnd imageAtomicOr imageAtomicXor imageAtomicExchange imageAtomicCompSwap atomicAdd atomicMin atomicMax
atomicAnd atomicOr atomicXor atomicExchange atomicCompSwap atomicCounter barrier memoryBarrier memoryBarrierShared
memoryBarrierBuffer memoryBarrierImage groupMemoryBarrier memoryBarrierAtomicCounter dFdx dFdy fwidth length
'''.split())
SWIZZLE = re.compile(r'[xyzwrgbastpq]{1,4}$')
IDENTIFIER = re.compile(r'[A-Za-z_]\w*$')
LETTERS = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'


def new_names():
    """Identifiers by length: a letter, then letters, digits and underscores."""
    yield from LETTERS
    for first in LETTERS:
        for second in LETTERS + '0123456789_':
            yield first + second
    for first in LETTERS:
        for second in LETTERS + '0123456789_':
            for third in LETTERS + '0123456789_':
                yield first + second + third


def shorten_names(results):
    """`results` with the names the shaders define (variables, functions, structures, blocks and their members)
    renamed to the shortest names free, the most used first. The names are shared by all shaders, so the one solid
    stream finds the same text in each. A name is kept when it is GLSL's (KEPT, gl_*), a qualifier of layout(...) or a
    swizzle (xyzw, rgba, stpq): those cannot be told from a member and are never renamed, nor is a name that the text
    uses and the table does not rename taken as a new one. Preprocessor lines are left alone."""
    kept = set(KEPT)
    bodies = []
    for text, _ in results:
        lines = text.decode('ascii').split('\n')
        bodies.append(lines)
        for line in lines:
            if line.startswith('#'):
                continue
            tokens = TOKEN.findall(line)
            for position, token in enumerate(tokens):
                if token == 'layout' and tokens[position + 1:position + 2] == ['(']:
                    end = tokens.index(')', position)
                    kept.update(word for word in tokens[position + 2:end] if IDENTIFIER.match(word))
    counts = collections.Counter()
    for lines in bodies:
        for line in lines:
            if not line.startswith('#'):
                counts.update(token for token in TOKEN.findall(line)
                              if IDENTIFIER.match(token) and token not in kept and not token.startswith('gl_')
                              and not SWIZZLE.match(token))
    present = {token for lines in bodies for line in lines if not line.startswith('#')
               for token in TOKEN.findall(line) if IDENTIFIER.match(token)}
    free = (name for name in new_names() if name not in kept and name not in present - set(counts)
            and not name.startswith('gl_'))
    mapping = {}
    for token, _ in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
        mapping[token] = next(free)
    shortened = []
    for (_, rows), lines in zip(results, bodies):
        out = []
        for line in lines:
            if line.startswith('#'):
                out.append(line)
                continue
            pieces, previous = [], ''
            for token in TOKEN.findall(line):
                token = mapping.get(token, token)
                if previous and ((WORD.match(previous[-1]) and WORD.match(token[0])) or previous[-1] + token[0] in JOINS):
                    pieces.append(' ')
                pieces.append(token)
                previous = token
            out.append(''.join(pieces))
        shortened.append(('\n'.join(out).encode('ascii'), rows))
    return shortened


def validate_final(shaders, results, work):
    """glslangValidator on the text that is packed, when it is on PATH."""
    validator = shutil.which('glslangValidator')
    if not validator:
        return
    for (name, stage, _, _, _), (text, _) in zip(shaders, results):
        scratch = pathlib.Path(work) / ('%s.packed.%s' % (name, STAGE_KIND[stage]))
        scratch.write_bytes(text)
        result = subprocess.run([validator, str(scratch)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode != 0:
            raise ShaderError('glslangValidator for %s (packed) failed:\n%s' % (name, result.stdout.strip()))


def stream_order(shaders):
    """Indices of `shaders` in the order their text goes into the stream: pack order, except that a backend variant
    follows the shader it is made from (the 32 KiB window of deflate matches a variant only when it is near its base)."""
    variants = {name: base for name, base, _ in GLES_VARIANTS}
    order = []
    for index, shader in enumerate(shaders):
        if shader[0] in variants:
            continue
        order.append(index)
        order += [other for other, (name, *_) in enumerate(shaders) if variants.get(name) == shader[0]]
    return order


def generate(shaders, results):
    # One solid zlib stream: the shaders share most of their text (the uniform blocks, structs and helpers of one
    # GLSL file), which separate streams each pay for again. A shader is at `offset` of the inflated text, in pack
    # order but for the variants (stream_order), so the shaders every frame needs (the first ones) are the cheapest to
    # reach: src/gpu/gles/shader_pack.cpp inflates from the start and stops after the shader it wants.
    solid, offsets, shared = bytearray(), {}, {}
    for index in stream_order(shaders):
        text = results[index][0]
        if text not in shared:
            shared[text] = len(solid)
            solid += text
        offsets[index] = shared[text]
    entries, bindings = [], []
    for index, ((name, stage, _, _, _), (text, rows)) in enumerate(zip(shaders, results)):
        entries.append((offsets[index], len(text), len(bindings), len(rows), STAGES[stage], name))
        bindings += rows
    blob = CACHE.stream(bytes(solid), optimal_deflate.zlib_best)
    if zlib.decompress(blob) != solid:
        raise SystemExit('error: the packed stream does not inflate to the shaders')
    lines = ['// Generated by tools/build_gles_shader_pack.py; do not edit.',
             'constexpr u32 kGlesShaderMaximumBytes=%d;' % max(entry[1] for entry in entries),
             'constexpr u32 kGlesShaderPackBytes=%d;' % len(solid),
             'constexpr GlesPackedShader kGlesShaders[]={']
    lines += ['{%d,%d,%d,%d,%d,0}, // %s' % entry for entry in entries]
    lines.append('};')
    lines.append('constexpr GlesBinding kGlesBindings[]={')
    lines += ['{%d,%d,%d,%d},' % row for row in bindings] or ['{0,0,0,0}']
    lines.append('};')
    for index, entry in enumerate(entries):
        lines.append('constexpr u32 kGlesShaderIndex_%s=%d;' % (entry[5], index))
    lines.append(embed_arrays([('alignas(16) constexpr u8 kGlesShaderPacked[]', PACKED_FILE)]).rstrip('\n'))
    return '\n'.join(lines) + '\n', bytes(blob)


def main():
    arguments = sys.argv[1:]
    validate = '--validate' in arguments
    dump = None
    if '--dump' in arguments:
        at = arguments.index('--dump')
        dump = pathlib.Path(arguments[at + 1])
        del arguments[at:at + 2]
    arguments = [argument for argument in arguments if not argument.startswith('--')]
    if len(arguments) != 1:
        raise SystemExit(__doc__)
    output = pathlib.Path(arguments[0])
    shaders = shader_list()
    try:
        with tempfile.TemporaryDirectory() as scratch:
            results = shorten_names([assemble(shader, validate, scratch) for shader in shaders])
            if validate:
                validate_final(shaders, results, scratch)
    except ShaderError as error:
        raise SystemExit('error: %s' % error)
    if dump:
        dump.mkdir(parents=True, exist_ok=True)
        for shader, (text, _) in zip(shaders, results):
            (dump / ('%s.%s.glsl' % (shader[0], shader[1]))).write_bytes(text)
    text, packed = generate(shaders, results)
    CACHE.save()
    output.parent.mkdir(parents=True, exist_ok=True)
    write_if_changed(output.with_name(PACKED_FILE), packed)
    if not output.exists() or output.read_text() != text:
        output.write_text(text, newline='\n')
    print('GLSL shader pack: %d shaders, %d bytes packed' % (len(shaders), len(packed)))


if __name__ == '__main__':
    main()
