"""Five-second native Sinhala input checks against saved Windows boundaries.

Final source-group boundaries constrain preparation; this is not glyph,
positioning, reordering, rasterization or application parity.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time

from tamil_cluster_oracle import identity, observe
from sinhala_input_samples import corpus


def native_stream(exe, cases, expand=False):
    payload = bytearray(b'MDVCL1\0\0' + struct.pack('<I', len(cases)))
    for scalars, _ in cases:
        payload += struct.pack('<I', len(scalars)) + struct.pack('<' + 'I'*len(scalars), *scalars)
    run = subprocess.run([str(exe)], input=payload, capture_output=True, timeout=2,
        env=dict(os.environ, MAICORD_SINHALA_INPUT_STREAM='2' if expand else '1'),
        creationflags=subprocess.CREATE_NO_WINDOW)
    if run.returncode or run.stdout[:12] != payload[:12]:
        raise RuntimeError('Invalid native input response: ' + repr(run.stdout[-200:]))
    return run.stdout[12:]


def worker(args):
    started = time.perf_counter()
    cases = [(tuple(s), 'explicit') for s in json.loads(args.case_file.read_text(encoding='utf-8'))] if args.case_file else corpus(args.stress)
    if not cases or any(not s or len(s)>256 or any(type(c) is not int or not 0<=c<=65535 or 0xd800<=c<=0xdfff for c in s) for s,_ in cases):
        raise ValueError('Expected nonempty BMP cases within the observer capacity')
    inputs = [identity(args.font), identity(args.reference)]
    if args.observations:
        saved = json.loads(args.observations.read_text(encoding='utf-8'))
        if saved['inputs'] != inputs or [r['scalars'] for r in saved['rows']] != [list(s) for s,_ in cases]:
            raise ValueError('Font, observer or corpus observations changed')
        rows = saved['rows']
    else:
        # Bound each reference invocation by input volume as well as time.
        # Persist completed chunks so a pathological later case never discards
        # prior observations. The parent still enforces five seconds overall.
        rows = [];at = 0
        while at < len(cases):
            end = at;units = 0
            while end < len(cases) and end-at < 256 and units+len(cases[end][0]) <= 8192:
                units += len(cases[end][0]);end += 1
            rows.extend(observe(args, cases[at:end], seed='d9a'));at = end
            pending = args.output_dir/'observations.pending.json'
            pending.write_text(json.dumps(dict(inputs=inputs,rows=rows,complete=at==len(cases))),encoding='utf-8')
            pending.replace(args.output_dir/'observations.json')
    (args.output_dir/'observations.json').write_text(json.dumps(dict(inputs=inputs, rows=rows)), encoding='utf-8')
    data = native_stream(args.native, cases)
    if len(data) != sum(len(s) for s,_ in cases):
        raise RuntimeError('Native boundary response length mismatch')
    at = 0
    for row in rows:
        n = len(row['scalars']);row['native'] = list(data[at:at+n]);at += n
        row['exact'] = None if 'unavailable' in row else row['native'] == row['expected']
    differences = sum(r['exact'] is False for r in rows)
    unavailable = sum(r['exact'] is None for r in rows)
    result = dict(status='failed' if differences else 'incomplete' if unavailable else 'passed',
        cases=len(rows), differences=differences, unavailable=unavailable, seconds=time.perf_counter()-started,
        native=identity(args.native), inputs=inputs, scope=__doc__, rows=rows)
    (args.output_dir/'result.json').write_text(json.dumps(result), encoding='utf-8')
    print(f'{len(rows)-differences-unavailable}/{len(rows)} exact boundary vectors in {result["seconds"]:.2f}s')
    return 1 if differences else 2 if unavailable else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('font','reference','native','output-dir'):
        parser.add_argument('--'+key, type=Path, required=True)
    parser.add_argument('--observations', type=Path)
    choice = parser.add_mutually_exclusive_group()
    choice.add_argument('--stress', action='store_true')
    choice.add_argument('--case-file', type=Path)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    for key in ('font','reference','native','output_dir'):
        setattr(args, key, getattr(args,key).resolve())
    if args.worker:
        return worker(args)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    if args.observations and args.observations.resolve() == (args.output_dir/'observations.json').resolve():
        parser.error('Keep saved observations separate')
    path = args.output_dir/'result.json';path.write_text('{"status":"running"}', encoding='utf-8')
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from verify_native_text import run_check, CheckFailure
    try:
        execution = run_check('sinhala-input', [sys.executable, str(Path(__file__).resolve()),
            *sys.argv[1:], '--worker'], Path.cwd(), args.output_dir, 5, threading.Event())
    except CheckFailure as error:
        execution = error.row
    result = json.loads(path.read_text(encoding='utf-8'));result['execution'] = execution
    if execution['exit']:
        result['status'] = execution['status']
    path.write_text(json.dumps(result), encoding='utf-8')
    print({k:result[k] for k in ('status','cases','differences','unavailable') if k in result})
    return execution['exit']


if __name__ == '__main__':
    raise SystemExit(main())
