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

The catalog publishes four signed indexes of one source, one per reader: INDEX_4, which the Linux client reads
(reader_source(source, 4): an effect that names a "universal" package lists it, the interface 4 package whose GLSL
section the GLES backend runs, as its file with "interface": 4); INDEX_3 (every item; any item may name the interface
it needs in "interface"), which MaiCord 0.6.4 and later on Windows read, and which keeps the DXBC package of every
effect: their reader ignores members and item kinds it doesn't know, so later fields need no further split.
"universal" and a plugin version's "linux" (the shared object built from the same source, files/<sha256>.so) exist in
the source only (check_source): reader 4 lists "linux" as the version's file, and no other reader sees either member.
FULL_INDEX, for 0.6.3,
is reader_source(source, 2) one sequence lower: its reader takes "interface"
on effects only, so themes and plugins that name one are left out. LEGACY_INDEX,
for 0.6.0 to 0.6.2, is reader_source(source, 1) two sequences lower: their
reader refuses "interface" anywhere and their package reader any .mcfx
interface but 1. parse_index(reader=) validates a document as each reader does;
render_index always refuses unknown members, so a typo in a source never ships.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
import time
import zlib
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
INDEX_4 = "index-4.json"
INDEX_3 = "index-3.json"
FULL_INDEX = "index-full.json"
LEGACY_INDEX = "index.json"
# (file, reader, sequence offset): each newer client's index is signed higher, so every update moves forward.
INDEXES = ((INDEX_4, 4, 3), (INDEX_3, 3, 2), (FULL_INDEX, 2, 1), (LEGACY_INDEX, 1, 0))
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


# The reader a validation runs as (INDEXES) and whether unknown members are refused: readers 1 and 2 refuse them,
# readers 3 and 4 skip them, and render_index refuses them for every reader. "source" is the catalog's own source
# (check_source), which alone may name an effect's "universal" package. Set by _reading().
_reader = {"level": 4, "strict": False, "source": False}


class _reading:
    def __init__(self, level: int, strict: bool, source: bool = False):
        self.state = {"level": level, "strict": strict or level < 3, "source": source}

    def __enter__(self):
        self.saved = dict(_reader)
        _reader.update(self.state)

    def __exit__(self, *_):
        _reader.update(self.saved)


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


# Any repeated name. The client refuses a repeated member it knows and never reads one it skips; render never writes one.
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


PAYLOAD_EXTENSIONS = {"windows": "dll", "linux": "so"}  # a plugin's file by the system it runs on


def file_name(kind: str, sha256: str, platform: str = "windows") -> str:
    """Where a package lives next to the index. A plugin has a file per platform (PAYLOAD_EXTENSIONS)."""
    extension = PAYLOAD_EXTENSIONS[platform] if kind == "plugin" else FILE_EXTENSIONS[kind]
    return f"files/{sha256}.{extension}"


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
    """fields: (name, check, required) in canonical order. Unknown members are refused by a strict reader and
    skipped (left out of the result) by reader 3."""
    names = {name for name, _, _ in fields}

    def run(value):
        members = _object(value)
        for name in members:
            if name not in names and _reader["strict"]:
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
    check = _obj([("version", _version_check, True), ("released", _released, True),
                  ("changelog", _text(CHANGELOG_MAX, False), False), ("commit", _commit, False),
                  ("api_level", _integer(1, 0xFFFF, "api_level"), True), ("permissions", _permission_set, True),
                  ("network", _list(_host_check, MAX_HOSTS, unique=True), True),
                  ("file", _file(FILE_MAX_BYTES["plugin"]), True),
                  ("linux", _file(FILE_MAX_BYTES["plugin"]), False)], _plugin_version_post)

    def run(value):
        if isinstance(value, dict) and "linux" in value and not _reader["source"]:
            # A member no reader defines: strict readers refuse it, the others skip it.
            if _reader["strict"]:
                raise PluginIndexError("unknown_field", "linux")
            value = {name: member for name, member in value.items() if name != "linux"}
        return check(value)
    return run


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


