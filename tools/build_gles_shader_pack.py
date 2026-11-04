#!/usr/bin/env python3
"""Builds the GLSL ES shader pack of the Linux GPU backend from the HLSL the Windows build compiles with fxc.

The shader list is the maicord_shader(... PACK <name> ...) calls of cmake/generated_sources.cmake, in call order,
which is the order of ShaderId (src/gpu/gles/shader_pack.cpp checks it). Per shader:

  HLSL --dxc -spirv (pinned, tools/linux_shader_toolchain.json)--> SPIR-V
       --mc_shadertool (pinned SPIRV-Cross)--> GLSL ES 3.10 + binding manifest
       --text pass--> minified GLSL --deflate--> pack

The text pass strips indentation and blank lines (the unroll and branch hints SPIRV-Cross emits are guarded by
GL_EXT_control_flow_attributes being defined, so a driver without it just ignores them). Streams come from
tools/optimal_deflate.py and tools/deflate_cache.txt like every other shipped table (namespace gles_shaders;
MAICORD_UPDATE_DEFLATE_CACHE=1 searches the misses, then commit the cache).

Usage: build_gles_shader_pack.py OUTPUT.inc [--validate]
Writes OUTPUT.inc and maicord_shader_pack_gles.bin beside it (read by #embed). With --validate each output also
goes through glslangValidator and each SPIR-V through spirv-val when they are on PATH.
"""
import concurrent.futures
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

import fetch_linux_shader_toolchain
import optimal_deflate
from font_reference.embed_fixture import embed_arrays, write_if_changed

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHADER_LIST = ROOT / 'cmake' / 'generated_sources.cmake'
PACKED_FILE = 'maicord_shader_pack_gles.bin'
STAGES = {'vs': 0, 'ps': 1, 'cs': 2}
STAGE_KIND = {'vs': 'vert', 'ps': 'frag', 'cs': 'comp'}
CACHE = optimal_deflate.DeflateCache('gles_shaders')


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
    names = [shader[0] for shader in shaders]
    if not shaders or len(set(names)) != len(names):
        raise SystemExit('error: cannot read the packed shaders of cmake/generated_sources.cmake')
    return shaders


def run(command, what):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        raise SystemExit('error: %s failed' % what)
    return result.stdout


def finish_glsl(text, name):
    lines = [line.strip() for line in text.splitlines()]
    text = '\n'.join(line for line in lines if line) + '\n'
    if not text.startswith('#version 310 es\n'):
        raise SystemExit('error: %s: not GLSL ES 3.10' % name)
    return text


def translate(shader, tools, work, validate):
    name, stage, entry, source, defines = shader
    dxc, mc_shadertool = tools
    spirv = work / (name + '.spv')
    command = [str(dxc), '-spirv', '-T', stage + '_6_0', '-E', entry, '-fspv-target-env=vulkan1.1',
               '-fvk-t-shift', '32', '0', '-fvk-s-shift', '64', '0', '-fvk-u-shift', '96', '0',
               '-O3', '-HV', '2016']
    for define in defines:
        command += ['-D', define]
    run(command + [str(ROOT / source), '-Fo', str(spirv)], 'dxc for ' + name)
    glsl, manifest = work / (name + '.glsl'), work / (name + '.manifest')
    run([str(mc_shadertool), str(spirv), stage, str(glsl), str(manifest)], 'mc_shadertool for ' + name)
    text = finish_glsl(glsl.read_text(), name)
    rows = [tuple(int(value) for value in line.split()) for line in manifest.read_text().splitlines()]
    if validate:
        validator = shutil.which('glslangValidator')
        if validator:
            minified = work / (name + '.min.' + STAGE_KIND[stage])
            minified.write_text(text, newline='\n')
            run([validator, str(minified)], 'glslangValidator for ' + name)
        spirv_val = shutil.which('spirv-val')
        if spirv_val:
            run([spirv_val, '--target-env', 'vulkan1.1', str(spirv)], 'spirv-val for ' + name)
    return text.encode('ascii'), rows


def zlib_best(data):
    best = None
    for mem in (8, 9):
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED):
            compressor = zlib.compressobj(9, zlib.DEFLATED, 15, mem, strategy)
            out = compressor.compress(data) + compressor.flush()
            if best is None or len(out) < len(best):
                best = out
    assert zlib.decompress(best) == data
    return best


def generate(shaders, results):
    blob, entries, bindings, shared = bytearray(), [], [], {}
    for (name, stage, _, _, _), (text, rows) in zip(shaders, results):
        if text in shared:
            offset, packed = shared[text]
        else:
            packed = CACHE.stream(text, zlib_best)
            if zlib.decompress(packed) != text:
                raise SystemExit('error: %s: the packed stream does not inflate to the shader' % name)
            offset = len(blob)
            blob += packed
            shared[text] = (offset, packed)
        entries.append((offset, len(packed), len(text), len(bindings), len(rows), STAGES[stage], name))
        bindings += rows
    lines = ['// Generated by tools/build_gles_shader_pack.py; do not edit.',
             'constexpr u32 kGlesShaderMaximumBytes=%d;' % max(entry[2] for entry in entries),
             'constexpr GlesPackedShader kGlesShaders[]={']
    lines += ['{%d,%d,%d,%d,%d,%d,0}, // %s' % entry for entry in entries]
    lines.append('};')
    lines.append('constexpr GlesBinding kGlesBindings[]={')
    lines += ['{%d,%d,%d,%d},' % row for row in bindings] or ['{0,0,0,0}']
    lines.append('};')
    for index, entry in enumerate(entries):
        lines.append('constexpr u32 kGlesShaderIndex_%s=%d;' % (entry[6], index))
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
