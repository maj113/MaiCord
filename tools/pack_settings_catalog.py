#!/usr/bin/env python3
"""Deflate-pack the settings catalog (src/app/settings/catalog_data.inc) into one generated include.

Usage: pack_settings_catalog.py OUTPUT CATALOG_DATA CATALOG_HEADER SCENE_HEADER SCHEMA_HEADER ICON_MANIFEST

catalog_data.inc is C++ the catalog test compiles as constant data: choice lists, nodes built with node(...),
sections and panels. This tool reads exactly that text (no C++ is compiled), resolves the enumerators it names
from their headers and writes one zlib stream plus the sizes src/app/settings/catalog.cpp needs to inflate it
on first use and to point its tables at the result. tests/app/settings/catalog_pack.cpp compiles the same file
and compares every field of the catalog the shipped stream decodes to against it, so a construct this reader
misreads fails the test instead of shipping.

Stream (little endian; a str is its bytes then a NUL, empty meaning no text):
    u8 sections, then a str header each
    u8 panels, then each: str key, title, keywords, summary; u8 icon, section; u16 nodes
    every node of every panel, in panel order: str key, label, description, keywords, action;
        u8 kind, variant; u16 flags, binding, default choice (0xffff: none);
        u8 choice mode: 0 none; 1 text (u8 count, then a str each); 2 numbered labels (u8 set, u16 count)
"""
import pathlib
import re
import sys
import zlib
import optimal_deflate
from font_reference.embed_fixture import embed_arrays, write_if_changed

PACKED_FILE = "settings_catalog_packed.bin"  # beside the include, read by #embed
LABEL_SETS = ["degree", "percent", "decibel", "millisecond", "zoom", "lens"]  # catalog.cpp's numbered label sets
NO_CHOICE = 0xFFFF

TOKEN = re.compile(r"""
    (?P<space>\s+|//[^\n]*|/\*.*?\*/)
  | (?P<string>"(?:[^"\\\n]|\\.)*")
  | (?P<number>0[xX][0-9A-Fa-f]+|\d+)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*|\.[A-Za-z_][A-Za-z0-9_]*)*)
  | (?P<punct>[{}()\[\],;|<>=/*])
""", re.X | re.S)


def fail(message):
    raise SystemExit("pack_settings_catalog.py: " + message)


def tokenize(text):
    tokens, at = [], 0
    while at < len(text):
        match = TOKEN.match(text, at)
        if not match:
            fail("unexpected text at: " + text[at:at + 40].replace("\n", " "))
        at = match.end()
        if match.lastgroup != "space":
            tokens.append((match.lastgroup, match.group()))
    return tokens


def decode_literal(token):
    """The bytes of one C++ string literal: UTF-8 source text, with \\xHH (greedy, as C) and simple escapes."""
    body, out, at = token[1:-1], bytearray(), 0
    simple = {'"': 34, "'": 39, "\\": 92, "n": 10, "t": 9, "0": 0}
    while at < len(body):
        ch = body[at]
        if ch != "\\":
            out += ch.encode("utf-8")
            at += 1
        elif body[at + 1] == "x":
            digits = re.match(r"[0-9A-Fa-f]+", body[at + 2:])
            if not digits or len(digits.group()) > 2:
                fail("hexadecimal escape out of range: " + token)
            out.append(int(digits.group(), 16))
            at += 2 + len(digits.group())
        elif body[at + 1] in simple:
            out.append(simple[body[at + 1]])
            at += 2
        else:
            fail("unsupported escape in " + token)
    return bytes(out)


class Parser:
    """A small expression reader: strings (adjacent ones join), numbers, names, calls, braces, `|` and casts."""

    def __init__(self, tokens):
        self.tokens, self.at = tokens, 0

    def peek(self):
        return self.tokens[self.at] if self.at < len(self.tokens) else (None, None)

    def take(self, value=None):
        kind, text = self.peek()
        if kind is None or (value is not None and text != value):
            fail(f"expected {value!r}, found {text!r}")
        self.at += 1
        return text

    def expression(self):
        left = self.primary()
        while self.peek()[1] == "|":
            self.take()
            left = ("or", left, self.primary())
        return left

    def primary(self):
        kind, text = self.peek()
        if kind == "string":
            parts = []
            while self.peek()[0] == "string":
                parts.append(decode_literal(self.take()))
            return ("string", b"".join(parts))
        if kind == "number":
            return ("number", int(self.take(), 0))
        if text == "{":
            self.take()
            items = []
            while self.peek()[1] != "}":
                items.append(self.expression())
                if self.peek()[1] == ",":
                    self.take()
            self.take("}")
            return ("brace", items)
        if kind == "ident":
            name = self.take()
            if name == "static_cast":
                self.take("<")
                self.take()
                self.take(">")
                self.take("(")
                value = self.expression()
                self.take(")")
                return value
            # `sizeof(a) / sizeof(a[0])` is the only division: it names the element count of an array.
            if name == "sizeof":
                return self.element_count()
            if self.peek()[1] == "(":
                self.take()
                args = []
                while self.peek()[1] != ")":
                    args.append(self.expression())
                    if self.peek()[1] == ",":
                        self.take()
                self.take(")")
                return ("call", name, args)
            return ("name", name)
        fail(f"unexpected {text!r}")

    def element_count(self):
        self.take("(")
        array = self.take()
        self.take(")")
        self.take("/")
        self.take("sizeof")
        self.take("(")
        self.take()
        self.take("[")
        self.take()
        self.take("]")
        self.take(")")
        return ("count", array)


