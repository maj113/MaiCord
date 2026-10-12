"""Bounded, alternating native Arabic run measurements against saved expectations."""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import time
from replay_arabic import identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--corpus', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    output = args.output_dir.resolve()/('run-'+str(time.time_ns()))
    output.mkdir(parents=True)
    started = time.perf_counter()
    report = dict(status='running', before=identity(args.before), after=identity(args.after),
                  corpus=identity(args.corpus/'corpus.bin'), reference=identity(args.corpus/'reference.json'), batches=[])
    pattern = re.compile(r'^BENCH case=(\d+) iterations=(\d+) ticks=(\d+) frequency=(\d+) cycles=(\d+)$', re.MULTILINE)
    try:
        expected = json.loads((args.corpus/'reference.json').read_text(encoding='utf-8'))
        if not expected:
            raise ValueError('Empty benchmark corpus')
        for batch in range(5):
            for side in ((0, 1) if batch % 2 == 0 else (1, 0)):
                remaining = 5 - (time.perf_counter()-started)
                if remaining <= 0:
                    raise TimeoutError('Five-second whole benchmark deadline exceeded')
                exe = (args.before, args.after)[side]
                run = subprocess.run([str(exe.resolve()), str((args.corpus/'corpus.bin').resolve()), '--bench-arabic'],
                                     capture_output=True, timeout=remaining, creationflags=subprocess.CREATE_NO_WINDOW)
                (output/f'{batch}-{side}.txt').write_bytes(run.stdout)
                if run.returncode:
                    raise RuntimeError(f'Observer exited {run.returncode}: {run.stderr!r}')
                text = run.stdout.decode('utf-8')
                actual = [json.loads(line) for line in text.splitlines() if line.startswith('{')]
                timings = [tuple(map(int, values)) for values in pattern.findall(text)]
                if len(actual) != len(expected) or len(timings) != len(expected):
                    raise ValueError('Incomplete output or timing rows')
                for index, (wanted, observed, timing) in enumerate(zip(expected, actual, timings)):
                    if (observed['case'] != index or observed['gate'] != 1 or observed['status'] or
                        any(wanted[key] != observed[key] for key in ('glyphs', 'clusters', 'placement_bits'))):
                        raise ValueError(f'Parity failure: batch {batch}, side {side}, case {index}')
                    if timing[0] != index or timing[1] != 256 or timing[3] <= 0:
                        raise ValueError('Invalid timing identity or iteration count')
                report['batches'].append(dict(batch=batch, side=side, cases=[dict(case=i, iterations=n,
                    nanoseconds_per_run=ticks*1e9/frequency/n, cycles_per_run=cycles/n)
                    for i,n,ticks,frequency,cycles in timings]))
        report['cases'] = []
        for index in range(len(expected)):
            value = dict(case=index, scalars=expected[index]['scalars'], mode=expected[index]['mode'], em_bits=expected[index]['em_bits'])
            for metric in ('nanoseconds_per_run', 'cycles_per_run'):
                medians = [statistics.median(row['cases'][index][metric] for row in report['batches'] if row['side']==side) for side in (0,1)]
                value[metric] = dict(before=medians[0], after=medians[1], ratio=medians[1]/medians[0])
            report['cases'].append(value)
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed';report['error'] = repr(error)
    report['seconds'] = time.perf_counter()-started
    (output/'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f"{report['status']}, {report['seconds']:.2f}s: {output}")
    if 'error' in report:
        print(report['error'])
    return int(report['status'] != 'passed')


if __name__ == '__main__':
    raise SystemExit(main())
