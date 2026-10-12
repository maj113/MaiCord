"""Replay saved Windows Sinhala policy observations against native lookup plans.

The tiny QA interpreter evaluates only single substitutions/positions in authored
single-base fixtures. This checks schedule compatibility, not the native GSUB
executor, syllable parsing, glyph reordering, rasterization or app admission.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time

from fontTools.ttLib import TTFont
from gsub_execution_oracle import identity


def plans(exe, font):
    run = subprocess.run([str(exe), str(font)], capture_output=True, text=True,
                         timeout=2, creationflags=subprocess.CREATE_NO_WINDOW)
    if run.returncode:
        raise RuntimeError(run.stdout + run.stderr)
    rows = [json.loads(line) for line in run.stdout.splitlines() if line.startswith('{')]
    if len(rows) not in (2, 4):
        raise RuntimeError('Missing native plan records')
    return rows


def worker(args):
    started = time.perf_counter()
    checks = []
    for path in args.observations:
        saved = json.loads(path.read_text(encoding='utf-8'))
        if saved['status'] != 'observed' or saved['execution']['exit']:
            raise RuntimeError('Incomplete Windows observations')
        for case in saved['checks']:
            font_path = Path(case['font']['path'])
            if identity(font_path) != case['font']:
                raise RuntimeError('Fixture identity changed')
            rows = plans(args.native, font_path)
            with TTFont(font_path) as font:
                for mode, observed in enumerate(case['observations']):
                    row = next(r for r in rows if r['independent'] == mode)
                    table = font['GPOS' if row['kind'] else 'GSUB'].table
                    glyph = font.getBestCmap()[0xd9a]
                    advance = font['hmtx'][glyph][0]
                    for index, mask, stage in row['steps']:
                        if mask:
                            raise RuntimeError('Wrong frontend eligibility mask')
                        lookup = table.LookupList.Lookup[index]
                        if lookup.LookupType != 1 or lookup.LookupFlag:
                            raise RuntimeError('Outside single-base fixture interpreter scope')
                        for sub in lookup.SubTable:
                            if not row['kind'] and glyph in sub.mapping:
                                glyph = sub.mapping[glyph]
                                break
                            if row['kind'] and glyph in sub.Coverage.glyphs:
                                if sub.Format != 1:
                                    raise RuntimeError('Outside SinglePos format-one scope')
                                advance += sub.Value.XAdvance
                                break
                    exact = [glyph] == observed['glyph_names']
                    if row['kind']:
                        bits = struct.unpack('<I', struct.pack('<f', advance))[0]
                        exact = exact and observed['result']['placement_bits'] == [[bits, 0, 0]]
                    checks.append(dict(name=case['name'], mode=mode, exact=exact, native=row,
                                       projected_glyph=glyph, reference=observed['glyph_names']))
    # Independent metadata check of the installed-font snapshot. Expectations
    # are explicit, not derived from the planner.
    if args.font:
        expected = [[[84, 0, 0], [85, 0, 1], [86, 0, 3], [87, 0, 3], [88, 0, 3],
                     [89, 0, 6], [90, 0, 6], [91, 0, 6], [92, 0, 6]], [[105, 0, 0]]]
        for row in plans(args.native, args.font):
            checks.append(dict(name='Nirmala-sinh', mode=row['independent'], kind=row['kind'],
                               exact=row['steps'] == expected[row['kind']], native=row))
    result = dict(status='passed' if checks and all(c['exact'] for c in checks) else 'failed',
                  seconds=time.perf_counter()-started, cases=len(checks), native=identity(args.native),
                  observations=[identity(p) for p in args.observations],
                  font=identity(args.font) if args.font else None,
                  scope=__doc__, checks=checks)
    (args.output_dir / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f'{sum(c["exact"] for c in checks)}/{len(checks)} checks in {result["seconds"]:.2f}s')
    return 0 if result['status'] == 'passed' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', required=True, type=Path)
    parser.add_argument('--observations', required=True, type=Path, action='append')
    parser.add_argument('--font', type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        return worker(args)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    path = args.output_dir / 'result.json'
    if path.resolve() in (p.resolve() for p in args.observations):
        parser.error('Keep saved observations separate')
    path.write_text('{"status":"running"}', encoding='utf-8')
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from verify_native_text import run_check, CheckFailure
    try:
        execution = run_check('sinhala-plan-comparison', [sys.executable, str(Path(__file__).resolve()),
            *sys.argv[1:], '--worker'], Path.cwd(), args.output_dir, 5, threading.Event())
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
