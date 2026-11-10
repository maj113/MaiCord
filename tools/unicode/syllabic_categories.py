"""Shaping categories of the Khmer, Myanmar and Universal Shaping Engine shapers.

Writes src/text/unicode/generated/syllabic_categories.inc from the pinned Unicode 17.0.0 UCD archive and the
two HarfBuzz 15.0.0 override files of the USE table (src/ms-use/IndicSyllabicCategory-Additional.txt and
IndicPositionalCategory-Additional.txt):

    python -I tools/unicode/syllabic_categories.py --ucd <UCD.zip> --harfbuzz <harfbuzz-15.0.0/src>

The rules are HarfBuzz's generators: gen-indic-table.py for the Khmer and Myanmar categories (Indic syllabic
category, its overrides, dependent vowels split by position) and gen-use-table.py for the USE categories. Each
table covers its shaper's blocks; USE covers USE_BLOCKS, the blocks of the scripts routed to it (add a script's
block there when it moves to the USE engine), the characters of those scripts' smaller blocks (USE_SPARSE), and
the characters of other blocks with a category other than O that can share a run with any script (the Common and
Inherited ones of HarfBuzz's table). Khmer and Myanmar runs hold no characters of the other Indic blocks, which
take X here.
"""
import argparse
import hashlib
import zipfile
from pathlib import Path

PIN = '2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e'
ADDITIONAL = {'IndicSyllabicCategory-Additional.txt': 'b9472e3e72d5fba8cb3f2e0578e73012aaae786db25779de0f1a5a5ab69b84a6',
              'IndicPositionalCategory-Additional.txt': '2baa1c1efe5a5f108c304b1e27d0d97864c806764eb2b0a1bd91db80ae26b5b8'}
OUTPUT = Path(__file__).resolve().parents[2] / 'src/text/unicode/generated/syllabic_categories.inc'

# Enumerator orders of K, M and U (src/text/scripts/syllabic_shape.cpp) and the
# columns of tools/syllable_machines.py.
KHMER = ['X', 'C', 'V', 'H', 'ZWNJ', 'ZWJ', 'PLACEHOLDER', 'DOTTEDCIRCLE', 'Ra', 'VAbv', 'VBlw', 'VPre', 'VPst',
         'Robatic', 'Xgroup', 'Ygroup']
MYANMAR = ['X', 'C', 'IV', 'DB', 'H', 'ZWNJ', 'ZWJ', 'SM', 'A', 'GB', 'DOTTEDCIRCLE', 'Ra', 'CS', 'SMPst', 'VAbv',
           'VBlw', 'VPre', 'VPst', 'As', 'MH', 'MR', 'MW', 'MY', 'PT', 'VS', 'ML']
USE = ['O', 'B', 'N', 'GB', 'CGJ', 'SUB', 'H', 'HN', 'ZWNJ', 'WJ', 'R', 'CS', 'IS', 'Sk', 'G', 'J', 'SB', 'SE', 'HVM',
       'HM', 'HR', 'RK', 'FAbv', 'FBlw', 'FPst', 'MAbv', 'MBlw', 'MPst', 'MPre', 'CMAbv', 'CMBlw', 'VAbv', 'VBlw', 'VPst',
       'VPre', 'VMAbv', 'VMBlw', 'VMPst', 'VMPre', 'SMAbv', 'SMBlw', 'FMAbv', 'FMBlw', 'FMPst']
KHMER_BLOCKS = [(0x1780, 0x17FF)]
MYANMAR_BLOCKS = [(0x1000, 0x109F), (0xA9E0, 0xA9FF), (0xAA60, 0xAA7F), (0x116D0, 0x116FF)]
USE_BLOCKS = [(0x0D80, 0x0DFF), (0x0F00, 0x0FFF)]  # Sinhala, Tibetan
# Other blocks of those scripts, searched like SHARED (their few characters stay out of the arrays).
USE_SPARSE = [(0x111E0, 0x111FF)]  # Sinhala Archaic Numbers
# Ranges searched for characters of other blocks that a run of any script can hold.
SHARED = [(0x0000, 0x00FF), (0x0300, 0x036F), (0x1AB0, 0x1AFF), (0x1DC0, 0x1DFF), (0x2000, 0x20FF), (0x25CC, 0x25FF),
          (0xFE00, 0xFE0F), (0xFE20, 0xFE2F), (0xFEFF, 0xFEFF), (0xE0000, 0xE01EF)]


