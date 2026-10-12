"""Compare focused native/reference app-field or wrapped-profile captures."""
import argparse
import hashlib
import json
import os
import re
import struct
from pathlib import Path
import threading
import time

from verify_native_text import CheckFailure, run_check


def identity(path):
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def bmp_difference(native_path, reference_path):
    """Locate differences in the probe's uncompressed 32-bit BMPs, without dependencies."""
    native, reference = native_path.read_bytes(), reference_path.read_bytes()
    width, height, planes, bits, compression = struct.unpack_from('<iiHHI', native, 18)
    if (native[:2] != b'BM' or reference[:2] != b'BM' or width <= 0 or height == 0 or
            (planes, bits, compression) != (1, 32, 0) or
            struct.unpack_from('<iiHHI', reference, 18) != (width, height, planes, bits, compression)):
        raise RuntimeError('Unexpected or incompatible capture BMP format')
    n_start, r_start = struct.unpack_from('<I', native, 10)[0], struct.unpack_from('<I', reference, 10)[0]
    count = 0
    left, top, right, bottom = width, abs(height), -1, -1
    stride = width * 4
    if n_start + stride * abs(height) != len(native) or r_start + stride * abs(height) != len(reference):
        raise RuntimeError('Incomplete capture BMP')
    for row in range(abs(height)):
        n_row = native[n_start + row * stride:n_start + (row + 1) * stride]
        r_row = reference[r_start + row * stride:r_start + (row + 1) * stride]
        if n_row == r_row:
            continue
        y = row if height < 0 else height - 1 - row
        for x in range(width):
            if n_row[x * 4:x * 4 + 4] != r_row[x * 4:x * 4 + 4]:
                count += 1
                left, top, right, bottom = min(left, x), min(top, y), max(right, x), max(bottom, y)
    return dict(changed_pixels=count, bounds=[left, top, right + 1, bottom + 1] if count else None,
                width=width, height=abs(height))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-exe', required=True, type=Path)
    parser.add_argument('--reference-exe', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--script-set', choices=('hangul', 'standard', 'extended-standard', 'thai', 'thai-wrap', 'devanagari', 'devanagari-tracking', 'devanagari-direction', 'direction-tracking'), default='hangul')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve() / ('run-' + str(time.time_ns()))
    output.mkdir(parents=True)
    runs = []
    for side, executable in [('native', args.native_exe), ('reference', args.reference_exe)]:
        executable = executable.resolve()
        before = identity(executable)
        captures, data = output / (side + '-captures'), output / (side + '-data')
        captures.mkdir(); data.mkdir()
        env = os.environ.copy()
        env.pop('MAICORD_HANGUL_RENDER_PROBE', None)
        env.pop('MAICORD_STANDARD_RENDER_PROBE', None)
        env.pop('MAICORD_EXTENDED_STANDARD_RENDER_PROBE', None)
        env.pop('MAICORD_THAI_RENDER_PROBE', None)
        env.pop('MAICORD_THAI_WRAP_RENDER_PROBE', None)
        env.pop('MAICORD_DEVANAGARI_RENDER_PROBE', None)
        env.pop('MAICORD_DEVANAGARI_TRACKING_RENDER_PROBE', None)
        env.pop('MAICORD_DEVANAGARI_DIRECTION_RENDER_PROBE', None)
        env.pop('MAICORD_DIRECTION_TRACKING_RENDER_PROBE', None)
        env['MAICORD_' + args.script_set.upper().replace('-', '_') + '_RENDER_PROBE'] = '1'
        env.update(MAICORD_GPU_METRICS='1',
                   MAICORD_UI_PROBE_CAPTURE_DIR=str(captures), LOCALAPPDATA=str(data))
        row = run_check(side, [str(executable)], repo, output, 15, threading.Event(), env=env)
        if identity(executable) != before:
            raise RuntimeError('Executable changed during verification')
        log = data / 'MaiCord/logs/maicord.log'
        text = log.read_text(encoding='utf-8')
        if args.script_set.replace('-', '_') + '_render_probe_passed' not in text or 'session_has_live_handles' in text:
            raise RuntimeError(side + ': missing successful probe or leaked font ownership')
        if ('native_outline_decodes=' in text) != (side == 'native'):
            raise RuntimeError(side + ': wrong renderer build supplied')
        counters = {key: int(value) for key, value in re.findall(r'\b(native_\w+)=(\d+)(?=\r?$)', text, re.MULTILINE)}
        color_builds = counters.get('native_colr_builds', 0)
        # The profile includes shell/composer emoji at additional sizes. Field
        # fixtures retain their exact two-build contract; both require COLRv1.
        color_valid = color_builds >= 2 if args.script_set in ('thai-wrap', 'direction-tracking', 'extended-standard') else color_builds == 2
        if side == 'native' and (counters.get('native_outline_failures') != 0 or
                                not color_valid or counters.get('native_paragraphs_shaped', 0) == 0):
            raise RuntimeError('Native outline/COLRv1/shaping evidence missing')
        runs.append(dict(side=side, executable=before, process=row, log=identity(log), counters=counters))
    names = {f'{args.script_set}-{sample}-{mode}-{scale}.bmp'
             for sample in range(6 if args.script_set == 'thai-wrap' else 4) for mode in range(3) for scale in range(2)}
    for side in ('native', 'reference'):
        if {p.name for p in (output / (side + '-captures')).glob('*.bmp')} != names:
            raise RuntimeError(side + ': capture set incomplete')
    comparisons = []
    for name in sorted(names):
        native = identity(output / 'native-captures' / name)
        reference = identity(output / 'reference-captures' / name)
        exact = native['sha256'] == reference['sha256']
        difference = None if exact else bmp_difference(Path(native['path']), Path(reference['path']))
        comparisons.append(dict(name=name, exact=exact, native=native, reference=reference,
                                difference=difference))
    passed = all(row['exact'] for row in comparisons)
    scope = ('six wrapped profile biographies, including contextual lookahead and attachments; 144/216/360-DIP viewports, 2/4/6 repetitions; 96/144 DPI' if args.script_set == 'thai-wrap'
             else 'four app-field strings; plain, selection, composition; 96/144 DPI')
    result = dict(status='passed' if passed else 'different', script_set=args.script_set, scope=scope,
                  runs=runs, comparisons=comparisons,
                  source=identity(repo / 'tests/integration/ui_probe.cpp'),
                  fixture=identity(repo / 'tests/integration/thai_wrapped_probe.inc') if args.script_set == 'thai-wrap' else None)
    (output / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f"{sum(row['exact'] for row in comparisons)}/{len(comparisons)} exact captures. Evidence: {output}")
    return 0 if passed else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except CheckFailure as error:
        raise SystemExit(error.row['exit'])
