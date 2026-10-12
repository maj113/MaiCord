"""Bounded Windows-only oracle for collection identity, ownership and cached acquisition."""
import argparse
import datetime
import json
import os
import statistics
import threading
from pathlib import Path

from benchmark_native_text_performance import identity
from verify_native_text import CheckFailure, run_check


def main():
    fonts = Path(os.environ.get('WINDIR', 'C:/Windows')) / 'Fonts'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=Path('D:/MaiCord-build/Release/MaiCordFontMatchingReference.exe'))
    parser.add_argument('--first', type=Path, default=fonts / 'segoeui.ttf')
    parser.add_argument('--second', type=Path, default=fonts / 'consola.ttf')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=5)
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 10:
        parser.error('Use 1-10 bounded workers')
    root = Path(__file__).resolve().parents[1]
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    target = out / 'collection-lifetime.json'
    if target.exists() or any(out.glob('lifetime-*.log')):
        parser.error('Preserve existing results; use an unused directory')
    executable = identity(args.exe.resolve())
    inputs = [identity(p.resolve()) for p in (args.first, args.second)]
    report = dict(status='running', utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  executable=executable, fonts=inputs, executions=[], samples=[],
                  sources=[identity(root / p) for p in ('tools/font_reference/collection_lifetime.inc',
                      'tools/font_reference/font_matching_reference.cpp', 'tools/probe_dwrite_collection_lifetime.py')],
                  scope='DirectWrite QA only. Two distinct families, immutable custom snapshots, retained '
                        'format/layout ownership, and 32 warm system collection acquire/release calls. '
                        'Timing excludes initialization, forced update, layout and rendering. '
                        'No actual system font installation or expiration is triggered. '
                        'Not a native engine, cold startup, RAM or throughput comparison.')
    try:
        for i in range(args.repetitions):
            execution = run_check(f'lifetime-{i}', [executable['path'], '--collection-lifetime',
                inputs[0]['path'], inputs[1]['path']], root, out, 2, threading.Event())
            report['executions'].append(execution)
            records = [json.loads(line) for line in Path(execution['log']).read_text(encoding='utf-8').splitlines()
                       if line.startswith('{')]
            rows = [r for r in records if r.get('kind') == 'collection_lifetime']
            if len(rows) != 1:
                raise ValueError('Expected one lifetime record')
            row = rows[0]
            if (not all(row.get(k) is True for k in ('exact', 'same_index_different_face',
                    'format_retained_collection', 'layout_retained_collection')) or
                    row.get('old_set_count') != 1 or row.get('new_set_count') != 2 or
                    row.get('cache_get_calls') != 32 or row['frequency'] <= 0 or
                    row['cached_ticks'] <= 0 or row['cached_cycles'] <= 0):
                raise ValueError('Incomplete ownership evidence')
            report['samples'].append(dict(result=row, records=records))
        if identity(args.exe.resolve()) != executable or [identity(p.resolve()) for p in (args.first, args.second)] != inputs:
            raise ValueError('Executable or font changed during observation')
        report['median_cached_get_us'] = statistics.median(s['result']['cached_ticks'] * 1e6 /
            s['result']['frequency'] / 32 for s in report['samples'])
        report['median_cached_get_cycles'] = statistics.median(s['result']['cached_cycles'] / 32 for s in report['samples'])
        report['status'] = 'observed'
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        if isinstance(error, CheckFailure):
            report['executions'].append(error.row)
        raise
    finally:
        target.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(target)
    print(json.dumps({k: report[k] for k in ('median_cached_get_us', 'median_cached_get_cycles')}))


if __name__ == '__main__':
    main()
