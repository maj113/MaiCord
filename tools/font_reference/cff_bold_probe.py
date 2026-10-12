"""Bounded CFF bold investigation. Default: first 64 glyphs of each input font.

--all-glyphs explicitly expands coverage, retaining a two-second process cap
and 64-glyph reference shards. A native unsupported result is recorded as a
coverage gap, not converted into a passing native comparison.
"""
import argparse
import contextlib
import hashlib
import io
import json
import struct
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from verify_native_text import CheckFailure, run_check


def glyph_count(path):
    data = path.read_bytes()
    count = struct.unpack_from('>H', data, 4)[0]
    for at in range(12, 12+16*count, 16):
        if data[at:at+4] == b'maxp':
            offset = struct.unpack_from('>I', data, at+8)[0]
            return struct.unpack_from('>H', data, offset+4)[0]
    raise ValueError(f'{path}: missing maxp')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fonts', nargs='+', type=Path)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--all-glyphs', action='store_true')
    parser.add_argument('--experiment', action='store_true', help='Use the optional native CFF bold entry.')
    parser.add_argument('--oblique', action='store_true', help='Compare simulations 2 and 3.')
    args = parser.parse_args()
    args.output = args.output.resolve()
    repo = Path(__file__).resolve().parents[2]
    args.output.mkdir(parents=True, exist_ok=True)
    executions, comparisons = [], []
    try:
        for index, font in enumerate(args.fonts):
            count = glyph_count(font)
            if not args.all_glyphs:
                count = min(count, 64)
            for simulation in ((2, 3) if args.oblique else (0, 1)):
                logs = []
                for first in range(0, count, 64):
                    name = f'{index}-{font.stem}-{simulation}-{first}'
                    command = [str(args.exe.resolve()), str(font.resolve()),
                               f'--simulate={simulation}',
                               f'--glyphs={first},{min(64,count-first)}',
                               *(['--cff-experiment'] if args.experiment else []), '--geometry-dump']
                    captured = io.StringIO()
                    try:
                        with contextlib.redirect_stdout(captured):
                            row = run_check(name, command, repo, args.output, 2, threading.Event())
                    except CheckFailure as error:
                        row = error.row
                        executions.append(row)
                        if not (simulation & 1) or row['exit'] != 6:
                            print(captured.getvalue())
                            raise
                        row['native_comparison_gap'] = True
                    else:
                        executions.append(row)
                    logs.append(row['log'])
                name = f'{index}-{font.stem}-{simulation}-model'
                report = args.output / (name+'.json')
                command = [sys.executable, str(Path(__file__).with_name('cff_bold_model.py')),
                           str(font.resolve()), *logs, *(['--bold'] if simulation & 1 else []),
                           *(['--oblique'] if simulation & 2 else []),
                           '--output', str(report)]
                try:
                    row = run_check(name, command, repo, args.output, 2, threading.Event())
                except CheckFailure as error:
                    executions.append(error.row)
                    raise
                executions.append(row)
                result = json.loads(report.read_text())
                assert result['compared'] == count
                comparisons.append(dict(font=str(font), sha256=hashlib.sha256(font.read_bytes()).hexdigest(),
                    simulation=simulation, compared=count, exact=result['exact'], report=str(report)))
                print(f'{font.stem}: simulation {simulation}, {result["exact"]}/{count} model exact')
    finally:
        (args.output/'manifest.json').write_text(json.dumps(dict(
            executable=str(args.exe), sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),
            all_glyphs=args.all_glyphs, oblique=args.oblique, experiment=args.experiment,
            executions=executions, comparisons=comparisons), indent=2))


if __name__ == '__main__':
    main()
