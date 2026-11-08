"""The plugin half of maicord-plugin: names, the SDK and dev folders, the toolchain, projects, the PE and ELF
and descriptor checks, and the new, build, check, dev and pack commands. maicord_plugin.py is the command line.

Python 3.9 or newer, standard library only. A Windows DLL is built with the pinned clang-cl and lld-link of
tools/llvm_toolchain.json (fetched once into %LOCALAPPDATA%\\MaiCord-toolchain, as tools/build.ps1 does), or the
pair found on PATH; a Linux shared object with clang and ld.lld (on PATH, or the pinned release on Windows).
Plugins compile against MaiCord's own SDK headers only (no Windows SDK, Visual Studio or libc): the repository's
include/ directory, or the include/ of an SDK folder.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import secrets
import shutil
import struct
import subprocess
import time
import urllib.request
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
ABI = 6
EXPORT_NAME = b"maicord_plugin_v6"
SLUG_MAX = 40
HOSTS_MAX = 8
# MC_PERM_* in bit order (include/maicord/sdk/maicord_plugin.h), as the catalog names them.
PERMISSIONS = ("message_send", "read_messages", "message_history", "read_activity", "network", "composer",
               "act_messages", "act_presence", "clipboard", "automated_actions", "persistent_log")
HOOK_LAST = 17
CATEGORIES = ("chat", "appearance", "notifications", "privacy", "utility", "voice", "accessibility", "moderation",
              "developer", "fun")
VERSION = re.compile(r"(0|[1-9][0-9]{0,2})(\.(0|[1-9][0-9]{0,2})){2}")  # MAJOR.MINOR.PATCH, each 0 to 999

# The flags of cmake's maicord_plugin() and of the catalog's CI (MaiCord-plugins scripts/build_catalog.py), so a
# local build is the catalog's build: same source, same compiler, same bytes (/Brepro).
# No C library in a plugin: -fno-builtin-strlen/wcslen keep LLVM from turning string-length loops into imports.
COMPILE = ["/nologo", "/c", "/std:c++latest", "/clang:-nostdinc", "/O2", "/W4", "/WX", "/utf-8", "/Zl", "/GS-", "/GR-",
           "/permissive-", "/clang:-fno-asynchronous-unwind-tables", "/clang:-fno-unwind-tables",
           "/clang:-fno-builtin-strlen", "/clang:-fno-builtin-wcslen", "/DMC_RUNTIME"]
LINK = ["/nologo", "/DLL", "/NOENTRY", "/NODEFAULTLIB", "/SUBSYSTEM:WINDOWS,6.02", "/DYNAMICBASE", "/HIGHENTROPYVA",
        "/NXCOMPAT", "/CETCOMPAT", "/OPT:REF", "/OPT:ICF=8", "/INCREMENTAL:NO", "/ALLOWBIND:NO",
        "/MERGE:.pdata=.rdata", "/DEBUG:NONE", "/Brepro", "/mllvm:-enable-machine-outliner"]
# The Linux recipe of cmake's maicord_plugin() and of the catalog's Linux build: a freestanding shared object (the SDK
# headers only, no libc), hidden symbols, relative relocations only (-fPIC with -Bsymbolic), a non-executable stack and
# no build id, so a local build is the catalog's build: same source, same compiler, same bytes. A section per function
# and per object lets --gc-sections drop what nothing reaches (MC_RUNTIME's unused memmove and memcmp too), and
# --strip-all leaves the symbol table out, as the DLL has none: the loader reads only the dynamic one.
LINUX_TRIPLE = "x86_64-unknown-linux-gnu"
LINUX_COMPILE = ["-std=c++23", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-nostdinc",
                 "-fPIC", "-fvisibility=hidden", "-fno-stack-protector", "-fcf-protection=none",
                 "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fno-exceptions", "-fno-rtti",
                 "-fno-threadsafe-statics", "-ffunction-sections", "-fdata-sections", "-fno-builtin-strlen",
                 "-fno-builtin-wcslen", "-DMC_RUNTIME"]
LINUX_LINK = ["-fuse-ld=lld", "-nostdlib", "-shared", "-Wl,-Bsymbolic", "-Wl,-z,noexecstack", "-Wl,-z,now",
              "-Wl,--hash-style=gnu", "-Wl,--build-id=none", "-Wl,--gc-sections", "-Wl,--strip-all"]
TARGETS = ("windows", "linux")


def host_target() -> str:
    return "windows" if os.name == "nt" else "linux"


def target_of(arguments) -> str:
    """The --target of a command, else the system the tool runs on."""
    return getattr(arguments, "target", None) or host_target()


def target_option(target: str) -> str:
    """What to add to a command line to ask for target when it is not the default."""
    return "" if target == host_target() else f" --target {target}"


def binary_kind(target: str) -> str:
    return "DLL" if target == "windows" else "shared object"


class ToolError(Exception):
    """A problem to report in one line, without a traceback."""


def say(text: str = "") -> None:
    print(text, flush=True)


def write_text(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")


# ---- names -----------------------------------------------------------------------------------------
def slug_valid(text: str) -> bool:
    return bool(re.fullmatch(r"[a-z0-9][a-z0-9-]*", text)) and len(text) <= SLUG_MAX


def slugify(name: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")[:SLUG_MAX].strip("-")
    if not slug_valid(slug):
        raise ToolError(f"cannot make a catalog slug (lower-case letters, digits and '-') from {name!r}")
    return slug


def display_name(name: str) -> str:
    """What the user typed when it has a space or capital, else the slug in title case ('hello' -> 'Hello')."""
    text = name.strip()
    if not text or len(text.encode("utf-8")) > 64 or any(ord(c) < 32 or c in '"\\' for c in text):
        raise ToolError("the name must be 1 to 64 bytes without quotes, backslashes or control characters")
    return text if re.search(r"[\sA-Z]", text) else " ".join(word.capitalize() for word in re.split(r"[-_]+", text))


# ---- the SDK and the folders MaiCord reads ----------------------------------------------------------
def sdk_include(explicit: str | None = None) -> Path:
    """The include/ directory that holds maicord/sdk/plugin.hpp and maicord/sys/libc/stdint.h: the named one (an
    error when it is not an SDK), else MAICORD_SDK, else this repository's or the SDK folder this tool sits in."""
    def valid(path: Path) -> Path | None:
        for root in (path, path / "include"):
            if (root / "maicord" / "sdk" / "plugin.hpp").is_file() and (root / "maicord" / "sys" / "libc" / "stdint.h").is_file():
                return root.resolve()
        return None

    named = explicit or os.environ.get("MAICORD_SDK")
    if named:
        found = valid(Path(named))
        if not found:
            raise ToolError(f"{named} has no maicord/sdk/plugin.hpp (use --sdk <a folder with include\\maicord\\sdk>)")
        return found
    for base in (HERE.parent.parent, HERE):  # the repository (tools/maicord-plugin), an SDK folder
        found = valid(base)
        if found:
            return found
    raise ToolError("the MaiCord SDK headers were not found (use --sdk <a folder with include\\maicord\\sdk> or set MAICORD_SDK)")


