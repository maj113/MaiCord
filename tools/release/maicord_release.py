#!/usr/bin/env python3
"""Package, describe and sign MaiCord update artifacts.

The formats here mirror the client's verifiers exactly:
  * src/app/update_pair_package.cpp - a ZIP with exactly two stored members,
    MaiCord.exe then MaiCordAccount.dll, no flags/extra fields/comments.
  * src/app/update_manifest.cpp - a compact JSON document with ten fields,
    signed as SHA-256("MaiCord update manifest v1\\n" || document) with
    ECDSA P-256, stored as a raw 64-byte P1363 (r || s) signature.

Run `python tools/release/maicord_release.py --help` for the subcommands.
Only the Python standard library and Windows CNG are used.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cng_p256  # noqa: E402

SIGNATURE_DOMAIN = b"MaiCord update manifest v1\n"
INVENTORY_DOMAIN = b"MaiCord publisher inventory v1\n"
MANIFEST_MAXIMUM_BYTES = 4096
PACKAGE_MAXIMUM_BYTES = 0xFFFFFFFE
MEMBER_NAMES = ("MaiCord.exe", "MaiCordAccount.dll")
CHANNELS = ("stable", "preview")
DEFAULT_VALIDITY_DAYS = 90
UPDATES_DIRECTORY = "updates"
CLOCK_SKEW_SECONDS = 600
TAG_PATTERN = re.compile(r"^v(0|[1-9]\d{0,2})\.(0|[1-9]\d{0,2})\.(0|[1-9]\d{0,2})(?:-preview\.([1-9]\d{0,2}))?$")
MANIFEST_FIELDS = ("format", "product", "arch", "channel", "version", "issued", "expires", "bytes", "sha256",
                   "artifact")
STABLE_SEQUENCE = 999


class ReleaseError(RuntimeError):
    pass


# --------------------------------------------------------------------------- versions

@dataclass(frozen=True)
class ReleaseVersion:
    tag: str
    semver: str
    major: int
    minor: int
    patch: int
    preview: int | None

    @property
    def channel(self) -> str:
        return "preview" if self.preview else "stable"

    @property
    def number(self) -> int:
        """Monotonic u64 compared by the updater's rollback floor.

        ((major * 1000 + minor) * 1000 + patch) * 1000 + sequence, where the
        sequence is the preview number (1-998) or 999 for the stable release, so
        v1.2.3-preview.4 = 1002003004 < v1.2.3 = 1002003999 < v1.2.4-preview.1.
        """
        sequence = self.preview if self.preview else STABLE_SEQUENCE
        return ((self.major * 1000 + self.minor) * 1000 + self.patch) * 1000 + sequence

    @property
    def artifact_name(self) -> str:
        return f"MaiCord-x64-{self.number}.zip"


def parse_tag(tag: str) -> ReleaseVersion:
    match = TAG_PATTERN.match(tag.strip())
    if not match:
        raise ReleaseError(f"Tag {tag!r} is not vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-preview.N "
                           "(components 0-999, preview 1-998).")
    major, minor, patch = (int(match.group(i)) for i in (1, 2, 3))
    preview = int(match.group(4)) if match.group(4) else None
    if preview is not None and preview >= STABLE_SEQUENCE:
        raise ReleaseError("Preview numbers must be between 1 and 998.")
    return ReleaseVersion(tag.strip(), tag.strip()[1:], major, minor, patch, preview)


def cmake_project_version(repository: Path) -> str:
    text = (repository / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    match = re.search(r"project\(\s*MaiCord\s+VERSION\s+(\d+\.\d+\.\d+)", text)
    if not match:
        raise ReleaseError("CMakeLists.txt has no project(MaiCord VERSION x.y.z).")
    return match.group(1)


# --------------------------------------------------------------------------- PE sanity

def check_pe(data: bytes, dll: bool, name: str) -> None:
    """Cheap publisher-side subset of the client's offline PE checks."""
    def fail(reason: str):
        raise ReleaseError(f"{name}: {reason}")
    if len(data) < 64 or data[:2] != b"MZ":
        fail("missing MZ header")
    pe = struct.unpack_from("<I", data, 60)[0]
    if pe < 64 or pe + 24 + 240 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        fail("missing PE header")
    machine, = struct.unpack_from("<H", data, pe + 4)
    optional_size, characteristics = struct.unpack_from("<HH", data, pe + 20)
    magic, = struct.unpack_from("<H", data, pe + 24)
    if machine != 0x8664 or optional_size != 240 or magic != 0x20B:
        fail("not an AMD64 PE32+ image")
    if not characteristics & 0x0002:
        fail("not marked executable")
    if bool(characteristics & 0x2000) != dll:
        fail("expected a DLL" if dll else "expected an EXE, not a DLL")


