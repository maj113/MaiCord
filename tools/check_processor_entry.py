#!/usr/bin/env python3
"""Checks that the client runs only first x86-64 instructions until its processor check decides.

    check_processor_entry.py IMAGE MAP
    check_processor_entry.py --self-test

IMAGE is the client, MaiCord.exe (PE) or MaiCord (ELF), and MAP is the linker map of its link (lld-link /MAP or
ld.lld -Map): the image itself has no symbol table. tools/audit_image.py runs the check after every link of the client
(--processor-entry-map, cmake/client.cmake).

The client refuses to start on a processor older than its baseline (include/maicord/app/shell/processor_check.hpp).
Code that runs before processor_too_old() returns runs on any x86-64 processor, so it must use only the instructions of
the first x86-64 (SSE2), as MAICORD_ANY_PROCESSOR compiles it. The check walks the machine code from the image's entry
point and follows every branch and every direct call:

  * a call of processor_too_old must be followed by `test al, al` and je or jne: the branch taken when the processor is
    supported is where the walk stops, and the refusal branch is walked;
  * code after a call returns only when the callee can return, and the code after a callee that reaches the check runs
    after the check;
  * indirect calls (imports) and jumps through a RIP-relative slot (import thunks, PLT entries) leave the image: the
    system's code is not checked. Any other indirect jump fails the check, as the walk cannot follow it.

Every instruction reached must be one of the first x86-64: no VEX, EVEX or XOP encoding, nothing of the 0F 38 and 0F 3A
maps (SSSE3, SSE4, AES, MOVBE, CRC32, ADX), no POPCNT, LZCNT or TZCNT, no SSE3 and no CMPXCHG16B. Exits 1 and names
the first instruction that is not, with its address and the function the map places it in.
"""

import re
import struct
import sys

# processor_too_old as lld-link's map names it, as ld.lld's map names its section (-ffunction-sections) and as ld.lld's
# map names the symbol (demangled).
CHECK_SYMBOLS = ("?processor_too_old@maicord@@YA_NXZ", "_ZN7maicord17processor_too_oldEv", "maicord::processor_too_old()")


class EntryError(Exception):
    pass


# --- Instruction lengths -------------------------------------------------------------------------------------------

LEGACY_PREFIXES = frozenset((0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3))
# One-byte opcodes with a ModRM byte (64-bit mode).
MODRM_1 = frozenset(
    [o + i for o in range(0x00, 0x40, 8) for i in range(4)] + [0x63, 0x69, 0x6B] + list(range(0x80, 0x90)) +
    [0xC0, 0xC1, 0xC6, 0xC7, 0xD0, 0xD1, 0xD2, 0xD3] + list(range(0xD8, 0xE0)) + [0xF6, 0xF7, 0xFE, 0xFF])
IMM8_1 = frozenset([o + 4 for o in range(0x00, 0x40, 8)] + [0x6A, 0x6B, 0x80, 0x83, 0xA8, 0xC0, 0xC1, 0xC6, 0xCD,
                                                             0xE4, 0xE5, 0xE6, 0xE7] + list(range(0xB0, 0xB8)))
IMMZ_1 = frozenset([o + 5 for o in range(0x00, 0x40, 8)] + [0x68, 0x69, 0x81, 0xA9, 0xC7])
REL8 = frozenset(list(range(0x70, 0x80)) + [0xE0, 0xE1, 0xE2, 0xE3, 0xEB])
INVALID_1 = frozenset((0x06, 0x07, 0x0E, 0x16, 0x17, 0x1E, 0x1F, 0x27, 0x2F, 0x37, 0x3F, 0x60, 0x61, 0x62, 0x82, 0x9A,
                       0xC4, 0xC5, 0xD4, 0xD5, 0xD6, 0xEA))
