"""Bounded, synthetic Windows Sinhala feature-policy observations; QA only.

These single-base probes reveal activation and stage grouping, not Sinhala
cluster/reordering correctness. FontTools writes fixtures, never shapes text.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

from fontTools.ttLib import TTFont, newTable
from feature_plan_oracle import language, script, layout
from gsub_execution_oracle import make_font, lookup, single, ORDER, identity
from gpos_execution_oracle import adjust

GROUPS = [('locl', 'ccmp', 'nukt', 'akhn'), ('rphf',), ('pref',),
          ('rkrf', 'abvf', 'blwf', 'half', 'pstf', 'vatu', 'cjct'), (),
          ('abvs', 'blws', 'calt', 'clig', 'haln', 'liga', 'pres', 'psts', 'rclt', 'rlig')]
POSITION = ('curs', 'dist', 'kern', 'mark', 'abvm', 'blwm', 'mkmk')


def scenarios():
    for kind, tags in [('GSUB', sum((list(g) for g in GROUPS), []) + ['rvrn', 'init', 'isol']),
                       ('GPOS', list(POSITION))]:
        for tag in tags:
            yield dict(name=kind + '-' + tag, kind=kind, features=[(tag, [0])],
                       lookups=[lookup(1, single({'A': 'B'})) if kind == 'GSUB' else adjust('A', 0, 0, 31)])
    for tags in [('locl', 'ccmp'), ('nukt', 'akhn'), ('pstf', 'vatu'),
                 ('half', 'blwf'), ('pres', 'abvs'), ('ccmp', 'pstf'), ('rphf', 'pref'),
                 ('rvrn', 'ccmp'), ('rvrn', 'rphf'), ('pstf', 'isol'), ('isol', 'pres'),
                 ('rvrn', 'pres'), ('rvrn', 'isol')]:
        for reverse in (False, True):
            yield dict(name='order-' + '-'.join(tags) + '-' + str(int(reverse)), kind='GSUB',
                       features=sorted([(tags[0], [int(reverse)]), (tags[1], [int(not reverse)])]),
                       lookups=[lookup(1, single({'A': 'B'})), lookup(1, single({'B': 'C'}))])
    for tags in [('ccmp', 'locl'), ('pstf', 'vatu'), ('ccmp', 'pstf'), ('pstf', 'pres'),
                 ('rvrn', 'ccmp'), ('rvrn', 'rphf'), ('pstf', 'isol'), ('isol', 'pres'),
                 ('rvrn', 'pres'), ('rvrn', 'isol')]:
        yield dict(name='shared-' + '-'.join(tags), kind='GSUB', features=sorted((t, [0]) for t in tags),
                   lookups=[lookup(1, single(dict(zip(ORDER[1:10], ORDER[2:11]))))])
    for tag in ('test', 'ccmp', 'liga'):
        yield dict(name='required-' + tag, kind='GSUB', features=[(tag, [0])], required=0,
                   lookups=[lookup(1, single(dict(zip(ORDER[1:10], ORDER[2:11]))))])


def worker(args):
    started = time.perf_counter()
    rows = []
    result = dict(status='running', reference=identity(args.reference), script=identity(Path(__file__)),
                  scope='Single Sinhala base; feature activation/order only; no shaping or pixel parity.', checks=rows)
    destination = args.output_dir / 'result.json'
    for case in scenarios():
        if args.case and case['name'] not in args.case:
            continue
        path = args.output_dir / (case['name'] + '.ttf')
        make_font(path, dict(name=case['name'], lookups=[lookup(1, single({'A': 'A'}))]))
        with TTFont(path, recalcTimestamp=False) as font:
            for cmap in font['cmap'].tables:
                if cmap.isUnicode():
                    cmap.cmap[0xd9a] = 'A'
            del font['GSUB']
            font[case['kind']] = newTable(case['kind'])
            font[case['kind']].table = layout(case['kind'], case['features'], case['lookups'],
                {'sinh': script(language(range(len(case['features'])), case.get('required', 65535)))})
            font.save(path)
        row = dict(name=case['name'], font=identity(path), observations=[])
        rows.append(row)
        for mode in ('default', 'liga=0,clig=0,kern=0'):
            run = subprocess.run([str(args.reference), str(path), 'd9a', mode, '1148846080', '0', '0', 'en-US'],
                capture_output=True, text=True, timeout=2, creationflags=subprocess.CREATE_NO_WINDOW)
            if run.returncode:
                raise RuntimeError(run.stdout + run.stderr)
            value = json.loads(run.stdout)
            if value['hresult'] or value['placement_hresult']:
                raise RuntimeError(value)
            row['observations'].append(dict(mode=mode, glyph_names=[ORDER[g] for g in value['glyphs']], result=value))
        destination.write_text(json.dumps(result, indent=2), encoding='utf-8')
    result.update(status='observed', seconds=time.perf_counter()-started)
    destination.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f'{len(rows)*2} observations in {result["seconds"]:.2f}s')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--case', action='append')
    args = parser.parse_args()
    if args.worker:
        worker(args)
        return 0
    args.output_dir.mkdir(parents=True, exist_ok=True)
    path = args.output_dir / 'result.json'
    path.write_text('{"status":"running"}', encoding='utf-8')
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from verify_native_text import run_check, CheckFailure
    try:
        execution = run_check('sinhala-policy', [sys.executable, str(Path(__file__).resolve()),
            '--worker', '--reference', str(args.reference.resolve()), '--output-dir', str(args.output_dir.resolve()),
            *sum((['--case', name] for name in args.case or []), [])],
            Path.cwd(), args.output_dir, 5, threading.Event())
    except CheckFailure as error:
        execution = error.row
    result = json.loads(path.read_text(encoding='utf-8'))
    result['execution'] = execution
    if execution['exit']:
        result['status'] = execution['status']
    path.write_text(json.dumps(result, indent=2), encoding='utf-8')
    return execution['exit']


if __name__ == '__main__':
    raise SystemExit(main())
