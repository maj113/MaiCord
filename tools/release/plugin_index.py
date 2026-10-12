#!/usr/bin/env python3
"""Reference writer and verifier for MaiCord plugin catalog indexes (format 1).

The client's verifier is src/app/plugin_index.cpp. This module mirrors it: the
same strict schema, limits, display-text rule and error codes, so the shared
vectors in tests/fixtures/plugin_index give the same result in both. Signing
uses Windows CNG through cng_p256 (stdlib only).

    python -B tools/release/plugin_index.py render <source.json> <index.json>
    python -B tools/release/plugin_index.py sign --key <private.jwk> <index.json>
    python -B tools/release/plugin_index.py verify --kind official --public-key <hex> [--now <unix>] <index.json>
    python -B tools/release/plugin_index.py manifest <index.json> --slug <slug> --version <x.y.z> [--arch x64]

render orders the members canonically and writes compact UTF-8 JSON;
sign writes <index.json>.sig (64-byte P1363 over SHA-256 of the domain and
the document); verify checks the signature, schema and freshness; manifest
prints the schema-3 manifest the client synthesizes for one listed version.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cng_p256  # noqa: E402

DOMAIN = b"MaiCord plugin index v1\n"
MAX_BYTES = 256 * 1024
MAX_DEPTH = 8
MAX_PLUGINS = 128
MAX_VERSIONS = 4
MAX_FILES = 2
MAX_AUTHORS = 4
MAX_CATEGORIES = 3
MAX_REVOKED = 64
MAX_BLOCKED = 64
DETAILS_MAX_BYTES = 16 * 1024
FILE_MAX_BYTES = 64 * 1024 * 1024
MAX_HOST_ABI = 0xFFFF
MAX_VALIDITY_SECONDS = 35 * 86400
CLOCK_SKEW_SECONDS = 600
U64_MAX = 2 ** 64 - 1
MANIFEST_ABI = 5

PROVIDER_ID_MAX = 40
PROVIDER_NAME_MAX = 64
SLUG_MAX = 40
NAME_MAX = 95
SUMMARY_MAX = 255
PUBLISHER_MAX = 63
AUTHOR_MAX = 64
GITHUB_MAX = 39
LICENSE_MAX = 32
URL_MAX = 256
CHANGELOG_MAX = 512
REASON_MAX = 128
PATH_MAX = 128
TAG_MAX = 64
ASSET_MAX = 128
VERSION_TEXT_MAX = 23

# Manifest permission names in MC_PERM_* bit order (src/app/plugin_manifest.cpp).
PERMISSIONS = ("settings", "menus", "message_send", "commands", "ui_slots", "store", "timer", "read_context",
               "read_guilds", "read_members", "read_presence", "read_voice", "read_messages", "message_render",
               "act_messages", "act_presence", "navigate", "clipboard", "open_links", "notify", "keybinds", "effects")
CATEGORIES = ("chat", "appearance", "notifications", "privacy", "utility", "voice", "accessibility", "moderation",
              "developer", "fun")
DISCLOSURES = ("network", "files", "processes", "input")
ARCHES = ("x64", "arm64")
SEVERITIES = ("malicious", "broken", "withdrawn")
KINDS = ("official", "custom_signed", "custom_unsigned")
OFFICIAL_ID = "maicord"

# Canonical member order (also the schema: anything else is an unknown field).
ROOT = ("format", "provider", "sequence", "issued", "expires", "plugins", "revoked", "blocklist")
PROVIDER = ("id", "name", "homepage", "key")
PLUGIN = ("slug", "owner_id", "name", "summary", "publisher", "authors", "license", "homepage", "categories",
          "disclosures", "details", "versions")
AUTHOR = ("name", "github")
DETAILS = ("sha256", "bytes")
VERSION = ("version", "version_code", "abi", "min_host_abi", "max_host_abi", "permissions", "files", "released",
           "commit", "changelog")
FILE = ("arch", "sha256", "bytes", "path", "github_release")
RELEASE = ("repository", "tag", "asset")
REVOKED = ("owner_id", "sha256", "severity", "reason", "since")
BLOCK = ("sha256", "reason", "since")

# Error codes, named like PluginIndexError in include/maicord/app/plugin_index.hpp.
ERRORS = ("size", "signature", "json", "depth", "unknown_field", "duplicate_field", "missing_field", "type", "format",
          "provider", "key_changed", "sequence", "time", "expired", "limit", "text", "identifier", "duplicate", "owner",
          "version", "order", "abi", "permission", "choice", "digest", "file", "location", "url")


class PluginIndexError(Exception):
    def __init__(self, code: str, detail: str = ""):
        assert code in ERRORS, code
        super().__init__(f"{code}: {detail}" if detail else code)
        self.code = code


class _NotUnsigned:
    """A JSON number that is not an unsigned 64-bit integer (fraction, exponent, sign or too long)."""

    def __init__(self, text: str):
        self.text = text


def _parse_int(text: str):
    return _NotUnsigned(text) if text.startswith("-") or len(text) > 20 else int(text)


def _refuse_constant(text: str):
    raise PluginIndexError("json", f"{text} is not JSON")


def _pairs(pairs):
    names = [name for name, _ in pairs]
    if len(set(names)) != len(names):
        raise PluginIndexError("duplicate_field")
    return dict(pairs)


def _surrogates(value) -> bool:
    if isinstance(value, str):
        return any(0xD800 <= ord(c) <= 0xDFFF for c in value)
    if isinstance(value, dict):
        return any(_surrogates(k) or _surrogates(v) for k, v in value.items())
    if isinstance(value, list):
        return any(_surrogates(v) for v in value)
    return False


def _depth(value, level: int) -> int:
    """Container nesting with the root object at level 1, as the client's reader counts it."""
    if isinstance(value, dict):
        return max([level] + [_depth(v, level + 1) for v in value.values()])
    if isinstance(value, list):
        return max([level] + [_depth(v, level + 1) for v in value])
    return level - 1