# Two-byte (0F) opcodes without a ModRM byte, and those with an imm8 after it.
NO_MODRM_2 = frozenset([0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x37, 0x77, 0xA0, 0xA1,
                        0xA2, 0xA8, 0xA9, 0xAA] + list(range(0x80, 0x90)) + list(range(0xC8, 0xD0)))
IMM8_2 = frozenset((0x70, 0x71, 0x72, 0x73, 0xA4, 0xAC, 0xBA, 0xC2, 0xC4, 0xC5, 0xC6))
# SSE3 in the 0F map, by mandatory prefix (None: no prefix).
SSE3 = {(0xF2, 0x7C), (0xF2, 0x7D), (0x66, 0x7C), (0x66, 0x7D), (0xF3, 0x12), (0xF3, 0x16), (0xF2, 0x12),
        (0x66, 0xD0), (0xF2, 0xD0), (0xF2, 0xF0)}


class Instruction:
    __slots__ = ("address", "length", "text", "late", "branch", "target", "rip_slot", "modrm")


def modrm_length(code, at):
    """Bytes of the ModRM byte, its SIB and displacement; whether it addresses memory relative to RIP."""
    modrm = code[at]
    mod, rm = modrm >> 6, modrm & 7
    length = 1
    if mod != 3 and rm == 4:
        sib = code[at + 1]
        length += 1
        if mod == 0 and sib & 7 == 5:
            length += 4
    if mod == 1:
        length += 1
    elif mod == 2 or (mod == 0 and rm == 5):
        length += 4
    return length, mod == 0 and rm == 5


