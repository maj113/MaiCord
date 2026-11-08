#!/usr/bin/env python3
"""Audits MaiCord images, Windows (PE) and Linux (ELF), with one set of options.

    audit_image.py IMAGE... [--no-test-hooks | --test-hooks] [--maximum-initialized-data-bytes N]
                            [--libc glibc|musl] [--needed NAME...]          (ELF)
                            [--static-imports DLL...] [--delay-load DLL...]  (PE)

The format is read from each image. Every image:

  * holds no test hook with --no-test-hooks, and one with --test-hooks (include/maicord/core/test_hook.hpp leaves a
    marker string in every image that does): what ships is built without them (tools/build.ps1 without -TestHooks,
    tools/build_linux.sh without --test, packaging/build_tarball.py);
  * keeps its writable initialized data within --maximum-initialized-data-bytes (PE: the file bytes of .data; ELF: the
    writable PROGBITS sections but the GOT, the init and fini arrays, .data.rel.ro and .dynamic). One nonzero default
    member initializer moves a whole zero-filled global (e.g. app_detail::g_app) from .bss into the file.

A PE image (the Windows client, the opt-in probes) must

  * be an x64 PE32+ image with an entry point, high-entropy ASLR, a dynamic base, NX and CET compatibility;
  * carry no debug data: its debug directory holds only the extended DLL characteristics (where CET compatibility
    lives), and no PDB lies beside it;
  * import nothing of a C or C++ runtime, a shader compiler, OpenSSL, a browser runtime, WebAuthn, Media Foundation or
    DirectWrite (the client opens what it needs of the system at run time);
  * import KERNEL32.dll statically; with --static-imports, exactly the DLLs listed, each once, statically;
  * delay-load exactly the DLLs of --delay-load (none without it), each once, RVA-based and not also a static import
    (the loader maps static imports before the entry point; cmake/client.cmake has the client's list), and never import
    kernel32's DelayLoadFailureHook (MaiCord's delay-load helper, src/core/delay_load.cpp, fails fast instead).

The loader appends ".dll" to a module name without an extension, so DLL names compare with it, case-insensitively.

An ELF image (a freestanding dynamic PIE, cmake/build_contract.cmake) must

  * be an x86-64 ET_DYN executable with the interpreter of its C library: /lib64/ld-linux-x86-64.so.2 (glibc, the
    builds' form) or /lib/ld-musl-x86_64.so.1 (musl: tools/retarget_libc.py makes it from the glibc image, which is
    otherwise identical);
  * need exactly libc.so.6 (the generated stub of tools/generate_libc_stubs.py) and no other library: everything else is
    dlopen()ed, and since glibc 2.34 dlopen and the pthread functions live in libc.so.6 (libdl and libpthread are empty).
    musl's loader answers a needed libc.* by itself and ignores symbol versions, so the same entry runs on both;
  * import only the functions the stub defines, each under a GLIBC_ version of at most 2.34, so no glibc newer than
    that is ever required (2.34 is the oldest glibc of a distribution that Linux 6.1 admits: Red Hat 9). Every import is
    a function musl defines too, except malloc_trim, which must be a weak import (null on musl);
  * carry no unwind data (.eh_frame, .eh_frame_hdr, .gcc_except_table), no build id and no debug sections;
  * have no text relocations, no RPATH and no RUNPATH, full RELRO (relro and bind-now) and a non-executable stack,
    and no segment that is writable and executable;
  * pack its relative relocations under the Android RELR tags if at all, never DT_RELR (src/core/linux/start.cpp
    applies them; glibc 2.36 would refuse DT_RELR without a version dependency that older glibc cannot satisfy).

Exits 1 and names the first violated rule of any image.
"""

import argparse
import os
import struct
import sys

TEST_HOOKS_MARKER = b"maicord-test-hooks-compiled-in"


class AuditError(Exception):
    pass


def check(condition, message):
    if not condition:
        raise AuditError(message)


