"""Compile the MaiCord icon manifest into the compact MIPK v1 icon pack.

Standard library only and deterministic: identical inputs produce identical bytes.

  vendor  copy the manifest's Lucide icons from a local lucide package into SVG files
  build   compile SVG sources into a C++ include holding the pack bytes

Pack layout (little endian):
  header   40 bytes: 'MIPK', u16 version, u16 icon count, u16 grid, u16 units per grid unit,
           u32 total bytes, u32 offsets of records, name index, commands, stems, names, u32 0
  records  16 bytes each: u32 command offset, u16 command bytes, u16 outline commands,
           u16 first stem, u8 stem count, u8 flags, u16 name offset, u8 name length, u8 0
  index    u16 record numbers sorted by name
  commands u8 opcode + i16 coordinates in 1/64 grid units:
           0 move (stroked contour), 1 move (filled contour), 2 line, 3 cubic, 4 close
  stems    4 bytes each: i16 position, u8 axis (0 vertical, 1 horizontal), u8 length
  names    ASCII manifest identifiers
"""
import argparse
import math
from pathlib import Path
import re
import struct
import sys
import xml.etree.ElementTree as ElementTree
import zlib

GRID = 24
UNITS = 64
VERSION = 1
HEADER_BYTES = 40
RECORD_BYTES = 16
MAXIMUM_ICONS = 255
MAXIMUM_OUTLINE_COMMANDS = 512
MAXIMUM_STEMS = 8
COORDINATE_LIMIT = 4  # grid units outside the 24-unit box
OP_MOVE_STROKE, OP_MOVE_FILL, OP_LINE, OP_CUBIC, OP_CLOSE = range(5)
FLAG_CLOSED_CONTOUR = 1

# Brand and trademarked service artwork is never admitted, whatever the source.
DENIED = re.compile(
    r'discord|clyde|nitro|wumpus|(^|[-_/])brand([-_]|$)|github|gitlab|twitter|facebook|instagram|'
    r'linkedin|youtube|twitch|slack|figma|framer|chromium|chrome|codepen|codesandbox|dribbble|'
    r'trello|pocket|steam|spotify|reddit|whatsapp|telegram|mastodon|bluesky')
IDENTIFIER = re.compile(r'[a-z][a-z0-9_]{0,31}')
SOURCE = re.compile(r'(lucide|original)/([a-z0-9]+(?:-[a-z0-9]+)*)')
MANIFEST_LINE = re.compile(r'MAICORD_ICON\(\s*([A-Za-z0-9_]+)\s*,\s*"([^"]+)"\s*\)')
SVG_NAMESPACE = '{http://www.w3.org/2000/svg}'
ROOT_ATTRIBUTES = {
    'xmlns', 'width', 'height', 'viewBox', 'fill', 'stroke', 'stroke-width', 'stroke-linecap',
    'stroke-linejoin', 'class'}
ELEMENT_ATTRIBUTES = {
    'path': {'d'},
    'circle': {'cx', 'cy', 'r'},
    'ellipse': {'cx', 'cy', 'rx', 'ry'},
    'rect': {'x', 'y', 'width', 'height', 'rx', 'ry'},
    'line': {'x1', 'y1', 'x2', 'y2'},
    'polyline': {'points'},
    'polygon': {'points'},
}
KAPPA = 0.5522847498307936


class IconError(ValueError):
    pass


def read_manifest(path):
    entries, names, sources = [], set(), set()
    for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        text = line.strip()
        if not text or text.startswith('//'):
            continue
        match = MANIFEST_LINE.fullmatch(text)
        if not match:
            raise IconError(f'{path}:{number}: expected MAICORD_ICON(name, "source")')
        name, source = match[1], match[2]
        if not IDENTIFIER.fullmatch(name) or name == 'count':
            raise IconError(f'{path}:{number}: invalid icon identifier {name!r}')
        if not SOURCE.fullmatch(source):
            raise IconError(f'{path}:{number}: invalid icon source {source!r}')
        if DENIED.search(name) or DENIED.search(source):
            raise IconError(f'{path}:{number}: brand icons are not permitted ({name}, {source})')
        if name in names or source in sources:
            raise IconError(f'{path}:{number}: duplicate icon {name} or source {source}')
        names.add(name)
        sources.add(source)
        entries.append((name, source))
    if not entries or len(entries) > MAXIMUM_ICONS:
        raise IconError(f'manifest must list 1..{MAXIMUM_ICONS} icons')
    return entries


