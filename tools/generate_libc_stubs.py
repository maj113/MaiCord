#!/usr/bin/env python3
"""Generates the stub library a Linux MaiCord image links against.

The image calls a handful of glibc functions (dlopen and friends, pthread_create and friends, clock_gettime;
include/maicord/sys/linux/libc.hpp). Linked against whatever glibc the build machine has, a reference would bind to the
newest version of the symbol that glibc defines and the image would refuse to start on an older one. This stub defines
the same symbols under the oldest version that libc.so.6 itself exports as the default: GLIBC_2.2.5, GLIBC_2.17 and
GLIBC_2.34. Since glibc 2.34 dlopen and the pthread functions live in libc.so.6 (libdl.so.2 and libpthread.so.0 are
empty), and every distribution the Linux floor (kernel 6.1) admits has at least that, so the image needs libc.so.6
alone and no glibc older than 2.34 runs it.

The stub is only linked against; the system's real libc.so.6 is what the loader maps at run time. musl needs none of
this: its loader answers a needed libc.* by itself and ignores the versions, which is why tools/retarget_libc.py makes
the musl image by changing only the program interpreter. Every function listed here must exist in musl too, or be
imported weakly (malloc_trim).

    generate_libc_stubs.py --compiler clang --output-directory build/libc-stubs
"""

import argparse
import os
import subprocess
import sys
import tempfile

# [(version node, exported functions)], oldest node first. Add a function here together with its declaration in
# include/maicord/sys/linux/libc.hpp. Each version is the default one glibc 2.34's libc.so.6 exports the symbol under on
# x86-64 (objdump -T): the dlopen and pthread functions moved into libc.so.6 with GLIBC_2.34, except pthread_attr_init
# and pthread_attr_destroy, which libc.so.6 exports as GLIBC_2.2.5.
SONAME = "libc.so.6"
NODES = [("GLIBC_2.2.5", ["malloc", "calloc", "free", "malloc_trim", "getaddrinfo", "freeaddrinfo",
                          "pthread_attr_init", "pthread_attr_destroy"]),
         ("GLIBC_2.17", ["clock_gettime"]),
         ("GLIBC_2.34", ["dlopen", "dlsym", "dlclose", "dlerror", "pthread_create", "pthread_join", "pthread_detach",
                         "pthread_attr_setstacksize"])]


def write_stub(compiler, directory, soname, nodes):
    functions = [name for _, names in nodes for name in names]
    source = "".join("void %s(void) {}\n" % name for name in functions)
    # One version node per introducing glibc version; each inherits the one before, as glibc's own nodes do.
    script = ""
    for index, (version, names) in enumerate(nodes):
        script += "%s {\n  global:\n%s" % (version, "".join("    %s;\n" % name for name in names))
        if index == len(nodes) - 1:
            script += "  local:\n    *;\n"
        script += "}%s;\n" % (" " + nodes[index - 1][0] if index else "")
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
    write_stub(arguments.compiler, arguments.output_directory, SONAME, NODES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
