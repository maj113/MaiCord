"""Bounded before/after renderer initialization; does not measure first visible paint."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import statistics
import struct
import threading

from verify_native_text import CheckFailure, run_check


def identity(path):
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--prefix-cache', type=Path, required=True)
    parser.add_argument('--complete-cache', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=7)
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 20:
        parser.error('--repetitions must be between 1 and 20')
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    exes = dict(before=args.before.resolve(), after=args.after.resolve())
    seeds = dict(prefix=args.prefix_cache.resolve(), hit=args.complete_cache.resolve())
    metadata = dict(executables={k: identity(p) for k, p in exes.items()},
                    seeds={k: identity(p) for k, p in seeds.items()},
                    scope='Renderer initialization through return, before scene setup or paint. '
                          'Seven alternating pairs by default. Fresh processes and isolated '
                          'application caches; OS and driver caches are not forcibly cold. '
                          'Candidate intentionally leaves font admission until first text demand. '
                          'Memory includes the complete process and driver; no GPU completion wait.',
                    timeout_seconds=5, repetitions=args.repetitions)
    (root / 'manifest.json').write_text(json.dumps(metadata, indent=2))
    rows = []
    try:
        for state in ('create', 'prefix', 'hit'):
            for rep in range(args.repetitions):
                for label in (('before', 'after') if rep % 2 == 0 else ('after', 'before')):
                    name = f'{state}-{rep}-{label}'
                    directory = root / name
                    cache = directory / 'cache/MaiCord'
                    cache.mkdir(parents=True)
                    if state in seeds:
                        shutil.copy2(seeds[state], cache / 'native-fonts-v1.bin')
                    env = {k: v for k, v in os.environ.items() if not k.startswith('MAICORD_')}
                    env.update(LOCALAPPDATA=str(cache.parent), MAICORD_GPU_METRICS='1',
                               MAICORD_NATIVE_BOOTSTRAP_PROBE='1', MAICORD_BOOTSTRAP_TITLE_PROBE='2',
                               MAICORD_SCENE_MEMORY=str(directory / 'memory.bin'))
                    row = run_check(name, [str(exes[label])], repo, directory, 5,
                                    threading.Event(), env=env)
                    rows.append(row)
                    log = (cache / 'logs/maicord.log').read_text(errors='replace')
                    data = (directory / 'memory.bin').read_bytes()
                    assert data[64:192].decode('utf-16-le').split('\0')[0] == 'renderer-initialized'
                    v = struct.unpack_from('<20Q', data, 192)
                    row.update(state=state, rep=rep, provider=label, width=v[0], height=v[1], dpi=v[2],
                               init_us=int(re.search(r'renderer_init_us=(\d+)', log)[1]),
                               init_cycles=int(re.search(r'renderer_init_cycles=(\d+)', log)[1]),
                               local=v[3], nonlocal_bytes=v[4], working_set=v[6], private=v[7],
                               builds=v[10], dispatches=v[11])
    except CheckFailure as error:
        rows.append(error.row)
        raise
    finally:
        (root / 'samples.json').write_text(json.dumps(rows, indent=2))
    assert len({(r['width'], r['height'], r['dpi']) for r in rows}) == 1
    summary = []
    for state in ('create', 'prefix', 'hit'):
        result = dict(state=state)
        for label in exes:
            selected = [r for r in rows if r['state'] == state and r['provider'] == label]
            result[label] = {key: statistics.median(r[key] for r in selected)
                             for key in ('init_us', 'init_cycles', 'local', 'nonlocal_bytes',
                                         'working_set', 'private', 'builds', 'dispatches')}
        summary.append(result)
    (root / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
