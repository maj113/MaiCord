#!/usr/bin/env python3
"""Self-contained checks for the plugin index reference tool.

    python -B tools/release/test_plugin_index_tools.py               # run the checks
    python -B tools/release/test_plugin_index_tools.py --regenerate  # rewrite tests/fixtures/plugin_index

The fixtures are shared with the client's tests (tests/plugin_index.cpp): the
canonical index documents, their signatures by the test-only scalar-one key
(public point = the P-256 generator, as in tests/update_signed_fixture_helpers.hpp),
the manifests the client synthesizes, and vectors.txt, whose malformed
documents must fail with the same error code in both implementations. All
content is synthetic.
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

FIXTURES = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "plugin_index"
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


def commit(label: str) -> str:
    return hashlib.sha1(label.encode("utf-8")).hexdigest()


def file(arch: str, label: str, size: int, **location) -> dict:
    return {"arch": arch, "sha256": digest(label), "bytes": size, **location}


def version(text: str, permissions: list, files: list, released: int, changelog: str, commit_label: str | None = None,
            abi: int = 5, min_host_abi: int = 5, max_host_abi: int = 0) -> dict:
    entry = {"version": text, "version_code": plugin_index.version_code(text), "abi": abi,
             "min_host_abi": min_host_abi, "max_host_abi": max_host_abi, "permissions": permissions, "files": files,
             "released": released}
    if commit_label:
        entry["commit"] = commit(commit_label)
    entry["changelog"] = changelog
    return entry


def official_spec() -> dict:
    return {
        "format": 1,
        "provider": {"id": "maicord", "name": "MaiCord plugins", "homepage": "https://plugins.example.com/maicord"},
        "sequence": 1760227200, "issued": ISSUED, "expires": EXPIRES,
        "plugins": [
            {"slug": "showcase", "owner_id": 0x4D43534844574341, "name": "Showcase",
             "summary": "SDK sample: message log, @username suffixes, quick reply, a gradient background.",
             "publisher": "MaiCord", "authors": [{"name": "Example Author", "github": "example-author"}],
             "license": "MPL-2.0", "homepage": "https://plugins.example.com/maicord/showcase",
             "categories": ["chat", "appearance"], "disclosures": [],
             "details": {"sha256": digest("showcase-details"), "bytes": 3172},
             "versions": [
                 version("1.1.0", ["settings", "menus", "ui_slots", "store", "timer"],
                         [file("x64", "showcase-1.1.0-x64", 49152)], 1760200000, "Adds a timer-driven status line.",
                         "showcase-1.1.0"),
                 version("1.0.0", ["settings", "menus", "ui_slots", "store"],
                         [file("x64", "showcase-1.0.0-x64", 48640)], 1759000000, "First catalog release.",
                         "showcase-1.0.0"),
             ]},
            {"slug": "quiet-hours", "owner_id": 0x1122334455667788, "name": "Quiet \"hours\" – Ruhezeit",
             "summary": "Mutes notification sounds on a schedule.", "publisher": "",
             "authors": [{"name": "Ana Example", "github": "ana-example"}, {"name": "Bo Example"}],
             "license": "MIT", "homepage": "https://example.com/quiet-hours", "categories": ["notifications"],
             "disclosures": [],
             "versions": [
                 version("2.0.0", ["settings", "store", "timer", "notify"], [file("x64", "quiet-hours-2.0.0-x64", 20480)],
                         1760100000, "", "quiet-hours-2.0.0"),
                 version("1.4.2", ["settings", "store", "timer"], [file("x64", "quiet-hours-1.4.2-x64", 18944)],
                         1759500000, "Fixes the end time.", "quiet-hours-1.4.2", max_host_abi=7),
                 version("1.4.1", ["settings", "store", "timer"], [file("x64", "quiet-hours-1.4.1-x64", 18432)],
                         1759400000, "Schedules per weekday.", "quiet-hours-1.4.1", max_host_abi=5),
             ]},
            {"slug": "dev-tools", "owner_id": 0xFEDCBA9876543210, "name": "Dev tools",
             "summary": "Inspector panes for plugin authors.", "publisher": "Example Labs",
             "authors": [{"name": "Example Labs", "github": "example-labs"}], "license": "Apache-2.0",
             "homepage": "https://example.com/dev-tools", "categories": ["developer", "utility"],
             "disclosures": ["network", "files"],
             "versions": [
                 version("0.3.0", ["settings", "read_context", "clipboard"],
                         [file("x64", "dev-tools-0.3.0-x64", 65536), file("arm64", "dev-tools-0.3.0-arm64", 70000)],
                         1760150000, "Needs a host with ABI 6.", "dev-tools-0.3.0", min_host_abi=6),
             ]},
        ],
        "revoked": [{"owner_id": 0x1122334455667788, "sha256": digest("quiet-hours-1.3.0-x64"), "severity": "broken",
                     "reason": "Crashes when the schedule spans midnight.", "since": 1760100000}],
        "blocklist": [{"sha256": digest("blocked-build"), "reason": "Known malicious build.", "since": 1760150000}],
    }


def custom_spec() -> dict:
    release = lambda tag, asset: {"repository": "example/plugins", "tag": tag, "asset": asset}  # noqa: E731
    return {
        "format": 1,
        "provider": {"id": "example-plugins", "name": "Example plugins", "homepage": "https://plugins.example.net/",
                     "key": SCALAR_ONE_POINT.hex()},
        "sequence": 42, "issued": ISSUED, "expires": EXPIRES,
        "plugins": [
            {"slug": "hello", "owner_id": 7, "name": "Hello", "summary": "Says hello.", "publisher": "Example",
             "authors": [{"name": "Example"}], "license": "0BSD", "homepage": "https://plugins.example.net/hello",
             "categories": ["fun"], "disclosures": [],
             "versions": [
                 version("1.2.0-preview.1", ["settings"],
                         [file("x64", "hello-1.2.0-preview.1-x64", 4096,
                               github_release=release("hello-v1.2.0-preview.1", "hello-1.2.0-preview.1-x64.dll"))],
                         1760220000, "Preview."),
                 version("1.1.0", ["settings", "commands"],
                         [file("x64", "hello-1.1.0-x64", 4096, github_release=release("hello-v1.1.0", "hello-1.1.0-x64.dll"))],
                         1760210000, "Adds /hello.", "hello-1.1.0"),
                 version("1.0.0", ["settings"], [file("x64", "hello-1.0.0-x64", 3584, path="dll/hello-1.0.0-x64.dll")],
                         1760000000, "First release."),
             ]},
        ],
        "revoked": [],
    }


BASES = {"official": ("official.index.json", official_spec), "custom": ("custom.index.json", custom_spec)}
KIND_OF = {"official": "official", "custom": "custom_signed"}


def manifest_targets(index: dict):
    for plugin in index["plugins"]:
        for listed in plugin["versions"]:
            yield plugin["slug"], listed["version"], FIXTURES / "manifests" / f"{plugin['slug']}-{listed['version']}.maicord-plugin.json"


def regenerate() -> None:
    (FIXTURES / "manifests").mkdir(parents=True, exist_ok=True)
    key = scalar_one_key()
    for base, (name, spec) in BASES.items():
        document = plugin_index.render_index(spec())
        (FIXTURES / name).write_bytes(document)
        signature_path = FIXTURES / (name + ".sig")
        current = signature_path.read_bytes() if signature_path.exists() else b""
        # ECDSA signatures are randomized: keep a still-valid one so the file only changes with the document.
        if len(current) != 64 or not cng_p256.verify_digest(SCALAR_ONE_POINT, hashlib.sha256(plugin_index.DOMAIN + document).digest(), current):
            signature_path.write_bytes(plugin_index.sign_index(document, key))
        if base == "official":
            index = plugin_index.parse_index(document, "official", NOW)
            for slug, text, path in manifest_targets(index):
                path.write_bytes(plugin_index.synthesize_manifest(index, slug, text))
        print(f"{name}: {len(document)} bytes")


def vectors():
    """(line, base, expected, find, replace) from vectors.txt; tab-separated, '#' comments."""
    for number, line in enumerate((FIXTURES / "vectors.txt").read_text(encoding="utf-8").split("\n"), 1):
        line = line.rstrip("\r")
        if not line or line.startswith("#"):
            continue
        base, expected, find, replace = line.split("\t")
        yield number, base, expected, find, replace


class PluginIndexToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.documents = {base: (FIXTURES / name).read_bytes() for base, (name, _) in BASES.items()}

    def parse(self, base: str, document: bytes, **options) -> dict:
        pinned = SCALAR_ONE_POINT if base == "custom" else None
        return plugin_index.parse_index(document, KIND_OF[base], options.pop("now", NOW), pinned, **options)

    def test_fixtures_are_the_canonical_rendering(self):
        for base, (name, spec) in BASES.items():
            self.assertEqual(plugin_index.render_index(spec()), self.documents[base], f"{name}: run --regenerate")
            reordered = json.loads(self.documents[base])
            reordered = dict(reversed(list(reordered.items())))
            self.assertEqual(plugin_index.render_index(reordered), self.documents[base], "member order is canonical")

    def test_fixture_signatures_verify_under_the_plugin_domain(self):
        official = plugin_index.verify_index(self.documents["official"], (FIXTURES / "official.index.json.sig").read_bytes(),
                                             SCALAR_ONE_POINT, "official", NOW)
        self.assertEqual((len(official["plugins"]), sum(len(p["versions"]) for p in official["plugins"]),
                          len(official["revoked"]), len(official["blocklist"]), official["ignored_versions"]), (3, 6, 1, 1, 0))
        custom = plugin_index.verify_index(self.documents["custom"], (FIXTURES / "custom.index.json.sig").read_bytes(),
                                           SCALAR_ONE_POINT, "custom_signed", NOW, SCALAR_ONE_POINT)
        self.assertEqual((len(custom["plugins"]), len(custom["plugins"][0]["versions"]), custom["ignored_versions"]), (1, 2, 1))
        self.assertEqual(official["plugins"][1]["name"], "Quiet \"hours\" – Ruhezeit")
        self.assertEqual(official["digest"], hashlib.sha256(plugin_index.DOMAIN + self.documents["official"]).hexdigest())

    def test_synthesized_manifests_match_the_client_fixtures(self):
        index = self.parse("official", self.documents["official"])
        targets = list(manifest_targets(index))
        self.assertEqual(len(targets), 6)
        for slug, text, path in targets:
            self.assertEqual(plugin_index.synthesize_manifest(index, slug, text), path.read_bytes(), path.name)
        self.assertEqual(len(list((FIXTURES / "manifests").iterdir())), 6, "no stale manifest fixtures")
        with self.assertRaises(plugin_index.PluginIndexError) as raised:
            plugin_index.synthesize_manifest(index, "showcase", "1.1.0", "arm64")
        self.assertEqual(raised.exception.code, "file")

    def test_shared_vectors_fail_with_their_error(self):
        count = 0
        for number, base, expected, find, replace in vectors():
            document = self.documents[base].decode("utf-8")
            self.assertEqual(document.count(find), 1, f"vectors.txt:{number}: the search text must occur once")
            self.assertIn(expected, plugin_index.ERRORS, f"vectors.txt:{number}")
            mutated = document.replace(find, replace).encode("utf-8")
            with self.assertRaises(plugin_index.PluginIndexError, msg=f"vectors.txt:{number}") as raised:
                self.parse(base, mutated)
            self.assertEqual(raised.exception.code, expected, f"vectors.txt:{number}: {raised.exception}")
            count += 1
        self.assertGreaterEqual(count, 25)

    def code(self, base: str, document: bytes, **options) -> str:
        try:
            self.parse(base, document, **options)
        except plugin_index.PluginIndexError as error:
            return error.code
        return "none"

    def test_list_limits(self):
        document = self.documents["official"].decode("utf-8")
        start = document.index('{"slug":"showcase"')
        end = document.index(',{"slug":"quiet-hours"')
        showcase = document[start:end]
        copies = [showcase.replace('"slug":"showcase"', f'"slug":"p{i:07d}"').replace(
            '"owner_id":5567385134209385281', f'"owner_id":55673851342093{i:05d}') for i in range(129)]
        prefix, suffix = document[:document.index('"plugins":[') + 11], document[document.index('],"revoked"'):]
        self.assertEqual(self.code("official", (prefix + ",".join(copies[:128]) + suffix).encode()), "none")
        self.assertEqual(self.code("official", (prefix + ",".join(copies) + suffix).encode()), "limit")
        newest = document[document.index('{"version":"2.0.0"'):document.index(',{"version":"1.4.2"')]
        extra = [newest.replace('"2.0.0"', f'"{n}.0.0"').replace("2000000999", f"{n}000000999") for n in (4, 3)]
        self.assertEqual(self.code("official", document.replace(newest, ",".join(extra[1:] + [newest])).encode()), "none")
        self.assertEqual(self.code("official", document.replace(newest, ",".join(extra + [newest])).encode()), "limit")
        revoked = document[document.index('{"owner_id":1234605616436508552,"sha256"'):document.index('],"blocklist"')]
        many = [revoked.replace('"owner_id":1234605616436508552', f'"owner_id":{1000 + i}') for i in range(65)]
        head, tail = document[:document.index(revoked)], document[document.index(revoked) + len(revoked):]
        self.assertEqual(self.code("official", (head + ",".join(many[:64]) + tail).encode()), "none")
        self.assertEqual(self.code("official", (head + ",".join(many) + tail).encode()), "limit")
        self.assertEqual(self.code("official", b" " * (plugin_index.MAX_BYTES + 1)), "size")
        self.assertEqual(self.code("official", b""), "size")

    def test_time_and_kind_rules(self):
        official, custom = self.documents["official"], self.documents["custom"]
        self.assertEqual(self.code("official", official, now=EXPIRES), "expired")
        self.assertTrue(self.parse("official", official, now=EXPIRES, accept_expired=True)["expired"])
        self.assertEqual(self.code("official", official, now=ISSUED - plugin_index.CLOCK_SKEW_SECONDS - 1), "time")
        self.assertEqual(self.code("official", official, now=ISSUED - plugin_index.CLOCK_SKEW_SECONDS), "none")
        with self.assertRaises(plugin_index.PluginIndexError) as raised:
            plugin_index.parse_index(official, "custom_signed", NOW, SCALAR_ONE_POINT)
        self.assertEqual(raised.exception.code, "provider")
        with self.assertRaises(plugin_index.PluginIndexError) as raised:
            plugin_index.parse_index(custom, "custom_signed", NOW, b"\x04" + GY + GX)
        self.assertEqual(raised.exception.code, "key_changed")
        with self.assertRaises(plugin_index.PluginIndexError) as raised:
            plugin_index.parse_index(custom, "custom_unsigned", NOW)
        self.assertEqual(raised.exception.code, "provider")
        unsigned = custom.replace(b',"key":"' + SCALAR_ONE_POINT.hex().encode() + b'"', b"")
        self.assertEqual(len(plugin_index.parse_index(unsigned, "custom_unsigned", NOW)["plugins"]), 1)

    def test_signatures(self):
        key, other = cng_p256.PrivateKey.generate(), cng_p256.PrivateKey.generate()
        document = self.documents["official"]
        signature = plugin_index.sign_index(document, key)
        self.assertEqual(len(plugin_index.verify_index(document, signature, key.public_point, "official", NOW)["plugins"]), 3)
        update_domain = key.sign_digest(hashlib.sha256(b"MaiCord update manifest v1\n" + document).digest())
        tampered = bytearray(document)
        tampered[document.index(b"Showcase")] ^= 0x20
        for bad_document, bad_signature, point in ((document, update_domain, key.public_point),
                                                    (document, signature, other.public_point),
                                                    (bytes(tampered), signature, key.public_point),
                                                    (document, signature[:63], key.public_point)):
            with self.assertRaises(plugin_index.PluginIndexError) as raised:
                plugin_index.verify_index(bad_document, bad_signature, point, "official", NOW)
            self.assertEqual(raised.exception.code, "signature")
        with self.assertRaises(plugin_index.PluginIndexError) as raised:
            plugin_index.verify_index(b" " * (plugin_index.MAX_BYTES + 1), signature, key.public_point, "official", NOW)
        self.assertEqual(raised.exception.code, "size", "size is refused before the signature")

    def test_versions(self):
        for text, code in (("0.0.1", 1999), ("1.2.3", 1002003999), ("999.999.999", 999999999999),
                           ("1.2.3-preview.4", 1002003004), ("999.999.999-preview.998", 999999999998)):
            self.assertEqual(plugin_index.version_code(text), code)
            self.assertEqual(plugin_index.version_text(code), text)
        for bad in ("1.2", "01.2.3", "1.2.3-preview.0", "1.2.3-preview.999", "1000.0.0", "1.2.3-rc.1"):
            with self.assertRaises(plugin_index.PluginIndexError, msg=bad):
                plugin_index.version_code(bad)

    def test_command_line(self):
        with tempfile.TemporaryDirectory(prefix="maicord-plugin-index-test-") as directory:
            root = Path(directory)
            key = cng_p256.PrivateKey.generate()
            (root / "key.jwk").write_text(key.to_jwk(), encoding="utf-8")
            (root / "source.json").write_text(json.dumps(official_spec(), indent=2), encoding="utf-8")
            output = io.StringIO()
            with redirect_stdout(output), redirect_stderr(io.StringIO()):
                self.assertEqual(plugin_index.main(["render", str(root / "source.json"), str(root / "index.json")]), 0)
                self.assertEqual(plugin_index.main(["sign", "--key", str(root / "key.jwk"), str(root / "index.json")]), 0)
                self.assertEqual(plugin_index.main(["verify", "--kind", "official", "--public-key", key.public_point.hex(),
                                                    "--now", str(NOW), str(root / "index.json")]), 0)
                self.assertEqual(plugin_index.main(["verify", "--kind", "official", "--public-key", SCALAR_ONE_POINT.hex(),
                                                    "--now", str(NOW), str(root / "index.json")]), 1)
                self.assertEqual(plugin_index.main(["manifest", str(root / "index.json"), "--slug", "showcase",
                                                    "--version", "1.0.0"]), 0)
            self.assertEqual((root / "index.json").read_bytes(), self.documents["official"])
            self.assertIn("valid: 3 plugins, 6 versions, 1 revoked", output.getvalue())
            self.assertIn((FIXTURES / "manifests" / "showcase-1.0.0.maicord-plugin.json").read_text(encoding="utf-8"),
                          output.getvalue())


if __name__ == "__main__":
    if "--regenerate" in sys.argv[1:]:
        regenerate()
    else:
        unittest.main(verbosity=1)