def spans(text, field=1):
    """{code point: value} of a UCD-style file (ranges, # comments)."""
    out = {}
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(';')]
        lo, _, hi = fields[0].partition('..')
        for cp in range(int(lo, 16), int(hi or lo, 16) + 1):
            out[cp] = fields[field]
    return out


# gen-indic-table.py ---------------------------------------------------------------------------------------------------

INDIC_MAP = {
    'Other': 'X', 'Avagraha': 'Symbol', 'Bindu': 'SM', 'Brahmi_Joining_Number': 'PLACEHOLDER', 'Cantillation_Mark': 'A',
    'Consonant': 'C', 'Consonant_Dead': 'C', 'Consonant_Final': 'CM', 'Consonant_Head_Letter': 'C',
    'Consonant_Initial_Postfixed': 'C', 'Consonant_Killer': 'M', 'Consonant_Medial': 'CM',
    'Consonant_Placeholder': 'PLACEHOLDER', 'Consonant_Preceding_Repha': 'Repha', 'Consonant_Prefixed': 'X',
    'Consonant_Subjoined': 'CM', 'Consonant_Succeeding_Repha': 'CM', 'Consonant_With_Stacker': 'CS',
    'Gemination_Mark': 'SM', 'Invisible_Stacker': 'H', 'Joiner': 'ZWJ', 'Modifying_Letter': 'X', 'Non_Joiner': 'ZWNJ',
    'Nukta': 'N', 'Number': 'PLACEHOLDER', 'Number_Joiner': 'PLACEHOLDER', 'Pure_Killer': 'M', 'Register_Shifter': 'RS',
    'Syllable_Modifier': 'SM', 'Tone_Letter': 'X', 'Tone_Mark': 'N', 'Virama': 'H', 'Visarga': 'SM', 'Vowel': 'V',
    'Vowel_Dependent': 'M', 'Vowel_Independent': 'V'}
INDIC_POSITION = {'Left': 'VPre', 'Top': 'VAbv', 'Bottom': 'VBlw', 'Right': 'VPst', 'Bottom_And_Right': 'VPst',
                  'Left_And_Right': 'VPst', 'Top_And_Bottom': 'VBlw', 'Top_And_Bottom_And_Left': 'VBlw',
                  'Top_And_Bottom_And_Right': 'VPst', 'Top_And_Left': 'VAbv', 'Top_And_Left_And_Right': 'VPst',
                  'Top_And_Right': 'VPst'}
INDIC_ALLOWED = ['Basic Latin', 'Latin-1 Supplement', 'Devanagari', 'Bengali', 'Gurmukhi', 'Gujarati', 'Oriya', 'Tamil',
                 'Telugu', 'Kannada', 'Malayalam', 'Myanmar', 'Khmer', 'Vedic Extensions', 'General Punctuation',
                 'Superscripts and Subscripts', 'Devanagari Extended', 'Myanmar Extended-B', 'Myanmar Extended-A',
                 'Myanmar Extended-C']
INDIC_OVERRIDES = {
    **{cp: 'VS' for cp in range(0xFE00, 0xFE10)},
    0x2015: 'PLACEHOLDER', 0x2022: 'PLACEHOLDER', 0x25FB: 'PLACEHOLDER', 0x25FC: 'PLACEHOLDER', 0x25FD: 'PLACEHOLDER',
    0x25FE: 'PLACEHOLDER', 0x25CC: 'DOTTEDCIRCLE',
    0x179A: 'Ra', 0x17CC: 'Robatic', 0x17C9: 'Robatic', 0x17CA: 'Robatic', 0x17C6: 'Xgroup', 0x17CB: 'Xgroup',
    0x17CD: 'Xgroup', 0x17CE: 'Xgroup', 0x17CF: 'Xgroup', 0x17D0: 'Xgroup', 0x17D1: 'Xgroup', 0x17C7: 'Ygroup',
    0x17C8: 'Ygroup', 0x17DD: 'Ygroup', 0x17D3: 'Ygroup', 0x17D9: 'PLACEHOLDER',
    0x104E: 'C', 0x1004: 'Ra', 0x101B: 'Ra', 0x105A: 'Ra', 0x1032: 'A', 0x1036: 'A', 0x103A: 'As', 0x103E: 'MH',
    0x1060: 'ML', 0x103C: 'MR', 0x103D: 'MW', 0x1082: 'MW', 0x103B: 'MY', 0x105E: 'MY', 0x105F: 'MY', 0x1063: 'PT',
    0x1064: 'PT', 0x1069: 'PT', 0x106A: 'PT', 0x106B: 'PT', 0x106C: 'PT', 0x106D: 'PT', 0xAA7B: 'PT', 0x1038: 'SM',
    0x1087: 'SM', 0x1088: 'SM', 0x1089: 'SM', 0x108A: 'SM', 0x108B: 'SM', 0x108C: 'SM', 0x108D: 'SM', 0x108F: 'SM',
    0x109A: 'SM', 0x109B: 'SM', 0x109C: 'SM', 0x104A: 'PLACEHOLDER'}


