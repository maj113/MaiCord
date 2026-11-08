"""Report PE sections and linker-map code extents, or the sections and symbol sizes of an ELF image; writes no artifacts.

A Linux client is linked stripped; tools/build_linux.sh --size-report keeps its symbol table and runs this report (the
section sizes of the stripped image are the same)."""
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


def mangled_group(name):
    """The namespace an Itanium-mangled C++ name lives in: the first two nested identifiers (maicord::dbus), else the name."""
    if not name.startswith("_ZN"):
        return name
    at, parts = 3, []
    while at < len(name) and len(parts) < 2:
        while at < len(name) and name[at] in "KLrV":
            at += 1
        digits = at
        while digits < len(name) and name[digits].isdigit():
            digits += 1
        if digits == at:
            break
        length = int(name[at:digits])
        parts.append(name[digits:digits + length])
        at = digits + length
    return "::".join(parts) if parts else name


def report_elf(image, data, limit):
    section_offset, = struct.unpack_from("<Q", data, 40)
    entry_size, count, names_index = struct.unpack_from("<HHH", data, 58)
    sections = []
    for index in range(count):
        sections.append(struct.unpack_from("<IIQQQQIIQQ", data, section_offset + index * entry_size))
    names = sections[names_index][4]

    def section_name(section):
        end = data.index(b"\0", names + section[0])
        return data[names + section[0]:end].decode("ascii")

    print(f"\n{image.name}: {len(data):,} file bytes")
    for section in sections[1:]:
        if section[1] != 8:  # SHT_NOBITS occupies no file bytes
            print(f"  {section_name(section):16} file={section[5]:9,}")
        else:
            print(f"  {section_name(section):16} file={0:9,} (bss {section[5]:,})")
    table = next((s for s in sections if s[1] == 2), None)  # SHT_SYMTAB
    if not table:
        print("  no symbol table: stripped (build with --size-report for symbol sizes)")
        return
    strings = sections[table[6]][4]
    wanted = {".text": "text", ".rodata": "rodata"}
    totals = {kind: collections.Counter() for kind in wanted.values()}
    largest = {kind: [] for kind in wanted.values()}
    for at in range(table[4], table[4] + table[5], 24):
        name_offset, info, _, shndx, _, size = struct.unpack_from("<IBBHQQ", data, at)
        if size == 0 or info & 15 not in (1, 2) or shndx >= count:
            continue
        kind = wanted.get(section_name(sections[shndx]))
        if kind:
            name = data[strings + name_offset:data.index(b"\0", strings + name_offset)].decode("ascii", "replace")
            totals[kind][mangled_group(name)] += size
            largest[kind].append((size, name))
    for section, kind in wanted.items():
        print(f"  {section} symbol sizes by namespace (named symbols only):")
        for group, length in totals[kind].most_common(limit):
            print(f"  {length:9,} {group}")
        print(f"  {section} largest symbols:")
        for length, name in sorted(largest[kind], reverse=True)[:limit]:
            print(f"  {length:9,} {name[:110]}")


def report(image, limit):
    data = image.read_bytes()
    if data[:4] == b"\x7fELF":
        return report_elf(image, data, limit)
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
