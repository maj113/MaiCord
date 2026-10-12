"""Validate a complete native/DirectWrite character-corpus capture and summarize it.

Diagnostic success does not imply parity. --require-exact rejects any captured
level difference. No visual/pixel result is inferred from bidi levels.
"""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
from make_bidi_supplementary import REPLACEMENTS

SOURCE_SHA256 = 'a3e6e905ab5afbe318a96df5401d0372a04cd73ef139ab5e3cf0ae241c255488'
VECTORS = ('dw_levels', 'native_levels', 'line_levels', 'layout_levels', 'candidate_levels')
COUNTERS = ('checked', 'removed', 'explicit_diff', 'paragraph_diff', 'line_diff',
            'surrogate_diff', 'layout_diff', 'layout_analyzer_diff')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summarize(capture, source, supplementary=False):
    if digest(source) != SOURCE_SHA256:
        raise ValueError('BidiCharacterTest source identity differs from pinned Unicode 17')
    expected = {}
    for line, text in enumerate(source.read_text(encoding='utf-8').splitlines(), 1):
        text = text.split('#')[0].strip()
        if not text:
            continue
        cp, policy, base, levels, order = text.split(';')[:5]
        cps = [int(x, 16) for x in cp.split()]
        if supplementary:
            cps = [REPLACEMENTS.get(cp, cp) for cp in cps]
        expected[line] = (cps, int(policy), int(base),
                          [255 if x == 'x' else int(x) for x in levels.split()])
    totals = collections.Counter()
    affected = collections.Counter()
    policy_groups = {i: collections.Counter() for i in range(3)}
    first_cp = collections.Counter()
    examples = []
    seen = set()
    with capture.open(encoding='utf-8', newline='') as stream:
        for raw in csv.DictReader(stream):
            r = {k: ([int(x) for x in v.split()] if k in VECTORS else int(v)) for k, v in raw.items()}
            line = r['source_line']
            if r.get('has_expected', 1) != 1:
                raise ValueError('Exploratory capture is not an official expected-output comparison')
            if line not in expected or line in seen:
                raise ValueError(f'Unknown/duplicate source line {line}')
            seen.add(line)
            cps, policy, base, levels = expected[line]
            n = len(cps)
            if (r['scalars'], r['policy'], r['base'], r['line_levels']) != (n, policy, base, levels):
                raise ValueError(f'Native output or case identity differs from official data at {line}')
            if any(len(r[k]) != n or any(x > 126 and x != 255 for x in r[k]) for k in VECTORS if k in r):
                raise ValueError(f'Invalid vector at {line}')
            visible = [i for i in range(n) if levels[i] != 255]
            if r['checked'] != len(visible) or r['removed'] != n-len(visible):
                raise ValueError(f'Invalid checked/removed accounting at {line}')
            for count_key, left, right in (
                ('paragraph_diff', 'dw_levels', 'native_levels'),
                ('line_diff', 'dw_levels', 'line_levels'),
                ('layout_diff', 'layout_levels', 'line_levels'),
                ('layout_analyzer_diff', 'layout_levels', 'dw_levels'),
            ):
                actual = sum(r[left][i] != r[right][i] for i in visible)
                if actual != r[count_key]:
                    raise ValueError(f'{count_key} accounting differs at {line}')
            for vector, index_key, level_key in (
                ('dw_levels', 'first_diff', 'dw_level'),
                ('layout_levels', 'first_layout_diff', 'layout_level'),
            ):
                indices = [i for i in visible if r[vector][i] != levels[i]]
                index = indices[0] if indices else 0xffffffff
                if r[index_key] != index or r[level_key] != (r[vector][index] if indices else 255):
                    raise ValueError(f'First difference disagrees with vector at {line}')
            if r['explicit_diff'] > len(visible) or r['surrogate_diff'] > sum(cp > 0xffff for cp in cps):
                raise ValueError(f'Invalid explicit/surrogate counter at {line}')
            if 'candidate_levels' in r:
                actual = sum(r['candidate_levels'][i] != r['dw_levels'][i] for i in visible)
                if actual != r['candidate_diff']:
                    raise ValueError(f'Candidate counter disagrees with vector at {line}')
                totals['candidate_diff'] += actual
                affected['candidate_diff'] += bool(actual)
                affected['candidate_new_difference'] += bool(actual and not r['line_diff'])
                affected['candidate_fixed'] += bool(r['line_diff'] and not actual)
            totals.update({k: r[k] for k in COUNTERS})
            affected.update({k: bool(r[k]) for k in COUNTERS})
            totals['scalars'] += n
            totals['supplementary_scalars'] += sum(cp > 0xffff for cp in cps)
            group = policy_groups[policy]
            group['cases'] += 1
            group['line_difference_cases'] += bool(r['line_diff'])
            group['line_difference_scalars'] += r['line_diff']
            if r['line_diff']:
                first_cp[cps[r['first_diff']]] += 1
            if line in (61, 129, 208, 211, 221, 225, 407, 446):
                examples.append({'source_line': line, 'policy': policy,
                                 'code_points': [f'U+{cp:04X}' for cp in cps],
                                 **{k: r[k] for k in VECTORS if k in r}})
    if seen != expected.keys():
        raise ValueError(f'Incomplete capture: {len(seen)} of {len(expected)} cases')
    return {'scope': 'installed DirectWrite analyzer and no-wrap Segoe UI 16 en-US layout hit-test levels; not pixels',
            'synthetic_supplementary': supplementary,
            'cases': len(seen), 'scalar_totals': dict(totals), 'affected_cases': dict(affected),
            'policies': {str(k): dict(v) for k, v in policy_groups.items()},
            'first_difference_code_points': {f'U+{k:04X}': v for k, v in first_cp.most_common()},
            'examples': examples, 'character_source_sha256': digest(source), 'capture_sha256': digest(capture),
            'exact': not any(totals[k] for k in COUNTERS if k.endswith('_diff'))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--character-tests', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--require-exact', action='store_true')
    parser.add_argument('--supplementary', action='store_true', help='Validate the deterministic supplementary transplant corpus')
    args = parser.parse_args()
    result = summarize(args.capture, args.character_tests, args.supplementary)
    args.out.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(f"Verified {result['cases']} cases; level parity: {result['exact']}")
    return 2 if args.require_exact and not result['exact'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
