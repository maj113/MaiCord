#!/usr/bin/env python3
"""Self-contained checks for the release tooling with a throwaway key.

    python -B tools/release/test_release_tools.py

Everything is written to a temporary directory that is removed afterwards.
"""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
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

# Byte-for-byte copy of the accepted document in tests/app/update/manifest.cpp.
CLIENT_FIXTURE_DOCUMENT = (b'{"format":2,"product":"MaiCord","arch":"x64","channel":"stable","version":6,'
                           b'"issued":90,"expires":200,"bytes":4096,"sha256":'
                           b'"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",'
                           b'"artifact":"MaiCord-x64-p2-6.zip"}')
# Byte-for-byte copy of the accepted document in tests/app/update/changelog.cpp.
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


def fake_elf(machine: int = 62, kind: int = 3, tag: int = 7) -> bytes:
    """An x86-64 ELF executable of the structure the client checks (never run): a code segment holding the entry
    point and a data segment with a zero-filled tail."""
    data = bytearray(0x1100)
    data[0:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHIQQQIHHH", data, 16, kind, machine, 1, 0x100, 64, 0, 0, 64, 56, 2)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0, 0, 0, 0x1000, 0x1000, 0x1000)
    struct.pack_into("<IIQQQQQQ", data, 120, 1, 6, 0x1000, 0x2000, 0, 0x100, 0x300, 0x1000)
    for index in range(0x100, 0x200):
        data[index] = (tag + index) & 255
    return bytes(data)


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

    # ---------------------------------------------------------------------------------------- the Linux package

    def linux_package(self, members=None, version: int = 1002003999, program: bytes | None = None) -> tuple[str, bytes]:
        """A release tarball in the producer's layout (GNU tar in one gzip member), with `members` replacing the
        default list of (path below the directory, type, mode, data)."""
        name = release.linux_artifact_name(version)
        top = name[:-len(".tar.gz")]
        if members is None:
            members = [("", tarfile.DIRTYPE, 0o755, b""), ("MaiCord", tarfile.REGTYPE, 0o755, program or fake_elf()),
                       ("fonts", tarfile.DIRTYPE, 0o755, b""), ("fonts/Kalam.ttf", tarfile.REGTYPE, 0o644, b"kalam"),
                       ("licenses", tarfile.DIRTYPE, 0o755, b""), ("licenses/Opus-BSD.txt", tarfile.REGTYPE, 0o644, b"opus"),
                       ("share/icons/a-long-path-" + "x" * 90 + ".png", tarfile.REGTYPE, 0o644, b"png"),
                       (".maicord-portable", tarfile.REGTYPE, 0o644, b"maicord-portable 1\n")]
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w", format=tarfile.GNU_FORMAT) as archive:
            for path, kind, mode, data in members:
                info = tarfile.TarInfo(f"{top}/{path}" if path else top)
                info.type, info.mode, info.size = kind, mode, len(data) if kind == tarfile.REGTYPE else 0
                if kind == tarfile.SYMTYPE:
                    info.linkname = "MaiCord"
                archive.addfile(info, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
        return name, gzip.compress(buffer.getvalue(), mtime=0)

    def test_linux_names_and_manifest_layout(self):
        self.assertEqual(release.linux_artifact_name(1002003999), "maicord-1.2.3-linux-x86_64.tar.gz")
        self.assertEqual(release.linux_artifact_name(1002003005), "maicord-1.2.3-dev.5-linux-x86_64.tar.gz")
        self.assertEqual(release.linux_artifact_name(6), "maicord-0.0.0-dev.6-linux-x86_64.tar.gz")
        for bad in (1002003000, 1000000000999):
            with self.assertRaises(release.ReleaseError):
                release.linux_artifact_name(bad)
        digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        document = release.render_manifest(6, "stable", 90, 200, 4096, digest, release.linux_artifact_name(6), release.LINUX)
        self.assertEqual(document, CLIENT_FIXTURE_DOCUMENT.replace(b'"x64"', b'"linux-x64"').replace(
            b"MaiCord-x64-p2-6.zip", b"maicord-0.0.0-dev.6-linux-x86_64.tar.gz"))
        self.assertEqual(release.parse_manifest(document, release.LINUX)["artifact"], "maicord-0.0.0-dev.6-linux-x86_64.tar.gz")
        # Neither system's reader takes the other's manifest, though it is signed with the same key.
        with self.assertRaises(release.ReleaseError):
            release.parse_manifest(document)
        with self.assertRaises(release.ReleaseError):
            release.parse_manifest(CLIENT_FIXTURE_DOCUMENT, release.LINUX)
        with self.assertRaises(release.ReleaseError):
            release.render_manifest(6, "stable", 90, 200, 4096, digest, "MaiCord-x64-p2-6.zip", release.LINUX)
        with self.assertRaises(release.ReleaseError):
            release.render_manifest(6, "stable", 90, 200, 4096, digest, release.linux_artifact_name(6))

    def test_linux_tarball_matches_client_subset(self):
        name, package = self.linux_package()
        self.assertEqual(release.validate_linux_tarball(package, name),
                         ["MaiCord", "fonts/Kalam.ttf", "licenses/Opus-BSD.txt"])
        top = name[:-len(".tar.gz")]
        file, directory = tarfile.REGTYPE, tarfile.DIRTYPE
        good = [("", directory, 0o755, b""), ("MaiCord", file, 0o755, fake_elf())]
        broken = {
            "no program": [("fonts/Kalam.ttf", file, 0o644, b"x")],
            "program that is not executable": [("MaiCord", file, 0o644, fake_elf())],
            "program that is not an ELF": [("MaiCord", file, 0o755, b"#!/bin/sh\n")],
            "ELF for another machine": [("MaiCord", file, 0o755, fake_elf(machine=183))],
            "shared object": [("MaiCord", file, 0o755, fake_elf(kind=1))],
            "second program": good + [("MaiCord", file, 0o755, fake_elf())],
            "repeated font": good + [("fonts/A.ttf", file, 0o644, b"a"), ("fonts/A.ttf", file, 0o644, b"b")],
            "nested font": good + [("fonts/deeper/A.ttf", file, 0o644, b"a")],
            "hidden font": good + [("fonts/.hidden", file, 0o644, b"a")],
            "font with a space": good + [("fonts/A b.ttf", file, 0o644, b"a")],
            "link": good + [("fonts/Link.ttf", tarfile.SYMTYPE, 0o777, b"")],
            "path that climbs out": good + [("fonts/../../escape", file, 0o644, b"a")],
            "program as a directory": [("MaiCord", directory, 0o755, b"")],
            "program with a child": good + [("MaiCord/x", file, 0o644, b"a")],
        }
        for label, members in broken.items():
            with self.assertRaises(release.ReleaseError, msg=label):
                release.validate_linux_tarball(self.linux_package(members)[1], name)
        # Outside the package's directory, in another version's directory, and not a .tar.gz at all.
        outside = self.linux_package(good + [("../elsewhere", file, 0o644, b"a")])[1]
        with self.assertRaises(release.ReleaseError):
            release.validate_linux_tarball(outside, name)
        with self.assertRaises(release.ReleaseError):
            release.validate_linux_tarball(package, release.linux_artifact_name(1002004999))
        with self.assertRaises(release.ReleaseError):
            release.validate_linux_tarball(package, top)
        # Not one gzip member: truncated, a second member, trailing bytes, a damaged checksum.
        damaged = bytearray(package)
        damaged[-8] ^= 1
        for bad in (package[:-30], package + gzip.compress(b"more", mtime=0), package + b"\0", bytes(damaged), b""):
            with self.assertRaises(release.ReleaseError):
                release.validate_linux_tarball(bad, name)
        # A tar that does not end, or has data after its end marker.
        raw = gzip.decompress(package)
        unterminated = raw[:(len(raw.rstrip(b"\0")) + 511) // 512 * 512]
        for bad in (unterminated, raw + b"x" * 512):
            with self.assertRaises(release.ReleaseError):
                release.validate_linux_tarball(gzip.compress(bad, mtime=0), name)

    def test_build_tarball_output_is_a_valid_linux_package(self):
        program = self.root / "MaiCord"
        program.write_bytes(fake_elf())
        output = self.root / "tarball"
        packaging = Path(__file__).resolve().parents[2] / "packaging" / "build_tarball.py"
        subprocess.run([sys.executable, "-B", str(packaging), "--binary", str(program), "--version", "1.2.3",
                        "--skip-audit-for-synthetic-image", "--output-dir", str(output)], check=True, capture_output=True,
                       env=dict(os.environ, SOURCE_DATE_EPOCH="1760000000"))
        name = release.linux_artifact_name(release.parse_tag("v1.2.3").number)
        unpacked = release.validate_linux_tarball((output / name).read_bytes(), name)
        self.assertIn("MaiCord", unpacked)
        self.assertTrue(any(path.startswith("fonts/") for path in unpacked) and
                        any(path.startswith("licenses/") for path in unpacked), unpacked)

    def linux_release(self, out: Path, tag: str = "v1.2.3", *extra: str) -> int:
        version = release.parse_tag(tag).number
        name, package = self.linux_package(version=version)
        tarball = self.root / "tarballs" / name
        tarball.parent.mkdir(exist_ok=True)
        tarball.write_bytes(package)
        return self.run_cli("linux", "--tag", tag, "--tarball", str(tarball), "--output-dir", str(out), "--key-file",
                            str(self.key_file), *extra)

    def test_linux_release_signs_and_verifies_each_channel(self):
        out = self.root / "dist"
        public = self.key.public_point.hex()
        self.assertEqual(self.linux_release(out, "v1.2.3", "--public-key", public), 0)
        name = release.linux_artifact_name(1002003999)
        package = (self.root / "tarballs" / name).read_bytes()
        for channel in ("stable", "preview"):
            stem = f"maicord-linux-{channel}.manifest.json"
            document, signature = (out / stem).read_bytes(), (out / f"{stem}.sig").read_bytes()
            fields = release.verify_offer(document, signature, self.key.public_point, package, flavor=release.LINUX)
            self.assertEqual((fields["channel"], fields["version"], fields["artifact"]), (channel, 1002003999, name))
            self.assertEqual(self.run_cli("verify", "--manifest", str(out / stem), "--signature", str(out / f"{stem}.sig"),
                                          "--public-key", public, "--artifact", str(self.root / "tarballs" / name),
                                          "--linux"), 0)
            # The Windows reader refuses it, as it refuses a package that is not its own.
            with self.assertRaises(release.ReleaseError):
                release.verify_offer(document, signature, self.key.public_point)
            self.assertEqual(self.run_cli("verify", "--manifest", str(out / stem), "--signature", str(out / f"{stem}.sig"),
                                          "--public-key", public), 1)
        # A tarball that is not named for the tag, one the sidecar disowns, a wrong key and a broken package.
        wrong = self.root / "tarballs" / release.linux_artifact_name(1002004999)
        wrong.write_bytes(package)
        self.assertEqual(self.run_cli("linux", "--tag", "v1.2.3", "--tarball", str(wrong), "--output-dir",
                                      str(self.root / "bad"), "--key-file", str(self.key_file)), 1)
        sidecar = self.root / "tarballs" / f"{name}.sha256"
        sidecar.write_text("0" * 64 + f"  {name}\n", encoding="ascii")
        self.assertEqual(self.linux_release(self.root / "bad", "v1.2.3"), 1)
        sidecar.write_text(hashlib.sha256(package).hexdigest() + f"  {name}\n", encoding="ascii")
        self.assertEqual(self.linux_release(self.root / "bad", "v1.2.3", "--public-key", self.other.public_point.hex()), 1)
        (self.root / "tarballs" / name).write_bytes(self.linux_package([("MaiCord", tarfile.REGTYPE, 0o755, b"nope")])[1])
        self.assertEqual(self.run_cli("linux", "--tag", "v1.2.3", "--tarball", str(self.root / "tarballs" / name),
                                      "--output-dir", str(self.root / "bad"), "--key-file", str(self.key_file)), 1)
        self.assertFalse((self.root / "bad").exists())

    def linux_site_fields(self, site: Path, slot: str) -> dict:
        document = site / next(entry.manifest for entry in release.LINUX_SLOTS if entry.name == slot)
        return release.verify_offer(document.read_bytes(), Path(f"{document}.sig").read_bytes(), self.key.public_point,
                                    flavor=release.LINUX)

    def test_channels_publish_the_linux_manifests_only_with_the_linux_flag(self):
        stable, linux = self.root / "stable", self.root / "linux"
        self.release_to("v1.2.0", stable)
        self.assertEqual(self.linux_release(linux, "v1.2.0"), 0)
        # One publish directory, as release.yml leaves it: the Windows manifests and the Linux ones.
        both = self.root / "both"
        both.mkdir()
        for directory in (stable, linux):
            for path in directory.iterdir():
                shutil.copyfile(path, both / path.name)
        site = self.root / "site"
        linux_directory = site / "updates" / "p2" / "linux"
        # Without the flag the Linux files are not read, and so are not installed.
        self.assertEqual(self.channels(site, "--publish", str(both)), 0)
        self.assertFalse(linux_directory.exists())
        self.assertEqual(self.channels(site, "--publish", str(both), "--linux"), 0)
        self.assertEqual(sorted(path.name for path in linux_directory.iterdir()),
                         ["maicord-stable.manifest.json", "maicord-stable.manifest.json.sig", "preview"])
        self.assertEqual(sorted(path.name for path in (linux_directory / "preview").iterdir()),
                         ["maicord-preview.manifest.json", "maicord-preview.manifest.json.sig"])
        self.assertEqual({slot.name: self.linux_site_fields(site, slot.name)["version"] for slot in release.LINUX_SLOTS},
                         {"linux-stable": 1002000999, "linux-preview": 1002000999})
        # The Windows reader never meets them: its files, and the Linux ones of another version, stay apart.
        self.assertEqual(self.site_versions(site), {"stable": 1002000999, "preview": 1002000999})
        # A Linux release moves both of its channels and never back; a dev build is refused on this feed.
        newer, older = self.root / "newer", self.root / "older"
        self.assertEqual(self.linux_release(newer, "v1.3.0"), 0)
        self.assertEqual(self.linux_release(older, "v1.1.0"), 0)
        self.assertEqual(self.channels(site, "--publish", str(newer), "--linux"), 0)
        self.assertEqual(self.channels(site, "--publish", str(older), "--linux"), 0)
        self.assertEqual({slot.name: self.linux_site_fields(site, slot.name)["version"] for slot in release.LINUX_SLOTS},
                         {"linux-stable": 1003000999, "linux-preview": 1003000999})
        # A development manifest never reaches the stable channel; on the preview feed it needs its tarball beside it.
        dev = self.root / "dev"
        dev.mkdir()
        document = release.render_manifest(1002003005, "stable", 10, 10 + 86400, 100, "0" * 64,
                                           release.linux_artifact_name(1002003005), release.LINUX)
        (dev / "maicord-linux-stable.manifest.json").write_bytes(document)
        (dev / "maicord-linux-stable.manifest.json.sig").write_bytes(release.sign_document(self.key, document, None))
        self.assertEqual(self.channels(site, "--publish", str(dev), "--linux"), 1)
        # A manifest signed by another key, or the Windows one at the Linux name, is refused outright.
        forged = self.root / "forged"
        other_key = self.root / "other.jwk"
        other_key.write_text(self.other.to_jwk(), encoding="utf-8")
        self.assertEqual(self.linux_release(forged, "v9.0.0", "--key-file", str(other_key)), 0)
        self.assertEqual(self.channels(site, "--publish", str(forged), "--linux"), 1)
        copied = self.root / "copied"
        copied.mkdir()
        shutil.copyfile(stable / "maicord-stable.manifest.json", copied / "maicord-linux-stable.manifest.json")
        shutil.copyfile(stable / "maicord-stable.manifest.json.sig", copied / "maicord-linux-stable.manifest.json.sig")
        self.assertEqual(self.channels(site, "--publish", str(copied), "--linux"), 1)
        self.assertEqual({slot.name: self.linux_site_fields(site, slot.name)["version"] for slot in release.LINUX_SLOTS},
                         {"linux-stable": 1003000999, "linux-preview": 1003000999})
        # Expiring Linux manifests are re-signed with the rest when the flag is given, and only then.
        short = self.root / "short"
        self.assertEqual(self.linux_release(short, "v2.0.0", "--validity-days", "10"), 0)
        self.assertEqual(self.channels(site, "--publish", str(short), "--linux"), 0)
        old = self.linux_site_fields(site, "linux-stable")
        self.assertEqual(self.channels(site, "--refresh-within-days", "30"), 0)
        self.assertEqual(self.linux_site_fields(site, "linux-stable"), old)
        self.assertEqual(self.channels(site, "--refresh-within-days", "30", "--linux"), 0)
        new = self.linux_site_fields(site, "linux-stable")
        self.assertGreater(new["expires"], old["expires"])
        self.assertEqual({k: new[k] for k in ("version", "bytes", "sha256")}, {k: old[k] for k in ("version", "bytes", "sha256")})
        # Damaged Linux state is refused with the flag instead of being deployed.
        (linux_directory / "preview" / "maicord-preview.manifest.json.sig").write_bytes(b"\0" * 64)
        self.assertEqual(self.channels(site, "--refresh-within-days", "30", "--linux"), 1)
        self.assertEqual(self.channels(site, "--refresh-within-days", "30"), 0)

    def linux_preview_build(self, repository: Path, out: Path, *extra: str) -> int:
        """HEAD's master build as a Linux tarball and its signed preview manifest, beside the Windows files in out."""
        identity = release.build_identity(repository, "HEAD")
        name, package = self.linux_package(version=identity.number)
        tarball = self.root / "tarballs" / name
        tarball.parent.mkdir(exist_ok=True)
        tarball.write_bytes(package)
        return self.run_cli("linux", "--preview", "--tarball", str(tarball), "--output-dir", str(out), "--repository",
                            str(repository), "--key-file", str(self.key_file), *extra)

    def test_channels_serve_linux_master_builds_on_the_linux_preview_feed_only(self):
        repository = self.git_repository()
        site = self.root / "site"
        feed = site / "updates" / "p2" / "linux" / "preview"
        stable_out = self.root / "stable"
        self.release_to("v1.2.0", stable_out)
        self.assertEqual(self.linux_release(stable_out, "v1.2.0"), 0)
        self.assertEqual(self.channels(site, "--publish", str(stable_out), "--linux"), 0)
        builds = []
        for index in range(3):
            out = self.root / f"dev{index}"
            self.preview_build(repository, out)
            self.assertEqual(self.linux_preview_build(repository, out), 0)
            builds.append(out)
            self.assertEqual(self.channels(site, "--publish", str(out), "--linux"), 0)
            self.commit(repository, f"Change number {index}")
        linux = {slot.name: self.linux_site_fields(site, slot.name)["version"] for slot in release.LINUX_SLOTS}
        self.assertEqual(linux, {"linux-stable": 1002000999, "linux-preview": 1002001005})
        self.assertEqual(self.site_versions(site), {"stable": 1002000999, "preview": 1002001005})
        # The current tarball and the one before it stay; older ones go. The Windows feed keeps its own packages.
        self.assertEqual(sorted(path.name for path in feed.glob("*.tar.gz")),
                         ["maicord-1.2.1-dev.4-linux-x86_64.tar.gz", "maicord-1.2.1-dev.5-linux-x86_64.tar.gz"])
        self.assertEqual(sorted(path.name for path in (site / "updates" / "p2" / "preview").glob("*.zip")),
                         ["MaiCord-x64-p2-1002001004.zip", "MaiCord-x64-p2-1002001005.zip"])
        fields = self.linux_site_fields(site, "linux-preview")
        package = feed / fields["artifact"]
        self.assertEqual(hashlib.sha256(package.read_bytes()).hexdigest(), fields["sha256"])
        # Replaying an older build never moves the feed back; without --linux the Linux files are left as they are.
        self.assertEqual(self.channels(site, "--publish", str(builds[0]), "--linux"), 0)
        self.assertEqual(self.linux_site_fields(site, "linux-preview")["version"], 1002001005)
        before = sorted(path.name for path in feed.iterdir())
        self.assertEqual(self.channels(site, "--publish", str(builds[0])), 0)
        self.assertEqual(sorted(path.name for path in feed.iterdir()), before)
        # A dev manifest never reaches the Linux stable channel.
        bad = self.root / "bad"
        bad.mkdir()
        for name in ("maicord-linux-preview.manifest.json", "maicord-linux-preview.manifest.json.sig"):
            (bad / name.replace("preview", "stable")).write_bytes((builds[2] / name).read_bytes())
        self.assertEqual(self.channels(site, "--publish", str(bad), "--linux"), 1)
        # Its tarball must come with it, and be the one the manifest names.
        lonely = self.root / "lonely"
        lonely.mkdir()
        for name in ("maicord-linux-preview.manifest.json", "maicord-linux-preview.manifest.json.sig"):
            (lonely / name).write_bytes((builds[2] / name).read_bytes())
        self.assertEqual(self.channels(site, "--publish", str(lonely), "--linux"), 1)
        # A stable release moves every Linux channel, and the development tarballs are retired.
        self.commit(repository, "MaiCord 1.2.1", project="1.2.1")
        self.git(repository, "tag", "v1.2.1")
        released = self.root / "released"
        self.release_to("v1.2.1", released)
        self.assertEqual(self.linux_release(released, "v1.2.1"), 0)
        self.assertEqual(self.channels(site, "--publish", str(released), "--linux"), 0)
        self.assertEqual({slot.name: self.linux_site_fields(site, slot.name)["version"] for slot in release.LINUX_SLOTS},
                         {"linux-stable": 1002001999, "linux-preview": 1002001999})
        self.assertEqual(list(feed.glob("*.tar.gz")), [])
        self.assertEqual(self.run_cli("linux", "--preview", "--tarball", str(self.root / "tarballs" /
                                      release.linux_artifact_name(1002001999)), "--output-dir", str(self.root / "no"),
                                      "--repository", str(repository), "--key-file", str(self.key_file)), 1)
        # A development manifest after it, a missing package for the served build, and a tarball of another build.
        self.commit(repository, "Start 1.2.2")
        dev = self.root / "dev-after"
        self.preview_build(repository, dev)
        self.assertEqual(self.linux_preview_build(repository, dev), 0)
        self.assertEqual(self.channels(site, "--publish", str(dev), "--linux"), 0)
        self.assertEqual(self.linux_site_fields(site, "linux-preview")["version"], 1002002001)
        (feed / release.linux_artifact_name(1002002001)).unlink()
        self.assertEqual(self.channels(site, "--linux"), 1)
        self.assertEqual(self.linux_preview_build(repository, self.root / "wrong", "--expect-version", "9.9.9-dev.1"), 1)

    # ---- the AppImage's manifests, a flavor of their own on the Linux feed

    def appimage_package(self, version: int = 1002003999, runtime: bytes | None = None, image: bytes = b"hsqs") -> tuple[str, bytes]:
        """An AppImage in the layout the client checks (never run): the type 2 runtime's ELF header, whose section header
        table ends the runtime, then the SquashFS."""
        header = bytearray(4096)
        header[0:4] = b"\x7fELF"
        header[4], header[5], header[8:11] = 2, 1, b"AI\x02"
        struct.pack_into("<Q", header, 0x28, 4096 - 64)
        struct.pack_into("<HH", header, 0x3A, 64, 1)
        return release.appimage_artifact_name(version), (runtime or bytes(header)) + image + b"\0" * 4096

    def appimage_release(self, out: Path, tag: str = "v1.2.3", *extra: str) -> int:
        name, package = self.appimage_package(release.parse_tag(tag).number)
        path = self.root / "appimages" / name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(package)
        return self.run_cli("linux", "--appimage", "--tag", tag, "--tarball", str(path), "--output-dir", str(out),
                            "--key-file", str(self.key_file), *extra)

    def appimage_site_fields(self, site: Path, slot: str) -> dict:
        document = site / next(entry.manifest for entry in release.APPIMAGE_SLOTS if entry.name == slot)
        return release.verify_offer(document.read_bytes(), Path(f"{document}.sig").read_bytes(), self.key.public_point,
                                    flavor=release.APPIMAGE)

    def test_appimage_names_manifest_and_package_checks(self):
        self.assertEqual(release.appimage_artifact_name(1002003999), "MaiCord-1.2.3-x86_64.AppImage")
        self.assertEqual(release.appimage_artifact_name(1002003005), "MaiCord-1.2.3-dev.5-x86_64.AppImage")
        for bad in (1002003000, 1000000000999):
            with self.assertRaises(release.ReleaseError):
                release.appimage_artifact_name(bad)
        number = release.APPIMAGE.package_number
        self.assertEqual(number("MaiCord-1.2.3-x86_64.AppImage"), 1002003999)
        self.assertEqual(number("MaiCord-1.2.3-dev.5-x86_64.AppImage"), 1002003005)
        self.assertIsNone(number("MaiCord-1.2.3-x86_64-musl.AppImage"))
        self.assertIsNone(number("maicord-1.2.3-linux-x86_64.tar.gz"))
        digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        document = release.render_manifest(6, "stable", 90, 200, 4096, digest, release.appimage_artifact_name(6), release.APPIMAGE)
        self.assertEqual(document, CLIENT_FIXTURE_DOCUMENT.replace(b'"x64"', b'"linux-x64-appimage"').replace(
            b"MaiCord-x64-p2-6.zip", b"MaiCord-0.0.0-dev.6-x86_64.AppImage"))
        self.assertEqual(release.parse_manifest(document, release.APPIMAGE)["arch"], "linux-x64-appimage")
        # Each reader takes only its own manifest, though all are signed with the same key.
        for other in (None, release.LINUX):
            with self.assertRaises(release.ReleaseError):
                release.parse_manifest(document, other) if other else release.parse_manifest(document)
        tarball_document = release.render_manifest(6, "stable", 90, 200, 4096, digest, release.linux_artifact_name(6), release.LINUX)
        with self.assertRaises(release.ReleaseError):
            release.parse_manifest(tarball_document, release.APPIMAGE)
        # The package: the runtime's identification and a SquashFS where the runtime's ELF ends.
        name, package = self.appimage_package()
        release.validate_appimage(package, name)
        broken = bytearray(package)
        broken[9] = ord("X")
        for label, bad in (("not a type 2 runtime", bytes(broken)), ("no SquashFS", self.appimage_package(image=b"nope")[1]),
                           ("truncated", package[:2000]), ("an ELF without the AppImage marks", fake_elf()),
                           ("a tarball", self.linux_package()[1])):
            with self.assertRaises(release.ReleaseError, msg=label):
                release.validate_appimage(bad, name)
        with self.assertRaises(release.ReleaseError):
            release.validate_appimage(package, "MaiCord-1.2.3-x86_64.zip")

    def test_appimage_release_signs_and_verifies_each_channel(self):
        out = self.root / "dist"
        public = self.key.public_point.hex()
        self.assertEqual(self.appimage_release(out, "v1.2.3", "--public-key", public), 0)
        name = release.appimage_artifact_name(1002003999)
        package = (self.root / "appimages" / name).read_bytes()
        for channel in ("stable", "preview"):
            stem = f"maicord-appimage-{channel}.manifest.json"
            document, signature = (out / stem).read_bytes(), (out / f"{stem}.sig").read_bytes()
            fields = release.verify_offer(document, signature, self.key.public_point, package, flavor=release.APPIMAGE)
            self.assertEqual((fields["channel"], fields["version"], fields["artifact"], fields["arch"]),
                             (channel, 1002003999, name, "linux-x64-appimage"))
            self.assertEqual(self.run_cli("verify", "--manifest", str(out / stem), "--signature", str(out / f"{stem}.sig"),
                                          "--public-key", public, "--artifact", str(self.root / "appimages" / name),
                                          "--appimage"), 0)
            # Neither the tarball's reader nor the Windows one takes it.
            for other in (release.LINUX, release.WINDOWS):
                with self.assertRaises(release.ReleaseError):
                    release.verify_offer(document, signature, self.key.public_point, flavor=other)
        # A file not named for the tag, a tarball under the AppImage's name, one the sidecar disowns and the wrong key.
        wrong = self.root / "appimages" / release.appimage_artifact_name(1002004999)
        wrong.write_bytes(package)
        self.assertEqual(self.run_cli("linux", "--appimage", "--tag", "v1.2.3", "--tarball", str(wrong), "--output-dir",
                                      str(self.root / "bad"), "--key-file", str(self.key_file)), 1)
        self.assertEqual(self.run_cli("linux", "--tag", "v1.2.3", "--tarball", str(self.root / "appimages" / name),
                                      "--output-dir", str(self.root / "bad"), "--key-file", str(self.key_file)), 1)
        sidecar = self.root / "appimages" / f"{name}.sha256"
        sidecar.write_text("0" * 64 + f"  {name}\n", encoding="ascii")
        self.assertEqual(self.appimage_release(self.root / "bad", "v1.2.3"), 1)
        sidecar.write_text(hashlib.sha256(package).hexdigest() + f"  {name}\n", encoding="ascii")
        self.assertEqual(self.appimage_release(self.root / "bad", "v1.2.3", "--public-key", self.other.public_point.hex()), 1)
        (self.root / "appimages" / name).write_bytes(self.linux_package()[1])
        self.assertEqual(self.run_cli("linux", "--appimage", "--tag", "v1.2.3", "--tarball", str(self.root / "appimages" / name),
                                      "--output-dir", str(self.root / "bad"), "--key-file", str(self.key_file)), 1)
        self.assertFalse((self.root / "bad").exists())

    def test_channels_publish_the_appimage_manifests_beside_the_linux_ones(self):
        stable, linux, image = self.root / "stable", self.root / "linux", self.root / "image"
        self.release_to("v1.2.0", stable)
        self.assertEqual(self.linux_release(linux, "v1.2.0"), 0)
        self.assertEqual(self.appimage_release(image, "v1.2.0"), 0)
        both = self.root / "both"
        both.mkdir()
        for directory in (stable, linux, image):
            for path in directory.iterdir():
                shutil.copyfile(path, both / path.name)
        site = self.root / "site"
        linux_directory = site / "updates" / "p2" / "linux"
        self.assertEqual(self.channels(site, "--publish", str(both)), 0)
        self.assertFalse(linux_directory.exists())
        self.assertEqual(self.channels(site, "--publish", str(both), "--linux"), 0)
        self.assertEqual(sorted(path.name for path in linux_directory.iterdir()),
                         ["appimage", "maicord-stable.manifest.json", "maicord-stable.manifest.json.sig", "preview"])
        self.assertEqual(sorted(path.name for path in (linux_directory / "appimage").iterdir()),
                         ["maicord-stable.manifest.json", "maicord-stable.manifest.json.sig", "preview"])
        self.assertEqual({slot.name: self.appimage_site_fields(site, slot.name)["version"] for slot in release.APPIMAGE_SLOTS},
                         {"appimage-stable": 1002000999, "appimage-preview": 1002000999})
        # The tarball's feed is its own: its manifests still say linux-x64.
        self.assertEqual({slot.name: self.linux_site_fields(site, slot.name)["arch"] for slot in release.LINUX_SLOTS},
                         {"linux-stable": "linux-x64", "linux-preview": "linux-x64"})
        # A newer release moves both of its channels and never back.
        newer, older = self.root / "newer", self.root / "older"
        self.assertEqual(self.appimage_release(newer, "v1.3.0"), 0)
        self.assertEqual(self.appimage_release(older, "v1.1.0"), 0)
        self.assertEqual(self.channels(site, "--publish", str(newer), "--linux"), 0)
        self.assertEqual(self.channels(site, "--publish", str(older), "--linux"), 0)
        self.assertEqual({slot.name: self.appimage_site_fields(site, slot.name)["version"] for slot in release.APPIMAGE_SLOTS},
                         {"appimage-stable": 1003000999, "appimage-preview": 1003000999})
        self.assertEqual({slot.name: self.linux_site_fields(site, slot.name)["version"] for slot in release.LINUX_SLOTS},
                         {"linux-stable": 1002000999, "linux-preview": 1002000999})
        # The tarball's manifest at the AppImage's name, and the reverse, are refused outright.
        for source, target in (("linux", "appimage"), ("appimage", "linux")):
            swapped = self.root / f"swapped-{source}"
            swapped.mkdir()
            for suffix in (".manifest.json", ".manifest.json.sig"):
                shutil.copyfile((linux if source == "linux" else image) / f"maicord-{source}-stable{suffix}",
                                swapped / f"maicord-{target}-stable{suffix}")
            self.assertEqual(self.channels(site, "--publish", str(swapped), "--linux"), 1)
        # Expiring AppImage manifests are re-signed with the rest when the flag is given.
        short = self.root / "short"
        self.assertEqual(self.appimage_release(short, "v2.0.0", "--validity-days", "10"), 0)
        self.assertEqual(self.channels(site, "--publish", str(short), "--linux"), 0)
        old = self.appimage_site_fields(site, "appimage-stable")
        self.assertEqual(self.channels(site, "--refresh-within-days", "30", "--linux"), 0)
        new = self.appimage_site_fields(site, "appimage-stable")
        self.assertGreater(new["expires"], old["expires"])

    def test_channels_serve_appimage_master_builds_on_the_appimage_preview_feed_only(self):
        repository = self.git_repository()
        site = self.root / "site"
        feed = site / "updates" / "p2" / "linux" / "appimage" / "preview"
        stable_out = self.root / "stable"
        self.release_to("v1.2.0", stable_out)
        self.assertEqual(self.appimage_release(stable_out, "v1.2.0"), 0)
        self.assertEqual(self.channels(site, "--publish", str(stable_out), "--linux"), 0)
        builds = []
        for index in range(3):
            out = self.root / f"dev{index}"
            identity = release.build_identity(repository, "HEAD")
            name, package = self.appimage_package(identity.number)
            path = self.root / "appimages" / name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(package)
            self.assertEqual(self.run_cli("linux", "--appimage", "--preview", "--tarball", str(path), "--output-dir", str(out),
                                          "--repository", str(repository), "--key-file", str(self.key_file)), 0)
            builds.append(out)
            self.assertEqual(self.channels(site, "--publish", str(out), "--linux"), 0)
            self.commit(repository, f"Change number {index}")
        versions = {slot.name: self.appimage_site_fields(site, slot.name)["version"] for slot in release.APPIMAGE_SLOTS}
        self.assertEqual(versions, {"appimage-stable": 1002000999, "appimage-preview": 1002001005})
        # The current AppImage and the one before it stay; older ones go. The tarball feed keeps to its own files.
        self.assertEqual(sorted(path.name for path in feed.glob("*.AppImage")),
                         ["MaiCord-1.2.1-dev.4-x86_64.AppImage", "MaiCord-1.2.1-dev.5-x86_64.AppImage"])
        self.assertFalse((site / "updates" / "p2" / "linux" / "preview").exists())
        fields = self.appimage_site_fields(site, "appimage-preview")
        self.assertEqual(hashlib.sha256((feed / fields["artifact"]).read_bytes()).hexdigest(), fields["sha256"])
        # A dev manifest on the stable channel, and one without its package, are refused.
        bad = self.root / "bad"
        bad.mkdir()
        for name in ("maicord-appimage-preview.manifest.json", "maicord-appimage-preview.manifest.json.sig"):
            (bad / name.replace("preview", "stable")).write_bytes((builds[2] / name).read_bytes())
        self.assertEqual(self.channels(site, "--publish", str(bad), "--linux"), 1)
        lonely = self.root / "lonely"
        lonely.mkdir()
        for name in ("maicord-appimage-preview.manifest.json", "maicord-appimage-preview.manifest.json.sig"):
            (lonely / name).write_bytes((builds[2] / name).read_bytes())
        self.assertEqual(self.channels(site, "--publish", str(lonely), "--linux"), 1)

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
