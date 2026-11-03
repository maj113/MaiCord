"""The effect half of maicord-plugin: `effect new`, `effect build` and `effect preview`.

An effect is one HLSL file written against maicord/sdk/background.hlsli. Its knobs and metadata are comments at
the top of the file (`// @effect`, `// @knob`, `// @uses`, `// @blend`, design section 16.3); this tool reads them,
compiles the file to ps_5_0 bytecode, lints what the client will run (only b0, t0-t4 and s0-s2, no UAVs, at most
4,096 instructions and 64 texture reads), keeps the `<slug>.effect` sidecar in step with the comments and packs the
`.mcfx` container the client's mcfx_parse reads:

  header   16 bytes: "MCFX", u16 format = 1, u16 section count (<= 9), u32 file bytes, u32 0
  table    count x 16 bytes: u32 type, u32 offset, u32 bytes, u32 0
  types    1 MANIFEST, 2 DXBC, 3 TEXTURE0, 4 TEXTURE1, 5 SOURCE, 6 PREVIEW, 7 TEXTURE2, 8 TEXTURE3, 9 SHADERS;
           ascending, each at most once
  sections packed back to back after the table; the last one ends at the end of the file

A pass graph (interface 3) is declared with `// @target` and `// @pass` lines: each pass names its own entry points
(ps_5_0, and vs_5_0 over host geometry), the tool compiles and lints every one (the textures t0-t10 then), packs the
first pass's pixel shader as DXBC and the others, in order of first use, as SHADERS. A declared texture N is packed
from texture<N>.png (8-bit RGB or RGBA) or texture<N>.mcft (float data: "MCFT", u16 width, u16 height, RGBA32F
texels; interface 3) beside the HLSL.

Compiling uses fxc.exe when the Windows SDK has it (MAICORD_FXC_EXECUTABLE, the SDK's bin, PATH; the catalog's CI
uses it, so its bytes are the catalog's), else the in-box d3dcompiler_47.dll through ctypes, so no SDK is needed.
The cost class (light, medium, heavy) is measured on WARP by the catalog's CI; here the static counts show whether
an effect is likely to be refused.

The manifest's "interface" is 1; 2 when the effect uses clock or appearance (mc_ambient, after mc_knobs); 3 with a
pass graph, texture2/texture3 or a float data texture. A MaiCord before an interface refuses such a package instead
of running it without what it needs. `effect preview` can stand in for the ambient inputs (--time, --date,
--day-seconds, --appearance) through simulate-inputs.txt in the dev folder, which MaiCord reads in Developer Mode.
"""

from __future__ import annotations

import argparse
import ctypes
import datetime
import json
import os
import re
import shutil
import struct
import subprocess
from pathlib import Path

import plugin_tool as core
from plugin_tool import ToolError, say, write_text

# Interface 1, and the names of interface 2 (mc_ambient) and 3: an effect that uses one is packed at that interface.
INTERFACE = 1
AMBIENT_USES = ("clock", "appearance")
GRAPH_USES = ("texture2", "texture3")
MANIFEST_MAX = 16 * 1024
DXBC_MAX = 64 * 1024
SHADERS_MAX = 512 * 1024
TEXTURE_MAX = 512 * 1024
SOURCE_MAX = 64 * 1024
PACKAGE_MAX = 1536 * 1024
INSTRUCTIONS_MAX = 4096
SAMPLES_MAX = 64
KNOB_ROWS = 8
NAME_MAX = 48
USES = ("noise", "glyphs", "blue_noise", "pointer", "texture0", "texture1", *AMBIENT_USES, *GRAPH_USES)
TEXTURES = ("texture0", "texture1", "texture2", "texture3")
SIMULATION_FILE = "simulate-inputs.txt"
BLENDS = ("normal", "screen", "add")
RESOLUTIONS = (25, 35, 50, 75, 100)
FRAME_RATES = (10, 15, 24, 30, 60)
SECTION = {"MANIFEST": 1, "DXBC": 2, "TEXTURE0": 3, "TEXTURE1": 4, "SOURCE": 5, "PREVIEW": 6, "TEXTURE2": 7,
           "TEXTURE3": 8, "SHADERS": 9}
# The pass graph's limits (include/maicord/render/effects/background_effect.hpp).
GRAPH_PASSES, GRAPH_TARGETS, GRAPH_SHADERS = 6, 4, 12
TARGET_EXTENT, FIXED_BYTES, SCALED_BYTES = 512, 8 << 20, 16
GRID_EXTENT, QUADS_MAX = 256, 16384
TEXEL_BYTES = {"rgba8": 4, "rgba16f": 8, "rgba32f": 16}
PASS_BLENDS = ("opaque", "alpha", "add", "premultiplied")
VERSION_TOKENS = {"ps_5_0": 0x50, "vs_5_0": 0x10050}


# ---- the HLSL header -------------------------------------------------------------------------------------
_PAIR = re.compile(r'(\w+)="([^"]*)"')
_NUMBER = r"-?\d+(?:\.\d+)?"
_KNOB = re.compile(r'//\s*@knob\s+([a-z][a-z0-9_]*)\s+(float|int|toggle|color|choice)\s*(.*)$')
_IDENTIFIER = r"[A-Za-z_][A-Za-z0-9_]*"
_TARGET = re.compile(r"//\s*@target\s+([a-z][a-z0-9_]*)\s+(rgba8|rgba16f|rgba32f)\s+(?:(\d+)x(\d+)|(\d+)%)(\s+persistent)?$")


def _number(text: str):
    value = float(text)
    return int(value) if value == int(value) and "." not in text else value


