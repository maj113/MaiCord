"""Reduce captured candidate mismatches by querying a frozen Windows QA oracle.

Inputs remain exploratory. Retain every batch input/output and the exact oracle
identity so later candidate edits cannot silently change the reduction target.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--failures', required=True, type=Path)
    parser.add_argument('--oracle', required=True, type=Path)
    parser.add_argument('--previous-oracle', type=Path, help='Preserve a regression: previous candidate must match Windows')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--limit', type=int, default=32)
    args = parser.parse_args()
    if args.limit <= 0:
        raise ValueError('Reduction limit must be positive')
    failures = json.loads(args.failures.read_text())
    failures.sort(key=lambda c: (c['line_levels'] != c['dw_levels'], len(c['cp']), c['id']))
    selected = failures[:args.limit]
    if not selected:
        raise ValueError('No failure inputs to reduce')
    args.out.mkdir(parents=True, exist_ok=True)
    states = [{'id': c['id'], 'base': c['base'], 'original': c['cp'], 'cp': c['cp'], 'parts': 2, 'done': False} for c in selected]
    cache = {}
    batches = 0

    def observe(requests):
        nonlocal batches
        fresh = list(dict.fromkeys(key for key in requests if key not in cache))
        if fresh:
            data = bytearray(b'MCBIDIP\0'+struct.pack('<I', len(fresh)))
            for ident, (base, text) in enumerate(fresh, 1):
                data += struct.pack('<6I', len(text), base, 255, 0, ident, 1)
                for cp in text:
                    data += struct.pack('<IBB2x', cp, 255, 255)
            name = args.out/f'batch-{batches:03d}'
            name.with_suffix('.bin').write_bytes(data)
            with name.with_suffix('.csv').open('wb') as output:
                subprocess.run([str(args.oracle.resolve()), str(name.with_suffix('.bin').resolve())], stdout=output, check=True)
            rows = list(csv.DictReader(name.with_suffix('.csv').open()))
            if len(rows) != len(fresh):
                raise ValueError('Incomplete oracle output')
            for ident, (key, row) in enumerate(zip(fresh, rows), 1):
                if int(row['source_line']) != ident or int(row['has_expected']) or int(row['scalars']) != len(key[1]):
                    raise ValueError('Oracle case identity mismatch')
                candidate = [int(x) for x in row['candidate_levels'].split()]
                windows = [int(x) for x in row['dw_levels'].split()]
                native = [int(x) for x in row['line_levels'].split()]
                if len(candidate) != len(key[1]) or len(windows) != len(candidate) or len(native) != len(candidate):
                    raise ValueError('Oracle vector length mismatch')
                if int(row['candidate_diff']) != sum(a != b for a, b, c in zip(candidate, windows, native) if c != 255):
                    raise ValueError('Oracle difference counter mismatch')
                cache[key] = row
            if args.previous_oracle:
                previous_path = name.with_name(name.name+'-previous').with_suffix('.csv')
                with previous_path.open('wb') as output:
                    subprocess.run([str(args.previous_oracle.resolve()), str(name.with_suffix('.bin').resolve())], stdout=output, check=True)
                previous_rows = list(csv.DictReader(previous_path.open()))
                if len(previous_rows) != len(fresh):
                    raise ValueError('Incomplete previous-oracle capture')
                for ident, (key, row) in enumerate(zip(fresh, previous_rows), 1):
                    if int(row['source_line']) != ident or row['dw_levels'] != cache[key]['dw_levels']:
                        raise ValueError('Previous oracle identity or Windows observation differs')
                    candidate = [int(x) for x in row['candidate_levels'].split()]
                    windows = [int(x) for x in row['dw_levels'].split()]
                    native = [int(x) for x in row['line_levels'].split()]
                    if len(candidate) != len(key[1]) or len(native) != len(candidate) or int(row['candidate_diff']) != sum(a != b for a, b, c in zip(candidate, windows, native) if c != 255):
                        raise ValueError('Previous oracle vector/counter mismatch')
                    cache[key]['previous_candidate_diff'] = int(row['candidate_diff'])
            batches += 1
        return [int(cache[key]['candidate_diff']) != 0 and not cache[key].get('previous_candidate_diff', 0) for key in requests]

    assert all(observe([(s['base'], tuple(s['cp'])) for s in states]))
    while not all(s['done'] for s in states):
        requests = []
        candidates = []
        for s in states:
            variations = []
            if not s['done']:
                n = len(s['cp'])
                width = (n+s['parts']-1)//s['parts']
                for start in range(0, n, width):
                    candidate = s['cp'][:start]+s['cp'][start+width:]
                    if candidate:
                        variations.append((len(requests), candidate))
                        requests.append((s['base'], tuple(candidate)))
            candidates.append(variations)
        observed = observe(requests)
        for s, variations in zip(states, candidates):
            if s['done']:
                continue
            failed = next((text for index, text in variations if observed[index]), None)
            if failed:
                s['cp'] = failed
                s['parts'] = max(2, s['parts']-1)
            elif s['parts'] >= len(s['cp']):
                s['done'] = True
            else:
                s['parts'] = min(len(s['cp']), s['parts']*2)
        print(f'Batches {batches}, remaining {sum(not s["done"] for s in states)}, total scalars {sum(len(s["cp"]) for s in states)}', flush=True)
    result = {'oracle_path': str(args.oracle.resolve()), 'oracle_sha256': hashlib.sha256(args.oracle.read_bytes()).hexdigest(),
              'source_sha256': hashlib.sha256(args.failures.read_bytes()).hexdigest(), 'batches': batches, 'observations': len(cache), 'cases': []}
    if args.previous_oracle:
        result['previous_oracle_sha256'] = hashlib.sha256(args.previous_oracle.read_bytes()).hexdigest()
        result['predicate'] = 'candidate differs and previous candidate matches the same Windows levels'
    for s in states:
        row = cache[(s['base'], tuple(s['cp']))]
        result['cases'].append({**s, 'dw_levels': row['dw_levels'], 'native_levels': row['line_levels'], 'candidate_levels': row['candidate_levels']})
    (args.out/'results.json').write_text(json.dumps(result, indent=2)+'\n')
    print('Finished deletion-minimal reductions; no Unicode conformance claim')


if __name__ == '__main__':
    main()
