"""Refresh prefix profiles and mixed-range observations with a QA-only oracle.

The root must contain neutral-ranges.json with the selected scalar corpus.
This replays that corpus; it does not derive expectations from native code.
"""
from pathlib import Path
import argparse
import json
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('root', type=Path)
    p.add_argument('--font', type=Path, required=True)
    a = p.parse_args()
    exe = a.root/'bin/Release/MaiCordFeaturePlanReference.exe'

    def observe(scalars, mode):
        result = subprocess.run([str(exe), str(a.font),
            ','.join(f'{cp:x}' for cp in scalars), 'default', '1148846080',
            '0', '0', 'en-US', mode], check=True, capture_output=True, text=True)
        observed = json.loads(result.stdout)
        assert observed['hresult'] == 0
        return observed

    for name, mode in [('profile.json', 'range-profile'),
                       ('interior.json', 'range-interior')]:
        observed = observe([97], mode)
        previous = json.loads((a.root/name).read_text())
        assert previous == observed, f'Oracle profile changed: {name}'
        (a.root/name).write_text(json.dumps(observed))
    cases = json.loads((a.root/'neutral-ranges.json').read_text())
    for case in cases:
        observed = observe(case['scalars'], 'ranges')
        assert observed == case['observed'], case['scalars']
        case['observed'] = observed
    (a.root/'neutral-ranges.json').write_text(json.dumps(cases, indent=2))
    print(f'Recaptured both Unicode prefix profiles and {len(cases)} mixed cases')


if __name__ == '__main__':
    main()
