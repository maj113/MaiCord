"""Write MaiCord's executable icon: the tray mark on the brand tile.

Standard library only and deterministic. The drawing mirrors render_mark in
src/platform/desktop/system_tray.cpp (rounded tile, white "M" stroke, whose
geometry lives in include/maicord/ui/shell/brand_mark.hpp) with the brand
gradient #8B5CF6 to #C026D3; the running client still re-tints window and tray
icons to the active theme.
PNG filters, DEFLATE strategies and gentle RGB posterization are searched
deterministically. Alpha and the white mark are preserved exactly; each image
must meet the PSNR floor against the original rendering. No external libraries.
Caption sizes always use 8-bit palette indices to minimize decoder code.

The search takes seconds, and its result depends only on this file, --min-psnr and
zlib's version: the icon is kept under that key in app-icon/ of the toolchain cache
($MAICORD_TOOLCHAIN_CACHE, else %LOCALAPPDATA%\\MaiCord-toolchain on Windows and
${XDG_CACHE_HOME:-~/.cache}/maicord-toolchain elsewhere), and a later run copies it.
Without a writable cache every run searches.

  python generate_app_icon.py --output maicord.ico
"""
import argparse
from collections import Counter
from functools import lru_cache
import hashlib
import json
import math
from operator import mul, sub
import os
from pathlib import Path
import struct
import tempfile
import zlib

SIZES = (16, 20, 24, 32, 40, 48, 64, 256)
MIN_PSNR = 46.0
# A filtered byte's magnitude as a signed residual.
RESIDUAL = bytes(min(v, 256 - v) for v in range(256))
# MaiCord's brand tile: a violet-to-magenta gradient from the top edge to the bottom edge, and the white mark.
BRAND = (0x8B / 255, 0x5C / 255, 0xF6 / 255)
SHADE = (0xC0 / 255, 0x26 / 255, 0xD3 / 255)
GLYPH = (1.0, 1.0, 1.0)
POINTS = ((.28, .71), (.28, .31), (.5, .55), (.72, .31), (.72, .71))


def clamp01(value):
    return 0.0 if value < 0 else 1.0 if value > 1 else value