def decode(code, at, address):
    """Decodes one instruction of `code` at offset `at`, which the image maps at `address`."""
    start = at
    prefixes = []
    while code[at] in LEGACY_PREFIXES:
        prefixes.append(code[at])
        at += 1
    rex = code[at] if 0x40 <= code[at] <= 0x4F else 0
    if rex:
        at += 1
    opcode = code[at]
    at += 1
    operand16 = 0x66 in prefixes
    mandatory = next((p for p in reversed(prefixes) if p in (0x66, 0xF2, 0xF3)), None)
    ins = Instruction()
    ins.address, ins.late, ins.branch, ins.target, ins.rip_slot, ins.modrm = address, None, None, None, False, None

    def finish(text):
        ins.length = at - start
        ins.text = text
        return ins

    if opcode in (0xC4, 0xC5, 0x62):
        # VEX and EVEX: the prefix names the opcode map (1: 0F, 2: 0F 38, 3: 0F 3A). XOP below is not decoded further.
        ins.late = ("EVEX" if opcode == 0x62 else "VEX") + " instruction"
        prefix_bytes = {0xC5: 1, 0xC4: 2, 0x62: 3}[opcode]
        table = 1 if opcode == 0xC5 else code[at] & (0x1F if opcode == 0xC4 else 0x07)
        at += prefix_bytes
        second = code[at]
        at += 1
        if not (table == 1 and second == 0x77):  # VZEROUPPER and VZEROALL have no ModRM byte
            at += modrm_length(code, at)[0]
        if table == 3 or (table == 1 and second in IMM8_2):
            at += 1
        return finish(ins.late)
    if opcode == 0x8F and code[at] & 0x1F >= 8:
        ins.late = "XOP instruction"
        return finish(ins.late)
    if opcode == 0x0F:
        second = code[at]
        at += 1
        if second in (0x38, 0x3A):
            ins.late = "0F %02X %02X (SSSE3, SSE4, AES, MOVBE, CRC32 or ADX)" % (second, code[at])
            at += 1 + modrm_length(code, at + 1)[0] + (1 if second == 0x3A else 0)
            return finish(ins.late)
        if second == 0x0F:
            ins.late = "3DNow! instruction"
            return finish(ins.late)
        if (mandatory, second) in SSE3:
            ins.late = "SSE3 instruction 0F %02X" % second
        if mandatory == 0xF3 and second in (0xB8, 0xBC, 0xBD):
            ins.late = {0xB8: "POPCNT", 0xBC: "TZCNT", 0xBD: "LZCNT"}[second]
        if second in NO_MODRM_2:
            if 0x80 <= second <= 0x8F:
                at += 4
                ins.branch = "jcc"
                ins.target = address + (at - start) + struct.unpack_from("<i", code, at - 4)[0]
            if second == 0x0B:
                ins.branch = "stop"
            return finish({0x05: "syscall", 0x0B: "ud2", 0xA2: "cpuid"}.get(second, "0F %02X" % second))
        modrm = code[at]
        length, _ = modrm_length(code, at)
        at += length
        if second == 0x01 and modrm in (0xC8, 0xC9):
            ins.late = "SSE3 instruction MONITOR or MWAIT"
        if second == 0xC7 and (modrm >> 3) & 7 == 1 and rex & 8:
            ins.late = "CMPXCHG16B"
        if second in IMM8_2:
            at += 1
        text = "xgetbv" if second == 0x01 and modrm == 0xD0 else "0F %02X" % second
        return finish(ins.late or text)
    if opcode in INVALID_1:
        raise EntryError("invalid instruction byte %02X at %x: the walk left the code" % (opcode, address))
    modrm = None
    if opcode in MODRM_1:
        modrm = code[at]
        length, ins.rip_slot = modrm_length(code, at)
        at += length
    ins.modrm = modrm
    if opcode in IMM8_1 or opcode in REL8:
        at += 1
    elif opcode in IMMZ_1:
        at += 2 if operand16 else 4
    elif 0xB8 <= opcode <= 0xBF:
        at += 8 if rex & 8 else (2 if operand16 else 4)
    elif opcode in (0xE8, 0xE9):
        at += 4
    elif opcode in (0xC2, 0xCA):
        at += 2
    elif opcode == 0xC8:
        at += 3
    elif 0xA0 <= opcode <= 0xA3:
        at += 4 if 0x67 in prefixes else 8
    elif opcode in (0xF6, 0xF7) and (modrm >> 3) & 7 in (0, 1):
        at += 1 if opcode == 0xF6 else (2 if operand16 else 4)
    end = address + (at - start)
    if opcode in REL8:
        ins.target = end + struct.unpack_from("<b", code, at - 1)[0]
        ins.branch = "jmp" if opcode == 0xEB else "jcc"
    elif opcode in (0xE8, 0xE9):
        ins.target = end + struct.unpack_from("<i", code, at - 4)[0]
        ins.branch = "call" if opcode == 0xE8 else "jmp"
    elif opcode in (0xC3, 0xC2):
        ins.branch = "ret"
    elif opcode in (0xCC, 0xF4):
        ins.branch = "stop"
    elif opcode == 0xFF and (modrm >> 3) & 7 in (2, 3):
        ins.branch = "call indirect"
    elif opcode == 0xFF and (modrm >> 3) & 7 in (4, 5):
        ins.branch = "jmp indirect"
    text = {0x84: "test", 0xC3: "ret", 0xCC: "int3", 0xE8: "call", 0xE9: "jmp", 0xEB: "jmp"}.get(opcode, "%02X" % opcode)
    return finish(text)


# --- Images and maps -----------------------------------------------------------------------------------------------

class Image:
    def __init__(self, data):
        self.segments = []  # (address, bytes) of the executable parts
        if data[:4] == b"\x7fELF":
            self.entry, phoff = struct.unpack_from("<QQ", data, 0x18)
            phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
            for index in range(phnum):
                kind, flags, offset, vaddr, _, filesz = struct.unpack_from("<IIQQQQ", data, phoff + index * phentsize)
                if kind == 1 and flags & 1:  # PT_LOAD, PF_X
                    self.segments.append((vaddr, data[offset:offset + filesz]))
        elif data[:2] == b"MZ":
            pe = struct.unpack_from("<I", data, 0x3C)[0]
            count, optional = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
            base = struct.unpack_from("<Q", data, pe + 48)[0]
            self.entry = base + struct.unpack_from("<I", data, pe + 40)[0]
            table = pe + 24 + optional
            for index in range(count):
                vsize, rva, rawsize, rawptr = struct.unpack_from("<IIII", data, table + 40 * index + 8)
                if struct.unpack_from("<I", data, table + 40 * index + 36)[0] & 0x20000000:  # executable
                    self.segments.append((base + rva, data[rawptr:rawptr + min(vsize, rawsize)]))
        else:
            raise EntryError("not a PE or ELF image")

    def code(self, address):
        for start, code in self.segments:
            if start <= address < start + len(code):
                return code, address - start
        raise EntryError("address %x is outside the image's code" % address)


