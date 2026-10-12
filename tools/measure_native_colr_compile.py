"""Bounded sequential native/Windows COLRv1 compilation cohorts, excluding GPU work.

Each fresh process checks at most 128 programs for matching nodes and geometry,
allowing shared rows and redundant zero-length edges, then performs six
alternating timed pairs with retained font/program owners. These are warm compile
costs, not cold OS font-cache costs or app startup latency. Child cap: two seconds.
"""
from pathlib import Path
import argparse
import json
import statistics
import threading
from verify_native_text import run_check, CheckFailure
from verify_hangul_render import identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=Path('D:/MaiCord-build/Release/MaiCordNativeColrCost.exe'))
    parser.add_argument('--font', type=Path, default=Path('C:/Windows/Fonts/seguiemj.ttf'))
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
    destination = out / 'measurement.json'
    if destination.exists(): parser.error('Choose a new evidence directory')
    exe, font = args.exe.resolve(), args.font.resolve()
    report = dict(status='running', scope=__doc__, executable=identity(exe),
                  font=identity(font), rows=[], source=[identity(root / p) for p in
                  ('tests/native_colr_cost_test.cpp', 'tests/colr_program_equal.hpp', 'tools/colr_reference/native_source.cpp',
                   'tools/colr_reference/native_source.hpp', 'src/render/colr_program.cpp',
                   'src/render/colr_paint.cpp', 'tools/measure_native_colr_compile.py')])
    def save(): destination.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    save()
    try:
        first = 0; total = None
        while total is None or first < total:
            row = dict(first=first); report['rows'].append(row)
            row['process'] = run_check('compile-' + str(first),
                [str(exe), str(font), '--program-cost', str(first)], root, out, 2, threading.Event())
            data = json.loads(Path(row['process']['log']).read_text(encoding='utf-8'))
            if total is None: total = data['total_glyphs']
            if (data['kind'] != 'colr_cost' or not data['equivalent_programs'] or
                data['total_glyphs'] != total or not 0 < total <= 16384 or
                data['first'] != first or data['glyphs'] != min(128, total-first) or
                len(data['batches']) != 6 or data['frequency'] <= 0 or
                any(len(pair) != 4 or any(value <= 0 for value in pair) for pair in data['batches'])):
                raise ValueError('Incomplete or malformed compilation cohort')
            row['measurement'] = data
            row['median_ticks'] = [statistics.median(pair[i] for pair in data['batches']) for i in range(2)]
            row['median_cycles'] = [statistics.median(pair[i+2] for pair in data['batches']) for i in range(2)]
            if identity(exe) != report['executable'] or identity(font) != report['font']:
                raise ValueError('Executable/font changed during measurement')
            first += data['glyphs']; save()
        report['totals'] = dict(glyphs=first,
            native_program_bytes=sum(row['measurement']['program_bytes'] for row in report['rows']),
            reference_program_bytes=sum(row['measurement']['reference_program_bytes'] for row in report['rows']),
            sum_cohort_median_ms=[sum(row['median_ticks'][i]*1000/row['measurement']['frequency'] for row in report['rows']) for i in range(2)],
            sum_cohort_median_cycles=[sum(row['median_cycles'][i] for row in report['rows']) for i in range(2)],
            worker_seconds=sum(row['process']['seconds'] for row in report['rows']),
            maximum_worker_seconds=max(row['process']['seconds'] for row in report['rows']))
        report['status'] = 'passed'
    except CheckFailure as error:
        report.update(status='failed', process=error.row, error=str(error)); raise
    except Exception as error:
        report.update(status='failed', error=str(error)); raise
    finally: save()
    print(json.dumps(report['totals'])); print(destination)


if __name__ == '__main__': main()
