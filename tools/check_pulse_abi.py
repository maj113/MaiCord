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

import os
import re
import sys

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


def main():
    compiler, includes = shared.command_line(__doc__, ["/usr/include"])
    functions, constants, layouts = read_header()
    if not functions or not constants or not layouts:
        raise SystemExit("found no table in " + HEADER)

    failures = shared.check_constants(compiler, includes, INCLUDES, constants, layouts)
    failures += shared.check_functions(shared.preprocess(compiler, includes, INCLUDES), functions, kind)
    return shared.summary("%d functions, %d constants and %d layouts" % (len(functions), len(constants), len(layouts)),
                          includes, failures)


if __name__ == "__main__":
    sys.exit(main())