# --------------------------------------------------------------------------- update ZIP

DOS_TIME = 0
DOS_DATE = (0 << 9) | (1 << 5) | 1  # 1980-01-01, fixed for reproducible packages
VERSION_NEEDED = 20
VERSION_MADE_BY = 20  # host 0 (MS-DOS/FAT), spec 2.0
EXTERNAL_ATTRIBUTES = 0x20  # FILE_ATTRIBUTE_ARCHIVE


def build_update_zip(executable: Path, account: Path, output: Path) -> None:
    members = []
    for path, name, dll in ((executable, MEMBER_NAMES[0], False), (account, MEMBER_NAMES[1], True)):
        if path.name.lower() != name.lower():
            raise ReleaseError(f"Expected {name}, got {path}.")
        data = path.read_bytes()
        if not data:
            raise ReleaseError(f"{path} is empty.")
        check_pe(data, dll, name)
        members.append((name.encode("ascii"), data))
    body = bytearray()
    central = bytearray()
    for name, data in members:
        offset = len(body)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        body += struct.pack("<IHHHHHIIIHH", 0x04034B50, VERSION_NEEDED, 0, 0, DOS_TIME, DOS_DATE, crc,
                            len(data), len(data), len(name), 0) + name + data
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, VERSION_MADE_BY, VERSION_NEEDED, 0, 0,
                               DOS_TIME, DOS_DATE, crc, len(data), len(data), len(name), 0, 0, 0, 0,
                               EXTERNAL_ATTRIBUTES, offset) + name
    end = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 2, 2, len(central), len(body), 0)
    package = bytes(body + central + end)
    if len(package) > PACKAGE_MAXIMUM_BYTES:
        raise ReleaseError("The update package exceeds the 4 GiB ZIP limit accepted by the client.")
    validate_update_zip(package)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(package)


def validate_update_zip(package: bytes) -> None:
    """Mirror of update_pair_package.cpp's structural ZIP checks (not PE/ordinal)."""
    def fail(reason: str):
        raise ReleaseError(f"Update ZIP rejected: {reason}.")
    if len(package) < 22 or len(package) > PACKAGE_MAXIMUM_BYTES:
        fail("size")
    signature, disk, disk_cd, count_disk, count, cd_bytes, cd_offset, comment = struct.unpack_from(
        "<IHHHHIIH", package, len(package) - 22)
    if (signature != 0x06054B50 or disk or disk_cd or count_disk != 2 or count != 2 or comment or
            cd_offset > len(package) - 22 or cd_bytes != len(package) - 22 - cd_offset):
        fail("end of central directory")
    next_local, next_central = 0, cd_offset
    for name in MEMBER_NAMES:
        raw_name = name.encode("ascii")
        local = struct.unpack_from("<IHHHHHIIIHH", package, next_local)
        record = struct.unpack_from("<IHHHHHHIIIHHHHHII", package, next_central)
        (l_sig, l_version, l_flags, l_method, l_time, l_date, l_crc, l_packed, l_bytes, l_name, l_extra) = local
        (c_sig, made, c_version, c_flags, c_method, c_time, c_date, c_crc, c_packed, c_bytes, c_name, c_extra,
         c_comment, c_disk, c_internal, c_external, c_offset) = record
        if (l_sig != 0x04034B50 or l_version not in (10, 20) or l_flags or l_method or not l_bytes or
                l_bytes == 0xFFFFFFFF or l_packed != l_bytes or l_name != len(raw_name) or l_extra or
                c_sig != 0x02014B50 or (made >> 8) not in (0, 10) or (made & 0xFF) > 20 or
                c_version != l_version or c_flags or c_method or c_time != l_time or c_date != l_date or
                c_crc != l_crc or c_packed != l_bytes or c_bytes != l_bytes or c_name != len(raw_name) or
                c_extra or c_comment or c_disk or c_internal or c_external & ~(0x20 | 0x80) or
                c_offset != next_local):
            fail(f"{name} header")
        data = next_local + 30 + len(raw_name)
        if data > cd_offset or l_bytes > cd_offset - data:
            fail(f"{name} bounds")
        if (package[next_local + 30:data] != raw_name or
                package[next_central + 46:next_central + 46 + len(raw_name)] != raw_name):
            fail(f"{name} name/order")
        if zlib.crc32(package[data:data + l_bytes]) & 0xFFFFFFFF != l_crc:
            fail(f"{name} CRC")
        next_local = data + l_bytes
        next_central += 46 + len(raw_name)
    if next_local != cd_offset or next_central != len(package) - 22:
        fail("trailing or overlapping data")


