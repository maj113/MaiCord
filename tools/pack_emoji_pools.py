#!/usr/bin/env python3
"""Deflate-pack the emoji name and glyph string pools and both entry tables into one generated include.

Usage: pack_emoji_pools.py OUTPUT CATALOG_INCLUDE SHORTCODE_INCLUDE

CATALOG_INCLUDE is the checked-in Unicode Emoji 16.0 catalog (src/ui/text/unicode_emoji_16.inc); only its entry
strings and category ranges matter, because the pools below are laid out again.

The catalog name prefix, shared shortcode names, shared glyphs, the 6-byte catalog and alias rows and the
primary-shortcode bit set form one stream: a zstd frame from tools/deflate_cache.txt (the pools repeat over more
than the 32 KB window of deflate), or a zlib stream when the cache lacks it. Generated alias rows use offsets from the shared name suffix.
The pools are laid out so that the rows' offsets mostly follow the rows, and each row ships its name and glyph
offsets as the distance from the end of the strings of the row before (deltas() and resolve() below).
Rows ship as byte planes of BLOCK-row blocks (a plane of one field deflates far better than interleaved
rows); src/ui/text/emoji_catalog.cpp inflates the stream once, interleaves the rows back in place, resolves the
offsets and checks the layout constants emitted here (interleave() below is the same permutation, checked
against the sources).
"""
import ast
import pathlib
import re
import struct
import sys
import zlib
import optimal_deflate
from embed_fixture import embed_arrays, write_if_changed


LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
SHORTCODE_NAME_BASE = 0
ROW = 6  # bytes per catalog and alias row
BLOCK = 256  # rows per plane block; emoji_catalog.cpp interleaves one block through a 1.5 KB stack buffer
PACKED_FILE = "emoji_pool_packed.bin"  # beside the include, read by #embed


def read_pool(source, symbol, ascii_only):
    match = re.search(r"(?:constexpr|const) char " + symbol + r"\[\] =(.*?);\n", source, re.S)
    if not match:
        raise SystemExit(f"{symbol}: pool not found")
    data = bytearray()
    for literal in LITERAL.findall(match.group(1)):
        # C hexadecimal escapes are greedy; Python's take exactly two digits.
        if re.search(r"\\x[0-9A-Fa-f]{3}", literal):
            raise SystemExit(f"{symbol}: ambiguous hexadecimal escape")
        data += ast.literal_eval('b"' + literal + '"')
    if ascii_only and not all(0x20 < b < 0x7F for b in data):
        raise SystemExit(f"{symbol}: unexpected name byte")
    if not ascii_only:
        data.decode("utf-8")
    return bytes(data)


def pack(data):
    def fallback(data):
        best = None
        for mem in (8, 9):
            for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED):
                c = zlib.compressobj(9, zlib.DEFLATED, 15, mem, strategy)
                out = c.compress(data) + c.flush()
                if best is None or len(out) < len(best):
                    best = out
        return best

    cache = optimal_deflate.DeflateCache("emoji_zstd", zstd=True)
    best = cache.stream(data, fallback)
    cache.save()
    return best


def planes(rows):
    """Rows (bytes, ROW each) as per-block byte planes."""
    out = bytearray()
    count = len(rows) // ROW
    for first in range(0, count, BLOCK):
        n = min(BLOCK, count - first)
        block = rows[first * ROW:(first + n) * ROW]
        for column in range(ROW):
            out += bytes(block[i * ROW + column] for i in range(n))
    return bytes(out)


def interleave(data, count):
    """The permutation emoji_catalog.cpp applies in place."""
    out = bytearray()
    for first in range(0, count, BLOCK):
        n = min(BLOCK, count - first)
        block = data[first * ROW:(first + n) * ROW]
        for i in range(n):
            out += bytes(block[column * n + i] for column in range(ROW))
    return bytes(out)


def maximal_strings(strings):
    """The distinct strings that are not inside another one, in the order of their first rows."""
    unique = list(dict.fromkeys(strings))
    joined = b"\0".join(unique)
    return [value for value in unique if joined.count(value) == 1]