# What the item needs of the client; any kind may name it (reader 3). Reader 2 takes it on effects only, reader 1
# nowhere (_item_check).
_INTERFACE = ("interface", _integer(1, 0xFFFF, "api_level"), False)
_ITEM_FIELDS = {
    "plugin": _COMMON + [("previews", _list(_preview_check(), MAX_PREVIEWS), True), ("versions", _versions, True),
                         _INTERFACE],
    "theme": _COMMON + [("previews", _list(_preview_check(), MAX_PREVIEWS, 1), True)] + _data_fields("theme") + [
        ("base", _choice(BASES), True), ("effect", _grammar(slug_valid, "identifier"), False),
        ("swatches", _swatches_check(), False), _INTERFACE],
    "effect": _COMMON + [("previews", _list(_preview_check(), MAX_PREVIEWS, 1), True)] + _data_fields("effect") + [
        ("cost", _choice(COSTS), True), ("blend", _choice(BLENDS), True),
        ("uses", _list(_choice(USES), len(USES), unique=True), True), ("knobs", _knobs, True),
        ("loop", _loop_check(), False), _INTERFACE, ("universal", _file(FILE_MAX_BYTES["effect"]), False)],
}
_ITEM = {kind: _obj(fields) for kind, fields in _ITEM_FIELDS.items()}


def _item_check(value) -> dict | None:
    """An item, or None for one of a kind reader 3 doesn't know (it skips it)."""
    kind = _object(value).get("kind")
    if kind not in KINDS:
        if type(kind) is str and not _reader["strict"]:
            return None
        raise PluginIndexError("choice" if "kind" in value else "missing_field", "kind")
    if "interface" in value and (_reader["level"] == 1 or (_reader["level"] == 2 and kind != "effect")):
        raise PluginIndexError("unknown_field", "interface")
    if "universal" in value and not _reader["source"]:
        # A member no reader defines: strict readers refuse it, the others skip it.
        if _reader["strict"]:
            raise PluginIndexError("unknown_field", "universal")
        value = {name: member for name, member in value.items() if name != "universal"}
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
    items = [item for item in _list(_item_check, MAX_ITEMS)(value) if item is not None]
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