def indic_category(cp, syllabic, positional, block):
    """HarfBuzz's Indic-table category of cp, for the Khmer and Myanmar shapers."""
    known = cp in syllabic or cp in positional
    allowed = cp in (0x00A0, 0x25CC) or block.get(cp) in INDIC_ALLOWED
    category = INDIC_MAP[syllabic.get(cp, 'Other')] if known and allowed else 'X'
    position = positional.get(cp, 'Not_Applicable') if known and allowed else 'Not_Applicable'
    if category == 'SM' and position == 'Not_Applicable':
        category = 'SMPst'
    category = INDIC_OVERRIDES.get(cp, category)
    if category in ('M', 'MPst') and block.get(cp, '').startswith(('Khmer', 'Myanmar')):
        category = INDIC_POSITION[position]
    return category


# gen-use-table.py -----------------------------------------------------------------------------------------------------

def use_category(cp, isc, ipc, jt, di, gc):
    """HarfBuzz's USE category of cp (map_to_use)."""
    if 0x1CE2 <= cp <= 0x1CE8:
        isc = 'Cantillation_Mark'
    if 0x0F18 <= cp <= 0x0F19 or 0x0F3E <= cp <= 0x0F3F:
        isc = 'Vowel_Dependent'
    if cp == 0x1CED:
        isc = 'Tone_Mark'
    mark = gc in ('Mc', 'Me', 'Mn')
    is_base = (isc in ('Number', 'Consonant', 'Consonant_Head_Letter', 'Tone_Letter', 'Vowel_Independent') or
               (jt in ('jt_C', 'jt_D', 'jt_L', 'jt_R') and isc != 'Joiner') or
               (gc == 'Lo' and isc in ('Avagraha', 'Bindu', 'Consonant_Final', 'Consonant_Medial', 'Consonant_Subjoined',
                                       'Vowel', 'Vowel_Dependent')))
    is_base_other = isc == 'Consonant_Placeholder' or cp in (0x2015, 0x2022, 0x25FB, 0x25FC, 0x25FD, 0x25FE)
    is_cgj = isc == 'Joiner' or (di and mark)
    is_sym_mod = isc == 'Symbol_Modifier'
    is_word_joiner = (di and cp not in (0x115F, 0x1160, 0x3164, 0xFFA0, 0x1BCA0, 0x1BCA1, 0x1BCA2, 0x1BCA3) and
                      isc == 'Other' and not is_cgj) or gc == 'Cn'
    rules = {
        'B': is_base,
        'N': isc == 'Brahmi_Joining_Number',
        'GB': is_base_other,
        'CGJ': is_cgj,
        'F': (isc == 'Consonant_Final' and gc != 'Lo') or isc == 'Consonant_Succeeding_Repha',
        'FM': isc == 'Syllable_Modifier',
        'M': (isc == 'Consonant_Medial' and gc != 'Lo') or isc == 'Consonant_Initial_Postfixed',
        'CM': isc in ('Nukta', 'Gemination_Mark', 'Consonant_Killer'),
        'SUB': isc == 'Consonant_Subjoined' and gc != 'Lo',
        'CS': isc == 'Consonant_With_Stacker',
        'H': isc == 'Virama' and cp != 0x0DCA,
        'HVM': cp == 0x0DCA,
        'HN': isc == 'Number_Joiner',
        'IS': isc == 'Invisible_Stacker' and cp != 0x1A60,
        'G': isc == 'Hieroglyph',
        'HM': isc == 'Hieroglyph_Modifier',
        'HR': isc == 'Hieroglyph_Mirror',
        'J': isc == 'Hieroglyph_Joiner',
        'SB': isc in ('Hieroglyph_Mark_Begin', 'Hieroglyph_Segment_Begin'),
        'SE': isc in ('Hieroglyph_Mark_End', 'Hieroglyph_Segment_End'),
        'ZWNJ': isc == 'Non_Joiner',
        'O': ((gc == 'Po' or isc in ('Consonant_Dead', 'Joiner', 'Modifying_Letter', 'Other')) and not is_base and
              not is_base_other and not is_cgj and not is_sym_mod and not is_word_joiner),
        'RK': isc == 'Reordering_Killer',
        'R': isc in ('Consonant_Preceding_Repha', 'Consonant_Prefixed'),
        'Sk': cp == 0x1A60,
        'SM': is_sym_mod,
        'V': isc == 'Pure_Killer' or (gc != 'Lo' and isc in ('Vowel', 'Vowel_Dependent')),
        'VM': isc in ('Tone_Mark', 'Cantillation_Mark', 'Register_Shifter', 'Visarga') or (gc != 'Lo' and isc == 'Bindu'),
        'WJ': is_word_joiner,
    }
    values = [k for k, v in rules.items() if v]
    assert len(values) == 1, (hex(cp), isc, ipc, jt, di, gc, values)
    value = values[0]
    if cp in (0x11302, 0x11303, 0x114C1):
        ipc = 'Top'
    positions = {
        'F': {'Abv': ['Top'], 'Blw': ['Bottom'], 'Pst': ['Right']},
        'M': {'Abv': ['Top'], 'Blw': ['Bottom', 'Bottom_And_Left', 'Bottom_And_Right'], 'Pst': ['Right'],
              'Pre': ['Left', 'Top_And_Bottom_And_Left']},
        'CM': {'Abv': ['Top'], 'Blw': ['Bottom', 'Overstruck']},
        'V': {'Abv': ['Top', 'Top_And_Bottom', 'Top_And_Bottom_And_Right', 'Top_And_Right'],
              'Blw': ['Bottom', 'Overstruck', 'Bottom_And_Right'], 'Pst': ['Right'],
              'Pre': ['Left', 'Top_And_Left', 'Top_And_Left_And_Right', 'Left_And_Right']},
        'VM': {'Abv': ['Top'], 'Blw': ['Bottom', 'Overstruck'], 'Pst': ['Right'], 'Pre': ['Left']},
        'SM': {'Abv': ['Top'], 'Blw': ['Bottom']},
        'FM': {'Abv': ['Top'], 'Blw': ['Bottom'], 'Pst': ['Not_Applicable']},
    }.get(value)
    if positions:
        suffix = [k for k, v in positions.items() if ipc in v]
        assert len(suffix) == 1, (hex(cp), ipc, value)
        value += suffix[0]
    return value


