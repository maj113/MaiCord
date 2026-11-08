#!/usr/bin/env python3
"""Compile the pinned Twemoji regional-flag SVGs into binary records in deflate groups.

Input: assets/regional_flags/<codepoints>.svg (CC BY 4.0, see
assets/licenses/Twemoji-graphics.txt). The flag sequence is derived from the
file name exactly as src/ui/text/regional_flag.cpp numbers it. Source numbers are
rounded to two decimals (1/3600 of the 36-unit view box); path positions are then
rounded to a tenth of a unit (PATH_UNIT), each once, so no error accumulates and
none exceeds 0.05 unit.

Each flag becomes the record src/media/image/regional_flag_art.cpp reads, with no
SVG left to parse at run time: its filled layers in paint order, groups resolved
into each layer's own fill and fill rule. A layer is a header byte (bits 0-1 the
kind: 0 path, 1 circle, 2 ellipse; bit 2 the even-odd fill rule; bit 3 the one
admitted ellipse rotation), the fill's three sRGB bytes, then for a path its SVG
path commands (the letter, the varint count of its numbers, the zigzag varint
numbers in tenths; a zero letter ends the path) and for a circle or ellipse its
centre and radii (zigzag varint hundredths). 0xFF ends the record. Output is a
deterministic C++ include with one zlib stream per group of similar records.
"""
import argparse
from collections import Counter
import pathlib
import re
import zlib

import optimal_deflate
from font_reference.embed_fixture import embed_arrays, write_if_changed

# A request inflates its flag's whole group: records up to this many bytes, or one larger record alone.
GROUP_RAW_BUDGET = 16 * 1024
PACKED_FILE = "regional_flag_packed.bin"  # beside the include, read by #embed
SUBDIVISIONS = {"gbeng": 677, "gbsct": 678, "gbwls": 679}


def sequence_of(stem):
    parts = [int(p, 16) for p in stem.split("-")]
    if len(parts) == 2 and all(0x1F1E6 <= p <= 0x1F1FF for p in parts):
        return (parts[0] - 0x1F1E6) * 26 + (parts[1] - 0x1F1E6) + 1
    if parts[0] == 0x1F3F4 and parts[-1] == 0xE007F:
        tag = "".join(chr(p - 0xE0000) for p in parts[1:-1])
        if tag in SUBDIVISIONS:
            return SUBDIVISIONS[tag]
    raise SystemExit(f"unrecognised flag file name: {stem}")


NUMBER = re.compile(rb"-?(?:\d+\.?\d*|\.\d+)")
# Geometry attributes only; names, colours and namespaces stay as written.
ATTRIBUTE = re.compile(rb' (d|viewBox|cx|cy|r|rx|ry)="([^"]*)"')


def rounded(token):
    """Two-decimal form of a decimal token; integers and runs such as the
    concatenated arc flags in "011.5" are kept as written."""
    body = token.lstrip(b"-")
    if b"." not in body or (len(body) > 1 and body[0:1] == b"0" and body[1:2] != b"."):
        return token
    text = f"{round(float(token), 2):.2f}".rstrip("0").rstrip(".")
    if text in ("-0", ""):
        text = "0"
    text = text.replace("0.", ".", 1) if text.startswith(("0.", "-0.")) else text
    return text.encode()


def round_numbers(value):
    out, previous, end = bytearray(), b"", 0
    for match in NUMBER.finditer(value):
        token = rounded(match.group(0))
        adjacent = match.start() == end and previous
        out += value[end:match.start()]
        # Keep adjacent numbers apart once rounding drops a decimal point.
        if adjacent and token[:1] != b"-" and (token[:1] != b"." or b"." not in previous):
            out += b" "
        out += token
        previous, end = token, match.end()
    out += value[end:]
    return bytes(out)


