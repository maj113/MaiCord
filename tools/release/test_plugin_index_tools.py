#!/usr/bin/env python3
"""Self-contained checks for the catalog index reference tool (format 2).

    python -B tools/release/test_plugin_index_tools.py               # run the checks
    python -B tools/release/test_plugin_index_tools.py --regenerate  # rewrite tests/fixtures/plugin_index/format2

The fixtures are shared with the client's tests: the canonical index document,
its signature by the test-only scalar-one key (public point = the P-256
generator, as in tests/app/update/signed_fixture_helpers.hpp), the install
records, and vectors.txt, whose malformed documents must fail with the same
error code in both implementations. All content is synthetic.
"""

from __future__ import annotations

import hashlib
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cng_p256  # noqa: E402
import plugin_index  # noqa: E402

FIXTURES = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "plugin_index" / "format2"
GX = bytes.fromhex("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296")
GY = bytes.fromhex("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5")
SCALAR_ONE_POINT = b"\x04" + GX + GY
ISSUED = 1760226600
EXPIRES = ISSUED + 30 * 86400
NOW = 1760400000  # the clock vectors.txt assumes


def scalar_one_key() -> cng_p256.PrivateKey:
    """Test-only signer (private scalar 1); never a trust root."""
    return cng_p256.PrivateKey(GX, GY, (1).to_bytes(32, "big"))


def digest(label: str) -> str:
    return hashlib.sha256(label.encode("utf-8")).hexdigest()


def file(label: str, size: int) -> dict:
    return {"sha256": digest(label), "bytes": size}


def preview(label: str, alt: str, width: int = 1440, height: int = 900) -> dict:
    return {"sha256": digest(label), "bytes": 4096, "width": width, "height": height, "alt": alt}


def plugin_version(text: str, released: int, permissions: list, network: list, label: str, size: int,
                   changelog: str = "", api_level: int = 1) -> dict:
    entry = {"version": text, "released": released, "changelog": changelog,
             "commit": hashlib.sha1(label.encode("utf-8")).hexdigest(), "api_level": api_level,
             "permissions": permissions, "network": network, "file": file(label, size)}
    if not changelog:
        del entry["changelog"]
    return entry


