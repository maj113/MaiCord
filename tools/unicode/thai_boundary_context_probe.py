"""Bounded Thai prefix/separator contrasts, with reusable complete observations.

This measures Windows layout and the experimental native overlay. It never
generates runtime exceptions or treats a known mismatch as a passing test.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import time

from thai_recovery_model import observations


def cases(matrix='separators'):
    prefixes = ['', 'ก', 'กิ', 'กฅฆ', 'วิญญัติ', 'ภาษา', 'ติ', 'กด', 'การ',
                'ฟิลด์', 'ดำ', 'แดง']
    # Deletions distinguish a whole-word effect from a trailing character class.
    seed = 'วิญญัติ'
    prefixes += [seed[:i] + seed[i + 1:] for i in range(len(seed))]
    separators = ['', ' ', '  ', '   ', '\t', '\n', '\r\n', '\u00a0',
                  '\u200b', '\u200c', '\u200d', '.', ',', '-', '/', ' A ',
                  ' ภาษา ', ' ติ ', ' ก ']
    tails = ['การ์ฟิลด์ดำแดง', 'การ์ภาษา', 'กดุภาษา', 'ภาษาไทย',
             'การตรวจจับ', 'โมโหโการตรวจจับ']
    if matrix == 'distance':
        separators = ['', *(' ' * i for i in range(1, 33)),
                      *('\u200b' * i for i in range(1, 33))]
        tails = tails[:3]
    elif matrix == 'unknown':
        prefixes = ['', *(unit * i for unit in ('ฅ', 'ฆ', 'กิ', 'ฅิ') for i in range(1, 33))]
        separators = ['', ' ', '  ']
        tails = ['ฅ', *tails[:4], 'ติ', 'ดำแดง']
    return [dict(prefix=p, separator=s, tail=t, text=p + s + t)
            for p in dict.fromkeys(prefixes) for s in separators for t in tails]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--matrix', choices=('separators', 'distance', 'unknown'), default='separators')
    parser.add_argument('--replay', action='store_true',
                        help='Re-analyse the saved complete observation; launch no process')
    parser.add_argument('--deadline', type=float, default=2)
    args = parser.parse_args()
    if not math.isfinite(args.deadline) or args.deadline <= 0:
        parser.error('--deadline must be finite and positive')
    started = time.monotonic()
    rows = cases(args.matrix)
    payload = bytearray(b'MCLBPRB1' + struct.pack('<I', len(rows)))
    for identifier, row in enumerate(rows, 1):
        scalars = list(map(ord, row['text']))
        if not 0 < len(scalars) <= 256:
            raise ValueError('Probe case exceeds scalar capacity')
        payload += struct.pack('<II', identifier, len(scalars))
        payload += struct.pack('<' + 'I' * len(scalars), *scalars)
    output = args.output_dir
    probe, log = output / 'input.bin', output / 'layout.log'
    manifest_path = output / 'capture.json'
    if args.replay:
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        if probe.read_bytes() != payload or manifest['input_sha256'] != digest(payload):
            raise ValueError('Saved input differs from this probe matrix')
        if manifest['log_sha256'] != digest(log.read_bytes()):
            raise ValueError('Saved observation digest differs')
    else:
        output.mkdir(parents=True, exist_ok=True)
        if any(p.exists() for p in (probe, log, manifest_path)):
            raise ValueError('Capture already exists; use --replay or a new evidence directory')
        identity = dict(path=str(args.exe.resolve()), sha256=digest(args.exe.read_bytes()))
        probe.write_bytes(payload)
        before = time.monotonic()
        with log.open('wb') as stream:
            result = subprocess.run([str(args.exe.resolve()), '--thai-break-probe', str(probe.resolve())],
                                    stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=args.deadline, creationflags=subprocess.CREATE_NO_WINDOW)
        elapsed = time.monotonic() - before
        if result.returncode:
            raise RuntimeError(f'Probe exited with {result.returncode}; log preserved')
        manifest = dict(executable=identity, input_sha256=digest(payload),
                        log_sha256=digest(log.read_bytes()), process_seconds=elapsed,
                        deadline_seconds=args.deadline)
    observed = observations(probe, log)
    standalone = {}
    for row, value in zip(rows, observed, strict=True):
        if row['text'] != value['text']:
            raise ValueError('Decoded source differs from generated case')
        offset = len(row['prefix']) + len(row['separator'])
        row.update(reference_tail=[b - offset for b in value['reference'] if b > offset],
                   native_tail=[b - offset for b in value['before'] if b > offset],
                   complete_exact=value['reference'] == value['before'])
        if not offset:
            standalone[row['tail']] = row['reference_tail']
    for row in rows:
        row['context_changes_tail'] = row['reference_tail'] != standalone[row['tail']]
    groups = []
    for separator in dict.fromkeys(row['separator'] for row in rows):
        subset = [r for r in rows if r['separator'] == separator]
        groups.append(dict(separator=separator, cases=len(subset),
                           context_changes=sum(r['context_changes_tail'] for r in subset),
                           tail_exact=sum(r['reference_tail'] == r['native_tail'] for r in subset)))
    report = dict(matrix=args.matrix, cases=len(rows), complete_exact=sum(r['complete_exact'] for r in rows),
                  tail_exact=sum(r['reference_tail'] == r['native_tail'] for r in rows),
                  context_changes=sum(r['context_changes_tail'] for r in rows),
                  process_seconds=manifest['process_seconds'], replay=args.replay,
                  observation=manifest, runtime_modified=False, groups=groups, rows=rows)
    if not args.replay:
        # Only a successfully parsed, complete capture receives a manifest.
        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k not in ('rows', 'groups', 'observation')}))
    print(json.dumps(dict(analysis_seconds=time.monotonic() - started)))


if __name__ == '__main__':
    main()