# ---- value grammars --------------------------------------------------------------
_LOWER_ALNUM = frozenset("abcdefghijklmnopqrstuvwxyz0123456789")
_ALNUM = _LOWER_ALNUM | frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
_HEX_ANY = frozenset("0123456789abcdefABCDEF")
_HEX_LOWER = frozenset("0123456789abcdef")
_URL_BYTES = _ALNUM | frozenset("-._~!$&'()*+,;=:@/?%#")
_VERSION = re.compile(r"(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})(?:-preview\.([1-9][0-9]{0,2}))?")


def _utf8(text: str) -> int:
    return len(text.encode("utf-8"))


def _banned(c: int) -> bool:
    return (c < 32 or 127 <= c <= 159 or c == 0x061C or 0x200B <= c <= 0x200F or 0x202A <= c <= 0x202E or
            0x2060 <= c <= 0x2064 or 0x2066 <= c <= 0x2069 or c == 0xFEFF)


def display_valid(text: str, maximum: int, required: bool) -> bool:
    """The manifest text rule (UTF-8, no C0/C1 controls) plus the bidi and zero-width ban."""
    if _utf8(text) > maximum or (required and not text):
        return False
    return not any(_banned(ord(c)) for c in text)


def origin_host_valid(host: str) -> bool:
    """update_origin_host_valid: an LDH DNS name of at most 64 bytes, not an IPv4 literal or localhost."""
    if not host or _utf8(host) > 64:
        return False
    labels = host.split(".")
    if len(labels) < 2:
        return False
    for label in labels:
        if not label or len(label) > 63 or label[0] == "-" or label[-1] == "-" or any(c not in _ALNUM and c != "-" for c in label):
            return False
    last = labels[-1]
    return not all(c in "0123456789" for c in last) and last.lower() != "localhost"


def url_valid(text: str) -> bool:
    if _utf8(text) > URL_MAX or len(text) <= 8 or not text.startswith("https://"):
        return False
    end = 8
    while end < len(text) and text[end] not in "/?#":
        end += 1
    return origin_host_valid(text[8:end]) and all(c in _URL_BYTES for c in text[end:])


