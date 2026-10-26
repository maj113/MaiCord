#!/usr/bin/env python3
"""Package, describe and sign MaiCord update artifacts.

The formats here mirror the client's verifiers exactly:
  * src/app/update_package.cpp - a ZIP of exactly one stored member, MaiCord.exe,
    with no flags/extra fields/comments.
  * src/app/update_manifest.cpp - a compact JSON document with ten fields,
    signed as SHA-256("MaiCord update manifest v1\\n" || document) with
    ECDSA P-256, stored as a raw 64-byte P1363 (r || s) signature.
  * src/app/update_changelog.cpp - the channel changelog, signed the same way
    under "MaiCord update changelog v1\\n".

Run `python tools/release/maicord_release.py --help` for the subcommands.
Only the Python standard library and Windows CNG are used.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zipfile
import zlib
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cng_p256  # noqa: E402

SIGNATURE_DOMAIN = b"MaiCord update manifest v1\n"
CHANGELOG_DOMAIN = b"MaiCord update changelog v1\n"
MANIFEST_MAXIMUM_BYTES = 4096
PACKAGE_MAXIMUM_BYTES = 0xFFFFFFFE
# The client refuses larger packages (kUpdateDefaultArtifactCap).
PACKAGE_CLIENT_MAXIMUM_BYTES = 64 * 1024 * 1024
# The update protocol (kUpdateProtocol in update_manifest.hpp): the package
# layout and manifest schema. A client reads only updates/p<protocol>/ on the
# Pages site and accepts manifests whose format is its protocol.
PROTOCOL = 2
ARTIFACT_PREFIX = f"MaiCord-x64-p{PROTOCOL}-"
# The one package member, the executable the client swaps in place of itself.
PACKAGE_EXECUTABLE = "MaiCord.exe"
CHANNELS = ("stable", "preview")
DEFAULT_VALIDITY_DAYS = 90
SITE_DIRECTORY = f"updates/p{PROTOCOL}"
PREVIEW_DIRECTORY = "preview"
# Preview packages kept on Pages: the current build and the one before it.
PREVIEW_PACKAGES_KEPT = 2
CLOCK_SKEW_SECONDS = 600
TAG_PATTERN = re.compile(r"^v(0|[1-9]\d{0,2})\.(0|[1-9]\d{0,2})\.(0|[1-9]\d{0,2})$")
MANIFEST_FIELDS = ("format", "product", "arch", "channel", "version", "issued", "expires", "bytes", "sha256",
                   "artifact")
STABLE_SEQUENCE = 999
DEV_SEQUENCE_MAXIMUM = 998
# Matches src/app/update_changelog.hpp.
CHANGELOG_MAXIMUM_BYTES = 65536
CHANGELOG_MAXIMUM_ENTRIES = 100
CHANGELOG_TEXT_MAXIMUM_BYTES = 8192
CHANGELOG_FIELDS = ("format", "product", "channel", "version", "base", "entries")
CHANGELOG_ENTRY_FIELDS = ("version", "date", "commit", "text")
# Publisher-side limits, tighter than the client's.
CHANGELOG_STABLE_RELEASES = 10
CHANGELOG_SUBJECT_BYTES = 200
CHANGELOG_NOTES_BYTES = 6000


class ReleaseError(RuntimeError):
    pass


# --------------------------------------------------------------------------- versions

def version_number(major: int, minor: int, patch: int, sequence: int) -> int:
    """Monotonic u64 compared by the updater's rollback floor.

    ((major * 1000 + minor) * 1000 + patch) * 1000 + sequence: 999 for a stable
    release, 1-998 for the master build that many commits after the last
    stable tag, so 1.2.3 = 1002003999 < 1.2.4-dev.5 = 1002004005 < 1.2.4.
    """
    return ((major * 1000 + minor) * 1000 + patch) * 1000 + sequence


def version_text(number: int) -> str:
    sequence, base = number % 1000, number // 1000
    text = f"{base // 1000000}.{base // 1000 % 1000}.{base % 1000}"
    return text if sequence == STABLE_SEQUENCE else f"{text}-dev.{sequence}"


def is_stable(number: int) -> bool:
    return number % 1000 == STABLE_SEQUENCE


def artifact_name(number: int) -> str:
    return f"{ARTIFACT_PREFIX}{number}.zip"


@dataclass(frozen=True)
class ReleaseVersion:
    tag: str
    semver: str
    major: int
    minor: int
    patch: int

    @property
    def number(self) -> int:
        return version_number(self.major, self.minor, self.patch, STABLE_SEQUENCE)

    @property
    def artifact_name(self) -> str:
        return artifact_name(self.number)


@dataclass(frozen=True)
class BuildIdentity:
    """What a commit's build calls itself: a stable release or a dev build."""
    major: int
    minor: int
    patch: int
    sequence: int
    commit: str

    @property
    def number(self) -> int:
        return version_number(self.major, self.minor, self.patch, self.sequence)

    @property
    def semver(self) -> str:
        return version_text(self.number)

    @property
    def label(self) -> str:
        return self.semver if is_stable(self.number) else f"{self.semver}+{self.commit[:7]}"

    @property
    def artifact_name(self) -> str:
        return artifact_name(self.number)


