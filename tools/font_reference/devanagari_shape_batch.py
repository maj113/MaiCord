"""Bounded full-glyph/cluster/placement comparison for generated Devanagari inputs.

One native font preparation and reused scratch for the whole stream. Windows
failures are preserved as unavailable evidence, never counted as matches.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time

from devanagari_cluster_oracle import corpus


def reference_rows(args, cases):
    rows = []
    while len(rows) < len(cases):
        remaining = cases[len(rows):]
        payload = bytearray(b'MCSHPB1\0' + struct.pack('<I', len(remaining)))
        for scalars, _ in remaining:
            payload += struct.pack('<II', len(scalars), int(args.independent) | (int(args.rtl) << 1)) + struct.pack('<'+'H'*len(scalars), *scalars)
        process = subprocess.run([str(args.reference), str(args.font), '915', 'default', '1148846080', '0', '0', 'en-US'],
            input=payload, capture_output=True, timeout=5, env=dict(os.environ, MAICORD_SHAPE_BATCH='1'), creationflags=subprocess.CREATE_NO_WINDOW)
        data = process.stdout
        if data[:12] != payload[:12]:
            raise RuntimeError('Invalid reference batch header')
        cursor = 12
        for scalars, _ in remaining:
            if cursor == len(data):
                break
            count = struct.unpack_from('<I', data, cursor)[0]
            cursor += 4
            glyphs = list(struct.unpack_from('<'+'H'*count, data, cursor)); cursor += count*2
            clusters = list(struct.unpack_from('<'+'H'*len(scalars), data, cursor)); cursor += len(scalars)*2
            placements = [list(struct.unpack_from('<III', data, cursor+i*12)) for i in range(count)]; cursor += count*12
            rows.append(dict(glyphs=glyphs, clusters=clusters, placements=placements))
        if not process.returncode:
            if len(rows) != len(cases) or cursor != len(data):
                raise RuntimeError('Missing or trailing reference batch data')
            break
        if len(rows) == len(cases):
            raise RuntimeError('Reference failed after last record')
        scalars, _ = cases[len(rows)]
        single = subprocess.run([str(args.reference), str(args.font), ','.join(f'{c:x}' for c in scalars),
            'liga=0,clig=0,kern=0' if args.independent else 'default', '1148846080', str(int(args.rtl)), '0', 'en-US'],
            capture_output=True, text=True, timeout=2, creationflags=subprocess.CREATE_NO_WINDOW)
        value = json.loads(single.stdout)
        if value['hresult'] or value['placement_hresult']:
            rows.append(dict(unavailable=value))
        else:
            rows.append(dict(glyphs=value['glyphs'], clusters=value['clusters'], placements=value['placement_bits']))
    return rows


def run(args, output):
    before = time.perf_counter()
    cases = [(tuple(int(c, 16) for c in case.split(',')), 'explicit') for case in args.case] if args.case else corpus()
    if not args.case:
        seen = {s for s, _ in cases}
        for base in (0x915, 0x930, 0x905):
            for matra in (0x93a, 0x93b, 0x93e, 0x93f, 0x940, 0x941, 0x944, 0x945, 0x948, 0x94b, 0x94e, 0x94f, 0x955, 0x957, 0x963, 0xa8ff):
                for sign in (0x900, 0x901, 0x902, 0x903, 0x953, 0x954):
                    for vedic in ((), (0x951,), (0x1cd0,)):
                        scalars = (base, matra, sign, *vedic, 0x200c)
                        if scalars not in seen:
                            seen.add(scalars);cases.append((scalars, 'extended-control-tails'))
    if args.group:
        cases = [(s, g) for s, g in cases if g in args.group]
    if not cases:
        raise RuntimeError('Empty comparison corpus')
    payload = bytearray(b'MDSHP1\0\0' + struct.pack('<I', len(cases)))
    for scalars, _ in cases:
        payload += struct.pack('<I', len(scalars)) + struct.pack('<'+'I'*len(scalars), *scalars)
    started = time.perf_counter()
    native = subprocess.run([str(args.native), str(args.font)], input=payload, capture_output=True, timeout=5,
        env=dict(os.environ, MAICORD_DEVANAGARI_BATCH='1', MAICORD_DEVANAGARI_RTL=str(int(args.rtl))), creationflags=subprocess.CREATE_NO_WINDOW)
    native_seconds = time.perf_counter()-started
    if native.returncode or native.stdout[:12] != payload[:12]:
        raise RuntimeError('Invalid native batch: ' + repr(native.stdout[-500:] + native.stderr))
    expected = reference_rows(args, cases)
    cursor = 12
    matches = failures = unavailable = 0
    groups, categories, examples = {}, Counter(), []
    with (output/'observations.jsonl').open('w', encoding='utf-8') as observations:
        for (scalars, group), reference in zip(cases, expected):
            status, count = struct.unpack_from('<II', native.stdout, cursor); cursor += 8
            glyphs = list(struct.unpack_from('<'+'H'*count, native.stdout, cursor)); cursor += count*2
            clusters = list(struct.unpack_from('<'+'I'*len(scalars), native.stdout, cursor)); cursor += len(scalars)*4
            placements = [list(struct.unpack_from('<III', native.stdout, cursor+i*12)) for i in range(count)]; cursor += count*12
            actual = dict(status=status, glyphs=glyphs, clusters=clusters, placements=placements)
            difference = 'unavailable' if 'unavailable' in reference else 'status' if status else next(
                (key for key in ('glyphs', 'clusters', 'placements') if actual[key] != reference[key]), '')
            groups.setdefault(group, Counter())[difference or 'exact'] += 1
            categories[difference or 'exact'] += 1
            if difference == 'unavailable':
                unavailable += 1
            elif difference:
                failures += 1
                if len(examples) < 20:
                    examples.append(dict(scalars=scalars, difference=difference, native=actual, reference=reference))
            else:
                matches += 1
            observations.write(json.dumps(dict(scalars=scalars, group=group, difference=difference, native=actual,
                                              reference=reference), separators=(',', ':')) + '\n')
    if cursor != len(native.stdout):
        raise RuntimeError('Trailing or truncated native records')
    result = dict(status='passed' if not failures else 'failed', cases=len(cases), exact=matches, mismatches=failures,
        reference_unavailable=unavailable, seconds=time.perf_counter()-before, native_process_seconds=native_seconds,
        typography='independent' if args.independent else 'default', rtl=args.rtl, groups=groups, differences=categories, examples=examples,
        inputs=[dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest()) for path in (args.font,args.native,args.reference)])
    (output/'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f"{matches}/{len(cases)} exact shaped inputs; {failures} differ; {unavailable} unavailable; {result['seconds']:.2f}s")
    for row in examples[:8]:
        print(','.join(f'{c:x}' for c in row['scalars']), row['difference'], row['native']['glyphs'], row['reference'].get('glyphs'))
    return int(bool(failures))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('font','native','reference','output-dir'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--independent', action='store_true')
    parser.add_argument('--rtl', action='store_true', help='Compare directional shaping for an RTL override.')
    parser.add_argument('--group', action='append', default=[])
    parser.add_argument('--case', action='append', default=[], help='One comma-separated hexadecimal scalar sequence; repeatable.')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        return run(args,args.output_dir)
    output = args.output_dir/('run-'+str(time.time_ns()));output.mkdir(parents=True)
    path = output/'result.json';path.write_text('{"status":"running"}',encoding='utf-8')
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
    from verify_native_text import run_check,CheckFailure
    command = [sys.executable,str(Path(__file__).resolve()),'--worker','--font',str(args.font.resolve()),
        '--native',str(args.native.resolve()),'--reference',str(args.reference.resolve()),'--output-dir',str(output.resolve())]
    if args.independent:command.append('--independent')
    if args.rtl:command.append('--rtl')
    for value in args.group:command.extend(('--group',value))
    for value in args.case:command.extend(('--case',value))
    try:
        execution=run_check('devanagari-shape-batch',command,Path.cwd(),output,10,threading.Event())
    except CheckFailure as error:
        execution=error.row
    result=json.loads(path.read_text(encoding='utf-8'));result.update(status=execution['status'],execution=execution)
    path.write_text(json.dumps(result,indent=2),encoding='utf-8');print('Evidence:',output)
    return execution['exit']


if __name__ == '__main__':
    raise SystemExit(main())
