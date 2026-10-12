#!/usr/bin/env python3
"""Self-contained checks for the release tooling with a throwaway key.

    python -B tools/release/test_release_tools.py

Everything is written to a temporary directory that is removed afterwards.
"""

from __future__ import annotations

import hashlib
import io
import struct
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cng_p256  # noqa: E402
import maicord_release as release  # noqa: E402

# Byte-for-byte copy of the accepted document in tests/update_manifest.cpp.
CLIENT_FIXTURE_DOCUMENT = (b'{"format":1,"product":"MaiCord","arch":"x64","channel":"stable","version":6,'
                           b'"issued":90,"expires":200,"bytes":4096,"sha256":'
                           b'"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",'
                           b'"artifact":"MaiCord-x64-6.zip"}')


def fake_pe(dll: bool, payload: bytes) -> bytes:
    header = bytearray(64 + 24 + 240)
    header[0:2] = b"MZ"
    struct.pack_into("<I", header, 60, 64)
    header[64:68] = b"PE\0\0"
    struct.pack_into("<HH", header, 68, 0x8664, 1)
    struct.pack_into("<HH", header, 84, 240, 0x0022 | (0x2000 if dll else 0))
    struct.pack_into("<H", header, 88, 0x20B)
    return bytes(header) + payload


class ReleaseToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.key = cng_p256.PrivateKey.generate()
        cls.other = cng_p256.PrivateKey.generate()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="maicord-release-test-")
        self.root = Path(self.directory.name)
        self.binaries = self.root / "bin"
        self.binaries.mkdir()
        (self.binaries / "MaiCord.exe").write_bytes(fake_pe(False, b"executable" * 100))
        (self.binaries / "MaiCordAccount.dll").write_bytes(fake_pe(True, b"account" * 77))
        self.key_file = self.root / "key.jwk"
        self.key_file.write_text(self.key.to_jwk(), encoding="utf-8")

    def tearDown(self):
        self.directory.cleanup()

    def run_cli(self, *arguments: str) -> int:
        with redirect_stdout(io.StringIO()):
            return release.main(list(arguments))

    def test_version_encoding_is_monotonic(self):
        tags = ["v0.0.1-preview.1", "v0.0.1", "v0.1.0-preview.2", "v0.1.0", "v1.0.0-preview.998", "v1.0.0",
                "v1.0.1", "v999.999.999"]
        numbers = [release.parse_tag(tag).number for tag in tags]
        self.assertEqual(numbers, sorted(numbers))
        self.assertEqual(len(set(numbers)), len(numbers))
        self.assertEqual(release.parse_tag("v1.2.3").number, 1002003999)
        self.assertEqual(release.parse_tag("v1.2.3-preview.4").channel, "preview")
        self.assertLess(max(numbers), 2 ** 64)
        for bad in ("1.2.3", "v1.2", "v01.2.3", "v1.2.3-rc.1", "v1.2.3-preview.0", "v1000.0.0", "v1.2.3-preview.999"):
            with self.assertRaises(release.ReleaseError, msg=bad):
                release.parse_tag(bad)

    def test_manifest_layout_matches_client_fixture(self):
        document = release.render_manifest(6, "stable", 90, 200, 4096,
                                           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                           "MaiCord-x64-6.zip")
        self.assertEqual(document, CLIENT_FIXTURE_DOCUMENT)
        self.assertEqual(release.parse_manifest(document)["version"], 6)
        for bad in (document.replace(b'"format":1', b'"format":1,"format":1'),
                    document.replace(b'"arch":"x64"', b'"arch":"arm64"'),
                    document.replace(b'MaiCord-x64-6.zip', b'MaiCord-x64-7.zip'),
                    document.replace(b'"product":"MaiCord"', b'"product":"M\\u0061iCord"'),
                    document[:-1] + b',"extra":0}'):
            with self.assertRaises(release.ReleaseError):
                release.parse_manifest(bad)

    def test_update_zip_matches_client_subset(self):
        output = self.root / "MaiCord-x64-1.zip"
        release.build_update_zip(self.binaries / "MaiCord.exe", self.binaries / "MaiCordAccount.dll", output)
        package = output.read_bytes()
        release.validate_update_zip(package)
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.namelist(), list(release.MEMBER_NAMES))
            self.assertEqual(archive.read("MaiCordAccount.dll"), (self.binaries / "MaiCordAccount.dll").read_bytes())
            self.assertTrue(all(info.compress_type == zipfile.ZIP_STORED for info in archive.infolist()))
        release.build_update_zip(self.binaries / "MaiCord.exe", self.binaries / "MaiCordAccount.dll",
                                 self.root / "again.zip")
        self.assertEqual((self.root / "again.zip").read_bytes(), package, "packages must be reproducible")
        corrupted = bytearray(package)
        corrupted[-2] = 1  # ZIP comment length
        with self.assertRaises(release.ReleaseError):
            release.validate_update_zip(bytes(corrupted))
        swapped = self.root / "swapped"
        swapped.mkdir()
        (swapped / "MaiCord.exe").write_bytes((self.binaries / "MaiCordAccount.dll").read_bytes())
        (swapped / "MaiCordAccount.dll").write_bytes((self.binaries / "MaiCord.exe").read_bytes())
        with self.assertRaises(release.ReleaseError):
            release.build_update_zip(swapped / "MaiCord.exe", swapped / "MaiCordAccount.dll", self.root / "bad.zip")

    def test_release_signs_and_verifies_each_channel(self):
        out = self.root / "dist"
        public = self.key.public_point.hex()
        self.assertEqual(self.run_cli("release", "--tag", "v1.2.3", "--binaries", str(self.binaries),
                                      "--output-dir", str(out), "--key-file", str(self.key_file),
                                      "--public-key", public), 0)
        artifact = (out / "MaiCord-x64-1002003999.zip").read_bytes()
        for channel in ("stable", "preview"):
            document = (out / f"maicord-{channel}.manifest.json").read_bytes()
            signature = (out / f"maicord-{channel}.manifest.json.sig").read_bytes()
            self.assertEqual(len(signature), 64)
            fields = release.verify_offer(document, signature, self.key.public_point, artifact)
            self.assertEqual((fields["channel"], fields["version"], fields["bytes"]),
                             (channel, 1002003999, len(artifact)))
            self.assertEqual(fields["sha256"], hashlib.sha256(artifact).hexdigest())
            self.assertLessEqual(fields["expires"] - fields["issued"], release.DEFAULT_VALIDITY_DAYS * 86400)
            with self.assertRaises(release.ReleaseError):
                release.verify_offer(document, signature, self.other.public_point)
            tampered = document.replace(b'"bytes":', b'"bytes":1')
            with self.assertRaises(release.ReleaseError):
                release.verify_offer(tampered, signature, self.key.public_point)
            # Signature covers the domain prefix: a bare-document signature must fail.
            bare = self.key.sign_digest(hashlib.sha256(document).digest())
            with self.assertRaises(release.ReleaseError):
                release.verify_offer(document, bare, self.key.public_point)

    def test_preview_tag_only_publishes_preview(self):
        out = self.root / "dist"
        self.assertEqual(self.run_cli("release", "--tag", "v1.3.0-preview.2", "--binaries", str(self.binaries),
                                      "--output-dir", str(out), "--key-file", str(self.key_file)), 0)
        self.assertTrue((out / "maicord-preview.manifest.json").is_file())
        self.assertFalse((out / "maicord-stable.manifest.json").exists())

    def test_wrong_key_is_refused(self):
        self.assertEqual(self.run_cli("release", "--tag", "v1.2.3", "--binaries", str(self.binaries),
                                      "--output-dir", str(self.root / "dist"), "--key-file", str(self.key_file),
                                      "--public-key", self.other.public_point.hex()), 1)

    def release_to(self, tag: str, out: Path, *extra: str) -> None:
        self.assertEqual(self.run_cli("release", "--tag", tag, "--binaries", str(self.binaries), "--output-dir", str(out),
                                      "--key-file", str(self.key_file), *extra), 0)

    def channels(self, site: Path, *extra: str) -> int:
        return self.run_cli("channels", "--site", str(site), "--public-key", self.key.public_point.hex(),
                            "--key-file", str(self.key_file), *extra)

    def site_fields(self, site: Path, channel: str) -> dict:
        document = site / "updates" / f"maicord-{channel}.manifest.json"
        return release.verify_offer(document.read_bytes(), Path(f"{document}.sig").read_bytes(), self.key.public_point)

    def test_channels_publish_and_never_move_back(self):
        stable, preview, older = self.root / "stable", self.root / "preview", self.root / "older"
        self.release_to("v1.2.0", stable)
        self.release_to("v1.3.0-preview.1", preview)
        self.release_to("v1.1.0", older)
        site = self.root / "site"
        self.assertEqual(self.channels(site, "--publish", str(stable)), 0)
        self.assertEqual(self.site_fields(site, "stable")["version"], 1002000999)
        self.assertEqual(self.site_fields(site, "preview")["version"], 1002000999)
        self.assertEqual(self.channels(site, "--publish", str(preview)), 0)
        self.assertEqual(self.site_fields(site, "preview")["version"], 1003000001)
        self.assertEqual(self.site_fields(site, "stable")["version"], 1002000999)
        # An older stable release (a hotfix branch) never lowers either channel.
        self.assertEqual(self.channels(site, "--publish", str(older)), 0)
        self.assertEqual(self.site_fields(site, "stable")["version"], 1002000999)
        self.assertEqual(self.site_fields(site, "preview")["version"], 1003000001)
        # A manifest signed by another key is refused outright.
        forged = self.root / "forged"
        forged.mkdir()
        other_key = self.root / "other.jwk"
        other_key.write_text(self.other.to_jwk(), encoding="utf-8")
        self.assertEqual(self.run_cli("release", "--tag", "v9.0.0", "--binaries", str(self.binaries), "--output-dir",
                                      str(forged), "--key-file", str(other_key)), 0)
        self.assertEqual(self.channels(site, "--publish", str(forged)), 1)
        self.assertEqual(self.site_fields(site, "stable")["version"], 1002000999)

    def test_channels_refresh_only_when_expiring(self):
        out = self.root / "dist"
        self.release_to("v2.0.0", out, "--validity-days", "10")
        site = self.root / "site"
        self.assertEqual(self.channels(site, "--publish", str(out)), 0)
        old = self.site_fields(site, "stable")
        self.assertEqual(self.channels(site, "--refresh-within-days", "5"), 0)
        self.assertEqual(self.site_fields(site, "stable"), old)
        self.assertEqual(self.channels(site, "--refresh-within-days", "30"), 0)
        for channel in ("stable", "preview"):
            new = self.site_fields(site, channel)
            self.assertEqual({k: new[k] for k in ("version", "bytes", "sha256", "channel")},
                             {k: old[k] for k in ("version", "bytes", "sha256")} | {"channel": channel})
            self.assertGreater(new["expires"], old["expires"])
        # Damaged site state is refused instead of being deployed.
        (site / "updates" / "maicord-preview.manifest.json.sig").write_bytes(b"\0" * 64)
        self.assertEqual(self.channels(site, "--refresh-within-days", "30"), 1)

    def test_release_notes_group_commits(self):
        commits = [("a" * 40, "Release 1.3.0"), ("b" * 40, "feat(ui): add a theme picker"),
                   ("c" * 40, "Fix a crash when the gateway drops"), ("d" * 40, "Halve startup latency"),
                   ("e" * 40, "Reorganize tests"), ("f" * 40, "perf: cache glyph runs")]
        notes = release.render_notes(release.parse_tag("v1.3.0"), "v1.2.0", commits, "owner/repo")
        self.assertNotIn("Release 1.3.0", notes)
        features, fixes, performance, other = (notes.index(f"### {name}") for name in release.NOTE_GROUPS)
        self.assertLess(features, fixes)
        self.assertLess(fixes, performance)
        self.assertLess(performance, other)
        self.assertIn("- Add a theme picker ([`bbbbbbb`](https://github.com/owner/repo/commit/" + "b" * 40 + "))", notes)
        self.assertIn("- Cache glyph runs", notes[performance:other])
        self.assertIn("Halve startup latency", notes[performance:other])
        self.assertIn("Fix a crash", notes[fixes:performance])
        self.assertIn("https://github.com/owner/repo/compare/v1.2.0...v1.3.0", notes)
        self.assertIn("gh attestation verify <file> -R owner/repo", notes)
        empty = release.render_notes(release.parse_tag("v1.3.1-preview.1"), None, [("a" * 40, "Bump version to 1.3.1")], "")
        self.assertIn("No user-visible changes.", empty)
        self.assertIn("Preview build", empty)
        self.assertNotIn("compare", empty)

    def test_checksums_cover_every_asset(self):
        out = self.root / "dist"
        self.release_to("v1.0.0", out)
        self.assertEqual(self.run_cli("checksums", str(out)), 0)
        listed = {line.split("  ", 1)[1] for line in (out / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines()}
        self.assertEqual(listed, {path.name for path in out.iterdir() if path.name != "SHA256SUMS.txt"})
        self.assertIn("maicord-stable.manifest.json.sig", listed)

    def test_jwk_round_trip_and_public_key_parsing(self):
        restored = cng_p256.PrivateKey.from_jwk(self.key.to_jwk())
        self.assertEqual(restored.public_point, self.key.public_point)
        spaced = " ".join(f"0x{b:02x}," for b in self.key.public_point)
        self.assertEqual(cng_p256.parse_public_point(spaced), self.key.public_point)
        with self.assertRaises(cng_p256.CngError):
            cng_p256.parse_public_point("04" + "00" * 10)


if __name__ == "__main__":
    unittest.main(verbosity=2)