def parse_tag(tag: str) -> ReleaseVersion:
    match = TAG_PATTERN.match(tag.strip())
    if not match:
        raise ReleaseError(f"Tag {tag!r} is not vMAJOR.MINOR.PATCH (components 0-999). Preview builds come "
                           "from master, not from tags.")
    major, minor, patch = (int(match.group(i)) for i in (1, 2, 3))
    return ReleaseVersion(tag.strip(), tag.strip()[1:], major, minor, patch)


def parse_project_version(text: str) -> tuple[int, int, int]:
    match = re.search(r"project\(\s*MaiCord\s+VERSION\s+(\d+)\.(\d+)\.(\d+)", text)
    if not match:
        raise ReleaseError("CMakeLists.txt has no project(MaiCord VERSION x.y.z).")
    return tuple(int(match.group(i)) for i in (1, 2, 3))


def cmake_project_version(repository: Path) -> str:
    parts = parse_project_version((repository / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace"))
    return ".".join(str(part) for part in parts)


def git(*command: str, repository: Path | str = ".") -> str:
    return subprocess.run(["git", "-C", str(repository), *command], check=True, capture_output=True,
                          encoding="utf-8", errors="replace").stdout.strip()


def stable_tags(repository: Path | str, ref: str) -> list[ReleaseVersion]:
    """Stable release tags reachable from ref, newest version first."""
    tags = [parse_tag(tag) for tag in git("tag", "--list", "v*", "--merged", ref, repository=repository).splitlines()
            if TAG_PATTERN.match(tag)]
    return sorted(tags, key=lambda tag: tag.number, reverse=True)


def next_base(release: ReleaseVersion, project: tuple[int, int, int]) -> tuple[int, int, int]:
    """Version a dev build after `release` is numbered under: the next patch,
    or the CMake project version when it was already bumped further."""
    following = (release.major, release.minor, release.patch + 1)
    if following[2] > 999:
        following = (release.major, release.minor + 1, 0)
    if following[1] > 999:
        following = (release.major + 1, 0, 0)
    return max(following, project)


def build_identity(repository: Path | str, ref: str = "HEAD") -> BuildIdentity:
    commit = git("rev-parse", f"{ref}^{{commit}}", repository=repository)
    tags = stable_tags(repository, commit)
    if not tags:
        raise ReleaseError("No stable release tag is reachable; dev builds are numbered after one.")
    latest = tags[0]
    if git("rev-parse", f"{latest.tag}^{{commit}}", repository=repository) == commit:
        return BuildIdentity(latest.major, latest.minor, latest.patch, STABLE_SEQUENCE, commit)
    sequence = int(git("rev-list", "--count", f"{latest.tag}..{commit}", repository=repository))
    if not 1 <= sequence <= DEV_SEQUENCE_MAXIMUM:
        raise ReleaseError(f"{sequence} commits since {latest.tag}: dev builds stop at {DEV_SEQUENCE_MAXIMUM}. "
                           "Publish a stable release.")
    project = parse_project_version(git("show", f"{commit}:CMakeLists.txt", repository=repository))
    major, minor, patch = next_base(latest, project)
    return BuildIdentity(major, minor, patch, sequence, commit)


# --------------------------------------------------------------------------- PE sanity

def check_pe(data: bytes, name: str) -> None:
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
    if characteristics & 0x2000:
        fail("expected an EXE, not a DLL")


# --------------------------------------------------------------------------- update ZIP

DOS_TIME = 0
DOS_DATE = (0 << 9) | (1 << 5) | 1  # 1980-01-01, fixed for reproducible packages
VERSION_NEEDED = 20
VERSION_MADE_BY = 20  # host 0 (MS-DOS/FAT), spec 2.0
EXTERNAL_ATTRIBUTES = 0x20  # FILE_ATTRIBUTE_ARCHIVE


def build_update_zip(binaries: Path, output: Path) -> None:
    path = binaries / PACKAGE_EXECUTABLE
    data = path.read_bytes()
    if not data:
        raise ReleaseError(f"{path} is empty.")
    check_pe(data, PACKAGE_EXECUTABLE)
    members = [(PACKAGE_EXECUTABLE.encode("ascii"), data)]
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
    end = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(members), len(members), len(central), len(body), 0)
    package = bytes(body + central + end)
    if len(package) > PACKAGE_MAXIMUM_BYTES:
        raise ReleaseError("The update package exceeds the 4 GiB ZIP limit accepted by the client.")
    validate_update_zip(package)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(package)


def validate_update_zip(package: bytes) -> list[str]:
    """Mirror of update_package.cpp's structural ZIP and name checks (not PE);
    returns the member names."""
    def fail(reason: str):
        raise ReleaseError(f"Update ZIP rejected: {reason}.")
    if len(package) < 22 or len(package) > PACKAGE_MAXIMUM_BYTES:
        fail("size")
    signature, disk, disk_cd, count_disk, count, cd_bytes, cd_offset, comment = struct.unpack_from(
        "<IHHHHIIH", package, len(package) - 22)
    if (signature != 0x06054B50 or disk or disk_cd or count != 1 or
            count_disk != count or comment or cd_offset > len(package) - 22 or cd_bytes != len(package) - 22 - cd_offset):
        fail("end of central directory")
    next_local, next_central = 0, cd_offset
    names = []
    for index in range(count):
        if next_local + 30 > cd_offset or next_central + 46 > len(package) - 22:
            fail("member bounds")
        name_bytes = struct.unpack_from("<H", package, next_local + 26)[0]
        raw_name = package[next_local + 30:next_local + 30 + name_bytes]
        name = raw_name.decode("ascii", "replace")
        if name != PACKAGE_EXECUTABLE:
            fail(f"member name {name!r}")
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
        names.append(name)
    if next_local != cd_offset or next_central != len(package) - 22:
        fail("trailing or overlapping data")
    return names


# --------------------------------------------------------------------------- manifest

def render_manifest(version: int, channel: str, issued: int, expires: int, artifact_bytes: int,
                    artifact_sha256: str, name: str) -> bytes:
    if channel not in CHANNELS:
        raise ReleaseError(f"Unknown channel {channel!r}.")
    if not re.fullmatch(r"[0-9a-f]{64}", artifact_sha256):
        raise ReleaseError("The artifact digest must be 64 lowercase hex digits.")
    if not (0 < issued < expires) or not (0 < version < 2 ** 64) or not (0 < artifact_bytes <= PACKAGE_MAXIMUM_BYTES):
        raise ReleaseError("Manifest numbers are out of range.")
    if name != artifact_name(version):
        raise ReleaseError(f"The artifact name must be {ARTIFACT_PREFIX}<version>.zip.")
    # Field order and compact separators are fixed; strings never need escapes.
    document = (f'{{"format":{PROTOCOL},"product":"MaiCord","arch":"x64","channel":"{channel}","version":{version},'
                f'"issued":{issued},"expires":{expires},"bytes":{artifact_bytes},"sha256":"{artifact_sha256}",'
                f'"artifact":"{name}"}}').encode("ascii")
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
    if (fields["format"] != PROTOCOL or fields["product"] != "MaiCord" or fields["arch"] != "x64" or
            fields["channel"] not in CHANNELS or not isinstance(fields["sha256"], str) or
            not re.fullmatch(r"[0-9a-f]{64}", fields["sha256"]) or not fields["version"] or
            not fields["issued"] or fields["expires"] <= fields["issued"] or not fields["bytes"] or
            fields["artifact"] != artifact_name(fields["version"])):
        raise ReleaseError("Manifest field values are invalid.")
    return fields


def manifest_digest(document: bytes) -> bytes:
    return hashlib.sha256(SIGNATURE_DOMAIN + document).digest()


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




# --------------------------------------------------------------------------- changelog

def changelog_digest(document: bytes) -> bytes:
    return hashlib.sha256(CHANGELOG_DOMAIN + document).digest()


def render_changelog(channel: str, version: int, base: int, entries: list[dict]) -> bytes:
    document = {"format": 1, "product": "MaiCord", "channel": channel, "version": version, "base": base,
                "entries": [{name: entry[name] for name in CHANGELOG_ENTRY_FIELDS} for entry in entries]}
    rendered = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    parse_changelog(rendered)
    return rendered


def parse_changelog(document: bytes) -> dict:
    """Strict reading equivalent to update_changelog.cpp."""
    if not document or len(document) > CHANGELOG_MAXIMUM_BYTES:
        raise ReleaseError("Changelog size is invalid.")

    def pairs(items):
        names = [name for name, _ in items]
        if len(names) != len(set(names)):
            raise ReleaseError("Changelog has duplicate fields.")
        return dict(items)
    try:
        fields = json.loads(document.decode("utf-8"), object_pairs_hook=pairs)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ReleaseError(f"Changelog is not valid JSON: {error}.") from None

    def unsigned(value) -> bool:
        return type(value) is int and 0 <= value < 2 ** 64
    if not isinstance(fields, dict) or set(fields) != set(CHANGELOG_FIELDS):
        raise ReleaseError(f"Changelog fields must be exactly {', '.join(CHANGELOG_FIELDS)}.")
    if (fields["format"] != 1 or fields["product"] != "MaiCord" or fields["channel"] not in CHANNELS or
            not unsigned(fields["version"]) or not fields["version"] or not unsigned(fields["base"]) or
            fields["base"] >= fields["version"] or not isinstance(fields["entries"], list) or
            len(fields["entries"]) > CHANGELOG_MAXIMUM_ENTRIES):
        raise ReleaseError("Changelog header values are invalid.")
    previous = fields["version"]
    for entry in fields["entries"]:
        if not isinstance(entry, dict) or set(entry) != set(CHANGELOG_ENTRY_FIELDS):
            raise ReleaseError(f"Changelog entries must have exactly {', '.join(CHANGELOG_ENTRY_FIELDS)}.")
        text = entry["text"]
        if (not unsigned(entry["version"]) or not fields["base"] < entry["version"] <= previous or
                not unsigned(entry["date"]) or not isinstance(entry["commit"], str) or
                not re.fullmatch(r"(?:[0-9a-f]{40})?", entry["commit"]) or not isinstance(text, str) or not text or
                len(text.encode("utf-8")) > CHANGELOG_TEXT_MAXIMUM_BYTES):
            raise ReleaseError("Changelog entry values are invalid.")
        previous = entry["version"]
    return fields


def sign_changelog(key: cng_p256.PrivateKey, document: bytes, expected_public: bytes | None) -> bytes:
    if expected_public is not None and key.public_point != expected_public:
        raise ReleaseError("The signing key does not match the expected public key. Refusing to sign.")
    signature = key.sign_digest(changelog_digest(document))
    if not cng_p256.verify_digest(key.public_point, changelog_digest(document), signature):
        raise ReleaseError("CNG produced a signature that does not verify.")
    return signature


def verify_changelog(document: bytes, signature: bytes, public_point: bytes) -> dict:
    if len(signature) != 64 or not cng_p256.verify_digest(public_point, changelog_digest(document), signature):
        raise ReleaseError("Changelog signature does not verify with the given public key.")
    return parse_changelog(document)


def truncate_utf8(text: str, limit: int) -> str:
    if len(text.encode("utf-8")) <= limit:
        return text
    return text.encode("utf-8")[:limit - 3].decode("utf-8", errors="ignore").rstrip() + "…"


def clean_subject(subject: str) -> str:
    text = " ".join("".join(c if c >= " " and c != "\x7f" else " " for c in subject).split())
    return truncate_utf8(text, CHANGELOG_SUBJECT_BYTES)


COMMIT_REFERENCE = re.compile(r"\s*\(\[`[0-9a-f]{7,40}`\]\([^)]*\)\)")
MARKDOWN_LINK = re.compile(r"\[([^\]]*)\]\([^)]*\)")


def release_notes_text(body: str) -> str:
    """The changes part of a release body (see render_notes), without links."""
    lines: list[str] = []
    for raw in body.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        line = raw.rstrip()
        if line.startswith("### Download"):
            break
        if re.match(r"^#{1,2}\s", line) or line.startswith(">") or line.startswith("**Full changelog:**"):
            continue
        line = MARKDOWN_LINK.sub(r"\1", COMMIT_REFERENCE.sub("", line))
        line = "".join(c for c in line.replace("\t", " ") if c >= " " and c != "\x7f").rstrip()
        if line or (lines and lines[-1]):
            lines.append(line)
    while lines and not lines[-1]:
        lines.pop()
    text = "\n".join(lines) or "No user-visible changes."
    while len(text.encode("utf-8")) > CHANGELOG_NOTES_BYTES and "\n" in text:
        text = text.rsplit("\n", 1)[0]
        if len((text + "\n…").encode("utf-8")) <= CHANGELOG_NOTES_BYTES:
            return text + "\n…"
    return truncate_utf8(text, CHANGELOG_NOTES_BYTES)


def fit_changelog(channel: str, version: int, base: int, entries: list[dict]) -> bytes:
    """Drops the oldest versions until the document fits the client's bounds."""
    entries = list(entries)
    while True:
        try:
            return render_changelog(channel, version, base, entries)
        except ReleaseError:
            if not entries:
                raise
        oldest = entries[-1]["version"]
        entries = [entry for entry in entries if entry["version"] != oldest]
        base = oldest


def iso_seconds(text: str | None) -> int:
    if not text:
        return 0
    return int(datetime.fromisoformat(text.replace("Z", "+00:00")).timestamp())


def tag_commit(repository: Path | str, tag: str) -> str:
    try:
        return git("rev-list", "-n", "1", tag, repository=repository)
    except subprocess.CalledProcessError:
        return ""


def stable_changelog(releases: list[dict], version: int, repository: Path | str | None) -> bytes:
    """Release notes of the newest published stable releases up to version."""
    listed: dict[int, tuple[ReleaseVersion, dict]] = {}
    for release in releases:
        tag = str(release.get("tag_name", ""))
        if release.get("draft") or release.get("prerelease") or not TAG_PATTERN.match(tag):
            continue
        parsed = parse_tag(tag)
        if parsed.number <= version:
            listed[parsed.number] = (parsed, release)
    ordered = sorted(listed.items(), key=lambda item: item[0], reverse=True)
    if not ordered or ordered[0][0] != version:
        raise ReleaseError(f"The release list has no published {version_text(version)}.")
    entries = [{"version": number, "date": iso_seconds(release.get("published_at")),
                "commit": tag_commit(repository, parsed.tag) if repository else "",
                "text": release_notes_text(release.get("body") or "")}
               for number, (parsed, release) in ordered[:CHANGELOG_STABLE_RELEASES]]
    base = ordered[CHANGELOG_STABLE_RELEASES][0] if len(ordered) > CHANGELOG_STABLE_RELEASES else 0
    return fit_changelog("stable", version, base, entries)


def preview_changelog(repository: Path | str, target: str, version: int) -> bytes:
    """Commit subjects after the previous stable release up to target, newest
    first, each numbered as the master build that first contained it."""
    target = git("rev-parse", f"{target}^{{commit}}", repository=repository)
    previous = next((tag for tag in stable_tags(repository, target) if tag.number < version), None)
    span = f"{previous.tag}..{target}" if previous else target
    log = {}
    for line in git("log", "--format=%H%x1f%ct%x1f%s", span, repository=repository).splitlines():
        sha, date, subject = line.split("", 2)
        log[sha] = (int(date), subject)
    count = len(log)
    entries: list[dict] = []
    base = previous.number if previous else 0
    for line in git("rev-list", "--first-parent", "--parents", span, repository=repository).splitlines():
        commit, *parents = line.split()
        if len(parents) > 1:
            side = git("rev-list", f"{parents[0]}..{commit}", repository=repository).splitlines()
            merged = [sha for sha in side if sha != commit]
        else:
            side = merged = [commit]
        number = version
        if previous:
            project = parse_project_version(git("show", f"{commit}:CMakeLists.txt", repository=repository))
            major, minor, patch = next_base(previous, project)
            number = min(version, version_number(major, minor, patch, min(count, DEV_SEQUENCE_MAXIMUM)))
        group = [{"version": number, "date": log[sha][0], "commit": sha, "text": clean_subject(log[sha][1])}
                 for sha in merged if sha in log and classify_commit(log[sha][1])]
        if len(entries) + len(group) > CHANGELOG_MAXIMUM_ENTRIES:
            base = number
            break
        entries += group
        count -= len(side)
    return fit_changelog("preview", version, base, entries)


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
        if project != version.semver:
            raise ReleaseError(f"Tag {version.tag} does not match project(MaiCord VERSION {project}) in "
                               "CMakeLists.txt. Bump the project version before tagging.")
    write_outputs({"tag": version.tag, "semver": version.semver, "number": version.number,
                   "artifact": version.artifact_name})


def command_build_identity(arguments) -> None:
    identity = build_identity(arguments.repository, arguments.ref)
    write_outputs({"version": identity.semver, "label": identity.label, "number": identity.number,
                   "commit": identity.commit, "artifact": identity.artifact_name,
                   "dev": str(not is_stable(identity.number)).lower()})


def signed_manifest(arguments, key, expected, version: int, channel: str, artifact_bytes: int, digest: str,
                    output_dir: Path, issued: int) -> None:
    expires = issued + arguments.validity_days * 86400
    document = render_manifest(version, channel, issued, expires, artifact_bytes, digest, artifact_name(version))
    signature = sign_document(key, document, expected)
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / f"maicord-{channel}.manifest.json").write_bytes(document)
    (output_dir / f"maicord-{channel}.manifest.json.sig").write_bytes(signature)


