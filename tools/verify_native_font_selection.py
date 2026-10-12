"""Check lazy font lookup, context lifetime and recovery without a catalog cache."""
import argparse
import json
import os
from pathlib import Path
import threading
from verify_native_text import run_check
from benchmark_native_text_performance import identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=Path('D:/MaiCord-build/Release/MaiCordNativeAppLineTests.exe'))
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    report = dict(status='running', executable=identity(args.exe), checks=[])
    try:
        for flag in ['font-selection-check', 'font-context-layouts', 'request-recovery']:
            data = out / flag
            data.mkdir()
            env = os.environ.copy()
            env['LOCALAPPDATA'] = str(data)
            report['checks'].append(run_check(flag, [str(args.exe.resolve()), '--' + flag], root, out, 5, threading.Event(), env=env))
            if (data / 'MaiCord/native-fonts-v1.bin').exists():
                raise ValueError('Font lookup unexpectedly persisted a global catalog')
        if identity(args.exe) != report['executable']:
            raise ValueError('Executable changed during verification')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
    (out / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(report['status'], out / 'verification.json')
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