def parse_header(source: str) -> dict:
    """The `@effect`, `@knob`, `@uses`, `@blend`, `@target` and `@pass` comment lines of one effect."""
    header = {"effect": {}, "knobs": [], "uses": [], "blend": None, "targets": [], "passes": []}
    for number, line in enumerate(source.splitlines(), 1):
        line = line.strip()
        where = f"line {number}"
        if line.startswith("// @effect"):
            header["effect"] = dict(_PAIR.findall(line))
        elif line.startswith("// @uses"):
            header["uses"] = line[len("// @uses"):].split()
        elif line.startswith("// @blend"):
            header["blend"] = line[len("// @blend"):].strip()
        elif line.startswith("// @knob"):
            match = _KNOB.match(line)
            if not match:
                raise ToolError(f"{where}: cannot read the @knob line")
            header["knobs"].append(_parse_knob(match, where))
        elif line.startswith("// @target"):
            match = _TARGET.match(line)
            if not match:
                raise ToolError(f"{where}: expected @target <id> rgba8|rgba16f|rgba32f <W>x<H>|<P>% [persistent]")
            target = {"id": match.group(1), "format": match.group(2), "persistent": bool(match.group(6))}
            if match.group(5):
                target["scale"] = int(match.group(5))
            else:
                target["size"] = [int(match.group(3)), int(match.group(4))]
            header["targets"].append(target)
        elif line.startswith("// @pass"):
            header["passes"].append(_parse_pass(line[len("// @pass"):].split(), where))
    for use in header["uses"]:
        if use not in USES:
            raise ToolError(f"@uses names {use!r}; the interface has {', '.join(USES)}")
    if header["blend"] not in BLENDS:
        raise ToolError(f"@blend must be one of {', '.join(BLENDS)}")
    if not header["effect"].get("name") or not header["effect"].get("license"):
        raise ToolError('@effect needs name="..." and license="..." (an SPDX id)')
    if len({k["id"] for k in header["knobs"]}) != len(header["knobs"]):
        raise ToolError("two knobs have the same id")
    if header["targets"] and not header["passes"]:
        raise ToolError("@target needs @pass lines that draw into it")
    return header


def _parse_pass(words: list, where: str) -> dict:
    """`@pass <ps> [vs=<entry>] [grid=WxH | quads=N] [to=<target>] [in=<target>,...] [blend=...] [clear]`."""
    if not words or not re.fullmatch(_IDENTIFIER, words[0]):
        raise ToolError(f"{where}: @pass starts with the pixel shader's entry point")
    result = {"ps": words[0], "to": "out", "in": [], "blend": "opaque", "clear": False}
    for word in words[1:]:
        key, _, value = word.partition("=")
        if word == "clear":
            result["clear"] = True
        elif key == "vs" and re.fullmatch(_IDENTIFIER, value):
            result["vs"] = value
        elif key == "grid" and re.fullmatch(r"\d+x\d+", value):
            result["grid"] = [int(v) for v in value.split("x")]
        elif key == "quads" and value.isdigit():
            result["quads"] = int(value)
        elif key == "to" and value:
            result["to"] = value
        elif key == "in" and value:
            result["in"] = value.split(",")
        elif key == "blend" and value in PASS_BLENDS:
            result["blend"] = value
        else:
            raise ToolError(f"{where}: cannot read {word!r} (vs=, grid=WxH, quads=N, to=, in=, "
                            f"blend={'|'.join(PASS_BLENDS)}, clear)")
    return result


def graph_of(header: dict) -> tuple[list, list, list] | None:
    """(manifest "targets", manifest "passes", the shaders as (entry, profile) in index order), checked against the
    client's limits; None without @pass lines. Shader 0 is the first pass's pixel shader; the others follow in order
    of first use."""
    targets, passes = header["targets"], header["passes"]
    if not passes:
        return None
    if len(passes) > GRAPH_PASSES or len(targets) > GRAPH_TARGETS:
        raise ToolError(f"a graph has at most {GRAPH_PASSES} passes and {GRAPH_TARGETS} targets")
    ids = [t["id"] for t in targets]
    if len(set(ids)) != len(ids) or "out" in ids:
        raise ToolError("target ids must differ, and 'out' is the output")
    fixed = scaled = 0
    documents = []
    for target in targets:
        planes = 2 if target["persistent"] else 1
        if "size" in target:
            width, height = target["size"]
            if not (1 <= width <= TARGET_EXTENT and 1 <= height <= TARGET_EXTENT):
                raise ToolError(f"target {target['id']}: a fixed size is 1x1 to {TARGET_EXTENT}x{TARGET_EXTENT}")
            fixed += TEXEL_BYTES[target["format"]] * planes * width * height
            document = {"format": target["format"], "size": [width, height]}
        else:
            if not 1 <= target["scale"] <= 100:
                raise ToolError(f"target {target['id']}: a scale is 1% to 100% of the output")
            scaled += TEXEL_BYTES[target["format"]] * planes * target["scale"] ** 2
            document = {"format": target["format"], "scale": target["scale"]}
        if target["persistent"]:
            document["persistent"] = True
        documents.append(document)
    if fixed > FIXED_BYTES or scaled > SCALED_BYTES * 10000:
        raise ToolError(f"the targets need more than {FIXED_BYTES >> 20} MiB at fixed sizes, or more than "
                        f"{SCALED_BYTES} bytes per output pixel")
    shaders = [(passes[0]["ps"], "ps_5_0")]
    for item in passes:
        for entry, profile in ((item.get("vs"), "vs_5_0"), (item["ps"], "ps_5_0")):
            if entry and (entry, profile) not in shaders:
                if (entry, "ps_5_0" if profile == "vs_5_0" else "vs_5_0") in shaders:
                    raise ToolError(f"{entry} is used as both a vertex and a pixel shader")
                shaders.append((entry, profile))
    if len(shaders) > GRAPH_SHADERS:
        raise ToolError(f"a graph has at most {GRAPH_SHADERS} shaders")
    pass_documents = []
    for item in passes:
        where = f"@pass {item['ps']}"
        document = {"ps": shaders.index((item["ps"], "ps_5_0"))}
        if ("grid" in item or "quads" in item) != ("vs" in item) or ("grid" in item and "quads" in item):
            raise ToolError(f"{where}: a vertex shader draws grid=WxH or quads=N, and only one")
        if "vs" in item:
            document["vs"] = shaders.index((item["vs"], "vs_5_0"))
        if "grid" in item:
            if not all(2 <= v <= GRID_EXTENT for v in item["grid"]):
                raise ToolError(f"{where}: a grid is 2x2 to {GRID_EXTENT}x{GRID_EXTENT} points")
            document["grid"] = item["grid"]
        if "quads" in item:
            if not 1 <= item["quads"] <= QUADS_MAX:
                raise ToolError(f"{where}: quads is 1 to {QUADS_MAX}")
            document["quads"] = item["quads"]
        for name in item["in"] + ([] if item["to"] == "out" else [item["to"]]):
            if name not in ids:
                raise ToolError(f"{where}: no target {name!r} (the output is 'out', which no pass reads)")
        inputs = [ids.index(name) for name in item["in"]]
        if len(set(inputs)) != len(inputs):
            raise ToolError(f"{where}: an input is named twice")
        if item["to"] != "out":
            target = ids.index(item["to"])
            if target in inputs and not targets[target]["persistent"]:
                raise ToolError(f"{where}: a pass reads the target it draws only when that target is persistent")
            document["target"] = target
        if inputs:
            document["inputs"] = inputs
        if item["blend"] != "opaque":
            document["blend"] = item["blend"]
        if item["clear"]:
            document["clear"] = True
        pass_documents.append(document)
    if all(item["to"] != "out" for item in passes):
        raise ToolError("no @pass draws the output (to=out, the default)")
    return documents, pass_documents, shaders