def lucide_elements(path):
    """Read the element array from a lucide dist/esm/icons/<name>.js module."""
    text = path.read_text(encoding='utf-8')
    elements = []
    for tag, body in re.findall(r'\[\s*"([a-z]+)"\s*,\s*\{(.*?)\}\s*\]', text, re.S):
        attributes = dict(re.findall(r'([a-zA-Z][a-zA-Z0-9-]*)\s*:\s*"([^"]*)"', body))
        elements.append((tag, attributes))
    if not elements:
        raise IconError(f'{path}: no SVG elements')
    return elements


def svg_text(elements):
    lines = ['<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" '
             'fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" '
             'stroke-linejoin="round">']
    for tag, attributes in elements:
        if tag not in ELEMENT_ATTRIBUTES:
            raise IconError(f'unsupported element {tag}')
        order = sorted(attributes, key=lambda key: (key == 'fill', key))
        rendered = ' '.join(f'{key}="{attributes[key]}"' for key in order)
        lines.append(f'  <{tag} {rendered} />')
    lines.append('</svg>')
    return '\n'.join(lines) + '\n'


def vendor(arguments):
    entries = read_manifest(arguments.manifest)
    icons = arguments.lucide_package / 'dist' / 'esm' / 'icons'
    arguments.output.mkdir(parents=True, exist_ok=True)
    wanted = set()
    for _, source in entries:
        kind, name = source.split('/')
        if kind != 'lucide':
            continue
        wanted.add(f'{name}.svg')
        module = icons / f'{name}.js'
        if not module.is_file():
            raise IconError(f'missing lucide icon {name}')
        target = arguments.output / f'{name}.svg'
        target.write_text(svg_text(lucide_elements(module)), encoding='utf-8', newline='\n')
    for stale in sorted(arguments.output.glob('*.svg')):
        if stale.name not in wanted:
            stale.unlink()


class PathReader:
    NUMBER = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')

    def __init__(self, text):
        self.text, self.at = text, 0

    def skip(self):
        while self.at < len(self.text) and self.text[self.at] in ' \t\r\n,':
            self.at += 1

    def done(self):
        self.skip()
        return self.at >= len(self.text)

    def command(self):
        self.skip()
        if self.at < len(self.text) and self.text[self.at].isalpha():
            self.at += 1
            return self.text[self.at - 1]
        return None

    def number(self):
        self.skip()
        match = self.NUMBER.match(self.text, self.at)
        if not match:
            raise IconError(f'expected a number at {self.text[self.at:self.at + 12]!r}')
        self.at = match.end()
        value = float(match[0])
        if not math.isfinite(value):
            raise IconError('non-finite path number')
        return value

    def flag(self):
        self.skip()
        if self.at < len(self.text) and self.text[self.at] in '01':
            self.at += 1
            return self.text[self.at - 1] == '1'
        raise IconError('expected an arc flag')

    def at_number(self):
        self.skip()
        return self.at < len(self.text) and (self.text[self.at] in '+-.' or self.text[self.at].isdigit())


class Contour:
    def __init__(self, start, filled):
        self.start, self.filled, self.segments, self.closed = start, filled, [], False


