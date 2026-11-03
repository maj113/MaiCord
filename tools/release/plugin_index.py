#!/usr/bin/env python3
"""Reference writer and verifier for the MaiCord catalog index (format 2).

The client's verifier mirrors this module: the same strict schema, limits,
display-text rule and error codes, so the shared vectors in
tests/fixtures/plugin_index/format2 give the same result in both. The index
lists plugins, themes and effects; local://prog/catalog-format2.md in the
design notes and the site's CATALOG.md describe the members. Signing uses
Windows CNG through cng_p256 (stdlib only).

    python -B tools/release/plugin_index.py render <source.json> <index.json>
    python -B tools/release/plugin_index.py sign --key <private.jwk> <index.json>
    python -B tools/release/plugin_index.py verify --public-key <hex> [--now <unix>] <index.json>
    python -B tools/release/plugin_index.py manifest <index.json> --kind <kind> --slug <slug> [--version <x.y.z>]

render orders the members canonically (items by kind, then slug) and writes
compact UTF-8 JSON; sign writes <index.json>.sig (64-byte P1363 over SHA-256
of the domain and the document); verify checks the signature, schema and
freshness; manifest prints the install record the client derives for one
listed file.
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

DOMAIN = b"MaiCord plugin index v2\n"
FORMAT = 2
MAX_BYTES = 256 * 1024
MAX_DEPTH = 8
MAX_ITEMS = 128
MAX_VERSIONS = 4
MAX_AUTHORS = 4
MAX_CATEGORIES = 3
MAX_PREVIEWS = 4
MAX_REVOKED = 64
MAX_BLOCKED = 64
MAX_HOSTS = 8
MAX_PERMISSIONS = 32
MAX_VALIDITY_SECONDS = 35 * 86400
CLOCK_SKEW_SECONDS = 600
U64_MAX = 2 ** 64 - 1
TIME_MAX = 2 ** 40

DETAILS_MAX_BYTES = 16 * 1024
PREVIEW_MAX_BYTES = 256 * 1024
LOOP_MAX_BYTES = 2 * 1024 * 1024
FILE_MAX_BYTES = {"plugin": 8 * 1024 * 1024, "theme": 64 * 1024, "effect": 1536 * 1024}
FILE_EXTENSIONS = {"plugin": "dll", "theme": "json", "effect": "mcfx"}
IMAGE_MAX_EXTENT = 4096

SLUG_MAX = 40
CATEGORY_MAX = 24
NAME_MAX = 95
SUMMARY_MAX = 255
AUTHOR_MAX = 64
GITHUB_MAX = 39
LICENSE_MAX = 32
URL_MAX = 256
CHANGELOG_MAX = 512
ALT_MAX = 255
REASON_MAX = 128
VERSION_TEXT_MAX = 23
KNOB_ID_MAX = 24
KNOB_LABEL_MAX = 48
CHOICE_MAX = 16
CHOICE_TEXT_MAX = 24
KNOB_SCALAR_MAX = 10 ** 9
# Per kind: scalars (float, int, toggle), colours and choices.
KNOB_LIMITS = {"scalar": 16, "color": 4, "choice": 8}

KINDS = ("plugin", "theme", "effect")
# MC_PERM_* names in bit order (include/maicord/sdk/maicord_plugin.h).
PERMISSIONS = ("message_send", "read_messages", "message_history", "read_activity", "network", "composer",
               "act_messages", "act_presence", "clipboard", "automated_actions", "persistent_log")
SEVERITIES = ("malicious", "broken", "withdrawn")
BASES = ("dark", "light", "amoled")
COSTS = ("light", "medium", "heavy")
BLENDS = ("normal", "screen", "add")
# Interface 1's effect inputs only: clock and appearance (interface 2) stay in the package's manifest, as every earlier
# client refuses an index that names them.
USES = ("noise", "glyphs", "blue_noise", "pointer", "texture0", "texture1")
KNOB_TYPES = ("float", "int", "toggle", "color", "choice")

# Error codes, named like the client's index errors.
ERRORS = ("size", "signature", "json", "depth", "unknown_field", "duplicate_field", "missing_field", "type", "format",
          "sequence", "time", "expired", "limit", "text", "identifier", "duplicate", "version", "order", "api_level",
          "permission", "choice", "digest", "file", "url", "host")


class PluginIndexError(Exception):
    def __init__(self, code: str, detail: str = ""):
        assert code in ERRORS, code
        super().__init__(f"{code}: {detail}" if detail else code)
        self.code, self.detail = code, detail


class _NotInteger:
    """A JSON integer that has more than 20 digits."""


class _Real:
    """A JSON number with a fraction or an exponent."""

    def __init__(self, text: str):
        self.text = text


def _parse_int(text: str):
    return _NotInteger() if len(text) > 20 else int(text)


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
_VERSION = re.compile(r"(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})")
_DECIMAL = re.compile(r"-?(0|[1-9][0-9]*)(\.[0-9]{1,3})?")
# Discord's own domains (src/app/plugins/http.cpp): no plugin request reaches them.
_DENIED_DOMAINS = ("discord.com", "discordapp.com", "discordapp.net", "discord.gg", "discord.media", "discordcdn.com",
                   "discord.co", "discord.gift", "discord.gifts", "discord.new", "discord.dev", "discord.design",
                   "discordstatus.com", "discordsays.com", "discordmerch.com")


def _utf8(text: str) -> int:
    return len(text.encode("utf-8"))


def _banned(c: int) -> bool:
    return (c < 32 or 127 <= c <= 159 or c == 0x061C or 0x200B <= c <= 0x200F or 0x202A <= c <= 0x202E or
            0x2060 <= c <= 0x2064 or 0x2066 <= c <= 0x2069 or c == 0xFEFF)


def display_valid(text: str, maximum: int, required: bool) -> bool:
    """The display text rule (UTF-8, no C0/C1 controls) plus the bidi and zero-width ban."""
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


def network_pattern_valid(text: str) -> bool:
    """plugin_network_pattern_valid: "host" or "*.host", lower case, never one of Discord's."""
    base = text[2:] if text.startswith("*.") else text
    if not origin_host_valid(base) or base != base.lower():
        return False
    return not any(base == d or base.endswith("." + d) for d in _DENIED_DOMAINS)