def _parse_knob(match, where: str) -> dict:
    knob_id, kind, rest = match.group(1), match.group(2), match.group(3)
    if len(knob_id) > 24:
        raise ToolError(f"{where}: a knob id is at most 24 characters")
    knob = {"id": knob_id, "type": kind}
    label = re.search(r'"([^"]*)"\s*$', rest)
    if not label or not 1 <= len(label.group(1).encode("utf-8")) <= 48:
        raise ToolError(f"{where}: the knob needs a label of 1 to 48 bytes in quotes at the end")
    knob["label"] = label.group(1)
    rest = rest[:label.start()].strip()
    if kind in ("float", "int"):
        parsed = re.fullmatch(rf"({_NUMBER})\.\.({_NUMBER})\s*=\s*({_NUMBER})(\s*%)?", rest)
        if not parsed:
            raise ToolError(f"{where}: expected <min>..<max> = <default>" + (" [%]" if kind == "float" else ""))
        if parsed.group(4) and kind != "float":
            raise ToolError(f"{where}: only a float knob can show as a percentage")
        low, high, default = (_number(parsed.group(i)) for i in (1, 2, 3))
        if kind == "int" and not all(isinstance(v, int) for v in (low, high, default)):
            raise ToolError(f"{where}: an int knob needs integers")
        if not low < high or not low <= default <= high:
            raise ToolError(f"{where}: expected min < max and min <= default <= max")
        knob.update({"min": low, "max": high, "default": default})
        if parsed.group(4):
            knob["percent"] = True  # shown as 75 % for 0.75
    elif kind == "toggle":
        if rest not in ("= on", "= off", "=on", "=off"):
            raise ToolError(f"{where}: expected = on or = off")
        knob["default"] = rest.endswith("on")
    elif kind == "color":
        parsed = re.fullmatch(r"=\s*(#[0-9A-Fa-f]{6})", rest)
        if not parsed:
            raise ToolError(f"{where}: expected = #RRGGBB")
        knob["default"] = parsed.group(1)
    else:
        parsed = re.fullmatch(r'"([^"]+)"\s*=\s*(\d+)', rest)
        if not parsed:
            raise ToolError(f'{where}: expected "A|B" = <index>')
        knob["choices"] = parsed.group(1).split("|")
        knob["default"] = int(parsed.group(2))
        if not 2 <= len(knob["choices"]) <= 16 or knob["default"] >= len(knob["choices"]) or \
                any(not c or len(c.encode("utf-8")) > 24 for c in knob["choices"]):
            raise ToolError(f"{where}: a choice knob needs 2 to 16 choices of 1 to 24 bytes and a default among them")
    return knob


def parse_sidecar(text: str) -> dict:
    values = {}
    for line in text.splitlines():
        if line.strip() and not line.startswith("#"):
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip()
    return values


# ---- knob slots ---------------------------------------------------------------------------------------------
def assign_slots(knobs: list) -> list:
    """(row, lane) per knob, in declaration order: scalars, toggles and choices take one lane and wrap at four;
    a colour skips to a fresh row and takes its .rgb (lane None)."""
    row, lane, slots = 0, 0, []
    for knob in knobs:
        if knob["type"] == "color":
            if lane:
                row, lane = row + 1, 0
            slots.append((row, None))
            row += 1
        else:
            slots.append((row, lane))
            lane += 1
            if lane == 4:
                row, lane = row + 1, 0
    if row + (1 if lane else 0) > KNOB_ROWS:
        raise ToolError(f"the knobs need more than {KNOB_ROWS} rows of mc_knobs")
    return slots


def define_text(knob: dict, slot: tuple) -> str:
    row, lane = slot
    access = "rgb" if lane is None else "xyzw"[lane]
    read = f"mc_knobs[{row}].{access}"
    if knob["type"] == "int" or knob["type"] == "choice":
        read = f"((int){read})"
    elif knob["type"] == "toggle":
        read = f"({read} != 0)"
    else:
        read = f"({read})"
    return f"#define knob_{knob['id']} {read}"


def knob_values(knobs: list) -> list:
    """The 32 floats of mc_knobs at the knobs' defaults."""
    values = [0.0] * (KNOB_ROWS * 4)
    for knob, (row, lane) in zip(knobs, assign_slots(knobs)):
        default = knob["default"]
        if knob["type"] == "color":
            for channel in range(3):
                values[row * 4 + channel] = int(default[1 + 2 * channel:3 + 2 * channel], 16) / 255
        else:
            values[row * 4 + lane] = float(default)
    return values


