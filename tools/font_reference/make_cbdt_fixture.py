"""First-party CBDT/CBLC fixture: a minimal sfnt that holds only the two colour bitmap tables.

No font library is needed: the sfnt container is a table directory and two tables, and the PNG images are written
with zlib. The fixture has three strikes (ppem 16, 32 and 64), every index subtable format (1 to 5) and every PNG image
format (17, 18, 19), so tests/text/opentype/cbdt_test.cpp reads each path of src/text/opentype/cbdt_font.cpp:

  glyph  strikes      index format / image format   image
  1      16, 32, 64   1 / 17 (16, 64), 1 / 18 (32)  quadrants: red, green / blue, transparent; bearing (2, 8) x ppem/16
  2      16, 32, 64   1 / 17                        opaque (200, 100, 50); bearing (1, 7) x ppem/16, odd on purpose
  3      16, 32       2 / 19                        4x4, (255, 255, 255) at alpha 128; metrics in the index
  4      16           3 / 17                        6x6 opaque (10, 20, 30)
  6, 8   16           5 / 19                        glyph 6: 5x5 opaque (1, 2, 3); glyph 8: 5x5 opaque (4, 5, 6)
  10, 12 16           4 / 18                        glyph 10: 7x7 opaque (7, 8, 9); glyph 12: 7x7 opaque (11, 12, 13)

Strike 16 covers glyphs 1 to 12, strike 32 glyphs 1 to 3 and strike 64 glyphs 1 and 2. Run:

    python tools/font_reference/make_cbdt_fixture.py [--output tests/generated/cbdt_fixture.bin]
"""

import argparse
import pathlib
import struct
import zlib


def png(width, height, pixel):
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows += bytes(pixel(x, y))

    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b''))


def quadrants(size):
    half = size // 2

    def pixel(x, y):
        if y < half:
            return (255, 0, 0, 255) if x < half else (0, 255, 0, 255)
        return (0, 0, 255, 255) if x < half else (0, 0, 0, 0)

    return png(size, size, pixel)


def solid(width, height, color):
    return png(width, height, lambda x, y: color)


def small(height, width, bearing_x, bearing_y, advance):
    return struct.pack('>BBbbB', height, width, bearing_x, bearing_y, advance)


def big(height, width, bearing_x, bearing_y, advance):
    return struct.pack('>BBbbBbbB', height, width, bearing_x, bearing_y, advance, 0, 0, 0)


class Strike:
    def __init__(self, ppem, first, last):
        self.ppem, self.first, self.last = ppem, first, last
        self.subtables = []  # (first, last, header + body bytes without the image data offset)


