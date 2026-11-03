"""Self-test of maicord-plugin: every subcommand against real files, a real compile and the repository's SDK.

    python -B tools/maicord-plugin/test_maicord_plugin.py

Everything is written to a temporary directory that is removed afterwards; the real %LOCALAPPDATA%\\MaiCord is
never touched (the dev folders come from MAICORD_PLUGIN_DEV and MAICORD_EFFECT_DEV). Builds need the pinned LLVM
toolchain (or MAICORD_LLVM_BIN, or clang-cl on PATH) and are skipped, loudly, without one.
"""

import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.dont_write_bytecode = True
# A plugin built without .api_level declares the SDK header's MC_API_LEVEL.
API_LEVEL = int(re.search(r"#define MC_API_LEVEL (\d+)u",
                          (REPOSITORY / "include/maicord/sdk/maicord_plugin.h").read_text(encoding="utf-8")).group(1))

import maicord_plugin as cli  # noqa: E402
import mcfx_tool  # noqa: E402
import plugin_tool as core  # noqa: E402

try:
    TOOLCHAIN = core.find_toolchain(fetch=False)
except core.ToolError as error:
    TOOLCHAIN = None
    print(f"NOTE: no LLVM toolchain ({error}); the build tests are skipped", file=sys.stderr)
needs_toolchain = unittest.skipUnless(TOOLCHAIN, "no clang-cl and lld-link")
needs_windows = unittest.skipUnless(os.name == "nt", "the effect compiler is d3dcompiler_47.dll")


