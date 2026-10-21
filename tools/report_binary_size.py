"""Report PE sections and linker-map code extents; writes no artifacts."""
import argparse
import collections
import pathlib
import re
import struct


def symbol_extents(lines, base, start, size):
    symbols = {}
    pattern = re.compile(r"\s+[0-9a-fA-F]{4}:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{16})\s+(?:f\s+)?(?:i\s+)?(.*)")
    for line in lines:
        match = pattern.fullmatch(line)
        if match:
            name, address, owner = match.groups()
            rva = int(address, 16) - base
            if start <= rva < start + size:
                # ICF aliases share one address and must be counted only once.
                symbols.setdefault(rva, (name, owner.strip()))
    addresses = sorted(symbols)
    return [(end - at, at, *symbols[at])
            for at, end in zip(addresses, addresses[1:] + [start + size])]


def report(image, limit):
    data = image.read_bytes()
    pe = struct.unpack_from("<I", data, 60)[0]
    if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"Not a PE image: {image}")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    if struct.unpack_from("<H", data, pe + 24)[0] != 0x20B:
        raise ValueError("Expected a PE32+ image")
    base = struct.unpack_from("<Q", data, pe + 48)[0]
    sections = []
    print(f"\n{image.name}: {len(data):,} file bytes")
    for index in range(count):
        at = pe + 24 + optional_size + 40 * index
        name = data[at:at + 8].rstrip(b"\0").decode("ascii")
        size, address, raw_size, _ = struct.unpack_from("<IIII", data, at + 8)
        sections.append((name, address, size))
        print(f"  {name:8} file={raw_size:9,} virtual={size:9,}")
    lines = image.with_suffix(".map").read_text().splitlines()
    for section, start, size in sections:
        if section not in (".text", ".rdata"):
            continue
        extents = symbol_extents(lines, base, start, size)
        print(f"  {section} symbol extents (include alignment and unnamed ranges; ICF owner is first alias):")
        for length, rva, name, owner in sorted(extents, reverse=True)[:limit]:
            print(f"  {length:9,} RVA={rva:08x} {name} [{owner}]")
        owners = collections.Counter()
        for length, _, _, owner in extents:
            owners[owner] += length
        print("  Object totals by the same extent estimate:")
        for owner, length in owners.most_common(limit):
            print(f"  {length:9,} {owner}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", type=pathlib.Path, nargs="+")
    parser.add_argument("--limit", type=int, default=30)
    args = parser.parse_args()
    for image in args.images:
        report(image, args.limit)