# --------------------------------------------------------------------------- manifest

def render_manifest(version: int, channel: str, issued: int, expires: int, artifact_bytes: int,
                    artifact_sha256: str, artifact_name: str) -> bytes:
    if channel not in CHANNELS:
        raise ReleaseError(f"Unknown channel {channel!r}.")
    if not re.fullmatch(r"[0-9a-f]{64}", artifact_sha256):
        raise ReleaseError("The artifact digest must be 64 lowercase hex digits.")
    if not (0 < issued < expires) or not (0 < version < 2 ** 64) or not (0 < artifact_bytes <= PACKAGE_MAXIMUM_BYTES):
        raise ReleaseError("Manifest numbers are out of range.")
    if artifact_name != f"MaiCord-x64-{version}.zip":
        raise ReleaseError("The artifact name must be MaiCord-x64-<version>.zip.")
    # Field order and compact separators are fixed; strings never need escapes.
    document = (f'{{"format":1,"product":"MaiCord","arch":"x64","channel":"{channel}","version":{version},'
                f'"issued":{issued},"expires":{expires},"bytes":{artifact_bytes},"sha256":"{artifact_sha256}",'
                f'"artifact":"{artifact_name}"}}').encode("ascii")
    if len(document) > MANIFEST_MAXIMUM_BYTES:
        raise ReleaseError("The manifest exceeds 4096 bytes.")
    return document


def parse_manifest(document: bytes) -> dict:
    """Strict reading equivalent to update_manifest.cpp's field rules."""
    if not document or len(document) > MANIFEST_MAXIMUM_BYTES:
        raise ReleaseError("Manifest size is invalid.")
    if b"\\" in document:
        raise ReleaseError("Manifest strings must not contain escapes.")

    def pairs(items):
        names = [name for name, _ in items]
        if len(names) != len(set(names)):
            raise ReleaseError("Manifest has duplicate fields.")
        return dict(items)
    try:
        fields = json.loads(document.decode("utf-8"), object_pairs_hook=pairs)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ReleaseError(f"Manifest is not valid JSON: {error}.") from None
    if not isinstance(fields, dict) or set(fields) != set(MANIFEST_FIELDS):
        raise ReleaseError(f"Manifest fields must be exactly {', '.join(MANIFEST_FIELDS)}.")
    integers = ("format", "version", "issued", "expires", "bytes")
    if any(type(fields[name]) is not int or not 0 <= fields[name] < 2 ** 64 for name in integers):
        raise ReleaseError("Manifest numeric fields must be unsigned 64-bit integers.")
    if (fields["format"] != 1 or fields["product"] != "MaiCord" or fields["arch"] != "x64" or
            fields["channel"] not in CHANNELS or not isinstance(fields["sha256"], str) or
            not re.fullmatch(r"[0-9a-f]{64}", fields["sha256"]) or not fields["version"] or
            not fields["issued"] or fields["expires"] <= fields["issued"] or not fields["bytes"] or
            fields["artifact"] != f"MaiCord-x64-{fields['version']}.zip"):
        raise ReleaseError("Manifest field values are invalid.")
    return fields


def manifest_digest(document: bytes) -> bytes:
    return hashlib.sha256(SIGNATURE_DOMAIN + document).digest()


def inventory_fingerprint(public_point: bytes) -> str:
    return hashlib.sha256(INVENTORY_DOMAIN + public_point).hexdigest()


def load_private_key(arguments) -> cng_p256.PrivateKey:
    if arguments.key_file:
        text = Path(arguments.key_file).read_text(encoding="utf-8")
    else:
        text = os.environ.get(arguments.key_env, "")
        if not text.strip():
            raise ReleaseError(f"Set the {arguments.key_env} environment variable or pass --key-file.")
    return cng_p256.PrivateKey.from_jwk(text)