def repository_valid(text: str) -> bool:
    """update_repository_valid: GitHub "owner/name"."""
    if not text or _utf8(text) > 100 or "/" not in text:
        return False
    owner, name = text.split("/", 1)
    if not owner or len(owner) > 39 or owner[0] == "-" or owner[-1] == "-" or any(c not in _ALNUM and c != "-" for c in owner):
        return False
    if not name or name in (".", "..") or any(c not in _ALNUM and c not in "-_." for c in name):
        return False
    return True


def _slug_valid(text: str) -> bool:
    return (0 < len(text) <= SLUG_MAX and text[0] in _LOWER_ALNUM and
            all(c in _LOWER_ALNUM or c == "-" for c in text))


def _provider_id_valid(text: str) -> bool:
    return 0 < len(text) <= PROVIDER_ID_MAX and all(c in _LOWER_ALNUM or c == "-" for c in text)


def _license_valid(text: str) -> bool:
    return 0 < len(text) <= LICENSE_MAX and all(c in _ALNUM or c in ".+-" for c in text)


def _github_valid(text: str) -> bool:
    return (0 < len(text) <= GITHUB_MAX and text[0] != "-" and text[-1] != "-" and
            all(c in _ALNUM or c == "-" for c in text))


def _hex(text: str, nbytes: int) -> bool:
    return len(text) == nbytes * 2 and all(c in _HEX_LOWER for c in text)


def _path_valid(text: str) -> bool:
    if not text or _utf8(text) > PATH_MAX:
        return False
    for segment in text.split("/"):
        if not segment or segment in (".", "..") or any(c not in _ALNUM and c not in "._-" for c in segment):
            return False
    return True


def _release_name_valid(text: str, maximum: int) -> bool:
    return (0 < len(text) and _utf8(text) <= maximum and text not in (".", "..") and
            all(c in _ALNUM or c in "._+-" for c in text))


def version_text(code: int) -> str:
    """update_version_text."""
    n, base = code % 1000, code // 1000
    text = f"{base // 1000000}.{base // 1000 % 1000}.{base % 1000}"
    return text if n == 999 else f"{text}-preview.{n}"


def version_code(text: str) -> int:
    """update_version_number for "MAJOR.MINOR.PATCH" or "MAJOR.MINOR.PATCH-preview.N" (N 1-998)."""
    match = _VERSION.fullmatch(text)
    if not match or (match.group(4) and int(match.group(4)) > 998):
        raise PluginIndexError("version", f"{text!r} is not MAJOR.MINOR.PATCH[-preview.N]")
    major, minor, patch = (int(match.group(i)) for i in (1, 2, 3))
    return ((major * 1000 + minor) * 1000 + patch) * 1000 + (int(match.group(4)) if match.group(4) else 999)


def _version_valid(text: str, code: int) -> bool:
    if not code or code % 1000 == 0 or code // 1000000000 >= 1000 or _utf8(text) > VERSION_TEXT_MAX:
        return False
    return text == version_text(code)


# ---- strict schema ---------------------------------------------------------------
def _number(value) -> int:
    if type(value) is not int or not 0 <= value <= U64_MAX:
        raise PluginIndexError("type", "expected an unsigned 64-bit integer")
    return value


def _string(value) -> str:
    if type(value) is not str:
        raise PluginIndexError("type", "expected a string")
    return value


def _object(value) -> dict:
    if type(value) is not dict:
        raise PluginIndexError("type", "expected an object")
    return value


def _array(value) -> list:
    if type(value) is not list:
        raise PluginIndexError("type", "expected an array")
    return value


def _members(value: dict, names: tuple):
    for name, member in value.items():
        if name not in names:
            raise PluginIndexError("unknown_field", name)
        yield name, member


def _require(seen: set, names) -> None:
    if not set(names) <= seen:
        raise PluginIndexError("missing_field", ", ".join(sorted(set(names) - seen)))


def _string_set(value, vocabulary: tuple, maximum: int, unknown: str) -> list:
    entries = []
    for index, entry in enumerate(_array(value)):
        if index >= maximum:
            raise PluginIndexError("limit")
        text = _string(entry)
        if text not in vocabulary:
            raise PluginIndexError(unknown, text)
        if text in entries:
            raise PluginIndexError("duplicate", text)
        entries.append(text)
    return entries