def local_app_data() -> Path:
    base = os.environ.get("LOCALAPPDATA")
    if not base:
        raise ToolError("LOCALAPPDATA is not set")
    return Path(base)


def data_home() -> Path:
    """MaiCord's data folder on this system: %LOCALAPPDATA%\\MaiCord on Windows, $XDG_DATA_HOME/maicord (else
    ~/.local/share/maicord) on Linux."""
    if os.name == "nt":
        return local_app_data() / "MaiCord"
    base = os.environ.get("XDG_DATA_HOME")
    return Path(base if base and os.path.isabs(base) else Path.home() / ".local" / "share") / "maicord"


def dev_folder(kind: str, explicit: str | None = None) -> Path:
    """Where Developer Mode watches: plugins\\dev or effects\\dev under the data folder, or the override."""
    if explicit:
        return Path(explicit)
    variable = "MAICORD_PLUGIN_DEV" if kind == "plugin" else "MAICORD_EFFECT_DEV"
    if os.environ.get(variable):
        return Path(os.environ[variable])
    return data_home() / ("plugins" if kind == "plugin" else "effects") / "dev"


# ---- the toolchain ----------------------------------------------------------------------------------
@dataclass
class Toolchain:
    clang: Path
    linker: Path
    origin: str


def _pin() -> dict:
    for candidate in (HERE.parent / "llvm_toolchain.json", HERE / "llvm_toolchain.json"):
        if candidate.is_file():
            return json.loads(candidate.read_text(encoding="utf-8"))
    return {}


def _bin_toolchain(directory: Path, origin: str) -> Toolchain | None:
    clang, linker = directory / "clang-cl.exe", directory / "lld-link.exe"
    return Toolchain(clang, linker, origin) if clang.is_file() and linker.is_file() else None


def fetch_toolchain(pin: dict, root: Path, url: str | None = None) -> Path:
    """Downloads the pinned LLVM release once, checks its SHA-256 and unpacks it to root. Returns root\\bin."""
    if not re.fullmatch(r"[0-9a-fA-F]{64}", pin.get("sha256", "")):
        raise ToolError("the toolchain pin has no valid sha256")
    address = url or f"https://github.com/{pin['repository']}/releases/download/{pin['tag']}/{pin['asset']}"
    staging = Path(str(root) + ".download")
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    try:
        say(f"Fetching the pinned LLVM toolchain {pin['tag']}...")
        archive = staging / "toolchain.zip"
        with urllib.request.urlopen(address, timeout=120) as response, open(archive, "wb") as output:
            shutil.copyfileobj(response, output)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != pin["sha256"].lower():
            raise ToolError(f"the download hashes to {digest}, not the pinned {pin['sha256'].lower()}")
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(staging / "toolchain")
        if not _bin_toolchain(staging / "toolchain" / "bin", "pin"):
            raise ToolError("the download has no clang-cl.exe and lld-link.exe in bin")
        shutil.rmtree(root, ignore_errors=True)
        (staging / "toolchain").replace(root)
    except OSError as error:
        raise ToolError(f"could not fetch the toolchain: {error}") from None
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return root / "bin"


def find_toolchain(explicit: str | None = None, fetch: bool = True) -> Toolchain:
    """--llvm-bin, MAICORD_LLVM_BIN, the pinned release in %LOCALAPPDATA%\\MaiCord-toolchain, clang-cl and
    lld-link on PATH, then (once) the pinned release downloaded."""
    for value, origin in ((explicit, "--llvm-bin"), (os.environ.get("MAICORD_LLVM_BIN"), "MAICORD_LLVM_BIN")):
        if value:
            found = _bin_toolchain(Path(value), origin)
            if not found:
                raise ToolError(f"{origin} {value} has no clang-cl.exe and lld-link.exe")
            return found
    pin = _pin()
    root = None
    if pin.get("tag") and os.environ.get("LOCALAPPDATA"):
        root = local_app_data() / "MaiCord-toolchain" / pin["tag"]
        found = _bin_toolchain(root / "bin", "the pinned toolchain")
        if found:
            return found
    clang, linker = shutil.which("clang-cl"), shutil.which("lld-link")
    if clang and linker:
        return Toolchain(Path(clang), Path(linker), "PATH")
    if fetch and root and pin.get("repository"):
        found = _bin_toolchain(fetch_toolchain(pin, root), "the pinned toolchain")
        if found:
            return found
    raise ToolError("clang-cl and lld-link were not found (use --llvm-bin, MAICORD_LLVM_BIN, or put them on PATH)")


def _linux_toolchain(directory: Path | None, origin: str) -> Toolchain | None:
    search = str(directory) if directory else None
    clang = next((found for name in ("clang++", "clang") if (found := shutil.which(name, path=search))), None)
    linker = shutil.which("ld.lld", path=search)
    return Toolchain(Path(clang), Path(linker), origin) if clang and linker else None


def find_linux_toolchain(explicit: str | None = None) -> Toolchain:
    """The clang and ld.lld that build a Linux shared object: --llvm-bin, MAICORD_LLVM_BIN, then PATH. A plugin includes
    only the SDK headers and links no library, so a plain LLVM install will do, on Windows too (the pinned release
    holds only clang-cl and lld-link, which build the DLL)."""
    for value, origin in ((explicit, "--llvm-bin"), (os.environ.get("MAICORD_LLVM_BIN"), "MAICORD_LLVM_BIN")):
        if value:
            found = _linux_toolchain(Path(value), origin)
            if not found:
                raise ToolError(f"{origin} {value} has no clang and ld.lld")
            return found
    found = _linux_toolchain(None, "PATH")
    if not found:
        raise ToolError("clang and ld.lld were not found (use --llvm-bin, MAICORD_LLVM_BIN, or put them on PATH)")
    return found


def find_target_toolchain(target: str, explicit: str | None = None, fetch: bool = True) -> Toolchain:
    return find_linux_toolchain(explicit) if target == "linux" else find_toolchain(explicit, fetch)


# ---- projects ------------------------------------------------------------------------------------------
@dataclass
class Project:
    directory: Path
    source: Path
    slug: str

    @property
    def build_directory(self) -> Path:
        return self.directory / "build"

    @property
    def dll(self) -> Path:
        return self.build_directory / f"{self.slug}.dll"

    @property
    def shared_object(self) -> Path:
        return self.build_directory / f"{self.slug}.so"

    def output(self, target: str) -> Path:
        """What a build for target leaves: the DLL (windows) or the shared object (linux)."""
        return self.dll if target == "windows" else self.shared_object


