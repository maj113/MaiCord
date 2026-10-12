"""Freeze focused scalar/boundary observations; never embeds installed font data."""
import argparse
import hashlib
import json
from pathlib import Path


REGRESSIONS = '''
915,200c,915
915,200d,905
915,200d,93c,200d
915,94d,901,915
930,94d,901,915
930,93c,94d,905
930,94d,93c,905
930,94d,200d,94d
930,94d,200d,93f
905,200c,94d,93c
905,200d,94d,200d
905,94d,200d,951
915,951,952,200c
915,93f,940,94d,940,94d
915,93f,941,93c,940,93c
a0,200d,93f
25cc,94d,915
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=Path('tests/generated/devanagari_cluster_fixture.inc'))
    args = parser.parse_args()
    raw = args.report.read_bytes()
    report = json.loads(raw)
    if report['status'] != 'passed' or report['mismatches']:
        raise RuntimeError('Only a successful comparison can provide frozen expectations')
    wanted = {tuple(int(s, 16) for s in line.split(',')) for line in REGRESSIONS.split()}
    rows = []
    for row in report['rows']:
        key = tuple(row['scalars'])
        if row['exact'] is True and (key in wanted or row['group'] in
                ('words', 'partial-input', 'scalar-coverage', 'long-conjunct', 'long-repetition')):
            wanted.discard(key)
            rows.append(row)
    if wanted:
        raise RuntimeError('Missing requested regressions: ' + repr(wanted))
    words = []
    for row in rows:
        assert 0 < len(row['scalars']) <= 256
        for i, (scalar, start) in enumerate(zip(row['scalars'], row['expected'])):
            words.append(scalar | start << 31 | int(i + 1 == len(row['scalars'])) << 30)
    lines = ['// First-party scalar corpus; Windows-observed boundaries, not font bytes.',
             '// Regenerate with tools/font_reference/devanagari_cluster_fixture.py.',
             '// Observation SHA-256: ' + hashlib.sha256(raw).hexdigest(),
             '// Source font SHA-256: ' + report['inputs'][0]['sha256'],
             '// Source observer SHA-256: ' + report['inputs'][2]['sha256'],
             f'constexpr u32 deva_cluster_fixture_cases={len(rows)};',
             '// Bit 31: cluster start. Bit 30: end of this test input.',
             'constexpr u32 deva_cluster_fixture[]{']
    lines += ['    ' + ','.join(f'0x{word:08x}u' for word in words[i:i+8]) + ',' for i in range(0, len(words), 8)]
    lines.append('};')
    args.output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'Wrote {len(rows)} cases, {len(words)} scalar/flag records, {len(words)*4} read-only QA bytes')


if __name__ == '__main__':
    main()
