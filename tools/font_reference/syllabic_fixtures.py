"""Re-derive the syllabic shaper fixtures (tests/generated/syllabic_shape_fixture.inc) from HarfBuzz.

Two kinds of expectation, both from HarfBuzz (the version is recorded in the fixture):

* Cluster-start corpora for Sinhala, Khmer, Myanmar and Tibetan (sfnt_syllabic_boundaries). Each corpus holds
  the vectors already in the fixture, words from optional corpus files (--words script=path, one word per
  whitespace-separated token) and seeded random sequences of the script's characters, joiners, CGJ, a variation
  selector, the dotted circle, NBSP and digits. HarfBuzz does not expose syllables or clusters at cluster level
  characters, but it flags every glyph after the first of a syllable or grapheme unsafe to break; in a font that
  maps every candidate character and has no layout tables nothing else sets the flag, so the unflagged scalars
  are the cluster starts. Characters the Python Unicode database leaves unassigned may be newer than the
  engine's Unicode data and are left out. Vectors in DIVERGENT stay in the corpus, pinned as divergent: the
  test requires them to keep differing until the engine is fixed and the pin removed.

* Shaping of sample strings on the Windows system fonts the owned tests open (glyphs, advances, offsets in
  design units with em = units per em, the character-glyph map), with the font's head checkSumAdjustment so
  the test only compares glyph identity against the font HarfBuzz shaped. The default-shaper choice (HarfBuzz's
  hb_ot_shaper_categorize) is recorded per font.

Requires uharfbuzz and fontTools. Usage:
    python tools/font_reference/syllabic_fixtures.py [--fuzz 1500] [--fonts C:/Windows/Fonts] [--words khmer=path ...]
"""
import argparse
import random
import re
import tempfile
import unicodedata
from pathlib import Path

import uharfbuzz as hb
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'tests' / 'generated' / 'syllabic_shape_fixture.inc'
# Name (SfntSyllabicScript order), HarfBuzz script, OpenType tags, candidate ranges, system font, fallback font.
SCRIPTS = [
    ('sinhala', 'Sinh', ['sinh'], [(0xd80, 0xe00), (0x111e0, 0x11200)], 'Nirmala.ttc', 'segoeui.ttf'),
    ('khmer', 'Khmr', ['khmr'], [(0x1780, 0x1800), (0x19e0, 0x1a00)], 'LeelawUI.ttf', 'segoeui.ttf'),
    ('myanmar', 'Mymr', ['mym2', 'mymr'], [(0x1000, 0x10a0), (0xa9e0, 0xaa00), (0xaa60, 0xaa80), (0x116d0, 0x116e4)],
     'mmrtext.ttf', 'segoeui.ttf'),
    ('tibetan', 'Tibt', ['tibt'], [(0xf00, 0x1000)], 'himalaya.ttf', 'segoeui.ttf'),
]
SHARED = [0x20, 0x30, 0x31, 0xa0, 0x34f, 0x200b, 0x200c, 0x200d, 0x2060, 0x25cc, 0xfe00]
# Sample strings per script: real words and the HarfBuzz behaviours the engine reproduces (broken clusters,
# split and composed vowels, joiners, CGJ, a sign after a space).
SAMPLES = {
    'sinhala': ['ශ්‍රී', 'ලංකාව', 'ක්‍ෂේත්‍රය', 'කොළඹ', 'ර්‍ය', 'ේ', 'අා', 'කෝ', 'ක\u034fා', 'ක ා'],
    'khmer': ['ក្រុម', 'ភាសាខ្មែរ', 'ស្ត្រី', 'កើ', 'ើ', 'ក\u17b4\u17b5', 'ក្\u200cស', 'ឞ៊ុ', 'ក\u034fៀ', 'ក ុ'],
    'myanmar': ['မြန်မာ', 'မြို့', 'င်္ဂ', 'သီဟိုဠ်', 'ကျွန်ုပ်', 'ေ', 'က\u200dေ', 'ပ္ပ', 'က\u034fု', 'က ု'],
    'tibetan': ['བོད་ཡིག', 'སྐད', 'བསྒྲུབས', 'ཀྵ', 'ཱི', '\u0f71\u0f72', 'ཨོཾ', 'ཀ\u034fི', 'ཀ ི', 'ྐ'],
}
FALLBACK_SAMPLES = ['\u034f\u034f', '\u200d\u200c']
# Vectors where the engine knowingly differs from HarfBuzz, per script.
DIVERGENT = {'sinhala': [], 'khmer': [], 'myanmar': [], 'tibetan': []}


