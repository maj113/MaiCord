#!/usr/bin/env python3
"""Renews the GPU golden PNGs of tests/gpu/golden from Direct3D 11 renders of the shared scenes.

Each scene has a Windows test (tests/gpu/ui_quads_windows.cpp, tests/gpu/glyphs_windows.cpp) that renders it and, with
its capture variable set, writes raw bytes instead of comparing. This script takes those files (--captures), or runs a
test executable that holds the tests (--exe, one built by tools/build.ps1 -KeepIntermediates -Test -TestNames
maicord_gpu_ui_quads,maicord_gpu_glyphs) to make them, and writes the PNGs: 8-bit grayscale or RGBA, filters picked per
row, the smallest of the zlib strategies. The OpenGL ES tests on Linux compare against these files within the
tolerances in the scene headers, so run it only when a shader or a scene changed on purpose, and commit the PNGs with
that change. Stdlib only.

  update_gpu_goldens.py --exe MaiCordGpuUnitTests.exe [--scene glyphs]
  update_gpu_goldens.py --captures DIR [--scene ui_quads]   (DIR holds ui_quads, glyphs.atlas, glyphs.draw)
"""
import argparse
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
GOLDEN = ROOT / 'tests' / 'gpu' / 'golden'

# scene -> (entry point, capture variable, [(capture suffix, PNG name, width, height, channels)])
SCENES = {
    # kWidth and kHeight of tests/gpu/ui_quads_scene.hpp
    'ui_quads': ('maicord_gpu_ui_quads_windows_entry', 'MAICORD_GPU_QUAD_CAPTURE',
                 [('', 'ui_quads.png', 192, 128, 4)]),
    # kAtlasWidth/Height and kFrameWidth/Height of tests/gpu/glyph_scene.hpp
    'glyphs': ('maicord_gpu_glyphs_windows_entry', 'MAICORD_GPU_GLYPH_CAPTURE',
               [('.atlas', 'glyph_atlas.png', 64, 128, 1), ('.draw', 'glyph_draw.png', 160, 112, 4)]),
    # kAtlasWidth/Height and kFrameWidth/Height of tests/gpu/media_scene.hpp
    'media': ('maicord_gpu_media_windows_entry', 'MAICORD_GPU_MEDIA_CAPTURE',
              [('.atlas', 'media_atlas.png', 96, 64, 4), ('.draw', 'media_draw.png', 160, 112, 4)]),
}

# The effects of tests/gpu/effects.cpp: (name, frames) in its table's order; every capture is <prefix>.<name>.<frame>
# and the composite <prefix>.composite, 192 x 108 RGBA.
EFFECT_FRAMES = [('ascii-rain', [11]), ('aurora', [11]), ('bokeh', [11]), ('day-night', [11]), ('film-grain', [11]),
                 ('plasma', [11]), ('spark-ribbon', [1, 11, 23]), ('silk-waves', [1, 11, 23]), ('starfield', [11]),
                 ('wave-lines', [11])]
SCENES['effects'] = ('maicord_gpu_effects_entry', 'MAICORD_GPU_EFFECT_CAPTURE',
                     [('.%s.%d' % (name, frame), 'effects/effect_%s_%d.png' % (name, frame), 192, 108, 4)
                      for name, frames in EFFECT_FRAMES for frame in frames] +
                     [('.composite', 'effects/effect_composite.png', 192, 108, 4)])


def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))


def paeth(left, above, corner):
    estimate = left + above - corner
    dl, da, dc = abs(estimate - left), abs(estimate - above), abs(estimate - corner)
    return left if dl <= da and dl <= dc else (above if da <= dc else corner)


def filtered_rows(pixels, width, height, channels):
    stride = width * channels
    previous = bytes(stride)
    for y in range(height):
        row = pixels[y * stride:(y + 1) * stride]
        candidates = {0: row}
        candidates[1] = bytes((row[i] - (row[i - channels] if i >= channels else 0)) & 255 for i in range(stride))
        candidates[2] = bytes((row[i] - previous[i]) & 255 for i in range(stride))
        candidates[3] = bytes((row[i] - (((row[i - channels] if i >= channels else 0) + previous[i]) >> 1)) & 255
                              for i in range(stride))
        candidates[4] = bytes((row[i] - paeth(row[i - channels] if i >= channels else 0, previous[i],
                                              previous[i - channels] if i >= channels else 0)) & 255
                              for i in range(stride))
        best = min(candidates, key=lambda f: sum(min(b, 256 - b) for b in candidates[f]))
        yield bytes([best]) + candidates[best]
        previous = row


def encode(pixels, width, height, channels):
    raw = b''.join(filtered_rows(pixels, width, height, channels))
    best = None
    for memory in (8, 9):
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED):
            compressor = zlib.compressobj(9, zlib.DEFLATED, 15, memory, strategy)
            stream = compressor.compress(raw) + compressor.flush()
            if best is None or len(stream) < len(best):
                best = stream
    color_type = 6 if channels == 4 else 0
    header = struct.pack('>IIBBBBB', width, height, 8, color_type, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', best) + chunk(b'IEND', b'')


def write_scene(name, base):
    for suffix, png_name, width, height, channels in SCENES[name][2]:
        capture = pathlib.Path(str(base) + suffix)
        if not capture.exists():
            sys.exit('error: %s is missing' % capture)
        pixels = capture.read_bytes()
        expected = width * height * channels
        if len(pixels) != expected:
            sys.exit('error: %s: expected %d bytes, got %d' % (capture, expected, len(pixels)))
        png = encode(pixels, width, height, channels)
        output = GOLDEN / png_name
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(png)
        print('wrote %s (%d bytes)' % (output.relative_to(ROOT), len(png)))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--captures', help='directory with the raw captures the Windows tests wrote')
    source.add_argument('--exe', help='test executable holding the Windows entries')
    parser.add_argument('--scene', choices=sorted(SCENES), action='append', help='default: every scene')
    arguments = parser.parse_args()
    scenes = arguments.scene or sorted(SCENES)
    with tempfile.TemporaryDirectory() as scratch:
        directory = pathlib.Path(arguments.captures) if arguments.captures else pathlib.Path(scratch)
        for name in scenes:
            base = directory / name
            if arguments.exe:
                entry, variable, _ = SCENES[name]
                environment = dict(os.environ, MAICORD_TEST_ENTRY=entry, **{variable: str(base)})
                result = subprocess.run([arguments.exe], env=environment)
                if result.returncode != 0:
                    sys.exit('error: the capture run of %s failed' % name)
            write_scene(name, base)


if __name__ == '__main__':
    main()