def compact(svg):
    rounded_svg = ATTRIBUTE.sub(lambda m: b" " + m.group(1) + b'="' + round_numbers(m.group(2)) + b'"', svg)
    numbers = lambda text: [m.group(0) for m in NUMBER.finditer(text)]
    shape = lambda text: NUMBER.sub(b"#", text).replace(b" ", b"")
    before, after = numbers(svg), numbers(rounded_svg)
    assert shape(svg) == shape(rounded_svg) and len(before) == len(after) and all(
        a == b or abs(float(a) - float(b)) <= 0.005 + 1e-9 for a, b in zip(before, after)), "rounding moved a number"
    return rounded_svg


PATH_NUMBER = re.compile(rb"[-+]?(?:\d+\.?\d*|\.\d+)")
KINDS = (b"path", b"circle", b"ellipse")
SHAPE_ATTRIBUTES = {b"circle": (b"cx", b"cy", b"r"), b"ellipse": (b"cx", b"cy", b"rx", b"ry")}
# The one transform the sources use; regional_flag_art.cpp applies it to the ellipse it marks.
ROTATION = b"rotate(-76.714 13.001 18.001)"
NAMED_FILLS = {b"red": 0xFF0000, b"green": 0x008000, b"navy": 0x000080}
END = 0xFF


def path_items(d):
    """The path's command letters and numbers; arc flags are single digits."""
    items, command, index, at = [], b"", 0, 0
    while at < len(d):
        c = d[at:at + 1]
        if c.isalpha():
            items.append(c); command, index, at = c, 0, at + 1
        elif c in b" ,\t\r\n":
            at += 1
        elif command in b"Aa" and index % 7 in (3, 4):
            assert c in b"01", d[at:at + 12]
            items.append(int(c) * 100); at += 1; index += 1
        else:
            match = PATH_NUMBER.match(d, at)
            assert match, d[at:at + 12]
            items.append(hundredths(match.group(0))); at = match.end(); index += 1
    return items


def hundredths(token):
    value = round(float(token) * 100)
    assert abs(value - float(token) * 100) < 1e-6, token
    return value


def varint(value, out, signed=True):
    if signed:
        value = (value << 1) ^ (value >> 63)
    while value >= 0x80:
        out.append(value & 0x7F | 0x80); value >>= 7
    out.append(value)


# Path numbers are stored in tenths of a view-box unit (PATH_UNIT hundredths). Every absolute position is
# rounded once, and a relative number is the difference of two rounded positions, so the error never exceeds
# PATH_UNIT / 2 hundredths (0.05 unit, under a tenth of a pixel at 64 px) and never accumulates along a path.
PATH_UNIT = 10
ARGUMENTS = {b"M": 2, b"L": 2, b"T": 2, b"C": 6, b"S": 4, b"Q": 4, b"A": 7, b"H": 1, b"V": 1, b"Z": 0}


def commands(items):
    """[(letter, numbers)] of path_items."""
    rows, at = [], 0
    while at < len(items):
        letter, at = items[at], at + 1
        end = at
        while end < len(items) and not isinstance(items[end], bytes):
            end += 1
        rows.append((letter, items[at:end]))
        at = end
    return rows


def divide_round(value, unit):
    """value / unit rounded half away from zero."""
    return (abs(value) * 2 + unit) // (2 * unit) * (-1 if value < 0 else 1)


