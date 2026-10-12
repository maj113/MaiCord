"""Five-second paired cluster-reuse benchmark; font setup is outside the timer."""
from pathlib import Path
import argparse
import json
import os
import statistics
import threading
import time

from benchmark_devanagari import identity
from verify_native_text import run_check


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('font', 'native', 'output-dir'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    output = args.output_dir.resolve()/('run-'+str(time.time_ns()))
    output.mkdir(parents=True)
    exe, font = args.native.resolve(), args.font.resolve()
    report = dict(status='running', executable=identity(exe), font=identity(font))
    try:
        env = dict(os.environ, MAICORD_DEVANAGARI_MEMO_BENCH='1')
        env.pop('MAICORD_DEVANAGARI_BATCH', None)
        report['execution'] = run_check('memo', [str(exe), str(font)],
            Path(__file__).resolve().parents[1], output, 5, threading.Event(), env=env)
        if identity(exe) != report['executable'] or identity(font) != report['font']:
            raise RuntimeError('Benchmark inputs changed during measurement')
        rows = [json.loads(line) for line in (output/'memo.log').read_text(encoding='utf-8').splitlines() if line.startswith('{')]
        if len(rows) != 3 or {row['corpus'] for row in rows} != {'repeated', 'natural', 'unique'}:
            raise ValueError('Expected three corpora from one single-face font')
        for row in rows:
            if row['kind'] != 'deva_memo_cpu' or row['iterations'] <= 0 or row['frequency'] <= 0 or len(row['batches']) != 5 or any(
                    len(batch) != 4 or any(value <= 0 for value in batch) for batch in row['batches']):
                raise ValueError('Invalid timing samples')
            row['cycle_ratio'] = statistics.median(batch[3] for batch in row['batches'])/statistics.median(batch[2] for batch in row['batches'])
            row['paired_cycle_ratios'] = [batch[3]/batch[2] for batch in row['batches']]
            print(f"{row['corpus']}: memo/baseline CPU cycles {row['cycle_ratio']:.4f}")
        report.update(status='measured', measurements=rows,
            scope='Warm retained font; same first-party run shaper with reuse disabled/enabled. No DirectWrite, GPU, raster, cold-start or whole-app comparison.')
    except Exception:
        report['status'] = 'failed'
        raise
    finally:
        path = output/'result.json'
        path.write_text(json.dumps(report, indent=2), encoding='utf-8')
        print('Evidence:', path)


if __name__ == '__main__':
    main()