def slug_valid(text: str) -> bool:
    return (0 < len(text) <= SLUG_MAX and text[0] in _LOWER_ALNUM and
            all(c in _LOWER_ALNUM or c == "-" for c in text))


def _category_valid(text: str) -> bool:
    return slug_valid(text) and len(text) <= CATEGORY_MAX


def _license_valid(text: str) -> bool:
    return 0 < len(text) <= LICENSE_MAX and all(c in _ALNUM or c in ".+-" for c in text)


def _github_valid(text: str) -> bool:
    return (0 < len(text) <= GITHUB_MAX and text[0] != "-" and text[-1] != "-" and
            all(c in _ALNUM or c == "-" for c in text))


def _knob_id_valid(text: str) -> bool:
    return (0 < len(text) <= KNOB_ID_MAX and text[0] in "abcdefghijklmnopqrstuvwxyz" and
            all(c in _LOWER_ALNUM or c == "_" for c in text))


def _color_valid(text: str) -> bool:
    return len(text) == 7 and text[0] == "#" and all(c in _HEX_ANY for c in text[1:])


def _hex(text: str, nbytes: int) -> bool:
    return len(text) == nbytes * 2 and all(c in _HEX_LOWER for c in text)


def version_key(text: str) -> tuple:
    """(major, minor, patch) of "MAJOR.MINOR.PATCH" (each 0-999, no leading zeros, no suffix)."""
    match = _VERSION.fullmatch(text)
    if not match:
        raise PluginIndexError("version", f"{text!r} is not MAJOR.MINOR.PATCH")
    return tuple(int(match.group(i)) for i in (1, 2, 3))


def file_name(kind: str, sha256: str) -> str:
    """Where a package lives next to the index."""
    return f"files/{sha256}.{FILE_EXTENSIONS[kind]}"


# ---- strict schema ---------------------------------------------------------------
# A check validates one member and returns it in canonical form (an object's members in
# the order its field table lists them). parse_index and render_index share them.
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


def _boolean(value) -> bool:
    if type(value) is not bool:
        raise PluginIndexError("type", "expected true or false")
    return value


def _integer(low: int, high: int, code: str = "limit"):
    def check(value):
        if type(value) is not int:
            raise PluginIndexError("type", "expected an integer")
        if not low <= value <= high:
            raise PluginIndexError(code, f"{value} is outside {low}..{high}")
        return value
    return check


