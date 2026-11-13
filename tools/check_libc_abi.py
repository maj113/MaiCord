#!/usr/bin/env python3
"""Checks what a Linux MaiCord image assumes of the C library against the headers of the installed one: glibc
(libc6-dev) or musl (musl-dev). The image declares its libc imports itself (include/maicord/sys/linux/libc.hpp) and
nothing at build time proves those declarations match the library it will meet, so this compiles them:

  * the dlopen modes (kDlLazy, kDlNow, kDlLocal, kDlGlobal) against RTLD_LAZY, RTLD_NOW, RTLD_LOCAL, RTLD_GLOBAL, and
    RTLD_DEFAULT being a null handle (src/core/linux/start.cpp asks dlsym for the libc's own functions with it);
  * PthreadAttribute's size and alignment against pthread_attr_t, and pthread_t being an unsigned long;
  * AddrInfo's size and field offsets against struct addrinfo, and TimeSpec against struct timespec;
  * the clock numbers (kClock*) and the errno numbers (kError*, which also reach the image from libc return values:
    pthread_create, posix_spawn) against CLOCK_* and E*, plus EWOULDBLOCK == EAGAIN and ENOTSUP == EOPNOTSUPP;
  * the EAI_* numbers src/platform/net/linux/resolver.cpp maps (written there as `case -2: // EAI_NONAME`).

    check_libc_abi.py [--compiler CC]

Run on glibc (CI installs libc6-dev) and inside the musl containers of tools/test_linux_distros.sh, where the C library
is musl's. The compiler needs only to accept C11 (`cc`, gcc or clang).
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBC_HEADER = os.path.join(ROOT, "include", "maicord", "sys", "linux", "libc.hpp")
ABI_HEADER = os.path.join(ROOT, "include", "maicord", "sys", "linux", "abi.hpp")
RESOLVER = os.path.join(ROOT, "src", "platform", "net", "linux", "resolver.cpp")

# kError<Name> of abi.hpp -> the errno macro.
ERRNO = {
    "Permission": "EPERM", "NoEntry": "ENOENT", "Interrupted": "EINTR", "BadDescriptor": "EBADF", "Again": "EAGAIN",
    "NoMemory": "ENOMEM", "Access": "EACCES", "Exists": "EEXIST", "NotDirectory": "ENOTDIR", "IsDirectory": "EISDIR",
    "Invalid": "EINVAL", "TimedOut": "ETIMEDOUT", "Pipe": "EPIPE", "MessageSize": "EMSGSIZE",
    "AddressFamily": "EAFNOSUPPORT", "AddressInUse": "EADDRINUSE", "AddressNotAvailable": "EADDRNOTAVAIL",
    "NetworkUnreachable": "ENETUNREACH", "ConnectionAborted": "ECONNABORTED", "ConnectionReset": "ECONNRESET",
    "NotConnected": "ENOTCONN", "ConnectionRefused": "ECONNREFUSED", "HostUnreachable": "EHOSTUNREACH",
    "InProgress": "EINPROGRESS", "Canceled": "ECANCELED",
}
CLOCKS = {"Realtime": "CLOCK_REALTIME", "Monotonic": "CLOCK_MONOTONIC", "Boottime": "CLOCK_BOOTTIME"}
MODES = {"Lazy": "RTLD_LAZY", "Now": "RTLD_NOW", "Local": "RTLD_LOCAL", "Global": "RTLD_GLOBAL"}


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def integer(text):
    return int(text.strip().rstrip("uUlL"), 0)


def assertions():
    """(description, C constant expression) pairs, each of which must be nonzero."""
    libc, abi, resolver = read(LIBC_HEADER), read(ABI_HEADER), read(RESOLVER)
    checks = []
    for name, macro in MODES.items():
        found = re.search(r"kDl%s = ([^;]+);" % name, libc)
        if not found:
            raise SystemExit("check_libc_abi: libc.hpp has no kDl%s" % name)
        checks.append(("kDl%s == %s" % (name, macro), "%s == %d" % (macro, integer(found[1]))))
    checks.append(("RTLD_DEFAULT is a null handle", "(unsigned long)RTLD_DEFAULT == 0"))
    attribute = re.search(r"alignas\((\d+)\) PthreadAttribute \{\s*unsigned char bytes\[(\d+)\];", libc)
    if not attribute:
        raise SystemExit("check_libc_abi: libc.hpp has no PthreadAttribute")
    checks.append(("PthreadAttribute size", "sizeof(pthread_attr_t) == %s" % attribute[2]))
    checks.append(("PthreadAttribute alignment", "_Alignof(pthread_attr_t) == %s" % attribute[1]))
    checks.append(("pthread_t is an unsigned long", "sizeof(pthread_t) == 8 && (pthread_t)-1 > 0"))
    # The layouts abi.hpp pins with static_asserts.
    expected = re.search(r"sizeof\(AddrInfo\) == (\d+)", abi)
    offsets = dict(re.findall(r"__builtin_offsetof\(AddrInfo, (\w+)\) == (\d+)", abi))
    fields = {"address_length": "ai_addrlen", "address": "ai_addr", "canonical_name": "ai_canonname", "next": "ai_next"}
    if not expected or set(offsets) != set(fields):
        raise SystemExit("check_libc_abi: abi.hpp does not pin AddrInfo's size and every field offset")
    checks.append(("AddrInfo size", "sizeof(struct addrinfo) == %s" % expected[1]))
    for field, native in fields.items():
        checks.append(("AddrInfo.%s offset" % field, "offsetof(struct addrinfo, %s) == %s" % (native, offsets[field])))
    checks.append(("AddrInfo flags, family, type and protocol are the first four ints",
                   "offsetof(struct addrinfo, ai_flags) == 0 && offsetof(struct addrinfo, ai_family) == 4 && "
                   "offsetof(struct addrinfo, ai_socktype) == 8 && offsetof(struct addrinfo, ai_protocol) == 12"))
    checks.append(("TimeSpec is a struct timespec",
                   "sizeof(struct timespec) == 16 && offsetof(struct timespec, tv_sec) == 0 && "
                   "offsetof(struct timespec, tv_nsec) == 8"))
    for name, macro in CLOCKS.items():
        found = re.search(r"kClock%s = (\d+);" % name, abi)
        if not found:
            raise SystemExit("check_libc_abi: abi.hpp has no kClock%s" % name)
        checks.append(("kClock%s == %s" % (name, macro), "%s == %s" % (macro, found[1])))
    seen = set()
    for name, value in re.findall(r"kError(\w+) = (\d+);", abi):
        if name not in ERRNO:
            raise SystemExit("check_libc_abi: no errno macro is named for kError%s (add it to ERRNO)" % name)
        seen.add(name)
        checks.append(("kError%s == %s" % (name, ERRNO[name]), "%s == %s" % (ERRNO[name], value)))
    for name in sorted(set(ERRNO) - seen):
        raise SystemExit("check_libc_abi: abi.hpp has no kError%s, which this script names" % name)
    checks.append(("EWOULDBLOCK == EAGAIN", "EWOULDBLOCK == EAGAIN"))
    checks.append(("ENOTSUP == EOPNOTSUPP", "ENOTSUP == EOPNOTSUPP"))
    for value, name in re.findall(r"case (-\d+): // (EAI_\w+)", resolver):
        checks.append(("resolver.cpp's %s is %s" % (name, value), "%s == %s" % (name, value)))
    return checks


SOURCE = """#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stddef.h>
#include <time.h>
%s
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    arguments = parser.parse_args()
    checks = assertions()
    failed = []
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "check.c")
        # One translation unit per assertion would be slow; try them together and bisect only on failure.
        body = "".join('_Static_assert(%s, "%s");\n' % (expression, description) for description, expression in checks)
        with open(source, "w", encoding="utf-8") as handle:
            handle.write(SOURCE % body)
        result = subprocess.run([arguments.compiler, "-fsyntax-only", "-std=gnu11", source], capture_output=True, text=True)
        if result.returncode == 0:
            print("libc ABI check passed: %d assertions against the installed C library's headers" % len(checks))
            return 0
        for description, expression in checks:
            with open(source, "w", encoding="utf-8") as handle:
                handle.write(SOURCE % ('_Static_assert(%s, "%s");\n' % (expression, description)))
            single = subprocess.run([arguments.compiler, "-fsyntax-only", "-std=gnu11", source], capture_output=True,
                                    text=True)
            if single.returncode:
                failed.append((description, single.stderr.strip().splitlines()[0] if single.stderr else ""))
    if not failed:
        sys.stderr.write(result.stderr)
        return 1
    for description, message in failed:
        print("check_libc_abi: %s: %s" % (description, message), file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
