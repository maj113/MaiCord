#!/usr/bin/env python3
"""Makes the musl form of a Linux MaiCord image: the same bytes but for the program interpreter.

The build links one image per target, for glibc (cmake/build_contract.cmake: interpreter
/lib64/ld-linux-x86-64.so.2, needed libc.so.6 alone, imports under GLIBC_2.2.5, 2.17 and 2.34). musl's
loader runs it unchanged: it answers a needed libc.* by itself (the reserved names of ldso/dynlink.c's load_library:
libc, libpthread, librt, libm, libdl, libutil, libxnet), takes every import from itself and ignores the symbol
versions, and the one function musl lacks, malloc_trim, is a weak import. Only the kernel's PT_INTERP differs, so this
rewrites that string in place (and PT_INTERP's size, and the .interp section's) and nothing else: the code, the stub
and the audit's rules are the glibc image's, which is what the tests ran.

    retarget_libc.py --libc musl IMAGE OUTPUT
    retarget_libc.py --libc musl --tree DIRECTORY        every executable image under DIRECTORY, in place

The interpreter must fit where the old one was (musl's path is shorter than glibc's), so a glibc image becomes a musl
one and not the other way round. tools/audit_image.py --libc musl audits the result.
"""

import argparse
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audit_image import INTERPRETERS, PT_INTERP  # noqa: E402


def retarget(source, destination, libc):
    data = bytearray(open(source, "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit("retarget_libc: %s is not a 64-bit little-endian ELF image" % source)
    program_offset, section_offset = struct.unpack_from("<QQ", data, 32)
    program_size, program_count, section_size, section_count, names_index = struct.unpack_from("<HHHHH", data, 54)
    interpreter = INTERPRETERS[libc].encode("ascii") + b"\0"
    found = False
    for index in range(program_count):
        header = program_offset + index * program_size
        kind, _, offset, address, _, file_size, memory_size, _ = struct.unpack_from("<IIQQQQQQ", data, header)
        if kind != PT_INTERP:
            continue
        if len(interpreter) > file_size:
            raise SystemExit("retarget_libc: %s's interpreter field (%d bytes) cannot hold %s (%d): a glibc image is "
                             "retargeted to musl, not back" % (source, file_size, INTERPRETERS[libc], len(interpreter)))
        data[offset:offset + file_size] = interpreter + bytes(file_size - len(interpreter))
        struct.pack_into("<QQ", data, header + 32, len(interpreter), len(interpreter))
        # The .interp section (stripped images keep their section headers).
        if section_count:
            names = struct.unpack_from("<IIQQQQIIQQ", data, section_offset + names_index * section_size)[4]
            for number in range(section_count):
                entry = section_offset + number * section_size
                name = struct.unpack_from("<I", data, entry)[0]
                end = data.index(b"\0", names + name)
                if data[names + name:end] == b".interp":
                    struct.pack_into("<Q", data, entry + 32, len(interpreter))
        found = True
    if not found:
        raise SystemExit("retarget_libc: %s has no PT_INTERP" % source)
    with open(destination, "wb") as handle:
        handle.write(data)
    shutil.copymode(source, destination)


def retarget_tree(directory, libc):
    """Retargets every executable image (an ELF file with a program interpreter) under `directory` in place."""
    count = 0
    for root, _, names in os.walk(directory):
        for name in names:
            path = os.path.join(root, name)
            with open(path, "rb") as handle:
                head = handle.read(64)
            if head[:4] != b"\x7fELF" or head[4] != 2 or not has_interpreter(path):
                continue
            retarget(path, path, libc)
            count += 1
    return count


def has_interpreter(path):
    data = open(path, "rb").read()
    program_offset = struct.unpack_from("<Q", data, 32)[0]
    program_size, program_count = struct.unpack_from("<HH", data, 54)
    return any(struct.unpack_from("<I", data, program_offset + index * program_size)[0] == PT_INTERP
               for index in range(program_count))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--libc", choices=sorted(INTERPRETERS), required=True)
    parser.add_argument("--tree", metavar="DIRECTORY")
    parser.add_argument("paths", nargs="*", metavar="IMAGE OUTPUT")
    arguments = parser.parse_args()
    if arguments.tree:
        if arguments.paths:
            parser.error("--tree takes no image and output")
        print("retargeted %d images under %s to %s" % (retarget_tree(arguments.tree, arguments.libc), arguments.tree,
                                                       arguments.libc))
    elif len(arguments.paths) == 2:
        retarget(arguments.paths[0], arguments.paths[1], arguments.libc)
    else:
        parser.error("give IMAGE OUTPUT, or --tree DIRECTORY")
    return 0


if __name__ == "__main__":
    sys.exit(main())