def row_greedy_pool(strings):
    """A greedy shortest superstring (the longest overlaps join first, as in pool_strings) in which equal
    overlaps join the strings of the nearest rows, and the chains follow in the order of their first rows. The
    pool is as short as the greedy one, and the rows' offsets mostly follow the rows."""
    maximal = maximal_strings(strings)
    first_row = {}
    for row, value in enumerate(strings):
        first_row.setdefault(value, row)
    prefixes = {}
    for index, value in enumerate(maximal):
        for count in range(1, len(value)):
            prefixes.setdefault(value[:count], []).append(index)
    edges = []
    for index, value in enumerate(maximal):
        for count in range(1, len(value)):
            for other in prefixes.get(value[-count:], ()):
                if other != index:
                    edges.append((-count, abs(first_row[value] - first_row[maximal[other]]), index, other))
    edges.sort()
    parent = list(range(len(maximal)))
    successor, predecessor, overlap = {}, {}, {}

    def root(index):
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    for negative_count, _, index, other in edges:
        if index in successor or other in predecessor or root(index) == root(other):
            continue
        successor[index], predecessor[other], overlap[index] = other, index, -negative_count
        parent[root(other)] = root(index)
    pool = bytearray()
    for start in sorted((index for index in range(len(maximal)) if index not in predecessor),
                        key=lambda index: first_row[maximal[index]]):
        index = start
        pool += maximal[index]
        while index in successor:
            count, index = overlap[index], successor[index]
            pool += maximal[index][count:]
    return bytes(pool)


def row_order_pool(strings):
    """The strings in row order, each overlapping the end of the pool as far as it can."""
    pool = bytearray()
    for value in maximal_strings(strings):
        count = min(len(value), len(pool))
        while count and not pool.endswith(value[:count]):
            count -= 1
        pool += value[count:]
    return bytes(pool)


def placements(pool, strings, start=0):
    """Each string's offset in pool: the occurrence nearest the end of the string before it."""
    offsets, end = [], start
    for value in strings:
        at, best = pool.find(value), None
        while at >= 0:
            if best is None or abs(at - end) < abs(best - end):
                best = at
            at = pool.find(value, at + 1)
        if best is None:
            raise SystemExit("A string is not in its pool")
        offsets.append(best)
        end = best + len(value)
    return offsets


def deltas(offsets, lengths, modulus):
    """Each offset as the distance from the end of the string before it (emoji_catalog.cpp resolve_offsets)."""
    out, end = [], 0
    for offset, length in zip(offsets, lengths):
        out.append((offset - end) % modulus)
        end = offset + length
    return out


def pool_strings(values):
    """A short pool that holds every value, and each value's offset in it: the longest overlaps join first."""
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


def shared_names(catalog_names, shortcode_names, shortcode_glyphs, shortcode_source):
    """The catalog names followed by the shortcode names they lack, and each alias as (name offset, glyph,
    name bytes): the name offset is its position in that shared pool."""
    match = re.search(r"constexpr EmojiAliasEntry kEmojiShortcodes\[\] =\s*\{(.*?)\};", shortcode_source, re.S)
    row_pattern = re.compile(r"\{(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\},")
    if not match or row_pattern.sub("", match[1]).strip():
        raise SystemExit("Invalid shortcode entry table")
    rows = [tuple(map(int, row)) for row in row_pattern.findall(match[1])]
    if not rows or not 0 <= SHORTCODE_NAME_BASE < len(catalog_names) < 65536:
        raise SystemExit("Invalid catalog name prefix")
    for name_offset, glyph_offset, name_bytes, glyph_bytes in rows:
        if (not 0 < name_bytes < 128 or name_offset + name_bytes > len(shortcode_names) or
                not 0 < glyph_bytes < 256 or glyph_offset + glyph_bytes > len(shortcode_glyphs)):
            raise SystemExit("Invalid shortcode string range")
    names = [shortcode_names[offset:offset + count] for offset, _, count, _ in rows]
    absent = [name for name in names if catalog_names.find(name, SHORTCODE_NAME_BASE) < 0]
    appendix, _ = pool_strings(absent)
    shared = catalog_names + appendix
    remapped = []
    absolute = placements(shared[SHORTCODE_NAME_BASE:], names)
    for relative, (_, glyph_offset, name_bytes, glyph_bytes) in zip(absolute, rows):
        if not 0 <= relative < 131072:
            raise SystemExit("Shared shortcode name exceeds its exact 17-bit range")
        remapped.append((relative, shortcode_glyphs[glyph_offset:glyph_offset + glyph_bytes], name_bytes))
    if len(shared) - SHORTCODE_NAME_BASE >= 131072 or not shared.startswith(catalog_names):
        raise SystemExit("Shared shortcode name suffix exceeds 17-bit bounds")
    return shared, remapped