def check_defines(source: str, knobs: list) -> None:
    """Every knob has a `#define knob_<id>` that reads the slot the container assigns."""
    swizzle = str.maketrans("rgba", "xyzw")
    for knob, slot in zip(knobs, assign_slots(knobs)):
        row, lane = slot
        match = re.search(rf"#define\s+knob_{knob['id']}\b.*?mc_knobs\[(\d+)\]\.([xyzwrgba]+)", source)
        want = ("rgb" if lane is None else "xyzw"[lane]).translate(swizzle)
        if not match or int(match.group(1)) != row or match.group(2).translate(swizzle) != want:
            raise ToolError(f"knob {knob['id']} must read its slot; add or fix:\n    {define_text(knob, slot)}")


# ---- compiling ------------------------------------------------------------------------------------------------
def find_fxc() -> str | None:
    candidates = [os.environ.get("MAICORD_FXC_EXECUTABLE")]
    if os.environ.get("WindowsSdkVerBinPath"):
        candidates.append(str(Path(os.environ["WindowsSdkVerBinPath"]) / "x64" / "fxc.exe"))
    candidates.append(shutil.which("fxc"))
    root = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    if root.is_dir():
        candidates += [str(p / "x64" / "fxc.exe") for p in sorted(root.iterdir(), reverse=True)]
    return next((c for c in candidates if c and Path(c).is_file()), None)


def expand_includes(source: str, include: Path, name: str = "", seen: set | None = None, depth: int = 0) -> str:
    """Inlines #include <...> and "..." (the SDK's background.hlsli) with #line markers, so d3dcompiler needs no
    include handler and its errors name the right file and line."""
    seen = set() if seen is None else seen
    if depth > 8:
        raise ToolError("#include nests too deeply")
    lines = []
    for number, line in enumerate(source.splitlines(), 1):
        match = re.match(r'\s*#\s*include\s*[<"]([^>"]+)[>"]', line)
        if not match:
            lines.append(line)
            continue
        path = include / match.group(1)
        if not path.is_file():
            raise ToolError(f"#include {match.group(1)} was not found under {include}")
        key = str(path.resolve())
        if key not in seen:
            seen.add(key)
            lines.append(f'#line 1 "{path.name}"')
            lines.append(expand_includes(path.read_text(encoding="utf-8"), include, path.name, seen, depth + 1))
            lines.append(f'#line {number + 1} "{name}"')
        else:
            lines.append("")  # keep the line numbers of what follows
    return "\n".join(lines) + "\n"


class _Blob:
    """An ID3DBlob returned by d3dcompiler_47."""

    def __init__(self, pointer):
        self.pointer = pointer

    def _call(self, index: int, result, *args):
        table = ctypes.cast(ctypes.cast(self.pointer, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        return ctypes.WINFUNCTYPE(result, ctypes.c_void_p, *[ctypes.c_void_p] * len(args))(table[index])(self.pointer, *args)

    def read(self) -> bytes:
        return ctypes.string_at(self._call(3, ctypes.c_void_p), self._call(4, ctypes.c_size_t))

    def release(self) -> None:
        self._call(2, ctypes.c_ulong)


def compile_d3dcompiler(source: str, name: str, entry: str = "main", profile: str = "ps_5_0") -> tuple[bytes, str]:
    """Bytecode and its disassembly from the in-box compiler, with fxc's /O3 /Ges /WX /Qstrip_*."""
    try:
        library = ctypes.WinDLL(str(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "d3dcompiler_47.dll"))
    except (OSError, AttributeError):
        raise ToolError("neither fxc.exe nor d3dcompiler_47.dll was found (set MAICORD_FXC_EXECUTABLE)") from None
    data = source.encode("utf-8")
    code, errors = ctypes.c_void_p(), ctypes.c_void_p()
    flags = 0x8000 | 0x800 | 0x40000  # OPTIMIZATION_LEVEL3, ENABLE_STRICTNESS, WARNINGS_ARE_ERRORS
    library.D3DCompile.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
                                   ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
    library.D3DCompile.restype = ctypes.c_long
    status = library.D3DCompile(data, len(data), name.encode(), None, None, entry.encode(), profile.encode(), flags, 0,
                                ctypes.byref(code), ctypes.byref(errors))
    if status < 0 or not code.value:
        message = ""
        if errors.value:
            blob = _Blob(errors)
            message = blob.read().decode("utf-8", "replace").replace("\0", "")
            blob.release()
        raise ToolError(f"the shader did not compile:\n{message.strip() or hex(status & 0xFFFFFFFF)}")
    blob = _Blob(code)
    bytecode = blob.read()
    blob.release()
    if errors.value:
        _Blob(errors).release()
    for function in (library.D3DDisassemble, library.D3DStripShader):
        function.restype = ctypes.c_long
    text = ctypes.c_void_p()
    library.D3DDisassemble.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p,
                                       ctypes.POINTER(ctypes.c_void_p)]
    library.D3DDisassemble(bytecode, len(bytecode), 0, None, ctypes.byref(text))
    assembly = ""
    if text.value:
        blob = _Blob(text)
        assembly = blob.read().decode("utf-8", "replace")
        blob.release()
    stripped = ctypes.c_void_p()
    library.D3DStripShader.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)]
    if library.D3DStripShader(bytecode, len(bytecode), 0x1 | 0x2, ctypes.byref(stripped)) >= 0 and stripped.value:
        blob = _Blob(stripped)
        bytecode = blob.read()
        blob.release()
    return bytecode, assembly


