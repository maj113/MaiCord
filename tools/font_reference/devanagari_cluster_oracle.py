"""Fast candidate Devanagari cluster comparisons against a frozen Windows observer.

The observer is explicitly seeded with Devanagari script analysis; this tests a
single-script frontend, not general Unicode itemization or final app rendering.
No fonts are installed and no external Python packages are required.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import threading
import time


def corpus():
    cases = {}

    def add(values, group):
        if isinstance(values, str):
            values = tuple(map(ord, values))
        else:
            values = tuple(values)
        if values:
            cases.setdefault(values, group)

    alphabet = [0x915, 0x930, 0x905, 0x93c, 0x94d, 0x200c, 0x200d, 0x93f,
                0x945, 0x941, 0x940, 0x901, 0x903, 0x951, 0x952, 0x1cd0, 0xa8e0]
    for base in (0x915, 0x930, 0x905):
        for values in itertools.product(alphabet, repeat=3):
            add((base, *values), 'base-triples')
    for values in itertools.product((0x940, 0x941, 0x93c, 0x94d, 0x901, 0x951, 0x200c, 0x200d), repeat=4):
        add((0x915, 0x93f, *values), 'long-mark-tails')
    for first in (0x915, 0x930, 0x905, 0xa0, 0x25cc, 0x93f, 0x94d, 0x901, 0x951):
        for pair in itertools.product(alphabet, repeat=2):
            add((first, *pair), 'starts-and-pairs')
    for scalar in itertools.chain(range(0x900, 0x980), range(0x1cd0, 0x1d00), range(0xa8e0, 0xa900)):
        for values in ((scalar,), (0x915, scalar), (scalar, 0x93c), (scalar, 0x94d, 0x915),
                       (0x915, scalar, 0x940), (0x915, scalar, 0x951), (0x915, scalar, scalar),
                       (0x915, 0x93f, scalar), (0x915, scalar, 0x200d)):
            add(values, 'scalar-coverage')
    vowels = [*range(0x904, 0x915), 0x960, 0x961, *range(0x972, 0x978)]
    matras = [c for c in range(0x93a, 0x94d) if c not in (0x93c, 0x93d)] + [0x94e, 0x94f, 0x955, 0x956, 0x957, 0x962, 0x963, 0xa8ff]
    for vowel in vowels:
        for matra in matras:
            for middle in ((), (0x93c,), (0x200c,), (0x200d,)):
                add((vowel, *middle, matra), 'vowel-matra-context')
    words = ('नमस्ते हिन्दी संस्कृत मराठी क्षत्रिय प्रार्थना श्री श्रद्धा राष्ट्र कर्तृत्व '
             'विद्यार्थी ज्ञान दृष्टि स्त्री क्रान्ति सूर्य पृथ्वी कृपया कम्प्यूटर दिल्ली '
             'हूँ दुःख क़िला ख़ुशी फ़ोन ज़रूरी ऋषि ॐ र्‍य क्‌ष क्‍ष').split()
    for word in words:
        add(word, 'words')
        for end in range(1, len(word) + 1):
            add(word[:end], 'partial-input')
    # The frozen Windows observer accepts at most 256 UTF-16 units per case.
    add([0x915, 0x94d] * 126 + [0x915, 0x93f], 'long-conjunct')
    for base in (0x915, 0x930, 0x905, 0x25cc):
        for link in ((0x94d,), (0x93c, 0x94d), (0x200d,), (0x94d, 0x200d)):
            add([base, *link] * 16 + [0x915, 0x93f], 'long-conjunct')
    for tail in (0x940, 0x903, 0x200c):
        add([0x915] + [tail] * 100, 'long-repetition')
    for prefix in ((0x915, 0x93c), (0x915, 0x93c, 0x93c), (0x930, 0x94d, 0x915),
                   (0x915, 0x94d)*4 + (0x915,), (0x905,)):
        for tail in (0x940, 0x903):
            add([*prefix] + [tail]*40, 'long-repetition')
    rng = random.Random(20260919)
    for _ in range(2048):
        add([rng.choice(alphabet) for _ in range(rng.randint(1, 24))], 'random-combinations')
    return [(values, group) for values, group in cases.items()]


def run(args, output):
    before = time.perf_counter()
    cases = corpus()
    native_input = bytearray(b'MDVCL1\0\0' + struct.pack('<I', len(cases)))
    for scalars, _ in cases:
        native_input += struct.pack('<I', len(scalars)) + struct.pack('<' + 'I' * len(scalars), *scalars)
    observations, reference_errors = [], {}
    while len(observations) < len(cases):
        remaining = cases[len(observations):]
        windows_input = bytearray(b'MCSHPB1\0' + struct.pack('<I', len(remaining)))
        for scalars, _ in remaining:
            windows_input += struct.pack('<II', len(scalars), int(args.independent)) + struct.pack('<' + 'H' * len(scalars), *scalars)
        windows = subprocess.run([str(args.reference), str(args.font), '915', 'default',
            '1148846080', '0', '0', 'en-US'], input=windows_input, capture_output=True, timeout=5,
            env=dict(os.environ, MAICORD_SHAPE_BATCH='1'), creationflags=subprocess.CREATE_NO_WINDOW)
        if windows.stdout[:12] != windows_input[:12]:
            raise RuntimeError('Invalid Windows batch header')
        cursor = 12
        for scalars, _ in remaining:
            if cursor == len(windows.stdout):
                break
            glyph_count = struct.unpack_from('<I', windows.stdout, cursor)[0]
            end = cursor + 4 + glyph_count*14 + len(scalars)*2
            if end > len(windows.stdout):
                raise RuntimeError('Truncated Windows batch record')
            observations.append(struct.unpack_from('<'+'H'*len(scalars), windows.stdout, cursor+4+glyph_count*2))
            cursor = end
        if not windows.returncode:
            if len(observations) != len(cases):
                raise RuntimeError('Missing successful Windows batch records')
            break
        # Analyzer failures stay unavailable evidence: probe the failing record
        # alone for its HRESULT, then continue the batch. Cases are never dropped
        # or counted as exact matches.
        if len(observations) == len(cases):
            raise RuntimeError('Windows failed after its last complete record')
        scalars, _ = cases[len(observations)]
        single = subprocess.run([str(args.reference), str(args.font), ','.join(f'{c:x}' for c in scalars),
            'liga=0,clig=0,kern=0' if args.independent else 'default', '1148846080', '0', '0', 'en-US'], capture_output=True, text=True,
            timeout=2, creationflags=subprocess.CREATE_NO_WINDOW)
        observation = json.loads(single.stdout)
        if observation['hresult']:
            reference_errors[len(observations)] = observation
            observations.append(None)
        else:
            observations.append(observation['clusters'])
    native = subprocess.run([str(args.native), '--clusters'], input=native_input,
        capture_output=True, timeout=5, creationflags=subprocess.CREATE_NO_WINDOW)
    if native.returncode or native.stdout[:12] != native_input[:12]:
        raise RuntimeError('Native batch failed: ' + repr(native.stdout[-500:] + native.stderr))
    ni = 12
    rows, mismatches = [], []
    for index, ((scalars, group), clusters) in enumerate(zip(cases, observations)):
        expected = [1] + [int(clusters[i] != clusters[i-1]) for i in range(1, len(scalars))] if clusters is not None else None
        actual = list(native.stdout[ni:ni + len(scalars)])
        ni += len(scalars)
        row = dict(scalars=scalars, group=group, expected=expected, actual=actual, exact=actual == expected if expected is not None else None)
        if index in reference_errors:
            row['reference_error'] = reference_errors[index]
        rows.append(row)
        if row['exact'] is False:
            mismatches.append(row)
    if ni != len(native.stdout):
        raise RuntimeError('Truncated or trailing batch response')
    result = dict(status='passed' if not mismatches else 'failed', seconds=time.perf_counter()-before,
        cases=len(rows), mismatches=len(mismatches), reference_unavailable=len(reference_errors), scope='Candidate single-script cluster boundaries, not full shaping',
        typography='independent' if args.independent else 'default',
        inputs=[dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
                for path in (args.font, args.native, args.reference)], rows=rows)
    (output/'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f"{len(rows)-len(mismatches)-len(reference_errors)}/{len(rows)} exact boundaries in {result['seconds']:.2f}s; {len(mismatches)} differ; {len(reference_errors)} Windows failures")
    for row in mismatches[:15]:
        text = ','.join(f'{c:x}' for c in row['scalars'][:16]) + ('...' if len(row['scalars']) > 16 else '')
        print(text, 'expected starts', [i for i,b in enumerate(row['expected']) if b],
              'native starts', [i for i,b in enumerate(row['actual']) if b])
    return int(bool(mismatches))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('font', 'native', 'reference', 'output-dir'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--independent', action='store_true', help='Compare the app mode disabling liga/clig/kern.')
    args = parser.parse_args()
    if args.worker:
        return run(args, args.output_dir)
    output = args.output_dir / ('run-' + str(time.time_ns()))
    output.mkdir(parents=True)
    result_path = output/'result.json'
    result_path.write_text('{"status":"running"}', encoding='utf-8')
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from verify_native_text import run_check, CheckFailure
    command = [sys.executable, str(Path(__file__).resolve()), '--worker', '--font', str(args.font.resolve()),
        '--native', str(args.native.resolve()), '--reference', str(args.reference.resolve()), '--output-dir', str(output.resolve())]
    if args.independent:
        command.append('--independent')
    try:
        execution = run_check('devanagari-clusters', command, Path.cwd(), output, 10, threading.Event())
    except CheckFailure as error:
        execution = error.row
    result = json.loads(result_path.read_text(encoding='utf-8'))
    result.update(status=execution['status'], execution=execution)
    result_path.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('Evidence:', output)
    return execution['exit']


if __name__ == '__main__':
    raise SystemExit(main())