def _decimal(value) -> float | int:
    """An integer, or a number with at most three decimals and no exponent."""
    if type(value) is int:
        pass
    elif isinstance(value, _Real):
        if not _DECIMAL.fullmatch(value.text):
            raise PluginIndexError("type", "expected a number with at most 3 decimals")
        value = float(value.text)
    elif type(value) is float:
        if value != value or abs(value) > 1e9 or round(value, 3) != value:
            raise PluginIndexError("type", "expected a number with at most 3 decimals")
    else:
        raise PluginIndexError("type", "expected a number")
    if abs(value) > KNOB_SCALAR_MAX:
        raise PluginIndexError("limit", "number out of range")
    return value


def _text(maximum: int, required: bool = True):
    def check(value):
        if not display_valid(_string(value), maximum, required):
            raise PluginIndexError("text", f"expected text of {'1' if required else '0'} to {maximum} bytes")
        return value
    return check


def _grammar(valid, code: str):
    def check(value):
        if not valid(_string(value)):
            raise PluginIndexError(code, repr(value)[:60])
        return value
    return check


def _choice(vocabulary: tuple, code: str = "choice"):
    def check(value):
        if _string(value) not in vocabulary:
            raise PluginIndexError(code, repr(value)[:40])
        return value
    return check


def _digest(value) -> str:
    if not _hex(_string(value), 32):
        raise PluginIndexError("digest", "expected 64 lower-case hex digits")
    return value


def _commit(value) -> str:
    if not _hex(_string(value), 20):
        raise PluginIndexError("digest", "expected 40 lower-case hex digits")
    return value


def _list(check, maximum: int, minimum: int = 0, unique: bool = False):
    def run(value):
        entries = _array(value)
        if not minimum <= len(entries) <= maximum:
            raise PluginIndexError("limit", f"expected {minimum} to {maximum} entries")
        result = []
        for index, entry in enumerate(entries):
            try:
                result.append(check(entry))
            except PluginIndexError as error:
                raise PluginIndexError(error.code, f"[{index}]" + (f" {error.detail}" if error.detail else "")) from None
        if unique and len({json.dumps(r, sort_keys=True) for r in result}) != len(result):
            raise PluginIndexError("duplicate")
        return result
    return run


def _obj(fields: list, post=None):
    """fields: (name, check, required) in canonical order. Unknown members are refused."""
    names = {name for name, _, _ in fields}

    def run(value):
        members = _object(value)
        for name in members:
            if name not in names:
                raise PluginIndexError("unknown_field", name)
        missing = [name for name, _, required in fields if required and name not in members]
        if missing:
            raise PluginIndexError("missing_field", ", ".join(missing))
        result = {}
        for name, check, _ in fields:
            if name in members:
                try:
                    result[name] = check(members[name])
                except PluginIndexError as error:
                    raise PluginIndexError(error.code, name + (f": {error.detail}" if error.detail else "")) from None
        if post:
            post(result)
        return result
    return run


def _file(maximum: int):
    return _obj([("sha256", _digest, True), ("bytes", _integer(1, maximum, "file"), True)])


def _author_check():
    return _obj([("name", _text(AUTHOR_MAX), True), ("github", _grammar(_github_valid, "identifier"), False)])


def _derived_check():
    return _obj([("url", _grammar(url_valid, "url"), True), ("author", _text(AUTHOR_MAX), True),
                 ("license", _grammar(_license_valid, "identifier"), True)])


def _preview_check():
    return _obj([("sha256", _digest, True), ("bytes", _integer(1, PREVIEW_MAX_BYTES, "file"), True),
                 ("width", _integer(1, IMAGE_MAX_EXTENT), True), ("height", _integer(1, IMAGE_MAX_EXTENT), True),
                 ("alt", _text(ALT_MAX), True)])


def _loop_check():
    return _obj([("sha256", _digest, True), ("bytes", _integer(1, LOOP_MAX_BYTES, "file"), True),
                 ("width", _integer(1, IMAGE_MAX_EXTENT), True), ("height", _integer(1, IMAGE_MAX_EXTENT), True),
                 ("fps", _integer(1, 60), True), ("seconds", _integer(1, 10), True)])


def _released(value):
    return _integer(1, TIME_MAX, "time")(value)


def _version_check(value) -> str:
    text = _string(value)
    if _utf8(text) > VERSION_TEXT_MAX:
        raise PluginIndexError("version", "too long")
    version_key(text)
    return text


def _host_check(value) -> str:
    if not network_pattern_valid(_string(value)):
        raise PluginIndexError("host", repr(value)[:80])
    return value


def _permission_set(value) -> list:
    names = _list(_choice(PERMISSIONS, "permission"), MAX_PERMISSIONS, unique=True)(value)
    if names != [p for p in PERMISSIONS if p in names]:
        raise PluginIndexError("order", "permissions are listed in bit order")
    return names


