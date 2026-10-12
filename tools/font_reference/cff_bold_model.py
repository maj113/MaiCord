"""QA-only compact CFF control-polygon bold model; never linked into the app.

Consumes saved --geometry-dump reference observations. fontTools only decodes
test inputs. Deliberately restricts this experiment to uniform CFF1 matrices
and charstrings supported by the recording pen. Flex round trips are not yet
modeled by this recording-pen adapter.
"""
import argparse
import json
import math
import struct
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.pens.recordingPen import DecomposingRecordingPen


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def fixed(value):
    return math.trunc(value + (0.5 if value >= 0 else -0.5))


def commands(font, glyph, oblique=False):
    units = font['head'].unitsPerEm
    matrix = font['CFF '].cff.topDictIndex[0].FontMatrix
    assert matrix[0] > 0 and matrix[0] == matrix[3]
    assert matrix[1] == matrix[2] == matrix[4] == matrix[5] == 0
    scale = fixed(matrix[0] * units * 65536)
    shear = fixed(scale * 87 / 256) if oblique else 0
    glyphs = font.getGlyphSet()
    pen = DecomposingRecordingPen(glyphs)
    glyphs[font.getGlyphOrder()[glyph]].draw(pen)
    result = []
    pending = None
    drawn = False
    for kind, points in pen.value:
        values = [(fixed(x * scale) + fixed(y * shear), fixed(y * scale)) for x, y in points]
        if kind == 'moveTo':
            pending = values
            drawn = False
        elif kind in ('lineTo', 'curveTo'):
            if not drawn:
                result.append((0, pending))
                drawn = True
            result.append((2 if kind == 'lineTo' else 3, values))
        elif kind == 'closePath':
            if drawn:
                result.append((5, []))
        else:
            raise ValueError(kind)
    return result


def widen(commands, matrix_scale, units, oblique=False):
    points, contours, begin = [], [], 0
    for kind, values in commands:
        if kind == 0:
            begin = len(points)
        points.extend(values)
        if kind == 5:
            contours.append((begin, len(points)))
    next_edge, previous_edge, representative = {}, {}, {}
    for begin, end in contours:
        live = [i for i in range(begin, end)
                if points[i] != points[begin if i + 1 == end else i + 1]]
        for at, i in enumerate(live):
            next_edge[i] = live[(at + 1) % len(live)]
            previous_edge[i] = live[at - 1]
        if live:
            target = live[0]
            for i in range(end-1, begin-1, -1):
                if i in next_edge:
                    target = i
                representative[i] = target
    edges = list(next_edge)
    if not edges:
        return commands
    # First endpoint at each strict extremum determines the orientation vote.
    extrema = [max(edges, key=lambda i: points[next_edge[i]][0]),
               min(edges, key=lambda i: points[next_edge[i]][0]),
               max(edges, key=lambda i: points[next_edge[i]][1]),
               min(edges, key=lambda i: points[next_edge[i]][1])]
    votes = 0
    for i in extrema:
        j = next_edge[i]
        a, b, c = points[i], points[j], points[next_edge[j]]
        votes += ((c[0]-b[0])*(a[1]-b[1]) -
                  (c[1]-b[1])*(a[0]-b[0])) > 0
    orientation = 1 if votes >= 2 else -1
    # FontMatrix uses 2.30 for this inverse-scale calculation. Its reciprocal
    # is 16.16, then multiplied by the 16.16 half-strength (655). NeoCastel's
    # rounded matrix distinguishes this from unitsPerEm * 655 by two units.
    inverse_scale = fixed((1 << 46) / fixed(matrix_scale * (1 << 30)))
    multiply = lambda a, b: (a*b+32768) >> 16
    scale = fixed(matrix_scale * units * 65536)
    vertical = horizontal = multiply(inverse_scale, scale)
    if oblique:
        vertical += multiply(multiply(inverse_scale, fixed(scale*87/256)), 0x561e)
    width_x, width_y = multiply(horizontal, 655), multiply(vertical, 655)

    def shifted(i):
        a, b = points[i], points[next_edge[i]]
        dx, dy = orientation*(b[0]-a[0]), orientation*(b[1]-a[1])
        sx = width_x if dy > 0 else -width_x if dy < 0 else 0
        sy = -width_y if dx > 0 else width_y if dx < 0 else 0
        return (a[0]+sx, a[1]+sy), (b[0]+sx, b[1]+sy)

    widened = {}
    for i in edges:
        a, b = shifted(previous_edge[i])
        c, d = shifted(i)
        x, y = c
        if b != c:
            dx, dy = float(b[0])-a[0], float(b[1])-a[1]
            ex, ey = float(d[0])-c[0], float(d[1])-c[1]
            determinant = dx*ey-dy*ex
            if determinant:
                t = ((float(a[1])-c[1])*ex-(float(a[0])-c[0])*ey)/determinant
                x, y = math.trunc(dx*t+a[0]), math.trunc(dy*t+a[1])
            x = min(points[i][0]+width_x, max(points[i][0]-width_x, x))
            y = min(points[i][1]+width_y, max(points[i][1]-width_y, y))
        widened[i] = (x+width_x, y+width_y)
    output, at = [], 0
    for kind, values in commands:
        revised = []
        for point in values:
            revised.append(widened[representative[at]] if at in representative else point)
            at += 1
        output.append((kind, revised))
    return output


def encode(commands, units, glyph):
    inverse = f32(1.0/units)
    coordinates, payload = [], []
    for kind, points in commands:
        values = []
        for x, y in points:
            x, y = f32(f32(x/65536)*inverse), f32(f32(-y/65536)*inverse)
            values += [bits(x), bits(y)]
            coordinates.append((x, y))
        payload += values + [0]*(6-len(values)) + [kind]
    box = ([min(x for x, y in coordinates), min(y for x, y in coordinates),
            max(x for x, y in coordinates), max(y for x, y in coordinates)]
           if coordinates else [0]*4)
    return [glyph, len(commands), 1, int(bool(coordinates))] + [bits(v) for v in box] + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('font', type=Path)
    parser.add_argument('observations', nargs='+', type=Path)
    parser.add_argument('--bold', action='store_true')
    parser.add_argument('--oblique', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    font = TTFont(args.font)
    rows = []
    for path in args.observations:
        for line in path.read_text(errors='replace').splitlines():
            if not line.startswith('gvar_outline='):
                continue
            expected = list(map(int, line.split('=', 1)[1].split(',')))
            glyph = expected[0]
            try:
                raw = commands(font, glyph, args.oblique)
                actual = encode(widen(raw, font['CFF '].cff.topDictIndex[0].FontMatrix[0],
                                      font['head'].unitsPerEm, args.oblique) if args.bold else raw,
                                font['head'].unitsPerEm, glyph)
                row = dict(glyph=glyph, exact=actual == expected)
                if not row['exact']:
                    row.update(candidate=actual, reference=expected)
            except Exception as error:
                row = dict(glyph=glyph, exact=False, error=str(error))
            rows.append(row)
    assert rows, 'No reference observations'
    args.output.write_text(json.dumps(dict(font=str(args.font), bold=args.bold, oblique=args.oblique,
        compared=len(rows), exact=sum(row['exact'] for row in rows), rows=rows), indent=2))
    print(f"{sum(row['exact'] for row in rows)}/{len(rows)} exact")
    return 0 if all(row['exact'] for row in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