def segment_distance(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    t = clamp01(((px - ax) * dx + (py - ay) * dy) / length) if length > 0 else 0.0
    return math.hypot(px - (ax + dx * t), py - (ay + dy * t))


def rounded_distance(px, py, centre, half, radius):
    qx = abs(px - centre) - (half - radius)
    qy = abs(py - centre) - (half - radius)
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - radius


def render(extent):
    e = float(extent)
    c = e * .5
    half_stroke = max(e * .072, .85)
    segments = [(POINTS[i][0] * e, POINTS[i][1] * e, POINTS[i + 1][0] * e, POINTS[i + 1][1] * e)
                for i in range(4)]
    rows = bytearray()
    for y in range(extent):
        rows.append(0)  # PNG filter: none
        py = y + .5
        t = py / e
        base = tuple(BRAND[k] + (SHADE[k] - BRAND[k]) * t for k in range(3))
        for x in range(extent):
            px = x + .5
            tile = clamp01(.5 - rounded_distance(px, py, c, c, e * .3))
            if tile <= 0:
                rows += b'\0\0\0\0'
                continue
            d = min(segment_distance(px, py, *segment) for segment in segments)
            glyph = clamp01(half_stroke - d + .5)
            color = (base[k] + (GLYPH[k] - base[k]) * glyph for k in range(3))
            rows += bytes(int(clamp01(v) * 255 + .5) for v in color)
            rows.append(int(tile * 255 + .5))
    return bytes(rows)


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def original_pixels(extent):
    rows = render(extent)
    stride = extent * 4 + 1
    return b''.join(rows[y * stride + 1:(y + 1) * stride] for y in range(extent))


def posterize(pixels, bits):
    levels = (1 << bits) - 1
    table = bytes(((value * levels + 127) // 255 * 255 + levels // 2) // levels
                  for value in range(256))
    result = bytearray(pixels.translate(table))
    result[3::4] = pixels[3::4]
    return bytes(result)


def quantize(pixels, limit):
    """Weighted median cut within each alpha level, keeping white exact.

    Quantization and PNG storage are independent: the reduced colors can
    compress well in an RGBA PNG too, without a palette chunk or runtime work.
    """
    counts = Counter(tuple(pixels[i:i + 4]) for i in range(0, len(pixels), 4))
    if len(counts) <= limit:
        return pixels
    fixed = {color: color for color in counts if color[3] == 0 or color == (255, 255, 255, 255)}
    groups = {}
    for color in counts:
        if color not in fixed:
            groups.setdefault(color[3], []).append(color)

    def box(colors):
        weight = sum(counts[color] for color in colors)
        mean = tuple(sum(color[k] * counts[color] for color in colors) / weight for k in range(3))
        variance = tuple(sum((color[k] - mean[k]) ** 2 * counts[color] for color in colors)
                         for k in range(3))
        return colors, mean, variance, weight

    boxes = [box(colors) for colors in groups.values()]
    if len(boxes) + len(fixed) > limit:
        return None
    while len(boxes) + len(fixed) < limit:
        index = max(range(len(boxes)), key=lambda i: sum(boxes[i][2]))
        colors, mean, variance, weight = boxes[index]
        if not any(variance):
            break
        channel = max(range(3), key=variance.__getitem__)
        colors = sorted(colors, key=lambda color: (color[channel], color))
        accumulated = 0
        split = 1
        for split, color in enumerate(colors[:-1], 1):
            accumulated += counts[color]
            if accumulated * 2 >= weight:
                break
        boxes[index:index + 1] = [box(colors[:split]), box(colors[split:])]
    lookup = dict(fixed)
    for colors, mean, variance, weight in boxes:
        representative = tuple(int(v + .5) for v in mean) + (colors[0][3],)
        lookup.update((color, representative) for color in colors)
    return bytes(v for i in range(0, len(pixels), 4) for v in lookup[tuple(pixels[i:i + 4])])


def psnr(reference, pixels):
    difference = list(map(sub, reference, pixels))
    error = sum(map(mul, difference, difference))
    return 10 * math.log10(255 * 255 * len(reference) / error) if error else math.inf


def composite_psnr(reference, pixels, background):
    # Alpha is exact, so this measures visible RGB error on either background.
    error = 0.0
    for offset in range(0, len(reference), 4):
        alpha = reference[offset + 3] / 255
        other_alpha = pixels[offset + 3] / 255
        for channel in range(3):
            a = reference[offset + channel] * alpha + background * (1 - alpha)
            b = pixels[offset + channel] * other_alpha + background * (1 - other_alpha)
            error += (a - b) ** 2
    samples = len(reference) // 4 * 3
    return 10 * math.log10(255 * 255 * samples / error) if error else math.inf


def filter_rows(rows, bpp):
    """Try all five PNG filters globally and two adaptive row heuristics.

    A row is one big integer: the byte-wise subtraction and the rounded average of
    the Sub, Up and Average filters run on all bytes at once.
    """
    size = len(rows[0])
    high = int.from_bytes(b'\x80' * size, 'little')
    low = high - (high >> 7)
    mask = (1 << 8 * size) - 1
    # Entropy of a byte seen n times in a row of `size`, as the per-row sum adds it up.
    bits = [0.0] + [n * math.log2(size / n) for n in range(1, size + 1)]

    def minus(x, y):
        return (((x | high) - (y & low)) ^ (((x ^ y) & high) ^ high)).to_bytes(size, 'little')

    streams = [bytearray() for _ in range(7)]
    previous = bytes(size)
    for row in rows:
        x, b = int.from_bytes(row, 'little'), int.from_bytes(previous, 'little')
        a, c = (x << 8 * bpp) & mask, (b << 8 * bpp) & mask
        paeth = int.from_bytes(bytes([
            left if pa <= pb and pa <= pc else up if pb <= pc else corner
            for left, up, corner in zip(a.to_bytes(size, 'little'), previous, c.to_bytes(size, 'little'))
            for pa, pb, pc in ((abs(up - corner), abs(left - corner), abs(left + up - 2 * corner)),)]), 'little')
        variants = [bytes(row)] + [minus(x, predicted)
                                   for predicted in (a, b, (a & b) + (((a ^ b) >> 1) & low), paeth)]
        for kind, encoded in enumerate(variants):
            streams[kind].append(kind)
            streams[kind].extend(encoded)
        # Signed residual magnitude and entropy prefer different rows on small
        # flat artwork. The actual compressed byte count decides between them.
        scores = [sum(encoded.translate(RESIDUAL)) for encoded in variants]
        entropies = [sum(map(bits.__getitem__, Counter(encoded).values())) for encoded in variants]
        for stream, costs in zip(streams[5:], (scores, entropies)):
            kind = min(range(5), key=costs.__getitem__)
            stream.append(kind)
            stream.extend(variants[kind])
        previous = row
    return tuple(dict.fromkeys(bytes(stream) for stream in streams))


def deflate_smallest(streams):
    best = None
    for stream in streams:
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED, zlib.Z_RLE):
            for memory in (8, 9):
                compressor = zlib.compressobj(9, zlib.DEFLATED, 15, memory, strategy)
                data = compressor.compress(stream) + compressor.flush()
                if best is None or len(data) < len(best):
                    best = data
    return best


def encode_png(extent, pixels, indexed=False):
    def encode(rows, bpp, color_type, palette=b''):
        header = struct.pack('>IIBBBBB', extent, extent, 8, color_type, 0, 0, 0)
        return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + palette +
                chunk(b'IDAT', deflate_smallest(filter_rows(rows, bpp))) + chunk(b'IEND', b''))

    if not indexed:
        return encode([pixels[y * extent * 4:(y + 1) * extent * 4] for y in range(extent)], 4, 6)
    colors = Counter(tuple(pixels[i:i + 4]) for i in range(0, len(pixels), 4))
    if len(colors) > 256:
        return None
    # Keep nonopaque entries first so tRNS need not store opaque alphas.
    orders = (sorted(colors, key=lambda c: (c[3] == 255, c)),
              sorted(colors, key=lambda c: (c[3] == 255, -colors[c], c)))
    candidates = []
    for palette in orders:
        lookup = {color: i for i, color in enumerate(palette)}
        indices = bytes(lookup[tuple(pixels[i:i + 4])] for i in range(0, len(pixels), 4))
        rows = [indices[y * extent:(y + 1) * extent] for y in range(extent)]
        alpha = bytes(color[3] for color in palette).rstrip(b'\xff')
        chunks = chunk(b'PLTE', bytes(v for color in palette for v in color[:3]))
        if alpha:
            chunks += chunk(b'tRNS', alpha)
        candidates.append(encode(rows, 1, 3, chunks))
    return min(candidates, key=len)


