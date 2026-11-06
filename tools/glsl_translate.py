"""HLSL to GLSL ES 3.10 with the pinned toolchain (DXC to SPIR-V, then tools/shadertool/mc_shadertool) for the effect
packages (tools/maicord-plugin/mcfx_tool.py, section GLSL of .mcfx interface 4). The client's own shaders are
hand-written GLSL (src/render/shaders/gles, tools/build_gles_shader_pack.py) and do not come through here.

The translation conventions are those the hand-written GLSL follows too (design: DXC with the register shifts below,
vertex output flipped and clip space fixed up and bindings assigned by mc_shadertool). Stdlib only.
"""
import pathlib
import shutil
import struct
import subprocess
import zlib

STAGES = {'vs': 0, 'ps': 1, 'cs': 2}
STAGE_KIND = {'vs': 'vert', 'ps': 'frag', 'cs': 'comp'}
# glsl_shader.hpp: the payload of one shader.
ENTRY_STAGE = {'ps': 1, 'vs': 2}
PACKED_MAXIMUM = 64 * 1024
RAW_MAXIMUM = 256 * 1024
BINDING_MAXIMUM = 64


class GlslError(Exception):
    pass


def run(command, what):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        raise GlslError('%s failed:\n%s' % (what, result.stdout.strip()))
    return result.stdout


def finish_glsl(text, name):
    lines = [line.strip() for line in text.splitlines()]
    text = '\n'.join(line for line in lines if line) + '\n'
    if not text.startswith('#version 310 es\n'):
        raise GlslError('%s: not GLSL ES 3.10' % name)
    return text


def translate_hlsl(tools, source, entry, stage, defines, work, name, include_directories=(), validate=False):
    """(GLSL text as ASCII bytes, binding rows [(kind, slot, binding, sampler)]) of one entry point of `source`.

    `stage` is 'vs', 'ps' or 'cs'; `tools` is (dxc, mc_shadertool)."""
    dxc, mc_shadertool = tools
    spirv = pathlib.Path(work) / (name + '.spv')
    command = [str(dxc), '-spirv', '-T', stage + '_6_0', '-E', entry, '-fspv-target-env=vulkan1.1',
               '-fvk-t-shift', '32', '0', '-fvk-s-shift', '64', '0', '-fvk-u-shift', '96', '0',
               '-O3', '-HV', '2016']
    for directory in include_directories:
        command += ['-I', str(directory)]
    for define in defines:
        command += ['-D', define]
    run(command + [str(source), '-Fo', str(spirv)], 'dxc for ' + name)
    glsl, manifest = pathlib.Path(work) / (name + '.glsl'), pathlib.Path(work) / (name + '.manifest')
    run([str(mc_shadertool), str(spirv), stage, str(glsl), str(manifest)], 'mc_shadertool for ' + name)
    text = finish_glsl(glsl.read_text(), name)
    rows = [tuple(int(value) for value in line.split()) for line in manifest.read_text().splitlines()]
    if validate:
        validator = shutil.which('glslangValidator')
        if validator:
            minified = pathlib.Path(work) / (name + '.min.' + STAGE_KIND[stage])
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


def payload_entry(text, stage, rows, packed=None):
    """One shader as include/maicord/gpu/glsl_shader.hpp lays it out (`stage` 'ps' or 'vs')."""
    packed = packed if packed is not None else zlib_best(text)
    if len(rows) > BINDING_MAXIMUM or len(text) > RAW_MAXIMUM or len(packed) > PACKED_MAXIMUM:
        raise GlslError('a translated shader is over the payload limits (%d bindings, %d bytes of GLSL, %d packed)'
                        % (len(rows), len(text), len(packed)))
    body = b''.join(struct.pack('<4B', *row) for row in rows)
    header = struct.pack('<IBBHII', 16 + len(body) + len(packed), ENTRY_STAGE[stage], len(rows), 0, len(text),
                         len(packed))
    return header + body + packed