def folder_slug(directory: Path) -> str:
    return re.sub(r"[^a-z0-9-]+", "-", directory.name.lower()).strip("-")[:SLUG_MAX].strip("-")


def open_project(directory: str | None, source: str | None = None) -> Project:
    root = Path(directory or ".").resolve()
    file = Path(source) if source else Path("plugin.cpp")
    file = file if file.is_absolute() else root / file
    if not file.is_file():
        raise ToolError(f"{file} was not found (run this in a plugin folder, or use --dir; 'maicord-plugin new <name>' makes one)")
    slug = folder_slug(root)
    if not slug_valid(slug):
        raise ToolError(f"the folder name {root.name!r} makes no catalog slug; rename the folder (lower-case letters, digits, '-')")
    return Project(root, file, slug)


def sources(project: Project) -> list[Path]:
    """Every file whose change calls for a rebuild: the project's C++ files and headers, not build/ or dist/."""
    skipped = (project.build_directory, project.directory / "dist")
    found = []
    for pattern in ("*.cpp", "*.hpp", "*.h", "*.inc"):
        found += [p for p in project.directory.rglob(pattern) if not any(s in p.parents for s in skipped)]
    return found


def is_stale(project: Project, target: str | None = None) -> bool:
    binary = project.output(target or host_target())
    if not binary.is_file():
        return True
    built = binary.stat().st_mtime_ns
    return any(p.stat().st_mtime_ns > built for p in sources(project))


def run(command: list, label: str, verbose: bool) -> None:
    if verbose:
        say(" ".join(str(part) for part in command))
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, errors="replace")
    if result.returncode:
        output = (result.stdout + result.stderr).strip()
        raise ToolError(f"{label} failed:\n{output}")
    if verbose and (result.stdout or result.stderr):
        say((result.stdout + result.stderr).strip())


def build_project(project: Project, toolchain: Toolchain, include: Path, verbose: bool = False,
                  target: str | None = None) -> Path:
    """Compiles and links plugin.cpp into build/<slug>.dll (Windows) or build/<slug>.so (Linux), then returns the file;
    the caller runs the checks on it."""
    target = target or host_target()
    project.build_directory.mkdir(exist_ok=True)
    if target == "linux":
        obj, shared_object = project.build_directory / f"{project.slug}.o", project.shared_object
        shared_object.unlink(missing_ok=True)  # a failed link must not leave the last good build looking current
        driver = [toolchain.clang, "--driver-mode=g++", f"--target={LINUX_TRIPLE}"]
        run([*driver, *LINUX_COMPILE, f"-I{include}", f"-I{include / 'maicord' / 'sys' / 'libc'}", "-c", project.source,
             "-o", obj], "the compile", verbose)
        run([*driver, *LINUX_LINK, obj, "-o", shared_object], "the link", verbose)
        obj.unlink(missing_ok=True)
        return shared_object
    obj = project.build_directory / f"{project.slug}.obj"
    dll = project.dll
    dll.unlink(missing_ok=True)  # a failed link must not leave the last good build looking current
    run([toolchain.clang, *COMPILE, f"/I{include}", f"/I{include / 'maicord' / 'sys' / 'libc'}", project.source,
         f"/Fo{obj}"], "the compile", verbose)
    # The export directory names the file, so the output name is the slug and a rebuild is byte for byte the same.
    run([toolchain.linker, *LINK, obj, f"/OUT:{dll}"], "the link", verbose)
    obj.unlink(missing_ok=True)
    for extra in (dll.with_suffix(".lib"), dll.with_suffix(".exp")):
        extra.unlink(missing_ok=True)
    return dll


# ---- the PE and the descriptor ---------------------------------------------------------------------------
@dataclass
class Inspection:
    errors: list = field(default_factory=list)
    warnings: list = field(default_factory=list)
    info: dict = field(default_factory=dict)
    format: str = "PE"  # "ELF" for a Linux shared object

    @property
    def ok(self) -> bool:
        return not self.errors


class _Image:
    def __init__(self, data: bytes):
        self.data = data
        if len(data) < 64 or data[:2] != b"MZ":
            raise ToolError("not a PE file")
        self.nt = struct.unpack_from("<I", data, 0x3C)[0]
        if self.nt < 64 or self.nt + 24 + 112 > len(data) or data[self.nt:self.nt + 4] != b"PE\0\0":
            raise ToolError("not a PE file")
        machine, count, _, _, _, optional_size, characteristics = struct.unpack_from("<HHIIIHH", data, self.nt + 4)
        self.opt = self.nt + 24
        if machine != 0x8664 or struct.unpack_from("<H", data, self.opt)[0] != 0x20B:
            raise ToolError("not an x64 PE32+ image")
        if not characteristics & 0x2000 or not characteristics & 0x0002:
            raise ToolError("not a DLL")
        if optional_size < 112 or self.opt + optional_size > len(data):
            raise ToolError("the optional header is cut off")
        self.entry = struct.unpack_from("<I", data, self.opt + 16)[0]
        self.base = struct.unpack_from("<Q", data, self.opt + 24)[0]
        self.section_alignment, self.file_alignment = struct.unpack_from("<II", data, self.opt + 32)
        self.image_bytes, self.headers_bytes = struct.unpack_from("<II", data, self.opt + 56)
        directory_count = struct.unpack_from("<I", data, self.opt + 108)[0]
        if directory_count > 16 or optional_size < 112 + 8 * directory_count:
            raise ToolError("the data directory table is malformed")
        self.directories = [struct.unpack_from("<II", data, self.opt + 112 + 8 * i) for i in range(directory_count)]
        table = self.opt + optional_size
        if not 0 < count <= 16 or table + 40 * count > len(data):
            raise ToolError("the section table is malformed")
        self.sections = []  # (name, rva, virtual bytes, raw offset, raw bytes, flags)
        for i in range(count):
            at = table + 40 * i
            name = data[at:at + 8].rstrip(b"\0").decode("ascii", "replace")
            virtual, rva, raw_bytes, raw = struct.unpack_from("<IIII", data, at + 8)
            flags = struct.unpack_from("<I", data, at + 36)[0]
            self.sections.append((name, rva, virtual or raw_bytes, raw, raw_bytes, flags))

    def directory(self, index: int) -> tuple:
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def offset(self, rva: int, size: int = 1) -> int:
        for _, start, virtual, raw, raw_bytes, _ in self.sections:
            if start <= rva and rva + size <= start + min(virtual, raw_bytes):
                return raw + rva - start
        raise ToolError(f"RVA {rva:#x} is not backed by the file")

    def read(self, rva: int, size: int) -> bytes:
        at = self.offset(rva, size)
        return self.data[at:at + size]

    def pointer(self, value: int, size: int) -> bytes:
        if value < self.base or value - self.base >= self.image_bytes:
            raise ToolError("a descriptor pointer leaves the image")
        return self.read(value - self.base, size)

    def executable(self, value: int) -> bool:
        rva = value - self.base
        return any(flags & 0x20000000 and start <= rva < start + virtual for _, start, virtual, _, _, flags in self.sections)

    def text(self, raw: bytes, low: int, high: int, what: str) -> str:
        pointer, size, reserved = struct.unpack("<QII", raw)
        if reserved or size < low or size > high:
            raise ToolError(f"{what} is {size} bytes (allowed {low} to {high}) or has reserved bits")
        if not size:
            return ""
        try:
            value = self.pointer(pointer, size).decode("utf-8")
        except UnicodeDecodeError:
            raise ToolError(f"{what} is not UTF-8") from None
        if any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise ToolError(f"{what} has control characters")
        return value