def write_changelog(key, expected, document: bytes, output_dir: Path) -> None:
    fields = parse_changelog(document)
    signature = sign_changelog(key, document, expected)
    verify_changelog(document, signature, expected or key.public_point)
    name = f"maicord-{fields['channel']}.changelog.json"
    (output_dir / name).write_bytes(document)
    (output_dir / f"{name}.sig").write_bytes(signature)
    print(f"{name}: {len(fields['entries'])} entries, {len(document)} bytes")


def package_binaries(binaries: Path, output_dir: Path, name: str) -> tuple[bytes, str]:
    output_dir.mkdir(parents=True, exist_ok=True)
    artifact_path = output_dir / name
    build_update_zip(binaries, artifact_path)
    artifact = artifact_path.read_bytes()
    if len(artifact) > PACKAGE_CLIENT_MAXIMUM_BYTES:
        raise ReleaseError(f"{name} exceeds the client's {PACKAGE_CLIENT_MAXIMUM_BYTES} byte limit.")
    return artifact, hashlib.sha256(artifact).hexdigest()


def command_release(arguments) -> None:
    """Build the update ZIP, sign the stable and preview manifests and self-verify."""
    version = parse_tag(arguments.tag)
    output_dir = Path(arguments.output_dir)
    artifact, digest = package_binaries(Path(arguments.binaries), output_dir, version.artifact_name)
    key = load_private_key(arguments)
    expected = cng_p256.parse_public_point(arguments.public_key) if arguments.public_key else None
    issued = (arguments.issued or int(time.time())) - CLOCK_SKEW_SECONDS
    # A stable release also advances the preview channel.
    for channel in CHANNELS:
        signed_manifest(arguments, key, expected, version.number, channel, len(artifact), digest, output_dir, issued)
        document = (output_dir / f"maicord-{channel}.manifest.json").read_bytes()
        signature = (output_dir / f"maicord-{channel}.manifest.json.sig").read_bytes()
        verify_offer(document, signature, expected or key.public_point, artifact, now=issued + CLOCK_SKEW_SECONDS)
    print(f"{version.artifact_name}: {len(artifact)} bytes, sha256 {digest}")