def _plugin_version_post(version: dict) -> None:
    if ("network" in version["permissions"]) != bool(version["network"]):
        raise PluginIndexError("permission", "network is listed exactly when hosts are")


def _plugin_version_check():
    return _obj([("version", _version_check, True), ("released", _released, True),
                 ("changelog", _text(CHANGELOG_MAX, False), False), ("commit", _commit, False),
                 ("api_level", _integer(1, 0xFFFF, "api_level"), True), ("permissions", _permission_set, True),
                 ("network", _list(_host_check, MAX_HOSTS, unique=True), True),
                 ("file", _file(FILE_MAX_BYTES["plugin"]), True)], _plugin_version_post)


def _versions(value) -> list:
    versions = _list(_plugin_version_check(), MAX_VERSIONS, 1)(value)
    for newer, older in zip(versions, versions[1:]):
        if version_key(newer["version"]) <= version_key(older["version"]) or newer["released"] < older["released"]:
            raise PluginIndexError("order", "versions are newest first")
    return versions


def _knob_post(knob: dict) -> None:
    kind = knob["type"]
    if kind in ("float", "int"):
        if not knob["min"] < knob["max"] or not knob["min"] <= knob["default"] <= knob["max"]:
            raise PluginIndexError("limit", "expected min < max and min <= default <= max")
    elif kind == "choice" and knob["default"] >= len(knob["choices"]):
        raise PluginIndexError("limit", "the default is not a choice")


_KNOB_COMMON = [("id", _grammar(_knob_id_valid, "identifier"), True), ("type", _choice(KNOB_TYPES), True),
                ("label", _text(KNOB_LABEL_MAX), True)]
_KNOB_BY_TYPE = {
    "float": _obj(_KNOB_COMMON + [("min", _decimal, True), ("max", _decimal, True), ("default", _decimal, True),
                                  ("percent", _boolean, False)], _knob_post),
    "int": _obj(_KNOB_COMMON + [("min", _integer(-KNOB_SCALAR_MAX, KNOB_SCALAR_MAX), True),
                                ("max", _integer(-KNOB_SCALAR_MAX, KNOB_SCALAR_MAX), True),
                                ("default", _integer(-KNOB_SCALAR_MAX, KNOB_SCALAR_MAX), True)], _knob_post),
    "toggle": _obj(_KNOB_COMMON + [("default", _boolean, True)]),
    "color": _obj(_KNOB_COMMON + [("default", _grammar(_color_valid, "text"), True)]),
    "choice": _obj(_KNOB_COMMON + [("choices", _list(_text(CHOICE_TEXT_MAX), CHOICE_MAX, 2, unique=True), True),
                                   ("default", _integer(0, CHOICE_MAX - 1), True)], _knob_post),
}


def _knob_check(value) -> dict:
    kind = _object(value).get("type")
    if kind not in KNOB_TYPES:
        raise PluginIndexError("choice" if "type" in value else "missing_field", "type")
    return _KNOB_BY_TYPE[kind](value)


def _knobs(value) -> list:
    knobs = _list(_knob_check, sum(KNOB_LIMITS.values()))(value)
    if len({k["id"] for k in knobs}) != len(knobs):
        raise PluginIndexError("duplicate", "knob id")
    counts = {"scalar": 0, "color": 0, "choice": 0}
    for knob in knobs:
        counts["scalar" if knob["type"] in ("float", "int", "toggle") else knob["type"]] += 1
    if any(counts[name] > limit for name, limit in KNOB_LIMITS.items()):
        raise PluginIndexError("limit", "too many knobs of one kind")
    return knobs


def _swatches_check():
    return _obj([(name, _grammar(_color_valid, "text"), True) for name in ("background", "surface", "text", "accent")])


_COMMON = [("kind", _choice(KINDS), True), ("slug", _grammar(slug_valid, "identifier"), True),
           ("name", _text(NAME_MAX), True), ("summary", _text(SUMMARY_MAX), True),
           ("authors", _list(_author_check(), MAX_AUTHORS, 1), True),
           ("license", _grammar(_license_valid, "identifier"), True), ("source", _grammar(url_valid, "url"), True),
           ("homepage", _grammar(url_valid, "url"), False),
           ("categories", _list(_grammar(_category_valid, "identifier"), MAX_CATEGORIES, unique=True), True),
           ("derived_from", _derived_check(), False), ("details", _file(DETAILS_MAX_BYTES), True)]