def enumerators(path, enum):
    """name -> value for `enum [class] NAME [: type] { a, b = 3, ... }` in a header."""
    text = re.sub(r"//[^\n]*", "", pathlib.Path(path).read_text(encoding="utf-8"))
    match = re.search(r"enum(?: class)?\s+" + enum + r"\s*(?::\s*\w+)?\s*\{(.*?)\};", text, re.S)
    if not match:
        fail(f"enum {enum} not found in {path}")
    values, next_value = {}, 0
    for item in match.group(1).split(","):
        item = item.strip()
        if not item:
            continue
        name, _, value = (part.strip() for part in item.partition("="))
        if value:
            shift = re.fullmatch(r"(\d+)u?\s*<<\s*(\d+)", value)
            next_value = int(shift[1]) << int(shift[2]) if shift else int(value, 0)
        values[name] = next_value
        next_value += 1
    return values


def icons(path):
    names = re.findall(r"^MAICORD_ICON\((\w+),", pathlib.Path(path).read_text(encoding="utf-8"), re.M)
    return {name: index for index, name in enumerate(names)}


def parse_statements(text):
    """name -> parsed initialiser of each `[static] constexpr TYPE NAME[]{...};` in the data file."""
    tokens = tokenize(text)
    parser, tables, at = Parser(tokens), {}, 0
    while at < len(tokens):
        if tokens[at][1] == "static_assert":
            while tokens[at][1] != ";":
                at += 1
            at += 1
            continue
        start = at
        while tokens[at][1] != "[":
            at += 1
        name = tokens[at - 1][1]
        if tokens[start][1] not in ("static", "constexpr"):
            fail("unexpected statement at " + tokens[start][1])
        at += 2  # [ ]
        parser.at = at
        tables[name] = parser.expression()
        parser.take(";")
        at = parser.at
    return tables


class Catalog:
    def __init__(self, data, binding, kind, variant, flags, icon):
        self.tables = parse_statements(data)
        self.enums = {"SettingBinding": binding, "K": kind, "UiSettingsControlKind": kind,
                      "UiSettingsVariant": variant, "UiIcon": icon}
        self.names = {"none": NO_CHOICE, "dynamic": flags["ui_settings_schema_dynamic"],
                      "hidden": flags["ui_settings_schema_search_hidden"],
                      "ui_settings_schema_marks": flags["ui_settings_schema_marks"],
                      "ui_settings_schema_dynamic": flags["ui_settings_schema_dynamic"],
                      "ui_settings_schema_search_hidden": flags["ui_settings_schema_search_hidden"]}

    def value(self, node):
        if node[0] == "number":
            return node[1]
        if node[0] == "or":
            return self.value(node[1]) | self.value(node[2])
        if node[0] == "name":
            name = node[1]
            if "::" in name:
                enum, _, member = name.partition("::")
                return self.enums[enum][member]
            if name in self.names:
                return self.names[name]
        fail(f"cannot evaluate {node!r}")

    @staticmethod
    def text(node):
        if node == ("brace", []):
            return b""
        if node[0] == "call" and node[1] == "utf8_literal":
            return node[2][0][1]
        fail(f"expected text, found {node!r}")

    def choices(self, node):
        if node == ("brace", []):
            return None
        source, count = node[1]
        count = self.count(count)
        if source[0] != "name":
            fail(f"choice source {source!r}")
        name = source[1]
        if name.endswith("_labels.values"):
            return ("labels", LABEL_SETS.index(name[:-len("_labels.values")]), count)
        table = self.tables[name]
        if table[0] != "brace":
            fail(f"{name} is not a list")
        if count > len(table[1]):
            fail(f"{name} has {len(table[1])} entries, {count} asked")
        return ("text", [self.text(item) for item in table[1][:count]])

    def count(self, node):
        if node[0] == "number":
            return node[1]
        if node[0] == "count":
            return len(self.tables[node[1]][1])
        if node[0] == "name" and node[1] == "kBackgroundEffectCount":
            return len(self.tables["background_effects"][1])
        fail(f"cannot count {node!r}")

    def node(self, call):
        if call[0] != "call" or call[1] != "node":
            fail(f"expected node(...), found {call!r}")
        args = call[2] + [None] * (11 - len(call[2]))
        key, kind, label, description, keywords = args[0], args[1], args[2], args[3], args[4]
        binding = self.value(args[5]) if args[5] else 0
        default = self.value(args[7]) if args[7] else NO_CHOICE
        action = self.text(args[8]) if args[8] else b""
        flags = self.value(args[9]) if args[9] else 0
        variant = self.value(args[10]) if args[10] else 0
        choices = self.choices(args[6]) if args[6] else None
        record = {"strings": [self.text(item) for item in (key, label, description, keywords)] + [action],
                  "numbers": (self.value(kind), variant, flags, binding, default), "choices": choices}
        if any(len(text) > 255 for text in record["strings"]) or binding > 0xFFFE or default > 0xFFFF or flags > 0xFFFF:
            fail("a node field is out of range")
        return record

    def build(self):
        sections = [self.text(item[1][0]) for item in self.tables["sections"][1]]
        panels, nodes = [], []
        for item in self.tables["panels"][1]:
            fields = item[1]
            key, title, keywords = (self.text(field) for field in fields[:3])
            icon, section = self.value(fields[3]), self.value(fields[4])
            list_node = fields[5]
            members = []
            if list_node != ("brace", []):
                array, size = list_node[1]
                if self.count(size) != len(self.tables[array[1]][1]):
                    fail(f"panel {key!r} lists part of {array[1]}")
                members = [self.node(call) for call in self.tables[array[1]][1]]
            summary = self.text(fields[6]) if len(fields) > 6 else b""
            panels.append((key, title, keywords, summary, icon, section, len(members)))
            nodes.extend(members)
        return sections, panels, nodes


