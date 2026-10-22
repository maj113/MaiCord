"""Generate immutable substring pools with Python standard libraries at build time."""
import argparse
import ast
from pathlib import Path
import re

CATEGORIES = (
    ('Smileys & Emotion', 'People & Body'), ('Animals & Nature',), ('Food & Drink',),
    ('Activities',), ('Travel & Places',), ('Objects',), ('Symbols',), ('Flags',),
)


def read_entries(path):
    entries, names, version, group = [], set(), '', ''
    for line in path.read_text(encoding='utf-8').splitlines():
        if match := re.match(r'# Version:\s*([0-9.]+)', line):
            version = match[1]
        elif match := re.match(r'# group:\s*(.+?)\s*$', line):
            group = match[1]
        elif match := re.match(r'\s*([0-9A-F ]+)\s*;\s*fully-qualified\s*#\s*(\S+)\s+E[0-9.]+\s+(.+?)\s*$', line):
            name = re.sub('[^a-z0-9]+', '_', match[3].lower()).strip('_')
            if not name or name in names:
                continue
            names.add(name)
            glyph = match[2]
            if glyph != ''.join(chr(int(value, 16)) for value in match[1].split()):
                raise ValueError('Code-point column and rendered glyph differ')
            category = next(i for i, groups in enumerate(CATEGORIES) if group in groups)
            entries.append((name.encode('ascii'), glyph.encode('utf-8'), category))
    if version != '16.0' or len(entries) != 3780:
        raise ValueError('Expected the complete Unicode Emoji 16.0 catalog')
    return entries


def read_canonical_entries(path):
    source = path.read_text(encoding='utf-8')

    def read_pool(symbol):
        fragment = source.split(f'const char kUnicodeEmojiCatalog{symbol}[] =', 1)[1].split(';', 1)[0]
        return b''.join(ast.literal_eval('b' + literal)
                        for literal in re.findall(r'"[^"\n]*"', fragment))

    names, glyphs = read_pool('Names'), read_pool('Glyphs')
    fragment = source.split('kUnicodeEmoji16CategoryRanges[][2] =', 1)[1].split(';', 1)[0]
    ranges = [tuple(map(int, row)) for row in re.findall(r'\{(\d+), (\d+)\}', fragment)]
    fragment = source.split('kUnicodeEmoji16Entries[] =', 1)[1]
    records = [tuple(map(int, row)) for row in re.findall(r'\{(\d+), (\d+), (\d+), (\d+)\}', fragment)]
    if len(records) != 3780 or len(ranges) != len(CATEGORIES):
        raise ValueError('Expected the complete Unicode Emoji 16.0 catalog')
    previous = 0
    for first, last in sorted(ranges):
        if first != previous or first >= last or last > len(records):
            raise ValueError('Invalid category ranges')
        previous = last
    if previous != len(records):
        raise ValueError('Incomplete category ranges')
    entries = []
    for index, (name_offset, glyph_offset, name_bytes, glyph_bytes) in enumerate(records):
        if not name_bytes or not glyph_bytes or name_offset + name_bytes > len(names) or glyph_offset + glyph_bytes > len(glyphs):
            raise ValueError('Invalid canonical string range')
        category = next(i for i, (first, last) in enumerate(ranges) if first <= index < last)
        entries.append((names[name_offset:name_offset + name_bytes],
                        glyphs[glyph_offset:glyph_offset + glyph_bytes], category))
    return entries


def pool_strings(values):
    maximal = []
    for value in sorted(set(values), key=lambda value: (-len(value), value)):
        if not any(value in previous for previous in maximal):
            maximal.append(value)
    # Reverse lexical order groups similar chains for deflate packing.
    maximal.sort(reverse=True)
    minimum = 1 if all(value.isascii() for value in values) else 3
    prefixes = {}
    for index, value in enumerate(maximal):
        for count in range(minimum, len(value)):
            prefixes.setdefault(value[:count], []).append(index)
    edges = []
    for index, value in enumerate(maximal):
        seen = set()
        for count in range(len(value) - 1, minimum - 1, -1):
            for other in prefixes.get(value[-count:], ()):
                if index != other and other not in seen:
                    edges.append((-count, index, other))
                    seen.add(other)
    edges.sort()
    parent = list(range(len(maximal)))
    successor, predecessor, overlap = {}, {}, {}

    def root(index):
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    # Select the longest overlaps between disjoint chains. Prevent cycles.
    for negative, index, other in edges:
        if index in successor or other in predecessor or root(index) == root(other):
            continue
        successor[index], predecessor[other], overlap[index] = other, index, -negative
        parent[root(other)] = root(index)
    pool = bytearray()
    for first in range(len(maximal)):
        if first in predecessor:
            continue
        index = first
        pool.extend(maximal[index])
        while index in successor:
            other = successor[index]
            pool.extend(maximal[other][overlap[index]:])
            index = other
    offsets = [pool.find(value) for value in values]
    if any(at < 0 or pool[at:at + len(value)] != value for at, value in zip(offsets, values)):
        raise ValueError('Lost string during pooling')
    return bytes(pool), offsets


