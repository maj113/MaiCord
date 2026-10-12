"""Bounded paired latency of failed public shape requests during font replacement.

Uses the QA --request-recovery fixture. This is neither ordinary layout timing
nor a DirectWrite comparison. It includes existing recovery diagnostic logging.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import statistics
import threading

from verify_native_text import run_check, CheckFailure


def identity(path):
    data = path.read_bytes()
    return dict(path=str(path), bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def observations(raw):
    markers = list(re.finditer(
        r'\{"kind":"request_recovery","mode":(\d+),"frequency":(\d+),"samples":\[', raw))
    if len(markers) != 3 or 'PASS public request recovery:' not in raw:
        raise ValueError('Incomplete replacement/recovery fixture')
    rows = []
    for index, marker in enumerate(markers):
        # The archived initial fixture interleaves recovery telemetry into its
        # record. Preserve the original log and accept only seven numeric pairs
        # within that record. Current fixtures emit uninterrupted JSON records.
        end = markers[index + 1].start() if index + 1 < len(markers) else len(raw)
        pairs = [list(map(int, pair)) for pair in re.findall(
            r'\[(\d+),(\d+)\]', raw[marker.end():end])]
        mode, frequency = int(marker[1]), int(marker[2])
        if mode != index or frequency <= 0 or len(pairs) != 7 or any(
                ticks <= 0 or cycles <= 0 for ticks, cycles in pairs):
            raise ValueError('Invalid recovery observations')
        rows.append(dict(mode=mode, frequency=frequency, samples=pairs))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('before', 'after', 'output-dir'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    binaries = {side: identity(getattr(args, side).resolve()) for side in ('before', 'after')}
    report = dict(status='running', scope=__doc__, executables=binaries, samples=[])
    env = os.environ.copy()
    env['LOCALAPPDATA'] = str(out / 'local-app-data')
    try:
        for trial in range(7):
            for order in range(2):
                side = ('before', 'after')[order ^ (trial & 1)]
                with contextlib.redirect_stdout(io.StringIO()):
                    execution = run_check(f'{trial}-{side}',
                        [binaries[side]['path'], '--request-recovery'], root, out,
                        2, threading.Event(), env=env)
                raw = Path(execution['log']).read_text(encoding='utf-8')
                report['samples'].append(dict(side=side, trial=trial,
                    execution=execution, rows=observations(raw)))
        summary = []
        for mode in range(3):
            row = dict(mode=mode)
            for side in ('before', 'after'):
                values = [s['rows'][mode] for s in report['samples'] if s['side'] == side]
                row[side] = dict(
                    microseconds=statistics.median(statistics.median(
                        p[0] * 1e6 / r['frequency'] for p in r['samples']) for r in values),
                    cycles=statistics.median(statistics.median(
                        p[1] for p in r['samples']) for r in values))
            summary.append(row)
        if any(identity(Path(value['path'])) != value for value in binaries.values()):
            raise ValueError('A measured executable changed')
        report.update(status='passed', summary=summary)
        print(json.dumps(summary, indent=2))
    except CheckFailure as error:
        report.update(status='failed', failure=error.row)
        raise
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