def parse_index(document: bytes, now: int | None, accept_expired: bool = False, reader: int = 3) -> dict:
    """Validates an index exactly as the client of `reader` (INDEXES) does, without the signature.

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
    with _reading(reader, False):
        index = _check_root(root, now)
    index["expired"] = False
    if now is not None and index["expires"] <= now:
        if not accept_expired:
            raise PluginIndexError("expired")
        index["expired"] = True
    index["digest"] = hashlib.sha256(DOMAIN + document).hexdigest()
    return index


def verify_index(document: bytes, signature: bytes, public_point: bytes, now: int | None,
                 accept_expired: bool = False, reader: int = 3) -> dict:
    """plugin_index_verify: size, then the signature under the index domain, then the schema."""
    if not document or len(document) > MAX_BYTES:
        raise PluginIndexError("size")
    if len(signature) != 64 or not cng_p256.verify_digest(public_point, hashlib.sha256(DOMAIN + document).digest(), signature):
        raise PluginIndexError("signature")
    return parse_index(document, now, accept_expired, reader)


def sign_index(document: bytes, key: cng_p256.PrivateKey) -> bytes:
    return key.sign_digest(hashlib.sha256(DOMAIN + document).digest())


# ---- writer ------------------------------------------------------------------------
def render_index(index: dict, reader: int = 3) -> bytes:
    """Compact canonical UTF-8 JSON for `reader`, validated as that client would (time checks need a clock and are
    left out); unknown members are refused whatever the reader.

    Items are ordered by kind, then slug; both lists of withdrawn digests are always written."""
    source = dict(index)
    source["items"] = sorted(source.get("items", []),
                             key=lambda i: (KINDS.index(i["kind"]) if i.get("kind") in KINDS else len(KINDS), i.get("slug", "")))
    source.setdefault("revoked", [])
    source.setdefault("blocklist", [])
    with _reading(reader, True):
        document = json.dumps(_check_root(source, None), ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    parse_index(document, None, reader=reader)
    return document


def check_source(index: dict) -> dict:
    """Validates the catalog's index source: the members every reader knows, plus an effect's "universal" package
    (a file record like "file", for the interface 4 package of the same version; the package itself is checked by
    check_universal) and a plugin version's "linux" payload (a file record for the shared object built from the same
    source: the same API level, permissions and hosts as the DLL). Returns the canonical form."""
    with _reading(4, True, True):
        return _check_root(dict(index, revoked=index.get("revoked", []), blocklist=index.get("blocklist", [])), None)


def _with_universal(index: dict, reader: int) -> dict:
    """Readers from 4 list an effect's "universal" package, the one with the GLSL section, as its file and name interface
    4 for it; the others never see the member (and keep the DXBC package of the item)."""
    items = []
    for item in index.get("items", []):
        if "universal" in item:
            rest = {k: v for k, v in item.items() if k != "universal"}
            item = dict(rest, file=item["universal"], interface=4) if reader >= 4 else rest
        items.append(item)
    return dict(index, items=items)


def _with_linux(index: dict, reader: int) -> dict:
    """Readers from 4 (the Linux client) list a plugin version's "linux" payload, its shared object, as the version's
    file, and leave out the versions without one (a plugin with none has nothing to run there). The other readers never
    see the member and keep the DLL of every version."""
    items = []
    for item in index.get("items", []):
        if item.get("kind") == "plugin":
            versions = []
            for version in item.get("versions", []):
                if reader < 4:
                    versions.append({k: v for k, v in version.items() if k != "linux"})
                elif "linux" in version:
                    versions.append(dict({k: v for k, v in version.items() if k not in ("linux", "file")}, file=version["linux"]))
            if not versions:
                continue
            item = dict(item, versions=versions)
        items.append(item)
    return dict(index, items=items)


def reader_source(index: dict, reader: int) -> dict:
    """The index source for `reader` (INDEXES). Reader 4 (the Linux client) lists an effect's "universal" package as
    its file at interface 4, and a plugin version's "linux" shared object as its file (a plugin version without one is
    left out); 3 reads the rest of the source as it is (an item names the interface it needs on any kind,
    and an effect keeps its DXBC package). Reader 2 (0.6.3) takes "interface" on
    effects only: plugins and themes that name an interface above 1 are left out, and the others lose the member.
    Reader 1 (0.6.0 to 0.6.2) takes it nowhere and runs .mcfx interface 1 only: effects that need more are left out
    too, with a theme's link to one. The other items are unchanged."""
    index = _with_linux(_with_universal(index, reader), reader)
    if reader >= 3:
        return index
    kept = ("effect",) if reader == 2 else ()
    held = {(i["kind"], i["slug"]) for i in index.get("items", [])
            if i.get("interface", 1) > 1 and i.get("kind") not in kept}
    items = []
    for item in index.get("items", []):
        if (item.get("kind"), item.get("slug")) in held:
            continue
        if item.get("kind") not in kept:
            item = {k: v for k, v in item.items() if k != "interface"}
        if item.get("kind") == "theme" and ("effect", item.get("effect")) in held:
            item = {k: v for k, v in item.items() if k != "effect"}
        items.append(item)
    return dict(index, items=items)


# ---- the interface 4 package ------------------------------------------------------------------------------------
_SECTIONS = {1: "MANIFEST", 2: "DXBC", 3: "TEXTURE0", 4: "TEXTURE1", 5: "SOURCE", 6: "PREVIEW", 7: "TEXTURE2",
             8: "TEXTURE3", 9: "SHADERS", 10: "GLSL"}
GLSL_ENTRY_HEADER = 16
GLSL_PACKED_MAX, GLSL_RAW_MAX, GLSL_BINDING_MAX = 64 * 1024, 256 * 1024, 64


def _package_sections(package: bytes, what: str) -> dict:
    """The sections of a .mcfx by name; the container as mcfx_parse reads it (ascending types, packed back to back)."""
    def bad(detail: str):
        return PluginIndexError("file", f"{what}: {detail}")
    if len(package) < 16 or package[:4] != b"MCFX":
        raise bad("not a .mcfx package")
    format_, count, total, reserved = struct.unpack_from("<HHII", package, 4)
    if format_ != 1 or not 1 <= count <= len(_SECTIONS) or total != len(package) or reserved or len(package) < 16 + 16 * count:
        raise bad("the container header is wrong")
    sections, at, last = {}, 16 + 16 * count, 0
    for i in range(count):
        kind, offset, size, zero = struct.unpack_from("<IIII", package, 16 + 16 * i)
        if kind not in _SECTIONS or kind <= last or zero or offset != at or size > len(package) - at:
            raise bad("the section table is wrong")
        sections[_SECTIONS[kind]] = package[offset:offset + size]
        last, at = kind, at + size
    if at != len(package):
        raise bad("the last section does not end the file")
    return sections


