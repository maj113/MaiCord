"""Generate deterministic exploratory bracket cases and validate their captures.

No expected result is generated. The distinct MCBIDIP format explicitly marks
these as Windows observations, never as Unicode conformance evidence.
"""
import argparse
import csv
import hashlib
import itertools
import json
from pathlib import Path
import random
import struct
import zipfile

UCD_SHA256 = '2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generate(ucd, output):
    if sha(ucd) != UCD_SHA256:
        raise ValueError('Unicode archive identity differs')
    cases = []

    def add(cps, group):
        assert 0 < len(cps) <= 512
        for base in (0, 1):
            cases.append({'id': len(cases)+1, 'base': base, 'cp': list(cps), 'group': group})

    alphabet = [0x61, 0x5d0, 0x31, 0x661, 0x28, 0x29, 0x5b, 0x5d]
    for size in range(1, 6):
        for text in itertools.product(alphabet, repeat=size):
            add(text, 'exhaustive_8_symbols_length_1_to_5')
    with zipfile.ZipFile(ucd) as archive:
        brackets = []
        for line in archive.read('BidiBrackets.txt').decode().splitlines():
            line = line.split('#')[0].strip()
            if not line:
                continue
            a, b, kind = [x.strip() for x in line.split(';')[:3]]
            if kind == 'o':
                brackets.append((int(a, 16), int(b, 16)))
    for a, b in brackets:
        for before, inside, after in itertools.product([[], [0x61], [0x5d0], [0x31], [0x661]], repeat=3):
            add(before+[a]+inside+[b]+after, 'all_unicode_bracket_pairs')
    for depth in range(1, 201):
        add([0x61]+[0x28]*depth+[0x62]+[0x29]*depth, 'nesting_depth_1_to_200')
    rng = random.Random(0xb1d1b2ac)
    random_alphabet = alphabet+[0x20, 0x300, 0x5b0, 0x627, 0x2b, 0x2c, 0x24,
                                0x7b, 0x7d, 0x2329, 0x232a, 0x3008, 0x3009]
    for _ in range(10000):
        add([rng.choice(random_alphabet) for _ in range(rng.randrange(1, 129))], 'random_weak_and_brackets')
    data = bytearray(b'MCBIDIP\0'+struct.pack('<I', len(cases)))
    for case in cases:
        data += struct.pack('<6I', len(case['cp']), case['base'], 255, 0, case['id'], 1)
        for cp in case['cp']:
            data += struct.pack('<IBB2x', cp, 255, 255)
    output.write_bytes(data)
    manifest = {'kind': 'exploratory; no generated expected output', 'ucd_sha256': UCD_SHA256,
                'corpus_sha256': sha(output), 'cases': cases}
    output.with_suffix('.json').write_text(json.dumps(manifest, separators=(',', ':'))+'\n')
    print(f'Generated {len(cases)} labeled exploratory cases')


def summarize(capture, manifest_path, output):
    manifest = json.loads(manifest_path.read_text())
    cases = {c['id']: c for c in manifest['cases']}
    seen = set()
    groups = {}
    failures = []
    vectors = ('dw_levels', 'native_levels', 'line_levels', 'layout_levels', 'candidate_levels')
    with capture.open(newline='') as stream:
        for raw in csv.DictReader(stream):
            row = {k: ([int(x) for x in v.split()] if k in vectors else int(v)) for k, v in raw.items()}
            ident = row['source_line']
            if ident in seen or ident not in cases or row['has_expected']:
                raise ValueError('Unexpected, duplicate or mislabeled case')
            seen.add(ident)
            case = cases[ident]
            n = len(case['cp'])
            if (row['base'], row['policy'], row['scalars'], row['checked'], row['removed']) != (case['base'], case['base'], n, n, 0):
                raise ValueError(f'Case identity/accounting mismatch at {ident}')
            if any(len(row[v]) != n or any(x > 126 for x in row[v]) for v in vectors):
                raise ValueError(f'Bad vector at {ident}')
            for count, left, right in [('line_diff', 'line_levels', 'dw_levels'),
                                       ('candidate_diff', 'candidate_levels', 'dw_levels'),
                                       ('layout_diff', 'layout_levels', 'line_levels'),
                                       ('layout_analyzer_diff', 'layout_levels', 'dw_levels')]:
                if row[count] != sum(a != b for a, b in zip(row[left], row[right])):
                    raise ValueError(f'Counter/vector mismatch at {ident}')
            group = groups.setdefault(case['group'], {'cases': 0, 'baseline_different': 0, 'candidate_different': 0,
                                                     'new_differences': 0, 'fixed': 0, 'layout_analyzer_different': 0})
            group['cases'] += 1
            group['baseline_different'] += bool(row['line_diff'])
            group['candidate_different'] += bool(row['candidate_diff'])
            group['new_differences'] += bool(row['candidate_diff'] and not row['line_diff'])
            group['fixed'] += bool(row['line_diff'] and not row['candidate_diff'])
            group['layout_analyzer_different'] += bool(row['layout_analyzer_diff'])
            if row['candidate_diff']:
                failures.append({**case, **{v: row[v] for v in vectors}})
    if seen != cases.keys():
        raise ValueError(f'Incomplete capture: {len(seen)} of {len(cases)} cases')
    result = {'cases': len(seen), 'groups': groups, 'capture_sha256': sha(capture),
              'manifest_sha256': sha(manifest_path), 'corpus_sha256': manifest['corpus_sha256'],
              'candidate_exact': not failures, 'failure_count': len(failures)}
    output.write_text(json.dumps(result, indent=2)+'\n')
    output.with_name(output.stem+'-failures.json').write_text(json.dumps(failures, separators=(',', ':'))+'\n')
    print(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    generate_parser = sub.add_parser('generate')
    generate_parser.add_argument('--ucd', required=True, type=Path)
    generate_parser.add_argument('--out', required=True, type=Path)
    summarize_parser = sub.add_parser('summarize')
    summarize_parser.add_argument('capture', type=Path)
    summarize_parser.add_argument('--manifest', required=True, type=Path)
    summarize_parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.action == 'generate':
        generate(args.ucd, args.out)
    else:
        summarize(args.capture, args.manifest, args.out)


if __name__ == '__main__':
    main()