def compile_fxc(fxc: str, hlsl: Path, include: Path, work: Path, entry: str = "main",
                profile: str = "ps_5_0") -> tuple[bytes, str]:
    work.mkdir(parents=True, exist_ok=True)
    stem = hlsl.stem if entry == "main" else f"{hlsl.stem}.{entry}"
    cso, asm = work / (stem + ".cso"), work / (stem + ".asm")
    result = subprocess.run([fxc, "/nologo", "/T", profile, "/E", entry, "/O3", "/Ges", "/WX", "/I", str(include),
                             "/Qstrip_debug", "/Qstrip_reflect", "/Fo", str(cso), "/Fc", str(asm), str(hlsl)],
                            capture_output=True, text=True, errors="replace")
    if result.returncode:
        raise ToolError(f"fxc failed for {hlsl.name} ({entry}):\n{(result.stdout + result.stderr).strip()}")
    code, assembly = cso.read_bytes(), asm.read_text(encoding="utf-8", errors="replace")
    cso.unlink()
    asm.unlink()
    return code, assembly


def compile_effect(hlsl: Path, include: Path, work: Path, compiler: str = "auto", entry: str = "main",
                   profile: str = "ps_5_0") -> tuple[bytes, str, str]:
    """(bytecode, disassembly, which compiler) of one entry point."""
    fxc = find_fxc() if compiler in ("auto", "fxc") else None
    if compiler == "fxc" and not fxc:
        raise ToolError("fxc.exe was not found (set MAICORD_FXC_EXECUTABLE or install the Windows SDK)")
    if fxc:
        return (*compile_fxc(fxc, hlsl, include, work, entry, profile), "fxc")
    source = expand_includes(hlsl.read_text(encoding="utf-8"), include, hlsl.name)
    return (*compile_d3dcompiler(source, hlsl.name, entry, profile), "d3dcompiler_47")


def check_bytecode(code: bytes, profile: str = "ps_5_0") -> None:
    if len(code) > DXBC_MAX:
        raise ToolError(f"the bytecode is {len(code)} bytes, over {DXBC_MAX}")
    if code[:4] != b"DXBC":
        raise ToolError("the compiler did not produce DXBC")
    count = struct.unpack_from("<I", code, 28)[0]
    for index in range(count):
        offset = struct.unpack_from("<I", code, 32 + 4 * index)[0]
        tag, _ = struct.unpack_from("<4sI", code, offset)
        if tag in (b"SHEX", b"SHDR"):
            if struct.unpack_from("<I", code, offset + 8)[0] != VERSION_TOKENS[profile]:
                raise ToolError(f"the shader is not {profile}")
            return
    raise ToolError("the bytecode has no shader chunk")


def lint(assembly: str, textures: int = 4) -> dict:
    """The static part of the catalog's gate, from the disassembly: only b0, t0-t4 (t0-t10 at interface 3: TEXTURE2/3
    and the graph's targets) and s0-s2; no UAVs; at most 4,096 instructions and 64 texture reads. Vertex shaders too."""
    instructions = samples = 0
    for raw in assembly.splitlines():
        line = raw.strip()
        if not line or line.startswith("//") or line.startswith("ps_") or line.startswith("vs_"):
            continue
        opcode = line.split()[0]
        if opcode.startswith("dcl_"):
            if "uav" in opcode or opcode in ("dcl_resource_texture3d", "dcl_resource_structured", "dcl_resource_raw"):
                raise ToolError(f"the shader declares {opcode}: unordered-access and 3D resources are not bound")
            if opcode == "dcl_constantbuffer" and not re.search(r"\bcb0\b", line, re.I):
                raise ToolError("only constant buffer b0 is bound")
            if opcode.startswith("dcl_resource") and not any(int(t) <= textures for t in re.findall(r"\bt(\d+)\b", line)):
                raise ToolError(f"only textures t0-t{textures} are bound")
            if opcode == "dcl_sampler" and not re.search(r"\bs[0-2]\b", line):
                raise ToolError("only samplers s0-s2 are bound")
            continue
        if opcode == "ret":
            continue
        instructions += 1
        if opcode.split("_")[0] in ("sample", "ld", "gather4", "lod"):
            samples += 1
    if instructions > INSTRUCTIONS_MAX:
        raise ToolError(f"{instructions} instructions, over the catalog's {INSTRUCTIONS_MAX}")
    if samples > SAMPLES_MAX:
        raise ToolError(f"{samples} texture reads, over the catalog's {SAMPLES_MAX}")
    return {"instructions": instructions, "samples": samples}


def cost_hint(counts: dict) -> str:
    """A static guess; the catalog measures the real class on WARP."""
    share = max(counts["instructions"] / INSTRUCTIONS_MAX, counts["samples"] / SAMPLES_MAX)
    return "likely light" if share < 0.1 else "likely medium" if share < 0.4 else "likely heavy, may be refused"


# ---- the package --------------------------------------------------------------------------------------------------
def interface_of(header: dict, data_textures: bool = False) -> int:
    """3 with a pass graph, TEXTURE2/3 or a float data texture; 2 when the effect reads mc_ambient (clock, appearance);
    else 1, so it still runs on every MaiCord."""
    if header.get("passes") or data_textures or any(u in GRAPH_USES for u in header["uses"]):
        return 3
    return 2 if any(u in AMBIENT_USES for u in header["uses"]) else INTERFACE


def manifest(slug: str, header: dict, sidecar: dict, version: str, data_textures: bool = False) -> dict:
    """The MANIFEST section, members in the order the client documents."""
    effect = header["effect"]
    result = {"interface": interface_of(header, data_textures), "slug": slug, "name": effect["name"], "version": version,
              "authors": [{"name": a.strip()} for a in effect.get("authors", "").split(";") if a.strip()] or [{"name": "Unknown"}],
              "license": effect["license"]}
    if effect.get("derived_from"):
        url, author, license_id = (effect["derived_from"].split("|") + ["", ""])[:3]
        result["derived_from"] = {"url": url, "author": author, "license": license_id}
    result["blend"] = header["blend"]
    result["uses"] = [u for u in USES if u in header["uses"]]
    result["resolution"] = int(sidecar.get("resolution", 50))
    result["fps"] = int(sidecar.get("fps", 24))
    result["knobs"] = header["knobs"]
    graph = graph_of(header)
    if graph:
        result["targets"], result["passes"] = graph[0], graph[1]
    return result