def c_string(data, offset):
    end = data.index(b"\0", offset)
    return data[offset:end].decode("ascii")


# --- PE -------------------------------------------------------------------------------------------------------------

IMAGE_FILE_MACHINE_AMD64 = 0x8664
PE32_PLUS = 0x20B
DYNAMIC_BASE, HIGH_ENTROPY_VA, NX_COMPAT = 0x40, 0x20, 0x100
DIRECTORY_IMPORT, DIRECTORY_DEBUG, DIRECTORY_DELAY_IMPORT = 1, 6, 13
DEBUG_TYPE_EX_DLLCHARACTERISTICS = 20
EX_DLLCHARACTERISTICS_CET_COMPAT = 0x1
DELAY_ATTRIBUTE_RVA_BASED = 0x1
IMPORT_BY_ORDINAL = 1 << 63
# DLLs no image imports, statically or delayed: the C runtime, WebAuthn, browser runtimes, Media Foundation, DirectWrite.
FORBIDDEN_DLLS = {"ucrtbase.dll", "webauthn.dll", "libcef.dll", "chrome_elf.dll", "webview2loader.dll", "electron.dll",
                  "mf.dll", "mfplat.dll", "mfreadwrite.dll", "msmpg2vdec.dll", "dwrite.dll", "dwritecore.dll"}
# Text no imported DLL or function name contains (case-insensitive): the C and C++ runtimes, the shader compilers,
# OpenSSL, Media Foundation and DirectWrite entry points.
FORBIDDEN_IMPORT_TEXT = ("vcruntime", "msvcp", "api-ms-win-crt", "d3dcompiler", "dxcompiler", "libcrypto", "libssl",
                         "mfstartup", "mfcreate", "mftenum", "dwritecreatefactory", "dwritecorecreatefactory")


def dll_name(name):
    name = name.lower()
    return name if "." in name else name + ".dll"