def catalog_rows(catalog_source):
    fragment = catalog_source.split("kUnicodeEmoji16Entries[] =", 1)[1]
    return [tuple(map(int, row)) for row in re.findall(r"\{(\d+), (\d+), (\d+), (\d+)\}", fragment)]


def category_ranges(catalog_source):
    fragment = catalog_source.split("kUnicodeEmoji16CategoryRanges[][2] =", 1)[1].split(";", 1)[0]
    return [tuple(map(int, row)) for row in re.findall(r"\{(\d+), (\d+)\}", fragment)]


def primary_rows(shortcode_source):
    match = re.search(r"kEmojiPrimaryShortcodes\[\]\s*=\s*\{(.*?)\};", shortcode_source, re.S)
    if not match:
        raise SystemExit("kEmojiPrimaryShortcodes not found")
    return [int(value) for value in re.findall(r"\d+", match[1])]


def resolve(rows, name_modulus):
    """The offsets of rows shipped as deltas: the loop of emoji_catalog.cpp resolve_offsets."""
    out, name_end, glyph_end = [], 0, 0
    for name_delta, glyph_delta, name_bytes, glyph_bytes in rows:
        name, glyph = (name_end + name_delta) % name_modulus, (glyph_end + glyph_delta) % 65536
        out.append((name, glyph, name_bytes, glyph_bytes))
        name_end, glyph_end = name + name_bytes, glyph + glyph_bytes
    return out