def sign_document(key: cng_p256.PrivateKey, document: bytes, expected_public: bytes | None) -> bytes:
    if expected_public is not None and key.public_point != expected_public:
        raise ReleaseError("The signing key does not match the expected public key "
                           "(MAICORD_UPDATE_PUBLIC_KEY). Refusing to sign.")
    signature = key.sign_digest(manifest_digest(document))
    if not cng_p256.verify_digest(key.public_point, manifest_digest(document), signature):
        raise ReleaseError("CNG produced a signature that does not verify.")
    return signature


def verify_offer(document: bytes, signature: bytes, public_point: bytes, artifact: bytes | None = None,
                 now: int | None = None) -> dict:
    if len(signature) != 64:
        raise ReleaseError("Signatures must be exactly 64 bytes (P1363 r || s).")
    if not cng_p256.verify_digest(public_point, manifest_digest(document), signature):
        raise ReleaseError("Manifest signature does not verify with the given public key.")
    fields = parse_manifest(document)
    if now is not None and not fields["issued"] <= now < fields["expires"]:
        raise ReleaseError("Manifest is not currently valid (issued/expires).")
    if artifact is not None:
        if len(artifact) != fields["bytes"] or hashlib.sha256(artifact).hexdigest() != fields["sha256"]:
            raise ReleaseError("Artifact size or SHA-256 does not match the manifest.")
        validate_update_zip(artifact)
    return fields


# --------------------------------------------------------------------------- commands

def write_outputs(values: dict) -> None:
    target = os.environ.get("GITHUB_OUTPUT")
    lines = "".join(f"{name}={value}\n" for name, value in values.items())
    if target:
        with open(target, "a", encoding="utf-8") as handle:
            handle.write(lines)
    sys.stdout.write(lines)


def command_version(arguments) -> None:
    version = parse_tag(arguments.tag)
    if arguments.check_cmake:
        project = cmake_project_version(Path(arguments.check_cmake))
        base = f"{version.major}.{version.minor}.{version.patch}"
        if project != base:
            raise ReleaseError(f"Tag {version.tag} does not match project(MaiCord VERSION {project}) in "
                               "CMakeLists.txt. Bump the project version before tagging.")
    write_outputs({"tag": version.tag, "semver": version.semver, "number": version.number,
                   "channel": version.channel, "preview": version.preview or 0,
                   "prerelease": str(bool(version.preview)).lower(),
                   "artifact": version.artifact_name})


def signed_manifest(arguments, key, expected, version: int, channel: str, artifact_bytes: int, digest: str,
                    output_dir: Path, issued: int) -> None:
    expires = issued + arguments.validity_days * 86400
    document = render_manifest(version, channel, issued, expires, artifact_bytes, digest,
                               f"MaiCord-x64-{version}.zip")
    signature = sign_document(key, document, expected)
    (output_dir / f"maicord-{channel}.manifest.json").write_bytes(document)
    (output_dir / f"maicord-{channel}.manifest.json.sig").write_bytes(signature)