@lru_cache(maxsize=len(SIZES))
def optimize(extent, min_psnr=MIN_PSNR, encoding=None):
    # Caption decoding only needs images through 64 pixels. Always emit an
    # 8-bit indexed PNG there, even in lossless mode, to keep runtime code small.
    # The 256-pixel shell image may use RGBA if lossless colors exceed 256.
    if encoding is None:
        encoding = 'palette' if extent <= 64 else 'auto'
    reference = original_pixels(extent)
    best = None
    candidates = [(f'rgb{bits}', reference if bits == 8 else posterize(reference, bits))
                  for bits in (8, 7, 6, 5)]
    candidates += [(f'colors{limit}', quantize(reference, limit)) for limit in (32, 48, 64, 96, 128, 256)]
    seen = set()
    for method, pixels in candidates:
        if pixels is None or pixels in seen:
            continue
        seen.add(pixels)
        quality = psnr(reference, pixels)
        visible = min(composite_psnr(reference, pixels, background) for background in (0, 255))
        if min(quality, visible) < min_psnr:
            continue
        for indexed in ((False, True) if encoding == 'auto' else (encoding == 'palette',)):
            data = encode_png(extent, pixels, indexed)
            if data is not None and (best is None or len(data) < len(best[0])):
                best = data, pixels, dict(size=extent, bytes=len(data), method=method,
                                         encoding='palette' if indexed else 'rgba',
                                         rgba_psnr_db=quality, composite_psnr_db=visible)
    if best is None:
        raise ValueError('No encoding meets the requested quality and palette size')
    return best


