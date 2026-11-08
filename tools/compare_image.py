#!/usr/bin/env python3
"""Compares two builds of an image section by section: MaiCord.exe (PE) or the Linux MaiCord (ELF).

    compare_image.py OLD NEW [--runs N] [--allow SECTION ...]

The identity gate of shared-code changes: the bytes a rebuild always changes are ignored, on PE the COFF TimeDateStamp
(a content hash under /Brepro), the optional header CheckSum and the debug directory with the data it points to; an ELF
image has none (no build id, no timestamp). Everything else must match: the headers before the first section (PE: DOS,
COFF and optional headers with the section table; ELF: the ELF and program headers), the section table (names, sizes,
addresses, flags) and every section's bytes. A difference is reported per section with the number of differing bytes
and the first runs (address, old and new bytes); an image is "identical" when nothing differs after the masking.

Exit status 0 when identical (or when only --allow sections differ), 1 otherwise.
"""

import argparse
import struct
import sys


class Image:
    """`data` (masked), `headers_end` and `sections`: (name, memory size, address, file size, file offset, flags)."""

    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = bytearray(handle.read())
        if self.data[:4] == b"\x7fELF":
            self.read_elf(path)
        else:
            self.read_pe(path)
        self.headers_end = min((section[4] for section in self.sections if section[3]), default=len(self.data))

    def read_elf(self, path):
        data = self.data
        if data[4] != 2 or data[5] != 1:
            raise SystemExit("%s: not a 64-bit little-endian ELF image" % path)
        section_offset = struct.unpack_from("<Q", data, 40)[0]
        section_size, section_count, names_index = struct.unpack_from("<HHH", data, 58)
        headers = [struct.unpack_from("<IIQQQQIIQQ", data, section_offset + index * section_size)
                   for index in range(section_count)]
        names = headers[names_index][4] if headers else 0
        self.sections = []
        for name, kind, flags, address, offset, size, _, _, _, _ in headers[1:]:
            label = data[names + name:data.index(b"\0", names + name)].decode("ascii", "replace")
            self.sections.append((label, size, address, 0 if kind == 8 else size, offset, (kind, flags)))  # 8: NOBITS

    def read_pe(self, path):
        data = self.data
        if data[:2] != b"MZ":
            raise SystemExit("%s: neither a PE nor an ELF image" % path)
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise SystemExit("%s: no PE signature" % path)
        coff = pe_offset + 4
        section_count, _, _, _, optional_size = struct.unpack_from("<HIIIH", data, coff + 2)
        optional = coff + 20
        if struct.unpack_from("<H", data, optional)[0] != 0x20B:
            raise SystemExit("%s: not a PE32+ image" % path)
        table = optional + optional_size
        self.sections = []
        for index in range(section_count):
            entry = table + 40 * index
            name = data[entry:entry + 8].rstrip(b"\0").decode("ascii", "replace")
            virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, entry + 8)
            characteristics = struct.unpack_from("<I", data, entry + 36)[0]
            self.sections.append((name, virtual_size, virtual_address, raw_size, raw_pointer, characteristics))
        # Masked: the timestamp, the checksum and the debug directory with its data.
        self.zero(coff + 4, 4)
        self.zero(optional + 64, 4)
        rva, size = struct.unpack_from("<II", data, optional + 112 + 8 * 6)
        start = self.file_offset(rva) if rva else None
        if start is not None:
            for entry in range(size // 28):
                position = start + 28 * entry
                data_size, _, raw_pointer = struct.unpack_from("<III", data, position + 16)
                self.zero(raw_pointer, data_size)
                self.zero(position, 28)

    def file_offset(self, rva):
        for _, virtual_size, virtual_address, raw_size, raw_pointer, _ in self.sections:
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                return raw_pointer + rva - virtual_address
        return None

    def zero(self, offset, size):
        if offset is not None and offset + size <= len(self.data):
            self.data[offset:offset + size] = bytes(size)


def runs(old, new, limit):
    """The first `limit` runs of differing bytes (gaps under 16 bytes merge), and the total differing bytes."""
    found = []
    total = 0
    position = 0
    length = min(len(old), len(new))
    while position < length:
        if old[position] == new[position]:
            position += 1
            continue
        start = position
        last = position
        while position < length and position - last < 16:
            if old[position] != new[position]:
                last = position
                total += 1
            position += 1
        if len(found) < limit:
            found.append((start, last + 1))
    return found, total + abs(len(old) - len(new))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("old")
    parser.add_argument("new")
    parser.add_argument("--runs", type=int, default=4, help="differing runs to show per section (default 4)")
    parser.add_argument("--allow", nargs="*", default=[], help="sections whose differences do not fail the gate")
    arguments = parser.parse_args()
    old, new = Image(arguments.old), Image(arguments.new)
    differing = []

    print("old: %s (%d bytes)\nnew: %s (%d bytes)" % (arguments.old, len(old.data), arguments.new, len(new.data)))
    if old.sections != new.sections:
        print("section table differs:")
        for name in sorted({section[0] for section in old.sections + new.sections}):
            before = next((s for s in old.sections if s[0] == name), None)
            after = next((s for s in new.sections if s[0] == name), None)
            if before != after:
                print("  %-8s old %s new %s" % (name, before and before[1:5], after and after[1:5]))
                differing.append("section table")
    header_old = old.data[:old.headers_end]
    header_new = new.data[:new.headers_end]
    header_runs, header_total = runs(header_old, header_new, arguments.runs)
    print("%-10s %s" % ("headers", "identical" if not header_total else "%d bytes differ" % header_total))
    if header_total:
        differing.append("headers")
        for start, end in header_runs:
            print("    +0x%x: %s -> %s" % (start, header_old[start:end].hex(), header_new[start:end].hex()))

    for before in old.sections:
        name = before[0]
        after = next((s for s in new.sections if s[0] == name), None)
        if after is None:
            print("%-10s missing from new" % name)
            differing.append(name)
            continue
        old_bytes = old.data[before[4]:before[4] + before[3]]
        new_bytes = new.data[after[4]:after[4] + after[3]]
        found, total = runs(old_bytes, new_bytes, arguments.runs)
        status = "identical (%d bytes)" % len(old_bytes) if not total else "%d of %d bytes differ" % (total, len(old_bytes))
        print("%-10s %s" % (name, status))
        if total:
            if name not in arguments.allow:
                differing.append(name)
            for start, end in found:
                print("    at 0x%x: %s -> %s" % (before[2] + start, old_bytes[start:end][:24].hex(),
                                                new_bytes[start:end][:24].hex()))
    for section in new.sections:
        if all(section[0] != s[0] for s in old.sections):
            print("%-10s only in new" % section[0])
            differing.append(section[0])

    if differing:
        print("DIFFERENT: %s" % ", ".join(dict.fromkeys(differing)))
        return 1
    print("IDENTICAL: headers and every section match%s" % (
        " (timestamp, checksum and debug directory ignored)" if old.data[:2] == b"MZ" else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