def command_release(arguments) -> None:
    """Build the update ZIP, sign one manifest per channel and self-verify."""
    version = parse_tag(arguments.tag)
    output_dir = Path(arguments.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    binaries = Path(arguments.binaries)
    artifact_path = output_dir / version.artifact_name
    build_update_zip(binaries / MEMBER_NAMES[0], binaries / MEMBER_NAMES[1], artifact_path)
    artifact = artifact_path.read_bytes()
    digest = hashlib.sha256(artifact).hexdigest()
    key = load_private_key(arguments)
    expected = cng_p256.parse_public_point(arguments.public_key) if arguments.public_key else None
    issued = (arguments.issued or int(time.time())) - CLOCK_SKEW_SECONDS
    # Stable releases also advance the preview channel; previews never reach stable.
    channels = ("preview",) if version.preview else CHANNELS
    for channel in channels:
        signed_manifest(arguments, key, expected, version.number, channel, len(artifact), digest, output_dir, issued)
        document = (output_dir / f"maicord-{channel}.manifest.json").read_bytes()
        signature = (output_dir / f"maicord-{channel}.manifest.json.sig").read_bytes()
        verify_offer(document, signature, expected or key.public_point, artifact, now=issued + CLOCK_SKEW_SECONDS)
    print(f"{version.artifact_name}: {len(artifact)} bytes, sha256 {digest}")
    print(f"channels: {', '.join(channels)}; publisher fingerprint {inventory_fingerprint(key.public_point)}")


def site_manifest_paths(site: Path, channel: str) -> tuple[Path, Path]:
    document = site / UPDATES_DIRECTORY / f"maicord-{channel}.manifest.json"
    return document, document.with_name(document.name + ".sig")


def command_channels(arguments) -> None:
    """Maintain the Pages site that serves the channel manifests.

    The site directory is the root of the update-channels branch and of the
    GitHub Pages deployment: updates/maicord-<channel>.manifest.json(.sig).
    --publish installs the signed manifests of a newly published release
    (a channel never moves to a lower version); --refresh-within-days re-signs
    manifests close to expiry. Every manifest left in the site must verify
    with the public key and be currently valid.
    """
    site = Path(arguments.site)
    (site / UPDATES_DIRECTORY).mkdir(parents=True, exist_ok=True)
    expected = cng_p256.parse_public_point(arguments.public_key)
    now = int(time.time())
    changed = []
    current = {}
    for channel in CHANNELS:
        document, signature = site_manifest_paths(site, channel)
        if document.exists() or signature.exists():
            try:
                current[channel] = verify_offer(document.read_bytes(), signature.read_bytes(), expected)
            except (ReleaseError, OSError) as error:
                raise ReleaseError(f"The site's {channel} manifest is invalid: {error}") from None
            if current[channel]["channel"] != channel:
                raise ReleaseError(f"The site's {channel} manifest carries the {current[channel]['channel']} channel.")
    if arguments.publish:
        source = Path(arguments.publish)
        for channel in CHANNELS:
            document = source / f"maicord-{channel}.manifest.json"
            if not document.exists():
                continue
            payload = document.read_bytes()
            signature = Path(f"{document}.sig").read_bytes()
            fields = verify_offer(payload, signature, expected, now=now)
            if fields["channel"] != channel:
                raise ReleaseError(f"{document.name} carries the {fields['channel']} channel.")
            previous = current.get(channel)
            if previous and fields["version"] < previous["version"]:
                print(f"{channel}: keeps version {previous['version']}; not moving back to {fields['version']}.")
                continue
            target, target_signature = site_manifest_paths(site, channel)
            if target.exists() and target.read_bytes() == payload and target_signature.read_bytes() == signature:
                continue
            target.write_bytes(payload)
            target_signature.write_bytes(signature)
            current[channel] = fields
            changed.append(f"{channel} -> {fields['version']}")
    if arguments.refresh_within_days is not None:
        key = None
        for channel, fields in sorted(current.items()):
            if fields["expires"] - now > arguments.refresh_within_days * 86400:
                continue
            key = key or load_private_key(arguments)
            signed_manifest(arguments, key, expected, fields["version"], channel, fields["bytes"], fields["sha256"],
                            site / UPDATES_DIRECTORY, now - CLOCK_SKEW_SECONDS)
            document, signature = site_manifest_paths(site, channel)
            current[channel] = verify_offer(document.read_bytes(), signature.read_bytes(), expected, now=now)
            changed.append(f"{channel} re-signed")
    for channel in current:
        document, signature = site_manifest_paths(site, channel)
        fields = verify_offer(document.read_bytes(), signature.read_bytes(), expected, now=now)
        print(f"{channel}: version {fields['version']}, expires in {(fields['expires'] - now) // 86400} days")
    print("changed: " + (", ".join(changed) if changed else "nothing"))
    write_outputs({"changed": str(bool(changed)).lower()})


def command_verify(arguments) -> None:
    fields = verify_offer(Path(arguments.manifest).read_bytes(), Path(arguments.signature).read_bytes(),
                          cng_p256.parse_public_point(arguments.public_key),
                          Path(arguments.artifact).read_bytes() if arguments.artifact else None,
                          now=int(time.time()) if arguments.check_time else None)
    print(json.dumps(fields, indent=2))


def command_portable(arguments) -> None:
    """Full portable ZIP for first installs: EXE/DLL pair plus fonts/ and licenses/."""
    binaries = Path(arguments.binaries)
    output = Path(arguments.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    files = [binaries / name for name in MEMBER_NAMES]
    for folder in ("fonts", "licenses"):
        directory = binaries / folder
        if not directory.is_dir():
            raise ReleaseError(f"Missing runtime asset directory {directory}.")
        files += sorted(path for path in directory.rglob("*") if path.is_file())
    repository = Path(arguments.repository) if arguments.repository else None
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in files:
            info = zipfile.ZipInfo("MaiCord/" + path.relative_to(binaries).as_posix(), (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0x20
            archive.writestr(info, path.read_bytes())
        if repository:
            for extra in ("README.md", "LICENSE"):
                if (repository / extra).is_file():
                    info = zipfile.ZipInfo(f"MaiCord/{extra}", (1980, 1, 1, 0, 0, 0))
                    info.compress_type = zipfile.ZIP_DEFLATED
                    archive.writestr(info, (repository / extra).read_bytes())
    print(f"{output.name}: {output.stat().st_size} bytes")


def git(*command: str) -> str:
    return subprocess.run(["git", *command], check=True, capture_output=True, text=True).stdout.strip()


NOTE_GROUPS = ("Features", "Fixes", "Performance", "Other")
CONVENTIONAL = re.compile(r"^(?P<type>[a-z]+)(?:\([^)]*\))?!?:\s*(?P<text>.+)$", re.IGNORECASE)
CONVENTIONAL_GROUPS = {"feat": "Features", "feature": "Features", "fix": "Fixes", "bugfix": "Fixes", "perf": "Performance"}
FIX_WORDS = re.compile(r"\b(fix\w*|crash\w*|bug\w*|broken|regression\w*|prevent\w*|restore\w*|repair\w*|never fail\w*|"
                       r"correct\w*|wrong)\b", re.IGNORECASE)
PERFORMANCE_WORDS = re.compile(r"\b(perf|performance|faster|speed\w*|latency|throughput|cpu|memory use|compile time|"
                               r"code size|startup time|optimi[sz]\w*)\b", re.IGNORECASE)
FEATURE_WORDS = re.compile(r"^(add|adds|added|support|supports|introduce\w*|implement\w*|enable\w*|allow\w*|new|show)\b",
                           re.IGNORECASE)
RELEASE_BUMP = re.compile(r"^(release|bump( version)?( to)?|version)\s+v?\d+\.\d+\.\d+\S*\s*$", re.IGNORECASE)


def classify_commit(subject: str) -> tuple[str, str] | None:
    """Returns (group, text) for a commit subject, or None for version bumps."""
    subject = subject.strip()
    if not subject or RELEASE_BUMP.match(subject):
        return None
    match = CONVENTIONAL.match(subject)
    if match:
        text = match.group("text").strip()
        return CONVENTIONAL_GROUPS.get(match.group("type").lower(), "Other"), text[:1].upper() + text[1:]
    for group, pattern in (("Fixes", FIX_WORDS), ("Performance", PERFORMANCE_WORDS), ("Features", FEATURE_WORDS)):
        if pattern.search(subject):
            return group, subject
    return "Other", subject


def render_notes(version: ReleaseVersion, previous: str | None, commits: list[tuple[str, str]], repository: str) -> str:
    """Markdown release notes from (sha, subject) pairs, newest first."""
    base = f"https://github.com/{repository}" if repository else ""
    groups: dict[str, list[str]] = {name: [] for name in NOTE_GROUPS}
    for sha, subject in commits:
        classified = classify_commit(subject)
        if not classified:
            continue
        group, text = classified
        reference = f"[`{sha[:7]}`]({base}/commit/{sha})" if base else f"`{sha[:7]}`"
        groups[group].append(f"- {text} ({reference})")
    lines = [f"## MaiCord {version.semver}", ""]
    if version.preview:
        lines += ["> Preview build for the `preview` update channel. Expect rough edges.", ""]
    if not any(groups.values()):
        lines += ["No user-visible changes.", ""]
    for name in NOTE_GROUPS:
        if groups[name]:
            lines += [f"### {name}", "", *groups[name], ""]
    if previous and base:
        lines += [f"**Full changelog:** {base}/compare/{previous}...{version.tag}", ""]
    verify = f"gh attestation verify <file> -R {repository}" if repository else "gh attestation verify <file>"
    lines += ["### Download", "",
              f"New install: download `MaiCord-{version.semver}-windows-x64.zip`, extract it anywhere and run "
              "`MaiCord.exe`. Existing installs update themselves from Settings > Updates.",
              "",
              f"`SHA256SUMS.txt` lists every asset's checksum, and `{verify}` checks its build provenance.",
              "",
              "MaiCord is an unofficial client. It is not affiliated with or endorsed by Discord, and using "
              "third-party clients may violate Discord's Terms of Service.", ""]
    return "\n".join(lines)


def command_notes(arguments) -> None:
    version = parse_tag(arguments.tag)
    tags = [tag for tag in git("tag", "--list", "v*", "--sort=-v:refname", "--merged", version.tag).splitlines()
            if tag != version.tag and TAG_PATTERN.match(tag)]
    if not version.preview:
        tags = [tag for tag in tags if "-preview." not in tag] or tags
    previous = tags[0] if tags else None
    span = f"{previous}..{version.tag}" if previous else version.tag
    log = git("log", "--no-merges", "--pretty=format:%H%x1f%s", span)
    commits = [tuple(line.split(chr(0x1f), 1)) for line in log.splitlines() if chr(0x1f) in line]
    repository = arguments.repository or os.environ.get("GITHUB_REPOSITORY", "")
    Path(arguments.output).write_text(render_notes(version, previous, commits, repository), encoding="utf-8", newline="\n")


def command_checksums(arguments) -> None:
    directory = Path(arguments.directory)
    # Release assets are immutable, including the attached channel manifests
    # (manifest-refresh.yml re-signs only the copies on the Pages site).
    lines = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}"
             for path in sorted(directory.iterdir())
             if path.is_file() and path.name != "SHA256SUMS.txt"]
    (directory / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def key_options(command):
        command.add_argument("--key-env", default="MAICORD_UPDATE_SIGNING_KEY",
                             help="environment variable holding the private JWK (default: %(default)s)")
        command.add_argument("--key-file", help="read the private JWK from a file instead (local testing)")
        command.add_argument("--validity-days", type=int, default=DEFAULT_VALIDITY_DAYS)

    command = sub.add_parser("version", help="map a release tag to the updater's version number and channel")
    command.add_argument("tag")
    command.add_argument("--check-cmake", metavar="REPOSITORY", help="require CMakeLists.txt project VERSION to match")
    command.set_defaults(run=command_version)

    command = sub.add_parser("release", help="build the update ZIP and signed channel manifests")
    command.add_argument("--tag", required=True)
    command.add_argument("--binaries", required=True, help="directory containing MaiCord.exe and MaiCordAccount.dll")
    command.add_argument("--output-dir", required=True)
    command.add_argument("--public-key", help="expected public point (hex); signing fails on mismatch")
    command.add_argument("--issued", type=int, help=argparse.SUPPRESS)
    key_options(command)
    command.set_defaults(run=command_release)

    command = sub.add_parser("channels", help="publish or re-sign the channel manifests of the Pages site")
    command.add_argument("--site", required=True, help="site root (the update-channels branch)")
    command.add_argument("--public-key", required=True)
    command.add_argument("--publish", metavar="DIRECTORY", help="install the signed manifests found here")
    command.add_argument("--refresh-within-days", type=int, help="re-sign manifests expiring within this many days")
    key_options(command)
    command.set_defaults(run=command_channels)

    command = sub.add_parser("verify", help="verify a manifest, signature and optional artifact")
    command.add_argument("--manifest", required=True)
    command.add_argument("--signature", required=True)
    command.add_argument("--public-key", required=True)
    command.add_argument("--artifact")
    command.add_argument("--check-time", action="store_true")
    command.set_defaults(run=command_verify)

    command = sub.add_parser("portable", help="build the portable first-install ZIP")
    command.add_argument("--binaries", required=True)
    command.add_argument("--output", required=True)
    command.add_argument("--repository", help="repository root, to include README.md/LICENSE")
    command.set_defaults(run=command_portable)

    command = sub.add_parser("notes", help="write release notes from commits since the previous tag")
    command.add_argument("--tag", required=True)
    command.add_argument("--output", required=True)
    command.add_argument("--repository", help="owner/repo for commit and compare links (default: $GITHUB_REPOSITORY)")
    command.set_defaults(run=command_notes)

    command = sub.add_parser("checksums", help="write SHA256SUMS.txt for every file in a directory")
    command.add_argument("directory")
    command.set_defaults(run=command_checksums)

    arguments = parser.parse_args(argv)
    try:
        arguments.run(arguments)
    except (ReleaseError, cng_p256.CngError, OSError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