def oracle_font(directory):
    codepoints = sorted({u for _, _, _, ranges, _, _ in SCRIPTS for first, end in ranges for u in range(first, end)} |
                        set(range(0x20, 0x7f)) | set(range(0xa0, 0x100)) | set(SHARED) | set(range(0x2000, 0x2070)))
    names = ['.notdef'] + [f'u{u:04X}' for u in codepoints]
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(names)
    builder.setupCharacterMap({u: f'u{u:04X}' for u in codepoints})
    pen = TTGlyphPen(None)
    pen.moveTo((0, 0))
    pen.lineTo((100, 0))
    pen.lineTo((100, 100))
    pen.closePath()
    glyph = pen.glyph()
    builder.setupGlyf({name: glyph for name in names})
    builder.setupHorizontalMetrics({name: (500, 0) for name in names})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable({'familyName': 'SyllabicClusterOracle', 'styleName': 'Regular'})
    builder.setupOS2()
    builder.setupPost()
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
    for info in buffer.glyph_infos:
        if not info.flags & hb.GlyphFlags.UNSAFE_TO_BREAK:
            result[info.cluster] = 1
    return result


def existing_vectors(name):
    if not FIXTURE.exists():
        return []
    match = re.search(rf'constexpr u32 {name}_clusters\[\]\{{([^}}]*)\}}', FIXTURE.read_text(encoding='utf-8'))
    vectors, current = [], []
    for word in (int(token, 16) for token in (re.findall(r'0x[0-9a-f]+', match.group(1)) if match else [])):
        current.append(word & 0x1fffff)
        if word & 0x40000000:
            vectors.append(tuple(current))
            current = []
    return vectors


def assigned(u):
    return unicodedata.category(chr(u)) != 'Cn'


def fuzz(ranges, count, seed):
    alphabet = [u for first, end in ranges for u in range(first, end) if assigned(u)] + SHARED
    generator = random.Random(seed)
    return [tuple(generator.choice(alphabet) for _ in range(generator.randint(1, 10))) for _ in range(count)]


def words(path, ranges):
    vectors = []
    for token in Path(path).read_text(encoding='utf-8').split():
        if any(first <= ord(c) < end for c in token for first, end in ranges) and len(token) <= 64:
            vectors.append(tuple(ord(c) for c in token))
    return vectors


def units(text):
    return [i for i, c in enumerate(text) for _ in range(2 if ord(c) > 0xffff else 1)]


def shape(path, script, text):
    blob = hb.Blob.from_file_path(str(path))
    face = hb.Face(blob, 0)
    font = hb.Font(face)
    buffer = hb.Buffer()
    buffer.add_str(text)
    buffer.direction = 'ltr'
    buffer.script = script
    buffer.language = 'en'
    hb.shape(font, buffer, {})
    # UTF-16 offsets of each scalar; clusters are scalar indices from add_str.
    offsets, unit = [], 0
    for c in text:
        offsets.append(unit)
        unit += 2 if ord(c) > 0xffff else 1
    glyphs = [(info.codepoint, pos.x_advance, pos.x_offset, pos.y_offset)
              for info, pos in zip(buffer.glyph_infos, buffer.glyph_positions)]
    clusters = [offsets[info.cluster] for info in buffer.glyph_infos]
    starts_sorted = sorted(set(clusters))
    characters = []
    for u in range(unit):
        cluster = max(c for c in starts_sorted if c <= u)
        characters.append(min(i for i, c in enumerate(clusters) if c == cluster))
    return glyphs, characters


def default_shaper(path, name, tags):
    font = TTFont(path, fontNumber=0, lazy=True)
    records = [record.ScriptTag for record in font['GSUB'].table.ScriptList.ScriptRecord] if 'GSUB' in font else []
    chosen = next((tag for tag in tags + ['DFLT', 'dflt', 'latn'] if tag in records), None)
    if name == 'khmer':
        return False
    if name == 'myanmar':
        return chosen in ('DFLT', 'latn', 'mymr')
    return chosen in ('DFLT', 'latn')


