"""Run complete style matrices as disjoint two-second correctness shards."""
import argparse
import concurrent.futures
import json
import re
import threading
import time
from pathlib import Path
from benchmark_native_text_performance import identity
from verify_native_text import CheckFailure, run_check


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=Path('D:/MaiCord-build/Release/MaiCordNativeAppLineTests.exe'))
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--jobs', type=int, choices=range(1, 5), default=4)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    destination = out / 'style-shards.json'
    if destination.exists() or any(out.glob('*.log')):
        parser.error('Preserve existing reports; choose an unused evidence directory')
    exe = args.exe.resolve()
    binary = identity(exe)
    matrices = [('style', 8), ('decoration', 16), ('bidi-styles', 32)]
    report = dict(status='running', executable=binary, jobs=args.jobs, workers=[], matrices={},
                  sources=[identity(root / p) for p in ('tests/native_app_line_test.cpp', 'tests/native_bidi_styles_cases.inc', 'src/ui/native_text_styled.inc')],
                  scope='Complete Segoe UI 400 style, decoration and bidi-style Cartesian matrices. '
                        'No quick sampling. Parallel correctness only; worker duration is not engine performance.')
    started = time.perf_counter()

    def worker(name, index, count):
        command = [str(exe), '--matrix-shard', str(index), str(count), f'--{name}-profile', '--family', 'Segoe UI', '400']
        try:
            execution = run_check(f'{name}-{index}', command, root, out, 2, threading.Event())
        except CheckFailure as error:
            return dict(matrix=name, execution=error.row, error='Child failed or exceeded two seconds')
        text = Path(execution['log']).read_text(encoding='utf-8')
        rows = [json.loads(line) for line in text.splitlines() if line.startswith('{"kind":"matrix_shard"')]
        totals = re.search(r'cases=(\d+) native_cases=(\d+) simulated_cases=(\d+) checks=(\d+) PASS native app lines', text)
        if len(rows) != 1 or not totals:
            return dict(matrix=name, execution=execution, error='Missing shard receipt or success totals')
        row = rows[0]
        if (row['index'] != index or row['count'] != count or row['seen'] <= 0 or
                row['selected'] != (row['seen'] + count - 1 - index) // count or
                row['selected'] != int(totals[1])):
            return dict(matrix=name, execution=execution, error='Invalid shard coverage receipt')
        return dict(matrix=name, execution=execution, receipt=row, native_cases=int(totals[2]), checks=int(totals[4]))

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(worker, name, i, count) for name, count in matrices for i in range(count)]
            for future in concurrent.futures.as_completed(futures):
                report['workers'].append(future.result())
        for name, count in matrices:
            group = [w for w in report['workers'] if w['matrix'] == name]
            if any('error' in w for w in group) or len(group) != count:
                raise ValueError(f'{name}: incomplete verification')
            assert sorted(w['receipt']['index'] for w in group) == list(range(count))
            seen = {w['receipt']['seen'] for w in group}
            if len(seen) != 1 or sum(w['receipt']['selected'] for w in group) != next(iter(seen)):
                raise ValueError(f'{name}: shard union does not cover the complete matrix')
            report['matrices'][name] = dict(cases=next(iter(seen)), native_cases=sum(w['native_cases'] for w in group), checks=sum(w['checks'] for w in group))
        if identity(exe) != binary:
            raise ValueError('Executable changed during verification')
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        raise
    finally:
        report['seconds'] = time.perf_counter() - started
        destination.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report['matrices']))
    print(destination)


if __name__ == '__main__':
    main()