def texture_sections(directory: Path, uses: list) -> tuple[dict, bool]:
    """TEXTURE<N> sections from texture<N>.png or texture<N>.mcft beside the HLSL, for each declared texture; and
    whether one is float data (interface 3)."""
    sections, data = {}, False
    for index, name in enumerate(TEXTURES):
        if name not in uses:
            continue
        png, mcft = directory / f"{name}.png", directory / f"{name}.mcft"
        if png.is_file() == mcft.is_file():
            raise ToolError(f"@uses {name} needs {png.name} or {mcft.name} beside the HLSL (one of them)")
        payload = (png if png.is_file() else mcft).read_bytes()
        if len(payload) > TEXTURE_MAX:
            raise ToolError(f"{name} is {len(payload)} bytes, over {TEXTURE_MAX}")
        if mcft.is_file():
            width, height = struct.unpack_from("<HH", payload, 4) if len(payload) >= 8 else (0, 0)
            if payload[:4] != b"MCFT" or not (0 < width <= 512 and 0 < height <= 512) or len(payload) != 8 + 16 * width * height:
                raise ToolError(f"{mcft.name} must be 'MCFT', u16 width and height (1..512), then width x height RGBA32F texels")
            data = True
        else:
            width, height, depth, colour = struct.unpack_from(">IIBB", payload, 16) if payload[:8] == b"\x89PNG\r\n\x1a\n" else (0,) * 4
            if not (0 < width <= 512 and 0 < height <= 512) or depth != 8 or colour not in (2, 6):
                raise ToolError(f"{png.name} must be an 8-bit RGB or RGBA PNG of at most 512x512")
        sections[name.upper()] = payload
    return sections, data


def float_texture(width: int, height: int, texels: list) -> bytes:
    """A float data texture (texture<N>.mcft): `texels` is width x height (r, g, b, a) tuples, rows top first."""
    return struct.pack("<4sHH", b"MCFT", width, height) + b"".join(struct.pack("<4f", *texel) for texel in texels)


def pack_mcfx(sections: dict) -> bytes:
    names = sorted(sections, key=lambda n: SECTION[n])
    if "MANIFEST" not in sections or "DXBC" not in sections:
        raise ToolError("a package needs MANIFEST and DXBC")
    offset = 16 + 16 * len(names)
    table, body = b"", b""
    for name in names:
        data = sections[name]
        table += struct.pack("<IIII", SECTION[name], offset + len(body), len(data), 0)
        body += data
    total = offset + len(body)
    if total > PACKAGE_MAX:
        raise ToolError(f"the package is {total} bytes, over {PACKAGE_MAX}")
    return struct.pack("<4sHHII", b"MCFX", 1, len(names), total, 0) + table + body


def sidecar_text(header: dict, sidecar: dict) -> str:
    lines = [f"name={header['effect']['name']}"]
    if header["uses"]:
        lines.append("uses=" + " ".join(u for u in USES if u in header["uses"]))
    lines.append(f"blend={header['blend']}")
    lines.append(f"resolution={sidecar.get('resolution', 50)}")
    lines.append(f"fps={sidecar.get('fps', 24)}")
    # Every mc_knobs value, as far as the format goes (the catalog checks all of them).
    values = knob_values(header["knobs"])
    lines.append("knobs=" + " ".join(str(round(v * 1000)) for v in values))
    # A pass graph as the manifest holds it (compact JSON), so the catalog can check the two agree.
    graph = graph_of(header)
    if graph:
        lines.append("targets=" + json.dumps(graph[0], separators=(",", ":")))
        lines.append("passes=" + json.dumps(graph[1], separators=(",", ":")))
    return "\n".join(lines) + "\n"


class Effect:
    def __init__(self, directory: str | None, source: str | None = None):
        self.directory = Path(directory or ".").resolve()
        slug = core.folder_slug(self.directory)
        self.hlsl = Path(source) if source else self.directory / f"{slug}.hlsl"
        if not self.hlsl.is_file():
            candidates = sorted(self.directory.glob("*.hlsl"))
            if source or len(candidates) != 1:
                raise ToolError(f"{self.hlsl} was not found (run this in an effect folder, or use --dir; "
                                f"'maicord-plugin effect new <name>' makes one)")
            self.hlsl = candidates[0]
        self.slug = self.hlsl.stem
        if not core.slug_valid(self.slug):
            raise ToolError(f"{self.hlsl.name} makes no slug: use lower-case letters, digits and '-'")
        self.sidecar = self.hlsl.with_suffix(".effect")
        self.output = self.directory / "build" / f"{self.slug}.mcfx"