def png(extent, min_psnr=MIN_PSNR):
    return optimize(extent, min_psnr)[0]


def pack_icon(images):
    directory = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for extent, image in zip(SIZES, images):
        side = 0 if extent >= 256 else extent
        directory += struct.pack('<BBBBHHII', side, side, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    return directory + b''.join(images)


def icon(min_psnr=MIN_PSNR):
    return pack_icon([png(extent, min_psnr) for extent in SIZES])


def unpack_icon(data):
    """The PNG images of an icon pack_icon wrote, in SIZES order (None when it is not one)."""
    if len(data) < 6 or struct.unpack_from('<HHH', data) != (0, 1, len(SIZES)):
        return None
    images = []
    for index, extent in enumerate(SIZES):
        side, _, _, _, _, _, size, offset = struct.unpack_from('<BBBBHHII', data, 6 + 16 * index)
        if side != (0 if extent >= 256 else extent) or offset + size > len(data):
            return None
        images.append(data[offset:offset + size])
    return images


def cache_path(min_psnr):
    root = os.environ.get('MAICORD_TOOLCHAIN_CACHE')
    if not root and os.name == 'nt':
        root = os.environ.get('LOCALAPPDATA') and os.path.join(os.environ['LOCALAPPDATA'], 'MaiCord-toolchain')
    elif not root:
        root = os.path.join(os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'), 'maicord-toolchain')
    if not root:
        return None
    key = hashlib.sha256(Path(__file__).read_bytes() + b'\0' + repr(min_psnr).encode() + b'\0' +
                         zlib.ZLIB_RUNTIME_VERSION.encode()).hexdigest()
    return Path(root) / 'app-icon' / f'{key}.ico'


def cached_icon(min_psnr):
    """The icon of the cache, else the searched one, which then joins the cache."""
    path = cache_path(min_psnr)
    if path and path.is_file():
        data = path.read_bytes()
        if unpack_icon(data) is not None:
            return data
    data = icon(min_psnr)
    if path:
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            # Written aside and renamed, so concurrent builds never read a partial icon.
            with tempfile.NamedTemporaryFile(dir=path.parent, suffix='.tmp', delete=False) as file:
                file.write(data)
            os.replace(file.name, path)
        except OSError:
            pass  # an unwritable cache (a sandboxed package build): every run searches
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--png-directory', type=Path, help='also export each optimized PNG')
    parser.add_argument('--report', type=Path, help='write size and PSNR measurements as JSON')
    parser.add_argument('--min-psnr', type=float, default=MIN_PSNR,
                        help='minimum RGBA and composited RGB PSNR in dB (default: 46; inf: lossless)')
    args = parser.parse_args()
    if math.isnan(args.min_psnr) or args.min_psnr < 0:
        parser.error('--min-psnr must be nonnegative')
    output = args.output
    data = cached_icon(args.min_psnr)
    if not output.exists() or output.read_bytes() != data:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
    if args.png_directory:
        args.png_directory.mkdir(parents=True, exist_ok=True)
        for extent, image in zip(SIZES, unpack_icon(data)):
            (args.png_directory / f'maicord-{extent}-optimized.png').write_bytes(image)
    if args.report:
        report = []
        for extent in SIZES:
            item = dict(optimize(extent, args.min_psnr)[2])
            original = zlib.compress(render(extent), 9)
            item['original_bytes'] = len(original) + 57
            for key in ('rgba_psnr_db', 'composite_psnr_db'):
                if math.isinf(item[key]):
                    item[key] = None  # null means lossless / infinite PSNR.
            report.append(item)
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