def convert(items, unit):
    """Re-express a path in `unit` hundredths: ([(letter, numbers)], source points, rounded points).

    The points are every absolute position the path visits, in hundredths, before and after rounding; the
    numbers in unit 1 are the source numbers. H, V and arc and curve control points count as positions."""
    rows, exact, rounded = [], [], []
    x = y = start_x = start_y = 0  # source position and sub-path start
    qx = qy = start_qx = start_qy = 0  # rounded position and sub-path start, in units
    for letter, numbers in commands(items):
        kind, relative = letter.upper(), letter.islower()
        if kind == b"Z":
            x, y, qx, qy = start_x, start_y, start_qx, start_qy
            rows.append((letter, []))
            continue
        step = ARGUMENTS[kind]
        assert numbers and len(numbers) % step == 0, (letter, len(numbers))
        converted = []
        for first in range(0, len(numbers), step):
            segment, bx, by, bqx, bqy = numbers[first:first + step], x, y, qx, qy
            offset_x, offset_y = (bx, by) if relative else (0, 0)
            offset_qx, offset_qy = (bqx, bqy) if relative else (0, 0)
            if kind == b"H":
                targets = [(0, None)]
            elif kind == b"V":
                targets = [(None, 0)]
            elif kind == b"A":
                targets = [(5, 6)]
            else:
                targets = [(k, k + 1) for k in range(0, step, 2)]
            values = [divide_round(v, unit) for v in segment]
            if kind == b"A":
                values[3], values[4] = segment[3] // unit, segment[4] // unit  # arc flags stay 0 and 1
            for index_x, index_y in targets:
                if index_x is not None:
                    x = segment[index_x] + offset_x
                    qx = divide_round(x, unit)
                    values[index_x] = qx - offset_qx
                if index_y is not None:
                    y = segment[index_y] + offset_y
                    qy = divide_round(y, unit)
                    values[index_y] = qy - offset_qy
                exact.append((x, y))
                rounded.append((qx * unit, qy * unit))
            converted += values
            if kind == b"M" and first == 0:
                start_x, start_y, start_qx, start_qy = x, y, qx, qy
        rows.append((letter, converted))
    return rows, exact, rounded


def elements(svg):
    """(closing, tag, attributes) for every element after the root."""
    rows = []
    for match in re.finditer(rb"<(/?)([a-z]+)([^>]*?)/?>", svg):
        closing, tag, body = match.groups()
        if tag == b"svg":
            attributes = dict(re.findall(rb' ([a-z:A-Z-]+)="([^"]*)"', body))
            assert closing or set(attributes) <= {b"xmlns", b"viewBox", b"xml:space"}, attributes
            continue
        rows.append((bool(closing), tag, dict(re.findall(rb' ([a-z:-]+)="([^"]*)"', body))))
    return rows


def fill_colour(value):
    """The sRGB value of a fill: one of the named colours, #rgb or #rrggbb."""
    if value in NAMED_FILLS:
        return NAMED_FILLS[value]
    assert re.fullmatch(rb"#(?:[0-9a-fA-F]{3}){1,2}", value), value
    digits = value[1:].decode()
    return int("".join(c * 2 for c in digits) if len(digits) == 3 else digits, 16)


def layers(svg):
    """(kind, colour, even-odd, rotated, attributes) of every shape in paint order. A shape takes its own fill
    and fill rule, else its innermost group's, else black and non-zero; clip-rule never paints."""
    rows, inherited = [], [(0, False)]
    for closing, tag, attributes in elements(svg):
        if closing:
            assert tag == b"g"; inherited.pop(); continue
        assert set(attributes) <= {b"fill", b"fill-rule", b"clip-rule", b"transform", b"d", *SHAPE_ATTRIBUTES.get(tag, ())}, attributes
        assert attributes.get(b"fill-rule", b"evenodd") == b"evenodd"
        colour, even_odd = inherited[-1]
        if b"fill" in attributes:
            colour = fill_colour(attributes[b"fill"])
        even_odd = even_odd or b"fill-rule" in attributes
        if tag == b"g":
            assert not {b"d", b"transform"} & set(attributes), attributes
            inherited.append((colour, even_odd)); continue
        rotated = b"transform" in attributes
        assert not rotated or (tag == b"ellipse" and attributes[b"transform"] == ROTATION), attributes
        rows.append((KINDS.index(tag), colour, even_odd, rotated, attributes))
    assert len(inherited) == 1 and 0 < len(rows) <= 64, len(rows)
    return rows


