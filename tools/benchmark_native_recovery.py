"""Paired recovery cloning/replay/retirement checkpoints, two seconds per child."""
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import statistics
import threading

from verify_native_text import CheckFailure, run_check


def identity(path):
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    exes = dict(before=args.before.resolve(), after=args.after.resolve())
    seed = args.cache.resolve()
    assert seed.read_bytes()[:4] == b'MCFS'
    report = dict(status='running', executables={k: identity(p) for k, p in exes.items()},
                  cache=identity(seed), executions=[], samples=[], memory=[],
                  scope='Three alternating process pairs per count/mode, seven operations per process. '
                        'Same-family declarations at different sizes. Clone includes context allocation. '
                        'Retirement includes existing diagnostic logging. Memory is a separate untimed '
                        'checkpoint before replay, not an observed maximum. Warm/unspecified OS caches.')
    try:
        for rep in range(3):
            for count in (1, 21, 131, 513):
                for mode in range(3):
                    for label in (('before', 'after') if rep % 2 == 0 else ('after', 'before')):
                        name = f'{rep}-{count}-{mode}-{label}'
                        directory = root / name
                        cache = directory / 'cache/MaiCord'
                        cache.mkdir(parents=True)
                        shutil.copy2(seed, cache / 'native-fonts-v1.bin')
                        env = {k: v for k, v in os.environ.items() if not k.startswith('MAICORD_')}
                        env['LOCALAPPDATA'] = str(cache.parent)
                        with contextlib.redirect_stdout(io.StringIO()):
                            execution = run_check(name, [str(exes[label]), '--recovery-cost', str(count), str(mode)],
                                                  repo, directory, 2, threading.Event(), env=env)
                        report['executions'].append(execution)
                        rows = [json.loads(line) for line in Path(execution['log']).read_text().splitlines()
                                if line.startswith('{')]
                        samples = [r for r in rows if r['kind'] == 'native_recovery_cost']
                        memory = [r for r in rows if r['kind'] == 'native_recovery_memory']
                        assert len(samples) == 7 and len(memory) == 1
                        for row in samples + memory:
                            assert row['formats'] == count and row['mode'] == mode
                            row.update(repetition=rep, provider=label)
                        for row in samples:
                            assert row['frequency'] > 0
                            assert sum(row[k+'_ticks'] for k in ('clone', 'replay', 'retire')) == row['total_ticks']
                        report['samples'].extend(samples)
                        report['memory'].extend(memory)
        assert all(identity(p) == report['executables'][k] for k, p in exes.items())
        summary = []
        for count in (1, 21, 131, 513):
            for mode in range(3):
                result = dict(formats=count, mode=mode)
                for label in exes:
                    selected = [r for r in report['samples'] if (r['formats'], r['mode'], r['provider']) == (count, mode, label)]
                    memory = [r for r in report['memory'] if (r['formats'], r['mode'], r['provider']) == (count, mode, label)]
                    result[label] = {key: statistics.median(r[key+'_ticks']*1e6/r['frequency'] for r in selected)
                                     for key in ('clone', 'replay', 'retire', 'total')}
                    result[label].update(total_cycles=statistics.median(r['total_cycles'] for r in selected),
                                         staged_private_delta=statistics.median(r['staged_private']-r['before_private'] for r in memory),
                                         after_private_delta=statistics.median(r['after_private']-r['before_private'] for r in memory))
                summary.append(result)
        report.update(status='passed', summary=summary)
    except Exception as error:
        report.update(status='failed', error=str(error))
        if isinstance(error, CheckFailure):
            report['executions'].append(error.row)
        raise
    finally:
        (root / 'recovery.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