def generate(catalog_source, shortcode_source):
    source_names = read_pool(catalog_source, "kUnicodeEmojiCatalogNames", True)
    shortcode_names = read_pool(shortcode_source, "kEmojiShortcodeNames", True)
    source_glyphs = read_pool(catalog_source, "kUnicodeEmojiCatalogGlyphs", False)
    shortcode_glyphs = read_pool(shortcode_source, "kEmojiShortcodeGlyphs", False)
    if len(source_glyphs) >= 65536:
        raise SystemExit("Catalog glyph pool exceeds u16 bounds")
    entries = catalog_rows(catalog_source)
    ranges = category_ranges(catalog_source)
    primary = primary_rows(shortcode_source)
    for name_offset, glyph_offset, name_bytes, glyph_bytes in entries:
        if (not 0 < name_bytes < 128 or not 0 < glyph_bytes < 64 or name_offset + name_bytes > len(source_names) or
                glyph_offset + glyph_bytes > len(source_glyphs)):
            raise SystemExit("Invalid catalog string range")
    entry_names = [source_names[offset:offset + count] for offset, _, count, _ in entries]
    entry_glyphs = [source_glyphs[offset:offset + count] for _, offset, _, count in entries]
    # The pools are laid out so that the rows' offsets mostly follow the rows: shipped as deltas, they pack
    # far smaller than offsets into the shortest pools. The catalog names stay as short as the greedy pool, so
    # their offsets fit 16 bits. The glyphs are in row order, and each distinct glyph has one offset (index_pool
    # finds an alias's catalog entry by glyph offset and length).
    catalog_names = row_greedy_pool(entry_names)
    glyphs = bytearray(row_order_pool(entry_glyphs))
    names, aliases = shared_names(catalog_names, shortcode_names, shortcode_glyphs, shortcode_source)
    entry_name_offsets = placements(catalog_names, entry_names)
    entry_glyph_offsets = placements(bytes(glyphs), entry_glyphs)
    glyph_offset = dict(zip(entry_glyphs, entry_glyph_offsets))
    for _, glyph, _ in aliases:
        if glyph not in glyph_offset:
            at = bytes(glyphs).find(glyph)
            glyph_offset[glyph] = at if at >= 0 else len(glyphs)
            if at < 0:
                glyphs += glyph
    glyphs = bytes(glyphs)
    if len(catalog_names) >= 65536 or len(glyphs) >= 65536:
        raise SystemExit("Emoji pool exceeds its 16-bit offsets")
    entries = [(name, glyph, len(name_text), len(glyph_text)) for name, glyph, name_text, glyph_text in
               zip(entry_name_offsets, entry_glyph_offsets, entry_names, entry_glyphs)]
    aliases = [(name, glyph_offset[glyph], name_bytes, len(glyph)) for name, glyph, name_bytes in aliases]
    for (name, glyph, name_bytes, glyph_bytes), name_text, glyph_text in zip(entries, entry_names, entry_glyphs):
        assert catalog_names[name:name + name_bytes] == name_text and glyphs[glyph:glyph + glyph_bytes] == glyph_text
    # The primary row of a glyph is looked up by glyph: a set of rows replaces the ordered list only when no
    # two of them share a glyph (emoji_catalog.cpp walks the rows in order and takes the first match).
    if len(set(primary)) != len(primary) or not all(0 <= row < len(aliases) for row in primary):
        raise SystemExit("Invalid primary shortcode rows")
    if len({(aliases[row][1], aliases[row][3]) for row in primary}) != len(primary):
        raise SystemExit("Two primary shortcode rows share a glyph")
    bits = bytearray((len(aliases) + 7) // 8)
    for row in primary:
        bits[row >> 3] |= 1 << (row & 7)
    entry_deltas = list(zip(deltas([row[0] for row in entries], [row[2] for row in entries], 65536),
                            deltas([row[1] for row in entries], [row[3] for row in entries], 65536),
                            [row[2] for row in entries], [row[3] for row in entries]))
    alias_deltas = list(zip(deltas([row[0] for row in aliases], [row[2] for row in aliases], 131072),
                            deltas([row[1] for row in aliases], [row[3] for row in aliases], 65536),
                            [row[2] for row in aliases], [row[3] for row in aliases]))
    assert resolve(entry_deltas, 65536) == entries and resolve(alias_deltas, 131072) == aliases

    def entry_bytes(rows):
        return b"".join(struct.pack("<HHBB", *row) for row in rows)

    def alias_bytes(rows):
        return b"".join(struct.pack("<HHBB", offset & 0xFFFF, glyph, length | ((offset >> 16) << 7), glyph_bytes)
                        for offset, glyph, length, glyph_bytes in rows)

    catalog_bytes, alias_table = entry_bytes(entry_deltas), alias_bytes(alias_deltas)
    pool = names + glyphs
    pool += bytes(-len(pool) % 2)
    catalog_offset = len(pool)
    alias_offset = catalog_offset + len(catalog_bytes)
    primary_offset = alias_offset + len(alias_table)
    blob = pool + planes(catalog_bytes) + planes(alias_table) + bytes(bits)
    assert interleave(blob[catalog_offset:alias_offset], len(entries)) == catalog_bytes
    assert interleave(blob[alias_offset:primary_offset], len(aliases)) == alias_table
    lines = ["// Generated by tools/pack_emoji_pools.py; do not edit.",
             "constexpr u32 kEmojiPoolCatalogNamesOffset=0;",
             f"constexpr u32 kEmojiPoolCatalogNamePrefixBytes={len(catalog_names)};",
             f"constexpr u32 kEmojiPoolShortcodeNamesOffset={SHORTCODE_NAME_BASE};",
             f"constexpr u32 kEmojiPoolCatalogGlyphsOffset={len(names)};",
             f"constexpr u32 kEmojiPoolShortcodeGlyphsOffset={len(names)};",
             f"constexpr u32 kEmojiPoolCatalogGlyphBytes={len(glyphs)};",
             f"constexpr u32 kEmojiPoolShortcodeGlyphBytes={len(glyphs)};",
             f"constexpr u32 kEmojiPoolShortcodeNameBytes={len(names) - SHORTCODE_NAME_BASE};",
             f"constexpr u32 kEmojiPoolCatalogEntryOffset={catalog_offset};",
             f"constexpr u32 kEmojiPoolCatalogEntryCount={len(entries)};",
             f"constexpr u32 kEmojiPoolAliasOffset={alias_offset};",
             f"constexpr u32 kEmojiPoolAliasCount={len(aliases)};",
             f"constexpr u32 kEmojiPoolPrimaryOffset={primary_offset};",
             f"constexpr u32 kEmojiPoolMaximumNameBytes={max(row[2] for row in entries)};",
             f"constexpr u32 kEmojiPoolMaximumGlyphBytes={max(row[3] for row in entries)};",
             "constexpr u16 kEmojiPoolCategoryRanges[][2]={" + ", ".join("{%d, %d}" % row for row in ranges) + "};",
             f"constexpr u32 kEmojiPoolBytes={len(blob)};"]
    packed = pack(bytes(blob))
    lines.append(embed_arrays([("constexpr u8 kEmojiPoolPacked[]", PACKED_FILE)]).rstrip("\n"))
    return "\n".join(lines) + "\n", packed


def main():
    output = pathlib.Path(sys.argv[1])
    sources = [pathlib.Path(path).read_text(encoding="utf-8") for path in sys.argv[2:4]]
    text, packed = generate(*sources)
    output.parent.mkdir(parents=True, exist_ok=True)
    write_if_changed(output.with_name(PACKED_FILE), packed)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)


if __name__ == "__main__":
    main()
