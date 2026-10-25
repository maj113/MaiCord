#!/usr/bin/env python3
"""Pack the pinned Twemoji regional-flag SVGs into deflate groups.

Input: assets/regional_flags/<codepoints>.svg (CC BY 4.0, see
assets/licenses/Twemoji-graphics.txt). The flag sequence is derived from the
file name exactly as src/ui/regional_flag.cpp numbers it. Coordinates are
rounded to two decimals (1/3600 of the 36-unit view box). Each flag becomes a
binary record (elements, fills, path letters with number counts, zigzag
varint hundredths) that src/app/regional_flag_assets.cpp turns back into the
SVG text `decode` writes here. Output is a deterministic C++ include with one
zlib stream per group of similar records.
"""
import argparse
from collections import Counter
import pathlib
import re
import zlib

GROUP_RAW_BUDGET = 20 * 1024
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
TAGS = (b"path", b"circle", b"ellipse", b"g")
SHAPE_ATTRIBUTES = {b"circle": (b"cx", b"cy", b"r"), b"ellipse": (b"cx", b"cy", b"rx", b"ry")}
ROTATION = b"rotate(-76.714 13.001 18.001)"
HEADER = b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 36 36">'


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


def encode(svg):
    out = bytearray()
    for closing, tag, attributes in elements(svg):
        if closing:
            assert tag == b"g"; out.append(4); continue
        assert set(attributes) <= {b"fill", b"fill-rule", b"clip-rule", b"transform", b"d", *SHAPE_ATTRIBUTES.get(tag, ())}, attributes
        assert attributes.get(b"fill-rule", b"evenodd") == b"evenodd" and attributes.get(b"transform", ROTATION) == ROTATION
        fill = attributes.get(b"fill", b"")
        out += bytes((TAGS.index(tag), len(fill))) + fill
        out.append((b"fill-rule" in attributes) | (b"transform" in attributes) << 1)
        if tag == b"path":
            items, at = path_items(attributes[b"d"]), 0
            while at < len(items):
                letter, at = items[at], at + 1
                end = at
                while end < len(items) and not isinstance(items[end], bytes):
                    end += 1
                out += letter; varint(end - at, out, False)
                for value in items[at:end]:
                    varint(value, out)
                at = end
            out.append(0)
        else:
            for name in SHAPE_ATTRIBUTES.get(tag, ()):
                varint(hundredths(attributes[name]), out)
    out.append(5)
    return bytes(out)


def decode(record):
    """The SVG text the client rebuilds from a record."""
    at, out = 0, bytearray(HEADER)

    def unsigned():
        nonlocal at
        value = shift = 0
        while True:
            byte = record[at]; at += 1
            value |= (byte & 0x7F) << shift; shift += 7
            if byte < 0x80:
                return value

    def number():
        value = unsigned()
        value = (value >> 1) ^ -(value & 1)
        text = f"{abs(value) // 100}" if abs(value) >= 100 or not abs(value) % 100 else ""
        if abs(value) % 100:
            text += f".{abs(value) % 100:02d}".rstrip("0")
        return (b"-" if value < 0 else b"") + text.encode()

    def separated(numbers):
        """Numbers joined as the parser reads them: a sign or a second point ends the previous one."""
        out = previous = b""
        for text in numbers:
            if previous and text[:1] != b"-" and not (text[:1] == b"." and b"." in previous):
                out += b" "
            out += text
            previous = text
        return out

    while True:
        tag = record[at]; at += 1
        if tag == 5:
            return bytes(out + b"</svg>")
        if tag == 4:
            out += b"</g>"; continue
        name, fill_bytes = TAGS[tag], record[at]
        fill, flags = record[at + 1:at + 1 + fill_bytes], record[at + 1 + fill_bytes]
        at += 2 + fill_bytes
        out += b"<" + name
        if fill:
            out += b' fill="' + fill + b'"'
        if flags & 1:
            out += b' fill-rule="evenodd"'
        if flags & 2:
            out += b' transform="' + ROTATION + b'"'
        if name == b"path":
            out += b' d="'
            while record[at]:
                out.append(record[at]); at += 1
                out += separated([number() for _ in range(unsigned())])
            at += 1
            out += b'"/>'
        elif name == b"g":
            out += b">"
        else:
            for attribute in SHAPE_ATTRIBUTES[name]:
                out += b" " + attribute + b'="' + number() + b'"'
            out += b"/>"


