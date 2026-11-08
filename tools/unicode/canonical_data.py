"""Canonical decompositions and primary compositions for HarfBuzz-style normalization.

Writes src/text/unicode/generated/canonical_data.inc from the pinned Unicode 17.0.0 UCD archive:

    python -I tools/unicode/canonical_data.py --ucd <UCD.zip> [--output <file>]

canonical_decompositions holds every one-step canonical decomposition (UnicodeData.txt field 5 without a
<tag>), sorted by code point, packed as code point << 42 | first << 21 | second (0 for a singleton). Hangul
syllables decompose algorithmically and CJK compatibility ideographs (F900..FAFF, 2F800..2FA1F) are left out:
font fallback selects a face that maps the ideograph itself. canonical_compositions lists, sorted by (first,
second), the indices of the pairs that are not Full_Composition_Exclusion.
"""
import argparse
import hashlib
import zipfile
from pathlib import Path

PIN = '2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e'
OUTPUT = Path(__file__).resolve().parents[2] / 'src/text/unicode/generated/canonical_data.inc'


def ranges(text, value):
    out = set()
    for raw in text.splitlines():
        fields = raw.split('#')[0].strip().split(';')
        if len(fields) >= 2 and fields[1].strip() == value:
            span = fields[0].strip().split('..')
            out.update(range(int(span[0], 16), int(span[-1], 16) + 1))
    return out


def derive(unicode_data, normalization):
    excluded = ranges(normalization, 'Full_Composition_Exclusion')
    decompositions = []
    for raw in unicode_data.splitlines():
        fields = raw.split(';')
        cp, mapping = int(fields[0], 16), fields[5].split()
        if not mapping or mapping[0].startswith('<') or 0xF900 <= cp <= 0xFAFF or 0x2F800 <= cp <= 0x2FA1F:
            continue
        parts = [int(v, 16) for v in mapping]
        decompositions.append((cp, parts[0], parts[1] if len(parts) == 2 else 0))
    decompositions.sort()
    compositions = sorted((i for i, (cp, first, second) in enumerate(decompositions) if second and cp not in excluded),
                          key=lambda i: decompositions[i][1:])
    return decompositions, compositions


def render(decompositions, compositions):
    assert len(decompositions) < 65536 and all(v < 1 << 21 for row in decompositions for v in row)
    packed = [cp << 42 | first << 21 | second for cp, first, second in decompositions]
    rows = lambda values, width, per: '\n'.join('    ' + ','.join(f'0x{v:0{width}x}' for v in values[i:i + per]) + ','
                                                for i in range(0, len(values), per))
    return ('// Unicode 17.0.0 canonical decompositions (one step) and primary compositions; tools/unicode/canonical_data.py.\n'
            '// Unicode-derived data: Unicode License v3. Copyright (c) 1991-2025 Unicode, Inc. See assets/licenses/Unicode-3.0.txt.\n'
            '// code point << 42 | first << 21 | second (0: singleton), by code point. No Hangul syllables or CJK\n'
            '// compatibility ideographs.\n'
            f'constexpr u64 canonical_decompositions[]{{\n{rows(packed, 16, 6)}\n}};\n'
            '// Indices of the primary composites in canonical_decompositions, by (first, second).\n'
            f'constexpr u16 canonical_compositions[]{{\n{rows(compositions, 4, 16)}\n}};\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ucd', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    arguments = parser.parse_args()
    data = arguments.ucd.read_bytes()
    if hashlib.sha256(data).hexdigest() != PIN:
        raise SystemExit(f'{arguments.ucd} is not the pinned Unicode 17.0.0 UCD archive')
    with zipfile.ZipFile(arguments.ucd) as archive:
        unicode_data = archive.read('UnicodeData.txt').decode()
        normalization = archive.read('DerivedNormalizationProps.txt').decode()
    decompositions, compositions = derive(unicode_data, normalization)
    arguments.output.write_text(render(decompositions, compositions), encoding='utf-8', newline='\n')
    print(f'{len(decompositions)} decompositions, {len(compositions)} compositions, '
          f'{len(decompositions) * 8 + len(compositions) * 2} bytes')


if __name__ == '__main__':
    main()