def build_effect(effect: Effect, include: Path, compiler: str = "auto") -> dict:
    source = effect.hlsl.read_text(encoding="utf-8")
    header = parse_header(source)
    name = header["effect"]["name"]
    if len(name.encode("utf-8")) > NAME_MAX:
        raise ToolError(f"the effect name is at most {NAME_MAX} bytes")
    version = header["effect"].get("version", "0.1.0")
    if not core.VERSION.fullmatch(version):
        raise ToolError(f"version {version!r} must be MAJOR.MINOR.PATCH")
    existing = parse_sidecar(effect.sidecar.read_text(encoding="utf-8")) if effect.sidecar.is_file() else {}
    if int(existing.get("resolution", 50)) not in RESOLUTIONS or int(existing.get("fps", 24)) not in FRAME_RATES:
        raise ToolError(f"resolution is one of {RESOLUTIONS} and fps one of {FRAME_RATES} in {effect.sidecar.name}")
    assign_slots(header["knobs"])
    check_defines(source, header["knobs"])
    work = effect.directory / "build"
    work.mkdir(exist_ok=True)
    graph = graph_of(header)
    textures, data = texture_sections(effect.directory, header["uses"])
    interface = interface_of(header, data)
    # Every shader is compiled and linted on its own; a graph's are counted together for the cost hint.
    codes, counts, used = [], {"instructions": 0, "samples": 0}, ""
    for entry, profile in graph[2] if graph else [("main", "ps_5_0")]:
        code, assembly, used = compile_effect(effect.hlsl, include, work, compiler, entry, profile)
        check_bytecode(code, profile)
        for key, value in lint(assembly, 10 if interface == 3 else 4).items():
            counts[key] += value
        codes.append(code)
    write_text(effect.sidecar, sidecar_text(header, existing))
    document = manifest(effect.slug, header, existing, version, data)
    text = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    source_bytes = source.encode("utf-8")
    if len(text) > MANIFEST_MAX or len(source_bytes) > SOURCE_MAX or sum(map(len, codes[1:])) > SHADERS_MAX:
        raise ToolError("the manifest, the source or the graph's shaders are too large")
    sections = {"MANIFEST": text, "DXBC": codes[0], "SOURCE": source_bytes, **textures}
    if len(codes) > 1:
        sections["SHADERS"] = b"".join(codes[1:])
    package = pack_mcfx(sections)
    effect.output.write_bytes(package)
    for leftover in (work / f"{effect.slug}.cso", work / f"{effect.slug}.asm"):
        leftover.unlink(missing_ok=True)
    return {"package": package, "manifest": document, "lint": counts, "compiler": used, "name": name,
            "shaders": len(codes)}


# ---- templates --------------------------------------------------------------------------------------------------
HLSL_TEMPLATE = '''\
// {name}: a soft drifting gradient between the theme's background and accent colours.
//
// An effect is the function background(pixel) at the bottom. Knobs and metadata are the comment lines below;
// `maicord-plugin effect build` reads them, compiles this file and writes the .mcfx package and the
// {slug}.effect sidecar.
//
// @effect   name="{name}" authors="{author}" license="MPL-2.0" version="0.1.0"
// @knob speed float 0..2 = 0.4 "Drift speed"
// @knob mix   float 0..1 = 0.35 "Colour strength"
// @uses blue_noise
// @blend normal
#include <maicord/sdk/background.hlsli>

// One define per knob, reading the slot the tool assigns (a mistake is reported with the right line).
#define knob_speed (mc_knobs[0].x)
#define knob_mix   (mc_knobs[0].y)

float4 background(float2 pixel) {{
    const float2 uv = mc_uv(pixel);
    const float t = mc.time * knob_speed;
    const float wave = 0.5 + 0.5 * sin(uv.x * 3.0 + t) * cos(uv.y * 2.0 - t * 0.7);
    const float3 colour = lerp(mc_role(MC_ROLE_BACKGROUND), mc_role(MC_ROLE_ACCENT), wave * knob_mix * mc.intensity);
    return float4(colour + mc_dither(pixel), 1.0);
}}
'''

EFFECT_README = '''\
# {name}

A MaiCord background effect. `{slug}.hlsl` is the whole effect.

```
maicord-plugin effect build      compile it, lint it, write build\\{slug}.mcfx and {slug}.effect
maicord-plugin effect preview    copy the .mcfx into MaiCord's Developer Mode folder (--watch rebuilds on save)
```

In MaiCord turn on Developer Mode, open Settings > Effects > Developer, and pick the effect in
Settings > Appearance > Background effect. It reloads when the file changes and keeps your knob values.
The catalog measures a cost class on WARP; `effect build` prints the static counts it limits.
'''


def command_effect_new(arguments) -> int:
    slug = core.slugify(arguments.name)
    name = core.display_name(arguments.name)
    directory = Path(arguments.dir) if arguments.dir else Path.cwd() / slug
    if directory.exists() and any(directory.iterdir()):
        raise ToolError(f"{directory} already exists and is not empty")
    directory.mkdir(parents=True, exist_ok=True)
    author = (arguments.author or "Your Name").replace('"', "")
    values = dict(name=name, slug=slug, author=author)
    write_text(directory / f"{slug}.hlsl", HLSL_TEMPLATE.format(**values))
    write_text(directory / "README.md", EFFECT_README.format(**values))
    write_text(directory / ".gitignore", "build/\n")
    effect = Effect(str(directory))
    header = parse_header(effect.hlsl.read_text(encoding="utf-8"))
    write_text(effect.sidecar, sidecar_text(header, {}))
    say(f"Created {directory}.")
    say(f"Next: cd {directory.name}; maicord-plugin effect build; maicord-plugin effect preview")
    return 0


def command_effect_build(arguments) -> int:
    effect = Effect(arguments.dir, arguments.source)
    result = build_effect(effect, core.sdk_include(arguments.sdk), arguments.compiler)
    counts = result["lint"]
    shaders = f" in {result['shaders']} shaders" if result["shaders"] > 1 else ""
    say(f"{result['name']}: {len(result['package']):,} bytes, {counts['instructions']} instructions, "
        f"{counts['samples']} texture reads{shaders} ({cost_hint(counts)}; the catalog measures the class on WARP).")
    say(f"Built {effect.output} with {result['compiler']}; {effect.sidecar.name} is up to date.")
    return 0


# ---- simulated inputs --------------------------------------------------------------------------------------------
_TIME = re.compile(r"([01]\d|2[0-3]):([0-5]\d)")
_DATE = re.compile(r"(0[1-9]|1[0-2])-(\d\d)")
_MONTH_DAYS = (31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)
SIMULATION_KEYS = ("time", "date", "day_seconds", "appearance")