def _page_up(value: int) -> int:
    return (value + 4095) & ~4095


class _ElfImage(_Image):
    """A Linux shared object as the loader sees it once mapped: its load segments at their virtual addresses, and (after
    _check_elf has applied the relative relocations) pointers that read as addresses, so the base is 0."""

    def __init__(self, data: bytes):
        self.data = data
        if len(data) < 64 or data[:4] != b"\x7fELF":
            raise ToolError("not an ELF file")
        if data[4] != 2 or data[5] != 1 or data[6] != 1:
            raise ToolError("not a 64-bit little-endian ELF file")
        kind, machine, version, entry, table, _, _, header_bytes, entry_bytes, count = struct.unpack_from("<HHIQQQIHHH", data, 16)
        if kind != 3 or version != 1 or header_bytes != 64:
            raise ToolError("not a shared object")
        if machine != 62:
            raise ToolError("not an x86-64 image")
        if entry:
            raise ToolError("the shared object has an entry point: link with -nostdlib and without -e")
        if entry_bytes != 56 or not 0 < count <= 32 or table < 64 or table + 56 * count > len(data):
            raise ToolError("the program header table is malformed")
        self.base = 0
        self.loads = []  # (vaddr, file offset, file bytes, memory bytes, flags)
        self.dynamic = None
        stack, end = False, 0
        for index in range(count):
            kind, flags, offset, vaddr, _, file_bytes, memory_bytes, align = struct.unpack_from("<IIQQQQQQ", data, table + 56 * index)
            if kind == 3:
                raise ToolError("the shared object names an interpreter: it is not a plugin")
            if kind == 7:
                raise ToolError("the shared object has a thread-local storage segment: a plugin must not use thread-local storage")
            if kind == 0x6474E551:  # PT_GNU_STACK
                if flags & 1:
                    raise ToolError("the stack is executable: link with -z noexecstack")
                stack = True
            elif kind == 2:
                if self.dynamic:
                    raise ToolError("the program headers have two dynamic segments")
                self.dynamic = (vaddr, offset, file_bytes)
            elif kind == 1:
                if (len(self.loads) == 16 or not memory_bytes or file_bytes > memory_bytes or offset + file_bytes > len(data) or
                        vaddr + memory_bytes > 32 << 20 or align < 4096 or align & (align - 1) or align > 0x200000 or
                        vaddr % align != offset % align or not flags & 4 or (flags & 1 and flags & 2)):
                    raise ToolError("a load segment is malformed, unreadable, or both writable and executable")
                ordered = (vaddr >= end and vaddr & ~4095 <= _page_up(end)) if self.loads else vaddr == 0
                if not ordered:
                    raise ToolError("the load segments are out of order, overlap, or leave pages unmapped between them")
                self.loads.append((vaddr, offset, file_bytes, memory_bytes, flags))
                end = vaddr + memory_bytes
        if not self.loads or not self.dynamic or not stack:
            raise ToolError("the program headers lack a load segment, the dynamic segment or PT_GNU_STACK")
        self.image_bytes = _page_up(end)
        self.flat = bytearray(self.image_bytes)
        for vaddr, offset, file_bytes, _, _ in self.loads:
            self.flat[vaddr:vaddr + file_bytes] = data[offset:offset + file_bytes]

    def backed(self, vaddr: int, size: int) -> int:
        """The file offset of [vaddr, vaddr + size) when it lies in the file-backed part of one load segment."""
        for start, offset, file_bytes, _, _ in self.loads:
            if start <= vaddr and vaddr - start <= file_bytes and size <= file_bytes - (vaddr - start):
                return offset + vaddr - start
        raise ToolError(f"address {vaddr:#x} is not backed by the file")

    def segment(self, vaddr: int, size: int):
        """The load segment that holds [vaddr, vaddr + size) in memory, or None."""
        for segment in self.loads:
            if segment[0] <= vaddr and vaddr - segment[0] <= segment[3] and size <= segment[3] - (vaddr - segment[0]):
                return segment
        return None

    def read(self, rva: int, size: int) -> bytes:
        if rva < 0 or size < 0 or rva + size > self.image_bytes:
            raise ToolError(f"address {rva:#x} is outside the image")
        return bytes(self.flat[rva:rva + size])

    def executable(self, value: int) -> bool:
        return any(flags & 1 and start <= value < start + memory for start, _, _, memory, flags in self.loads)


