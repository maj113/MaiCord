"""Generate tests/fixtures/image_decode_corpus.hpp: small synthetic images in
the PNG, JPEG and WebP variants message attachments use.

Every image is drawn from a formula; nothing is downloaded. PNGs come from a
writer here (all colour types and bit depths, Adam7, split IDAT runs, tRNS,
ancillary chunks, APNG) with exact expected scanline hashes. JPEGs come from
Pillow plus a baseline encoder here for sampling layouts Pillow cannot select
(4:4:0, 4:1:1) and restart intervals; their oracle is Pillow's decode. WebPs
come from Pillow. Pillow is a development-only dependency.

Usage: python tools/generate_image_decode_corpus.py [--check]
"""
import io
import math
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image

from font_reference.embed_fixture import array_block, write_files

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / 'tests' / 'fixtures' / 'image_decode_corpus.hpp'


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


# ---- PNG ---------------------------------------------------------------------
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)


def sample(x, y, c, depth, color_type, palette_size):
    if color_type == 3:
        return (x + 2 * y + (x * y) % 3) % palette_size
    top = (1 << depth) - 1
    return (x * 37 + y * 91 + c * 53 + (x * y % 7) * 11 + (x * 4099 if depth == 16 else 0)) & top


def pack_row(xs, y, depth, color_type, palette_size):
    channels = CHANNELS[color_type]
    if depth >= 8:
        out = bytearray()
        for x in xs:
            for c in range(channels):
                v = sample(x, y, c, depth, color_type, palette_size)
                out += bytes([v]) if depth == 8 else struct.pack('>H', v)
        return bytes(out)
    bits = bytearray((len(xs) * depth + 7) // 8)
    for i, x in enumerate(xs):
        v = sample(x, y, 0, depth, color_type, palette_size)
        bit = i * depth
        bits[bit >> 3] |= v << (8 - depth - (bit & 7))
    return bytes(bits)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filter_rows(rows, bpp):
    out = bytearray()
    previous = bytes(len(rows[0])) if rows else b''
    for index, row in enumerate(rows):
        kind = index % 5
        out.append(kind)
        for x, v in enumerate(row):
            a = row[x - bpp] if x >= bpp else 0
            b = previous[x]
            c = previous[x - bpp] if x >= bpp else 0
            predicted = [0, a, b, (a + b) >> 1, paeth(a, b, c)][kind]
            out.append((v - predicted) & 255)
        previous = row
    return bytes(out)


def png_image(width, height, color_type, depth, interlace=0, split=None, palette_size=7,
              trns=None, extra=(), animation=None):
    channels = CHANNELS[color_type]
    bpp = max(1, channels * depth // 8)
    progressive = [pack_row(range(width), y, depth, color_type, palette_size) for y in range(height)]
    if interlace:
        raw = b''
        for x0, y0, dx, dy in ADAM7:
            xs = list(range(x0, width, dx))
            ys = list(range(y0, height, dy))
            if xs and ys:
                raw += filter_rows([pack_row(xs, y, depth, color_type, palette_size) for y in ys], bpp)
    else:
        raw = filter_rows(progressive, bpp)
    body = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, depth, color_type, 0, 0, interlace))
    for kind, data in extra:
        body += chunk(kind, data)
    if color_type == 3:
        body += chunk(b'PLTE', bytes(v for i in range(palette_size) for v in ((i * 40) & 255, (255 - i * 30) & 255, (i * 77) & 255)))
    if trns is not None:
        body += chunk(b'tRNS', trns)
    if animation:
        body += chunk(b'acTL', struct.pack('>II', 2, 0))
        body += chunk(b'fcTL', struct.pack('>IIIIIHHBB', 0, width, height, 0, 0, 1, 10, 0, 0))
    compressed = zlib.compress(raw, 9)
    pieces = [compressed] if not split else [compressed[i:i + split] for i in range(0, len(compressed), split)]
    for index, piece in enumerate(pieces):
        body += chunk(b'IDAT', piece)
        if split and index == 1:
            body += chunk(b'IDAT', b'')  # zero-length IDAT inside the run
    expected = fnv1a(b''.join(progressive))
    if animation:
        # Frame 1: a 5x4 region at (2,1), split over three fdAT chunks.
        fw, fh = 5, 4
        frame_rows = [bytes((x * 13 + y * 29 + c) & 255 for x in range(fw) for c in range(channels)) for y in range(fh)]
        body += chunk(b'fcTL', struct.pack('>IIIIIHHBB', 1, fw, fh, 2, 1, 1, 10, 0, 0))
        data = zlib.compress(filter_rows(frame_rows, bpp), 9)
        third = max(1, len(data) // 3)
        for sequence, start in enumerate((0, third, 2 * third)):
            end = len(data) if start == 2 * third else start + third
            body += chunk(b'fdAT', struct.pack('>I', 2 + sequence) + data[start:end])
        expected = fnv1a(b''.join(frame_rows))
    body += chunk(b'IEND', b'')
    return body, expected


def png_entries():
    entries = []
    for interlace in (0, 1):
        for color_type, depths in ((0, (1, 2, 4, 8, 16)), (2, (8, 16)), (3, (1, 2, 4, 8)), (4, (8, 16)), (6, (8, 16))):
            for depth in depths:
                palette = min(7, 1 << depth)
                data, expected = png_image(13, 11, color_type, depth, interlace, palette_size=palette)
                entries.append((f'png_ct{color_type}_{depth}bit{"_adam7" if interlace else ""}', 'png', data, 13, 11, expected))
    data, expected = png_image(9, 7, 0, 8, trns=struct.pack('>H', 37))
    entries.append(('png_gray_trns', 'png', data, 9, 7, expected))
    data, expected = png_image(9, 7, 2, 16, trns=struct.pack('>HHH', 1, 2, 3))
    entries.append(('png_rgb16_trns', 'png', data, 9, 7, expected))
    data, expected = png_image(9, 7, 3, 8, trns=bytes([0, 128, 255]), interlace=1)
    entries.append(('png_palette_trns_adam7', 'png', data, 9, 7, expected))
    icc = b'synthetic\x00\x00' + zlib.compress(b'\x00' * 128)
    extra = [(b'iCCP', icc), (b'cHRM', struct.pack('>8I', 31270, 32900, 64000, 33000, 30000, 60000, 15000, 6000)),
             (b'gAMA', struct.pack('>I', 45455)), (b'sBIT', bytes([5, 6, 5, 8])), (b'pHYs', struct.pack('>IIB', 3780, 3780, 1)),
             (b'tEXt', b'Comment\x00synthetic'), (b'eXIf', b'MM\x00\x2a\x00\x00\x00\x08\x00\x00'),
             (b'tIME', struct.pack('>HBBBBB', 2026, 10, 7, 12, 0, 0)), (b'bKGD', struct.pack('>HHH', 1, 2, 3))]
    data, expected = png_image(13, 11, 6, 8, extra=extra)
    entries.append(('png_ancillary_chunks', 'png', data, 13, 11, expected))
    # Regression: the media proxy passes through PNGs with 65+ IDAT chunks.
    data, expected = png_image(40, 30, 6, 8, split=24)
    entries.append(('png_many_idat_chunks', 'png', data, 40, 30, expected))
    data, expected = png_image(40, 30, 6, 16, interlace=1, split=40)
    entries.append(('png_many_idat_chunks_adam7', 'png', data, 40, 30, expected))
    data, expected = png_image(2048, 3, 2, 8)
    entries.append(('png_maximum_width', 'png', data, 2048, 3, expected))
    data, expected = png_image(3, 2048, 0, 1, interlace=1)
    entries.append(('png_maximum_height_adam7', 'png', data, 3, 2048, expected))
    data, expected = png_image(9, 7, 6, 8, animation=True)
    entries.append(('apng_two_frames', 'apng', data, 9, 7, expected))
    return entries


# ---- JPEG --------------------------------------------------------------------
def photo(width, height):
    image = Image.new('RGB', (width, height))
    image.putdata([(
        int(128 + 100 * math.sin(x / 5.0)),
        int(128 + 90 * math.cos(y / 4.0)),
        int((x * 3 + y * 5) % 256 * 0.6 + 50),
    ) for y in range(height) for x in range(width)])
    return image


ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
          28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54,
          47, 55, 62, 63]