def simulation(arguments, existing: dict) -> dict | None:
    """simulate-inputs.txt after this run's --time, --date, --day-seconds and --appearance, merged over the file's
    keys (None: no option given, the file stays as it is). The same grammar MaiCord reads."""
    given = {}
    if arguments.time is not None:
        if not _TIME.fullmatch(arguments.time):
            raise ToolError("--time is HH:MM, 00:00 to 23:59")
        given["time"] = arguments.time
    if arguments.date is not None:
        match = _DATE.fullmatch(arguments.date)
        if not match or not 1 <= int(match.group(2)) <= _MONTH_DAYS[int(match.group(1)) - 1]:
            raise ToolError("--date is MM-DD, a day of the year")
        given["date"] = arguments.date
    if arguments.day_seconds is not None:
        if not 10 <= arguments.day_seconds <= 86400:
            raise ToolError("--day-seconds is 10 to 86400 (seconds for a whole simulated day)")
        given["day_seconds"] = str(arguments.day_seconds)
    if arguments.appearance is not None:
        given["appearance"] = arguments.appearance
    if not given:
        return None
    values = {k: v for k, v in existing.items() if k in SIMULATION_KEYS}
    values.update(given)
    if "day_seconds" in values and "time" not in values:
        values["time"] = datetime.datetime.now().strftime("%H:%M")  # a running day starts from now
    return values


def simulation_text(values: dict) -> str:
    lines = ["# Simulated effect inputs (maicord-plugin effect preview); MaiCord reads them in Developer Mode."]
    lines += [f"{key}={values[key]}" for key in SIMULATION_KEYS if key in values]
    return "\n".join(lines) + "\n"


def simulation_summary(values: dict) -> str:
    parts = []
    if "time" in values:
        parts.append(f"time {values['time']}" + (f", a day every {values['day_seconds']} s" if "day_seconds" in values else ""))
    if "date" in values:
        parts.append(f"date {values['date']}")
    if "appearance" in values:
        parts.append(f"{values['appearance']} appearance")
    return ", ".join(parts)


def apply_simulation(arguments, folder: Path, header: dict) -> None:
    """Writes, keeps or removes the dev folder's simulate-inputs.txt and says what every effect now sees."""
    path = folder / SIMULATION_FILE
    if arguments.real_inputs:
        if path.exists():
            path.unlink()
            say("Removed the simulated inputs: effects see the real clock and appearance again.")
        return
    existing = parse_sidecar(path.read_text(encoding="utf-8")) if path.is_file() else {}
    values = simulation(arguments, existing)
    if values is None:
        if existing:
            say(f"MaiCord simulates {simulation_summary(existing)} for every effect (--real-inputs stops it).")
        return
    folder.mkdir(parents=True, exist_ok=True)
    write_text(path, simulation_text(values))
    say(f"MaiCord simulates {simulation_summary(values)} for every effect, in Developer Mode; a change glides "
        "like a real one (--real-inputs stops it).")
    unused = [use for keys, use in ((("time", "date", "day_seconds"), "clock"), (("appearance",), "appearance"))
              if any(k in values for k in keys) and use not in header["uses"]]
    if unused:
        say(f"Note: the effect does not declare @uses {' '.join(unused)}, so it reads 0 there.")


def command_effect_preview(arguments) -> int:
    effect = Effect(arguments.dir, arguments.source)
    include = core.sdk_include(arguments.sdk)
    folder = core.dev_folder("effect", arguments.dev_folder)

    def step() -> None:
        build_effect(effect, include, arguments.compiler)
        core.deploy(effect.output, folder)

    step()
    apply_simulation(arguments, folder, parse_header(effect.hlsl.read_text(encoding="utf-8")))
    if not arguments.watch:
        say("In MaiCord (Developer Mode): Settings > Appearance > Background effect, or Settings > Effects > Developer.")
        return 0
    say("Watching for changes; Ctrl+C stops.")
    core.watch(lambda: [effect.hlsl, *effect.directory.glob("*.hlsli"), *effect.directory.glob("*.png"),
                        *effect.directory.glob("*.mcft")], step,
               interval=0.3, rounds=arguments.rounds)
    return 0


def add_commands(effect_parser, common) -> None:
    """Registers `effect new|build|preview` on the `effect` subparser (common adds --sdk, --dir, --source)."""
    commands = effect_parser.add_subparsers(dest="effect_command", required=True)

    def compiler(p):
        p.add_argument("--compiler", choices=("auto", "fxc", "d3dcompiler"), default="auto",
                       help="auto: fxc.exe when the Windows SDK has it, else the in-box d3dcompiler_47.dll")

    p = commands.add_parser("new", help="create an effect project")
    p.add_argument("name", help="the effect's name, e.g. aurora or \"Soft Rain\"")
    p.add_argument("--dir", help="where to create it (default: ./<slug>)")
    p.add_argument("--author", help="the author named in @effect")
    p.set_defaults(run=command_effect_new)

    p = commands.add_parser("build", help="compile, lint and pack build/<slug>.mcfx")
    common(p)
    compiler(p)
    p.set_defaults(run=command_effect_build)

    p = commands.add_parser("preview", help="build and copy the .mcfx into the Developer Mode folder")
    common(p)
    compiler(p)
    p.add_argument("--dev-folder", help="default: MAICORD_EFFECT_DEV or %%LOCALAPPDATA%%\\MaiCord\\effects\\dev")
    p.add_argument("--watch", action="store_true", help="rebuild and copy whenever the HLSL changes")
    p.add_argument("--rounds", type=int, help=argparse.SUPPRESS)
    simulated = p.add_argument_group("simulated inputs", "what every effect reads for @uses clock and appearance, "
                                     "kept in the dev folder until --real-inputs; a change glides as a real one does")
    simulated.add_argument("--time", metavar="HH:MM", help="the local time of day")
    simulated.add_argument("--date", metavar="MM-DD", help="the local date (month and day)")
    simulated.add_argument("--day-seconds", type=int, metavar="N", help="a whole day passes in N seconds, from --time")
    simulated.add_argument("--appearance", choices=("dark", "light"), help="the light or dark appearance")
    simulated.add_argument("--real-inputs", action="store_true", help="back to the real clock and appearance")
    p.set_defaults(run=command_effect_preview)