def command_preview(arguments) -> None:
    """Package and sign a master build for the preview channel, with its changelog."""
    repository = Path(arguments.repository)
    identity = build_identity(repository, "HEAD")
    if is_stable(identity.number):
        raise ReleaseError(f"HEAD is the {identity.semver} release; release.yml publishes it.")
    if arguments.expect_version and arguments.expect_version != identity.semver:
        raise ReleaseError(f"The binaries were built as {arguments.expect_version}, HEAD is {identity.semver}.")
    output_dir = Path(arguments.output_dir)
    artifact, digest = package_binaries(Path(arguments.binaries), output_dir, identity.artifact_name)
    key = load_private_key(arguments)
    expected = cng_p256.parse_public_point(arguments.public_key) if arguments.public_key else None
    issued = (arguments.issued or int(time.time())) - CLOCK_SKEW_SECONDS
    signed_manifest(arguments, key, expected, identity.number, "preview", len(artifact), digest, output_dir, issued)
    verify_offer((output_dir / "maicord-preview.manifest.json").read_bytes(),
                 (output_dir / "maicord-preview.manifest.json.sig").read_bytes(), expected or key.public_point,
                 artifact, now=issued + CLOCK_SKEW_SECONDS)
    write_changelog(key, expected, preview_changelog(repository, identity.commit, identity.number), output_dir)
    print(f"{identity.label}: {identity.artifact_name}, {len(artifact)} bytes, sha256 {digest}")
    write_outputs({"version": identity.semver, "label": identity.label, "artifact": identity.artifact_name})


