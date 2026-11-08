"""Re-derive the Indic syllable boundary corpora (tests/generated/<script>_input_fixture.{bin,inc}) from HarfBuzz.

Each corpus is a list of scalar vectors: the vectors already in the corpus plus seeded random sequences of
the script's characters, joiners, dotted circle, NBSP and Vedic signs. The expected boundaries are
HarfBuzz's Indic syllables. HarfBuzz does not expose syllables, so the generator builds a font that maps
every candidate character and whose per-syllable 'pres' lookup substitutes each glyph that has a
predecessor in its syllable: after shaping, each unsubstituted glyph starts a syllable, and the syllable
starts at its lowest cluster (scalar index, cluster level characters).

Two kinds of vector are left out. A character the Python Unicode database leaves unassigned may be newer
than the engine's Unicode data, which HarfBuzz already classifies. HarfBuzz numbers syllables modulo 16
and the context lookup skips joiners, so a vector of more than 14 syllables can alias two of them.

Requires uharfbuzz (records the HarfBuzz version) and fontTools.
Usage: python tools/indic_boundaries.py [--fuzz 3000]
Then reconfigure: the test registry hashes the .bin resources.
"""
import argparse
import hashlib
import random
import struct
import tempfile
import unicodedata
from pathlib import Path

import uharfbuzz as hb
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / 'tests' / 'generated'
# Script, OpenType tag, HarfBuzz script, block, resource ID.
SCRIPTS = [('bengali', 'bng2', 'Beng', 0x980, 1001), ('gujarati', 'gjr2', 'Gujr', 0xa80, 1002),
           ('gurmukhi', 'gur2', 'Guru', 0xa00, 1003), ('odia', 'ory2', 'Orya', 0xb00, 1004),
           ('telugu', 'tel2', 'Telu', 0xc00, 1005), ('kannada', 'knd2', 'Knda', 0xc80, 1006),
           ('malayalam', 'mlm2', 'Mlym', 0xd00, 1007), ('devanagari', 'dev2', 'Deva', 0x900, 1008),
           ('tamil', 'tml2', 'Taml', 0xb80, 1009)]
RANGES = [(0x20, 0x7f), (0xa0, 0x100), (0x900, 0xd80), (0x1cd0, 0x1d00), (0x2000, 0x20a0), (0xa8e0, 0xa900), (0x25cc, 0x25cd)]


def oracle_font(directory):
    codepoints = [u for first, end in RANGES for u in range(first, end)]
    names = ['.notdef'] + [f'u{u:04X}' for u in codepoints]
    marked = [name + '.m' for name in names]
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(names + marked)
    builder.setupCharacterMap({u: f'u{u:04X}' for u in codepoints})
    pen = TTGlyphPen(None)
    pen.moveTo((0, 0))
    pen.lineTo((100, 0))
    pen.lineTo((100, 100))
    pen.closePath()
    glyph = pen.glyph()
    builder.setupGlyf({name: glyph for name in names + marked})
    builder.setupHorizontalMetrics({name: (500, 0) for name in names + marked})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable({'familyName': 'IndicSyllableOracle', 'styleName': 'Regular'})
    builder.setupOS2()
    builder.setupPost()
    features = ''.join(f'languagesystem {tag} dflt;\n' for _, tag, _, _, _ in SCRIPTS)
    features += '@ALL = [' + ' '.join(names[1:]) + '];\n@ALLM = [' + ' '.join(marked[1:]) + '];\n'
    features += 'lookup MARK { sub @ALL by @ALLM; } MARK;\n'
    features += "feature pres { sub [@ALL @ALLM] @ALL' lookup MARK; } pres;\n"
    addOpenTypeFeaturesFromString(builder.font, features)
    path = Path(directory) / 'oracle.ttf'
    builder.save(path)
    return hb.Font(hb.Face(hb.Blob.from_file_path(str(path))))


def starts(font, script, scalars):
    buffer = hb.Buffer()
    buffer.add_codepoints(list(scalars))
    buffer.direction = 'ltr'
    buffer.script = script
    buffer.cluster_level = hb.BufferClusterLevel.CHARACTERS
    hb.shape(font, buffer, {})
    result = [0] * len(scalars)
    group = None
    for info in buffer.glyph_infos:
        if not font.glyph_to_string(info.codepoint).endswith('.m'):
            if group is not None:
                result[group] = 1
            group = info.cluster
        else:
            group = min(group, info.cluster)
    if group is not None:
        result[group] = 1
    return result


def existing_vectors(path):
    if not path.exists():
        return []
    vectors, current = [], []
    for (word,) in struct.iter_unpack('<I', path.read_bytes()):
        current.append(word & 0x1fffff)
        if word & 0x40000000:
            vectors.append(tuple(current))
            current = []
    return vectors


def fuzz(block, count, seed):
    alphabet = [u for u in range(block, block + 128) if unicodedata.category(chr(u)) != 'Cn']
    alphabet += [0x200c, 0x200d, 0x25cc, 0xa0, 0x30, 0x951, 0x952, 0x953, 0x1cd0, 0x1cf2, 0xa8f2, block + 0x4d, block + 0x4d,
                 block + 0x30]
    generator = random.Random(seed)
    return [tuple(generator.choice(alphabet) for _ in range(generator.randint(1, 12))) for _ in range(count)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--fuzz', type=int, default=3000, help='random vectors added per script')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        font = oracle_font(directory)
        for name, _, script, block, resource in SCRIPTS:
            binary = GENERATED / f'{name}_input_fixture.bin'
            vectors = existing_vectors(binary) + fuzz(block, args.fuzz, resource)
            seen, words, cases = set(), [], 0
            for vector in vectors:
                if vector in seen or not 0 < len(vector) <= 256 or any(unicodedata.category(chr(u)) == 'Cn' for u in vector):
                    continue
                seen.add(vector)
                boundaries = starts(font, script, vector)
                if sum(boundaries) > 14:
                    continue
                for i, (scalar, start) in enumerate(zip(vector, boundaries)):
                    words.append(scalar | (start << 31) | (int(i + 1 == len(vector)) << 30))
                cases += 1
            data = struct.pack(f'<{len(words)}I', *words)
            binary.write_bytes(data)
            lines = ['// Scalar vectors with HarfBuzz ' + hb.version_string() + ' Indic syllable starts (bit 31; bit 30 ends a',
                     '// vector). Generated by tools/indic_boundaries.py; no font bytes.',
                     f'constexpr u32 {name}_input_fixture_cases={cases};',
                     f'// Resource SHA-256: {hashlib.sha256(data).hexdigest()}',
                     f'constexpr u32 {name}_input_fixture_words={len(words)};',
                     f'constexpr u16 {name}_input_fixture_resource={resource};']
            (GENERATED / f'{name}_input_fixture.inc').write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
            print(name, cases, 'vectors', len(words), 'scalars')


if __name__ == '__main__':
    main()