def read_map(text):
    """Function addresses by name from an lld-link /MAP or ld.lld -Map file. ld.lld's map gives each input section a
    line (`VMA LMA size align file:(.text.<mangled name>)`) and each symbol in it a line with its demangled name."""
    symbols = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 3 and re.match(r"^000[0-9a-f]:[0-9a-f]{8}$", parts[0]):  # lld-link: section:offset name VA
            if re.match(r"^[0-9a-f]{16}$", parts[2]):
                symbols.setdefault(parts[1], int(parts[2], 16))
            continue
        match = re.match(r"^\s*([0-9a-f]+)\s+[0-9a-f]+\s+[0-9a-f]+\s+\d+\s+(\S.*?)\s*$", line)
        if match:
            address, name = int(match.group(1), 16), match.group(2)
            section = re.search(r":\(\.text\.(\S+)\)$", name)
            symbols.setdefault(section.group(1) if section else name, address)
    return symbols


# --- The walk ------------------------------------------------------------------------------------------------------

class Walk:
    def __init__(self, image, check):
        self.image, self.check = image, check
        self.functions = {}  # entry -> (returns, reaches the check)
        self.stops = set()
        self.late = []  # (address, text)
        self.reached = 0
        self.decided = False

    def function(self, entry):
        if entry in self.functions:
            return self.functions[entry]
        self.functions[entry] = (True, False)  # a recursive call: assume it returns
        returns, reaches = False, False
        seen, work = set(), [entry]
        while work:
            address = work.pop()
            while address not in seen and address not in self.stops:
                seen.add(address)
                code, at = self.image.code(address)
                ins = decode(code, at, address)
                self.reached += 1
                if ins.late:
                    self.late.append((address, ins.late))
                    if ins.late.startswith("XOP"):
                        break  # its length is not decoded
                after = address + ins.length
                if ins.branch == "call":
                    if ins.target == self.check:
                        self.function(self.check)  # the check's own code runs before it decides
                        work.append(self.decision(after))
                        reaches = True
                        break
                    callee_returns, callee_reaches = self.function(ins.target)
                    if callee_reaches:
                        reaches = True
                        break  # the code after this call runs after the check
                    if not callee_returns:
                        break
                elif ins.branch == "jmp":
                    address = ins.target
                    continue
                elif ins.branch == "jcc":
                    work.append(ins.target)
                elif ins.branch == "ret":
                    returns = True
                    break
                elif ins.branch == "jmp indirect":
                    if not ins.rip_slot:
                        raise EntryError("indirect jump at %x: the walk cannot follow it" % address)
                    returns = True  # an import thunk or PLT entry: the system's function returns to the caller
                    break
                elif ins.branch == "stop":
                    break
                address = after
        self.functions[entry] = (returns, reaches)
        return returns, reaches

    def decision(self, after):
        """The refusal branch after `call processor_too_old`; the supported branch becomes a stop."""
        code, at = self.image.code(after)
        test = decode(code, at, after)
        branch = decode(code, at + test.length, after + test.length) if test.text == "test" else None
        if test.text != "test" or test.modrm != 0xC0 or branch is None or branch.branch != "jcc" or \
                code[at + test.length] not in (0x74, 0x75, 0x0F) or (code[at + test.length] == 0x0F and
                                                                      code[at + test.length + 1] not in (0x84, 0x85)):
            raise EntryError("the result of processor_too_old at %x is not tested with test al, al and je or jne"
                             % after)
        taken_when_zero = code[at + test.length] == 0x74 or code[at + test.length:at + test.length + 2] == b"\x0f\x84"
        fall = after + test.length + branch.length
        supported, refused = (branch.target, fall) if taken_when_zero else (fall, branch.target)
        self.stops.add(supported)
        self.decided = True
        return refused


