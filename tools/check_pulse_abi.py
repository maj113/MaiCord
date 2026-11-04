#!/usr/bin/env python3
"""Checks include/maicord/platform/audio/pulse/abi.hpp against the installed libpulse headers (libpulse-dev): the
analogue of tools/check_openssl_abi.py for the audio backend.

The audio backend never links libpulse: it declares the functions it resolves with dlsym, and the constants and
struct layouts it uses, itself. Nothing at build time proves those declarations match the library, so this script
does:

  * every constant of abi.hpp whose comment names a libpulse macro or enumerator (`// PA_STREAM_READY`) is compiled
    against the real headers in a _Static_assert, so a value that differs fails;
  * the sizeof and offsetof numbers pinned by the static_asserts of abi.hpp are compiled against pa_sample_spec,
    pa_buffer_attr, pa_channel_map, pa_cvolume, pa_sink_info, pa_source_info and pa_server_info;
  * every function of the MAICORD_PULSE_FUNCTIONS table is found in the preprocessed headers, and its argument count
    and the kind (pointer, integer of a size, double, void) of its return type and every argument must be the
    declared one. The opaque structs and the callback typedefs are pointers in both.

    check_pulse_abi.py [--include DIR]... [--compiler CC]

With no --include the system headers (/usr/include) are used; CI installs libpulse-dev and runs this beside the build.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_openssl_abi as shared  # noqa: E402  (its declaration parsing is not OpenSSL specific)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "maicord", "platform", "audio", "pulse", "abi.hpp")
INCLUDES = ("pulse/pulseaudio.h",)

# Scalar names either side uses, beyond the ones check_openssl_abi knows.
shared.SCALARS.update({
    "i32": 4, "i64": 8, "u8": 1, "int64_t": 8, "uint8_t": 1, "double": "double",
    "pa_volume_t": 4, "pa_usec_t": 8,
})

STRUCTS = {
    "SampleSpec": "pa_sample_spec", "BufferAttr": "pa_buffer_attr", "ChannelMap": "pa_channel_map",
    "CVolume": "pa_cvolume", "SinkInfo": "pa_sink_info", "SourceInfo": "pa_source_info",
    "ServerInfo": "pa_server_info",
}


def kind(type_text):
    text = re.sub(r"\b(const|volatile|struct|enum)\b", " ", type_text)
    text = " ".join(text.split())
    # An opaque struct (pa_context*), a callback typedef (pa_context_notify_cb_t, ContextNotifyCb) or a pointer.
    if "*" in text or re.fullmatch(r"\w+_cb_t|\w+Cb", text):
        return "pointer"
    # The enumerations (pa_context_state_t, pa_stream_flags_t, pa_seek_mode_t, ...) are int-sized.
    if re.fullmatch(r"pa_\w+_t", text) and text not in shared.SCALARS:
        return 4
    return shared.kind(text)


def read_header():
    text = open(HEADER, encoding="utf-8").read()
    table = text[text.index("#define MAICORD_PULSE_FUNCTIONS"):text.index("struct Api")]
    functions = []
    for match in re.finditer(r"^\s*F\((?P<ret>[^,]+),\s*(?P<name>\w+),\s*\((?P<args>.*?)\)\)\s*\\?\s*$", table, re.M):
        functions.append((match["name"], match["ret"].strip(), shared.split_arguments(match["args"])))
    constants = []
    for match in re.finditer(
            r"inline constexpr (?P<type>[\w ]+?) k\w+ = (?P<value>[^;]+);\s*//\s*(?P<macro>PA_[A-Z0-9_]+)", text):
        constants.append((match["macro"], match["value"].strip()))
    layouts = []
    for match in re.finditer(r"sizeof\((?P<type>\w+)\) == (?P<size>\d+)", text):
        layouts.append("sizeof(%s) == %s" % (STRUCTS[match["type"]], match["size"]))
    for match in re.finditer(r"__builtin_offsetof\((?P<type>\w+), (?P<field>\w+)\) == (?P<offset>\d+)", text):
        layouts.append("offsetof(%s, %s) == %s" % (STRUCTS[match["type"]], match["field"], match["offset"]))
    return functions, constants, layouts


def run(command, source):
    result = subprocess.run(command + [source], capture_output=True, text=True)
    return result


def preprocess(compiler, includes):
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "check.c")
        with open(source, "w", encoding="utf-8") as handle:
            for header in INCLUDES:
                handle.write("#include <%s>\n" % header)
        result = run([compiler, "-E", "-P"] + ["-I" + d for d in includes], source)
        if result.returncode:
            raise SystemExit("preprocessing failed (is libpulse-dev installed?):\n" + result.stderr)
        return result.stdout


def check_compiled(compiler, includes, constants, layouts):
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "constants.c")
        with open(source, "w", encoding="utf-8") as handle:
            handle.write("#include <stddef.h>\n#include <stdint.h>\n")
            for header in INCLUDES:
                handle.write("#include <%s>\n" % header)
            for macro, value in constants:
                handle.write("_Static_assert((unsigned long long)(%s) == (unsigned long long)(%s), \"%s\");\n"
                             % (macro, value, macro))
            for layout in layouts:
                handle.write("_Static_assert(%s, \"%s\");\n" % (layout, layout))
        result = run([compiler, "-fsyntax-only"] + ["-I" + d for d in includes], source)
        if result.returncode:
            print(result.stderr)
            return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    arguments = parser.parse_args()
    includes = arguments.include or ["/usr/include"]
    functions, constants, layouts = read_header()
    if not functions or not constants or not layouts:
        raise SystemExit("found no table in " + HEADER)

    failures = check_compiled(arguments.compiler, includes, constants, layouts)
    text = preprocess(arguments.compiler, includes)
    for name, ret, table_arguments in functions:
        declared = shared.find_declaration(text, name)
        if not declared:
            print("%s: not declared by the libpulse headers" % name)
            failures += 1
            continue
        declared_ret, declared_arguments = declared
        problems = []
        try:
            if len(declared_arguments) != len(table_arguments):
                problems.append("%d arguments declared, %d in the table" % (len(declared_arguments), len(table_arguments)))
            elif kind(declared_ret) != kind(ret):
                problems.append("returns %s, the table says %s" % (declared_ret, ret))
            else:
                for index, (theirs, ours) in enumerate(zip(declared_arguments, table_arguments)):
                    if kind(theirs) != kind(ours):
                        problems.append("argument %d is '%s', the table says '%s'" % (index + 1, theirs, ours))
        except ValueError as error:
            problems = [str(error)]
        if problems:
            failures += 1
            print("%s: %s" % (name, "; ".join(problems)))
    print("%d functions, %d constants and %d layouts checked against %s: %s" % (
        len(functions), len(constants), len(layouts), ", ".join(includes), "FAILED" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
