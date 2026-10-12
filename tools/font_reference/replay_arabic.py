"""Replay saved Arabic Windows expectations without launching the reference tool."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def identity(path):
    return dict(path=str(path.resolve()), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--corpus', type=Path, action='append', required=True,
                        help='Saved capture directory containing corpus.bin and reference.json; repeatable.')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=5,
                        help='Whole replay deadline in seconds (default: 5).')
    args = parser.parse_args()
    if not 0 < args.timeout < float('inf'):
        parser.error('--timeout must be finite and positive')
    output = args.output_dir.resolve() / ('run-' + str(time.time_ns()))
    output.mkdir(parents=True)
    started = time.perf_counter()
    report = dict(status='running', seconds=0, comparisons=0, differences=[], corpora=[])
    try:
        report['native'] = identity(args.native)
        for index, root in enumerate(args.corpus):
            fixture, reference = root/'corpus.bin', root/'reference.json'
            expected = json.loads(reference.read_text(encoding='utf-8'))
            entry = dict(fixture=identity(fixture), reference=identity(reference))
            report['corpora'].append(entry)
            remaining = args.timeout - (time.perf_counter() - started)
            if remaining <= 0:
                raise TimeoutError('Whole replay deadline exceeded')
            run = subprocess.run([str(args.native.resolve()), str(fixture.resolve()), '--arabic-script'],
                                 capture_output=True, timeout=remaining,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
            (output/f'native-{index}.jsonl').write_bytes(run.stdout)
            (output/f'native-{index}.stderr').write_bytes(run.stderr)
            if run.returncode:
                raise RuntimeError(f'Native observer exited {run.returncode}')
            actual = [json.loads(line) for line in run.stdout.decode('utf-8').splitlines() if line.startswith('{')]
            if not expected or len(actual) != len(expected):
                raise ValueError(f'Expected {len(expected)} rows, received {len(actual)}')
            entry['comparisons'] = len(expected)
            for case, (wanted, observed) in enumerate(zip(expected, actual)):
                if observed['case'] != case or observed['gate'] != 1:
                    raise ValueError(f'Invalid case identity or native gate at {case}')
                if observed['status'] or any(wanted[key] != observed[key] for key in
                                            ('glyphs', 'clusters', 'placement_bits')):
                    report['differences'].append(dict(corpus=index, case=case, expected=wanted, actual=observed))
                report['comparisons'] += 1
        report['status'] = 'different' if report['differences'] else 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = repr(error)
    report['seconds'] = time.perf_counter() - started
    (output/'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f"{report['comparisons']} comparisons, {len(report['differences'])} differences, "
          f"{report['seconds']:.2f}s, {report['status']}: {output}")
    if 'error' in report:
        print(report['error'])
    return int(report['status'] != 'passed')


if __name__ == '__main__':
    raise SystemExit(main())