def _dxbc_count(blob: bytes, what: str) -> int:
    """The DXBC containers back to back in a SHADERS section (each states its own size at byte 24)."""
    count, at = 0, 0
    while at < len(blob):
        size = struct.unpack_from("<I", blob, at + 24)[0] if len(blob) - at >= 28 else 0
        if blob[at:at + 4] != b"DXBC" or size < 32 or size > len(blob) - at:
            raise PluginIndexError("file", f"{what}: the SHADERS section is not a run of DXBC containers")
        count, at = count + 1, at + size
    return count


def check_universal(plain: bytes, universal: bytes) -> None:
    """What an effect's "universal" package must be next to its "file": the same package with the manifest naming
    interface 4 and a GLSL section that holds one well-formed entry per shader (u16 count, u16 0, then glsl_shader.hpp's
    entries in DXBC order: pixel, or vertex where a pass names the shader as its "vs"). Every other section is byte for
    byte the plain package's, so one effect runs on both systems; the C++ mcfx_parse is the oracle. Raises
    PluginIndexError("file")."""
    old, new = _package_sections(plain, "file"), _package_sections(universal, "universal")
    if "GLSL" in old or "GLSL" not in new:
        raise PluginIndexError("file", "the universal package carries the GLSL section and the plain one does not")
    if set(old) - {"GLSL"} != set(new) - {"GLSL"}:
        raise PluginIndexError("file", "the universal package's sections are not the plain package's")
    for name in old:
        if name != "MANIFEST" and old[name] != new[name]:
            raise PluginIndexError("file", f"the universal package's {name} differs from the plain package's")
    try:
        manifest = {"plain": json.loads(old["MANIFEST"].decode("utf-8")), "universal": json.loads(new["MANIFEST"].decode("utf-8"))}
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise PluginIndexError("file", "a manifest is not JSON") from None
    if manifest["plain"].get("interface") not in (1, 2, 3) or manifest["universal"].get("interface") != 4:
        raise PluginIndexError("file", "the plain package is interface 1 to 3 and the universal one interface 4")
    if {k: v for k, v in manifest["plain"].items() if k != "interface"} != {
            k: v for k, v in manifest["universal"].items() if k != "interface"}:
        raise PluginIndexError("file", "the manifests differ in more than the interface")
    shaders = 1 + (_dxbc_count(old["SHADERS"], "file") if "SHADERS" in old else 0)
    vertex = {p["vs"] for p in manifest["plain"].get("passes", []) if "vs" in p}
    section = new["GLSL"]
    if len(section) < 4 or struct.unpack_from("<HH", section) != (shaders, 0):
        raise PluginIndexError("file", f"the GLSL section does not hold {shaders} shaders")
    at = 4
    for i in range(shaders):
        if len(section) - at < GLSL_ENTRY_HEADER:
            raise PluginIndexError("file", f"GLSL shader {i} is cut short")
        size, stage, rows, z0, z1, raw, packed = struct.unpack_from("<IBBBBII", section, at)
        if (size != GLSL_ENTRY_HEADER + 4 * rows + packed or size > len(section) - at or z0 or z1 or
                stage != (2 if i in vertex else 1) or rows > GLSL_BINDING_MAX or not 0 < raw <= GLSL_RAW_MAX or
                not 0 < packed <= GLSL_PACKED_MAX):
            raise PluginIndexError("file", f"GLSL shader {i} is malformed")
        inflater = zlib.decompressobj()
        try:
            text = inflater.decompress(section[at + GLSL_ENTRY_HEADER + 4 * rows:at + size], raw + 1)
        except zlib.error:
            raise PluginIndexError("file", f"GLSL shader {i} is not a zlib stream") from None
        if len(text) != raw or not inflater.eof or not text.startswith(b"#version 310 es\n"):
            raise PluginIndexError("file", f"GLSL shader {i} is not {raw} bytes of GLSL ES 3.10")
        at += size
    if at != len(section):
        raise PluginIndexError("file", "the GLSL section has bytes after its last shader")


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
    verify.add_argument("--reader", type=int, choices=(1, 2, 3, 4), default=3,
                        help="the client the file is for: 4 index-4.json, 3 index-3.json, 2 index-full.json, 1 index.json")
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
            index = verify_index(document, signature, point, now, reader=arguments.reader)
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
