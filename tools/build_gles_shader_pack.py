#!/usr/bin/env python3
"""Builds the GLSL ES shader pack of the Linux GPU backend from the HLSL the Windows build compiles with fxc.

The shader list is the maicord_shader(... PACK <name> ...) calls of cmake/generated_sources.cmake, in call order,
which is the order of ShaderId (src/gpu/gles/shader_pack.cpp checks it). Per shader:

  HLSL --dxc -spirv (pinned, tools/linux_shader_toolchain.json)--> SPIR-V
       --mc_shadertool (pinned SPIRV-Cross)--> GLSL ES 3.10 + binding manifest
       --text pass--> minified GLSL --one solid deflate stream for all shaders--> pack

The text pass strips indentation and blank lines (the unroll and branch hints SPIRV-Cross emits are guarded by
GL_EXT_control_flow_attributes being defined, so a driver without it just ignores them). The stream comes from
tools/optimal_deflate.py and tools/deflate_cache.txt like every other shipped table (namespace gles_shaders;
MAICORD_UPDATE_DEFLATE_CACHE=1 searches the miss, then commit the cache).

Usage: build_gles_shader_pack.py OUTPUT.inc [--validate]
Writes OUTPUT.inc and maicord_shader_pack_gles.bin beside it (read by #embed). With --validate each output also
goes through glslangValidator and each SPIR-V through spirv-val when they are on PATH.
"""
import concurrent.futures
import pathlib
import re
import sys
import tempfile
import zlib

import fetch_linux_shader_toolchain
import glsl_translate
import optimal_deflate
from font_reference.embed_fixture import embed_arrays, write_if_changed

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHADER_LIST = ROOT / 'cmake' / 'generated_sources.cmake'
PACKED_FILE = 'maicord_shader_pack_gles.bin'
STAGES = {'vs': 0, 'ps': 1, 'cs': 2}
CACHE = optimal_deflate.DeflateCache('gles_shaders')
GLES_VARIANTS = [('colr_layers_narrow_compute', 'colr_layers_compute', ['COLR_SLICES=2']),
                 # Without dual-source blending the composite is drawn twice (gles/draw.cpp): once for its weight and
                 # once for its colour, each the only output of a plain pixel shader.
                 ('fx_composite_weight_pixel', 'fx_composite_pixel', ['MC_COMPOSITE_WEIGHT=1']),
                 ('fx_composite_color_pixel', 'fx_composite_pixel', ['MC_COMPOSITE_COLOR=1'])]


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


def translate(shader, tools, work, validate):
    name, stage, entry, source, defines = shader
    try:
        return glsl_translate.translate_hlsl(tools, ROOT / source, entry, stage, defines, work, name,
                                             validate=validate)
    except glsl_translate.GlslError as error:
        raise SystemExit('error: %s' % error)


zlib_best = glsl_translate.zlib_best


def generate(shaders, results):
    # One solid zlib stream: the shaders share most of their text (the SPIRV-Cross preamble, the uniform blocks and
    # structs of one HLSL source, the same helper code), which separate streams each pay for again. A shader is at
    # `offset` of the inflated text, in pack order, so the shaders every frame needs (the first ones) are the cheapest
    # to reach: src/gpu/gles/shader_pack.cpp inflates from the start and stops after the shader it wants.
    solid, entries, bindings, shared = bytearray(), [], [], {}
    for (name, stage, _, _, _), (text, rows) in zip(shaders, results):
        if text not in shared:
            shared[text] = len(solid)
            solid += text
        entries.append((shared[text], len(text), len(bindings), len(rows), STAGES[stage], name))
        bindings += rows
    blob = CACHE.stream(bytes(solid), zlib_best)
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
    arguments = [argument for argument in sys.argv[1:] if not argument.startswith('--')]
    validate = '--validate' in sys.argv[1:]
    if len(arguments) != 1:
        raise SystemExit(__doc__)
    output = pathlib.Path(arguments[0])
    shaders = shader_list()
    tools = fetch_linux_shader_toolchain.ensure_toolchain()
    with tempfile.TemporaryDirectory() as scratch:
        work = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda shader: translate(shader, tools, work, validate), shaders))
    text, packed = generate(shaders, results)
    CACHE.save()
    output.parent.mkdir(parents=True, exist_ok=True)
    write_if_changed(output.with_name(PACKED_FILE), packed)
    if not output.exists() or output.read_text() != text:
        output.write_text(text, newline='\n')
    print('GLSL shader pack: %d shaders, %d bytes packed' % (len(shaders), len(packed)))


if __name__ == '__main__':
    main()