def checksum(path):
    return TTFont(path, fontNumber=0, lazy=True)['head'].checkSumAdjustment


def utf8_literal(text):
    return '"' + ''.join(f'\\x{b:02x}' for b in text.encode('utf-8')) + '"'


def sample_lines(prefix, path, script, texts):
    lines = []
    for index, text in enumerate(texts):
        glyphs, characters = shape(path, script, text)
        flat = ','.join(f'{g},{a},{x},{y}' for g, a, x, y in glyphs)
        lines.append(f'constexpr i32 {prefix}_{index}_glyphs[]{{{flat}}};')
        lines.append(f'constexpr u32 {prefix}_{index}_characters[]{{{",".join(map(str, characters))}}};')
    entries = ','.join(f'{{{utf8_literal(text)},{len(shape(path, script, text)[0])},{len(units(text))},{prefix}_{index}_glyphs,'
                       f'{prefix}_{index}_characters}}' for index, text in enumerate(texts))
    lines.append(f'constexpr SyllabicSample {prefix}[]{{{entries}}};')
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--fuzz', type=int, default=1500, help='random vectors added per script')
    parser.add_argument('--fonts', default='C:/Windows/Fonts', help='directory of the Windows system fonts')
    parser.add_argument('--words', action='append', default=[], help='script=path of a word corpus')
    args = parser.parse_args()
    corpora = dict(entry.split('=', 1) for entry in args.words)
    fonts = Path(args.fonts)
    lines = [f'// Syllabic shaper expectations from HarfBuzz {hb.version_string()}. Generated by',
             '// tools/font_reference/syllabic_fixtures.py; no font bytes. Cluster corpora: scalar | cluster start << 31 |',
             '// vector end << 30 | pinned divergent << 29 (on the last scalar). Samples: gid, advance, x and y offset per',
             '// glyph in design units, then the UTF-16 character-glyph map.',
             'struct SyllabicSample {const char* text;u32 glyph_count,unit_count;const i32* glyphs;const u32* characters;};']
    with tempfile.TemporaryDirectory() as directory:
        oracle = oracle_font(directory)
        for seed, (name, script, tags, ranges, system, fallback) in enumerate(SCRIPTS):
            vectors = existing_vectors(name) + [tuple(ord(c) for c in text) for text in SAMPLES[name]]
            if name in corpora:
                vectors += words(corpora[name], ranges)
            vectors += fuzz(ranges, args.fuzz, 4100 + seed)
            pinned = {tuple(v) for v in DIVERGENT[name]}
            seen, entries, cases = set(), [], 0
            for vector in vectors:
                if vector in seen or not 0 < len(vector) <= 256 or not all(assigned(u) for u in vector):
                    continue
                seen.add(vector)
                for i, (scalar, start) in enumerate(zip(vector, starts(oracle, script, vector))):
                    last = i + 1 == len(vector)
                    entries.append(scalar | start << 31 | int(last) << 30 | int(last and vector in pinned) << 29)
                cases += 1
            body = ''.join(f'0x{word:x},' + ('\n' if i % 12 == 11 else '') for i, word in enumerate(entries))
            lines.append(f'constexpr u32 {name}_cluster_cases={cases};')
            lines.append(f'constexpr u32 {name}_clusters[]{{\n{body}}};')
            lines += sample_lines(f'{name}_samples', fonts / system, script, SAMPLES[name])
            lines.append(f'constexpr u32 {name}_samples_checksum={checksum(fonts / system)}u;')
            lines += sample_lines(f'{name}_fallback_samples', fonts / fallback, script, FALLBACK_SAMPLES)
            lines.append(f'constexpr u32 {name}_fallback_checksum={checksum(fonts / fallback)}u;')
            lines.append(f'constexpr bool {name}_fallback_default_shaper={str(default_shaper(fonts / fallback, name, tags)).lower()};')
            print(name, cases, 'vectors')
    FIXTURE.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
