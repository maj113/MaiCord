#!/usr/bin/env python3
"""Audits Linux MaiCord images: the ELF twin of tools/audit_pe.ps1.

A MaiCord image is a freestanding dynamic PIE (see cmake/build_contract_linux.cmake). It must

  * be an x86-64 ET_DYN executable with the interpreter /lib64/ld-linux-x86-64.so.2;
  * need exactly libc.so.6, libdl.so.2 and libpthread.so.0 (the generated stubs of tools/generate_libc_stubs.py) and
    no other library: everything else is dlopen()ed;
  * import only the functions the stubs define, each under a GLIBC_ version of at most 2.17, so no glibc newer than
    that is ever required;
  * carry no unwind data (.eh_frame, .eh_frame_hdr, .gcc_except_table) and no debug sections;
  * have no text relocations, no RPATH and no RUNPATH, full RELRO (relro and bind-now) and a non-executable stack,
    and no segment that is writable and executable.

    audit_elf.py IMAGE... [--needed NAME...] [--maximum-initialized-data-bytes N]

Exits 1 and names the first violated rule of any image.
"""

import argparse
import struct
import sys

EXPECTED_INTERPRETER = "/lib64/ld-linux-x86-64.so.2"
EXPECTED_NEEDED = ("libc.so.6", "libdl.so.2", "libpthread.so.0")
# The imports of include/maicord/sys/linux/libc.hpp.
ALLOWED_IMPORTS = {
    "dlopen", "dlsym", "dlclose", "dlerror",
    "pthread_create", "pthread_join", "pthread_detach", "pthread_attr_init", "pthread_attr_setstacksize",
    "pthread_attr_destroy",
    "clock_gettime",
    "malloc", "calloc", "free",
    "getaddrinfo", "freeaddrinfo",
}
MAXIMUM_GLIBC = (2, 17)
FORBIDDEN_SECTIONS = (".eh_frame", ".eh_frame_hdr", ".gcc_except_table", ".note.gnu.build-id", ".rela.dyn.text")

PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_GNU_STACK, PT_GNU_RELRO = 1, 2, 3, 0x6474E551, 0x6474E552
PF_X, PF_W = 1, 2
SHT_PROGBITS, SHT_DYNSYM, SHT_NOBITS = 1, 11, 8
SHF_WRITE, SHF_ALLOC = 1, 2
DT_NEEDED, DT_RPATH, DT_RUNPATH, DT_TEXTREL, DT_FLAGS, DT_FLAGS_1 = 1, 15, 29, 22, 30, 0x6FFFFFFB
DT_STRTAB, DT_SYMTAB, DT_VERSYM, DT_VERNEED, DT_VERNEEDNUM = 5, 6, 0x6FFFFFF0, 0x6FFFFFFE, 0x6FFFFFFF
DF_TEXTREL, DF_BIND_NOW, DF_1_NOW = 4, 8, 1


class AuditError(Exception):
    pass


def check(condition, message):
    if not condition:
        raise AuditError(message)


def c_string(data, offset):
    end = data.index(b"\0", offset)
    return data[offset:end].decode("ascii")


class Image:
    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()
        data = self.data
        check(data[:4] == b"\x7fELF", "not an ELF file")
        check(data[4] == 2 and data[5] == 1, "not a 64-bit little-endian ELF image")
        (self.type, self.machine, _, _, self.program_offset, self.section_offset, _, _, self.program_size,
         self.program_count, self.section_size, self.section_count, self.section_names_index) = struct.unpack_from(
            "<HHIQQQIHHHHHH", data, 16)
        self.programs = []
        for index in range(self.program_count):
            self.programs.append(struct.unpack_from("<IIQQQQQQ", data, self.program_offset + index * self.program_size))
        self.sections = []
        for index in range(self.section_count):
            self.sections.append(struct.unpack_from("<IIQQQQIIQQ", data, self.section_offset + index * self.section_size))
        names = self.sections[self.section_names_index] if self.sections else None
        self.section_names = []
        for section in self.sections:
            self.section_names.append(c_string(data, names[4] + section[0]) if names else "")

    def offset_of(self, address):
        for kind, _, offset, vaddr, _, file_size, _, _ in self.programs:
            if kind == PT_LOAD and vaddr <= address < vaddr + file_size:
                return offset + address - vaddr
        raise AuditError("dynamic table address 0x%x is outside every loaded segment" % address)

    def dynamic(self):
        entries = []
        for kind, _, offset, _, _, size, _, _ in self.programs:
            if kind == PT_DYNAMIC:
                for position in range(offset, offset + size, 16):
                    tag, value = struct.unpack_from("<qQ", self.data, position)
                    if tag == 0:
                        break
                    entries.append((tag, value))
        return entries


def glibc_version(name):
    parts = name[len("GLIBC_"):].split(".")
    return tuple(int(part) for part in parts)


