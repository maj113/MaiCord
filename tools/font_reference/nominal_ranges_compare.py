"""Compare native range dispatch with recorded Windows observations (QA only)."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

TEXTS = [
    'office AV To', 'ab\u0301cd', 'a\u200db', 'ab\u200db', 'ab\uff9ecd',
    '1\ufe0f\u20e3x', 'ab\U0001f600cd', 'a\U0001f3fbb', 'ab\U0001f3fbcd',
    '\u03b1\u03b2 a \u00e9', 'abc\nxyz', 'a\u05d0b', 'a\u093eb',
    '\u0633\u0644\u0627\u0645 abc', '\u6f22\u5b57 abc', 'a\U000e0100b',
]


def run(args):
    return subprocess.run([str(x) for x in args], check=True, capture_output=True,
                          text=True).stdout


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('root', type=Path)
    p.add_argument('--inventory', type=Path, required=True)
    p.add_argument('--config', choices=['Release', 'Debug'], required=True)
    p.add_argument('--reuse', type=Path, help='Reuse frozen reference results')
    a = p.parse_args()
    native = a.root/'MaiCordRunPlanTests.exe'
    reference = a.root/'bin/Release/MaiCordFeaturePlanReference.exe'
    inventory = json.loads(a.inventory.read_text())
    previous = json.loads(a.reuse.read_text()) if a.reuse else None
    rows, differences = [], []
    for item in inventory:
        font = item['font']
        path = Path(font['path'])
        assert hashlib.sha256(path.read_bytes()).hexdigest() == font['sha256'], path
        weights = [0, 400, 600] if path.stem.lower() == 'bahnschrift' else [0]
        for weight in weights:
            for text in TEXTS:
                scalars = ','.join(f'{ord(c):x}' for c in text)
                if previous is None:
                    args = [reference, path, scalars, 'default', '1148846080',
                            '0', '0', 'en-US', 'ranges']
                    if weight:
                        args.append(str(weight))
                    observed = json.loads(run(args))
                else:
                    row = previous[len(rows)]
                    assert (row['font'], row['weight'], row['text']) == (font, weight, text)
                    observed = row['reference']
                assert observed['hresult'] == 0
                output = run([native, path, weight, scalars])
                actual = json.loads(output.splitlines()[0].removeprefix('ranges='))
                row = dict(font=font, weight=weight, text=text,
                           reference=observed, native=actual)
                rows.append(row)
                if actual != observed['ranges']:
                    differences.append(row)
    if previous is not None:
        assert len(previous) == len(rows)
    suffix = a.config.lower()
    (a.root/f'app-ranges-{suffix}.json').write_text(json.dumps(rows, indent=2))
    (a.root/f'app-differences-{suffix}.json').write_text(json.dumps(differences, indent=2))
    print(f'{a.config}: {len(rows)} comparisons; {len(differences)} differences')
    if differences:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
