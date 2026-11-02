#!/usr/bin/env python3
"""Deflate-pack the emoji name and glyph string pools and both entry tables into one generated include.

Usage: pack_emoji_pools.py OUTPUT CATALOG_INCLUDE SHORTCODE_INCLUDE

The catalog name prefix, shared shortcode names, shared glyphs, the 6-byte catalog and alias rows and the
primary-shortcode bit set form one zlib stream. Generated alias rows use offsets from the shared name suffix.
Rows ship as byte planes of BLOCK-row blocks (a plane of one field deflates far better than interleaved
rows); src/ui/text/emoji_catalog.cpp inflates the stream once, interleaves the rows back in place and checks
the layout constants emitted here (interleave() below is the same permutation, checked against the sources).
"""
import ast
import pathlib
import re
import struct
import sys
import zlib
import optimal_deflate
from font_reference.embed_fixture import embed_arrays, write_if_changed
from generate_unicode_emoji_catalog import pool_strings


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

    cache = optimal_deflate.DeflateCache("emoji")
    best = cache.stream(data, fallback)
    cache.save()
    assert zlib.decompress(best) == data
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



def shared_names(catalog_names, shortcode_names, shortcode_glyphs, shortcode_source):
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
    for name, (_, glyph_offset, name_bytes, glyph_bytes) in zip(names, rows):
        absolute = shared.find(name, SHORTCODE_NAME_BASE)
        relative = absolute - SHORTCODE_NAME_BASE
        if not 0 <= relative < 131072 or shared[absolute:absolute + name_bytes] != name:
            raise SystemExit("Shared shortcode name exceeds its exact 17-bit range")
        remapped.append((relative, glyph_offset, name_bytes, glyph_bytes))
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


def generate(catalog_source, shortcode_source):
    catalog_names = read_pool(catalog_source, "kUnicodeEmojiCatalogNames", True)
    shortcode_names = read_pool(shortcode_source, "kEmojiShortcodeNames", True)
    glyphs = read_pool(catalog_source, "kUnicodeEmojiCatalogGlyphs", False)
    shortcode_glyphs = read_pool(shortcode_source, "kEmojiShortcodeGlyphs", False)
    if not glyphs.startswith(shortcode_glyphs) or len(glyphs) >= 65536:
        raise SystemExit("Catalog must share the exact shortcode glyph prefix within u16 bounds")
    names, aliases = shared_names(catalog_names, shortcode_names, shortcode_glyphs, shortcode_source)
    entries = catalog_rows(catalog_source)
    ranges = category_ranges(catalog_source)
    primary = primary_rows(shortcode_source)
    for name_offset, glyph_offset, name_bytes, glyph_bytes in entries:
        if (not 0 < name_bytes < 128 or not 0 < glyph_bytes < 64 or name_offset + name_bytes > len(catalog_names) or
                glyph_offset + glyph_bytes > len(glyphs) or name_offset >= 65536):
            raise SystemExit("Invalid catalog string range")
    # The primary row of a glyph is looked up by glyph: a set of rows replaces the ordered list only when no
    # two of them share a glyph (emoji_catalog.cpp walks the rows in order and takes the first match).
    if len(set(primary)) != len(primary) or not all(0 <= row < len(aliases) for row in primary):
        raise SystemExit("Invalid primary shortcode rows")
    if len({(aliases[row][1], aliases[row][3]) for row in primary}) != len(primary):
        raise SystemExit("Two primary shortcode rows share a glyph")
    bits = bytearray((len(aliases) + 7) // 8)
    for row in primary:
        bits[row >> 3] |= 1 << (row & 7)
    catalog_bytes = b"".join(struct.pack("<HHBB", *row) for row in entries)
    alias_bytes = b"".join(struct.pack("<HHBB", offset & 0xFFFF, glyph, length | ((offset >> 16) << 7), glyph_bytes)
                           for offset, glyph, length, glyph_bytes in aliases)
    pool = names + glyphs
    pool += bytes(-len(pool) % 2)
    catalog_offset = len(pool)
    alias_offset = catalog_offset + len(catalog_bytes)
    primary_offset = alias_offset + len(alias_bytes)
    blob = pool + planes(catalog_bytes) + planes(alias_bytes) + bytes(bits)
    assert interleave(blob[catalog_offset:alias_offset], len(entries)) == catalog_bytes
    assert interleave(blob[alias_offset:primary_offset], len(aliases)) == alias_bytes
    lines = ["// Generated by tools/pack_emoji_pools.py; do not edit.",
             "constexpr u32 kEmojiPoolCatalogNamesOffset=0;",
             f"constexpr u32 kEmojiPoolCatalogNamePrefixBytes={len(catalog_names)};",
             f"constexpr u32 kEmojiPoolShortcodeNamesOffset={SHORTCODE_NAME_BASE};",
             f"constexpr u32 kEmojiPoolCatalogGlyphsOffset={len(names)};",
             f"constexpr u32 kEmojiPoolShortcodeGlyphsOffset={len(names)};",
             f"constexpr u32 kEmojiPoolCatalogGlyphBytes={len(glyphs)};",
             f"constexpr u32 kEmojiPoolShortcodeGlyphBytes={len(shortcode_glyphs)};",
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