def use_table(archive_read, harfbuzz):
    isc = spans(archive_read('IndicSyllabicCategory.txt'))
    ipc = spans(archive_read('IndicPositionalCategory.txt'))
    for cp, value in spans(harfbuzz['IndicSyllabicCategory-Additional.txt']).items():
        isc[cp] = 'Syllable_Modifier' if value == 'Consonant_Final_Modifier' else value
    for cp, value in spans(harfbuzz['IndicPositionalCategory-Additional.txt']).items():
        ipc[cp] = 'Not_Applicable' if value == 'NA' else value
    jt = {cp: 'jt_' + v for cp, v in spans(archive_read('ArabicShaping.txt'), 2).items()}
    properties = archive_read('DerivedCoreProperties.txt')
    di = set(spans('\n'.join(line for line in properties.splitlines() if 'Default_Ignorable_Code_Point' in line)))
    gc = {}
    for line in archive_read('UnicodeData.txt').splitlines():
        fields = line.split(';')
        gc[int(fields[0], 16)] = fields[2]
    for line in archive_read('UnicodeData.txt').splitlines():  # First/Last ranges
        fields = line.split(';')
        if fields[1].endswith('First>'):
            first = int(fields[0], 16)
        elif fields[1].endswith('Last>'):
            for cp in range(first, int(fields[0], 16) + 1):
                gc[cp] = fields[2]
    scripts = spans(archive_read('Scripts.txt'))
    disabled = {'Arabic', 'Lao', 'Samaritan', 'Syriac', 'Thai'}

    def category(cp):
        # HarfBuzz's table holds only code points with a syllabic, positional, joining or ignorable property.
        if not (cp in isc or cp in ipc or cp in jt or cp in di) or scripts.get(cp, 'Unknown') in disabled:
            return 'O'
        return use_category(cp, isc.get(cp, 'Other'), ipc.get(cp, 'Not_Applicable'), jt.get(cp, 'jt_X'), cp in di,
                            gc.get(cp, 'Cn'))
    return category


# Output ---------------------------------------------------------------------------------------------------------------