def run(*arguments, expect=0):
    """The command line in this process: (exit code, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cli.main([str(a) for a in arguments])
    return code, out.getvalue(), err.getvalue()


class Case(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="maicord-plugin-test-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.plugin_dev = self.root / "dev-plugins"
        self.effect_dev = self.root / "dev-effects"
        patch = {"MAICORD_PLUGIN_DEV": str(self.plugin_dev), "MAICORD_EFFECT_DEV": str(self.effect_dev)}
        old = {key: os.environ.get(key) for key in patch}
        os.environ.update(patch)

        def restore():
            for key, value in old.items():
                os.environ.pop(key, None) if value is None else os.environ.__setitem__(key, value)

        self.addCleanup(restore)

    def new(self, name="hello", **options):
        directory = self.root / name
        arguments = ["new", name, "--dir", directory]
        for key, value in options.items():
            arguments += [f"--{key}", value]
        code, out, err = run(*arguments)
        self.assertEqual(code, 0, err)
        return directory

    def built(self, name="hello"):
        directory = self.new(name)
        code, out, err = run("build", "--dir", directory)
        self.assertEqual(code, 0, out + err)
        return directory, directory / "build" / f"{name}.dll"


# ---- new ------------------------------------------------------------------------------------------------------
class NewTests(Case):
    def test_skeleton(self):
        directory = self.new("hello")
        self.assertEqual(sorted(p.name for p in directory.iterdir()), [".clangd", ".gitignore", "README.md", "plugin.cpp"])
        source = (directory / "plugin.cpp").read_text(encoding="utf-8")
        for needle in ("#include <maicord/sdk/plugin.hpp>", "MC_SETTINGS(", "MC_HOOKS(pre_send)", "MC_COMMAND(hello",
                       "MC_PLUGIN(", '.name = "Hello"'):
            self.assertIn(needle, source)
        self.assertLess(len(source.splitlines()), 60, "the skeleton stays ClearURLs-sized")
        ignore = (directory / ".gitignore").read_text()
        self.assertTrue(all(line in ignore.splitlines() for line in ("build/", "*.dll", ".clangd")))
        clangd = (directory / ".clangd").read_text()
        include = core.sdk_include().as_posix()
        self.assertIn(f"-I{include}", clangd)
        self.assertIn(f"-I{include}/maicord/sys/libc", clangd)
        self.assertIn("-nostdinc", clangd)
        self.assertIn("maicord-plugin build", (directory / "README.md").read_text())

    def test_owner_is_random_and_nonzero(self):
        owners = set()
        for index in range(4):
            text = (self.new(f"plug{index}") / "plugin.cpp").read_text(encoding="utf-8")
            match = re.search(r"\.owner = 0x([0-9A-F]{4}(?:'[0-9A-F]{4}){3})", text)
            self.assertIsNotNone(match)
            value = int(match.group(1).replace("'", ""), 16)
            self.assertTrue(0 < value < 1 << 64)
            owners.add(value)
        self.assertEqual(len(owners), 4)

    def test_names(self):
        directory = self.new("My Plugin")
        self.assertTrue(directory.is_dir())
        self.assertIn('.name = "My Plugin"', (directory / "plugin.cpp").read_text(encoding="utf-8"))
        self.assertEqual(core.slugify("My Plugin!"), "my-plugin")
        self.assertEqual(core.display_name("read-all"), "Read All")
        self.assertEqual(core.display_name("Read All"), "Read All")
        for bad in ("", '"quote"', "back\\slash", "x" * 65):
            with self.assertRaises(core.ToolError):
                core.display_name(bad)
        with self.assertRaises(core.ToolError):
            core.slugify("!!!")

    def test_command_name_is_a_valid_identifier(self):
        self.assertIn("MC_COMMAND(read_all,", (self.new("read-all") / "plugin.cpp").read_text(encoding="utf-8"))
        self.assertIn("MC_COMMAND(_2fast,", (self.new("2fast") / "plugin.cpp").read_text(encoding="utf-8"))

    def test_refuses_a_used_folder(self):
        directory = self.new("hello")
        code, out, err = run("new", "hello", "--dir", directory)
        self.assertEqual(code, 1)
        self.assertIn("not empty", err)
        (self.root / "empty").mkdir()
        self.assertEqual(run("new", "hello", "--dir", self.root / "empty")[0], 0)

    def test_sdk_is_found_or_reported(self):
        include = core.sdk_include()
        self.assertTrue((include / "maicord" / "sdk" / "plugin.hpp").is_file())
        self.assertEqual(core.sdk_include(str(include)), include)
        self.assertEqual(core.sdk_include(str(include.parent)), include, "a folder that holds include\\ works too")
        with self.assertRaises(core.ToolError):
            core.sdk_include(str(self.root))  # a named folder without the SDK is an error, not a silent fallback


# ---- build ----------------------------------------------------------------------------------------------------
class ToolchainTests(Case):
    def test_flags_are_cmakes(self):
        """The flags of maicord_plugin() (cmake/test_support.cmake) and the link contract all appear."""
        support = (REPOSITORY / "cmake" / "test_support.cmake").read_text(encoding="utf-8")
        compile_line = re.search(r"function\(maicord_plugin target\).*?target_compile_options\(\$\{target\} PRIVATE ([^\n]+)\)",
                                 support, re.S).group(1)
        for flag in re.findall(r"(?<![\w:$<])/[A-Za-z][^\s>)]*|/clang:-nostdinc", compile_line):
            self.assertIn(flag, core.COMPILE)
        self.assertIn("/GR-", core.COMPILE)
        contract = (REPOSITORY / "cmake" / "build_contract.cmake").read_text(encoding="utf-8")
        block = re.search(r"add_library\(maicord_link_contract INTERFACE\)\s*target_link_options\(maicord_link_contract INTERFACE(.*?)\)\n",
                          contract, re.S).group(1)
        for flag in re.findall(r"(?<![\w])/[A-Z][A-Za-z:=.,0-9-]*", block):
            self.assertIn(flag, core.LINK)
        self.assertIn("/NOENTRY", core.LINK)
        self.assertIn("/SUBSYSTEM:WINDOWS,6.02", core.LINK)

    def test_lookup_order(self):
        fake = self.root / "llvm"
        fake.mkdir()
        with self.assertRaises(core.ToolError):
            core.find_toolchain(str(fake))  # named, but empty: never falls back silently
        for name in ("clang-cl.exe", "lld-link.exe"):
            (fake / name).write_bytes(b"")
        found = core.find_toolchain(str(fake))
        self.assertEqual((found.clang.parent, found.origin), (fake, "--llvm-bin"))
        old = os.environ.get("MAICORD_LLVM_BIN")
        os.environ["MAICORD_LLVM_BIN"] = str(fake)
        self.addCleanup(lambda: os.environ.pop("MAICORD_LLVM_BIN", None) if old is None else os.environ.update(MAICORD_LLVM_BIN=old))
        self.assertEqual(core.find_toolchain().origin, "MAICORD_LLVM_BIN")

    def test_the_pin_is_read_from_the_repository(self):
        pin = json.loads((REPOSITORY / "tools" / "llvm_toolchain.json").read_text())
        self.assertEqual(core._pin(), pin)

    def test_fetch_checks_the_hash_and_unpacks(self):
        bundle = self.root / "pin.zip"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.writestr("bin/clang-cl.exe", b"a")
            archive.writestr("bin/lld-link.exe", b"b")
        digest = hashlib.sha256(bundle.read_bytes()).hexdigest()
        pin = {"repository": "x/y", "tag": "t", "asset": "a.zip", "sha256": digest}
        root = self.root / "tool"
        self.assertEqual(core.fetch_toolchain(pin, root, bundle.as_uri()), root / "bin")
        self.assertTrue((root / "bin" / "lld-link.exe").is_file())
        self.assertFalse(Path(str(root) + ".download").exists())
        other = self.root / "other"
        with self.assertRaises(core.ToolError) as caught:
            core.fetch_toolchain(dict(pin, sha256="0" * 64), other, bundle.as_uri())
        self.assertIn("not the pinned", str(caught.exception))
        self.assertFalse(other.exists(), "a download that fails its hash unpacks nothing")
        with self.assertRaises(core.ToolError):
            core.fetch_toolchain(dict(pin, sha256="nope"), other, bundle.as_uri())


@needs_toolchain
class BuildTests(Case):
    def test_build_makes_a_checked_dll(self):
        directory, dll = self.built("hello")
        self.assertTrue(dll.is_file() and 1000 < dll.stat().st_size < 64 * 1024)
        self.assertEqual(sorted(p.name for p in dll.parent.iterdir()), ["hello.dll"], "no objects or import libraries stay")
        report = core.inspect_dll(dll)
        self.assertTrue(report.ok, report.errors)
        info = report.info
        self.assertEqual((info["name"], info["version"], info["api_level"]), ("Hello", "0.1.0", API_LEVEL))
        source = (directory / "plugin.cpp").read_text(encoding="utf-8")
        owner = int(re.search(r"\.owner = (0x[0-9A-F']+)", source).group(1).replace("'", ""), 16)
        self.assertEqual(info["owner_id"], owner)
        self.assertEqual(info["permissions"], ["message_send"])
        self.assertEqual((info["settings"], info["commands"], info["network"]), (2, 1, []))
        self.assertTrue(info["hooks"] >= 1 and info["state_bytes"] == 4)

    def test_build_is_reproducible(self):
        directory, dll = self.built("hello")
        first = hashlib.sha256(dll.read_bytes()).hexdigest()
        self.assertEqual(run("build", "--dir", directory)[0], 0)
        self.assertEqual(hashlib.sha256(dll.read_bytes()).hexdigest(), first, "the same source is the same file (/Brepro)")

    def test_build_reports_compile_errors(self):
        directory = self.new("broken")
        source = directory / "plugin.cpp"
        source.write_text(source.read_text(encoding="utf-8").replace("MC_HOOKS(pre_send);", "MC_HOOKS(missing_function);"),
                          encoding="utf-8")
        code, out, err = run("build", "--dir", directory)
        self.assertEqual(code, 1)
        self.assertIn("the compile failed", err)
        self.assertIn("missing_function", err)
        self.assertFalse((directory / "build" / "broken.dll").exists())

    def test_a_failed_build_removes_the_old_dll(self):
        directory, dll = self.built("hello")
        source = directory / "plugin.cpp"
        source.write_text(source.read_text(encoding="utf-8") + "\nint broken(\n", encoding="utf-8")
        self.assertEqual(run("build", "--dir", directory)[0], 1)
        self.assertFalse(dll.exists(), "a stale DLL must not look like the build of the broken source")

    def test_build_outside_a_project(self):
        code, out, err = run("build", "--dir", self.root)
        self.assertEqual(code, 1)
        self.assertIn("plugin.cpp was not found", err)

    def test_the_samples_build_and_pass(self):
        """The catalog's sources compile with this tool (and so with the catalog's CI flags)."""
        for sample in ("clear-urls", "translate", "silent-toggle"):
            directory = self.root / sample
            directory.mkdir()
            shutil.copyfile(REPOSITORY / "samples" / "plugins" / sample / "plugin.cpp", directory / "plugin.cpp")
            code, out, err = run("build", "--dir", directory)
            self.assertEqual(code, 0, out + err)
        translate = core.inspect_dll(self.root / "translate" / "build" / "translate.dll").info
        self.assertEqual(translate["network"], ["translate.googleapis.com", "api-free.deepl.com", "api.deepl.com"])
        self.assertIn("network", translate["permissions"])

    def test_the_shim_runs_from_a_clean_shell(self):
        """new, build and dev as a user types them, through maicord-plugin.cmd in a fresh working folder."""
        shim = HERE / "maicord-plugin.cmd"
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        results = []
        for arguments, cwd in ((["new", "hello"], self.root), (["build"], self.root / "hello"), (["dev"], self.root / "hello")):
            done = subprocess.run([str(shim), *arguments], cwd=cwd, env=env, capture_output=True, text=True)
            results.append(done)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertTrue((self.plugin_dev / "hello.dll").is_file())
        self.assertIn("copied", results[2].stdout)
        self.assertEqual((self.plugin_dev / "hello.dll").read_bytes(), (self.root / "hello" / "build" / "hello.dll").read_bytes())


# ---- dev ------------------------------------------------------------------------------------------------------
@needs_toolchain
class DevTests(Case):
    def test_dev_builds_and_copies(self):
        directory = self.new("hello")
        code, out, err = run("dev", "--dir", directory)
        self.assertEqual(code, 0, out + err)
        self.assertIn("copied", out)
        self.assertEqual(sorted(p.name for p in self.plugin_dev.iterdir()), ["hello.dll"])
        code, out, err = run("dev", "--dir", directory)
        self.assertEqual(code, 0)
        self.assertIn("unchanged", out)

    def test_dev_copies_a_changed_build_atomically(self):
        directory, dll = self.built("hello")
        run("dev", "--dir", directory)
        first = (self.plugin_dev / "hello.dll").read_bytes()
        source = directory / "plugin.cpp"
        source.write_text(source.read_text(encoding="utf-8").replace('"0.1.0"', '"0.2.0"'), encoding="utf-8")
        os.utime(source, (time.time() + 5, time.time() + 5))
        code, out, err = run("dev", "--dir", directory)
        self.assertEqual(code, 0, out + err)
        self.assertIn("copied", out)
        second = (self.plugin_dev / "hello.dll").read_bytes()
        self.assertNotEqual(first, second)
        self.assertEqual(core.inspect_dll(self.plugin_dev / "hello.dll").info["version"], "0.2.0")
        self.assertEqual(sorted(p.name for p in self.plugin_dev.iterdir()), ["hello.dll"], "no .part file stays behind")

    def test_dev_rebuilds_only_a_stale_dll(self):
        directory, dll = self.built("hello")
        stamp = dll.stat().st_mtime_ns
        run("dev", "--dir", directory)
        self.assertEqual(dll.stat().st_mtime_ns, stamp, "an up-to-date DLL is copied, not rebuilt")
        project = core.open_project(str(directory))
        self.assertFalse(core.is_stale(project))
        os.utime(directory / "plugin.cpp", (time.time() + 5, time.time() + 5))
        self.assertTrue(core.is_stale(project))

    def test_dev_refuses_a_dll_that_fails_the_checks(self):
        directory = self.new("hello")
        source = directory / "plugin.cpp"
        source.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
        original = core.inspect_dll

        def failing(path):
            report = core.Inspection()
            report.errors.append("forced")
            return report

        core.inspect_dll = failing
        self.addCleanup(setattr, core, "inspect_dll", original)
        code, out, err = run("dev", "--dir", directory)
        self.assertEqual(code, 1)
        self.assertIn("forced", err)
        self.assertFalse(self.plugin_dev.exists())

    def test_watch_rebuilds_on_a_change(self):
        directory, dll = self.built("hello")
        project = core.open_project(str(directory))
        steps = []

        def touch():
            time.sleep(0.15)
            future = time.time() + 10
            os.utime(directory / "plugin.cpp", (future, future))

        thread = threading.Thread(target=touch)
        thread.start()
        core.watch(lambda: core.sources(project), lambda: steps.append(1), interval=0.05, rounds=12)
        thread.join()
        self.assertEqual(steps, [1], "one change, one step")
        steps.clear()
        core.watch(lambda: core.sources(project), lambda: steps.append(1), interval=0.02, rounds=3)
        self.assertEqual(steps, [], "no change, no step")

    def test_dev_watch_survives_a_compile_error(self):
        directory, dll = self.built("hello")
        source = directory / "plugin.cpp"

        def edit():
            time.sleep(0.8)
            source.write_text(source.read_text(encoding="utf-8") + "\nint broken(\n", encoding="utf-8")
            future = time.time() + 20
            os.utime(source, (future, future))

        thread = threading.Thread(target=edit)
        thread.start()
        code, out, err = run("dev", "--dir", directory, "--watch", "--rounds", 20)
        thread.join()
        self.assertEqual(code, 0, err)
        self.assertIn("the compile failed", out, "the error is printed and the watch goes on")


# ---- check ----------------------------------------------------------------------------------------------------
@needs_toolchain
class CheckTests(Case):
    def setUp(self):
        super().setUp()
        self.directory, self.dll = self.built("hello")
        self.good = self.dll.read_bytes()
        self.image = core._Image(self.good)

    def mutated(self, offset, data):
        bad = bytearray(self.good)
        bad[offset:offset + len(data)] = data
        path = self.root / "bad.dll"
        path.write_bytes(bytes(bad))
        return core.inspect_dll(path)

    def descriptor_offset(self):
        export_rva, _ = self.image.directory(0)
        functions = struct.unpack_from("<I", self.image.read(export_rva + 28, 4))[0]
        return self.image.offset(struct.unpack("<I", self.image.read(functions, 4))[0])

    def test_check_passes_a_good_dll(self):
        code, out, err = run("check", "--dir", self.directory)
        self.assertEqual(code, 0, out + err)
        self.assertIn("pass", out)
        self.assertIn("Hello 0.1.0", out)
        self.assertEqual(run("check", self.dll)[0], 0)

    def test_check_names_a_missing_file(self):
        code, out, err = run("check", self.root / "nothing.dll")
        self.assertEqual(code, 1)
        self.assertIn("error: cannot read", out)

    def test_pe_problems(self):
        entry = self.image.opt + 16
        self.assertIn("entry point", " ".join(self.mutated(entry, struct.pack("<I", 0x1000)).errors))
        tls = self.image.opt + 112 + 9 * 8
        self.assertIn("TLS", " ".join(self.mutated(tls, struct.pack("<II", 0x1000, 24)).errors))
        self.assertIn("x64", " ".join(self.mutated(self.image.nt + 4, struct.pack("<H", 0x14C)).errors))
        self.assertIn("not a DLL", " ".join(self.mutated(self.image.nt + 22, struct.pack("<H", 0x0002)).errors))
        self.assertIn("not a PE", " ".join(self.mutated(0, b"XX").errors))
        self.assertFalse(core.inspect_dll(self.write_prefix(100)).ok)
        self.assertFalse(core.inspect_dll(self.write_prefix(1500)).ok)

    def write_prefix(self, length):
        path = self.root / "short.dll"
        path.write_bytes(self.good[:length])
        return path

    def test_export_problems(self):
        export_rva, _ = self.image.directory(0)
        names_rva = struct.unpack("<I", self.image.read(export_rva + 32, 4))[0]
        name_rva = struct.unpack("<I", self.image.read(names_rva, 4))[0]
        report = self.mutated(self.image.offset(name_rva), b"maicord_plugin_v5")
        self.assertIn("not maicord_plugin_v6", " ".join(report.errors))
        count = export_rva + 20
        self.assertIn("exactly one", " ".join(self.mutated(self.image.offset(count), struct.pack("<I", 2)).errors))

    def test_writable_and_executable_section(self):
        table = self.image.opt + struct.unpack_from("<H", self.good, self.image.nt + 20)[0]
        for index, (name, *_rest) in enumerate(self.image.sections):
            flags_at = table + 40 * index + 36
            flags = struct.unpack_from("<I", self.good, flags_at)[0]
            if flags & 0x20000000:
                report = self.mutated(flags_at, struct.pack("<I", flags | 0x80000000))
                self.assertIn("writable and executable", " ".join(report.errors))
                return
        self.fail("no executable section")

    def test_descriptor_problems(self):
        at = self.descriptor_offset()
        self.assertIn("ABI 5", " ".join(self.mutated(at + 4, struct.pack("<I", 5)).errors))
        self.assertIn("owner", " ".join(self.mutated(at + 16, struct.pack("<Q", 0)).errors))
        self.assertIn("api level", " ".join(self.mutated(at + 8, struct.pack("<I", 0)).errors))
        self.assertIn("permission", " ".join(self.mutated(at + 104, struct.pack("<Q", 1 << 20)).errors))
        self.assertIn("flags", " ".join(self.mutated(at + 12, struct.pack("<I", 1)).errors))
        self.assertIn("name", " ".join(self.mutated(at + 24 + 8, struct.pack("<I", 0)).errors))
        self.assertIn("maximum", " ".join(self.mutated(at + 156, struct.pack("<I", 99)).errors))
        hooks_pointer = struct.unpack_from("<Q", self.good, at + 160)[0]
        hook_at = self.image.offset(hooks_pointer - self.image.base)
        self.assertIn("hook", " ".join(self.mutated(hook_at, struct.pack("<I", 99)).errors))
        self.assertIn("hook", " ".join(self.mutated(hook_at + 16, struct.pack("<Q", self.image.base + 8)).errors),
                      "a hook function outside the code is refused")

    def test_a_clean_dll_has_no_warnings(self):
        self.assertEqual(core.inspect_dll(self.dll).warnings, [])

    def test_hosts_grammar(self):
        for good in ("api.example.com", "*.example.com", "a-b.example.co"):
            self.assertTrue(core._host_valid(good), good)
        for bad in ("localhost", "Example.com", "*.com.", "1.2.3.4", "exa mple.com", "-a.example.com", "*example.com"):
            self.assertFalse(core._host_valid(bad), bad)


# ---- pack -----------------------------------------------------------------------------------------------------
@needs_toolchain
class PackTests(Case):
    def pack(self, directory, *options):
        return run("pack", "--dir", directory, *options)

    def test_pack_writes_the_catalog_layout(self):
        directory, dll = self.built("hello")
        code, out, err = self.pack(directory, "--author", "Ada", "--github", "ada", "--license", "MPL-2.0", "--category", "chat")
        self.assertEqual(code, 0, out + err)
        folder = directory / "dist" / "hello"
        self.assertEqual(sorted(p.name for p in folder.iterdir()), ["catalog.json", "details.md", "plugin.cpp"])
        self.assertEqual((folder / "plugin.cpp").read_bytes(), (directory / "plugin.cpp").read_bytes())
        catalog = json.loads((folder / "catalog.json").read_text(encoding="utf-8"))
        info = core.inspect_dll(dll).info
        self.assertEqual(catalog, {"name": "Hello", "summary": "Says hello and signs your messages.",
                                   "authors": [{"name": "Ada", "github": "ada"}], "license": "MPL-2.0",
                                   "categories": ["chat"], "owner_id": f"0x{info['owner_id']:016X}"})
        self.assertTrue((folder / "details.md").read_text().strip())
        # The catalog repo's members: nothing outside them.
        self.assertLessEqual(set(catalog), {"name", "summary", "authors", "license", "categories", "homepage",
                                            "derived_from", "owner_id", "changelog"})

    def test_pack_uses_the_descriptors_author_and_details_file(self):
        directory = self.new("hello")
        source = directory / "plugin.cpp"
        source.write_text(source.read_text(encoding="utf-8").replace('.summary = "Says hello and signs your messages.");',
                                                                         '.summary = "Says hello and signs your messages.", .author = "Grace");'),
                          encoding="utf-8")
        (directory / "DETAILS.md").write_text("Hello details.\n", encoding="utf-8")
        self.assertEqual(run("build", "--dir", directory)[0], 0)
        code, out, err = self.pack(directory, "--license", "MIT")
        self.assertEqual(code, 0, out + err)
        catalog = json.loads((directory / "dist" / "hello" / "catalog.json").read_text(encoding="utf-8"))
        self.assertEqual(catalog["authors"], [{"name": "Grace"}])
        self.assertEqual((directory / "dist" / "hello" / "details.md").read_text(), "Hello details.\n")
        # Packing does not make the DLL look stale: dist/ is not a source.
        self.assertFalse(core.is_stale(core.open_project(str(directory))))

    def test_pack_needs_what_the_catalog_needs(self):
        directory, dll = self.built("hello")
        code, out, err = self.pack(directory, "--license", "MPL-2.0")
        self.assertEqual(code, 1)
        self.assertIn("--author", err)
        code, out, err = self.pack(directory, "--author", "Ada")
        self.assertEqual(code, 1)
        self.assertIn("--license", err)
        code, out, err = self.pack(directory, "--author", "Ada", "--license", "MPL-2.0", "--category", "nonsense")
        self.assertEqual(code, 1)
        self.assertIn("--category", err)
        self.assertFalse((directory / "dist").exists(), "a refused pack writes nothing")

    def test_pack_refuses_an_existing_folder_without_force(self):
        directory, dll = self.built("hello")
        options = ("--author", "Ada", "--license", "MPL-2.0")
        self.assertEqual(self.pack(directory, *options)[0], 0)
        code, out, err = self.pack(directory, *options)
        self.assertEqual(code, 1)
        self.assertIn("--force", err)
        self.assertEqual(self.pack(directory, *options, "--force")[0], 0)

    def test_pack_needs_a_current_build(self):
        directory = self.new("hello")
        code, out, err = self.pack(directory, "--author", "Ada", "--license", "MPL-2.0")
        self.assertEqual(code, 1)
        self.assertIn("build first", err)

    def test_pack_refuses_what_the_catalog_refuses(self):
        directory = self.new("hello")
        source = directory / "plugin.cpp"
        source.write_text(source.read_text(encoding="utf-8").replace(
            '.summary = "Says hello and signs your messages.");',
            '.summary = "Says hello and signs your messages.", .permissions = MC_PERM_AUTOMATED_ACTIONS);'), encoding="utf-8")
        self.assertEqual(run("build", "--dir", directory)[0], 0)
        code, out, err = self.pack(directory, "--author", "Ada", "--license", "MPL-2.0")
        self.assertEqual(code, 1)
        self.assertIn("automated_actions", err)
        self.assertIn("act_messages", core.inspect_dll(directory / "build" / "hello.dll").info["permissions"],
                      "the SDK bakes the implied permission into the descriptor")


# ---- effect ---------------------------------------------------------------------------------------------------
@needs_windows
class EffectTests(Case):
    def effect(self, name="soft-rain"):
        directory = self.root / name
        self.assertEqual(run("effect", "new", name, "--dir", directory, "--author", "Ada")[0], 0)
        return directory

    def test_new_writes_a_buildable_effect(self):
        directory = self.effect("Soft Rain")
        self.assertEqual(sorted(p.name for p in directory.iterdir()), [".gitignore", "README.md", "soft-rain.effect", "soft-rain.hlsl"])
        source = (directory / "soft-rain.hlsl").read_text(encoding="utf-8")
        header = mcfx_tool.parse_header(source)
        self.assertEqual(header["effect"], {"name": "Soft Rain", "authors": "Ada", "license": "MPL-2.0", "version": "0.1.0"})
        self.assertEqual([k["id"] for k in header["knobs"]], ["speed", "mix"])
        self.assertEqual(mcfx_tool.parse_sidecar((directory / "soft-rain.effect").read_text()),
                         {"name": "Soft Rain", "uses": "blue_noise", "blend": "normal", "resolution": "50", "fps": "24",
                          "knobs": " ".join(["400", "350"] + ["0"] * 30)})
        self.assertEqual(run("effect", "new", "x", "--dir", directory)[0], 1, "an effect folder is not overwritten")

    def test_build_with_both_compilers(self):
        directory = self.effect()
        for compiler in ("d3dcompiler", "auto"):
            code, out, err = run("effect", "build", "--dir", directory, "--compiler", compiler)
            self.assertEqual(code, 0, out + err)
            self.assertIn("texture reads", out)
            package = (directory / "build" / "soft-rain.mcfx").read_bytes()
            self.assertEqual(package[:4], b"MCFX")
            sections = self.sections(package)
            self.assertEqual(sorted(sections), [1, 2, 5])
            self.assertEqual(sections[2][:4], b"DXBC")
            manifest = json.loads(sections[1])
            self.assertEqual((manifest["slug"], manifest["name"], manifest["interface"], manifest["blend"], manifest["uses"]),
                             ("soft-rain", "Soft Rain", 1, "normal", ["blue_noise"]))
            self.assertEqual(manifest["authors"], [{"name": "Ada"}])
            self.assertEqual([k["id"] for k in manifest["knobs"]], ["speed", "mix"])
            self.assertEqual(sections[5].decode(), (directory / "soft-rain.hlsl").read_text(encoding="utf-8"))
            self.assertEqual(sorted(p.name for p in (directory / "build").iterdir()), ["soft-rain.mcfx"])

    @staticmethod
    def sections(package):
        magic, version, count, total, zero = struct.unpack_from("<4sHHII", package, 0)
        assert (magic, version, total, zero) == (b"MCFX", 1, len(package), 0), "the container header"
        found, last = {}, 16 + 16 * count
        for index in range(count):
            kind, offset, size, pad = struct.unpack_from("<IIII", package, 16 + 16 * index)
            assert pad == 0 and offset == last, "sections are packed back to back"
            found[kind] = package[offset:offset + size]
            last = offset + size
        assert last == len(package), "the last section ends the file"
        return found

    def test_build_keeps_the_sidecar_in_step(self):
        directory = self.effect()
        source = directory / "soft-rain.hlsl"
        text = source.read_text(encoding="utf-8").replace("// @knob mix   float 0..1 = 0.35", "// @knob mix   float 0..1 = 0.6")
        source.write_text(text, encoding="utf-8")
        sidecar = directory / "soft-rain.effect"
        sidecar.write_text(sidecar.read_text().replace("resolution=50", "resolution=75").replace("fps=24", "fps=30"))
        self.assertEqual(run("effect", "build", "--dir", directory)[0], 0)
        values = mcfx_tool.parse_sidecar(sidecar.read_text())
        self.assertEqual((values["resolution"], values["fps"], values["knobs"]), ("75", "30", " ".join(["400", "600"] + ["0"] * 30)))
        manifest = json.loads(self.sections((directory / "build" / "soft-rain.mcfx").read_bytes())[1])
        self.assertEqual((manifest["resolution"], manifest["fps"]), (75, 30))

    def test_the_sidecar_holds_every_knob_value(self):
        """Knobs past the first twelve values (here a colour on row 4) reach the sidecar, as the catalog checks 32."""
        directory = self.effect()
        source = directory / "soft-rain.hlsl"
        knobs = "".join(f'// @knob k{i} float 0..20 = {i + 1} "K{i}"\n' for i in range(14))
        defines = "".join(f"#define knob_k{i} (mc_knobs[{(i + 2) // 4}].{'xyzw'[(i + 2) % 4]})\n" for i in range(14))
        text = source.read_text(encoding="utf-8").replace("// @uses", knobs + '// @knob tint color = #FF8000 "Tint"\n// @uses')
        text = text.replace("#define knob_mix   (mc_knobs[0].y)\n", "#define knob_mix   (mc_knobs[0].y)\n" + defines +
                            "#define knob_tint (mc_knobs[4].rgb)\n")
        source.write_text(text, encoding="utf-8")
        code, out, err = run("effect", "build", "--dir", directory, "--compiler", "d3dcompiler")
        self.assertEqual(code, 0, out + err)
        values = [int(v) for v in mcfx_tool.parse_sidecar((directory / "soft-rain.effect").read_text())["knobs"].split()]
        self.assertEqual(len(values), mcfx_tool.KNOB_ROWS * 4)
        self.assertEqual(values[2:16], [(i + 1) * 1000 for i in range(14)])
        self.assertEqual(values[16:20], [1000, 502, 0, 0], "the colour's rgb on row 4, beyond the old twelve values")

    def test_ambient_inputs_make_an_interface_2_package(self):
        directory = self.effect()
        source = directory / "soft-rain.hlsl"
        source.write_text(source.read_text(encoding="utf-8").replace("// @uses blue_noise", "// @uses appearance blue_noise clock")
                          .replace("wave * knob_mix", "wave * knob_mix * (1.0 - 0.5 * mc_ambient.dark) * mc_daylight(6.0, 19.0)"),
                          encoding="utf-8")
        code, out, err = run("effect", "build", "--dir", directory, "--compiler", "d3dcompiler")
        self.assertEqual(code, 0, out + err)
        manifest = json.loads(self.sections((directory / "build" / "soft-rain.mcfx").read_bytes())[1])
        self.assertEqual((manifest["interface"], manifest["uses"]), (2, ["blue_noise", "clock", "appearance"]))
        self.assertEqual(mcfx_tool.parse_sidecar((directory / "soft-rain.effect").read_text())["uses"], "blue_noise clock appearance")
        self.assertEqual(mcfx_tool.interface_of({"uses": ["blue_noise", "pointer"]}), 1, "an effect without them stays interface 1")

    def test_a_pass_graph_makes_an_interface_3_package(self):
        """spark-ribbon: four passes, two targets, six shaders of two stages, in DXBC and SHADERS."""
        directory = self.root / "spark-ribbon"
        shutil.copytree(REPOSITORY / "samples" / "effects" / "spark-ribbon", directory, ignore=shutil.ignore_patterns("*.webp"))
        code, out, err = run("effect", "build", "--dir", directory, "--compiler", "d3dcompiler")
        self.assertEqual(code, 0, out + err)
        self.assertIn("in 6 shaders", out)
        sections = self.sections((directory / "build" / "spark-ribbon.mcfx").read_bytes())
        self.assertEqual(sorted(sections), [1, 2, 5, 9])
        manifest = json.loads(sections[1])
        self.assertEqual(manifest["interface"], 3)
        self.assertEqual(manifest["targets"], [{"format": "rgba32f", "size": [32, 32], "persistent": True},
                                               {"format": "rgba16f", "scale": 50}])
        self.assertEqual(manifest["passes"], [{"ps": 0, "target": 0, "inputs": [0]},
                                              {"ps": 2, "vs": 1, "grid": [96, 24], "target": 1, "blend": "add", "clear": True},
                                              {"ps": 3, "inputs": [1]},
                                              {"ps": 5, "vs": 4, "quads": 1024, "inputs": [0], "blend": "add"}])
        # SHADERS: shaders 1..5, whole containers back to back, of the stage each pass uses them as.
        blob, stages = sections[9], []
        while blob:
            size = struct.unpack_from("<I", blob, 24)[0]
            mcfx_tool.check_bytecode(blob[:size], "vs_5_0" if len(stages) in (0, 3) else "ps_5_0")
            stages.append(size)
            blob = blob[size:]
        self.assertEqual(len(stages), 5)
        mcfx_tool.check_bytecode(sections[2], "ps_5_0")
        sidecar = mcfx_tool.parse_sidecar((directory / "spark-ribbon.effect").read_text())
        self.assertEqual((json.loads(sidecar["targets"]), json.loads(sidecar["passes"])), (manifest["targets"], manifest["passes"]))

    def test_graph_errors(self):
        good = '// @effect name="X" license="MIT"\n// @blend normal\n// @target a rgba16f 50%\n'
        self.assertIsNone(mcfx_tool.graph_of(mcfx_tool.parse_header('// @effect name="X" license="MIT"\n// @blend normal\n')))
        for text, needle in (
                ("// @pass p to=a\n", "draws the output"),
                ("// @pass p to=a in=a\n// @pass q\n", "persistent"),
                ("// @pass p vs=v\n", "grid=WxH or quads=N"),
                ("// @pass p grid=4x4\n", "grid=WxH or quads=N"),
                ("// @pass p vs=v grid=1x4\n", "2x2"),
                ("// @pass p vs=v quads=16385\n", "quads is 1"),
                ("// @pass p in=b\n", "no target 'b'"),
                ("// @pass p in=out\n", "no target 'out'"),
                ("// @pass p blend=multiply\n", "cannot read"),
                ("// @pass p\n" * 7, "at most 6 passes"),
                ("// @target b rgba32f 512x512 persistent\n// @target c rgba8 1x1\n// @pass p\n", "MiB"),
                ("// @target b rgba32f 100% persistent\n// @pass p\n", "per output pixel"),
                ("// @target b rgba8 513x2\n// @pass p\n", "fixed size"),
                ("// @pass p vs=p quads=1\n", "both a vertex and a pixel")):
            with self.assertRaises(core.ToolError, msg=text) as caught:
                mcfx_tool.graph_of(mcfx_tool.parse_header(good + text))
            self.assertIn(needle, str(caught.exception))
        with self.assertRaises(core.ToolError):
            mcfx_tool.parse_header(good)  # a target nothing draws
        with self.assertRaises(core.ToolError):
            mcfx_tool.parse_header(good + "// @target b rgba64 4x4\n// @pass p\n")

    def test_textures_are_packed_and_float_data_is_interface_3(self):
        directory = self.effect()
        source = directory / "soft-rain.hlsl"
        source.write_text(source.read_text(encoding="utf-8").replace("// @uses blue_noise", "// @uses blue_noise texture0 texture2")
                          .replace("wave * knob_mix", "wave * knob_mix * mc_tex0.Load(int3(0, 0, 0)).r * mc_tex2.Load(int3(0, 0, 0)).g"),
                          encoding="utf-8")
        code, out, err = run("effect", "build", "--dir", directory, "--compiler", "d3dcompiler")
        self.assertEqual(code, 1, "a declared texture needs its file")
        self.assertIn("texture0.png", err)
        png = (b"\x89PNG\r\n\x1a\n" + struct.pack(">I4sIIBBBBB", 13, b"IHDR", 2, 2, 8, 6, 0, 0, 0) + bytes(4))
        (directory / "texture0.png").write_bytes(png)
        (directory / "texture2.mcft").write_bytes(mcfx_tool.float_texture(2, 1, [(0.5, 2.0, -1.0, 1.0), (0, 0, 0, 1)]))
        code, out, err = run("effect", "build", "--dir", directory, "--compiler", "d3dcompiler")
        self.assertEqual(code, 0, out + err)
        sections = self.sections((directory / "build" / "soft-rain.mcfx").read_bytes())
        self.assertEqual((sorted(sections), sections[3], sections[7][:8]), ([1, 2, 3, 5, 7], png, b"MCFT\x02\x00\x01\x00"))
        self.assertEqual(json.loads(sections[1])["interface"], 3)
        (directory / "texture2.mcft").write_bytes(b"MCFT" + bytes(8))
        self.assertEqual(run("effect", "build", "--dir", directory, "--compiler", "d3dcompiler")[0], 1, "a bad float texture")

    def test_preview_simulates_the_clock_and_appearance(self):
        directory = self.effect()
        path = self.effect_dev / mcfx_tool.SIMULATION_FILE
        code, out, err = run("effect", "preview", "--dir", directory, "--time", "18:30", "--appearance", "light")
        self.assertEqual(code, 0, out + err)
        self.assertIn("time 18:30", out)
        self.assertIn("does not declare @uses clock appearance", out)
        self.assertEqual(mcfx_tool.parse_sidecar(path.read_text()), {"time": "18:30", "appearance": "light"})
        # A later run changes one key and keeps the others; a running day starts from the simulated time.
        self.assertEqual(run("effect", "preview", "--dir", directory, "--appearance", "dark", "--day-seconds", "120",
                             "--date", "12-24")[0], 0)
        self.assertEqual(mcfx_tool.parse_sidecar(path.read_text()),
                         {"time": "18:30", "date": "12-24", "day_seconds": "120", "appearance": "dark"})
        self.assertIn("simulates", run("effect", "preview", "--dir", directory)[1], "a plain preview says what is simulated")
        for bad in (("--time", "24:00"), ("--date", "02-30"), ("--day-seconds", "5")):
            code, out, err = run("effect", "preview", "--dir", directory, *bad)
            self.assertEqual(code, 1, bad)
        self.assertEqual(run("effect", "preview", "--dir", directory, "--real-inputs")[0], 0)
        self.assertFalse(path.exists())

    def test_a_missing_define_is_reported_with_the_line_to_add(self):
        directory = self.effect()
        source = directory / "soft-rain.hlsl"
        source.write_text(source.read_text(encoding="utf-8").replace("#define knob_mix   (mc_knobs[0].y)\n", ""), encoding="utf-8")
        code, out, err = run("effect", "build", "--dir", directory)
        self.assertEqual(code, 1)
        self.assertIn("#define knob_mix (mc_knobs[0].y)", err)

    def test_compile_errors_say_where(self):
        directory = self.effect()
        source = directory / "soft-rain.hlsl"
        source.write_text(source.read_text(encoding="utf-8").replace("return float4(colour", "return float4(undeclared"), encoding="utf-8")
        for compiler in ("d3dcompiler", "fxc"):
            if compiler == "fxc" and not mcfx_tool.find_fxc():
                continue
            code, out, err = run("effect", "build", "--dir", directory, "--compiler", compiler)
            self.assertEqual(code, 1)
            self.assertIn("undeclared", err)
            self.assertRegex(err, r"soft-rain\.hlsl\(\d+,\d+(-\d+)?\)|\(\d+,\d+(-\d+)?\)")
        self.assertFalse((directory / "build" / "soft-rain.mcfx").exists())

    def test_preview_copies_into_the_dev_folder(self):
        directory = self.effect()
        code, out, err = run("effect", "preview", "--dir", directory)
        self.assertEqual(code, 0, out + err)
        self.assertIn("copied", out)
        self.assertEqual((self.effect_dev / "soft-rain.mcfx").read_bytes(), (directory / "build" / "soft-rain.mcfx").read_bytes())
        self.assertIn("unchanged", run("effect", "preview", "--dir", directory)[1])
        source = directory / "soft-rain.hlsl"
        source.write_text(source.read_text(encoding="utf-8").replace("0.5 + 0.5 * sin", "0.4 + 0.6 * sin"), encoding="utf-8")
        self.assertIn("copied", run("effect", "preview", "--dir", directory)[1])
        self.assertEqual(sorted(p.name for p in self.effect_dev.iterdir()), ["soft-rain.mcfx"])

    def test_preview_watch_rebuilds(self):
        directory = self.effect()
        source = directory / "soft-rain.hlsl"

        def edit():
            time.sleep(0.4)
            source.write_text(source.read_text(encoding="utf-8").replace("0.5 + 0.5 * sin", "0.3 + 0.7 * sin"), encoding="utf-8")
            future = time.time() + 30
            os.utime(source, (future, future))

        thread = threading.Thread(target=edit)
        thread.start()
        code, out, err = run("effect", "preview", "--dir", directory, "--watch", "--rounds", 14)
        thread.join()
        self.assertEqual(code, 0, err)
        self.assertEqual(out.count("copied"), 2, out)

    def test_the_repositorys_samples_build(self):
        """Every sample compiles and its sidecar says what the comments say."""
        for sample in sorted((REPOSITORY / "samples" / "effects").iterdir()):
            if not sample.is_dir():
                continue
            directory = self.root / sample.name
            shutil.copytree(sample, directory, ignore=shutil.ignore_patterns("*.webp"))
            before = (directory / f"{sample.name}.effect").read_text()
            code, out, err = run("effect", "build", "--dir", directory, "--compiler", "d3dcompiler")
            self.assertEqual(code, 0, f"{sample.name}: {out}{err}")
            after = mcfx_tool.parse_sidecar((directory / f"{sample.name}.effect").read_text())
            expected = mcfx_tool.parse_sidecar(before)
            knobs = lambda text: [int(v) for v in text.split()]
            self.assertTrue(all(abs(a - b) <= 1 for a, b in zip(knobs(after.pop("knobs")), knobs(expected.pop("knobs")))),
                            f"{sample.name}: the knob defaults match (to the sidecar's rounding)")
            self.assertEqual(after, expected, f"{sample.name}: the generated sidecar matches the committed one")

    def test_header_errors(self):
        good = '// @effect name="X" license="MIT"\n// @blend normal\n'
        mcfx_tool.parse_header(good)
        for text, needle in (
                ('// @effect name="X" license="MIT"\n', "@blend"),
                ('// @blend normal\n', "@effect needs"),
                (good + "// @uses sparkles\n", "@uses"),
                (good + "// @knob a float 5..1 = 2 \"A\"\n", "min < max"),
                (good + "// @knob a int 0..2.5 = 1 \"A\"\n", "integers"),
                (good + "// @knob a toggle = maybe \"A\"\n", "on or = off"),
                (good + "// @knob a color = red \"A\"\n", "#RRGGBB"),
                (good + '// @knob a choice "x" = 0 "A"\n', "2 to 16"),
                (good + "// @knob a float 0..1 = 0.5\n", "label"),
                (good + '// @knob a float 0..1 = 0.5 "A"\n// @knob a float 0..1 = 0.5 "B"\n', "same id")):
            with self.assertRaises(core.ToolError, msg=text) as caught:
                mcfx_tool.parse_header(text)
            self.assertIn(needle, str(caught.exception))

    def test_percent_knobs(self):
        good = '// @effect name="X" license="MIT"\n// @blend normal\n'
        header = mcfx_tool.parse_header(good + '// @knob reach float 0.3..1 = 0.75 % "Reach"\n// @knob plain float 0..1 = 0.5 "Plain"\n')
        self.assertEqual(header["knobs"][0], {"id": "reach", "type": "float", "label": "Reach", "min": 0.3, "max": 1,
                                              "default": 0.75, "percent": True})
        self.assertNotIn("percent", header["knobs"][1])
        for text in ('// @knob a int 0..4 = 2 % "A"\n', '// @knob a toggle = on % "A"\n', '// @knob a color = #FFFFFF % "A"\n'):
            with self.assertRaises(core.ToolError, msg=text):
                mcfx_tool.parse_header(good + text)

    def test_knob_slots(self):
        knobs = [{"id": "a", "type": "float"}, {"id": "b", "type": "toggle"}, {"id": "c", "type": "color", "default": "#FF8000"},
                 {"id": "d", "type": "int", "default": 3}, {"id": "e", "type": "choice", "default": 1}]
        self.assertEqual(mcfx_tool.assign_slots(knobs), [(0, 0), (0, 1), (1, None), (2, 0), (2, 1)])
        self.assertEqual(mcfx_tool.define_text(knobs[2], (1, None)), "#define knob_c (mc_knobs[1].rgb)")
        self.assertEqual(mcfx_tool.define_text(knobs[1], (0, 1)), "#define knob_b (mc_knobs[0].y != 0)")
        self.assertEqual(mcfx_tool.define_text(knobs[3], (2, 0)), "#define knob_d ((int)mc_knobs[2].x)")
        too_many = [{"id": f"k{i}", "type": "color"} for i in range(9)]
        with self.assertRaises(core.ToolError):
            mcfx_tool.assign_slots(too_many)

    def test_lint(self):
        counts = mcfx_tool.lint("ps_5_0\n dcl_constantbuffer CB0[1], immediateIndexed\n dcl_sampler s1, mode_default\n"
                                " dcl_resource_texture2d (float,float,float,float) t3\n sample_l r0.xyzw, v0, t3.xyzw, s1, l(0)\n"
                                " mul r0.x, r0.x, r0.y\n ret\n")
        self.assertEqual(counts, {"instructions": 2, "samples": 1})
        for text, needle in (("dcl_constantbuffer CB1[4], immediateIndexed\n", "b0"),
                             ("dcl_sampler s3, mode_default\n", "s0-s2"),
                             ("dcl_resource_texture2d (float,float,float,float) t5\n", "t0-t4"),
                             ("dcl_uav_typed_texture2d (float,float,float,float) u0\n", "uav"),
                             ("dcl_resource_texture3d (float,float,float,float) t0\n", "3D"),
                             ("mov r0.x, r0.y\n" * (mcfx_tool.INSTRUCTIONS_MAX + 1), "instructions"),
                             ("sample r0.x, v0, t0.x, s0\n" * (mcfx_tool.SAMPLES_MAX + 1), "texture reads")):
            with self.assertRaises(core.ToolError, msg=text[:30]) as caught:
                mcfx_tool.lint(text)
            self.assertIn(needle, str(caught.exception))
        graph = "vs_5_0\n dcl_resource_texture2d (float,float,float,float) t10\n ld r0.xyzw, v0, t10.xyzw\n ret\n"
        self.assertEqual(mcfx_tool.lint(graph, 10), {"instructions": 1, "samples": 1}, "interface 3 binds t0-t10")
        with self.assertRaises(core.ToolError):
            mcfx_tool.lint(graph.replace("t10", "t11"), 10)
        self.assertIn("light", mcfx_tool.cost_hint({"instructions": 20, "samples": 1}))
        self.assertIn("heavy", mcfx_tool.cost_hint({"instructions": 3000, "samples": 1}))

    def test_container_limits(self):
        with self.assertRaises(core.ToolError):
            mcfx_tool.pack_mcfx({"DXBC": b"x"})
        with self.assertRaises(core.ToolError):
            mcfx_tool.pack_mcfx({"MANIFEST": b"{}", "DXBC": b"x" * mcfx_tool.PACKAGE_MAX})
        package = mcfx_tool.pack_mcfx({"SOURCE": b"s", "DXBC": b"DXBC", "MANIFEST": b"{}"})
        self.assertEqual(sorted(self.sections(package)), [1, 2, 5])

    def test_include_expansion(self):
        include = self.root / "inc"
        (include / "lib").mkdir(parents=True)
        (include / "lib" / "a.hlsli").write_text("float a() { return 1; }\n")
        expanded = mcfx_tool.expand_includes('#include <lib/a.hlsli>\n#include "lib/a.hlsli"\nfloat b() { return a(); }\n', include)
        self.assertEqual(expanded.count("float a()"), 1, "an include is expanded once")
        with self.assertRaises(core.ToolError):
            mcfx_tool.expand_includes("#include <lib/missing.hlsli>\n", include)

    def test_bytecode_check(self):
        with self.assertRaises(core.ToolError):
            mcfx_tool.check_bytecode(b"NOPE" + bytes(60))
        with self.assertRaises(core.ToolError):
            mcfx_tool.check_bytecode(b"DXBC" + bytes(mcfx_tool.DXBC_MAX))


if __name__ == "__main__":
    unittest.main(verbosity=1)
