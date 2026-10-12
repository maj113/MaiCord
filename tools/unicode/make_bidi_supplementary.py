"""Transplant official bidi cases to supplementary L/R/ON characters.

Expected results are unchanged official vectors, not computed by our resolver.
These are synthetic cases, not additional official Unicode conformance cases.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

SOURCE_SHA256 = 'a3e6e905ab5afbe318a96df5401d0372a04cd73ef139ab5e3cf0ae241c255488'
# Unicode 17: Latin -> Deseret (L); Hebrew -> Adlam (R); die -> emoji (ON).
# None of these sources/targets is a paired bracket or a formatting control.
REPLACEMENTS = {**{0x61+i: 0x10428+i for i in range(6)},
                **{0x5d0+i: 0x1e900+i for i in range(6)}, 0x2680: 0x1f600}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--character-tests', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if hashlib.sha256(args.character_tests.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError('Official source identity differs')
    records = bytearray()
    cases = supplementary = 0
    for line, text in enumerate(args.character_tests.read_text(encoding='utf-8').splitlines(), 1):
        text = text.split('#')[0].strip()
        if not text:
            continue
        cp, policy, base, levels, visual = text.split(';')[:5]
        cps = [REPLACEMENTS.get(int(x, 16), int(x, 16)) for x in cp.split()]
        levels = [255 if x == 'x' else int(x) for x in levels.split()]
        visual = [int(x) for x in visual.split()]
        assert len(cps) == len(levels)
        records += struct.pack('<6I', len(cps), int(policy), int(base), len(visual), line, 1)
        for cp, level in zip(cps, levels):
            records += struct.pack('<IBB2x', cp, 255, level)
        records += struct.pack(f'<{len(visual)}I', *visual)
        cases += 1
        supplementary += sum(cp > 0xffff for cp in cps)
    data = b'MCBIDI1\0'+struct.pack('<I', cases)+records
    args.out.write_bytes(data)
    result = {'kind': 'synthetic supplementary transplantation', 'cases': cases,
              'supplementary_scalars': supplementary, 'source_sha256': SOURCE_SHA256,
              'corpus_sha256': hashlib.sha256(data).hexdigest(),
              'replacements': {f'U+{k:04X}': f'U+{v:04X}' for k, v in REPLACEMENTS.items()}}
    args.out.with_suffix('.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