class _Context:
    def __init__(self, kind: str, pinned_key: bytes | None):
        self.kind = kind
        self.pinned_key = pinned_key
        self.official = kind == "official"
        self.ignored_versions = 0


def _provider(value, ctx: _Context) -> dict:
    provider, seen = {}, set()
    for name, member in _members(_object(value), PROVIDER):
        seen.add(name)
        text = _string(member)
        if name == "id":
            if not _provider_id_valid(text):
                raise PluginIndexError("identifier", "provider.id")
            if (text == OFFICIAL_ID) != ctx.official:
                raise PluginIndexError("provider", "only the official provider is called maicord")
        elif name == "name":
            if not display_valid(text, PROVIDER_NAME_MAX, True):
                raise PluginIndexError("text", "provider.name")
        elif name == "homepage":
            if not url_valid(text):
                raise PluginIndexError("url", "provider.homepage")
        else:
            if (ctx.kind != "custom_signed" or len(text) != 130 or any(c not in _HEX_ANY for c in text) or
                    text[:2] != "04"):
                raise PluginIndexError("provider", "provider.key")
            if bytes.fromhex(text) != ctx.pinned_key:
                raise PluginIndexError("key_changed")
        provider[name] = text
    _require(seen, PROVIDER[:3])
    if ("key" in provider) != (ctx.kind == "custom_signed"):
        raise PluginIndexError("provider", "provider.key")
    return provider


def _file(value, ctx: _Context) -> dict:
    file, seen = {}, set()
    for name, member in _members(_object(value), FILE):
        seen.add(name)
        if name == "arch":
            if _string(member) not in ARCHES:
                raise PluginIndexError("choice", "arch")
        elif name == "sha256":
            if not _hex(_string(member), 32):
                raise PluginIndexError("digest", "sha256")
        elif name == "bytes":
            if not 0 < _number(member) <= FILE_MAX_BYTES:
                raise PluginIndexError("file", "bytes")
        elif name == "path":
            if ctx.official or not _path_valid(_string(member)):
                raise PluginIndexError("location", "path")
        else:
            if ctx.official:
                raise PluginIndexError("location", "github_release")
            release, release_seen = {}, set()
            for field, text in _members(_object(member), RELEASE):
                release_seen.add(field)
                text = _string(text)
                valid = (repository_valid(text) if field == "repository" else
                         _release_name_valid(text, TAG_MAX if field == "tag" else ASSET_MAX))
                if not valid:
                    raise PluginIndexError("location", field)
                release[field] = text
            _require(release_seen, RELEASE)
            member = release
        file[name] = member
    _require(seen, FILE[:3])
    located = ("path" in file) + ("github_release" in file)
    if not ctx.official and located != 1:
        raise PluginIndexError("location", "a custom file needs exactly one of path and github_release")
    return file


def _version(value, ctx: _Context) -> dict:
    version, seen = {}, set()
    for name, member in _members(_object(value), VERSION):
        seen.add(name)
        if name == "version":
            if _utf8(_string(member)) > VERSION_TEXT_MAX:
                raise PluginIndexError("version")
        elif name in ("version_code", "abi", "min_host_abi", "max_host_abi"):
            _number(member)
        elif name == "permissions":
            member = _string_set(member, PERMISSIONS, len(PERMISSIONS), "permission")
        elif name == "files":
            files = []
            for index, entry in enumerate(_array(member)):
                if index >= MAX_FILES:
                    raise PluginIndexError("limit", "files")
                file = _file(entry, ctx)
                if any(f["arch"] == file["arch"] for f in files):
                    raise PluginIndexError("duplicate", "arch")
                files.append(file)
            if not files:
                raise PluginIndexError("limit", "files")
            member = files
        elif name == "released":
            if not _number(member):
                raise PluginIndexError("time", "released")
        elif name == "commit":
            if not _hex(_string(member), 20):
                raise PluginIndexError("digest", "commit")
        else:
            if not display_valid(_string(member), CHANGELOG_MAX, False):
                raise PluginIndexError("text", "changelog")
        version[name] = member
    _require(seen, [n for n in VERSION if n != "commit"])
    if not _version_valid(version["version"], version["version_code"]):
        raise PluginIndexError("version", version["version"])
    abi, low, high = version["abi"], version["min_host_abi"], version["max_host_abi"]
    if not 0 < abi <= MAX_HOST_ABI or not abi <= low <= MAX_HOST_ABI or (high and not low <= high <= MAX_HOST_ABI):
        raise PluginIndexError("abi")
    if ctx.official and "commit" not in version:
        raise PluginIndexError("provider", "official versions name their commit")
    return version


