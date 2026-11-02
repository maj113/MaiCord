"""The plugin half of maicord-plugin: names, the SDK and dev folders, the toolchain, projects, the PE and
descriptor checks, and the new, build, check, dev and pack commands. maicord_plugin.py is the command line.

Python 3.9 or newer, standard library only. The compiler is the pinned clang-cl and lld-link of
tools/llvm_toolchain.json (fetched once into %LOCALAPPDATA%\\MaiCord-toolchain, as tools/build.ps1 does),
or the pair found on PATH. Plugins compile against MaiCord's own SDK headers only (no Windows SDK or
Visual Studio): the repository's include/ directory, or the include/ of an SDK folder.
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

# The flags of cmake's maicord_plugin() and of the catalog's CI (MaiCord-plugins scripts/build_catalog.py), so a
# local build is the catalog's build: same source, same compiler, same bytes (/Brepro).
COMPILE = ["/nologo", "/c", "/std:c++latest", "/clang:-nostdinc", "/O2", "/W4", "/WX", "/utf-8", "/Zl", "/GS-", "/GR-",
           "/permissive-", "/clang:-fno-asynchronous-unwind-tables", "/clang:-fno-unwind-tables", "/DMC_RUNTIME"]
LINK = ["/nologo", "/DLL", "/NOENTRY", "/NODEFAULTLIB", "/SUBSYSTEM:WINDOWS,6.02", "/DYNAMICBASE", "/HIGHENTROPYVA",
        "/NXCOMPAT", "/CETCOMPAT", "/OPT:REF", "/OPT:ICF=8", "/INCREMENTAL:NO", "/ALLOWBIND:NO",
        "/MERGE:.pdata=.rdata", "/DEBUG:NONE", "/Brepro", "/mllvm:-enable-machine-outliner"]


class ToolError(Exception):
    """A problem to report in one line, without a traceback."""


def say(text: str = "") -> None:
    print(text, flush=True)


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


def dev_folder(kind: str, explicit: str | None = None) -> Path:
    """Where Developer Mode watches: plugins\\dev or effects\\dev under %LOCALAPPDATA%\\MaiCord, or the override."""
    if explicit:
        return Path(explicit)
    variable = "MAICORD_PLUGIN_DEV" if kind == "plugin" else "MAICORD_EFFECT_DEV"
    if os.environ.get(variable):
        return Path(os.environ[variable])
    return local_app_data() / "MaiCord" / ("plugins" if kind == "plugin" else "effects") / "dev"


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


def open_project(directory: str | None, source: str | None = None) -> Project:
    root = Path(directory or ".").resolve()
    file = Path(source) if source else Path("plugin.cpp")
    file = file if file.is_absolute() else root / file
    if not file.is_file():
        raise ToolError(f"{file} was not found (run this in a plugin folder, or use --dir; 'maicord-plugin new <name>' makes one)")
    slug = re.sub(r"[^a-z0-9-]+", "-", root.name.lower()).strip("-")[:SLUG_MAX].strip("-")
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


def is_stale(project: Project) -> bool:
    if not project.dll.is_file():
        return True
    built = project.dll.stat().st_mtime_ns
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


def build_project(project: Project, toolchain: Toolchain, include: Path, verbose: bool = False) -> Path:
    """Compiles and links plugin.cpp into build/<slug>.dll, then runs the PE checks on it."""
    project.build_directory.mkdir(exist_ok=True)
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

    @property
    def ok(self) -> bool:
        return not self.errors


class _Image:
    def __init__(self, data: bytes, report: Inspection):
        self.data, self.report = data, report
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


def _host_valid(host: str) -> bool:
    body = host[2:] if host.startswith("*.") else host
    labels = body.split(".")
    return (len(labels) >= 2 and len(host) <= 253 and all(re.fullmatch(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?", label) and
            len(label) <= 63 for label in labels)) and not re.fullmatch(r"[0-9.]+", body)


def inspect_dll(path: Path) -> Inspection:
    """The checks the client's loader runs before it maps a plugin, done on the file alone. The client repeats
    all of them (and more) at load: this finds the obvious problems before MaiCord does."""
    report = Inspection()
    try:
        data = Path(path).read_bytes()
        _check_pe(_Image(data, report), report)
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
maicord-plugin build      compile build\\{slug}.dll and check it
maicord-plugin dev        copy it into MaiCord's Developer Mode folder (add --watch to rebuild on save)
maicord-plugin check      validate the DLL again, offline
maicord-plugin pack --author "Your Name" --license MPL-2.0
                          write dist\\{slug}\\ for a pull request to the plugin catalog
```

In MaiCord turn on Developer Mode, then open Settings > Plugins > Developer: the plugin loads from
`%LOCALAPPDATA%\\MaiCord\\plugins\\dev` (the first time it asks for your consent), reloads whenever the DLL
changes, and its `ctx.log` lines, budget strikes and reloads show on that page.

`plugin.cpp` is the whole plugin. Bump `.version` in `MC_PLUGIN` for every release you publish: the catalog
refuses a changed source with an old version.
'''

IGNORE_TEMPLATE = "build/\ndist/\n*.dll\n*.obj\n*.lib\n*.exp\n*.pdb\n# machine-specific: `maicord-plugin build` writes it again when it is missing\n.clangd\n"


def clangd_text(include: Path) -> str:
    inc = include.as_posix()
    return ("# Resolves the SDK headers in editors that use clangd (machine-specific; not committed).\n"
            "CompileFlags:\n"
            "  Add: [--target=x86_64-pc-windows-msvc, -std=c++23, -nostdinc, -fno-rtti, -fno-exceptions, -DMC_RUNTIME, "
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
    summary = arguments.summary or f"Says hello and signs your messages."
    owner = new_owner()
    values = dict(name=name, slug=slug, command=command, summary=summary, owner=owner_text(owner))
    (directory / "plugin.cpp").write_text(PLUGIN_TEMPLATE.format(**values), encoding="utf-8", newline="\n")
    (directory / "README.md").write_text(README_TEMPLATE.format(**values), encoding="utf-8", newline="\n")
    (directory / ".gitignore").write_text(IGNORE_TEMPLATE, encoding="utf-8", newline="\n")
    (directory / ".clangd").write_text(clangd_text(include), encoding="utf-8", newline="\n")
    say(f"Created {directory} (owner id {owner_text(owner)}).")
    say(f"Next: cd {directory.name}; maicord-plugin build; maicord-plugin dev")
    return 0


# ---- build, check, dev ------------------------------------------------------------------------------------
def command_build(arguments) -> int:
    project = open_project(arguments.dir, arguments.source)
    toolchain = find_toolchain(arguments.llvm_bin, not arguments.no_fetch)
    include = sdk_include(arguments.sdk)
    if not (project.directory / ".clangd").exists():
        (project.directory / ".clangd").write_text(clangd_text(include), encoding="utf-8", newline="\n")
    started = time.monotonic()
    dll = build_project(project, toolchain, include, arguments.verbose)
    report = inspect_dll(dll)
    for line in describe(report, dll):
        say(line)
    if not report.ok:
        dll.unlink(missing_ok=True)
        raise ToolError("the DLL fails the plugin checks (above); MaiCord would refuse to load it")
    say(f"Built {dll} in {time.monotonic() - started:.1f} s ({toolchain.origin}).")
    return 0


def command_check(arguments) -> int:
    if arguments.dll:
        path = Path(arguments.dll)
    else:
        path = open_project(arguments.dir, arguments.source).dll
    report = inspect_dll(path)
    for line in describe(report, path):
        say(line)
    if report.ok:
        say("The PE headers and the descriptor pass.")
    return 0 if report.ok else 1


def deploy(dll: Path, folder: Path) -> str:
    """Copies dll into folder (atomically, so the watcher never sees half a file). 'unchanged' when it is there."""
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / dll.name
    if target.is_file() and target.read_bytes() == dll.read_bytes():
        return "unchanged"
    partial = folder / (dll.name + ".part")
    shutil.copyfile(dll, partial)
    os.replace(partial, target)
    return "copied"


def watch(project: Project, step, interval: float = 0.4, rounds: int | None = None) -> None:
    """Calls step() whenever a source changes (polling mtimes); rounds limits the polls, for tests."""
    def stamp():
        return max((p.stat().st_mtime_ns for p in sources(project)), default=0)

    seen = stamp()
    polled = 0
    while rounds is None or polled < rounds:
        time.sleep(interval)
        polled += 1
        now = stamp()
        if now != seen:
            time.sleep(interval)  # let the editor finish writing
            seen = stamp()
            step()


def command_dev(arguments) -> int:
    project = open_project(arguments.dir, arguments.source)
    folder = dev_folder("plugin", arguments.dev_folder)
    include = sdk_include(arguments.sdk)
    toolchain = None

    def build_and_deploy(force: bool) -> None:
        nonlocal toolchain
        if force or is_stale(project):
            toolchain = toolchain or find_toolchain(arguments.llvm_bin, not arguments.no_fetch)
            dll = build_project(project, toolchain, include, arguments.verbose)
            report = inspect_dll(dll)
            if not report.ok:
                dll.unlink(missing_ok=True)
                raise ToolError("the DLL fails the plugin checks:\n" + "\n".join(describe(report, dll)))
        result = deploy(project.dll, folder)
        say(f"{project.dll.name}: {result} in {folder}" + (" (MaiCord reloads it)" if result == "copied" else ""))

    build_and_deploy(False)
    if not arguments.watch:
        return 0
    say("Watching for changes; Ctrl+C stops.")

    def again() -> None:
        try:
            build_and_deploy(True)
        except ToolError as error:
            say(str(error))  # keep watching: the next save may fix it

    try:
        watch(project, again, rounds=arguments.rounds)
    except KeyboardInterrupt:
        pass
    return 0


# ---- pack -------------------------------------------------------------------------------------------------
def command_pack(arguments) -> int:
    project = open_project(arguments.dir, arguments.source)
    dll = project.dll
    if not dll.is_file() or is_stale(project):
        raise ToolError("build first (maicord-plugin build): the DLL is missing or older than the source")
    report = inspect_dll(dll)
    for line in describe(report, dll):
        say(line)
    if not report.ok:
        raise ToolError("the DLL fails the plugin checks")
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
    if not re.fullmatch(r"(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})", info["version"]):
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
    (folder / "catalog.json").write_text(json.dumps(catalog, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    details = project.directory / "DETAILS.md"
    text = details.read_text(encoding="utf-8") if details.is_file() else (
        f"{info['summary']}\n\n### Settings\n\n- Describe each setting here.\n")
    (folder / "details.md").write_text(text, encoding="utf-8", newline="\n")
    say(f"Wrote {folder}: plugin.cpp, catalog.json, details.md.")
    say(f"Copy it to plugins/{project.slug}/ in a clone of maj113/MaiCord-plugins and open a pull request;")
    say("CI builds the DLL from plugin.cpp, so keep it the only source file, and bump .version for every change.")
    return 0
