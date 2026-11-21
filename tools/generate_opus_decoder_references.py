#!/usr/bin/env python3
"""Writes tests/fixtures/opus_decoder_vectors/references.bin: libopus's decode of the Opus decoder coverage streams.

maicord_opus_decoder_vectors (tests/media/audio/opus_decoder_vectors.cpp) compares MaiCord's decode of each stream in
tests/fixtures/opus_decoder_vectors.inc against it, where the stream's digest there is 0. A nonzero digest is the PCM
that MaiCord must produce bit for bit: this script checks that libopus produces it too.

The reference decoder is libopus 1.6.1 (fixed point), the fork that MaiCord used before its own codec: third_party/opus
at commit a7d4c64f4 (the parent of fbe843e32). tools/opus_reference_decode.c drives it as the test drives MaiCord's
decoder. The script takes the fork from git, builds both with a hosted C compiler and decodes each stream to mono and
to stereo. Of each output it keeps every fourth segment of 480 samples per channel (only whole segments, from the
first), which the test reads in the same order: streams in table order, mono before stereo. The file is the zlib
stream (level 9) of those samples.

Usage, from the repository root:
    python tools/generate_opus_decoder_references.py [--cc COMPILER]
COMPILER is a hosted C compiler with the GNU or the MSVC command line (default: clang, else clang-cl). On Windows,
run it where that compiler links programs against the C runtime (a Visual Studio developer prompt).
"""

import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zlib

FORK_COMMIT = 'a7d4c64f4'
SEGMENT = 480
STRIDE = 4
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INC = os.path.join(ROOT, 'tests', 'fixtures', 'opus_decoder_vectors.inc')
OUT = os.path.join(ROOT, 'tests', 'fixtures', 'opus_decoder_vectors', 'references.bin')


def vectors():
    """(name, stream bytes, mono digest, stereo digest) in table order."""
    text = open(INC, encoding='utf-8').read()
    data = {}
    for m in re.finditer(r'inline constexpr u8 (kOpusDecoderVector\d+)\[\]\{(.*?)\};', text, re.S):
        body = m.group(2)
        embed = re.search(r'#embed "([^"]+)"', body)
        if embed:
            data[m.group(1)] = open(os.path.join(os.path.dirname(INC), embed.group(1)), 'rb').read()
        else:
            data[m.group(1)] = bytes(int(x) for x in re.findall(r'\d+', body))
    rows = re.findall(r'\{"(\w+)", (kOpusDecoderVector\d+), \d+, \d+, \d+, \d+, \{(0x[0-9a-f]+|0)(?:ull)?, '
                      r'(0x[0-9a-f]+|0)(?:ull)?\}\}', text)
    return [(name, data[symbol], int(mono, 16), int(stereo, 16)) for name, symbol, mono, stereo in rows]


def fnv(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def build(cc, work):
    archive = subprocess.run(['git', '-C', ROOT, 'archive', FORK_COMMIT, 'third_party/opus'], check=True,
                             capture_output=True).stdout
    tarfile.open(fileobj=io.BytesIO(archive)).extractall(work)
    fork = os.path.join(work, 'third_party', 'opus')
    cmake = open(os.path.join(fork, 'CMakeLists.txt'), encoding='utf-8').read()
    sources = re.search(r'add_library\(\$\{MAIOPUS_DECODER_TARGET\} STATIC(.*?)\)', cmake, re.S).group(1).split()
    sources = [os.path.join(fork, s) for s in sources if s.endswith('.c')]
    sources += [os.path.join(fork, 'celt', 'x86', s) for s in ('celt_lpc_sse4_1.c', 'pitch_sse4_1.c')]
    sources.append(os.path.join(ROOT, 'tools', 'opus_reference_decode.c'))
    defines = ['OPUS_BUILD', 'FIXED_POINT', 'DISABLE_FLOAT_API', 'USE_ALLOCA', 'CUSTOM_SUPPORT',
               'PACKAGE_VERSION="1.6.1"']
    includes = [os.path.join(fork, d) for d in ('include', '', 'celt', 'silk', 'src')]
    exe = os.path.join(work, 'opus_reference_decode.exe')
    if os.path.basename(cc).lower().startswith('clang-cl') or os.path.basename(cc).lower() == 'cl.exe':
        command = [cc, '/nologo', '/O2', '/arch:AVX2', '/W0', '/Fe' + exe, '/Fo' + work + os.sep]
        command += ['/D' + d for d in defines] + ['/I' + i for i in includes]
    else:
        command = [cc, '-O2', '-mavx2', '-w', '-o', exe] + ['-D' + d for d in defines] + ['-I' + i for i in includes]
    subprocess.run(command + sources, check=True, cwd=work)
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cc', default=shutil.which('clang') or shutil.which('clang-cl'))
    args = parser.parse_args()
    work = tempfile.mkdtemp(prefix='opus-references-')
    try:
        exe = build(args.cc, work)
        out = bytearray()
        for name, stream, *digests in vectors():
            source = os.path.join(work, name + '.bit')
            open(source, 'wb').write(stream)
            for channels in (1, 2):
                target = os.path.join(work, f'{name}_{channels}.pcm')
                subprocess.run([exe, str(channels), source, target], check=True)
                pcm = open(target, 'rb').read()
                digest = digests[channels - 1]
                if digest:
                    if fnv(pcm) != digest:
                        sys.exit(f'{name} ({channels} ch): libopus does not produce the pinned PCM')
                    continue
                segment = SEGMENT * channels * 2
                for k, at in enumerate(range(0, len(pcm) - segment + 1, segment)):
                    if k % STRIDE == 0:
                        out += pcm[at:at + segment]
        open(OUT, 'wb').write(zlib.compress(bytes(out), 9))
        print(f'{OUT}: {len(out)} bytes of PCM16 in {os.path.getsize(OUT)}')
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