def _plugin(value, ctx: _Context) -> dict:
    plugin, seen = {}, set()
    for name, member in _members(_object(value), PLUGIN):
        seen.add(name)
        if name == "slug":
            if not _slug_valid(_string(member)):
                raise PluginIndexError("identifier", "slug")
        elif name == "owner_id":
            if _number(member) <= 1:
                raise PluginIndexError("owner")
        elif name in ("name", "summary", "publisher"):
            maximum = {"name": NAME_MAX, "summary": SUMMARY_MAX, "publisher": PUBLISHER_MAX}[name]
            if not display_valid(_string(member), maximum, name == "name"):
                raise PluginIndexError("text", name)
        elif name == "authors":
            authors = []
            for index, entry in enumerate(_array(member)):
                if index >= MAX_AUTHORS:
                    raise PluginIndexError("limit", "authors")
                author, author_seen = {}, set()
                for field, text in _members(_object(entry), AUTHOR):
                    author_seen.add(field)
                    text = _string(text)
                    if field == "name" and not display_valid(text, AUTHOR_MAX, True):
                        raise PluginIndexError("text", "author")
                    if field == "github" and not _github_valid(text):
                        raise PluginIndexError("identifier", "github")
                    author[field] = text
                _require(author_seen, ("name",))
                authors.append(author)
            if not authors:
                raise PluginIndexError("limit", "authors")
            member = authors
        elif name == "license":
            if not _license_valid(_string(member)):
                raise PluginIndexError("identifier", "license")
        elif name == "homepage":
            if not url_valid(_string(member)):
                raise PluginIndexError("url", "homepage")
        elif name == "categories":
            member = _string_set(member, CATEGORIES, MAX_CATEGORIES, "choice")
        elif name == "disclosures":
            member = _string_set(member, DISCLOSURES, len(DISCLOSURES), "choice")
        elif name == "details":
            details, details_seen = {}, set()
            for field, entry in _members(_object(member), DETAILS):
                details_seen.add(field)
                if field == "sha256" and not _hex(_string(entry), 32):
                    raise PluginIndexError("digest", "details")
                if field == "bytes" and not 0 < _number(entry) <= DETAILS_MAX_BYTES:
                    raise PluginIndexError("file", "details")
                details[field] = entry
            _require(details_seen, DETAILS)
            member = details
        else:
            listed, previous = [], None
            entries = _array(member)
            for index, entry in enumerate(entries):
                if index >= MAX_VERSIONS:
                    raise PluginIndexError("limit", "versions")
                version = _version(entry, ctx)
                if previous is not None and version["version_code"] >= previous:
                    raise PluginIndexError("order")
                previous = version["version_code"]
                if version["version_code"] % 1000 == 999:
                    listed.append(version)
                else:
                    ctx.ignored_versions += 1  # previews are reserved for later clients
            if not entries:
                raise PluginIndexError("limit", "versions")
            member = listed
        plugin[name] = member
    _require(seen, [n for n in PLUGIN if n != "details"])
    return plugin


def _revoked(value) -> list:
    entries = []
    for index, entry in enumerate(_array(value)):
        if index >= MAX_REVOKED:
            raise PluginIndexError("limit", "revoked")
        revocation, seen = {}, set()
        for name, member in _members(_object(entry), REVOKED):
            seen.add(name)
            if name == "owner_id" and _number(member) <= 1:
                raise PluginIndexError("owner", "revoked")
            if name == "sha256" and not _hex(_string(member), 32):
                raise PluginIndexError("digest", "revoked")
            if name == "severity" and _string(member) not in SEVERITIES:
                raise PluginIndexError("choice", "severity")
            if name == "reason" and not display_valid(_string(member), REASON_MAX, False):
                raise PluginIndexError("text", "reason")
            if name == "since" and not _number(member):
                raise PluginIndexError("time", "since")
            revocation[name] = member
        _require(seen, REVOKED)
        if any(r["owner_id"] == revocation["owner_id"] and r["sha256"] == revocation["sha256"] for r in entries):
            raise PluginIndexError("duplicate", "revoked")
        entries.append(revocation)
    return entries