def shared_glyphs(entries, shortcode_include):
    source = shortcode_include.read_text(encoding='utf-8')
    fragment = re.search(r'constexpr char kEmojiShortcodeGlyphs\[\] =(.*?);\n', source, re.S)
    if not fragment:
        raise ValueError('Missing shortcode glyph pool')
    original = b''.join(ast.literal_eval('b' + literal)
                        for literal in re.findall(r'"(?:[^"\\\n]|\\.)*"', fragment[1]))
    original.decode('utf-8')
    glyphs, offsets = bytearray(original), []
    for _, glyph, _ in entries:
        # Exact bytes preserve presentation selectors and all shortcode offsets.
        offset = glyphs.find(glyph)
        if offset < 0:
            offset = len(glyphs)
            glyphs.extend(glyph)
        offsets.append(offset)
    if glyphs[:len(original)] != original or any(
            glyphs[offset:offset + len(row[1])] != row[1]
            for offset, row in zip(offsets, entries)):
        raise ValueError('Shared glyph pool changed an entry')
    return bytes(glyphs), offsets


def generate(entries, shortcode_include=None):
    names, name_offsets = pool_strings([row[0] for row in entries])
    glyphs, glyph_offsets = (shared_glyphs(entries, shortcode_include) if shortcode_include
                            else pool_strings([row[1] for row in entries]))
    if len(names) >= 1 << 16 or len(glyphs) >= 1 << 16:
        raise ValueError('Pool exceeds packed offset capacity')
    if max(len(row[0]) for row in entries) >= 128 or max(len(row[1]) for row in entries) >= 64:
        raise ValueError('String exceeds packed length capacity')
    lines = ['// Generated by tools/generate_unicode_emoji_catalog.py.',
             '// Source: https://unicode.org/Public/emoji/16.0/emoji-test.txt',
             '// Unicode Emoji version 16.0; fully-qualified sequences only.']
    for symbol, pool, hexadecimal in [('Names', names, False), ('Glyphs', glyphs, True)]:
        lines.append(f'const char kUnicodeEmojiCatalog{symbol}[] =')
        for first in range(0, len(pool), 32):
            chunk = pool[first:first + 32]
            value = ''.join(f'\\x{byte:02X}' for byte in chunk) if hexadecimal else chunk.decode('ascii')
            lines.append(f'    "{value}"')
        lines.append('    ;')
    ranges = []
    for category in range(len(CATEGORIES)):
        indices = [i for i, row in enumerate(entries) if row[2] == category]
        if not indices or indices != list(range(indices[0], indices[-1] + 1)):
            raise ValueError('Missing or noncontiguous category')
        ranges.append(f'{{{indices[0]}, {indices[-1] + 1}}}')
    lines.append('inline constexpr u16 kUnicodeEmoji16CategoryRanges[][2] = {' + ', '.join(ranges) + '};')
    lines.append('inline constexpr EmojiCatalogEntry kUnicodeEmoji16Entries[] = {')
    for first in range(0, len(entries), 4):
        lines.append('    ' + ' '.join(f'{{{name_offsets[i]}, {glyph_offsets[i]}, {len(entries[i][0])}, {len(entries[i][1])}}},'
                                     for i in range(first, min(first + 4, len(entries)))))
    lines.append('};')
    return '\n'.join(lines) + '\n', len(names), len(glyphs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--canonical-include', action='store_true',
                        help='Read the existing immutable catalog instead of emoji-test.txt')
    parser.add_argument('--shortcode-include', type=Path,
                        help='Share the immutable shortcode glyph pool and preserve its offsets')
    args = parser.parse_args()
    source_root = Path(__file__).resolve().parent.parent
    if args.output.resolve().is_relative_to(source_root):
        parser.error('Generated output must be outside the source tree')
    entries = read_canonical_entries(args.input) if args.canonical_include else read_entries(args.input)
    data, names, glyphs = generate(entries, args.shortcode_include)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(data, encoding='utf-8', newline='\n')
    print(f'Generated 3780 entries; name pool {names} B; glyph pool {glyphs} B')


if __name__ == '__main__':
    main()