def build():
    cbdt = bytearray(struct.pack('>HH', 3, 0))
    strikes = []

    def image(data):
        offset = len(cbdt)
        cbdt.extend(data)
        return offset

    # Strike 16.
    s16 = Strike(16, 1, 12)
    q = quadrants(8)
    g1 = small(8, 8, 2, 8, 9) + struct.pack('>I', len(q)) + q
    p2 = solid(8, 8, (200, 100, 50, 255))
    g2 = small(8, 8, 1, 7, 9) + struct.pack('>I', len(p2)) + p2
    start = image(g1)
    image(g2)
    # Index format 1: offsets relative to the image data offset.
    s16.subtables.append((1, 2, 1, 17, start, [0, len(g1), len(g1) + len(g2)]))
    p3 = solid(4, 4, (255, 255, 255, 128))
    g3 = struct.pack('>I', len(p3)) + p3
    start3 = image(g3)
    s16.subtables.append((3, 3, 2, 19, start3, ('fixed', len(g3), big(4, 4, 0, 4, 5))))
    p4 = solid(6, 6, (10, 20, 30, 255))
    g4 = small(6, 6, 0, 6, 7) + struct.pack('>I', len(p4)) + p4
    start4 = image(g4)
    s16.subtables.append((4, 4, 3, 17, start4, [0, len(g4)]))
    p6 = solid(5, 5, (1, 2, 3, 255))
    p8 = solid(5, 5, (4, 5, 6, 255))
    g6 = struct.pack('>I', len(p6)) + p6
    g8 = struct.pack('>I', len(p8)) + p8
    # Index format 5 images are the same size; pad the smaller to the larger.
    size5 = max(len(g6), len(g8))
    g6 = g6.ljust(size5, b'\0')
    g8 = g8.ljust(size5, b'\0')
    start5 = image(g6 + g8)
    s16.subtables.append((5, 9, 5, 19, start5, ('sparse', size5, big(5, 5, 0, 5, 6), [6, 8])))
    p10 = solid(7, 7, (7, 8, 9, 255))
    p12 = solid(7, 7, (11, 12, 13, 255))
    g10 = big(7, 7, 0, 7, 8) + struct.pack('>I', len(p10)) + p10
    g12 = big(7, 7, 0, 7, 8) + struct.pack('>I', len(p12)) + p12
    start10 = image(g10 + g12)
    s16.subtables.append((10, 12, 4, 18, start10, ('pairs', [(10, 0), (12, len(g10))], len(g10) + len(g12))))
    strikes.append(s16)

    # Strike 32: glyph 1 as image format 18, glyph 2 as 17, glyph 3 as 19.
    s32 = Strike(32, 1, 3)
    q32 = quadrants(16)
    g1b = big(16, 16, 4, 16, 18) + struct.pack('>I', len(q32)) + q32
    p2b = solid(16, 16, (200, 100, 50, 255))
    g2b = small(16, 16, 2, 14, 18) + struct.pack('>I', len(p2b)) + p2b
    start = image(g1b)
    image(g2b)
    # One image format per subtable: glyphs 1 (format 18) and 2 (format 17) are in separate subtables.
    s32.subtables.append((1, 1, 1, 18, start, [0, len(g1b)]))
    s32.subtables.append((2, 2, 1, 17, start + len(g1b), [0, len(g2b)]))
    p3b = solid(8, 8, (255, 255, 255, 128))
    g3b = struct.pack('>I', len(p3b)) + p3b
    start3b = image(g3b)
    s32.subtables.append((3, 3, 2, 19, start3b, ('fixed', len(g3b), big(8, 8, 0, 8, 10))))
    strikes.append(s32)

    # Strike 64: glyphs 1 and 2, image format 17.
    s64 = Strike(64, 1, 2)
    q64 = quadrants(32)
    g1c = small(32, 32, 8, 32, 36) + struct.pack('>I', len(q64)) + q64
    p2c = solid(32, 32, (200, 100, 50, 255))
    g2c = small(32, 32, 4, 28, 36) + struct.pack('>I', len(p2c)) + p2c
    startc = image(g1c)
    image(g2c)
    s64.subtables.append((1, 2, 1, 17, startc, [0, len(g1c), len(g1c) + len(g2c)]))
    strikes.append(s64)

    # CBLC: header, BitmapSize records, then each strike's subtable array and subtables.
    header = struct.pack('>HHI', 3, 0, len(strikes))
    records_size = 48 * len(strikes)
    body = bytearray()
    records = bytearray()
    base = 8 + records_size
    for strike in strikes:
        array_offset = base + len(body)
        array = bytearray()
        tables = bytearray()
        array_size = 8 * len(strike.subtables)
        for first, last, index_format, image_format, image_offset, payload in strike.subtables:
            table = bytearray(struct.pack('>HHI', index_format, image_format, image_offset))
            count = last - first + 1
            if index_format == 1:
                offsets = payload
                assert len(offsets) == count + 1
                table += b''.join(struct.pack('>I', value) for value in offsets)
            elif index_format == 3:
                offsets = payload
                assert len(offsets) == count + 1
                table += b''.join(struct.pack('>H', value) for value in offsets)
                if len(offsets) % 2:
                    table += b'\0\0'
            elif index_format == 2:
                _, size, metrics = payload
                table += struct.pack('>I', size) + metrics
            elif index_format == 4:
                _, pairs, end = payload
                table += struct.pack('>I', len(pairs))
                for glyph, offset in pairs:
                    table += struct.pack('>HH', glyph, offset)
                table += struct.pack('>HH', 0, end)
            elif index_format == 5:
                _, size, metrics, glyphs = payload
                table += struct.pack('>I', size) + metrics + struct.pack('>I', len(glyphs))
                table += b''.join(struct.pack('>H', value) for value in glyphs)
                if len(glyphs) % 2:
                    table += b'\0\0'
            array += struct.pack('>HHI', first, last, array_size + len(tables))
            tables += table
        body += array + tables
        line = struct.pack('>bbBbbbbbbbbb', 6, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        record = struct.pack('>IIII', array_offset, len(array) + len(tables), len(strike.subtables), 0)
        record += line + line + struct.pack('>HHBBBb', strike.first, strike.last, strike.ppem, strike.ppem, 32, 1)
        assert len(record) == 48
        records += record
    cblc = header + records + body

    def pad(data):
        return bytes(data) + b'\0' * (-len(data) % 4)

    def checksum(data):
        data = pad(data)
        return sum(struct.unpack('>%dI' % (len(data) // 4), data)) & 0xFFFFFFFF

    tables = [(b'CBDT', bytes(cbdt)), (b'CBLC', bytes(cblc))]
    directory = struct.pack('>IHHHH', 0x00010000, len(tables), 32, 1, 0)
    offset = 12 + 16 * len(tables)
    blobs = b''
    for tag, data in tables:
        directory += struct.pack('>4sIII', tag, checksum(data), offset + len(blobs), len(data))
        blobs += pad(data)
    return directory + blobs


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[2] / 'tests/generated/cbdt_fixture.bin')
    args = parser.parse_args()
    data = build()
    args.output.write_bytes(data)
    include = args.output.with_suffix('.inc')
    include.write_text(
        '// First-party CBDT/CBLC fixture (tools/font_reference/make_cbdt_fixture.py): three strikes, index formats 1 to 5,\n'
        '// image formats 17, 18 and 19.\n'
        '#pragma clang diagnostic push\n'
        '#pragma clang diagnostic ignored "-Wc++26-extensions"\n'
        'static constexpr u8 cbdt_fixture[]{\n'
        '#embed "%s"\n'
        '};\n'
        '#pragma clang diagnostic pop\n' % args.output.name, encoding='utf-8', newline='\n')
    print('%s: %d bytes' % (args.output, len(data)))


if __name__ == '__main__':
    main()