class Pe:
    def __init__(self, path, data):
        self.data = data
        check(data[:2] == b"MZ", "not a PE image")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        check(data[pe:pe + 4] == b"PE\0\0", "no PE signature")
        self.machine, section_count, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
        optional = pe + 24
        check(struct.unpack_from("<H", data, optional)[0] == PE32_PLUS, "not a PE32+ image")
        self.entry_point = struct.unpack_from("<I", data, optional + 16)[0]
        self.dll_characteristics = struct.unpack_from("<H", data, optional + 70)[0]
        directory_count = struct.unpack_from("<I", data, optional + 108)[0]
        self.directories = [struct.unpack_from("<II", data, optional + 112 + 8 * index) if index < directory_count
                            else (0, 0) for index in range(16)]
        self.sections = []
        for index in range(section_count):
            entry = optional + optional_size + 40 * index
            name = data[entry:entry + 8].rstrip(b"\0").decode("ascii", "replace")
            virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, entry + 8)
            self.sections.append((name, virtual_size, virtual_address, raw_size, raw_pointer))

    def offset(self, rva):
        for _, _, virtual_address, raw_size, raw_pointer in self.sections:
            if virtual_address <= rva < virtual_address + raw_size:
                return raw_pointer + rva - virtual_address
        raise AuditError("RVA 0x%x lies outside every section's file bytes" % rva)

    def string(self, rva):
        return c_string(self.data, self.offset(rva))

    def thunk_names(self, rva):
        names = []
        position = self.offset(rva)
        while True:
            thunk = struct.unpack_from("<Q", self.data, position)[0]
            if not thunk:
                return names
            if not thunk & IMPORT_BY_ORDINAL:
                names.append(self.string((thunk & 0x7FFFFFFF) + 2))
            position += 8

    def imports(self):
        """[(dll, [function names])] of the import directory."""
        found = []
        rva, _ = self.directories[DIRECTORY_IMPORT]
        position = self.offset(rva) if rva else None
        while position is not None:
            lookup, _, _, name, address = struct.unpack_from("<IIIII", self.data, position)
            if not name:
                break
            found.append((self.string(name), self.thunk_names(lookup or address)))
            position += 20
        return found

    def delay_imports(self):
        """[(dll, attributes, [function names])] of the delay-load directory."""
        found = []
        rva, _ = self.directories[DIRECTORY_DELAY_IMPORT]
        position = self.offset(rva) if rva else None
        while position is not None:
            attributes, name, _, _, names, _, _, _ = struct.unpack_from("<IIIIIIII", self.data, position)
            if not name:
                break
            rva_based = attributes & DELAY_ATTRIBUTE_RVA_BASED
            found.append((self.string(name) if rva_based else "?", attributes,
                          self.thunk_names(names) if rva_based and names else []))
            position += 32
        return found

    def debug_entries(self):
        """[(type, data)] of the debug directory."""
        rva, size = self.directories[DIRECTORY_DEBUG]
        entries = []
        if rva:
            position = self.offset(rva)
            for index in range(size // 28):
                kind, data_size, _, raw_pointer = struct.unpack_from("<IIII", self.data, position + 28 * index + 12)
                entries.append((kind, self.data[raw_pointer:raw_pointer + data_size]))
        return entries


def audit_pe(path, data, arguments):
    image = Pe(path, data)
    check(image.machine == IMAGE_FILE_MACHINE_AMD64, "not an x64 image")
    check(image.entry_point, "no entry point")
    for flag, name in ((HIGH_ENTROPY_VA, "high-entropy ASLR"), (DYNAMIC_BASE, "dynamic base"), (NX_COMPAT, "NX")):
        check(image.dll_characteristics & flag, "mitigation missing: %s" % name)
    debug = image.debug_entries()
    for kind, _ in debug:
        check(kind == DEBUG_TYPE_EX_DLLCHARACTERISTICS, "debug data (debug directory entry of type %d)" % kind)
    check(any(len(payload) >= 4 and struct.unpack_from("<I", payload)[0] & EX_DLLCHARACTERISTICS_CET_COMPAT
              for _, payload in debug), "mitigation missing: CET compatibility")
    check(not os.path.exists(os.path.splitext(path)[0] + ".pdb"), "a PDB lies beside the image")

    static = image.imports()
    delayed = image.delay_imports()
    static_names = sorted(dll_name(dll) for dll, _ in static)
    delayed_names = sorted(dll_name(dll) for dll, _, _ in delayed)
    for dll in static_names + delayed_names:
        check(dll not in FORBIDDEN_DLLS, "forbidden import %s" % dll)
    for dll, functions in static + [(dll, functions) for dll, _, functions in delayed]:
        for name in [dll] + functions:
            for text in FORBIDDEN_IMPORT_TEXT:
                check(text not in name.lower(), "forbidden import %s (%s)" % (name, dll))
    check("kernel32.dll" in static_names, "KERNEL32.dll is not a static import")
    if arguments.static_imports is not None:
        expected = sorted(dll_name(dll) for dll in arguments.static_imports)
        check(static_names == expected, "static imports are [%s], expected [%s]" % (
            ", ".join(static_names), ", ".join(expected)))
    for dll, attributes, _ in delayed:
        check(attributes & DELAY_ATTRIBUTE_RVA_BASED, "delay-load descriptor of %s is not RVA-based" % dll)
    expected = sorted(dll_name(dll) for dll in arguments.delay_load)
    check(delayed_names == expected, "delay-loaded DLLs are [%s], expected [%s]" % (
        ", ".join(delayed_names), ", ".join(expected)))
    for dll in delayed_names:
        check(dll not in static_names, "%s is delay-loaded but also a static import (mapped before the entry point)" % dll)
    check(not any(dll_name(dll) == "kernel32.dll" and "DelayLoadFailureHook" in functions for dll, functions in static),
          "imports kernel32's DelayLoadFailureHook (MaiCord's delay-load helper fails fast)")

    data_section = next((section for section in image.sections if section[0] == ".data"), None)
    initialized = data_section[3] if data_section else 0
    return "PE; imports %s; delay-loads %s; initialized data %d bytes" % (
        ", ".join(static_names), ", ".join(delayed_names) or "none", initialized), initialized


# --- ELF ------------------------------------------------------------------------------------------------------------

INTERPRETERS = {"glibc": "/lib64/ld-linux-x86-64.so.2", "musl": "/lib/ld-musl-x86_64.so.1"}
EXPECTED_NEEDED = ("libc.so.6",)
# The imports of include/maicord/sys/linux/libc.hpp: all defined by glibc and musl, but for the glibc-only ones, which
# the image must import weakly.
ALLOWED_IMPORTS = {
    "dlopen", "dlsym", "dlclose", "dlerror",
    "pthread_create", "pthread_join", "pthread_detach", "pthread_attr_init", "pthread_attr_setstacksize",
    "pthread_attr_destroy",
    "clock_gettime",
    "malloc", "calloc", "free", "malloc_trim",
    "getaddrinfo", "freeaddrinfo",
}
GLIBC_ONLY_IMPORTS = {"malloc_trim"}
MAXIMUM_GLIBC = (2, 34)
FORBIDDEN_SECTIONS = (".eh_frame", ".eh_frame_hdr", ".gcc_except_table", ".note.gnu.build-id", ".rela.dyn.text")

PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_GNU_STACK, PT_GNU_RELRO = 1, 2, 3, 0x6474E551, 0x6474E552
STB_WEAK = 2
PF_X, PF_W = 1, 2
SHT_PROGBITS, SHT_DYNSYM = 1, 11
SHF_WRITE, SHF_ALLOC = 1, 2
DT_NEEDED, DT_RPATH, DT_RUNPATH, DT_TEXTREL, DT_FLAGS, DT_FLAGS_1 = 1, 15, 29, 22, 30, 0x6FFFFFFB
DT_STRTAB, DT_VERSYM, DT_VERNEED, DT_VERNEEDNUM = 5, 0x6FFFFFF0, 0x6FFFFFFE, 0x6FFFFFFF
DF_TEXTREL, DF_BIND_NOW, DF_1_NOW = 4, 8, 1
DT_RELR = 36


class Elf:
    def __init__(self, data):
        self.data = data
        check(data[:4] == b"\x7fELF", "not an ELF file")
        check(data[4] == 2 and data[5] == 1, "not a 64-bit little-endian ELF image")
        (self.type, self.machine, _, _, program_offset, section_offset, _, _, program_size,
         program_count, section_size, section_count, section_names_index) = struct.unpack_from(
            "<HHIQQQIHHHHHH", data, 16)
        self.programs = [struct.unpack_from("<IIQQQQQQ", data, program_offset + index * program_size)
                         for index in range(program_count)]
        self.sections = [struct.unpack_from("<IIQQQQIIQQ", data, section_offset + index * section_size)
                         for index in range(section_count)]
        names = self.sections[section_names_index] if self.sections else None
        self.section_names = [c_string(data, names[4] + section[0]) if names else "" for section in self.sections]

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
    return tuple(int(part) for part in name[len("GLIBC_"):].split("."))


def audit_elf(path, data, arguments):
    libc, needed = arguments.libc, arguments.needed
    image = Elf(data)
    check(image.machine == 62, "not an x86-64 image")
    check(image.type == 3, "not a position-independent executable (ET_DYN)")

    interpreters = [c_string(data, offset) for kind, _, offset, _, _, _, _, _ in image.programs if kind == PT_INTERP]
    check(interpreters == [INTERPRETERS[libc]], "interpreter is %s, expected %s for %s" % (
        interpreters, INTERPRETERS[libc], libc))

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
    # Packed relative relocations use the Android tags: glibc 2.36 refuses DT_RELR without a version dependency that
    # older glibc cannot satisfy, and a loader that does not know the tags leaves the work to src/core/linux/start.cpp.
    check(DT_RELR not in tags, "the image has DT_RELR (link with --pack-dyn-relocs=relr --use-android-relr-tags)")
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
            _, _, index, name, next_aux = struct.unpack_from("<IHHII", data, aux)
            version = c_string(data, strings + name)
            versions[index] = (library, version)
            check(version.startswith("GLIBC_") and not version.startswith("GLIBC_ABI_"),
                  "requires %s from %s (only the pinned GLIBC_x.y nodes: a GLIBC_ABI_* dependency, DT_RELR's, "
                  "stops the image on an older glibc)" % (version, library))
            check(glibc_version(version) <= MAXIMUM_GLIBC, "requires %s from %s, above GLIBC_%d.%d" % (
                version, library, MAXIMUM_GLIBC[0], MAXIMUM_GLIBC[1]))
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
        # musl has no glibc-only function: a strong import would stop the image at load time (BIND_NOW).
        check(symbol not in GLIBC_ONLY_IMPORTS or info >> 4 == STB_WEAK,
              "%s is glibc-only and must be a weak import (musl does not define it)" % symbol)
        imported.append("%s@%s%s" % (symbol, versions[version][1], " (weak)" if info >> 4 == STB_WEAK else ""))

    initialized = 0
    for section, name in zip(image.sections, image.section_names):
        if section[1] == SHT_PROGBITS and section[2] & SHF_ALLOC and section[2] & SHF_WRITE and name not in (
                ".got", ".got.plt", ".init_array", ".fini_array", ".data.rel.ro", ".dynamic"):
            initialized += section[5]
    return "ELF, %s; needs %s; imports %s; initialized data %d bytes" % (
        libc, ", ".join(needed), ", ".join(imported) or "none", initialized), initialized


# --------------------------------------------------------------------------------------------------------------------

def audit(path, arguments):
    with open(path, "rb") as handle:
        data = handle.read()
    summary, initialized = (audit_elf if data[:4] == b"\x7fELF" else audit_pe)(path, data, arguments)
    maximum = arguments.maximum_initialized_data_bytes
    check(maximum < 0 or initialized <= maximum, (
        "%d bytes of initialized data, above the budget of %d: a global with a nonzero default member initializer "
        "was likely serialized into the file; zero it in its definition and apply the default at startup") % (
        initialized, maximum))
    hooked = TEST_HOOKS_MARKER in data
    check(not (arguments.no_test_hooks and hooked), "the image holds test hooks (built with tests)")
    check(not (arguments.test_hooks and not hooked), "the image holds no test hooks (built without them)")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("images", nargs="+")
    hooks = parser.add_mutually_exclusive_group()
    hooks.add_argument("--no-test-hooks", action="store_true", help="fail when an image holds a test hook")
    hooks.add_argument("--test-hooks", action="store_true", help="fail when an image holds no test hook")
    parser.add_argument("--maximum-initialized-data-bytes", type=int, default=-1, metavar="N",
                        help="fail when the writable initialized data exceeds N bytes")
    parser.add_argument("--libc", choices=sorted(INTERPRETERS), default="glibc",
                        help="ELF: the C library whose interpreter the images name (default: glibc)")
    parser.add_argument("--needed", nargs="+", default=list(EXPECTED_NEEDED), metavar="NAME",
                        help="ELF: the exact DT_NEEDED set (default: libc.so.6)")
    parser.add_argument("--static-imports", nargs="+", metavar="DLL",
                        help="PE: the exact set of statically imported DLLs (default: any that include KERNEL32.dll)")
    parser.add_argument("--delay-load", nargs="+", default=[], metavar="DLL",
                        help="PE: the exact set of delay-loaded DLLs (default: none)")
    arguments = parser.parse_args()
    for path in arguments.images:
        try:
            summary = audit(path, arguments)
        except (AuditError, OSError, ValueError, struct.error, StopIteration, IndexError) as error:
            print("audit_image: %s: %s" % (path, error), file=sys.stderr)
            return 1
        print("Audit passed: %s (%d bytes; %s)" % (path, os.path.getsize(path), summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