def check_image(data, map_text):
    """Walks IMAGE from its entry; returns a summary or raises EntryError."""
    image = Image(data)
    symbols = read_map(map_text)
    check = next((symbols[name] for name in CHECK_SYMBOLS if name in symbols), None)
    if check is None:
        raise EntryError("the map names no processor_too_old")
    walk = Walk(image, check)
    walk.function(image.entry)
    if walk.late:
        names = sorted((address, name) for name, address in symbols.items())
        address, text = min(walk.late)
        owner = max((entry for entry in names if entry[0] <= address), default=(0, "?"))
        raise EntryError("%s at %x (%s+%x) runs before the processor check decides%s" % (
            text, address, owner[1], address - owner[0],
            " (and %d more)" % (len(walk.late) - 1) if len(walk.late) > 1 else ""))
    if not walk.decided:
        raise EntryError("the walk from the entry point never calls processor_too_old")
    return "%d first x86-64 instructions before the processor check decides" % walk.reached


# --- Self-test -----------------------------------------------------------------------------------------------------

def self_test():
    """Hand-assembled entries, each wrapped in a minimal PE and ELF image with a map in the linker's format. The code
    is mapped at 0x1000 (the entry) and the check function at 0x1100."""
    check = bytes.fromhex("53 31c0 31c9 0fa2 5b 31c0 c3".replace(" ", ""))  # push rbx; cpuid; xor eax, eax; ret

    def image(entry_code, extra=b""):
        code = bytearray(b"\xcc" * 0x200)
        code[:len(entry_code)] = entry_code
        code[0x100:0x100 + len(check)] = check
        code[0x180:0x180 + len(extra)] = extra
        return bytes(code)

    def call(at, target):
        return b"\xe8" + struct.pack("<i", target - (at + 5))

    refuse = b"\x31\xc9\xff\x15\x00\x00\x00\x00\xcc"  # xor ecx, ecx; call [rip+0]; int3
    plain = call(0, 0x100) + b"\x84\xc0\x74\x09" + refuse + b"\x0f\xbc\xc0\xc4\xe2\x78\xf2\xc0\xc3"
    cases = [
        ("plain entry; BSF and VEX after the check", True, plain, b""),
        ("TZCNT before the check", False, b"\xf3\x48\x0f\xbc\xc0" + call(5, 0x100) + b"\x84\xc0\x74\x00\xc3", b""),
        ("VEX in the refusal branch", False,
         call(0, 0x100) + b"\x84\xc0\x75\x01\xc3\xc5\xf8\x77\xc3", b""),
        ("MOVBE in a function called before the check", False,
         call(0, 0x180) + call(5, 0x100) + b"\x84\xc0\x74\x00\xc3", b"\x0f\x38\xf0\x07\xc3"),
        ("a plain helper before the check, the supported branch with jne", True,
         call(0, 0x180) + call(5, 0x100) + b"\x84\xc0\x75\x03\xc5\xf8\x77\xc3", b"\x48\x0f\xbc\xc0\xc3"),
        ("the result is not tested", False, call(0, 0x100) + b"\xc3", b""),
        ("a jump table before the check", False, b"\xff\xe0", b""),
        ("an import thunk, then the check", True,
         call(0, 0x180) + call(5, 0x100) + b"\x84\xc0\x74\x00\xc3", b"\xff\x25\x00\x00\x00\x00"),
        ("code after a callee that reaches the check is not walked", True,
         call(0, 0x180) + b"\xc5\xf8\x77\xc3", call(0x180, 0x100) + b"\x84\xc0\x74\x00\xc3"),
    ]

    def pe(code):
        header = bytearray(0x200)
        header[:2] = b"MZ"
        struct.pack_into("<I", header, 0x3C, 0x40)
        header[0x40:0x44] = b"PE\0\0"
        struct.pack_into("<HH", header, 0x44, 0x8664, 1)
        struct.pack_into("<H", header, 0x54, 0xF0)
        struct.pack_into("<HIQ", header, 0x58, 0x20B, 0, 0)
        struct.pack_into("<I", header, 0x68, 0x1000)  # AddressOfEntryPoint
        struct.pack_into("<Q", header, 0x70, 0x140000000)  # ImageBase
        struct.pack_into("<8sIIII", header, 0x148, b".text", len(code), 0x1000, len(code), 0x200)
        struct.pack_into("<I", header, 0x148 + 36, 0x60000020)
        mapped = (" 0001:00000000       entry                      0000000140001000     a.obj\n"
                  " 0001:00000100       %s 0000000140001100     a.obj\n" % CHECK_SYMBOLS[0])
        return bytes(header) + code, mapped

    def elf(code):
        header = bytearray(0x200)
        header[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
        struct.pack_into("<HHIQQQIHHHHHH", header, 16, 3, 0x3E, 1, 0x1000, 64, 0, 0, 64, 56, 1, 0, 0, 0)
        struct.pack_into("<IIQQQQQQ", header, 64, 1, 5, 0x200, 0x1000, 0x1000, len(code), len(code), 0x1000)
        # As ld.lld writes it: an input section line with the mangled name, a symbol line with the demangled one.
        mapped = ("             VMA              LMA     Size Align Out     In      Symbol\n"
                  "            1000             1000      200    16 .text\n"
                  "            1000             1000      100     1         MaiCord.lto.o:(.text.maicord_entry)\n"
                  "            1000             1000      100     1                 maicord_entry\n"
                  "            1100             1100        a     1         MaiCord.lto.o:(.text.%s)\n"
                  "            1100             1100        a     1                 %s\n"
                  "            1180             1180       10     1         MaiCord.lto.o:(.text._ZN7maicord9sys_"
                  "writeEiPKvy)\n"
                  "            1180             1180       10     1                 maicord::sys_write(int, void "
                  "const*, unsigned long)\n" % CHECK_SYMBOLS[1:])
        return bytes(header) + code, mapped

    failures = 0
    # Each ld.lld line alone names the check.
    for line in (" ea9dc  ea9dc  77  1   MaiCord.lto.o:(.text._ZN7maicord17processor_too_oldEv)",
                 " ea9dc  ea9dc  77  1           maicord::processor_too_old()"):
        if not any(read_map(line).get(name) == 0xEA9DC for name in CHECK_SYMBOLS):
            failures += 1
            print("FAIL self-test: the ld.lld map line %r" % line.strip())
    for name, expect_pass, entry_code, extra in cases:
        for form in (pe, elf):
            data, mapped = form(image(entry_code, extra))
            try:
                check_image(data, mapped)
                passed = True
            except EntryError:
                passed = False
            if passed != expect_pass:
                failures += 1
                print("FAIL self-test %s (%s): expected %s" % (name, form.__name__, "pass" if expect_pass else
                                                               "failure"))
    if failures:
        return 1
    print("PASS check_processor_entry self-test (%d cases, PE and ELF)" % len(cases))
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        with open(argv[0], "rb") as image, open(argv[1], encoding="latin-1") as map_file:
            print("%s: %s" % (argv[0], check_image(image.read(), map_file.read())))
    except (EntryError, OSError, struct.error, IndexError) as error:
        print("check_processor_entry: %s: %s" % (argv[0], error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
