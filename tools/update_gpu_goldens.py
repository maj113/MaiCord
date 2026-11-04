#!/usr/bin/env python3
"""Renews tests/gpu/golden/ui_quads.png from a Direct3D 11 render of the ui_quads scene.

The Windows test (tests/gpu/ui_quads_windows.cpp) renders the scene and, with MAICORD_GPU_QUAD_CAPTURE=<file> set,
writes it as raw RGBA bytes instead of comparing. This script takes that file (--raw), or runs a test executable
that holds the test (--exe, one built by tools/build.ps1 -KeepIntermediates -Test -TestNames maicord_gpu_ui_quads)
to make it, and writes the PNG: 8-bit RGBA, filters picked per row, the smallest of the zlib strategies. The
OpenGL ES test on Linux compares against this file within the tolerance in tests/gpu/ui_quads_scene.hpp, so run it
only when a shader or the scene changed on purpose, and commit the PNG with that change. Stdlib only.

  update_gpu_goldens.py --raw capture.rgba
  update_gpu_goldens.py --exe MaiCordGpuUnitTests.exe
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
OUTPUT = ROOT / 'tests' / 'gpu' / 'golden' / 'ui_quads.png'
WIDTH, HEIGHT = 192, 128  # kWidth and kHeight of tests/gpu/ui_quads_scene.hpp
ENTRY = 'maicord_gpu_ui_quads_windows_entry'


def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))


def paeth(left, above, corner):
    estimate = left + above - corner
    dl, da, dc = abs(estimate - left), abs(estimate - above), abs(estimate - corner)
    return left if dl <= da and dl <= dc else (above if da <= dc else corner)


def filtered_rows(pixels):
    stride = WIDTH * 4
    previous = bytes(stride)
    for y in range(HEIGHT):
        row = pixels[y * stride:(y + 1) * stride]
        candidates = {0: row}
        candidates[1] = bytes((row[i] - (row[i - 4] if i >= 4 else 0)) & 255 for i in range(stride))
        candidates[2] = bytes((row[i] - previous[i]) & 255 for i in range(stride))
        candidates[3] = bytes((row[i] - (((row[i - 4] if i >= 4 else 0) + previous[i]) >> 1)) & 255
                              for i in range(stride))
        candidates[4] = bytes((row[i] - paeth(row[i - 4] if i >= 4 else 0, previous[i],
                                              previous[i - 4] if i >= 4 else 0)) & 255 for i in range(stride))
        best = min(candidates, key=lambda f: sum(min(b, 256 - b) for b in candidates[f]))
        yield bytes([best]) + candidates[best]
        previous = row


def encode(pixels):
    raw = b''.join(filtered_rows(pixels))
    best = None
    for memory in (8, 9):
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED):
            compressor = zlib.compressobj(9, zlib.DEFLATED, 15, memory, strategy)
            stream = compressor.compress(raw) + compressor.flush()
            if best is None or len(stream) < len(best):
                best = stream
    header = struct.pack('>IIBBBBB', WIDTH, HEIGHT, 8, 6, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', best) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--raw', help='raw RGBA capture written by the Windows test')
    source.add_argument('--exe', help='test executable holding maicord_gpu_ui_quads_windows_entry')
    arguments = parser.parse_args()
    if arguments.exe:
        with tempfile.TemporaryDirectory() as scratch:
            capture = pathlib.Path(scratch) / 'ui_quads.rgba'
            environment = dict(os.environ, MAICORD_TEST_ENTRY=ENTRY, MAICORD_GPU_QUAD_CAPTURE=str(capture))
            result = subprocess.run([arguments.exe], env=environment)
            if result.returncode != 0 or not capture.exists():
                sys.exit('error: the capture run failed')
            pixels = capture.read_bytes()
    else:
        pixels = pathlib.Path(arguments.raw).read_bytes()
    if len(pixels) != WIDTH * HEIGHT * 4:
        sys.exit('error: expected %d bytes of RGBA, got %d' % (WIDTH * HEIGHT * 4, len(pixels)))
    png = encode(pixels)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(png)
    print('wrote %s (%d bytes)' % (OUTPUT.relative_to(ROOT), len(png)))


if __name__ == '__main__':
    main()