def arc_cubics(start, rx, ry, rotation, large, sweep, end):
    """SVG endpoint arc as cubic Beziers of at most 90 degrees (SVG 1.1 F.6.5)."""
    x1, y1 = start
    x2, y2 = end
    if (x1, y1) == (x2, y2):
        return []
    rx, ry = abs(rx), abs(ry)
    if rx == 0 or ry == 0:
        return [('line', (end,))]
    phi = math.radians(rotation % 360)
    cos_phi, sin_phi = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    xp = cos_phi * dx + sin_phi * dy
    yp = -sin_phi * dx + cos_phi * dy
    scale = (xp * xp) / (rx * rx) + (yp * yp) / (ry * ry)
    if scale > 1:
        rx, ry = rx * math.sqrt(scale), ry * math.sqrt(scale)
    numerator = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp
    denominator = rx * rx * yp * yp + ry * ry * xp * xp
    factor = math.sqrt(max(0.0, numerator / denominator)) if denominator else 0.0
    if large == sweep:
        factor = -factor
    cxp, cyp = factor * rx * yp / ry, -factor * ry * xp / rx
    cx = cos_phi * cxp - sin_phi * cyp + (x1 + x2) / 2
    cy = sin_phi * cxp + cos_phi * cyp + (y1 + y2) / 2

    def angle(ux, uy, vx, vy):
        return math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)

    theta = angle(1, 0, (xp - cxp) / rx, (yp - cyp) / ry)
    delta = angle((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry)
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    elif sweep and delta < 0:
        delta += 2 * math.pi
    count = max(1, math.ceil(abs(delta) / (math.pi / 2) - 1e-9))
    step = delta / count
    alpha = 4 / 3 * math.tan(step / 4)

    def point(t):
        x, y = rx * math.cos(t), ry * math.sin(t)
        return (cos_phi * x - sin_phi * y + cx, sin_phi * x + cos_phi * y + cy)

    def derivative(t):
        x, y = -rx * math.sin(t), ry * math.cos(t)
        return (cos_phi * x - sin_phi * y, sin_phi * x + cos_phi * y)

    segments = []
    for index in range(count):
        a, b = theta + step * index, theta + step * (index + 1)
        pa, pb, da, db = point(a), point(b), derivative(a), derivative(b)
        last = end if index == count - 1 else pb
        segments.append(('cubic', ((pa[0] + alpha * da[0], pa[1] + alpha * da[1]),
                                   (pb[0] - alpha * db[0], pb[1] - alpha * db[1]), last)))
    return segments


def parse_path(data, filled):
    reader, contours = PathReader(data), []
    current = start = (0.0, 0.0)
    previous_control, previous_kind, command, contour = None, None, None, None

    def begin(point):
        nonlocal contour
        contour = Contour(point, filled)
        contours.append(contour)

    def append(kind, points):
        nonlocal contour
        if contour is None or contour.closed:
            begin(current)
        contour.segments.append((kind, points))

    while not reader.done():
        explicit = reader.command()
        if explicit:
            command = explicit
        elif command is None or command in 'Zz':
            raise IconError('path data must start with a command')
        relative = command.islower()
        base = current if relative else (0.0, 0.0)
        upper = command.upper()
        if upper == 'M':
            point = (base[0] + reader.number(), base[1] + reader.number())
            current = start = point
            begin(point)
            command = 'l' if relative else 'L'
            previous_kind = 'M'
            continue
        if upper == 'Z':
            if contour is not None and not contour.closed:
                contour.closed = True
            current = start
            previous_kind = 'Z'
            command = None
            continue
        if upper in 'LHV':
            if upper == 'L':
                point = (base[0] + reader.number(), base[1] + reader.number())
            elif upper == 'H':
                point = ((current[0] if relative else 0.0) + reader.number(), current[1])
            else:
                point = (current[0], (current[1] if relative else 0.0) + reader.number())
            append('line', (point,))
            current, previous_kind = point, 'L'
        elif upper in 'CS':
            if upper == 'C':
                c1 = (base[0] + reader.number(), base[1] + reader.number())
            else:
                c1 = (2 * current[0] - previous_control[0], 2 * current[1] - previous_control[1]) \
                    if previous_kind == 'C' else current
            c2 = (base[0] + reader.number(), base[1] + reader.number())
            point = (base[0] + reader.number(), base[1] + reader.number())
            append('cubic', (c1, c2, point))
            current, previous_control, previous_kind = point, c2, 'C'
            continue
        elif upper in 'QT':
            if upper == 'Q':
                control = (base[0] + reader.number(), base[1] + reader.number())
            else:
                control = (2 * current[0] - previous_control[0], 2 * current[1] - previous_control[1]) \
                    if previous_kind == 'Q' else current
            point = (base[0] + reader.number(), base[1] + reader.number())
            c1 = (current[0] + 2 / 3 * (control[0] - current[0]), current[1] + 2 / 3 * (control[1] - current[1]))
            c2 = (point[0] + 2 / 3 * (control[0] - point[0]), point[1] + 2 / 3 * (control[1] - point[1]))
            append('cubic', (c1, c2, point))
            current, previous_control, previous_kind = point, control, 'Q'
            continue
        elif upper == 'A':
            rx, ry, rotation = reader.number(), reader.number(), reader.number()
            large, sweep = reader.flag(), reader.flag()
            point = (base[0] + reader.number(), base[1] + reader.number())
            for segment in arc_cubics(current, rx, ry, rotation, large, sweep, point):
                append(*segment)
            current, previous_kind = point, 'A'
        else:
            raise IconError(f'unsupported path command {command}')
    return contours


def ellipse_contour(cx, cy, rx, ry, filled):
    if rx <= 0 or ry <= 0:
        raise IconError('ellipse radii must be positive')
    kx, ky = rx * KAPPA, ry * KAPPA
    contour = Contour((cx + rx, cy), filled)
    contour.segments = [
        ('cubic', ((cx + rx, cy + ky), (cx + kx, cy + ry), (cx, cy + ry))),
        ('cubic', ((cx - kx, cy + ry), (cx - rx, cy + ky), (cx - rx, cy))),
        ('cubic', ((cx - rx, cy - ky), (cx - kx, cy - ry), (cx, cy - ry))),
        ('cubic', ((cx + kx, cy - ry), (cx + rx, cy - ky), (cx + rx, cy))),
    ]
    contour.closed = True
    return contour


def rect_contour(x, y, width, height, rx, ry, filled):
    if width <= 0 or height <= 0:
        raise IconError('rect size must be positive')
    rx = min(rx, width / 2)
    ry = min(ry, height / 2)
    contour = Contour((x + rx, y), filled)
    if rx <= 0 or ry <= 0:
        contour.segments = [('line', ((x + width, y),)), ('line', ((x + width, y + height),)),
                            ('line', ((x, y + height),)), ('line', ((x, y),))]
    else:
        kx, ky = rx * (1 - KAPPA), ry * (1 - KAPPA)
        r, b = x + width, y + height
        contour.segments = [
            ('line', ((r - rx, y),)),
            ('cubic', ((r - kx, y), (r, y + ky), (r, y + ry))),
            ('line', ((r, b - ry),)),
            ('cubic', ((r, b - ky), (r - kx, b), (r - rx, b))),
            ('line', ((x + rx, b),)),
            ('cubic', ((x + kx, b), (x, b - ky), (x, b - ry))),
            ('line', ((x, y + ry),)),
            ('cubic', ((x, y + ky), (x + kx, y), (x + rx, y))),
        ]
    contour.closed = True
    return contour


def number_attribute(attributes, name, default=None):
    if name not in attributes:
        if default is None:
            raise IconError(f'missing attribute {name}')
        return default
    try:
        value = float(attributes[name])
    except ValueError:
        raise IconError(f'invalid number in {name}') from None
    if not math.isfinite(value):
        raise IconError(f'non-finite {name}')
    return value


def points_attribute(text):
    values = [float(value) for value in re.findall(PathReader.NUMBER.pattern, text)]
    if len(values) < 4 or len(values) % 2:
        raise IconError('points needs at least two coordinate pairs')
    return list(zip(values[0::2], values[1::2]))


def read_svg(path):
    try:
        root = ElementTree.parse(path).getroot()
    except ElementTree.ParseError as error:
        raise IconError(f'{path}: {error}') from None
    if root.tag != SVG_NAMESPACE + 'svg':
        raise IconError(f'{path}: root must be an SVG element')
    for key, value in root.attrib.items():
        if key not in ROOT_ATTRIBUTES:
            raise IconError(f'{path}: unsupported root attribute {key}')
    if root.get('viewBox', '0 0 24 24').split() != ['0', '0', '24', '24']:
        raise IconError(f'{path}: viewBox must be 0 0 24 24')
    if root.get('fill', 'none') != 'none' or root.get('stroke', 'currentColor') != 'currentColor':
        raise IconError(f'{path}: icons are stroked with currentColor')
    contours = []
    for element in root:
        if not element.tag.startswith(SVG_NAMESPACE):
            raise IconError(f'{path}: foreign element {element.tag}')
        tag = element.tag[len(SVG_NAMESPACE):]
        if tag not in ELEMENT_ATTRIBUTES or len(element):
            raise IconError(f'{path}: unsupported element {tag}')
        attributes = dict(element.attrib)
        fill = attributes.pop('fill', 'none')
        if fill not in ('none', 'currentColor'):
            raise IconError(f'{path}: fill must be none or currentColor')
        for key in attributes:
            if key not in ELEMENT_ATTRIBUTES[tag]:
                raise IconError(f'{path}: unsupported attribute {key} on {tag}')
        filled = fill == 'currentColor'
        if tag == 'path':
            contours.extend(parse_path(attributes['d'], filled))
        elif tag == 'circle':
            r = number_attribute(attributes, 'r')
            contours.append(ellipse_contour(number_attribute(attributes, 'cx', 0.0),
                                            number_attribute(attributes, 'cy', 0.0), r, r, filled))
        elif tag == 'ellipse':
            contours.append(ellipse_contour(number_attribute(attributes, 'cx', 0.0),
                                            number_attribute(attributes, 'cy', 0.0),
                                            number_attribute(attributes, 'rx'),
                                            number_attribute(attributes, 'ry'), filled))
        elif tag == 'rect':
            rx = number_attribute(attributes, 'rx', -1.0)
            ry = number_attribute(attributes, 'ry', -1.0)
            rx, ry = (ry if rx < 0 else rx), (rx if ry < 0 else ry)
            contours.append(rect_contour(number_attribute(attributes, 'x', 0.0),
                                         number_attribute(attributes, 'y', 0.0),
                                         number_attribute(attributes, 'width'),
                                         number_attribute(attributes, 'height'),
                                         max(rx, 0.0), max(ry, 0.0), filled))
        elif tag == 'line':
            contour = Contour((number_attribute(attributes, 'x1'), number_attribute(attributes, 'y1')), filled)
            contour.segments = [('line', ((number_attribute(attributes, 'x2'), number_attribute(attributes, 'y2')),))]
            contours.append(contour)
        else:
            points = points_attribute(attributes['points'])
            contour = Contour(points[0], filled)
            contour.segments = [('line', (point,)) for point in points[1:]]
            contour.closed = tag == 'polygon'
            contours.append(contour)
    contours = [contour for contour in contours if contour.segments]
    if not contours:
        raise IconError(f'{path}: no drawable contours')
    return contours


def quantize(value):
    scaled = math.floor(value * UNITS + 0.5)
    if not -COORDINATE_LIMIT * UNITS <= scaled <= (GRID + COORDINATE_LIMIT) * UNITS:
        raise IconError(f'coordinate {value} outside the icon box')
    return scaled


def compile_icon(contours):
    commands, outline_count, flags = bytearray(), 0, 0
    stems = {}

    def coordinates(*points):
        for x, y in points:
            commands.extend(struct.pack('<hh', quantize(x), quantize(y)))

    for contour in contours:
        points = [contour.start] + [point for _, segment in contour.segments for point in segment]
        if all(point == contour.start for point in points):
            raise IconError('degenerate contour (missing geometry attributes?)')
        commands.append(OP_MOVE_FILL if contour.filled else OP_MOVE_STROKE)
        coordinates(contour.start)
        current = contour.start
        for kind, points in contour.segments:
            if kind == 'line':
                commands.append(OP_LINE)
                coordinates(points[0])
                end = points[0]
                for axis, a, b in ((0, current[0], end[0]), (1, current[1], end[1])):
                    length = abs(end[1 - axis] - current[1 - axis])
                    if abs(a - b) < 1e-6 and length >= 1.5:
                        key = (axis, quantize(a))
                        stems[key] = stems.get(key, 0.0) + length
            else:
                commands.append(OP_CUBIC)
                coordinates(*points)
                end = points[2]
            current = end
        if contour.closed:
            commands.append(OP_CLOSE)
            flags |= FLAG_CLOSED_CONTOUR
        outline_count += len(contour.segments) + 2
    if outline_count > MAXIMUM_OUTLINE_COMMANDS or len(commands) > 0xFFFF:
        raise IconError('icon outline exceeds the decoder bound')
    ranked = sorted(stems.items(), key=lambda item: (-round(item[1], 6), item[0]))[:MAXIMUM_STEMS]
    stem_bytes = b''.join(struct.pack('<hBB', position, axis, min(255, int(length + 0.5)))
                          for (axis, position), length in ranked)
    return bytes(commands), outline_count, flags, stem_bytes, len(ranked)


def build_pack(entries, lucide_directory, original_directory):
    records, command_blob, stem_blob, name_blob = [], bytearray(), bytearray(), bytearray()
    stem_count_total = 0
    for name, source in entries:
        kind, file = source.split('/')
        path = (lucide_directory if kind == 'lucide' else original_directory) / f'{file}.svg'
        if not path.is_file():
            raise IconError(f'missing icon source {path}')
        try:
            commands, outline_count, flags, stems, stem_count = compile_icon(read_svg(path))
        except IconError as error:
            raise IconError(f'{path}: {error}') from None
        records.append(struct.pack('<IHHHBBHBB', len(command_blob), len(commands), outline_count,
                                   stem_count_total, stem_count, flags, len(name_blob), len(name), 0))
        command_blob += commands
        stem_blob += stems
        stem_count_total += stem_count
        name_blob += name.encode('ascii')
    if stem_count_total > 0xFFFF or len(name_blob) > 0xFFFF:
        raise IconError('pack tables exceed 16-bit offsets')
    order = sorted(range(len(entries)), key=lambda index: entries[index][0].encode('ascii'))
    index_blob = b''.join(struct.pack('<H', index) for index in order)
    records_offset = HEADER_BYTES
    index_offset = records_offset + RECORD_BYTES * len(records)
    commands_offset = index_offset + len(index_blob)
    stems_offset = commands_offset + len(command_blob)
    names_offset = stems_offset + len(stem_blob)
    total = names_offset + len(name_blob)
    header = struct.pack('<4sHHHHIIIIIII', b'MIPK', VERSION, len(entries), GRID, UNITS, total,
                         records_offset, index_offset, commands_offset, stems_offset, names_offset, 0)
    pack = header + b''.join(records) + index_blob + bytes(command_blob) + bytes(stem_blob) + bytes(name_blob)
    assert len(pack) == total
    return pack


def write_include(path, pack, count):
    lines = ['// Generated by tools/generate_icon_pack.py from include/maicord/ui/icon_ids.inc.',
             f'// MIPK v{VERSION}: {count} icons, {len(pack)} bytes.',
             f'alignas(4) constexpr u8 kUiIconPackData[{len(pack)}] = {{']
    for start in range(0, len(pack), 24):
        lines.append(''.join(f'{value},' for value in pack[start:start + 24]))
    lines.append('};')
    # The client ships only this zlib copy and inflates it on first use; the raw
    # array above serves compile-time header checks.
    packed = zlib.compress(bytes(pack), 9)
    assert zlib.decompress(packed) == bytes(pack)
    lines.append(f'constexpr u8 kUiIconPackPacked[{len(packed)}] = {{')
    for start in range(0, len(packed), 24):
        lines.append(''.join(f'{value},' for value in packed[start:start + 24]))
    lines.append('};')
    text = '\n'.join(lines) + '\n'
    if not path.is_file() or path.read_text(encoding='ascii') != text:
        path.write_text(text, encoding='ascii', newline='\n')


def build(arguments):
    entries = read_manifest(arguments.manifest)
    pack = build_pack(entries, arguments.lucide, arguments.original)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    write_include(arguments.output, pack, len(entries))
    if arguments.binary:
        arguments.binary.write_bytes(pack)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest='command', required=True)
    vendor_parser = commands.add_parser('vendor')
    vendor_parser.add_argument('--manifest', type=Path, required=True)
    vendor_parser.add_argument('--lucide-package', type=Path, required=True)
    vendor_parser.add_argument('--output', type=Path, required=True)
    build_parser = commands.add_parser('build')
    build_parser.add_argument('--manifest', type=Path, required=True)
    build_parser.add_argument('--lucide', type=Path, required=True)
    build_parser.add_argument('--original', type=Path, required=True)
    build_parser.add_argument('--output', type=Path, required=True)
    build_parser.add_argument('--binary', type=Path)
    arguments = parser.parse_args()
    try:
        (vendor if arguments.command == 'vendor' else build)(arguments)
    except IconError as error:
        print(f'generate_icon_pack: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
