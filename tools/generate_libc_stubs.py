#!/usr/bin/env python3
"""Generates the stub libraries a Linux MaiCord image links against.

The image calls a handful of glibc functions (dlopen and friends, pthread_create and friends, clock_gettime;
include/maicord/sys/linux/libc.hpp). Linked against a real glibc, each reference would bind to the newest version of
the symbol that glibc defines (pthread_create@GLIBC_2.34, ...) and the image would refuse to start on anything
older. These stubs define the same symbols under the oldest versions that exist, so the linker records
GLIBC_2.2.5 and GLIBC_2.17 and nothing newer, and name the libraries as glibc does: before 2.34 the functions live
in libdl.so.2, libpthread.so.0 and libc.so.6; after it libdl and libpthread are empty libraries that still carry
the old version nodes, so the same image runs on both.

The stubs are only linked against; the system's real libraries are what the loader maps at run time.

    generate_libc_stubs.py --compiler clang --output-directory build/libc-stubs
"""

import argparse
import os
import subprocess
import sys
import tempfile

# soname -> (version node, exported functions). Add a function here together with its declaration in
# include/maicord/sys/linux/libc.hpp. The versions are those glibc introduced the symbols with on x86-64.
STUBS = {
    "libc.so.6": ("GLIBC_2.17", ["clock_gettime"]),
    "libdl.so.2": ("GLIBC_2.2.5", ["dlopen", "dlsym", "dlclose", "dlerror"]),
    "libpthread.so.0": ("GLIBC_2.2.5", ["pthread_create", "pthread_join", "pthread_detach"]),
}


def write_stub(compiler, directory, soname, version, functions):
    source = "".join("void %s(void) {}\n" % name for name in functions)
    script = "%s {\n  global:\n%s  local:\n    *;\n};\n" % (version, "".join("    %s;\n" % name for name in functions))
    with tempfile.TemporaryDirectory() as scratch:
        source_path = os.path.join(scratch, "stub.c")
        script_path = os.path.join(scratch, "stub.map")
        with open(source_path, "w", newline="\n") as handle:
            handle.write(source)
        with open(script_path, "w", newline="\n") as handle:
            handle.write(script)
        output = os.path.join(directory, soname)
        command = [
            compiler, "-shared", "-fPIC", "-O0", "-nostdlib", "-nostdinc", "-fuse-ld=lld",
            "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
            "-Wl,-soname," + soname, "-Wl,--version-script=" + script_path, "-Wl,--build-id=none",
            "-Wl,-z,noexecstack", "-o", output, source_path,
        ]
        subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compiler", required=True, help="clang, the C compiler driver (it links with lld)")
    parser.add_argument("--output-directory", required=True)
    arguments = parser.parse_args()
    os.makedirs(arguments.output_directory, exist_ok=True)
    for soname, (version, functions) in STUBS.items():
        write_stub(arguments.compiler, arguments.output_directory, soname, version, functions)
    return 0


if __name__ == "__main__":
    sys.exit(main())