def _host_valid(host: str) -> bool:
    body = host[2:] if host.startswith("*.") else host
    labels = body.split(".")
    return (len(labels) >= 2 and len(host) <= 253 and all(re.fullmatch(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?", label) and
            len(label) <= 63 for label in labels)) and not re.fullmatch(r"[0-9.]+", body)


def inspect_dll(path: Path) -> Inspection:
    """The checks the client's loader runs before it maps a plugin (a PE DLL or an ELF shared object, by its first
    bytes), done on the file alone. The client repeats all of them (and more) at load: this finds the obvious problems
    before MaiCord does."""
    report = Inspection()
    try:
        data = Path(path).read_bytes()
        if data[:4] == b"\x7fELF":
            report.format = "ELF"
            _check_elf(_ElfImage(data), report)
        else:
            _check_pe(_Image(data), report)
    except ToolError as error:
        report.errors.append(str(error))
    except OSError as error:
        report.errors.append(f"cannot read {path}: {error}")
    except struct.error:
        report.errors.append("the file is cut off")
    return report


def _check_pe(image: _Image, report: Inspection) -> None:
    errors = report.errors
    if image.entry:
        errors.append("the DLL has an entry point: link with /NOENTRY so loading it runs no code")
    if any(image.directory(9)):
        errors.append("the DLL has a TLS directory: a plugin must not use thread-local storage")
    if any(image.directory(1)) or any(image.directory(13)):
        report.warnings.append("the DLL has imports: a plugin reaches MaiCord only through its mc_api table")
    if (image.section_alignment < 0x1000 or image.section_alignment & (image.section_alignment - 1) or
            image.file_alignment < 0x200 or image.image_bytes % image.section_alignment or image.image_bytes > 32 << 20):
        errors.append("the section or file alignment is not one the loader accepts")
    previous = image.headers_bytes
    for name, rva, virtual, raw, raw_bytes, flags in image.sections:
        if rva < previous or rva + virtual > image.image_bytes or raw + raw_bytes > len(image.data):
            errors.append(f"section {name} is out of bounds or overlaps")
        if flags & 0x20000000 and flags & 0x80000000:
            errors.append(f"section {name} is both writable and executable")
        previous = rva + virtual
    if errors:
        return
    # Exactly one export, by name, and it is data in a section the loader never executes.
    export_rva, export_size = image.directory(0)
    if not export_rva or export_size < 40:
        errors.append(f"the DLL exports nothing: it must export {EXPORT_NAME.decode()}")
        return
    functions, names, address_functions, address_names, address_ordinals = struct.unpack_from(
        "<IIIII", image.read(export_rva + 20, 20))
    if functions != 1 or names != 1:
        errors.append("a plugin exports exactly one named symbol")
        return
    name = image.read(struct.unpack("<I", image.read(address_names, 4))[0], len(EXPORT_NAME) + 1)
    if name != EXPORT_NAME + b"\0":
        errors.append(f"the export is {name.rstrip(chr(0).encode()).decode('ascii', 'replace')}, not {EXPORT_NAME.decode()}")
        return
    if struct.unpack("<H", image.read(address_ordinals, 2))[0] != 0:
        errors.append("the export's ordinal is not 0")
        return
    rva = struct.unpack("<I", image.read(address_functions, 4))[0]
    if export_rva <= rva < export_rva + export_size:
        errors.append("the export is forwarded")
        return
    if not any(start <= rva < start + virtual and not flags & 0x20000000 for _, start, virtual, _, _, flags in image.sections):
        errors.append("the export is code or outside every section: it must be the const descriptor")
        return
    _check_descriptor(image, rva, report)


def _check_elf(image: _ElfImage, report: Inspection) -> None:
    """What the client's loader (src/app/plugins/loader.cpp) checks of a shared object, in its order: nothing is
    needed, nothing runs at load or unload, only relative relocations, no undefined symbol and exactly one export, the
    descriptor as data. The descriptor itself is then read from the image with the relocations applied."""
    try:
        data = image.data
        vaddr, offset, size = image.dynamic
        if size < 16 or size > 4096 or size % 16 or image.backed(vaddr, size) != offset:
            raise ToolError("the dynamic segment is malformed")
        tags: dict = {}
        ended = False
        for at in range(0, size, 16):
            tag, value = struct.unpack_from("<QQ", data, offset + at)
            if tag == _DT_NULL:
                ended = True
                break
            if tag in _DT_IMPORTS:
                raise ToolError("the shared object needs another library (DT_NEEDED) or filters one: link with -nostdlib and no libraries")
            if tag in _DT_RUNS_CODE:
                raise ToolError("the shared object has an initializer or finalizer: dlopen would run it")
            if tag == _DT_TEXTREL or (tag == _DT_FLAGS and value & 4):
                raise ToolError("the shared object has text relocations: it must be position independent (-fPIC)")
            if tag in _DT_OTHER_RELOCATIONS:
                raise ToolError("the shared object has REL or RELR relocations: only RELA relative ones are accepted")
            if (tag == _DT_SYMENT and value != 24) or (tag == _DT_RELAENT and value != 24) or (tag == _DT_PLTREL and value != 7):
                raise ToolError("the symbol or relocation entry size is not the x86-64 one")
            if tag in _DT_USED:
                if tag in tags:
                    raise ToolError("a dynamic tag is repeated: which one would the loader use?")
                tags[tag] = value
        if not ended:
            raise ToolError("the dynamic section has no end")
        value = _elf_symbols(image, tags)
        _elf_relocations(image, tags.get(_DT_RELA, 0), tags.get(_DT_RELASZ, 0))
        _elf_relocations(image, tags.get(_DT_JMPREL, 0), tags.get(_DT_PLTRELSZ, 0))
    except ToolError as error:
        report.errors.append(str(error))
        return
    _check_descriptor(image, value, report)


# Dynamic tags of the loader's checks (DT_*): what the shared object may not name, what runs, and what it needs.
_DT_NULL, _DT_PLTRELSZ, _DT_HASH, _DT_STRTAB, _DT_SYMTAB, _DT_RELA, _DT_RELASZ, _DT_RELAENT = 0, 2, 4, 5, 6, 7, 8, 9
_DT_STRSZ, _DT_SYMENT, _DT_PLTREL, _DT_TEXTREL, _DT_JMPREL, _DT_FLAGS, _DT_GNU_HASH = 10, 11, 20, 22, 23, 30, 0x6FFFFEF5
_DT_IMPORTS = {1, 0x6FFFFEFB, 0x6FFFFEFC, 0x6FFFFFFE, 0x7FFFFFFD, 0x7FFFFFFF}  # NEEDED, DEPAUDIT, AUDIT, VERNEED, AUXILIARY, FILTER
_DT_RUNS_CODE = {12, 13, 25, 26, 32}                                             # INIT, FINI, INIT_ARRAY, FINI_ARRAY, PREINIT_ARRAY
_DT_OTHER_RELOCATIONS = {17, 18, 19, 35, 36, 37}                                 # REL, RELSZ, RELENT, RELRSZ, RELR, RELRENT
_DT_USED = {_DT_SYMTAB, _DT_STRTAB, _DT_STRSZ, _DT_HASH, _DT_GNU_HASH, _DT_RELA, _DT_RELASZ, _DT_JMPREL, _DT_PLTRELSZ}
_SYMBOLS_MAX = 4096


def _elf_symbol_count(image: _ElfImage, tags: dict) -> int:
    """How many entries the symbol table has, as the hash tables the loader looks symbols up with count them."""
    data = image.data
    count = 0
    if _DT_HASH in tags:
        count = struct.unpack_from("<I", data, image.backed(tags[_DT_HASH], 8) + 4)[0]
    if _DT_GNU_HASH in tags:
        offset = image.backed(tags[_DT_GNU_HASH], 16)
        buckets, first, bloom = struct.unpack_from("<III", data, offset)
        if not 0 < buckets <= _SYMBOLS_MAX or bloom > _SYMBOLS_MAX or first > _SYMBOLS_MAX:
            raise ToolError("the GNU hash table is malformed")
        image.backed(tags[_DT_GNU_HASH], 16 + 8 * bloom + 4 * buckets)
        bucket_at = offset + 16 + 8 * bloom
        last = max(struct.unpack_from(f"<{buckets}I", data, bucket_at))
        gnu = first
        if last:
            if last < first:
                raise ToolError("the GNU hash table is malformed")
            for index in range(last, _SYMBOLS_MAX):
                if struct.unpack_from("<I", data, bucket_at + 4 * buckets + 4 * (index - first))[0] & 1:
                    gnu = index + 1
                    break
            else:
                raise ToolError("the GNU hash chains are too long")
        count = max(count, gnu)
    if count > _SYMBOLS_MAX:
        raise ToolError("the symbol table is too long")
    return count


def _elf_symbols(image: _ElfImage, tags: dict) -> int:
    """No undefined, thread-local or indirect symbol; exactly one exported, named as EXPORT_NAME, as data in a segment
    that is not executable. Returns its address."""
    data = image.data
    needed = (_DT_SYMTAB, _DT_STRTAB, _DT_STRSZ)
    count = _elf_symbol_count(image, tags) if all(tag in tags for tag in needed) and (_DT_HASH in tags or _DT_GNU_HASH in tags) else 0
    if count < 2 or not tags[_DT_STRSZ]:
        raise ToolError(f"the shared object exports nothing: it must export {EXPORT_NAME.decode()}")
    symbols = image.backed(tags[_DT_SYMTAB], count * 24)
    strings = image.backed(tags[_DT_STRTAB], tags[_DT_STRSZ])
    exported, found = 0, 0
    for index in range(1, count):
        name, info, other, section, value, _ = struct.unpack_from("<IBBHQQ", data, symbols + 24 * index)
        if not section or info & 15 in (6, 10):
            raise ToolError("the shared object has an undefined, thread-local or indirect symbol: it imports something")
        if not info >> 4 or other & 3 in (1, 2):
            continue  # local, or not exported
        exported += 1
        spelled = data[strings + name:strings + name + len(EXPORT_NAME) + 1] if name < tags[_DT_STRSZ] else b""
        if exported > 1 or info & 15 != 1 or section >= 0xFF00 or spelled != EXPORT_NAME + b"\0":
            raise ToolError(f"a plugin exports exactly one symbol, {EXPORT_NAME.decode()}, as data")
        segment = image.segment(value, 232)
        if not segment or segment[4] & 1:
            raise ToolError("the export is code or outside every segment: it must be the const descriptor")
        found = value
    if exported != 1:
        raise ToolError(f"the shared object exports nothing: it must export {EXPORT_NAME.decode()}")
    return found


def _elf_relocations(image: _ElfImage, address: int, size: int) -> None:
    """Only R_X86_64_RELATIVE without a symbol, aimed at a word of a writable segment; applied to the image."""
    if not size:
        return
    if size % 24 or size > 4 << 20:
        raise ToolError("the relocation table is malformed")
    offset = image.backed(address, size)
    for at in range(0, size, 24):
        target, info, addend = struct.unpack_from("<QQq", image.data, offset + at)
        if info != 8:
            raise ToolError("the shared object imports something (a relocation that resolves a symbol) or has a kind of "
                            "relocation other than relative: build with -fPIC -fvisibility=hidden -Wl,-Bsymbolic")
        segment = image.segment(target, 8)
        if not segment or not segment[4] & 2:
            raise ToolError("a relocation is aimed outside the writable segments")
        image.flat[target:target + 8] = struct.pack("<Q", addend & (2 ** 64 - 1))


def _check_descriptor(image: _Image, rva: int, report: Inspection) -> None:
    try:
        size = struct.unpack("<I", image.read(rva, 4))[0]
        if size < 232 or size > 4096:
            raise ToolError(f"the descriptor is {size} bytes (expected 232 to 4096)")
        raw = image.read(rva, 232)
        _, abi, api_level, flags, owner = struct.unpack_from("<IIIIQ", raw, 0)
        if abi != ABI:
            raise ToolError(f"the descriptor is ABI {abi}, this tool builds ABI {ABI}")
        if flags or not owner:
            raise ToolError("the descriptor's flags must be 0 and its owner id non-zero")
        if not 1 <= api_level <= 0xFFFF:
            raise ToolError("the descriptor's api level is out of range")
        info = {"owner_id": owner, "api_level": api_level}
        info["name"] = image.text(raw[24:40], 1, 64, "the name")
        info["version"] = image.text(raw[40:56], 1, 32, "the version")
        info["summary"] = image.text(raw[56:72], 0, 256, "the summary")
        info["author"] = image.text(raw[72:88], 0, 64, "the author")
        info["homepage"] = image.text(raw[88:104], 0, 256, "the homepage")
        if info["homepage"] and not info["homepage"].startswith("https://"):
            raise ToolError("the homepage must start with https://")
        permissions, network_pointer, network_count = struct.unpack_from("<QQI", raw, 104)
        if permissions >> len(PERMISSIONS):
            raise ToolError("the descriptor asks for a permission this tool does not know")
        if network_count > HOSTS_MAX:
            raise ToolError("the descriptor lists more than 8 network hosts")
        hosts = []
        if network_count:
            table = image.pointer(network_pointer, 16 * network_count)
            hosts = [image.text(table[16 * i:16 * i + 16], 1, 253, "a network host") for i in range(network_count)]
        for host in hosts:
            if not _host_valid(host):
                raise ToolError(f"{host!r} is not an allowed network host (lower-case names, exact or '*.example.com')")
        if len(set(hosts)) != len(hosts):
            raise ToolError("a network host is listed twice")
        info["permissions"] = [p for i, p in enumerate(PERMISSIONS) if permissions >> i & 1]
        info["network"] = hosts
        state_bytes, state_align, settings_bytes, state_layout = struct.unpack_from("<IIIQ", raw, 124)
        if state_bytes > 1 << 20 or settings_bytes > 4096 or (state_bytes and (state_align & (state_align - 1) or not state_align)):
            raise ToolError("the state or settings block has an invalid size or alignment")
        info["state_bytes"], info["state_layout"] = state_bytes, state_layout
        settings, setting_count, hook_count, hooks = struct.unpack_from("<QIIQ", raw, 144)
        commands, command_count, menu_count = struct.unpack_from("<QII", raw, 168)
        buttons, button_count = struct.unpack_from("<QI", raw, 192)
        if setting_count > 64 or hook_count > HOOK_LAST or command_count > 16 or menu_count > 32 or button_count > 16:
            raise ToolError("a descriptor table is longer than its maximum")
        keys = set()
        for i in range(setting_count):
            entry = image.pointer(settings + 128 * i, 128)
            key = image.text(entry[0:16], 1, 32, "a setting key")
            if not re.fullmatch(r"[a-z0-9_]+", key) or key in keys:
                raise ToolError(f"the setting key {key!r} is not [a-z0-9_] or is used twice")
            keys.add(key)
            image.text(entry[16:32], 1, 64, f"the label of setting {key}")
        kinds = set()
        for i in range(hook_count):
            kind, _, _, function = struct.unpack("<IIQQ", image.pointer(hooks + 24 * i, 24))
            if not 1 <= kind <= HOOK_LAST or kind in kinds or not image.executable(function):
                raise ToolError(f"hook {i} has an unknown or repeated kind, or its function is not code")
            kinds.add(kind)
        names = set()
        for i in range(command_count):
            entry = image.pointer(commands + 56 * i, 56)
            name = image.text(entry[0:16], 1, 32, "a command name")
            image.text(entry[16:32], 1, 100, f"the description of /{name}")
            if not re.fullmatch(r"[a-z0-9_-]+", name) or name in names or not image.executable(struct.unpack_from("<Q", entry, 48)[0]):
                raise ToolError(f"command /{name} has a bad or repeated name, or its function is not code")
            names.add(name)
        info.update(settings=setting_count, hooks=hook_count, commands=command_count, menus=menu_count, buttons=button_count)
        if hosts and 17 not in kinds:
            raise ToolError("network hosts need a completion hook (the C++ layer adds it with ctx.http)")
        report.info = info
    except ToolError as error:
        report.errors.append(str(error))


def describe(report: Inspection, path: Path | None = None) -> list:
    lines = []
    info = report.info
    if info:
        lines.append(f"{info['name']} {info['version']}  owner 0x{info['owner_id']:016X}  api level {info['api_level']}")
        if path and Path(path).is_file():
            data = Path(path).read_bytes()
            lines.append(f"{len(data):,} bytes, SHA-256 {hashlib.sha256(data).hexdigest()}")
        lines.append(f"{info['settings']} settings, {info['hooks']} hooks, {info['commands']} commands, "
                     f"{info['menus']} menus, {info['buttons']} buttons, state {info['state_bytes']} bytes")
        lines.append("permissions: " + (", ".join(info["permissions"]) or "none"))
        if info["network"]:
            lines.append("network: " + ", ".join(info["network"]))
    lines += [f"warning: {w}" for w in report.warnings]
    lines += [f"error: {e}" for e in report.errors]
    return lines


# ---- new ------------------------------------------------------------------------------------------------
PLUGIN_TEMPLATE = '''\
// {name}: {summary}
#include <maicord/sdk/plugin.hpp>
using namespace mc;

// Settings get an automatic page; read them in any handler with ctx.settings().<field>.
MC_SETTINGS((Text, greeting, "Greeting", "Hello", .hint = "What /{command} says"),
            (Toggle, sign, "Sign my messages", false));

// State is kept by the host: zeroed at the first start, and kept across a Developer Mode reload while the
// layout (size, alignment and .version) stays the same. Hold no pointers in it.
struct State {{
    u32 greeted;
}};
MC_STATE(State, .version = 1);

// A hook is a plain function; its parameter types pick the hook. This one runs before a message is sent.
static void pre_send(Ctx& ctx, Draft& draft) {{
    if (!ctx.settings().sign)
        return;
    Text<MC_DRAFT_TEXT_MAX> text;
    text.append(draft.text());
    text.append(" ~{slug}");
    draft.replace(text); // does nothing when the text doesn't fit
}}
MC_HOOKS(pre_send);

// A slash command. The Developer page of Settings > Plugins shows what ctx.log writes.
MC_COMMAND({command}, (), "Say hello") {{
    ++ctx.state<State>().greeted;
    ctx.log("{command} ran");
    return Reply::toast(ctx.settings().greeting);
}}

// The owner id is this plugin's identity (its settings and store belong to it). Keep it forever.
MC_PLUGIN(.owner = {owner}, .name = "{name}", .version = "0.1.0", .summary = "{summary}");
'''

README_TEMPLATE = '''\
# {name}

{summary}

Written with the MaiCord plugin SDK (`include/maicord/sdk/README.md`).

```
maicord-plugin build      compile build\\{slug}.dll (build/{slug}.so on Linux) and check it
maicord-plugin dev        copy it into MaiCord's Developer Mode folder (add --watch to rebuild on save)
maicord-plugin check      validate the DLL or shared object again, offline
maicord-plugin pack --author "Your Name" --license MPL-2.0
                          write dist\\{slug}\\ for a pull request to the plugin catalog
```

In MaiCord turn on Developer Mode, then open Settings > Plugins > Developer: the plugin loads from
`%LOCALAPPDATA%\\MaiCord\\plugins\\dev` (the first time it asks for your consent), reloads whenever the DLL or `.so`
changes, and its `ctx.log` lines, budget strikes and reloads show on that page.

On Linux the same commands build `build/{slug}.so` (clang and ld.lld; `--target linux` builds it on Windows too,
`--target windows` the DLL) and the folder is `$XDG_DATA_HOME/maicord/plugins/dev`.

`plugin.cpp` is the whole plugin. Bump `.version` in `MC_PLUGIN` for every release you publish: the catalog
refuses a changed source with an old version.
'''

IGNORE_TEMPLATE = "build/\ndist/\n*.dll\n*.so\n*.obj\n*.o\n*.lib\n*.exp\n*.pdb\n# machine-specific: `maicord-plugin build` writes it again when it is missing\n.clangd\n"


def clangd_text(include: Path, target: str | None = None) -> str:
    inc = include.as_posix()
    triple = LINUX_TRIPLE if (target or host_target()) == "linux" else "x86_64-pc-windows-msvc"
    return ("# Resolves the SDK headers in editors that use clangd (machine-specific; not committed).\n"
            "CompileFlags:\n"
            f"  Add: [--target={triple}, -std=c++23, -nostdinc, -fno-rtti, -fno-exceptions, -DMC_RUNTIME, "
            f"\"-I{inc}\", \"-I{inc}/maicord/sys/libc\"]\n")


def new_owner() -> int:
    """A random 64-bit plugin identity, never 0."""
    while True:
        value = secrets.randbits(64)
        if value:
            return value


def owner_text(owner: int) -> str:
    text = f"{owner:016X}"
    return "0x" + "'".join(text[i:i + 4] for i in range(0, 16, 4))


def command_new(arguments) -> int:
    slug = slugify(arguments.name)
    name = display_name(arguments.name)
    include = sdk_include(arguments.sdk)
    directory = Path(arguments.dir) if arguments.dir else Path.cwd() / slug
    if directory.exists() and any(directory.iterdir()):
        raise ToolError(f"{directory} already exists and is not empty")
    directory.mkdir(parents=True, exist_ok=True)
    command = re.sub(r"[^a-z0-9_]", "_", slug)
    if not re.match(r"[a-z_]", command):
        command = "_" + command
    summary = arguments.summary or "Says hello and signs your messages."
    owner = new_owner()
    values = dict(name=name, slug=slug, command=command, summary=summary, owner=owner_text(owner))
    write_text(directory / "plugin.cpp", PLUGIN_TEMPLATE.format(**values))
    write_text(directory / "README.md", README_TEMPLATE.format(**values))
    write_text(directory / ".gitignore", IGNORE_TEMPLATE)
    write_text(directory / ".clangd", clangd_text(include))
    say(f"Created {directory} (owner id {owner_text(owner)}).")
    say(f"Next: cd {directory.name}; maicord-plugin build; maicord-plugin dev")
    return 0


# ---- build, check, dev ------------------------------------------------------------------------------------
def command_build(arguments) -> int:
    project = open_project(arguments.dir, arguments.source)
    target = target_of(arguments)
    toolchain = find_target_toolchain(target, arguments.llvm_bin, not arguments.no_fetch)
    include = sdk_include(arguments.sdk)
    if not (project.directory / ".clangd").exists():
        write_text(project.directory / ".clangd", clangd_text(include, target))
    started = time.monotonic()
    binary = build_project(project, toolchain, include, arguments.verbose, target)
    report = inspect_dll(binary)
    for line in describe(report, binary):
        say(line)
    if not report.ok:
        binary.unlink(missing_ok=True)
        raise ToolError(f"the {binary_kind(target)} fails the plugin checks (above); MaiCord would refuse to load it")
    say(f"Built {binary} in {time.monotonic() - started:.1f} s ({toolchain.origin}).")
    return 0


def command_check(arguments) -> int:
    target = target_of(arguments)
    path = Path(arguments.dll) if arguments.dll else open_project(arguments.dir, arguments.source).output(target)
    report = inspect_dll(path)
    for line in describe(report, path):
        say(line)
    if report.ok:
        say(f"The {report.format} headers and the descriptor pass.")
    return 0 if report.ok else 1


def deploy(dll: Path, folder: Path) -> None:
    """Copies dll into folder (atomically, so the watcher never sees half a file) unless it is there, and says which."""
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / dll.name
    copied = not (target.is_file() and target.read_bytes() == dll.read_bytes())
    if copied:
        partial = folder / (dll.name + ".part")
        shutil.copyfile(dll, partial)
        os.replace(partial, target)
    say(f"{dll.name}: {'copied' if copied else 'unchanged'} in {folder}" + (" (MaiCord reloads it)" if copied else ""))


def watch(files, step, interval: float = 0.4, rounds: int | None = None) -> None:
    """Calls step() whenever the newest of files() changes (polling mtimes) until Ctrl+C; a ToolError is printed and
    the watch goes on, as the next save may fix it. rounds limits the polls, for tests."""
    def stamp():
        return max((p.stat().st_mtime_ns for p in files() if p.is_file()), default=0)

    seen = stamp()
    polled = 0
    try:
        while rounds is None or polled < rounds:
            time.sleep(interval)
            polled += 1
            if stamp() != seen:
                time.sleep(interval)  # let the editor finish writing
                seen = stamp()
                try:
                    step()
                except ToolError as error:
                    say(str(error))
    except KeyboardInterrupt:
        pass


def command_dev(arguments) -> int:
    project = open_project(arguments.dir, arguments.source)
    target = target_of(arguments)
    binary = project.output(target)
    folder = dev_folder("plugin", arguments.dev_folder)
    include = sdk_include(arguments.sdk)
    toolchain = None

    def build_and_deploy(force: bool) -> None:
        nonlocal toolchain
        if force or is_stale(project, target):
            toolchain = toolchain or find_target_toolchain(target, arguments.llvm_bin, not arguments.no_fetch)
            build_project(project, toolchain, include, arguments.verbose, target)
            report = inspect_dll(binary)
            if not report.ok:
                binary.unlink(missing_ok=True)
                raise ToolError(f"the {binary_kind(target)} fails the plugin checks:\n" + "\n".join(describe(report, binary)))
        deploy(binary, folder)

    build_and_deploy(False)
    if not arguments.watch:
        return 0
    say("Watching for changes; Ctrl+C stops.")
    watch(lambda: sources(project), lambda: build_and_deploy(True), rounds=arguments.rounds)
    return 0


# ---- pack -------------------------------------------------------------------------------------------------
def command_pack(arguments) -> int:
    project = open_project(arguments.dir, arguments.source)
    target = target_of(arguments)
    binary = project.output(target)
    if not binary.is_file() or is_stale(project, target):
        raise ToolError(f"build first (maicord-plugin build{target_option(target)}): the {binary_kind(target)} is missing or "
                        "older than the source")
    report = inspect_dll(binary)
    for line in describe(report, binary):
        say(line)
    if not report.ok:
        raise ToolError(f"the {binary_kind(target)} fails the plugin checks")
    info = report.info
    authors = [{"name": arguments.author}] if arguments.author else ([{"name": info["author"]}] if info["author"] else [])
    if not authors:
        raise ToolError("give the author the catalog credits: --author \"Name\" (or set .author in MC_PLUGIN)")
    if arguments.github:
        authors[0]["github"] = arguments.github
    if not arguments.license or not re.fullmatch(r"[A-Za-z0-9.+-]{1,32}", arguments.license):
        raise ToolError("give the license as an SPDX id, e.g. --license MPL-2.0 (it must allow redistribution and modification)")
    categories = arguments.category or ["utility"]
    if len(categories) > 3 or any(c not in CATEGORIES for c in categories):
        raise ToolError(f"--category takes up to 3 of: {', '.join(CATEGORIES)}")
    if not VERSION.fullmatch(info["version"]):
        raise ToolError(f"the version {info['version']!r} must be MAJOR.MINOR.PATCH (each 0 to 999)")
    if len(info["name"].encode("utf-8")) > 95 or not info["summary"] or len(info["summary"].encode("utf-8")) > 255:
        raise ToolError("the catalog needs a name up to 95 bytes and a summary of 1 to 255 bytes in MC_PLUGIN")
    banned = [p for p in info["permissions"] if p in ("automated_actions", "persistent_log")]
    if banned:
        raise ToolError(f"the official catalog refuses {', '.join(banned)}; such a plugin can only be sideloaded")
    output = Path(arguments.output) if arguments.output else project.directory / "dist"
    folder = output / project.slug
    if folder.exists() and not arguments.force:
        raise ToolError(f"{folder} exists (use --force to replace it)")
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    shutil.copyfile(project.source, folder / "plugin.cpp")
    catalog = {"name": info["name"], "summary": info["summary"], "authors": authors, "license": arguments.license,
               "categories": categories}
    if info["homepage"]:
        catalog["homepage"] = info["homepage"]
    catalog["owner_id"] = f"0x{info['owner_id']:016X}"
    write_text(folder / "catalog.json", json.dumps(catalog, indent=2, ensure_ascii=False) + "\n")
    details = project.directory / "DETAILS.md"
    text = details.read_text(encoding="utf-8") if details.is_file() else (
        f"{info['summary']}\n\n### Settings\n\n- Describe each setting here.\n")
    write_text(folder / "details.md", text)
    say(f"Wrote {folder}: plugin.cpp, catalog.json, details.md.")
    say(f"Copy it to plugins/{project.slug}/ in a clone of maj113/MaiCord-plugins and open a pull request;")
    say("CI builds the DLL and the Linux shared object from plugin.cpp, so keep it the only source file, and bump .version "
        "for every change.")
    return 0
