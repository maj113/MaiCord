"""Write MaiCord's executable icon: the tray mark in the default theme colours.

Standard library only and deterministic. The drawing mirrors render_mark in
src/platform/system_tray.cpp (rounded brand tile, white "M" stroke); the
running client still re-tints window and tray icons to the active theme.
Every image is stored as PNG to keep the resource small.

  python generate_app_icon.py --output maicord.ico
"""
import argparse
import math
from pathlib import Path
import struct
import zlib

SIZES = (16, 20, 24, 32, 40, 48, 64, 256)
# Default theme core accent (include/maicord/ui/theme.hpp) and text.on_accent.
BRAND = (0.3451, 0.3961, 0.9490)
SHADE = tuple(channel * 0.78 for channel in BRAND)
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


def png(extent):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    header = struct.pack('>IIBBBBB', extent, extent, 8, 6, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
            chunk(b'IDAT', zlib.compress(render(extent), 9)) + chunk(b'IEND', b''))


def icon():
    images = [png(extent) for extent in SIZES]
    directory = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for extent, image in zip(SIZES, images):
        side = 0 if extent >= 256 else extent
        directory += struct.pack('<BBBBHHII', side, side, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    return directory + b''.join(images)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--output', type=Path, required=True)
    output = parser.parse_args().output
    data = icon()
    if not output.exists() or output.read_bytes() != data:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)


if __name__ == '__main__':
    main()