def function(name, names, category, blocks, singles):
    """C++ lookup: one array per block, then the single characters elsewhere (ranges of one value)."""
    lines = []
    for first, last in blocks:
        values = [names.index(category(cp)) for cp in range(first, last + 1)]
        lines.append(f'constexpr u8 {name}_{first:04x}[{last - first + 1}]{{')
        lines += ['    ' + ','.join(str(v) for v in values[i:i + 32]) + ',' for i in range(0, len(values), 32)]
        lines.append('};')
    lines.append(f'constexpr u8 {name}_category(u32 u) noexcept {{')
    for first, last in blocks:
        lines.append(f'    if(u-0x{first:x}u<{last - first + 1}u)return {name}_{first:04x}[u-0x{first:x}u];')
    runs = []
    for cp in sorted(singles):
        value = names.index(singles[cp])
        if runs and runs[-1][1] == cp - 1 and runs[-1][2] == value:
            runs[-1][1] = cp
        else:
            runs.append([cp, cp, value])
    lines.append('    switch(u){')
    for first, last, value in runs:
        if first == last:
            lines.append(f'    case 0x{first:x}:return {value};')
        elif last - first < 4:
            lines.append('    ' + ''.join(f'case 0x{cp:x}:' for cp in range(first, last + 1)) + f'return {value};')
    lines.append('    default:break;}')
    for first, last, value in runs:
        if last - first >= 4:
            lines.append(f'    if(u-0x{first:x}u<={last - first}u)return {value};')
    lines.append('    return 0;')
    lines.append('}')
    return lines


def shared(category, blocks, default, extra=()):
    inside = lambda cp: any(first <= cp <= last for first, last in blocks)
    return {cp: category(cp) for first, last in SHARED + list(extra) for cp in range(first, last + 1)
            if not inside(cp) and category(cp) != default}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ucd', required=True, type=Path, help='Unicode 17.0.0 UCD.zip')
    parser.add_argument('--harfbuzz', required=True, type=Path, help='HarfBuzz 15.0.0 src directory (ms-use overrides)')
    parser.add_argument('--output', type=Path, default=OUTPUT)
    arguments = parser.parse_args()
    data = arguments.ucd.read_bytes()
    if hashlib.sha256(data).hexdigest() != PIN:
        raise SystemExit(f'{arguments.ucd} is not the pinned Unicode 17.0.0 UCD archive')
    harfbuzz = {}
    for name, pin in ADDITIONAL.items():
        path = arguments.harfbuzz / 'ms-use' / name
        if not path.is_file():
            path = arguments.harfbuzz / name
        content = path.read_bytes()
        if hashlib.sha256(content).hexdigest() != pin:
            raise SystemExit(f'{path} is not HarfBuzz 15.0.0\'s')
        harfbuzz[name] = content.decode('utf-8')
    with zipfile.ZipFile(arguments.ucd) as archive:
        def archive_read(name):
            return archive.read(name).decode('utf-8')
        syllabic, positional, block = (spans(archive_read('IndicSyllabicCategory.txt')),
                                       spans(archive_read('IndicPositionalCategory.txt')), spans(archive_read('Blocks.txt')))
        use = use_table(archive_read, harfbuzz)

    def khmer(cp):
        value = indic_category(cp, syllabic, positional, block)
        return value if value in KHMER else 'X'

    def myanmar(cp):
        value = {'V': 'IV', 'N': 'DB', 'PLACEHOLDER': 'GB'}.get(indic_category(cp, syllabic, positional, block),
                                                               indic_category(cp, syllabic, positional, block))
        return value if value in MYANMAR else 'X'

    lines = ['// Generated by tools/unicode/syllabic_categories.py from Unicode 17.0.0 and HarfBuzz 15.0.0; do not edit.',
             '// Unicode-derived data: Unicode License v3. Copyright (c) 1991-2025 Unicode, Inc.',
             '// See assets/licenses/Unicode-3.0.txt. Values are the K, M and U enumerators of syllabic_shape.cpp.']
    lines += function('khmer', KHMER, khmer, KHMER_BLOCKS, shared(khmer, KHMER_BLOCKS, 'X'))
    lines += function('myanmar', MYANMAR, myanmar, MYANMAR_BLOCKS, shared(myanmar, MYANMAR_BLOCKS, 'X'))
    lines += function('use', USE, use, USE_BLOCKS, shared(use, USE_BLOCKS, 'O', USE_SPARSE))
    arguments.output.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