def _blocklist(value, ctx: _Context) -> list:
    if not ctx.official:
        raise PluginIndexError("provider", "only the official index carries a blocklist")
    entries = []
    for index, entry in enumerate(_array(value)):
        if index >= MAX_BLOCKED:
            raise PluginIndexError("limit", "blocklist")
        block, seen = {}, set()
        for name, member in _members(_object(entry), BLOCK):
            seen.add(name)
            if name == "sha256" and not _hex(_string(member), 32):
                raise PluginIndexError("digest", "blocklist")
            if name == "reason" and not display_valid(_string(member), REASON_MAX, False):
                raise PluginIndexError("text", "reason")
            if name == "since" and not _number(member):
                raise PluginIndexError("time", "since")
            block[name] = member
        _require(seen, BLOCK)
        if any(b["sha256"] == block["sha256"] for b in entries):
            raise PluginIndexError("duplicate", "blocklist")
        entries.append(block)
    return entries


def parse_index(document: bytes, kind: str, now: int | None, pinned_key: bytes | None = None,
                accept_expired: bool = False) -> dict:
    """Validates an index exactly as the client does, without the signature.

    now=None skips the comparisons with the current time (render uses it).
    Returns the members in document form; listed versions exclude previews."""
    if kind not in KINDS:
        raise ValueError(f"kind must be one of {KINDS}")
    if not document or len(document) > MAX_BYTES:
        raise PluginIndexError("size")
    if document.startswith(b"\xef\xbb\xbf"):
        raise PluginIndexError("json", "byte order mark")
    try:
        text = document.decode("utf-8")
        root = json.loads(text, parse_int=_parse_int, parse_float=_NotUnsigned, parse_constant=_refuse_constant,
                          object_pairs_hook=_pairs)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise PluginIndexError("json", str(error)) from None
    if type(root) is not dict or _surrogates(root):
        raise PluginIndexError("json")
    ctx = _Context(kind, pinned_key)
    index, seen = {}, set()
    for name, member in root.items():
        # The client's reader bounds each member's nesting before visiting it.
        if _depth(member, 2) > MAX_DEPTH:
            raise PluginIndexError("depth")
        if name not in ROOT:
            raise PluginIndexError("unknown_field", name)
        seen.add(name)
        if name == "format":
            if _number(member) != 1:
                raise PluginIndexError("format")
        elif name == "provider":
            member = _provider(member, ctx)
        elif name == "sequence":
            if not _number(member):
                raise PluginIndexError("sequence")
        elif name in ("issued", "expires"):
            _number(member)
        elif name == "plugins":
            plugins = []
            for position, entry in enumerate(_array(member)):
                if position >= MAX_PLUGINS:
                    raise PluginIndexError("limit", "plugins")
                plugin = _plugin(entry, ctx)
                if any(p["owner_id"] == plugin["owner_id"] or p["slug"] == plugin["slug"] for p in plugins):
                    raise PluginIndexError("duplicate", plugin["slug"])
                plugins.append(plugin)
            member = plugins
        elif name == "revoked":
            member = _revoked(member)
        else:
            member = _blocklist(member, ctx)
        index[name] = member
    _require(seen, ROOT[:7])
    issued, expires = index["issued"], index["expires"]
    if (not issued or expires <= issued or expires - issued > MAX_VALIDITY_SECONDS or
            (now is not None and issued > now + CLOCK_SKEW_SECONDS)):
        raise PluginIndexError("time")
    index["expired"] = False
    if now is not None and expires <= now:
        if not accept_expired:
            raise PluginIndexError("expired")
        index["expired"] = True
    index["ignored_versions"] = ctx.ignored_versions
    index["digest"] = hashlib.sha256(DOMAIN + document).hexdigest()
    return index


