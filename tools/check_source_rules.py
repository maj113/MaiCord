#!/usr/bin/env python3
"""Runs the source rules that need no build, in one process, and records what they read.

    check_source_rules.py [--stamp FILE]

  * tools/check_shader_twins.py: every packed HLSL shader has its GLSL twin, written against the HLSL as it is;
  * tools/check_cbuffer_layout.py and its --self-test: the cbuffers agree between HLSL packing and std140;
  * tools/check_theme_literals.py: no new presentation literal in src/ui;
  * tools/generate_theme_example.py --check: assets/themes/example.maicord-theme.json matches the token tables.

The build runs it with --stamp: on success it touches FILE and writes FILE.d, a depfile that lists every file and
directory the rules read, so Ninja runs it again only when one of them changes (an added or removed file changes its
directory). Each rule's own script keeps its options (--stamp, --update, --list).

Exit status: nonzero when a rule fails; every rule runs.
"""

import argparse
import contextlib
import io
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import check_cbuffer_layout  # noqa: E402
import check_shader_twins  # noqa: E402
import check_theme_literals  # noqa: E402
import generate_theme_example  # noqa: E402

RULES = (
    ("shader twins", check_shader_twins.main, []),
    ("cbuffer layout", check_cbuffer_layout.main, []),
    ("cbuffer layout self-test", check_cbuffer_layout.main, ["--self-test"]),
    ("theme literals", check_theme_literals.main, []),
    ("theme example", generate_theme_example.main, ["--check"]),
)


def inputs():
    """Every file and directory the rules read."""
    shaders = ROOT / "src" / "render" / "shaders"
    theme = ROOT / "include" / "maicord" / "ui" / "theme"
    paths = [pathlib.Path(module.__file__) for module in (
        check_cbuffer_layout, check_shader_twins, check_theme_literals, generate_theme_example)]
    paths += [pathlib.Path(__file__), ROOT / "cmake" / "generated_sources.cmake",
              ROOT / "include" / "maicord" / "sdk" / "background.hlsli", check_theme_literals.BASELINE,
              generate_theme_example.OUTPUT, shaders, shaders / "gles"]
    paths += sorted(shaders.glob("*.hlsl")) + sorted((shaders / "gles").glob("*.glsl"))
    paths += [theme / name for name in ("theme.hpp", "presets.hpp", "tokens.inc", "styles.hpp")]
    sources = check_theme_literals.SOURCES
    paths += [sources] + sorted(path for path in sources.rglob("*")
                                if path.is_dir() or path.suffix in (".cpp", ".hpp", ".inc"))
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--stamp", type=pathlib.Path, help="touch this file and write its depfile (FILE.d) on success")
    arguments = parser.parse_args()
    failed = []
    for name, rule, argv in RULES:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = rule(argv)
        if status:
            print("%s:\n%s" % (name, output.getvalue().rstrip()))
            failed.append(name)
    if failed:
        print("check_source_rules: failed: %s" % ", ".join(failed), file=sys.stderr)
        return 1
    if arguments.stamp:
        escaped = (path.as_posix().replace(" ", "\\ ") for path in inputs())
        depfile = arguments.stamp.with_name(arguments.stamp.name + ".d")
        depfile.write_text("%s: %s\n" % (arguments.stamp.as_posix().replace(" ", "\\ "), " \\\n  ".join(escaped)),
                           encoding="utf-8", newline="\n")
        arguments.stamp.write_bytes(b"")
    return 0


if __name__ == "__main__":
    sys.exit(main())
