#!/usr/bin/env python3
"""maicord-plugin: write, build, load, check and package MaiCord plugins and background effects.

    maicord-plugin new <name>         a plugin project: plugin.cpp, README.md, .gitignore, .clangd
    maicord-plugin build              compile and link it as the catalog does, then check the DLL (or .so)
    maicord-plugin dev [--watch]      build if needed, then copy it into Developer Mode's dev folder
    maicord-plugin check [file]       validate a DLL's PE or a shared object's ELF headers and descriptor offline
    maicord-plugin pack               check, then write the catalog submission folder (plugins/<slug>/)
    maicord-plugin effect new|build|preview    background effects

build, dev, check and pack take --target windows|linux (default: the system the tool runs on): a Windows DLL or a
Linux shared object, from the same plugin.cpp.

Python 3.9 or newer, standard library only. See include/maicord/sdk/README.md for the guide.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import mcfx_tool  # noqa: E402
import plugin_tool as core  # noqa: E402
from plugin_tool import CATEGORIES, ToolError  # noqa: E402


def parser() -> argparse.ArgumentParser:
    top = argparse.ArgumentParser(prog="maicord-plugin", description=__doc__.split("\n\n")[0])
    commands = top.add_subparsers(dest="command", required=True)

    def common(p, project=True):
        p.add_argument("--sdk", help="a folder with include\\maicord\\sdk (default: this repository or SDK folder)")
        if project:
            p.add_argument("--dir", help="the project folder (default: the current folder)")
            p.add_argument("--source", help="the source file (default: plugin.cpp, or <slug>.hlsl for effects)")

    def target(p):
        p.add_argument("--target", choices=core.TARGETS, help="windows (a DLL) or linux (a shared object); "
                       "default: the system the tool runs on")

    def compiler(p):
        p.add_argument("--llvm-bin", help="a folder with clang-cl.exe and lld-link.exe (Windows) or clang and ld.lld "
                                          "(Linux) (default: MAICORD_LLVM_BIN, the pin, PATH)")
        p.add_argument("--no-fetch", action="store_true", help="never download the pinned toolchain")
        p.add_argument("-v", "--verbose", action="store_true", help="print the compiler and linker commands")

    p = commands.add_parser("new", help="create a plugin project")
    p.add_argument("name", help="the plugin's name, e.g. hello or \"My Plugin\"")
    p.add_argument("--dir", help="where to create it (default: ./<slug>)")
    p.add_argument("--summary", help="the one-line summary")
    common(p, False)
    p.set_defaults(run=core.command_new)

    p = commands.add_parser("build", help="compile build/<slug>.dll or .so and check it")
    common(p)
    compiler(p)
    target(p)
    p.set_defaults(run=core.command_build)

    p = commands.add_parser("dev", help="copy the DLL or shared object into the Developer Mode folder (building it if needed)")
    common(p)
    compiler(p)
    target(p)
    p.add_argument("--dev-folder", help="default: MAICORD_PLUGIN_DEV or %%LOCALAPPDATA%%\\MaiCord\\plugins\\dev "
                                        "($XDG_DATA_HOME/maicord/plugins/dev on Linux)")
    p.add_argument("--watch", action="store_true", help="rebuild and copy whenever a source changes")
    p.add_argument("--rounds", type=int, help=argparse.SUPPRESS)
    p.set_defaults(run=core.command_dev)

    p = commands.add_parser("check", help="validate a DLL's or shared object's headers and descriptor offline")
    p.add_argument("dll", nargs="?", help="a DLL or .so (default: the project's build/<slug>.dll or .so for --target)")
    common(p)
    target(p)
    p.set_defaults(run=core.command_check)

    p = commands.add_parser("pack", help="write the catalog submission folder for a pull request")
    common(p)
    target(p)
    p.add_argument("--author", help="the name the catalog credits (default: .author of MC_PLUGIN)")
    p.add_argument("--github", help="the author's GitHub user")
    p.add_argument("--license", help="an SPDX id that allows redistribution and modification")
    p.add_argument("--category", action="append", help=f"up to 3 of {', '.join(CATEGORIES)} (default: utility)")
    p.add_argument("--output", help="default: dist/")
    p.add_argument("--force", action="store_true", help="replace an existing dist/<slug>")
    p.set_defaults(run=core.command_pack)

    p = commands.add_parser("effect", help="background effects: new, build, preview")
    mcfx_tool.add_commands(p, common)
    return top


def main(argv=None) -> int:
    arguments = parser().parse_args(argv)
    try:
        return arguments.run(arguments)
    except ToolError as error:
        print(f"maicord-plugin: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
