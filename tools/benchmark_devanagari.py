"""Bounded before/after Devanagari scratch and CPU microbenchmark; not a GPU/app benchmark."""
from pathlib import Path
import argparse
import hashlib
import json
import statistics
import threading
import time

from verify_native_text import run_check


def identity(path):
    data = path.read_bytes()
    return dict(path=str(path), bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def observations(path):
    rows = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()
            if line.startswith('{')]
    memory = {r['bytes']: r for r in rows if r['kind'] == 'deva_storage'}
    cpu = {(r['bytes'], r['mode']): r for r in rows if r['kind'] == 'deva_cpu'}
    if len(rows) != 9 or set(memory) != {411, 3267, 13059} or set(cpu) != {
            (size, mode) for size in memory for mode in (0, 1)}:
        raise ValueError('Incomplete or unexpected benchmark corpus')
    for row in cpu.values():
        if row['iterations'] <= 0 or row['frequency'] <= 0 or len(row['batches']) != 5 or any(
                len(batch) != 4 or any(value <= 0 for value in batch) for batch in row['batches']):
            raise ValueError('Invalid benchmark measurements')
    return memory, cpu


def compare(before, after):
    old_memory, old_cpu = observations(before)
    new_memory, new_cpu = observations(after)
    rows = []
    for size in sorted(old_memory):
        old, new = old_memory[size], new_memory[size]
        if old['units'] != new['units']:
            raise ValueError('Source coverage changed')
        row = dict(input_bytes=size, source_units=new['units'], before=old, after=new,
                   committed_reduction=1-new['committed']/old['committed'], cpu=[])
        for mode in (0, 1):
            previous, current = old_cpu[size, mode], new_cpu[size, mode]
            def cycles(value, side):
                return statistics.median(batch[side+2] for batch in value['batches'])/value['iterations']
            row['cpu'].append(dict(mode='cached' if mode == 0 else 'alternating_input',
                before_native_cycles=cycles(previous, 0), after_native_cycles=cycles(current, 0),
                current_reference_cycles=cycles(current, 1),
                native_reference_ratio=cycles(current, 0)/cycles(current, 1)))
        rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline-exe', 'candidate-exe', 'output-dir'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    output = args.output_dir.resolve()/('run-'+str(time.time_ns()))
    output.mkdir(parents=True)
    report = dict(status='running', runs=[])
    path = output/'result.json'
    try:
        for name, exe in (('baseline', args.baseline_exe), ('candidate', args.candidate_exe)):
            exe = exe.resolve()
            before = identity(exe)
            result = run_check(name, [str(exe), '--devanagari-bench'],
                Path(__file__).resolve().parents[1], output, 10, threading.Event())
            if identity(exe) != before:
                raise RuntimeError('Benchmark executable changed while running')
            report['runs'].append(dict(executable=before, execution=result))
        report.update(status='measured', comparisons=compare(output/'baseline.log', output/'candidate.log'),
                      scope='One warmed Nirmala font; wrapped repeated Devanagari; cached and alternating input. No GPU/VRAM, cold-start or whole-app claim.')
    except Exception:
        report['status'] = 'failed'
        raise
    finally:
        path.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('Evidence:', path)


if __name__ == '__main__':
    main()