def encode(svg):
    out = bytearray()
    for kind, colour, even_odd, rotated, attributes in layers(svg):
        out.append(kind | even_odd << 2 | rotated << 3)
        out += colour.to_bytes(3, "big")
        if kind == 0:
            for letter, numbers in convert(path_items(attributes[b"d"]), PATH_UNIT)[0]:
                out += letter; varint(len(numbers), out, False)
                for value in numbers:
                    varint(value, out)
            out.append(0)
        else:
            for name in SHAPE_ATTRIBUTES[KINDS[kind]]:
                varint(hundredths(attributes[name]), out)
    out.append(END)
    return bytes(out)


def decode(record):
    """The layers the client reads from a record: (kind, colour, even-odd, rotated, geometry), the geometry
    a path's [(letter, hundredths)] or a shape's centre and radii in hundredths."""
    at, rows = 0, []

    def unsigned():
        nonlocal at
        value = shift = 0
        while True:
            byte = record[at]; at += 1
            value |= (byte & 0x7F) << shift; shift += 7
            if byte < 0x80:
                return value

    def signed():
        value = unsigned()
        return (value >> 1) ^ -(value & 1)

    while record[at] != END:
        header, colour = record[at], int.from_bytes(record[at + 1:at + 4], "big")
        at += 4
        kind = header & 3
        if kind == 0:
            geometry = []
            while record[at]:
                letter = record[at:at + 1]; at += 1
                geometry.append((letter, [signed() * PATH_UNIT for _ in range(unsigned())]))
            at += 1
        else:
            geometry = [signed() for _ in SHAPE_ATTRIBUTES[KINDS[kind]]]
        rows.append((kind, colour, bool(header & 4), bool(header & 8), geometry))
    assert at + 1 == len(record)
    return rows


def verify(svg, record):
    """The record keeps every layer, fill, fill rule, rotation and path command of the rounded source and its
    shapes exactly; its paths visit the same points to within half a PATH_UNIT, and its arc radii and
    rotations likewise."""
    source, rebuilt = layers(svg), decode(record)
    assert len(source) == len(rebuilt), "record changed a flag"
    for (kind, colour, even_odd, rotated, attributes), row in zip(source, rebuilt):
        assert (kind, colour, even_odd, rotated) == row[:4], "record changed a layer"
        if kind:
            assert row[4] == [hundredths(attributes[name]) for name in SHAPE_ATTRIBUTES[KINDS[kind]]], "record moved a shape"
            continue
        items, path = path_items(attributes[b"d"]), row[4]
        assert [(letter, len(numbers)) for letter, numbers in commands(items)] == \
            [(letter, len(numbers)) for letter, numbers in path], "record changed a path"
        moved = [item for letter, numbers in path for item in (letter, *numbers)]
        for (a, b) in zip(convert(items, 1)[1], convert(moved, 1)[1]):
            assert max(abs(a[0] - b[0]), abs(a[1] - b[1])) <= PATH_UNIT // 2, "record moved a point"
        for (letter, numbers), (_, rounded) in zip(commands(items), path):
            if letter in (b"A", b"a"):
                for first in range(0, len(numbers), 7):
                    assert all(abs(numbers[first + k] - rounded[first + k]) <= PATH_UNIT // 2 for k in range(3)), "arc moved"
                    assert numbers[first + 3:first + 5] == rounded[first + 3:first + 5], "arc flags changed"


def best_deflate(data):
    best = None
    for mem in (8, 9):
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED):
            c = zlib.compressobj(9, zlib.DEFLATED, 15, mem, strategy)
            out = c.compress(data) + c.flush()
            if best is None or len(out) < len(best):
                best = out
    return best


def adjacent_groups(flags):
    groups, current, raw = [], [], 0
    for sequence, record in flags:
        if current and raw + len(record) > GROUP_RAW_BUDGET:
            groups.append(current)
            current, raw = [], 0
        current.append((sequence, record))
        raw += len(record)
    groups.append(current)

    return groups