def audit(path, needed, maximum_data):
    image = Image(path)
    data = image.data
    check(image.machine == 62, "not an x86-64 image")
    check(image.type == 3, "not a position-independent executable (ET_DYN)")

    interpreters = [c_string(data, offset) for kind, _, offset, _, _, _, _, _ in image.programs if kind == PT_INTERP]
    check(interpreters == [EXPECTED_INTERPRETER], "interpreter is %s, expected %s" % (interpreters, EXPECTED_INTERPRETER))

    dynamic = image.dynamic()
    check(dynamic, "no dynamic section")
    tags = {}
    for tag, value in dynamic:
        tags.setdefault(tag, value)
    strings = image.offset_of(tags[DT_STRTAB])
    actual = sorted(c_string(data, strings + value) for tag, value in dynamic if tag == DT_NEEDED)
    check(actual == sorted(needed), "needed libraries are %s, expected %s" % (actual, sorted(needed)))
    check(DT_RPATH not in tags and DT_RUNPATH not in tags, "the image has an RPATH or RUNPATH")
    check(DT_TEXTREL not in tags and not tags.get(DT_FLAGS, 0) & DF_TEXTREL, "the image has text relocations")
    check(tags.get(DT_FLAGS, 0) & DF_BIND_NOW or tags.get(DT_FLAGS_1, 0) & DF_1_NOW, "the image is not linked -z now")
    kinds = {kind for kind, _, _, _, _, _, _, _ in image.programs}
    check(PT_GNU_RELRO in kinds, "the image has no RELRO segment")
    stacks = [flags for kind, flags, _, _, _, _, _, _ in image.programs if kind == PT_GNU_STACK]
    check(stacks and not stacks[0] & PF_X, "the stack is executable or unmarked")
    for kind, flags, _, _, _, _, _, _ in image.programs:
        check(kind != PT_LOAD or flags & (PF_W | PF_X) != (PF_W | PF_X), "a segment is writable and executable")

    check(image.sections, "the image has no section headers to audit")
    for name in image.section_names:
        check(name not in FORBIDDEN_SECTIONS, "forbidden section %s (unwind data or build id)" % name)
        check(not name.startswith(".debug"), "debug section %s" % name)

    # Imports: undefined dynamic symbols, each bound to a GLIBC_ version node of an allowed library. An image that
    # calls nothing of libc has none, and then no version information either.
    symbol_table = next(section for section in image.sections if section[1] == SHT_DYNSYM)
    has_imports = any(
        struct.unpack_from("<H", data, symbol_table[4] + index * 24 + 6)[0] == 0
        for index in range(1, symbol_table[5] // 24))
    check(not has_imports or (DT_VERNEED in tags and DT_VERSYM in tags), "the imports carry no symbol versions")
    versions = {}
    position = image.offset_of(tags[DT_VERNEED]) if DT_VERNEED in tags else 0
    for _ in range(tags.get(DT_VERNEEDNUM, 0)):
        _, auxiliaries, file_name, aux_offset, next_offset = struct.unpack_from("<HHIII", data, position)
        library = c_string(data, strings + file_name)
        check(library in needed, "versions are required from %s, which is not a needed library" % library)
        aux = position + aux_offset
        for _ in range(auxiliaries):
            _, flags, index, name, next_aux = struct.unpack_from("<IHHII", data, aux)
            version = c_string(data, strings + name)
            versions[index] = (library, version)
            check(version.startswith("GLIBC_"), "%s requires %s from %s" % (path, version, library))
            check(glibc_version(version) <= MAXIMUM_GLIBC, "%s requires %s from %s, above GLIBC_%d.%d" % (
                path, version, library, MAXIMUM_GLIBC[0], MAXIMUM_GLIBC[1]))
            aux += next_aux
        position += next_offset
    version_table = image.offset_of(tags[DT_VERSYM]) if DT_VERSYM in tags else 0
    imported = []
    for index in range(symbol_table[5] // 24):
        name, info, _, section, _, _ = struct.unpack_from("<IBBHQQ", data, symbol_table[4] + index * 24)
        if index == 0 or section != 0:
            continue
        symbol = c_string(data, strings + name)
        version = struct.unpack_from("<H", data, version_table + index * 2)[0] & 0x7FFF
        check(symbol in ALLOWED_IMPORTS, "unexpected import %s" % symbol)
        check(version in versions, "import %s is unversioned" % symbol)
        imported.append("%s@%s" % (symbol, versions[version][1]))

    initialized = 0
    for section, name in zip(image.sections, image.section_names):
        if section[1] == SHT_PROGBITS and section[2] & SHF_ALLOC and section[2] & SHF_WRITE and name not in (
                ".got", ".got.plt", ".init_array", ".fini_array", ".data.rel.ro", ".dynamic"):
            initialized += section[5]
    if maximum_data >= 0:
        check(initialized <= maximum_data, "%d bytes of initialized data, above the budget of %d" % (initialized,
                                                                                                  maximum_data))
    return imported, initialized


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("images", nargs="+")
    parser.add_argument("--needed", nargs="+", default=list(EXPECTED_NEEDED),
                        help="the exact DT_NEEDED set (default: libc.so.6 libdl.so.2 libpthread.so.0)")
    parser.add_argument("--maximum-initialized-data-bytes", type=int, default=-1,
                        help="fail when the writable initialized data exceeds this many bytes")
    arguments = parser.parse_args()
    for path in arguments.images:
        try:
            imported, initialized = audit(path, arguments.needed, arguments.maximum_initialized_data_bytes)
        except (AuditError, ValueError, struct.error, StopIteration) as error:
            print("audit_elf: %s: %s" % (path, error), file=sys.stderr)
            return 1
        print("ELF audit passed: %s (needs %s; imports %s; initialized data %d bytes)" % (
            path, ", ".join(arguments.needed), ", ".join(imported) if imported else "none", initialized))
    return 0


if __name__ == "__main__":
    sys.exit(main())