def _data_fields(kind: str) -> list:
    return [("version", _version_check, True), ("released", _released, True),
            ("changelog", _text(CHANGELOG_MAX, False), False), ("file", _file(FILE_MAX_BYTES[kind]), True)]


_ITEM_FIELDS = {
    "plugin": _COMMON + [("previews", _list(_preview_check(), MAX_PREVIEWS), True), ("versions", _versions, True)],
    "theme": _COMMON + [("previews", _list(_preview_check(), MAX_PREVIEWS, 1), True)] + _data_fields("theme") + [
        ("base", _choice(BASES), True), ("effect", _grammar(slug_valid, "identifier"), False),
        ("swatches", _swatches_check(), False)],
    "effect": _COMMON + [("previews", _list(_preview_check(), MAX_PREVIEWS, 1), True)] + _data_fields("effect") + [
        ("cost", _choice(COSTS), True), ("blend", _choice(BLENDS), True),
        ("uses", _list(_choice(USES), len(USES), unique=True), True), ("knobs", _knobs, True),
        ("loop", _loop_check(), False)],
}
_ITEM = {kind: _obj(fields) for kind, fields in _ITEM_FIELDS.items()}


def _item_check(value) -> dict:
    kind = _object(value).get("kind")
    if kind not in KINDS:
        raise PluginIndexError("choice" if "kind" in value else "missing_field", "kind")
    return _ITEM[kind](value)


_REVOKED = _obj([("sha256", _digest, True), ("severity", _choice(SEVERITIES), True),
                 ("reason", _text(REASON_MAX, False), True), ("since", _released, True)])
_BLOCKED = _obj([("sha256", _digest, True), ("reason", _text(REASON_MAX, False), True), ("since", _released, True)])


def _unique_digests(check, maximum: int):
    def run(value):
        entries = _list(check, maximum)(value)
        if len({e["sha256"] for e in entries}) != len(entries):
            raise PluginIndexError("duplicate", "sha256")
        return entries
    return run


def _items(value) -> list:
    items = _list(_item_check, MAX_ITEMS)(value)
    if len({(i["kind"], i["slug"]) for i in items}) != len(items):
        raise PluginIndexError("duplicate", "kind and slug")
    return items


def _root_post(root: dict) -> None:
    if not root["issued"] < root["expires"] <= root["issued"] + MAX_VALIDITY_SECONDS:
        raise PluginIndexError("time")


_ROOT = _obj([("format", _integer(0, U64_MAX, "format"), True), ("sequence", _integer(1, U64_MAX, "sequence"), True),
              ("issued", _integer(1, TIME_MAX, "time"), True), ("expires", _integer(1, TIME_MAX, "time"), True),
              ("items", _items, True), ("revoked", _unique_digests(_REVOKED, MAX_REVOKED), False),
              ("blocklist", _unique_digests(_BLOCKED, MAX_BLOCKED), False)], _root_post)


def _check_root(root, now: int | None) -> dict:
    members = _object(root)
    for member in members.values():
        # The client's reader bounds each member's nesting before visiting it.
        if _depth(member, 2) > MAX_DEPTH:
            raise PluginIndexError("depth")
    if "format" in members and members["format"] != FORMAT:
        raise PluginIndexError("format")
    index = _ROOT(members)
    if now is not None and index["issued"] > now + CLOCK_SKEW_SECONDS:
        raise PluginIndexError("time")
    return index


def parse_index(document: bytes, now: int | None, accept_expired: bool = False) -> dict:
    """Validates an index exactly as the client does, without the signature.

    now=None skips the comparisons with the current time (render uses it).
    Returns the members in canonical form plus "expired" and "digest"."""
    if not document or len(document) > MAX_BYTES:
        raise PluginIndexError("size")
    if document.startswith(b"\xef\xbb\xbf"):
        raise PluginIndexError("json", "byte order mark")
    try:
        text = document.decode("utf-8")
        root = json.loads(text, parse_int=_parse_int, parse_float=_Real, parse_constant=_refuse_constant,
                          object_pairs_hook=_pairs)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise PluginIndexError("json", str(error)) from None
    if type(root) is not dict or _surrogates(root):
        raise PluginIndexError("json")
    index = _check_root(root, now)
    index["expired"] = False
    if now is not None and index["expires"] <= now:
        if not accept_expired:
            raise PluginIndexError("expired")
        index["expired"] = True
    index["digest"] = hashlib.sha256(DOMAIN + document).hexdigest()
    return index