def verify(svg, text):
    """The rebuilt text keeps every element, fill, fill rule and number of the rounded source."""
    def meaning(source):
        rows = []
        for closing, tag, attributes in elements(source):
            attributes = {k: v for k, v in attributes.items() if k != b"clip-rule"}
            if b"d" in attributes:
                attributes[b"d"] = path_items(attributes[b"d"])
            for name in SHAPE_ATTRIBUTES.get(tag, ()):
                attributes[name] = hundredths(attributes[name])
            rows.append((closing, tag, attributes))
        return rows
    assert meaning(svg) == meaning(text), "record changed a flag"


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
    for sequence, svg in flags:
        if current and raw + len(svg) > GROUP_RAW_BUDGET:
            groups.append(current)
            current, raw = [], 0
        current.append((sequence, svg))
        raw += len(svg)
    groups.append(current)

    return groups


def group_flags(flags):
    # Keep each flag's inflated byte count at or below its previous adjacent group.
    baseline = adjacent_groups(flags)
    limits = {sequence: sum(len(svg) for _, svg in group)
              for group in baseline for sequence, _ in group}
    # Sample short byte strings to find shared artwork. Common XML boilerplate
    # provides little compression benefit, so omit features shared by many flags.
    features = [{svg[i:i + 6] for i in range(0, len(svg) - 5, 3)} for _, svg in flags]
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
    texts = {}
    for index, (sequence, svg) in enumerate(flags):
        record = encode(svg)
        texts[sequence] = decode(record)
        verify(svg, texts[sequence])
        flags[index] = (sequence, record)
    checksum = 0xCBF29CE484222325
    for sequence in sorted(texts):
        for byte in texts[sequence]:
            checksum = (checksum ^ byte) * 0x100000001B3 % (1 << 64)

    groups, entries = group_flags(flags), []
    assert len(groups) <= 256

    blob, group_rows, maximum_raw = bytearray(), [], 0
    for index, group in enumerate(groups):
        offsets, raw_data = {}, bytearray()
        for _, svg in group:
            if svg not in offsets:
                offsets[svg] = len(raw_data)
                raw_data += svg
        packed = best_deflate(raw_data)
        assert zlib.decompress(packed) == raw_data
        group_rows.append((len(blob), len(packed), len(raw_data)))
        blob += packed
        maximum_raw = max(maximum_raw, len(raw_data) + max(len(texts[sequence]) for sequence, _ in group))
        for sequence, svg in group:
            offset = offsets[svg]
            assert offset < 65536 and len(svg) < 65536
            entries.append((sequence, offset, len(svg), index))

    entries.sort()  # Runtime lookup remains a binary search by sequence.
    lines = ["// Generated by tools/generate_regional_flag_bundle.py; do not edit.",
             "// Twemoji artwork, CC BY 4.0, coordinates rounded. See assets/licenses/Twemoji-graphics.txt.",
             f"constexpr u64 kFlagTextChecksum=0x{checksum:016X}ull;",
             f"constexpr u32 kFlagCount={len(entries)};",
             f"constexpr u32 kFlagGroupCount={len(groups)};",
             f"constexpr u32 kFlagScratchBytes={maximum_raw};",
             "constexpr FlagGroup kFlagGroups[]={"]
    lines += [f"{{{o},{c},{r}}}," for o, c, r in group_rows]
    lines.append("};")
    lines.append("constexpr FlagEntry kFlagEntries[]={")
    lines += [f"{{{s},{o},{n},{g}}}," for s, o, n, g in entries]
    lines.append("};")
    lines.append("constexpr u8 kFlagPacked[]={")
    for i in range(0, len(blob), 40):
        lines.append(",".join(str(b) for b in blob[i:i + 40]) + ",")
    lines.append("};")
    text = "\n".join(lines) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != text:
        args.output.write_text(text, newline="\n")


if __name__ == "__main__":
    main()