LUMA_Q = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51,
          87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101,
          72, 92, 95, 98, 112, 100, 103, 99]
DC_BITS = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
DC_VALS = list(range(12))
AC_BITS = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D]
AC_VALS = bytes.fromhex(
    '01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738393a'
    '434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9'
    'aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa')


def huffman_codes(bits, values):
    codes, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            codes[values[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return codes


class BitWriter:
    def __init__(self):
        self.out, self.acc, self.count = bytearray(), 0, 0

    def put(self, value, length):
        for i in range(length - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.count += 1
            if self.count == 8:
                self.out.append(self.acc)
                if self.acc == 0xFF:
                    self.out.append(0)
                self.acc, self.count = 0, 0

    def align(self):
        while self.count:
            self.put(1, 1)


def baseline_jpeg(image, sampling, restart_interval=0):
    """Baseline Huffman JPEG with arbitrary per-component sampling factors."""
    width, height = image.size
    ycc = image.convert('YCbCr')
    planes = [list(ycc.getchannel(i).tobytes()) for i in range(3)]
    hmax = max(h for h, _ in sampling)
    vmax = max(v for _, v in sampling)
    mcu_w, mcu_h = 8 * hmax, 8 * vmax
    mcus_x, mcus_y = (width + mcu_w - 1) // mcu_w, (height + mcu_h - 1) // mcu_h
    q = [max(1, min(255, (v * 50 + 50) // 100)) for v in LUMA_Q]
    dc, ac = huffman_codes(DC_BITS, DC_VALS), huffman_codes(AC_BITS, list(AC_VALS))

    def component_sample(c, x, y):
        h, v = sampling[c]
        sx, sy = hmax // h, vmax // v
        total = 0
        for dy in range(sy):
            for dx in range(sx):
                total += planes[c][min(height - 1, y * sy + dy) * width + min(width - 1, x * sx + dx)]
        return total / (sx * sy)

    def block(c, bx, by):
        values = [component_sample(c, bx * 8 + i % 8, by * 8 + i // 8) - 128 for i in range(64)]
        out = []
        for v in range(8):
            for u in range(8):
                s = 0.0
                for y in range(8):
                    for x in range(8):
                        s += values[y * 8 + x] * math.cos((2 * x + 1) * u * math.pi / 16) * math.cos((2 * y + 1) * v * math.pi / 16)
                cu = math.sqrt(0.5) if u == 0 else 1.0
                cv = math.sqrt(0.5) if v == 0 else 1.0
                out.append(int(round(0.25 * cu * cv * s / q[v * 8 + u])))
        return out

    def magnitude(value):
        size = abs(value).bit_length()
        return size, (value if value >= 0 else value + (1 << size) - 1)

    writer, predictors, mcu_index, marker = BitWriter(), [0, 0, 0], 0, 0
    for my in range(mcus_y):
        for mx in range(mcus_x):
            if restart_interval and mcu_index and mcu_index % restart_interval == 0:
                writer.align()
                writer.out += bytes([0xFF, 0xD0 + marker])
                marker = (marker + 1) % 8
                predictors = [0, 0, 0]
            for c, (h, v) in enumerate(sampling):
                for by in range(v):
                    for bx in range(h):
                        coefficients = block(c, mx * h + bx, my * v + by)
                        diff = coefficients[0] - predictors[c]
                        predictors[c] = coefficients[0]
                        size, bits = magnitude(diff)
                        writer.put(*dc[size])
                        writer.put(bits, size)
                        run = 0
                        for k in range(1, 64):
                            value = coefficients[ZIGZAG[k]]
                            if value == 0:
                                run += 1
                                continue
                            while run > 15:
                                writer.put(*ac[0xF0])
                                run -= 16
                            size, bits = magnitude(value)
                            writer.put(*ac[(run << 4) | size])
                            writer.put(bits, size)
                            run = 0
                        if run:
                            writer.put(*ac[0x00])
            mcu_index += 1
    writer.align()

    def segment(marker_byte, payload):
        return bytes([0xFF, marker_byte]) + struct.pack('>H', len(payload) + 2) + payload

    out = b'\xff\xd8' + segment(0xE0, b'JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00')
    out += segment(0xDB, b'\x00' + bytes(q[ZIGZAG[k]] for k in range(64)))
    out += segment(0xC0, struct.pack('>BHHB', 8, height, width, 3) +
                   b''.join(bytes([c + 1, (h << 4) | v, 0]) for c, (h, v) in enumerate(sampling)))
    out += segment(0xC4, b'\x00' + bytes(DC_BITS) + bytes(DC_VALS) + b'\x10' + bytes(AC_BITS) + AC_VALS)
    if restart_interval:
        out += segment(0xDD, struct.pack('>H', restart_interval))
    out += segment(0xDA, b'\x03\x01\x00\x02\x00\x03\x00\x00\x3f\x00') + bytes(writer.out) + b'\xff\xd9'
    return out


def rgba_samples(image):
    rgba = image.convert('RGBA')
    width, height = rgba.size
    return [rgba.getpixel(((width - 1) * i // 3, (height - 1) * j // 3)) for j in range(4) for i in range(4)]


def jpeg_entries():
    entries = []
    source = photo(33, 19)
    for name, options in (('jpeg_444', dict(subsampling=0)), ('jpeg_422', dict(subsampling=1)),
                          ('jpeg_420_odd', dict(subsampling=2)), ('jpeg_progressive_restart', dict(subsampling=2, progressive=True, restart_marker_blocks=3)),
                          ('jpeg_restart_rows', dict(subsampling=1, restart_marker_rows=1))):
        buffer = io.BytesIO()
        source.save(buffer, 'JPEG', quality=90, **options)
        data = buffer.getvalue()
        entries.append((name, 'jpeg', data, 33, 19, rgba_samples(Image.open(io.BytesIO(data)))))
    buffer = io.BytesIO()
    source.convert('L').save(buffer, 'JPEG', quality=90)
    data = buffer.getvalue()
    entries.append(('jpeg_grayscale', 'jpeg', data, 33, 19, rgba_samples(Image.open(io.BytesIO(data)))))
    # Grayscale declaring 2x2 sampling: legal, and the scan layout is unchanged.
    sof = data.index(bytes([0xFF, 0xC0]))
    patched = bytearray(data)
    assert patched[sof + 11] == 0x11
    patched[sof + 11] = 0x22
    data = bytes(patched)
    entries.append(('jpeg_grayscale_sampling_22', 'jpeg', data, 33, 19, rgba_samples(Image.open(io.BytesIO(data)))))
    for name, sampling, restart in (('jpeg_440', ((1, 2), (1, 1), (1, 1)), 0), ('jpeg_420_restart', ((2, 2), (1, 1), (1, 1)), 2),
                                    ('jpeg_440_restart', ((1, 2), (1, 1), (1, 1)), 1)):
        data = baseline_jpeg(source, sampling, restart)
        entries.append((name, 'jpeg', data, 33, 19, rgba_samples(Image.open(io.BytesIO(data)))))
    return entries


# ---- WebP --------------------------------------------------------------------
def translucent(width, height):
    image = photo(width, height).convert('RGBA')
    image.putalpha(Image.frombytes('L', (width, height), bytes((x * 17 + y * 9) & 255 for y in range(height) for x in range(width))))
    return image


def webp_entries():
    entries = []
    source = translucent(21, 13)
    buffer = io.BytesIO()
    source.save(buffer, 'WEBP', lossless=True)
    data = buffer.getvalue()
    entries.append(('webp_lossless_alpha', 'webp_lossless', data, 21, 13, fnv1a(Image.open(io.BytesIO(data)).convert('RGBA').tobytes())))
    for name, image, options in (('webp_lossy', photo(21, 13), dict(quality=80)),
                                 ('webp_lossy_alpha', source, dict(quality=80, alpha_quality=100)),
                                 ('webp_lossy_alpha_filtered', source, dict(quality=70, alpha_quality=100, alpha_filtering=2))):
        buffer = io.BytesIO()
        image.save(buffer, 'WEBP', **options)
        data = buffer.getvalue()
        alpha = Image.open(io.BytesIO(data)).convert('RGBA').getchannel('A').tobytes()
        entries.append((name, 'webp_lossy', data, 21, 13, fnv1a(alpha)))
    frames = [translucent(16, 12).rotate(angle) for angle in (0, 90, 180)]
    for name, lossless in (('webp_animated_lossless', True), ('webp_animated_lossy', False)):
        buffer = io.BytesIO()
        frames[0].save(buffer, 'WEBP', save_all=True, append_images=frames[1:], duration=[40, 50, 60], loop=0,
                       lossless=lossless, quality=80, minimize_size=False, allow_mixed=False)
        data = buffer.getvalue()
        first = Image.open(io.BytesIO(data))
        expected = fnv1a(first.convert('RGBA').tobytes()) if lossless else 0
        entries.append((name, 'webp_animated', data, 16, 12, expected))
    return entries


KINDS = {'png': 0, 'apng': 1, 'jpeg': 2, 'webp_lossless': 3, 'webp_lossy': 4, 'webp_animated': 5}


def inline_array(declaration, data):
    wrapped = ',\n    '.join(','.join(str(b) for b in data[i:i + 32]) for i in range(0, len(data), 32))
    return f'{declaration}{{\n    {wrapped}}};\n'


def emit(entries):
    lines = ['#pragma once', '#include "maicord/core/base.hpp"', 'namespace maicord {',
             '// Generated by tools/generate_image_decode_corpus.py; synthetic images only.',
             '// expected: PNG/APNG FNV-1a of unfiltered rows (APNG: frame 1), lossless WebP and',
             '// animated lossless WebP FNV-1a of frame-0 RGBA, lossy WebP FNV-1a of its alpha',
             '// plane. JPEG samples: 4x4 grid of Pillow-decoded RGBA at (w-1)*i/3, (h-1)*j/3.',
             'enum class ImageCorpusKind : u8 { png, apng, jpeg, webp_lossless, webp_lossy, webp_animated };',
             'struct ImageCorpusEntry { const char* name; ImageCorpusKind kind; const u8* data; u32 bytes; u32 width, height, expected; u8 samples[16][4]; };']
    block, files = array_block(OUTPUT.with_suffix(''),
                               [(f'inline constexpr u8 kImageCorpus{index}[]', f'kImageCorpus{index}', data)
                                for index, (_, _, data, _, _, _) in enumerate(entries)], inline_array)
    lines.append(block.rstrip('\n'))
    lines.append('inline constexpr ImageCorpusEntry kImageCorpus[]{')
    for index, (name, kind, data, width, height, oracle) in enumerate(entries):
        if isinstance(oracle, list):
            expected, samples = 0, '{' + ','.join('{%d,%d,%d,%d}' % p for p in oracle) + '}'
        else:
            expected, samples = oracle, '{}'
        lines.append(f'    {{"{name}", ImageCorpusKind::{kind}, kImageCorpus{index}, {len(data)}, {width}, {height}, 0x{expected:08x}u, {samples}}},')
    lines += ['};', '} // namespace maicord', '']
    return '\n'.join(lines), files


def main():
    text, files = emit(png_entries() + jpeg_entries() + webp_entries())
    if '--check' in sys.argv:
        current = OUTPUT.read_text(encoding='utf-8') if OUTPUT.exists() else ''
        if current != text or any(not path.is_file() or path.read_bytes() != data for path, data in files.items()):
            print(f'{OUTPUT} is stale; rerun {Path(__file__).name}')
            return 1
        return 0
    write_files(files)
    OUTPUT.write_text(text, encoding='utf-8', newline='\n')
    print(f'wrote {OUTPUT} ({len(text)} bytes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