def verify_index(document: bytes, signature: bytes, public_point: bytes, now: int | None,
                 accept_expired: bool = False) -> dict:
    """plugin_index_verify: size, then the signature under the index domain, then the schema."""
    if not document or len(document) > MAX_BYTES:
        raise PluginIndexError("size")
    if len(signature) != 64 or not cng_p256.verify_digest(public_point, hashlib.sha256(DOMAIN + document).digest(), signature):
        raise PluginIndexError("signature")
    return parse_index(document, now, accept_expired)


def sign_index(document: bytes, key: cng_p256.PrivateKey) -> bytes:
    return key.sign_digest(hashlib.sha256(DOMAIN + document).digest())


# ---- writer ------------------------------------------------------------------------
def render_index(index: dict) -> bytes:
    """Compact canonical UTF-8 JSON, validated as the client would (time checks need a clock and are left out).

    Items are ordered by kind, then slug; both lists of withdrawn digests are always written."""
    source = dict(index)
    source["items"] = sorted(source.get("items", []),
                             key=lambda i: (KINDS.index(i["kind"]) if i.get("kind") in KINDS else len(KINDS), i.get("slug", "")))
    source.setdefault("revoked", [])
    source.setdefault("blocklist", [])
    document = json.dumps(_check_root(source, None), ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    parse_index(document, None)
    return document


def _quoted(text: str) -> str:
    return json.dumps(text, ensure_ascii=False)


def install_record(index: dict, kind: str, slug: str, version: str | None = None) -> bytes:
    """plugin_index_manifest: the canonical record of one listed file: where it lives and what the client checks.

    version selects a plugin's version (default: the newest); themes and effects have one."""
    item = next((i for i in index["items"] if i["kind"] == kind and i["slug"] == slug), None)
    if item is None:
        raise KeyError(f"{kind} {slug}")
    listed = item["versions"][0] if kind == "plugin" and version is None else None
    if kind == "plugin":
        listed = listed or next((v for v in item["versions"] if v["version"] == version), None)
    elif version is None or version == item["version"]:
        listed = item
    if listed is None:
        raise KeyError(version)
    file = listed["file"]
    text = (f'{{"kind":"{kind}","slug":"{slug}","version":{_quoted(listed["version"])},"released":{listed["released"]},'
            f'"file":{{"name":"{file_name(kind, file["sha256"])}","sha256":"{file["sha256"]}","bytes":{file["bytes"]}}}')
    if kind == "plugin":
        text += (f',"api_level":{listed["api_level"]},"permissions":[' + ",".join(_quoted(p) for p in listed["permissions"]) +
                 '],"network":[' + ",".join(_quoted(h) for h in listed["network"]) + "]")
    return (text + "}").encode("utf-8")


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
    verify.add_argument("--public-key", required=True, help="130 hex digits: the compiled catalog key")
    verify.add_argument("--now", type=int, default=None, help="unix time (default: now)")
    verify.add_argument("index", type=Path)
    manifest = commands.add_parser("manifest", help="print the install record of one listed file")
    manifest.add_argument("index", type=Path)
    manifest.add_argument("--kind", choices=KINDS, required=True)
    manifest.add_argument("--slug", required=True)
    manifest.add_argument("--version", default=None)
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
            parse_index(document, None)
            Path(str(arguments.index) + ".sig").write_bytes(sign_index(document, key))
            print(f"{arguments.index}.sig: 64 bytes")
        elif arguments.command == "verify":
            document = arguments.index.read_bytes()
            now = arguments.now if arguments.now is not None else int(time.time())
            point = cng_p256.parse_public_point(arguments.public_key)
            signature = Path(str(arguments.index) + ".sig").read_bytes()
            index = verify_index(document, signature, point, now)
            counts = {kind: sum(1 for i in index["items"] if i["kind"] == kind) for kind in KINDS}
            print(f"valid: {len(index['items'])} items ({counts['plugin']} plugins, {counts['theme']} themes, "
                  f"{counts['effect']} effects), {len(index.get('revoked', []))} revoked, "
                  f"sequence {index['sequence']}, expires {index['expires']}")
        else:
            index = parse_index(arguments.index.read_bytes(), None)
            sys.stdout.write(install_record(index, arguments.kind, arguments.slug, arguments.version).decode("utf-8"))
            sys.stdout.write("\n")
    except PluginIndexError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except (OSError, cng_p256.CngError, KeyError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