def spec() -> dict:
    author = {"name": "Example Author", "github": "example-author"}
    return {
        "format": 2, "sequence": 1760227200, "issued": ISSUED, "expires": EXPIRES,
        "items": [
            {"kind": "effect", "slug": "starfield", "name": "Starfield", "summary": "Slow stars drifting upward.",
             "authors": [{"name": "Example Author"}], "license": "MPL-2.0",
             "source": "https://github.com/example/plugins/tree/main/effects/starfield",
             "categories": ["space"], "details": file("starfield-details", 900),
             "previews": [preview("starfield-preview", "Stars on a dark window", 960, 540)],
             "version": "1.0.0", "released": 1760100000, "file": file("starfield-1.0.0", 3800),
             "cost": "medium", "blend": "add", "uses": [], "knobs": []},
            {"kind": "plugin", "slug": "translate", "name": "Translate", "summary": "Translates messages.",
             "authors": [author, {"name": "Bo Example"}], "license": "MIT", "homepage": "https://example.com/translate",
             "source": "https://github.com/example/plugins/tree/main/plugins/translate",
             "categories": ["utility"],
             "derived_from": {"url": "https://example.com/original", "author": "Ana Example", "license": "MIT"},
             "details": file("translate-details", 2048), "previews": [preview("translate-preview", "The card", 800, 450)],
             "versions": [plugin_version("2.0.0", 1760100000, ["network", "composer"], ["translate.example.com", "*.deepl.example.net"],
                                         "translate-2.0.0", 41984, "Adds a chat bar button.")]},
            {"kind": "theme", "slug": "obsidian", "name": "Obsidian", "summary": "True black for OLED panels.",
             "authors": [{"name": "MaiCord"}], "license": "CC0-1.0",
             "source": "https://github.com/example/plugins/tree/main/themes/obsidian", "categories": ["dark", "oled"],
             "details": file("obsidian-details", 1500), "previews": [preview("obsidian-preview", "Chat in Obsidian")],
             "version": "1.0.1", "released": 1760150000, "changelog": "Better muted text.",
             "file": file("obsidian-1.0.1", 1209), "base": "amoled", "effect": "aurora",
             "swatches": {"background": "#000000", "surface": "#101011", "text": "#ececee", "accent": "#34D399"}},
            {"kind": "plugin", "slug": "clear-urls", "name": "Quiet \"urls\" – Ruhezeit",
             "summary": "Removes tracking parameters.", "authors": [author], "license": "MPL-2.0",
             "source": "https://github.com/example/plugins/tree/main/plugins/clear-urls",
             "categories": ["privacy", "chat"], "details": file("clear-urls-details", 700), "previews": [],
             "versions": [plugin_version("1.1.0", 1760200000, ["message_send"], [], "clear-urls-1.1.0", 9216),
                          plugin_version("1.0.0", 1759000000, ["message_send"], [], "clear-urls-1.0.0", 9000,
                                         "First catalog release.")]},
            {"kind": "effect", "slug": "aurora", "name": "Aurora", "summary": "Curtains of light.",
             "authors": [author], "license": "MPL-2.0",
             "source": "https://github.com/example/plugins/tree/main/effects/aurora", "homepage": "https://example.com/aurora",
             "categories": ["space", "calm"], "details": file("aurora-details", 1200),
             "previews": [preview("aurora-preview", "Green curtains", 960, 540)],
             "version": "1.2.0", "released": 1760120000, "changelog": "A colour knob.", "file": file("aurora-1.2.0", 5200),
             "cost": "light", "blend": "normal", "uses": ["blue_noise", "noise"],
             "knobs": [{"id": "speed", "type": "float", "label": "Drift speed", "min": 0, "max": 2.0, "default": 0.35},
                       {"id": "curtains", "type": "int", "label": "Curtains", "min": 1, "max": 4, "default": 3},
                       {"id": "mirror", "type": "toggle", "label": "Mirrored", "default": True},
                       {"id": "tint", "type": "color", "label": "Colour", "default": "#26FF66"},
                       {"id": "style", "type": "choice", "label": "Style", "choices": ["Soft", "Sharp"], "default": 1}],
             "loop": {"sha256": digest("aurora-loop"), "bytes": 90000, "width": 480, "height": 270, "fps": 15, "seconds": 3}},
            {"kind": "theme", "slug": "warm-paper", "name": "Warm Paper", "summary": "A cream light theme.",
             "authors": [{"name": "MaiCord"}], "license": "CC0-1.0",
             "source": "https://github.com/example/plugins/tree/main/themes/warm-paper", "categories": [],
             "details": file("warm-paper-details", 800), "previews": [preview("warm-paper-preview", "Chat in Warm Paper")],
             "version": "1.0.0", "released": 1760090000, "file": file("warm-paper-1.0.0", 1347), "base": "light"},
        ],
        "revoked": [{"sha256": digest("clear-urls-0.9.0"), "severity": "broken", "reason": "Crashes on empty drafts.",
                     "since": 1760100000}],
        "blocklist": [{"sha256": digest("blocked-build"), "reason": "Known malicious build.", "since": 1760150000}],
    }


def record_targets(index: dict):
    """(kind, slug, version or None, path) of every install record fixture."""
    for item in index["items"]:
        for listed in item["versions"] if item["kind"] == "plugin" else [item]:
            yield item["kind"], item["slug"], listed["version"], FIXTURES / "manifests" / f"{item['kind']}-{item['slug']}-{listed['version']}.json"


def regenerate() -> None:
    (FIXTURES / "manifests").mkdir(parents=True, exist_ok=True)
    document = plugin_index.render_index(spec())
    (FIXTURES / "index.json").write_bytes(document)
    signature_path = FIXTURES / "index.json.sig"
    current = signature_path.read_bytes() if signature_path.exists() else b""
    # ECDSA signatures are randomized: keep a still-valid one so the file only changes with the document.
    if len(current) != 64 or not cng_p256.verify_digest(SCALAR_ONE_POINT, hashlib.sha256(plugin_index.DOMAIN + document).digest(), current):
        signature_path.write_bytes(plugin_index.sign_index(document, scalar_one_key()))
    index = plugin_index.parse_index(document, NOW)
    for kind, slug, version, path in record_targets(index):
        path.write_bytes(plugin_index.install_record(index, kind, slug, version))
    print(f"index.json: {len(document)} bytes")


def vectors():
    """(line, expected, find, replace) from vectors.txt; tab-separated, '#' comments."""
    for number, line in enumerate((FIXTURES / "vectors.txt").read_text(encoding="utf-8").split("\n"), 1):
        line = line.rstrip("\r")
        if not line or line.startswith("#"):
            continue
        expected, find, replace = line.split("\t")
        yield number, expected, find, replace


class PluginIndexToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.document = (FIXTURES / "index.json").read_bytes()
        cls.signature = (FIXTURES / "index.json.sig").read_bytes()

    def code(self, document: bytes, **options) -> str:
        try:
            plugin_index.parse_index(document, options.pop("now", NOW), **options)
        except plugin_index.PluginIndexError as error:
            return error.code
        return "none"

    def test_fixture_is_the_canonical_rendering(self):
        self.assertEqual(plugin_index.render_index(spec()), self.document, "run --regenerate")
        reordered = dict(reversed(list(json.loads(self.document).items())))
        self.assertEqual(plugin_index.render_index(reordered), self.document, "member order is canonical")
        kinds = [(i["kind"], i["slug"]) for i in json.loads(self.document)["items"]]
        self.assertEqual(kinds, [("plugin", "clear-urls"), ("plugin", "translate"), ("theme", "obsidian"),
                                 ("theme", "warm-paper"), ("effect", "aurora"), ("effect", "starfield")])

    def test_fixture_signature_verifies_under_the_index_domain(self):
        index = plugin_index.verify_index(self.document, self.signature, SCALAR_ONE_POINT, NOW)
        self.assertEqual((len(index["items"]), len(index["revoked"]), len(index["blocklist"])), (6, 1, 1))
        self.assertEqual(index["items"][0]["name"], "Quiet \"urls\" – Ruhezeit")
        self.assertEqual(index["digest"], hashlib.sha256(plugin_index.DOMAIN + self.document).hexdigest())
        self.assertEqual(plugin_index.DOMAIN, b"MaiCord plugin index v2\n")

    def test_install_records_match_the_client_fixtures(self):
        index = plugin_index.parse_index(self.document, NOW)
        targets = list(record_targets(index))
        self.assertEqual(len(targets), 7)
        for kind, slug, version, path in targets:
            self.assertEqual(plugin_index.install_record(index, kind, slug, version), path.read_bytes(), path.name)
        self.assertEqual(len(list((FIXTURES / "manifests").iterdir())), 7, "no stale record fixtures")
        record = json.loads(plugin_index.install_record(index, "plugin", "translate"))
        self.assertEqual(record["file"]["name"], f"files/{digest('translate-2.0.0')}.dll")
        self.assertEqual(record["network"], ["translate.example.com", "*.deepl.example.net"])
        self.assertEqual(json.loads(plugin_index.install_record(index, "theme", "obsidian"))["file"]["name"][-5:], ".json")
        self.assertEqual(json.loads(plugin_index.install_record(index, "effect", "aurora"))["file"]["name"][-5:], ".mcfx")
        for bad in (("plugin", "nothing", None), ("plugin", "clear-urls", "9.9.9"), ("theme", "obsidian", "2.0.0")):
            with self.assertRaises(KeyError):
                plugin_index.install_record(index, *bad)

    def test_shared_vectors_fail_with_their_error(self):
        count = 0
        for number, expected, find, replace in vectors():
            document = self.document.decode("utf-8")
            self.assertEqual(document.count(find), 1, f"vectors.txt:{number}: the search text must occur once")
            self.assertIn(expected, plugin_index.ERRORS, f"vectors.txt:{number}")
            mutated = document.replace(find, replace).encode("utf-8")
            with self.assertRaises(plugin_index.PluginIndexError, msg=f"vectors.txt:{number}") as raised:
                if expected == "signature":
                    plugin_index.verify_index(mutated, self.signature, SCALAR_ONE_POINT, NOW)
                else:
                    plugin_index.parse_index(mutated, NOW)
            self.assertEqual(raised.exception.code, expected, f"vectors.txt:{number}: {raised.exception}")
            count += 1
        self.assertGreaterEqual(count, 60)

    def test_list_limits(self):
        index = json.loads(self.document)
        template = index["items"][0]
        def with_items(count):
            copies = [dict(template, slug=f"p{i:07d}") for i in range(count)]
            return json.dumps(dict(index, items=copies), separators=(",", ":")).encode()
        self.assertEqual(self.code(with_items(128)), "none")
        self.assertEqual(self.code(with_items(129)), "limit")
        def with_revoked(count):
            entries = [{"sha256": digest(f"r{i}"), "severity": "broken", "reason": "", "since": 5} for i in range(count)]
            return json.dumps(dict(index, revoked=entries), separators=(",", ":")).encode()
        self.assertEqual(self.code(with_revoked(64)), "none")
        self.assertEqual(self.code(with_revoked(65)), "limit")
        self.assertEqual(self.code(b" " * (plugin_index.MAX_BYTES + 1)), "size")
        self.assertEqual(self.code(b""), "size")
        four = index["items"][0]["versions"] + [dict(index["items"][0]["versions"][-1], version=f"0.{n}.0", released=n) for n in (9, 8)]
        self.assertEqual(self.code(json.dumps(dict(index, items=[dict(template, versions=four)]), separators=(",", ":")).encode()), "none")
        five = four + [dict(four[-1], version="0.7.0", released=7)]
        self.assertEqual(self.code(json.dumps(dict(index, items=[dict(template, versions=five)]), separators=(",", ":")).encode()), "limit")

    def test_time_rules(self):
        self.assertEqual(self.code(self.document, now=EXPIRES), "expired")
        self.assertTrue(plugin_index.parse_index(self.document, EXPIRES, accept_expired=True)["expired"])
        self.assertEqual(self.code(self.document, now=ISSUED - plugin_index.CLOCK_SKEW_SECONDS - 1), "time")
        self.assertEqual(self.code(self.document, now=ISSUED - plugin_index.CLOCK_SKEW_SECONDS), "none")

    def test_signatures(self):
        key, other = cng_p256.PrivateKey.generate(), cng_p256.PrivateKey.generate()
        signature = plugin_index.sign_index(self.document, key)
        self.assertEqual(len(plugin_index.verify_index(self.document, signature, key.public_point, NOW)["items"]), 6)
        update_domain = key.sign_digest(hashlib.sha256(b"MaiCord update manifest v1\n" + self.document).digest())
        format_one = key.sign_digest(hashlib.sha256(b"MaiCord plugin index v1\n" + self.document).digest())
        tampered = bytearray(self.document)
        tampered[self.document.index(b"Obsidian")] ^= 0x20
        for bad_document, bad_signature, point in ((self.document, update_domain, key.public_point),
                                                    (self.document, format_one, key.public_point),
                                                    (self.document, signature, other.public_point),
                                                    (bytes(tampered), signature, key.public_point),
                                                    (self.document, signature[:63], key.public_point)):
            with self.assertRaises(plugin_index.PluginIndexError) as raised:
                plugin_index.verify_index(bad_document, bad_signature, point, NOW)
            self.assertEqual(raised.exception.code, "signature")
        with self.assertRaises(plugin_index.PluginIndexError) as raised:
            plugin_index.verify_index(b" " * (plugin_index.MAX_BYTES + 1), signature, key.public_point, NOW)
        self.assertEqual(raised.exception.code, "size", "size is refused before the signature")

    def test_versions_and_hosts(self):
        self.assertEqual(plugin_index.version_key("0.0.1"), (0, 0, 1))
        self.assertEqual(plugin_index.version_key("999.999.999"), (999, 999, 999))
        for bad in ("1.2", "01.2.3", "1.2.3-preview.1", "1000.0.0", "1.2.3.4", "v1.2.3", ""):
            with self.assertRaises(plugin_index.PluginIndexError, msg=bad):
                plugin_index.version_key(bad)
        for good in ("example.com", "*.example.com", "a-b.example.co.uk"):
            self.assertTrue(plugin_index.network_pattern_valid(good), good)
        for bad in ("localhost", "*", "*.*.example.com", "Example.com", "10.0.0.1", "discord.com", "cdn.discordapp.com",
                    "*.discord.gg", "example", "ex ample.com", "*.com.", ""):
            self.assertFalse(plugin_index.network_pattern_valid(bad), bad)

    def test_command_line(self):
        with tempfile.TemporaryDirectory(prefix="maicord-plugin-index-test-") as directory:
            root = Path(directory)
            key = cng_p256.PrivateKey.generate()
            (root / "key.jwk").write_text(key.to_jwk(), encoding="utf-8")
            (root / "source.json").write_text(json.dumps(spec(), indent=2), encoding="utf-8")
            output = io.StringIO()
            with redirect_stdout(output), redirect_stderr(io.StringIO()):
                self.assertEqual(plugin_index.main(["render", str(root / "source.json"), str(root / "index.json")]), 0)
                self.assertEqual(plugin_index.main(["sign", "--key", str(root / "key.jwk"), str(root / "index.json")]), 0)
                self.assertEqual(plugin_index.main(["verify", "--public-key", key.public_point.hex(),
                                                    "--now", str(NOW), str(root / "index.json")]), 0)
                self.assertEqual(plugin_index.main(["verify", "--public-key", SCALAR_ONE_POINT.hex(),
                                                    "--now", str(NOW), str(root / "index.json")]), 1)
                self.assertEqual(plugin_index.main(["manifest", str(root / "index.json"), "--kind", "plugin",
                                                    "--slug", "clear-urls", "--version", "1.0.0"]), 0)
            self.assertEqual((root / "index.json").read_bytes(), self.document)
            self.assertIn("valid: 6 items (2 plugins, 2 themes, 2 effects), 1 revoked", output.getvalue())
            self.assertIn((FIXTURES / "manifests" / "plugin-clear-urls-1.0.0.json").read_text(encoding="utf-8"),
                          output.getvalue())


if __name__ == "__main__":
    if "--regenerate" in sys.argv[1:]:
        regenerate()
    else:
        unittest.main(verbosity=1)
