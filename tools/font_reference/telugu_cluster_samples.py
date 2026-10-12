"""Extended Telugu scanner inputs and a QA-only font with blwf disabled."""
import argparse
import itertools
import json
from pathlib import Path
import random


def extended():
    cases = {}
    def add(s): cases.setdefault(tuple(s), None)
    consonants = (*range(0xc15, 0xc29), *range(0xc2a, 0xc3a), 0xc58, 0xc59, 0xc5a, 0xc5d)
    vowels = (0xc05, 0xc06, 0xc07, 0xc08, 0xc09, 0xc0a, 0xc0b, 0xc0c,
              0xc0e, 0xc0f, 0xc10, 0xc12, 0xc13, 0xc14, 0xc60, 0xc61)
    marks = (0xc00, 0xc01, 0xc02, 0xc03, 0xc04, 0xc3c, 0xc3e, 0xc3f,
             0xc40, 0xc41, 0xc42, 0xc43, 0xc44, 0xc46, 0xc47, 0xc48,
             0xc4a, 0xc4b, 0xc4c, 0xc4d, 0xc55, 0xc56, 0xc62, 0xc63,
             0x951, 0x952, 0x953, 0x954, 0x1cd0, 0x1ce2, 0xa8e0)
    for v in vowels:
        for link in ((0xc4d,), (0x200d,), (0xc4d, 0x200d),
                     (0x200d, 0xc4d), (0xc4d, 0x200d, 0xc3c)):
            for c in (0xc31, 0xc34, 0xc5d):
                for m in marks: add((v, *link, c, m))
    for c in (0xc31, 0xc34, 0xc5d):
        for m, n in itertools.product(marks + (0x200c, 0x200d, 0xc15), repeat=2):
            add((c, m, n))
    for v in (0xc0b, 0xc0c, 0xc12):
        for c, d in itertools.product(consonants, repeat=2):
            add((v, 0xc4d, c, 0xc4d, d, 0xc3e if v != 0xc12 else 0xc55))
    rng = random.Random(928203)
    alphabet = consonants + vowels + marks + (0x200c, 0x200d, 0xa0, 0x25cc)
    for _ in range(4096): add(rng.choices(alphabet, k=rng.randrange(1, 50)))
    return list(cases)


def targeted():
    cases = []
    for base in (0xc05, 0xc0b, 0xc15):
        for mark in (0xc00, 0xc01, 0xc02, 0xc03, 0xc04, 0x951, 0x952, 0x953, 0x954):
            cases.append((base, 0xc4d, mark, 0xc15))
    for link in ((0x200d,), (0xc4d,), (0xc4d, 0x200d)):
        for tail in ((0x200c, 0xc15), (0x200d, 0xc04), (0xc4d, 0xc30, 0xc3e)):
            cases.append((0xc0b, *link, 0xc15, *tail))
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--font', type=Path, help='Optional standalone reference font to derive without-blwf.ttf')
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    outputs = {args.output_dir / 'extended-cases.json': extended(),
               args.output_dir / 'targeted-cases.json': targeted()}
    font_output = args.output_dir / 'without-blwf.ttf'
    if any(p.exists() for p in outputs) or (args.font and font_output.exists()):
        parser.error('Preserve existing observation inputs; choose a new evidence directory')
    for path, cases in outputs.items():
        path.write_text(json.dumps(cases), encoding='utf-8')
    if args.font:
        from fontTools.ttLib import TTFont
        with TTFont(args.font, recalcTimestamp=False) as font:
            for record in font['GSUB'].table.FeatureList.FeatureRecord:
                if record.FeatureTag == 'blwf':
                    record.Feature.LookupListIndex = []
                    record.Feature.LookupCount = 0
            font.save(font_output)


if __name__ == '__main__':
    main()