def verify_index(document: bytes, signature: bytes, public_point: bytes, kind: str, now: int | None,
                 pinned_key: bytes | None = None, accept_expired: bool = False) -> dict:
    """plugin_index_verify: size, then the signature under the plugin domain, then the schema."""
    if kind == "custom_unsigned":
        raise ValueError("unsigned providers have no signature to verify")
    if not document or len(document) > MAX_BYTES:
        raise PluginIndexError("size")
    if len(signature) != 64 or not cng_p256.verify_digest(public_point, hashlib.sha256(DOMAIN + document).digest(), signature):
        raise PluginIndexError("signature")
    return parse_index(document, kind, now, pinned_key, accept_expired)


def sign_index(document: bytes, key: cng_p256.PrivateKey) -> bytes:
    return key.sign_digest(hashlib.sha256(DOMAIN + document).digest())


def infer_kind(index) -> str:
    provider = index.get("provider") if isinstance(index, dict) else None
    if not isinstance(provider, dict):
        return "custom_unsigned"
    if provider.get("id") == OFFICIAL_ID:
        return "official"
    return "custom_signed" if "key" in provider else "custom_unsigned"


def parse_self_pinned(document: bytes) -> tuple[str, dict]:
    """Tooling only: the schema check with the kind inferred from the document
    and a custom provider's key pinned to itself. Returns (kind, index)."""
    try:
        raw = json.loads(document)
    except ValueError:
        raw = None
    kind = infer_kind(raw)
    key = raw["provider"]["key"] if kind == "custom_signed" else None
    try:
        pinned = bytes.fromhex(key) if isinstance(key, str) else None
    except ValueError:
        pinned = None
    return kind, parse_index(document, kind, None, pinned)


# ---- writer ------------------------------------------------------------------------
def _ordered(value: dict, order: tuple) -> dict:
    return {name: value[name] for name in order if name in value} | {k: v for k, v in value.items() if k not in order}


def canonical(index: dict) -> dict:
    """The index with every object's members in canonical order."""
    root = _ordered(index, ROOT)
    if isinstance(root.get("provider"), dict):
        root["provider"] = _ordered(root["provider"], PROVIDER)
    plugins = []
    for plugin in root.get("plugins", []):
        plugin = _ordered(plugin, PLUGIN)
        plugin["authors"] = [_ordered(a, AUTHOR) for a in plugin.get("authors", [])]
        if isinstance(plugin.get("details"), dict):
            plugin["details"] = _ordered(plugin["details"], DETAILS)
        versions = []
        for version in plugin.get("versions", []):
            version = _ordered(version, VERSION)
            files = []
            for file in version.get("files", []):
                file = _ordered(file, FILE)
                if isinstance(file.get("github_release"), dict):
                    file["github_release"] = _ordered(file["github_release"], RELEASE)
                files.append(file)
            version["files"] = files
            versions.append(version)
        plugin["versions"] = versions
        plugins.append(plugin)
    root["plugins"] = plugins
    root["revoked"] = [_ordered(r, REVOKED) for r in root.get("revoked", [])]
    if "blocklist" in root:
        root["blocklist"] = [_ordered(b, BLOCK) for b in root["blocklist"]]
    return root


