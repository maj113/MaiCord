"""Build first-party Unicode lookup data from the pinned official UCD text files.
No network access or third-party Python modules. Runtime tables carry Unicode
License v3 data; see assets/licenses/Unicode-3.0.txt. This is not a shaper.
"""
from compact_pages import compact_index
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import re
import zipfile
LIMIT = 0x110000
EXPECTED_UCD_SHA256 = '2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e'
# Runtime text properties: UCD alias, C++ name, getter, source, default.
FIELDS = [
    ('bc', 'BidiClass', 'bidi_class', 'extracted/DerivedBidiClass.txt', 'L'),
    ('gc', 'GeneralCategory', 'general_category', 'extracted/DerivedGeneralCategory.txt', 'Cn'),
    ('sc', 'Script', 'script', 'Scripts.txt', 'Zzzz'),
    ('ccc', None, 'combining_class', 'extracted/DerivedCombiningClass.txt', '0'),
    ('GCB', 'GraphemeBreak', 'grapheme_break', 'auxiliary/GraphemeBreakProperty.txt', 'XX'),
    ('WB', 'WordBreak', 'word_break', 'auxiliary/WordBreakProperty.txt', 'XX'),
    ('lb', 'LineBreak', 'line_break', 'LineBreak.txt', 'XX'),
    ('jt', 'JoiningType', 'joining_type', 'extracted/DerivedJoiningType.txt', 'U'),
    ('InCB', 'IndicConjunctBreak', 'indic_conjunct_break', 'DerivedCoreProperties.txt', 'None'),
    ('InSC', 'IndicSyllabicCategory', 'indic_syllabic_category', 'IndicSyllabicCategory.txt', 'Other'),
    ('InPC', 'IndicPositionalCategory', 'indic_positional_category', 'IndicPositionalCategory.txt', 'NA'),
]
FLAGS = ['Default_Ignorable_Code_Point', 'White_Space', 'Bidi_Mirrored',
         'Emoji', 'Extended_Pictographic', 'Variation_Selector',
         'Arabic_Script_Extension', 'East_Asian_Line_Break']
FLAG_XML = ['DI', 'WSpace', 'Bidi_M', 'Emoji', 'ExtPict', 'VS', 'scx', 'ea']


def fields(line):
    return [v.strip() for v in line.split('#', 1)[0].split(';')]


def bounds(text):
    pair = text.split('..')
    lo, hi = int(pair[0], 16), int(pair[-1], 16)
    if not 0 <= lo <= hi < LIMIT:
        raise ValueError(f'Invalid range {text}')
    return lo, hi + 1


def digest(data):
    return hashlib.sha256(data).hexdigest()


def cpp_array(kind, name, values, width=16):
    values = list(values)
    body = '\n'.join('    ' + ','.join(str(v) for v in values[i:i + width]) + ','
                     for i in range(0, len(values), width))
    return f'static constexpr {kind} {name}[] = {{\n{body}\n}};\n'


def mirror_arrays(mirrors, brackets):
    """Sorted BMP mirror pairs; each value packs the signed mirror delta above
    the paired-bracket type (0 none, 1 open, 2 close). Every paired bracket is
    its own mirror, so one table answers both lookups."""
    pairs = dict(mirrors)
    kinds = {cp: kind for cp, _, kind in brackets}
    if len(pairs) != len(mirrors) or any(pairs.get(cp) != paired for cp, paired, _ in brackets):
        raise ValueError('Every paired bracket must be its own Bidi_Mirroring_Glyph')
    points, values = sorted(pairs), []
    for cp in points:
        delta = pairs[cp] - cp
        if pairs[cp] > 0xffff or cp > 0xffff or not -0x2000 <= delta < 0x2000:
            raise ValueError(f'Mirror {cp:x} does not fit the packed BMP table')
        values.append((delta & 0x3fff) << 2 | kinds.get(cp, 0))
    return cpp_array('u16', 'unicode_mirror_points', points) + cpp_array('u16', 'unicode_mirror_values', values)