def command_changelogs(arguments) -> None:
    """Signed stable and preview changelogs for a published stable release."""
    version = parse_tag(arguments.tag)
    if arguments.releases:
        releases = json.loads(Path(arguments.releases).read_text(encoding="utf-8-sig"))
    else:
        listing = subprocess.run(["gh", "api", f"repos/{arguments.github}/releases?per_page=30"], check=True,
                                 capture_output=True).stdout
        releases = json.loads(listing.decode("utf-8"))
    if not isinstance(releases, list):
        raise ReleaseError("The release list must be the JSON array GET /repos/{owner}/{repo}/releases returns.")
    output_dir = Path(arguments.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    key = load_private_key(arguments)
    expected = cng_p256.parse_public_point(arguments.public_key) if arguments.public_key else None
    repository = Path(arguments.repository)
    write_changelog(key, expected, stable_changelog(releases, version.number, repository), output_dir)
    write_changelog(key, expected, preview_changelog(repository, version.tag, version.number), output_dir)


@dataclass(frozen=True)
class Slot:
    name: str
    channel: str
    manifest: str
    changelog: str | None


# The Pages site: this protocol's files under updates/p<protocol>/, with the
# preview feed's master builds served beside its manifest. Other directories
# are left as published, so clients of an earlier protocol keep the newest
# release published for them.
SLOTS = (
    Slot("stable", "stable", f"{SITE_DIRECTORY}/maicord-stable.manifest.json",
         f"{SITE_DIRECTORY}/maicord-stable.changelog.json"),
    Slot("preview", "preview", f"{SITE_DIRECTORY}/{PREVIEW_DIRECTORY}/maicord-preview.manifest.json",
         f"{SITE_DIRECTORY}/{PREVIEW_DIRECTORY}/maicord-preview.changelog.json"),
)
PREVIEW_SLOT = SLOTS[1]


def read_offer(site: Path, slot: Slot, public: bytes, now: int | None = None) -> dict:
    document = site / slot.manifest
    fields = verify_offer(document.read_bytes(), Path(f"{document}.sig").read_bytes(), public, now=now)
    if fields["channel"] != slot.channel:
        raise ReleaseError(f"The {slot.name} manifest carries the {fields['channel']} channel.")
    if slot is not PREVIEW_SLOT and not is_stable(fields["version"]):
        raise ReleaseError(f"The {slot.name} manifest offers dev build {fields['version']}.")
    return fields


def preview_package(site: Path, number: int) -> Path:
    return site / SITE_DIRECTORY / PREVIEW_DIRECTORY / artifact_name(number)


def command_channels(arguments) -> None:
    """Maintain the Pages site that serves the channel manifests and changelogs.

    The site directory is the root of the update-channels branch and of the
    GitHub Pages deployment (see SLOTS). --publish installs the signed files of
    a release or master build (a channel never moves to a lower version);
    --refresh-within-days re-signs manifests close to expiry. Every manifest
    left must verify and be currently valid, every changelog must match its
    manifest's version, and a preview dev build's package must be present.
    """
    site = Path(arguments.site)
    expected = cng_p256.parse_public_point(arguments.public_key)
    now = int(time.time())
    changed = []
    current = {}
    for slot in SLOTS:
        document = site / slot.manifest
        if document.exists() or Path(f"{document}.sig").exists():
            try:
                current[slot.name] = read_offer(site, slot, expected)
            except (ReleaseError, OSError) as error:
                raise ReleaseError(f"The site's {slot.name} manifest is invalid: {error}") from None
    if arguments.publish:
        source = Path(arguments.publish)
        offers = []
        for channel in CHANNELS:
            document = source / f"maicord-{channel}.manifest.json"
            if not document.exists():
                continue
            payload, signature = document.read_bytes(), Path(f"{document}.sig").read_bytes()
            fields = verify_offer(payload, signature, expected, now=now)
            if fields["channel"] != channel:
                raise ReleaseError(f"{document.name} carries the {fields['channel']} channel.")
            if is_stable(fields["version"]):
                offers += [(slot, payload, signature, fields, None) for slot in SLOTS if slot.channel == channel]
            elif channel == "stable":
                raise ReleaseError(f"{document.name} offers dev build {fields['version']} on the stable channel.")
            else:
                package = source / fields["artifact"]
                verify_offer(payload, signature, expected, package.read_bytes())
                if fields["bytes"] > PACKAGE_CLIENT_MAXIMUM_BYTES:
                    raise ReleaseError(f"{package.name} exceeds the client's package limit.")
                offers.append((PREVIEW_SLOT, payload, signature, fields, package))
        for slot, payload, signature, fields, package in offers:
            previous = current.get(slot.name)
            # An equal version keeps the site's copy, which may be re-signed.
            if previous and fields["version"] <= previous["version"]:
                if fields["version"] < previous["version"]:
                    print(f"{slot.name}: keeps version {previous['version']}; not moving back to {fields['version']}.")
                continue
            target = site / slot.manifest
            target.parent.mkdir(parents=True, exist_ok=True)
            if package:
                shutil.copyfile(package, preview_package(site, fields["version"]))
            target.write_bytes(payload)
            Path(f"{target}.sig").write_bytes(signature)
            current[slot.name] = fields
            changed.append(f"{slot.name} -> {fields['version']}")
        for slot in SLOTS:
            document = source / f"maicord-{slot.channel}.changelog.json"
            if not slot.changelog or not document.exists():
                continue
            payload, signature = document.read_bytes(), Path(f"{document}.sig").read_bytes()
            fields = verify_changelog(payload, signature, expected)
            offer = current.get(slot.name)
            if fields["channel"] != slot.channel or not offer or fields["version"] != offer["version"]:
                print(f"{slot.name}: {document.name} is for version {fields['version']}; not installed.")
                continue
            target = site / slot.changelog
            if target.exists() and target.read_bytes() == payload:
                continue
            target.write_bytes(payload)
            Path(f"{target}.sig").write_bytes(signature)
            changed.append(f"{slot.name} changelog -> {fields['version']}")
    if arguments.refresh_within_days is not None:
        key = None
        for slot in SLOTS:
            fields = current.get(slot.name)
            if not fields or fields["expires"] - now > arguments.refresh_within_days * 86400:
                continue
            key = key or load_private_key(arguments)
            signed_manifest(arguments, key, expected, fields["version"], slot.channel, fields["bytes"],
                            fields["sha256"], (site / slot.manifest).parent, now - CLOCK_SKEW_SECONDS)
            current[slot.name] = read_offer(site, slot, expected, now)
            changed.append(f"{slot.name} re-signed")
    for slot in SLOTS:
        if not slot.changelog or not (site / slot.changelog).exists():
            continue
        target = site / slot.changelog
        fields = verify_changelog(target.read_bytes(), Path(f"{target}.sig").read_bytes(), expected)
        offer = current.get(slot.name)
        if fields["channel"] != slot.channel or not offer or fields["version"] != offer["version"]:
            target.unlink()
            Path(f"{target}.sig").unlink(missing_ok=True)
            changed.append(f"{slot.name} changelog removed")
    feed = current.get(PREVIEW_SLOT.name)
    keep = []
    if feed and not is_stable(feed["version"]):
        package = preview_package(site, feed["version"])
        if not package.exists():
            raise ReleaseError(f"The preview package {package.name} is missing from the site.")
        keep.append(package)
    packages = sorted(((int(path.stem.rsplit("-", 1)[1]), path) for path in
                       (site / SITE_DIRECTORY / PREVIEW_DIRECTORY).glob(f"{ARTIFACT_PREFIX}*.zip")
                       if re.fullmatch(re.escape(ARTIFACT_PREFIX) + r"\d+", path.stem)), reverse=True)
    for number, path in packages:
        if path in keep:
            continue
        if keep and len(keep) < PREVIEW_PACKAGES_KEPT and number < feed["version"]:
            keep.append(path)
            continue
        path.unlink()
        changed.append(f"removed {path.name}")
    for slot in SLOTS:
        if slot.name not in current:
            continue
        fields = read_offer(site, slot, expected, now)
        if slot is PREVIEW_SLOT and not is_stable(fields["version"]):
            verify_offer((site / slot.manifest).read_bytes(), Path(f"{site / slot.manifest}.sig").read_bytes(),
                         expected, preview_package(site, fields["version"]).read_bytes(), now)
        print(f"{slot.name}: {version_text(fields['version'])}, expires in {(fields['expires'] - now) // 86400} days")
    print("changed: " + (", ".join(changed) if changed else "nothing"))
    write_outputs({"changed": str(bool(changed)).lower()})


def command_verify(arguments) -> None:
    public = cng_p256.parse_public_point(arguments.public_key)
    fields = verify_offer(Path(arguments.manifest).read_bytes(), Path(arguments.signature).read_bytes(), public,
                          Path(arguments.artifact).read_bytes() if arguments.artifact else None,
                          now=int(time.time()) if arguments.check_time else None)
    print(json.dumps(fields, indent=2))
    if arguments.changelog:
        changelog = Path(arguments.changelog)
        log = verify_changelog(changelog.read_bytes(), Path(f"{changelog}.sig").read_bytes(), public)
        if (log["channel"], log["version"]) != (fields["channel"], fields["version"]):
            raise ReleaseError(f"{changelog.name} does not describe this manifest's version.")
        print(f"{changelog.name}: {len(log['entries'])} entries")


def command_portable(arguments) -> None:
    """Full portable ZIP for first installs: the executable plus fonts/ and licenses/."""
    binaries = Path(arguments.binaries)
    output = Path(arguments.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    files = [binaries / PACKAGE_EXECUTABLE]
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


NOTE_GROUPS = ("Features", "Fixes", "Performance", "Other")
CONVENTIONAL = re.compile(r"^(?P<type>[a-z]+)(?:\([^)]*\))?!?:\s*(?P<text>.+)$", re.IGNORECASE)
CONVENTIONAL_GROUPS = {"feat": "Features", "feature": "Features", "fix": "Fixes", "bugfix": "Fixes", "perf": "Performance"}
FIX_WORDS = re.compile(r"\b(fix\w*|crash\w*|bug\w*|broken|regression\w*|prevent\w*|restore\w*|repair\w*|never fail\w*|"
                       r"correct\w*|wrong)\b", re.IGNORECASE)
PERFORMANCE_WORDS = re.compile(r"\b(perf|performance|faster|speed\w*|latency|throughput|cpu|memory use|compile time|"
                               r"code size|startup time|optimi[sz]\w*)\b", re.IGNORECASE)
FEATURE_WORDS = re.compile(r"^(add|adds|added|support|supports|introduce\w*|implement\w*|enable\w*|allow\w*|new|show)\b",
                           re.IGNORECASE)
RELEASE_BUMP = re.compile(r"^(maicord|release|bump( version)?( to)?|version)\s+v?\d+\.\d+\.\d+\S*\s*$", re.IGNORECASE)


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
    tags = [tag.tag for tag in stable_tags(".", version.tag) if tag.number < version.number]
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

    command = sub.add_parser("version", help="map a release tag to the updater's version number")
    command.add_argument("tag")
    command.add_argument("--check-cmake", metavar="REPOSITORY", help="require CMakeLists.txt project VERSION to match")
    command.set_defaults(run=command_version)

    command = sub.add_parser("identity", help="the version a commit builds as (a release or a dev build)")
    command.add_argument("--repository", default=".")
    command.add_argument("--ref", default="HEAD")
    command.set_defaults(run=command_build_identity)

    command = sub.add_parser("release", help="build the update ZIP and signed channel manifests")
    command.add_argument("--tag", required=True)
    command.add_argument("--binaries", required=True, help="directory containing MaiCord.exe")
    command.add_argument("--output-dir", required=True)
    command.add_argument("--public-key", help="expected public point (hex); signing fails on mismatch")
    command.add_argument("--issued", type=int, help=argparse.SUPPRESS)
    key_options(command)
    command.set_defaults(run=command_release)

    command = sub.add_parser("preview", help="package and sign HEAD's master build for the preview channel")
    command.add_argument("--binaries", required=True)
    command.add_argument("--output-dir", required=True)
    command.add_argument("--repository", default=".")
    command.add_argument("--expect-version", help="the version the binaries were built as")
    command.add_argument("--public-key")
    command.add_argument("--issued", type=int, help=argparse.SUPPRESS)
    key_options(command)
    command.set_defaults(run=command_preview)

    command = sub.add_parser("changelogs", help="signed stable and preview changelogs for a published release")
    command.add_argument("--tag", required=True)
    source = command.add_mutually_exclusive_group(required=True)
    source.add_argument("--releases", help="JSON from GET /repos/{owner}/{repo}/releases")
    source.add_argument("--github", metavar="OWNER/REPO", help="list the releases with the GitHub CLI")
    command.add_argument("--output-dir", required=True)
    command.add_argument("--repository", default=".")
    command.add_argument("--public-key")
    key_options(command)
    command.set_defaults(run=command_changelogs)

    command = sub.add_parser("channels", help="publish or re-sign the update files of the Pages site")
    command.add_argument("--site", required=True, help="site root (the update-channels branch)")
    command.add_argument("--public-key", required=True)
    command.add_argument("--publish", metavar="DIRECTORY", help="install the signed files found here")
    command.add_argument("--refresh-within-days", type=int, help="re-sign manifests expiring within this many days")
    key_options(command)
    command.set_defaults(run=command_channels)

    command = sub.add_parser("verify", help="verify a manifest, signature, optional artifact and changelog")
    command.add_argument("--manifest", required=True)
    command.add_argument("--signature", required=True)
    command.add_argument("--public-key", required=True)
    command.add_argument("--artifact")
    command.add_argument("--changelog", help="a changelog (with .sig beside it) that must match the manifest")
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
    except (ReleaseError, cng_p256.CngError, OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
