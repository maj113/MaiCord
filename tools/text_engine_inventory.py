"""Read-only design inventory; not a font validator or a DirectWrite-free audit.

Uses only Python's standard library. Reports source identities, direct API
references, font table directories, and explicitly selected artifact identities.
No font bytes or private user text are written to the report.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
from datetime import datetime, timezone


def identity(path):
    data = path.read_bytes()
    return {"path": str(path.resolve()), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def font_directory(path):
    data = path.read_bytes()

    def unpack(fmt, offset):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(data):
            raise ValueError("truncated font directory")
        return struct.unpack_from(fmt, data, offset)

    if data[:4] == b"ttcf":
        count, = unpack(">I", 8)
        if count > (len(data) - 12) // 4:
            raise ValueError("invalid collection count")
        offsets = unpack(">" + "I" * count, 12)
    else:
        offsets = (0,)
    faces = []
    for offset in offsets:
        signature, count = unpack(">4sH", offset)
        if signature not in (b"\x00\x01\x00\x00", b"OTTO", b"true", b"typ1"):
            raise ValueError("unsupported SFNT signature")
        tables = []
        for index in range(count):
            tag, checksum, start, size = unpack(">4sIII", offset + 12 + 16 * index)
            if start > len(data) or size > len(data) - start:
                raise ValueError("table outside font")
            table = {"tag": tag.decode("ascii", errors="replace"), "bytes": size}
            if tag == b"COLR" and size >= 2:
                table["version"], = unpack(">H", start)
            tables.append(table)
        faces.append({"offset": offset, "tables": tables})
    return {**identity(path), "faces": faces}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--artifact", type=Path, action="append", default=[])
    parser.add_argument("--font", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.repository.resolve()
    source_paths = [root / "CMakeLists.txt", root / "CMakePresets.json", *(root / "cmake").glob("*.cmake")]
    for directory in ("src", "include"):
        source_paths.extend(p for p in (root / directory).rglob("*")
                            if p.is_file())
    sources, references = [], []
    pattern = re.compile(r"IDWrite\w*|DWRITE_\w*|DWriteCreateFactory|\bdwrite(?:core)?\b", re.I)
    for path in sorted(source_paths):
        relative = path.relative_to(root).as_posix()
        sources.append({**identity(path), "path": relative})
        for line_number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
            matches = sorted(set(pattern.findall(line)))
            if matches:
                references.append({"path": relative, "line": line_number, "tokens": matches})
    fonts = sorted(p for p in (root / "assets" / "fonts").iterdir()
                   if p.suffix.lower() in (".ttf", ".otf", ".ttc"))
    fonts.extend(args.font)
    emoji = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts" / "seguiemj.ttf"
    if emoji.is_file():
        fonts.append(emoji)
    manifest = {
        "schema": 1,
        "captured_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "Design inventory only. Source is not proven to match selected existing binaries. "
                 "Direct references are lexical, not transitive runtime dependency evidence. "
                 "Font inspection covers table directories, not rendering validity.",
        "sources": sources,
        "directwrite_references": references,
        "directwrite_source_file_count": len({r["path"] for r in references}),
        "fonts": [font_directory(p) for p in sorted(set(fonts))],
        "artifacts": [identity(p) for p in args.artifact],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(args.output.resolve()), "source_files": len(sources),
                      "directwrite_source_files": manifest["directwrite_source_file_count"],
                      "fonts": len(manifest["fonts"]), "artifacts": len(manifest["artifacts"])}))


if __name__ == "__main__":
    main()