def tier(values, shift, uniform):
    size = 1 << shift
    pages, ids, flat = {}, [], []
    for at in range(0, len(values), size):
        page = tuple(values[at:at + size])
        if len(page) < size:
            page += (page[-1],) * (size - len(page))
        # At the second tier only uniform direct-record leaves can collapse.
        if uniform and len(set(page)) == 1 and (uniform == 1 or page[0] & 0x8000):
            ids.append(page[0] | 0x8000)
        else:
            if page not in pages:
                pages[page] = len(pages)
                flat.extend(page)
            ids.append(pages[page])
    if len(pages) >= 0x8000:
        raise ValueError('Tier index exceeds marker range')
    return ids, flat


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ucd', required=True, type=Path)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    if digest(args.ucd.read_bytes()) != EXPECTED_UCD_SHA256:
        raise ValueError('UCD archive does not match the pinned Unicode 17.0.0 release')
    used_sources = {}
    with zipfile.ZipFile(args.ucd) as archive:
        def read(name):
            data = archive.read(name)
            used_sources[name] = {'bytes': len(data), 'sha256': digest(data)}
            return data.decode('utf-8').splitlines()
        if not any('17.0.0' in line for line in read('ReadMe.txt')):
            raise ValueError('Expected Unicode 17.0.0')
        aliases, names = {}, {}
        for line in read('PropertyValueAliases.txt'):
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            parts = fields(line)
            key, canonical = parts[:2]
            names.setdefault(key, set()).add(canonical)
            for value in parts[1:]:
                if value:
                    aliases.setdefault(key, {})[value] = canonical
        columns, enumerations = [], {}
        for key, enum, getter, filename, default in [*FIELDS, ('ea', None, None, 'EastAsianWidth.txt', 'N')]:
            if key == 'ccc':
                values = bytearray(LIMIT)
                number = lambda value: int(aliases[key].get(value, value))
            else:
                labels = sorted(names[key])
                if len(labels) > 256:
                    raise ValueError(f'{key}: enum exceeds byte representation')
                index = {label: i for i, label in enumerate(labels)}
                def number(value):
                    return index[aliases[key].get(value, value)]
                values = bytearray([number(default)]) * LIMIT
            lines = read(filename)
            # Defaults apply first, in file order; explicit rows override them.
            for missing in (True, False):
                for line in lines:
                    if missing:
                        if '@missing:' not in line:
                            continue
                        parts = fields(line.split('@missing:', 1)[1])
                    else:
                        if not line.strip() or line.lstrip().startswith('#'):
                            continue
                        parts = fields(line)
                    if key == 'InCB':
                        if len(parts) < 3 or parts[1] != 'InCB':
                            continue
                        value = parts[2]
                    else:
                        value = parts[1]
                    lo, end = bounds(parts[0])
                    values[lo:end] = bytes([number(value)]) * (end - lo)
            if key != 'ccc':
                used = set(values)
                if key == 'sc':
                    for line in read('ScriptExtensions.txt'):
                        if line.strip() and not line.lstrip().startswith('#'):
                            used.update(number(v) for v in fields(line)[1].split())
                new_labels = [v for i, v in enumerate(labels) if i in used]
                translation = bytes(new_labels.index(v) if i in used else 0 for i, v in enumerate(labels))
                values = values.translate(translation + bytes(256 - len(translation)))
                enumerations[key] = new_labels
            if key == 'ea':
                east_asian = values
            else:
                columns.append(values)
        flags = array('I', [0]) * LIMIT
        flag_bits = {name: 1 << i for i, name in enumerate(FLAGS)}
        for filename in ['DerivedCoreProperties.txt', 'PropList.txt', 'emoji/emoji-data.txt']:
            for line in read(filename):
                if not line.strip() or line.lstrip().startswith('#'):
                    continue
                parts = fields(line)
                if parts[1] in flag_bits:
                    lo, end = bounds(parts[0]); bit = flag_bits[parts[1]]
                    for cp in range(lo, end):
                        flags[cp] |= bit
        for line in read('UnicodeData.txt'):
            parts = line.split(';')
            if parts[9] == 'Y':
                flags[int(parts[0], 16)] |= flag_bits['Bidi_Mirrored']
        scripts = enumerations['sc']
        arabic_id = scripts.index('Arab')
        arabic = bytearray(value == arabic_id for value in columns[2])
        for line in read('ScriptExtensions.txt'):
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            parts = fields(line); lo, end = bounds(parts[0])
            member = any(aliases['sc'].get(v, v) == 'Arab' for v in parts[1].split())
            arabic[lo:end] = bytes([member]) * (end - lo)
        east_values = {enumerations['ea'].index(v) for v in ('F', 'W', 'H')}
        for cp in range(LIMIT):
            if arabic[cp]:
                flags[cp] |= flag_bits['Arabic_Script_Extension']
            if east_asian[cp] in east_values:
                flags[cp] |= flag_bits['East_Asian_Line_Break']
        columns.append(flags)
        widths = [max(1, max(column).bit_length()) for column in columns]
        offsets, bits = [], 0
        for width in widths:
            offsets.append(bits); bits += width
        words = (bits + 31) // 32
        if bits != 64:
            raise ValueError(f'Expected 64-bit runtime properties, got {bits}')
        records, ids, record_ids = [], array('H'), {}
        for cp in range(LIMIT):
            value = sum(int(column[cp]) << at for column, at in zip(columns, offsets))
            if value not in record_ids:
                record_ids[value] = len(records); records.append(value)
            ids.append(record_ids[value])
        if len(records) >= 0x8000:
            raise ValueError('Property record count exceeds marker range')
        candidates = []
        best = None
        for low_shift in range(3, 9):
            middle_ids, leaves = tier(ids, low_shift, 1)
            for middle_shift in range(3, 9):
                top, middle = tier(middle_ids, middle_shift, 2)
                size = 2 * (len(top) + len(middle) + len(leaves))
                candidate = {'leaf_shift': low_shift, 'middle_shift': middle_shift, 'index_bytes': size,
                             'top_entries': len(top), 'middle_entries': len(middle), 'leaf_entries': len(leaves)}
                candidates.append(candidate)
                if best is None or size < best[0]['index_bytes']:
                    best = (candidate, top, middle, leaves)
        layout, top, middle, leaves = best
        # Exhaustively prove the chosen layout reconstructs the uncompressed IDs.
        a, b = layout['leaf_shift'], layout['middle_shift']
        top, middle, leaves = compact_index(top, middle, leaves, a, b)
        layout = dict(layout, uncompacted_index_bytes=layout['index_bytes'],
                      index_bytes=2*(len(top)+len(middle)+len(leaves)),
                      top_entries=len(top), middle_entries=len(middle), leaf_entries=len(leaves),
                      addressing='overlapped-element-offsets')
        for cp, expected in enumerate(ids):
            value = top[cp >> (a + b)]
            if not value & 0x8000:
                value = middle[value + ((cp >> a) & ((1 << b) - 1))]
                if not value & 0x8000:
                    value = leaves[value + (cp & ((1 << a) - 1))]
            if value & 0x8000:
                value &= 0x7fff
            if value != expected:
                raise AssertionError(f'Compression mismatch at {cp:x}')
        mirrors, brackets = [], []
        for line in read('BidiMirroring.txt'):
            if line.strip() and not line.lstrip().startswith('#'):
                parts = fields(line); mirrors.append(tuple(int(v, 16) for v in parts[:2]))
        for line in read('BidiBrackets.txt'):
            if line.strip() and not line.lstrip().startswith('#'):
                parts = fields(line); brackets.append((int(parts[0], 16), int(parts[1], 16), {'o': 1, 'c': 2}[parts[2]]))
        graphemes, grapheme_count = [], 0
        for line in read('auxiliary/GraphemeBreakTest.txt'):
            tokens = line.partition('#')[0].split()
            if not tokens:
                continue
            if len(tokens)%2 != 1 or tokens[0] != '÷' or tokens[-1] != '÷':
                raise ValueError('Invalid official grapheme vector')
            case = []
            for i in range(1,len(tokens),2):
                if tokens[i-1] not in ('÷','×'):
                    raise ValueError('Invalid grapheme boundary marker')
                cp = int(tokens[i],16)
                if not 0 <= cp < LIMIT or 0xd800 <= cp <= 0xdfff:
                    raise ValueError('Invalid test scalar')
                case.append(cp | (0x80000000 if tokens[i-1]=='÷' else 0))
            graphemes.extend([len(case),*case]);grapheme_count += 1
    mirrors.sort(); brackets.sort()
    notice = '// Generated from Unicode 17.0.0; do not edit. Unicode License v3.\n// Copyright (c) 1991-2025 Unicode, Inc. See assets/licenses/Unicode-3.0.txt.\n'
    header = notice + '#pragma once\n#include "maicord/text/base.hpp"\nnamespace maicord {\n'
    header += 'inline constexpr u32 unicode_data_version = 0x00110000;\n'
    for key, enum, *_ in FIELDS:
        if enum:
            header += 'enum class Unicode' + enum + ' : u8 {\n    ' + ',\n    '.join(v.lower() for v in enumerations[key]) + '\n};\n'
    header += 'enum class UnicodeFlag : u32 {\n' + '\n'.join(f'    {name.lower()}=1u<<{i},' for i, name in enumerate(FLAGS)) + '\n};\n'
    header += f'struct UnicodeProperties {{\n    u32 words[{words}];\n'
    header += 'private:\n    template<u32 At,u32 Width> constexpr u32 value() const noexcept {\n        u32 v=words[At/32]>>(At%32);\n        if constexpr(At%32+Width>32)v|=words[At/32+1]<<(32-At%32);\n        return v&((1u<<Width)-1);\n    }\n'
    header += 'public:\n'
    for i, (key, enum, getter, *_) in enumerate(FIELDS):
        kind = 'Unicode' + enum if enum else 'u8'
        header += f'    constexpr {kind} {getter}() const noexcept {{return static_cast<{kind}>(value<{offsets[i]},{widths[i]}>());}}\n'
    header += f'    constexpr u32 flags() const noexcept {{return value<{offsets[-1]},{widths[-1]}>();}}\n'
    header += '    constexpr bool has(UnicodeFlag flag) const noexcept {return (flags()&static_cast<u32>(flag))!=0;}\n};\n'
    header += f'static_assert(sizeof(UnicodeProperties)=={words*4});\n}} // namespace maicord\n'
    data = notice + f'static constexpr u32 unicode_leaf_shift={a},unicode_middle_shift={b};\n'
    data += cpp_array('u16', 'unicode_ascii', ids[:128])
    data += cpp_array('u16', 'unicode_top', top) + cpp_array('u16', 'unicode_middle', middle) + cpp_array('u16', 'unicode_leaves', leaves)
    data += 'static constexpr UnicodeProperties unicode_records[] = {\n'
    data += '\n'.join('    {{' + ','.join(f'0x{(v>>(32*i))&0xffffffff:08x}u' for i in range(words)) + '}},' for v in records) + '\n};\n'
    data += mirror_arrays(mirrors, brackets)
    hpath = args.repo / 'include/maicord/text/generated/unicode_data.hpp'
    dpath = args.repo / 'src/text/generated/unicode_data.inc'
    hpath.parent.mkdir(parents=True, exist_ok=True); dpath.parent.mkdir(parents=True, exist_ok=True)
    hpath.write_text(header, encoding='utf-8'); dpath.write_text(data, encoding='utf-8')
    gpath = args.repo / 'tests/generated/grapheme_break_test.inc'
    gpath.parent.mkdir(parents=True, exist_ok=True)
    gpath.write_text(notice + f'static constexpr u32 grapheme_vector_count={grapheme_count};\n' + cpp_array('u32','grapheme_vectors',graphemes), encoding='utf-8')
    auxiliary_bytes = 256 + len(mirrors)*4
    report = {'unicode_version': '17.0.0', 'ucd_sha256': digest(args.ucd.read_bytes()), 'sources': used_sources,
              'fields': [{'ucd': k, 'getter': g, 'enum': e, 'values': enumerations.get(k), 'offset': offsets[i], 'width': widths[i]}
                         for i, (k,e,g,*_) in enumerate(FIELDS)],
              'flags': [{'name': name, 'xml': alias, 'bit': i} for i, (name, alias) in enumerate(zip(FLAGS, FLAG_XML))],
              'record_count': len(records),
              'grapheme_vectors': grapheme_count, 'grapheme_scalars': len(graphemes)-grapheme_count, 'record_bits': bits, 'record_bytes': words*4, 'layout': layout, 'layout_candidates': candidates,
              'table_bytes': layout['index_bytes'] + len(records)*words*4 + auxiliary_bytes,
              'mirror_count': len(mirrors), 'bracket_count': len(brackets),
              'generated': {str(p.relative_to(args.repo)): digest(p.read_bytes()) for p in [hpath,dpath,gpath]}}
    args.report.parent.mkdir(parents=True, exist_ok=True); args.report.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ['record_count','record_bits','record_bytes','layout','table_bytes','mirror_count','bracket_count']}))

if __name__ == '__main__':
    main()
