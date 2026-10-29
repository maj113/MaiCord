#!/usr/bin/env python3
"""Self-contained checks for the release tooling with a throwaway key.

    python -B tools/release/test_release_tools.py

Everything is written to a temporary directory that is removed afterwards.
"""

from __future__ import annotations

import hashlib
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile
from contextlib import redirect_stdout
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cng_p256  # noqa: E402
import maicord_release as release  # noqa: E402

# Byte-for-byte copy of the accepted document in tests/update_manifest.cpp.
CLIENT_FIXTURE_DOCUMENT = (b'{"format":2,"product":"MaiCord","arch":"x64","channel":"stable","version":6,'
                           b'"issued":90,"expires":200,"bytes":4096,"sha256":'
                           b'"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",'
                           b'"artifact":"MaiCord-x64-p2-6.zip"}')
# Byte-for-byte copy of the accepted document in tests/update_changelog.cpp.
CLIENT_CHANGELOG_DOCUMENT = ('{"format":1,"product":"MaiCord","channel":"preview","version":3004002,"base":3003999,'
                             '"entries":[{"version":3004002,"date":1760000100,"commit":"' + "a" * 40 + '",'
                             '"text":"Fix \\"quoted\\" names\\nin the caf\u00e9 list"},'
                             '{"version":3004001,"date":1760000000,"commit":"","text":"Add a theme picker"}]}'
                             ).encode("utf-8")


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
        self.key_file = self.root / "key.jwk"
        self.key_file.write_text(self.key.to_jwk(), encoding="utf-8")

    def tearDown(self):
        self.directory.cleanup()

    def run_cli(self, *arguments: str) -> int:
        with redirect_stdout(io.StringIO()):
            return release.main(list(arguments))

    def test_version_encoding_is_monotonic(self):
        stable = [release.parse_tag(tag).number for tag in ("v0.0.1", "v0.1.0", "v1.0.0", "v1.0.1", "v999.999.999")]
        dev = [release.version_number(0, 0, 2, 1), release.version_number(0, 1, 1, 998),
               release.version_number(1, 0, 2, 1)]
        ordered = [stable[0], dev[0], stable[1], dev[1], stable[2], stable[3], dev[2], stable[4]]
        self.assertEqual(ordered, sorted(ordered))
        self.assertEqual(release.parse_tag("v1.2.3").number, 1002003999)
        self.assertEqual(release.version_text(1002003999), "1.2.3")
        self.assertEqual(release.version_text(1002004005), "1.2.4-dev.5")
        self.assertTrue(release.is_stable(1002003999))
        self.assertFalse(release.is_stable(1002004005))
        self.assertLess(max(stable), 2 ** 64)
        for bad in ("1.2.3", "v1.2", "v01.2.3", "v1.2.3-rc.1", "v1.2.3-preview.1", "v1000.0.0"):
            with self.assertRaises(release.ReleaseError, msg=bad):
                release.parse_tag(bad)
        self.assertEqual(release.next_base(release.parse_tag("v1.2.3"), (1, 2, 3)), (1, 2, 4))
        self.assertEqual(release.next_base(release.parse_tag("v1.2.3"), (1, 3, 0)), (1, 3, 0))
        self.assertEqual(release.next_base(release.parse_tag("v1.2.999"), (1, 2, 999)), (1, 3, 0))

    def git_repository(self) -> Path:
        """A throwaway history: v1.2.0 and three commits after it."""
        repository = self.root / "repo"
        repository.mkdir()
        self.commit_time = 1760000000
        self.git(repository, "init", "-q", "-b", "master")
        for name, value in (("user.name", "Test"), ("user.email", "test@example.invalid"),
                            ("commit.gpgsign", "false"), ("core.autocrlf", "false")):
            self.git(repository, "config", name, value)
        self.commit(repository, "MaiCord 1.2.0", project="1.2.0")
        self.git(repository, "tag", "v1.2.0")
        for subject in ("Add a theme picker", "Fix a crash when the gateway drops", "Halve startup latency"):
            self.commit(repository, subject)
        return repository

    def git(self, repository: Path, *arguments: str) -> str:
        environment = dict(os.environ, GIT_COMMITTER_DATE=f"{self.commit_time} +0000",
                           GIT_AUTHOR_DATE=f"{self.commit_time} +0000")
        return subprocess.run(["git", "-C", str(repository), *arguments], check=True, capture_output=True,
                              text=True, env=environment).stdout.strip()

    def commit(self, repository: Path, subject: str, project: str | None = None) -> str:
        cmake = repository / "CMakeLists.txt"
        if project or not cmake.exists():
            cmake.write_text(f"project(MaiCord VERSION {project or '1.2.0'} LANGUAGES C)\n", encoding="utf-8")
        (repository / "log.txt").write_text(f"{subject}\n{self.commit_time}\n", encoding="utf-8")
        self.git(repository, "add", "-A")
        self.git(repository, "commit", "-q", "-m", subject)
        self.commit_time += 60
        return self.git(repository, "rev-parse", "HEAD")

    def test_build_identity_numbers_master_builds_after_the_last_release(self):
        repository = self.git_repository()
        identity = release.build_identity(repository)
        head = self.git(repository, "rev-parse", "HEAD")
        self.assertEqual((identity.semver, identity.number, identity.commit), ("1.2.1-dev.3", 1002001003, head))
        self.assertEqual(identity.label, f"1.2.1-dev.3+{head[:7]}")
        # A bumped project version names the dev builds; ordering still holds.
        self.commit(repository, "Prepare 1.3.0", project="1.3.0")
        bumped = release.build_identity(repository)
        self.assertEqual(bumped.semver, "1.3.0-dev.4")
        self.assertGreater(bumped.number, identity.number)
        self.git(repository, "tag", "v1.3.0")
        tagged = release.build_identity(repository)
        self.assertEqual((tagged.semver, tagged.number), ("1.3.0", 1003000999))
        self.assertGreater(tagged.number, bumped.number)
        self.commit(repository, "Polish the updater page")
        after = release.build_identity(repository)
        self.assertEqual(after.semver, "1.3.1-dev.1")
        self.assertGreater(after.number, tagged.number)
        self.assertLess(after.number, release.parse_tag("v1.3.1").number)

    def test_local_builds_number_at_or_after_the_published_master_build(self):
        """cmake/client.cmake numbers a local build with `identity`, as CI numbers the published
        master build, so the updater never offers a local build an older one."""
        repository = self.git_repository()
        published = release.build_identity(repository)
        # The same commit, built locally (edits or not), is the same version: up to date.
        self.assertEqual(release.build_identity(repository).number, published.number)
        self.commit(repository, "Try a change before pushing it")
        local = release.build_identity(repository)
        self.assertGreater(local.number, published.number)
        output = io.StringIO()
        environment = {name: value for name, value in os.environ.items() if name != "GITHUB_OUTPUT"}
        with mock.patch.dict(os.environ, environment, clear=True), redirect_stdout(output):
            self.assertEqual(release.main(["identity", "--repository", str(repository)]), 0)
        self.assertIn(f"version={local.semver}\n", output.getvalue())
        self.assertIn(f"commit={local.commit}\n", output.getvalue())

    def test_changelog_layout_matches_client_fixture(self):
        entries = [{"version": 3004002, "date": 1760000100, "commit": "a" * 40,
                    "text": 'Fix "quoted" names\nin the café list'},
                   {"version": 3004001, "date": 1760000000, "commit": "", "text": "Add a theme picker"}]
        document = release.render_changelog("preview", 3004002, 3003999, entries)
        self.assertEqual(document, CLIENT_CHANGELOG_DOCUMENT)
        self.assertEqual(len(release.parse_changelog(document)["entries"]), 2)
        oversized = json.dumps({"format": 1, "product": "MaiCord", "channel": "stable", "version": 9, "base": 0,
                                "entries": [{"version": 9, "date": 0, "commit": "", "text": "x" * 8193}]})
        crowded = json.dumps({"format": 1, "product": "MaiCord", "channel": "stable", "version": 9, "base": 0,
                              "entries": [{"version": 9, "date": 0, "commit": "", "text": "x"}] * 101})
        for bad in (document.replace(b'"format":1', b'"format":1,"format":1'),
                    document.replace(b'"base":3003999', b'"base":3004002'),
                    document.replace(b'"version":3004001', b'"version":3004003'),
                    document.replace(b'"commit":""', b'"commit":"abc"'),
                    document.replace(b'"text":"Add a theme picker"', b'"text":""'),
                    document.replace(b'"channel":"preview"', b'"channel":"beta"'),
                    document[:-1] + b',"extra":0}', oversized.encode(), crowded.encode(),
                    b"[" * 10 + b"]" * 10):
            with self.assertRaises(release.ReleaseError, msg=bad[:60]):
                release.parse_changelog(bad)
        signature = release.sign_changelog(self.key, document, self.key.public_point)
        release.verify_changelog(document, signature, self.key.public_point)
        # The changelog domain keeps manifest signatures from verifying as changelogs.
        with self.assertRaises(release.ReleaseError):
            release.verify_changelog(document, release.sign_document(self.key, document, None), self.key.public_point)

    def test_release_notes_text_keeps_only_the_changes(self):
        notes = release.render_notes(release.parse_tag("v1.3.0"), "v1.2.0",
                                     [("b" * 40, "feat(ui): add a [theme](https://example.invalid) picker"),
                                      ("c" * 40, "Fix a crash")], "owner/repo")
        text = release.release_notes_text(notes)
        self.assertTrue(text.startswith("### Features"))
        self.assertIn("- Add a theme picker", text)
        self.assertIn("- Fix a crash", text)
        for gone in ("http", "## MaiCord", "Download", "Full changelog", "`bbbbbbb`", "attestation"):
            self.assertNotIn(gone, text)
        long = release.release_notes_text("\n".join(f"- change {i:04}" + "x" * 40 for i in range(400)))
        self.assertLessEqual(len(long.encode("utf-8")), release.CHANGELOG_NOTES_BYTES)
        self.assertTrue(long.endswith("…"))

    def test_stable_changelog_lists_recent_releases(self):
        releases = [{"tag_name": f"v1.{minor}.0", "published_at": f"2026-01-{minor + 1:02}T12:00:00Z",
                     "body": f"## MaiCord 1.{minor}.0\n\n### Fixes\n\n- Fix {minor}\n", "draft": False,
                     "prerelease": False} for minor in range(12)]
        releases += [{"tag_name": "v1.12.0", "body": "draft", "draft": True, "prerelease": False},
                     {"tag_name": "v1.3.0-preview.1", "body": "old preview", "draft": False, "prerelease": True}]
        fields = release.parse_changelog(release.stable_changelog(releases, release.parse_tag("v1.11.0").number, None))
        versions = [entry["version"] for entry in fields["entries"]]
        self.assertEqual(versions, [release.parse_tag(f"v1.{minor}.0").number for minor in range(11, 1, -1)])
        self.assertEqual(fields["base"], release.parse_tag("v1.1.0").number)
        self.assertEqual(fields["entries"][0]["text"], "### Fixes\n\n- Fix 11")
        self.assertEqual(fields["entries"][0]["date"], 1767268800 + 11 * 86400)
        with self.assertRaises(release.ReleaseError):
            release.stable_changelog(releases, release.parse_tag("v1.12.0").number, None)

    def test_manifest_layout_matches_client_fixture(self):
        document = release.render_manifest(6, "stable", 90, 200, 4096,
                                           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                           "MaiCord-x64-p2-6.zip")
        self.assertEqual(document, CLIENT_FIXTURE_DOCUMENT)
        self.assertEqual(release.parse_manifest(document)["version"], 6)
        for bad in (document.replace(b'"format":2', b'"format":2,"format":2'),
                    document.replace(b'"format":2', b'"format":1'),
                    document.replace(b'"arch":"x64"', b'"arch":"arm64"'),
                    document.replace(b'MaiCord-x64-p2-6.zip', b'MaiCord-x64-p2-7.zip'),
                    document.replace(b'MaiCord-x64-p2-6.zip', b'MaiCord-x64-6.zip'),
                    document.replace(b'"product":"MaiCord"', b'"product":"M\\u0061iCord"'),
                    document[:-1] + b',"extra":0}'):
            with self.assertRaises(release.ReleaseError):
                release.parse_manifest(bad)

    def test_update_zip_matches_client_subset(self):
        output = self.root / "MaiCord-x64-p2-1.zip"
        release.build_update_zip(self.binaries, output)
        package = output.read_bytes()
        self.assertEqual(release.validate_update_zip(package), [release.PACKAGE_EXECUTABLE])
        release.build_update_zip(self.binaries, self.root / "again.zip")
        self.assertEqual((self.root / "again.zip").read_bytes(), package, "packages must be reproducible")
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.namelist(), ["MaiCord.exe"])
            self.assertEqual(archive.read("MaiCord.exe"), (self.binaries / "MaiCord.exe").read_bytes())
            self.assertTrue(all(info.compress_type == zipfile.ZIP_STORED for info in archive.infolist()))
        corrupted = bytearray(package)
        corrupted[-2] = 1  # ZIP comment length
        renamed = package.replace(b"MaiCord.exe", b"maicord.exe")
        two = self.root / "two.zip"
        with zipfile.ZipFile(two, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr("MaiCord.exe", (self.binaries / "MaiCord.exe").read_bytes())
            archive.writestr("notes.txt", b"release notes")
        for bad in (bytes(corrupted), renamed, two.read_bytes()):
            with self.assertRaises(release.ReleaseError):
                release.validate_update_zip(bad)
        # The one member must be an AMD64 executable, not a DLL.
        library = self.root / "library"
        library.mkdir()
        (library / "MaiCord.exe").write_bytes(fake_pe(True, b"x"))
        with self.assertRaises(release.ReleaseError):
            release.build_update_zip(library, self.root / "bad.zip")

    def test_release_signs_and_verifies_each_channel(self):
        out = self.root / "dist"
        public = self.key.public_point.hex()
        self.assertEqual(self.run_cli("release", "--tag", "v1.2.3", "--binaries", str(self.binaries),
                                      "--output-dir", str(out), "--key-file", str(self.key_file),
                                      "--public-key", public), 0)
        artifact = (out / "MaiCord-x64-p2-1002003999.zip").read_bytes()
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

    def test_preview_signs_head_with_its_changelog(self):
        repository = self.git_repository()
        out = self.root / "dist"
        public = self.key.public_point.hex()
        arguments = ("preview", "--binaries", str(self.binaries), "--output-dir", str(out), "--repository",
                     str(repository), "--key-file", str(self.key_file), "--public-key", public)
        self.assertEqual(self.run_cli(*arguments, "--expect-version", "1.2.1-dev.2"), 1)
        self.assertEqual(self.run_cli(*arguments, "--expect-version", "1.2.1-dev.3"), 0)
        artifact = (out / "MaiCord-x64-p2-1002001003.zip").read_bytes()
        document = (out / "maicord-preview.manifest.json").read_bytes()
        fields = release.verify_offer(document, (out / "maicord-preview.manifest.json.sig").read_bytes(),
                                      self.key.public_point, artifact)
        self.assertEqual((fields["channel"], fields["version"]), ("preview", 1002001003))
        self.assertFalse((out / "maicord-stable.manifest.json").exists())
        log = release.verify_changelog((out / "maicord-preview.changelog.json").read_bytes(),
                                       (out / "maicord-preview.changelog.json.sig").read_bytes(), self.key.public_point)
        self.assertEqual((log["channel"], log["version"], log["base"]), ("preview", 1002001003, 1002000999))
        self.assertEqual([(entry["version"], entry["text"]) for entry in log["entries"]],
                         [(1002001003, "Halve startup latency"), (1002001002, "Fix a crash when the gateway drops"),
                          (1002001001, "Add a theme picker")])
        self.assertEqual(log["entries"][0]["commit"], self.git(repository, "rev-parse", "HEAD"))
        # A tagged HEAD is a release, never a preview build.
        self.git(repository, "tag", "v1.2.1")
        self.assertEqual(self.run_cli(*arguments), 1)

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

    def site_fields(self, site: Path, slot: str) -> dict:
        document = site / next(entry.manifest for entry in release.SLOTS if entry.name == slot)
        return release.verify_offer(document.read_bytes(), Path(f"{document}.sig").read_bytes(), self.key.public_point)

    def site_versions(self, site: Path) -> dict:
        return {slot.name: self.site_fields(site, slot.name)["version"] for slot in release.SLOTS
                if (site / slot.manifest).exists()}

    def preview_build(self, repository: Path, out: Path) -> None:
        self.assertEqual(self.run_cli("preview", "--binaries", str(self.binaries), "--output-dir", str(out),
                                      "--repository", str(repository), "--key-file", str(self.key_file)), 0)

    def release_changelogs(self, repository: Path, tag: str, out: Path) -> None:
        listing = self.root / f"releases-{tag}.json"
        listing.write_text(json.dumps([{"tag_name": tag, "published_at": "2026-02-01T00:00:00Z",
                                        "body": "### Fixes\n\n- Fix it\n", "draft": False, "prerelease": False}]),
                           encoding="utf-8")
        self.assertEqual(self.run_cli("changelogs", "--tag", tag, "--releases", str(listing), "--output-dir", str(out),
                                      "--repository", str(repository), "--key-file", str(self.key_file)), 0)

    def test_channels_publish_and_never_move_back(self):
        stable, older = self.root / "stable", self.root / "older"
        self.release_to("v1.2.0", stable)
        self.release_to("v1.1.0", older)
        site = self.root / "site"
        self.assertEqual(self.channels(site, "--publish", str(stable)), 0)
        self.assertEqual(self.site_versions(site), {"stable": 1002000999, "preview": 1002000999})
        self.assertTrue((site / "updates" / "p2" / "maicord-stable.manifest.json").exists())
        # An older stable release (a hotfix branch) never lowers a channel.
        self.assertEqual(self.channels(site, "--publish", str(older)), 0)
        self.assertEqual(set(self.site_versions(site).values()), {1002000999})
        # A manifest signed by another key is refused outright.
        forged = self.root / "forged"
        other_key = self.root / "other.jwk"
        other_key.write_text(self.other.to_jwk(), encoding="utf-8")
        self.assertEqual(self.run_cli("release", "--tag", "v9.0.0", "--binaries", str(self.binaries), "--output-dir",
                                      str(forged), "--key-file", str(other_key)), 0)
        self.assertEqual(self.channels(site, "--publish", str(forged)), 1)
        self.assertEqual(set(self.site_versions(site).values()), {1002000999})

    def test_channels_serve_master_builds_on_the_preview_feed_only(self):
        repository = self.git_repository()
        site = self.root / "site"
        feed = site / "updates" / "p2" / "preview"
        # Files another protocol's clients read are left as published.
        other = site / "updates" / "maicord-stable.manifest.json"
        other.parent.mkdir(parents=True)
        other.write_bytes(b"{}")
        self.release_to("v1.2.0", self.root / "stable")
        self.assertEqual(self.channels(site, "--publish", str(self.root / "stable")), 0)
        builds = []
        for index in range(3):
            out = self.root / f"dev{index}"
            self.preview_build(repository, out)
            builds.append(out)
            self.assertEqual(self.channels(site, "--publish", str(out)), 0)
            self.commit(repository, f"Change number {index}")
        self.assertEqual(self.site_versions(site), {"stable": 1002000999, "preview": 1002001005})
        # The current package and the one before it stay; older ones go.
        self.assertEqual(sorted(path.name for path in feed.glob("*.zip")),
                         ["MaiCord-x64-p2-1002001004.zip", "MaiCord-x64-p2-1002001005.zip"])
        log = json.loads((feed / "maicord-preview.changelog.json").read_bytes())
        self.assertEqual((log["version"], log["entries"][0]["text"]), (1002001005, "Change number 1"))
        # Replaying an older build never moves the feed back.
        self.assertEqual(self.channels(site, "--publish", str(builds[0])), 0)
        self.assertEqual(self.site_versions(site)["preview"], 1002001005)
        # A dev manifest never reaches the stable channel.
        bad = self.root / "bad"
        bad.mkdir()
        for name in ("maicord-preview.manifest.json", "maicord-preview.manifest.json.sig"):
            (bad / name.replace("preview", "stable")).write_bytes((builds[2] / name).read_bytes())
        self.assertEqual(self.channels(site, "--publish", str(bad)), 1)
        # A stable release moves every channel; dev packages are retired.
        self.commit(repository, "MaiCord 1.2.1", project="1.2.1")
        self.git(repository, "tag", "v1.2.1")
        released = self.root / "released"
        self.release_to("v1.2.1", released)
        self.release_changelogs(repository, "v1.2.1", released)
        self.assertEqual(self.channels(site, "--publish", str(released)), 0)
        self.assertEqual(set(self.site_versions(site).values()), {1002001999})
        self.assertEqual(list(feed.glob("*.zip")), [])
        stable_log = json.loads((site / "updates" / "p2" / "maicord-stable.changelog.json").read_bytes())
        self.assertEqual((stable_log["version"], stable_log["entries"][0]["text"]), (1002001999, "### Fixes\n\n- Fix it"))
        preview_log = json.loads((feed / "maicord-preview.changelog.json").read_bytes())
        self.assertEqual((preview_log["version"], preview_log["base"]), (1002001999, 1002000999))
        self.assertEqual(preview_log["entries"][0]["text"], "Change number 2")
        # A manifest that moves without its changelog leaves no stale one.
        self.commit(repository, "Start 1.2.2")
        dev = self.root / "dev-after"
        self.preview_build(repository, dev)
        (dev / "maicord-preview.changelog.json").unlink()
        self.assertEqual(self.channels(site, "--publish", str(dev)), 0)
        self.assertFalse((feed / "maicord-preview.changelog.json").exists())
        # A missing package for the served build is refused.
        (feed / "MaiCord-x64-p2-1002002001.zip").unlink()
        self.assertEqual(self.channels(site), 1)
        self.assertEqual(other.read_bytes(), b"{}")

    def test_channels_refresh_only_when_expiring(self):
        out = self.root / "dist"
        self.release_to("v2.0.0", out, "--validity-days", "10")
        site = self.root / "site"
        self.assertEqual(self.channels(site, "--publish", str(out)), 0)
        old = self.site_fields(site, "stable")
        self.assertEqual(self.channels(site, "--refresh-within-days", "5"), 0)
        self.assertEqual(self.site_fields(site, "stable"), old)
        self.assertEqual(self.channels(site, "--refresh-within-days", "30"), 0)
        for slot in release.SLOTS:
            new = self.site_fields(site, slot.name)
            self.assertEqual({k: new[k] for k in ("version", "bytes", "sha256", "channel")},
                             {k: old[k] for k in ("version", "bytes", "sha256")} | {"channel": slot.channel})
            self.assertGreater(new["expires"], old["expires"])
        # Damaged site state is refused instead of being deployed.
        (site / "updates" / "p2" / "preview" / "maicord-preview.manifest.json.sig").write_bytes(b"\0" * 64)
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
        empty = release.render_notes(release.parse_tag("v1.3.1"), None, [("a" * 40, "Bump version to 1.3.1"),
                                                                          ("b" * 40, "MaiCord 1.3.1")], "")
        self.assertIn("No user-visible changes.", empty)
        self.assertNotIn("compare", empty)

    def test_release_notes_start_with_the_tags_handwritten_notes(self):
        repository = self.git_repository()
        handwritten = repository / ".github" / "release-notes" / "v1.3.0.md"
        handwritten.parent.mkdir(parents=True)
        handwritten.write_bytes(b"### Before you update\r\n\r\nReplace `MaiCord.exe` by hand.\r\n")
        self.commit(repository, "Release 1.3.0")
        self.git(repository, "tag", "v1.3.0")
        # The tagged commit's file counts, not the working tree's.
        handwritten.write_text("Not in the tag.\n", encoding="utf-8")
        output = self.root / "notes.md"
        directory = os.getcwd()
        os.chdir(repository)
        try:
            self.assertEqual(self.run_cli("notes", "--tag", "v1.3.0", "--output", str(output),
                                          "--repository", "owner/repo"), 0)
            self.assertEqual(release.handwritten_notes(".", "v1.2.0"), "")
        finally:
            os.chdir(directory)
        notes = output.read_text(encoding="utf-8")
        self.assertTrue(notes.startswith("## MaiCord 1.3.0\n\n### Before you update\n\nReplace `MaiCord.exe` by hand."
                                         "\n\n### Features\n"), notes)
        self.assertNotIn("Not in the tag", notes)
        self.assertNotIn("\r", notes)
        self.assertNotIn("Release 1.3.0", notes)
        # The in-app changelog keeps the hand-written notes.
        self.assertTrue(release.release_notes_text(notes).startswith(
            "### Before you update\n\nReplace `MaiCord.exe` by hand.\n\n### Features"))

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