def serialise(sections, panels, nodes):
    out = bytearray()

    def text(value):
        if b"\0" in value:
            fail("text holds a NUL")
        out.extend(value + b"\0")

    out.append(len(sections))
    for header in sections:
        text(header)
    out.append(len(panels))
    for key, title, keywords, summary, icon, section, count in panels:
        for value in (key, title, keywords, summary):
            text(value)
        out += bytes((icon, section)) + count.to_bytes(2, "little")
    choice_texts = 0
    for record in nodes:
        for value in record["strings"]:
            text(value)
        kind, variant, flags, binding, default = record["numbers"]
        out += bytes((kind, variant)) + b"".join(number.to_bytes(2, "little") for number in (flags, binding, default))
        choices = record["choices"]
        if not choices:
            out.append(0)
        elif choices[0] == "text":
            out += bytes((1, len(choices[1])))
            for value in choices[1]:
                text(value)
            choice_texts += len(choices[1])
        else:
            out += bytes((2, choices[1])) + choices[2].to_bytes(2, "little")
    return bytes(out), choice_texts


def pack(data):
    """The smallest zlib stream of `data`: the repository's size-optimal encoder (about two seconds for this
    stream), or zlib's own when that is smaller."""
    best = optimal_deflate.zlib_stream(data)
    for mem in (8, 9):
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED):
            compressor = zlib.compressobj(9, zlib.DEFLATED, 15, mem, strategy)
            candidate = compressor.compress(data) + compressor.flush()
            if len(candidate) < len(best):
                best = candidate
    assert zlib.decompress(best) == data
    return best


def generate(data_path, binding_header, scene_header, schema_header, icon_manifest):
    catalog = Catalog(pathlib.Path(data_path).read_text(encoding="utf-8"),
                      enumerators(binding_header, "SettingBinding"),
                      enumerators(scene_header, "UiSettingsControlKind"),
                      enumerators(scene_header, "UiSettingsVariant"),
                      enumerators(schema_header, "UiSettingsSchemaFlag"), icons(icon_manifest))
    sections, panels, nodes = catalog.build()
    raw, choice_texts = serialise(sections, panels, nodes)
    packed = pack(raw)
    lines = ["// Generated by tools/pack_settings_catalog.py; do not edit.",
             f"constexpr u32 kSettingsCatalogSections={len(sections)};",
             f"constexpr u32 kSettingsCatalogPanels={len(panels)};",
             f"constexpr u32 kSettingsCatalogNodes={len(nodes)};",
             f"constexpr u32 kSettingsCatalogChoices={choice_texts};",
             f"constexpr u32 kSettingsCatalogBytes={len(raw)};",
             embed_arrays([("constexpr u8 kSettingsCatalogPacked[]", PACKED_FILE)]).rstrip("\n")]
    return "\n".join(lines) + "\n", packed


def main():
    output = pathlib.Path(sys.argv[1])
    text, packed = generate(*sys.argv[2:7])
    output.parent.mkdir(parents=True, exist_ok=True)
    write_if_changed(output.with_name(PACKED_FILE), packed)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)


if __name__ == "__main__":
    main()