def render_index(index: dict) -> bytes:
    """Compact canonical UTF-8 JSON, validated as the client would (time checks need a clock and are left out)."""
    document = json.dumps(canonical(index), ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    kind = infer_kind(index)
    pinned = bytes.fromhex(index["provider"]["key"]) if kind == "custom_signed" else None
    parse_index(document, kind, None, pinned)
    return document


def _quoted(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def synthesize_manifest(index: dict, slug: str, version: str, arch: str = "x64") -> bytes:
    """plugin_index_manifest: the canonical schema-3 manifest (native_plugin_manifest_write) for one listed version."""
    plugin = next((p for p in index["plugins"] if p["slug"] == slug), None)
    if plugin is None:
        raise KeyError(slug)
    listed = next((v for v in plugin["versions"] if v["version"] == version), None)
    if listed is None:
        raise KeyError(version)
    file = next((f for f in listed["files"] if f["arch"] == arch), None)
    if file is None:
        raise PluginIndexError("file", f"no {arch} file")
    if listed["abi"] != MANIFEST_ABI:
        raise PluginIndexError("abi", "a manifest describes ABI 5 only")
    permissions = [p for p in PERMISSIONS if p in listed["permissions"]]
    text = f'{{"schema":3,"owner_id":{plugin["owner_id"]},"abi":{listed["abi"]}'
    if listed["min_host_abi"]:
        text += f',"min_host_abi":{listed["min_host_abi"]}'
    if plugin["publisher"]:
        text += f',"publisher":{_quoted(plugin["publisher"])}'
    text += ',"permissions":[' + ",".join(_quoted(p) for p in permissions) + "]"
    text += f',"name":{_quoted(plugin["name"])},"version":{_quoted(version_text(listed["version_code"]))}'
    text += f',"description":{_quoted(plugin["summary"])}'
    text += f',"dll":"{plugin["owner_id"]:016x}-{file["sha256"]}.dll","sha256":"{file["sha256"]}"}}'
    return text.encode("utf-8")


# ---- command line ------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    render = commands.add_parser("render", help="write the canonical compact index")
    render.add_argument("source", type=Path)
    render.add_argument("output", type=Path)
    sign = commands.add_parser("sign", help="write <index>.sig")
    sign.add_argument("--key", type=Path, required=True, help="private JWK")
    sign.add_argument("index", type=Path)
    verify = commands.add_parser("verify", help="check signature, schema and freshness")
    verify.add_argument("--kind", choices=KINDS, required=True)
    verify.add_argument("--public-key", help="130 hex digits: the compiled key, or the pinned key of a custom provider")
    verify.add_argument("--now", type=int, default=None, help="unix time (default: now)")
    verify.add_argument("index", type=Path)
    manifest = commands.add_parser("manifest", help="print the synthesized schema-3 manifest")
    manifest.add_argument("index", type=Path)
    manifest.add_argument("--slug", required=True)
    manifest.add_argument("--version", required=True)
    manifest.add_argument("--arch", choices=ARCHES, default="x64")
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "render":
            source = json.loads(arguments.source.read_text(encoding="utf-8"))
            document = render_index(source)
            arguments.output.write_bytes(document)
            print(f"{arguments.output}: {len(document)} bytes, sha256 {hashlib.sha256(document).hexdigest()}")
        elif arguments.command == "sign":
            key = cng_p256.PrivateKey.from_jwk(arguments.key.read_text(encoding="utf-8"))
            document = arguments.index.read_bytes()
            kind, index = parse_self_pinned(document)
            if kind == "custom_unsigned":
                raise PluginIndexError("provider", "a signed custom index names its key in provider.key")
            if kind == "custom_signed" and bytes.fromhex(index["provider"]["key"]) != key.public_point:
                raise PluginIndexError("key_changed", "provider.key is not this signing key")
            signature = sign_index(document, key)
            Path(str(arguments.index) + ".sig").write_bytes(signature)
            print(f"{arguments.index}.sig: 64 bytes")
        elif arguments.command == "verify":
            document = arguments.index.read_bytes()
            now = arguments.now if arguments.now is not None else int(time.time())
            if arguments.kind == "custom_unsigned":
                index = parse_index(document, arguments.kind, now)
            else:
                if not arguments.public_key:
                    parser.error("--public-key is required for signed kinds")
                point = cng_p256.parse_public_point(arguments.public_key)
                signature = Path(str(arguments.index) + ".sig").read_bytes()
                pinned = point if arguments.kind == "custom_signed" else None
                index = verify_index(document, signature, point, arguments.kind, now, pinned)
            versions = sum(len(p["versions"]) for p in index["plugins"])
            print(f"valid: {len(index['plugins'])} plugins, {versions} versions, {len(index['revoked'])} revoked, "
                  f"sequence {index['sequence']}, expires {index['expires']}")
        else:
            _, index = parse_self_pinned(arguments.index.read_bytes())
            sys.stdout.write(synthesize_manifest(index, arguments.slug, arguments.version, arguments.arch).decode("utf-8"))
            sys.stdout.write("\n")
    except PluginIndexError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except (OSError, cng_p256.CngError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
