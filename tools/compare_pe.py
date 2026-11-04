#!/usr/bin/env python3
"""Compares two builds of a PE image section by section: the Windows identity gate of the Linux port.

    compare_pe.py OLD.exe NEW.exe [--runs N] [--allow SECTION ...]

The bytes a rebuild always changes are ignored: the COFF TimeDateStamp (a content hash under /Brepro), the optional
header CheckSum and the debug directory with the data it points to. Everything else must match: the headers, the
section table (names, sizes, addresses, flags) and every section's bytes. A difference is reported per section with
the number of differing bytes and the first runs (RVA, old and new bytes); an image is "identical" when nothing
differs after the masking.

Exit status 0 when identical (or when only --allow sections differ), 1 otherwise.
"""

import argparse
import struct
import sys


class Pe:
    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = bytearray(handle.read())
        data = self.data
        if data[:2] != b"MZ":
            raise SystemExit("%s: not a PE image" % path)
        self.pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if data[self.pe_offset:self.pe_offset + 4] != b"PE\0\0":
            raise SystemExit("%s: no PE signature" % path)
        coff = self.pe_offset + 4
        (self.machine, self.section_count, self.timestamp, _, _, optional_size, _) = struct.unpack_from(
            "<HHIIIHH", data, coff)
        self.timestamp_offset = coff + 4
        self.optional_offset = coff + 20
        magic = struct.unpack_from("<H", data, self.optional_offset)[0]
        if magic != 0x20B:
            raise SystemExit("%s: not a PE32+ image" % path)
        self.checksum_offset = self.optional_offset + 64
        self.entry_point = struct.unpack_from("<I", data, self.optional_offset + 16)[0]
        directories = self.optional_offset + 112
        self.directories = [struct.unpack_from("<II", data, directories + 8 * index) for index in range(16)]
        table = self.optional_offset + optional_size
        self.table_offset = table
        self.sections = []
        for index in range(self.section_count):
            raw = data[table + 40 * index:table + 40 * (index + 1)]
            name = raw[:8].rstrip(b"\0").decode("ascii", "replace")
            virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", raw, 8)
            characteristics = struct.unpack_from("<I", raw, 36)[0]
            self.sections.append((name, virtual_size, virtual_address, raw_size, raw_pointer, characteristics))
        self.headers_end = min(section[4] for section in self.sections if section[3])
        self.mask()

    def file_offset(self, rva):
        for _, virtual_size, virtual_address, raw_size, raw_pointer, _ in self.sections:
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                return raw_pointer + rva - virtual_address
        return None

    def zero(self, offset, size):
        if offset is not None and offset + size <= len(self.data):
            self.data[offset:offset + size] = bytes(size)

    def mask(self):
        """Zeroes the timestamp, the checksum and the debug directory with its data."""
        self.zero(self.timestamp_offset, 4)
        self.zero(self.checksum_offset, 4)
        rva, size = self.directories[6]
        start = self.file_offset(rva) if rva else None
        if start is not None:
            for entry in range(size // 28):
                position = start + 28 * entry
                data_size, _, raw_pointer = struct.unpack_from("<III", self.data, position + 16)
                self.zero(raw_pointer, data_size)
                self.zero(position, 28)


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
    old, new = Pe(arguments.old), Pe(arguments.new)
    differing = []

    print("old: %s (%d bytes)\nnew: %s (%d bytes)" % (arguments.old, len(old.data), arguments.new, len(new.data)))
    if old.sections != new.sections:
        print("section table differs:")
        for name in sorted({section[0] for section in old.sections + new.sections}):
            before = next((s for s in old.sections if s[0] == name), None)
            after = next((s for s in new.sections if s[0] == name), None)
            if before != after:
                print("  %-8s old %s new %s" % (name, before and before[1:5], after and after[1:5]))
    header_old = old.data[:old.headers_end]
    header_new = new.data[:new.headers_end]
    # Headers: everything before the first section's data, which includes the section table, entry point and sizes.
    header_runs, header_total = runs(header_old, header_new, arguments.runs)
    print("%-10s %s" % ("headers", "identical" if not header_total else "%d bytes differ" % header_total))
    if header_total:
        differing.append("headers")
        for start, end in header_runs:
            print("    +0x%x: %s -> %s" % (start, header_old[start:end].hex(), header_new[start:end].hex()))

    for name in [section[0] for section in old.sections]:
        before = next(s for s in old.sections if s[0] == name)
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
                print("    rva 0x%x: %s -> %s" % (before[2] + start, old_bytes[start:end][:24].hex(),
                                                 new_bytes[start:end][:24].hex()))
    for name in [section[0] for section in new.sections if all(section[0] != s[0] for s in old.sections)]:
        print("%-10s only in new" % name)
        differing.append(name)

    if differing:
        print("DIFFERENT: %s" % ", ".join(differing))
        return 1
    print("IDENTICAL: headers and every section match (timestamp, checksum and debug directory ignored)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
