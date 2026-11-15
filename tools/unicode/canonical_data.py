"""Canonical decompositions and primary compositions for HarfBuzz-style normalization.

Writes src/text/unicode/generated/canonical_data.inc from the pinned Unicode 17.0.0 UCD archive:

    python -I tools/unicode/canonical_data.py --ucd <UCD.zip> [--output <file>]

The one-step canonical decompositions (UnicodeData.txt field 5 without a <tag>) are sorted by code point, with a
second of 0 for a singleton. A BMP code point decomposes inside the BMP: canonical_bmp_code_points holds it and
canonical_bmp_pairs its first << 16 | second at the same index. canonical_supplementary_decompositions holds the
others, packed as code point << 42 | first << 21 | second. Hangul syllables decompose algorithmically and CJK
compatibility ideographs (F900..FAFF, 2F800..2FA1F) are left out: font fallback selects a face that maps the
ideograph itself. canonical_bmp_compositions and canonical_supplementary_compositions list, sorted by (first,
second), the indices in each table of the pairs that are not Full_Composition_Exclusion. The first of a BMP
composite is in the BMP and the first of a supplementary composite is not, so a composition searches one table.
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
    bmp = [row for row in decompositions if row[0] < 0x10000]
    supplementary = decompositions[len(bmp):]
    assert all(v < 0x10000 for row in bmp for v in row) and all(row[0] >= 0x10000 for row in supplementary)
    assert len(supplementary) < 256 and all(v < 1 << 21 for row in supplementary for v in row)
    bmp_compositions = [i for i in compositions if i < len(bmp)]
    supplementary_compositions = [i - len(bmp) for i in compositions if i >= len(bmp)]
    # By (first, second) the BMP composites come first, so each list keeps the order of its table's search.
    assert compositions == bmp_compositions + [i + len(bmp) for i in supplementary_compositions]
    assert all(supplementary[i][1] >= 0x10000 for i in supplementary_compositions)
    packed = [cp << 42 | first << 21 | second for cp, first, second in supplementary]
    rows = lambda values, width, per: '\n'.join('    ' + ','.join(f'0x{v:0{width}x}' for v in values[i:i + per]) + ','
                                                for i in range(0, len(values), per))
    return ('// Unicode 17.0.0 canonical decompositions (one step) and primary compositions; tools/unicode/canonical_data.py.\n'
            '// Unicode-derived data: Unicode License v3. Copyright (c) 1991-2025 Unicode, Inc. See assets/licenses/Unicode-3.0.txt.\n'
            '// By code point, second 0 for a singleton. No Hangul syllables or CJK compatibility ideographs.\n'
            '// The BMP code points: each decomposes inside the BMP, to the pair at its index (first << 16 | second).\n'
            f'constexpr u16 canonical_bmp_code_points[]{{\n{rows([cp for cp, _, _ in bmp], 4, 16)}\n}};\n'
            f'constexpr u32 canonical_bmp_pairs[]{{\n{rows([first << 16 | second for _, first, second in bmp], 8, 10)}\n}};\n'
            '// The other code points, as code point << 42 | first << 21 | second.\n'
            f'constexpr u64 canonical_supplementary_decompositions[]{{\n{rows(packed, 16, 6)}\n}};\n'
            '// Indices of the primary composites in each table, by (first, second). The first of a BMP composite is in\n'
            '// the BMP, the first of a supplementary composite is not.\n'
            f'constexpr u16 canonical_bmp_compositions[]{{\n{rows(bmp_compositions, 4, 16)}\n}};\n'
            f'constexpr u8 canonical_supplementary_compositions[]{{\n{rows(supplementary_compositions, 2, 16)}\n}};\n')


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
    bmp = sum(row[0] < 0x10000 for row in decompositions)
    bmp_compositions = sum(i < bmp for i in compositions)
    print(f'{len(decompositions)} decompositions, {len(compositions)} compositions, '
          f'{bmp * 6 + (len(decompositions) - bmp) * 8 + bmp_compositions * 2 + len(compositions) - bmp_compositions} bytes')


if __name__ == '__main__':
    main()
