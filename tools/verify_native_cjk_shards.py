"""Run the full CJK draw/editor survey as bounded, individually reproducible shards."""
import argparse
import contextlib
import io
import json
import os
import re
import threading
from pathlib import Path
from benchmark_native_text_performance import identity
from verify_native_text import CheckFailure, run_check


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=Path('D:/MaiCord-build/Release/MaiCordNativeAppLineTests.exe'))
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--mode', choices=('paragraph', 'editor', 'paint', 'both'), default='both')
    parser.add_argument('--sample', type=int, choices=range(20))
    parser.add_argument('--variant', type=int)
    args = parser.parse_args()
    if args.variant is not None and (args.sample is None or args.mode in ('paint', 'both') or not 0 <= args.variant < (288 if args.mode == 'editor' else 576)):
        parser.error('--variant requires one sample, one mode and an in-range variant')
    if args.mode == 'paint' and args.sample is not None and args.sample >= 4:
        parser.error('Paint coverage has four samples (0-3)')
    root = Path(__file__).resolve().parents[1]
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    target = out / 'cjk-shards.json'
    if target.exists():
        parser.error('Preserve prior evidence; use an unused output directory')
    modes = ('paragraph', 'editor') if args.mode == 'both' else (args.mode,)
    samples = range(4 if args.mode == 'paint' else 20) if args.sample is None else (args.sample,)
    binary = identity(args.exe.resolve())
    report = dict(status='running', scope='full' if args.sample is None and args.mode == 'both' else 'focused', executable=binary,
                  fixture=identity(root / 'tests/cjk_paragraph_cases.inc'), timeout_seconds=2, shards=[])
    def save():
        target.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    save()
    for mode in modes:
        for sample in samples:
            env = os.environ.copy()
            for key in ('MAICORD_CJK_SAMPLE', 'MAICORD_CJK_VARIANT', 'MAICORD_HIT_OBSERVE'):
                env.pop(key, None)
            env['MAICORD_CJK_SAMPLE'] = str(sample)
            if args.variant is not None:
                env['MAICORD_CJK_VARIANT'] = str(args.variant)
            with contextlib.redirect_stdout(io.StringIO()):
                try:
                    result = run_check(f'{mode}-{sample}', [str(args.exe.resolve()), f"--cjk-{'editor-paint' if mode == 'paint' else mode}-profile", '--family', 'Segoe UI', '400'], root, out, 2, threading.Event(), env=env)
                except CheckFailure as error:
                    result = error.row
            log = Path(result['log']).read_text(encoding='utf-8')
            counts = re.findall(r'cases=(\d+) native_cases=(\d+) simulated_cases=(\d+) checks=(\d+) (PASS|FAIL) native app lines', log)
            expected = (1728, 1440, 1152, 720)[sample] if mode == 'paint' else 1 if args.variant is not None else 288 if mode == 'editor' else 576
            complete = len(counts) == 1 and int(counts[0][0]) == expected
            passed = complete and result['status'] == 'passed' and counts[0][4] == 'PASS' and int(counts[0][1]) == expected
            report['shards'].append(dict(mode=mode, sample=sample, variant=args.variant, execution=result,
                                         complete=complete, passed=passed, counts=counts, log=identity(Path(result['log']))))
            save()
            print(mode, sample, 'passed' if passed else 'failed', f"{result['seconds']:.3f}s", flush=True)
    if identity(args.exe.resolve()) != binary:
        raise RuntimeError('Executable changed during verification')
    report['status'] = 'passed' if all(s['passed'] for s in report['shards']) else 'failed'
    report['seconds'] = sum(s['execution']['seconds'] for s in report['shards'])
    save()
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