def group_flags(flags):
    # Keep each flag's inflated byte count at or below its previous adjacent group.
    baseline = adjacent_groups(flags)
    limits = {sequence: sum(len(record) for _, record in group)
              for group in baseline for sequence, _ in group}
    # Sample short byte strings to find shared artwork. Bytes most flags share
    # provide little compression benefit, so omit features shared by many flags.
    features = [{record[i:i + 6] for i in range(0, len(record) - 5, 3)} for _, record in flags]
    frequency = Counter(feature for row in features for feature in row)
    features = [{feature for feature in row if frequency[feature] < len(flags) // 3}
                for row in features]
    remaining, groups = set(range(len(flags))), []
    while remaining:
        seed = max(remaining, key=lambda i: (len(flags[i][1]), -i))
        remaining.remove(seed)
        group, raw, limit = [flags[seed]], len(flags[seed][1]), limits[flags[seed][0]]
        shared = features[seed].copy()
        while True:
            choices = [i for i in remaining
                       if raw + len(flags[i][1]) <= min(limit, limits[flags[i][0]])]
            if not choices:
                break
            chosen = max(choices, key=lambda i: (
                len(shared & features[i]) / max(1, len(features[i])), len(flags[i][1]), -i))
            remaining.remove(chosen)
            group.append(flags[chosen])
            raw += len(flags[chosen][1])
            limit = min(limit, limits[flags[chosen][0]])
            shared |= features[chosen]
        groups.append(group)
    return groups


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    flags = sorted((sequence_of(p.stem), compact(p.read_bytes())) for p in args.source.glob("*.svg"))
    if len({s for s, _ in flags}) != len(flags):
        raise SystemExit("duplicate flag sequence")
    for index, (sequence, svg) in enumerate(flags):
        record = encode(svg)
        verify(svg, record)
        flags[index] = (sequence, record)
    checksum = 0xCBF29CE484222325
    for _, record in flags:
        for byte in record:
            checksum = (checksum ^ byte) * 0x100000001B3 % (1 << 64)

    groups, entries = group_flags(flags), []
    cache = optimal_deflate.DeflateCache("regional_flags")
    assert len(groups) <= 256

    blob, group_rows, maximum_raw = bytearray(), [], 0
    for index, group in enumerate(groups):
        offsets, raw_data = {}, bytearray()
        for _, record in group:
            if record not in offsets:
                offsets[record] = len(raw_data)
                raw_data += record
        packed = cache.stream(bytes(raw_data), best_deflate)
        assert zlib.decompress(packed) == raw_data
        group_rows.append((len(blob), len(packed), len(raw_data)))
        blob += packed
        maximum_raw = max(maximum_raw, len(raw_data))
        for sequence, record in group:
            offset = offsets[record]
            assert offset < 65536 and len(record) < 65536
            entries.append((sequence, offset, len(record), index))
    cache.save()

    entries.sort()  # Runtime lookup remains a binary search by sequence.
    lines = ["// Generated by tools/generate_regional_flag_bundle.py; do not edit.",
             "// Twemoji artwork, CC BY 4.0, coordinates rounded. See assets/licenses/Twemoji-graphics.txt.",
             f"constexpr u64 kFlagRecordChecksum=0x{checksum:016X}ull;",
             f"constexpr u32 kFlagCount={len(entries)};",
             f"constexpr u32 kFlagPathUnit={PATH_UNIT};",
             f"constexpr u32 kFlagGroupCount={len(groups)};",
             f"constexpr u32 kFlagScratchBytes={maximum_raw};",
             "constexpr FlagGroup kFlagGroups[]={"]
    lines += [f"{{{o},{c},{r}}}," for o, c, r in group_rows]
    lines.append("};")
    lines.append("constexpr FlagEntry kFlagEntries[]={")
    lines += [f"{{{s},{o},{n},{g}}}," for s, o, n, g in entries]
    lines.append("};")
    lines.append(embed_arrays([("constexpr u8 kFlagPacked[]", PACKED_FILE)]).rstrip("\n"))
    text = "\n".join(lines) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_if_changed(args.output.with_name(PACKED_FILE), bytes(blob))
    if not args.output.exists() or args.output.read_text() != text:
        args.output.write_text(text, newline="\n")


if __name__ == "__main__":
    main()
