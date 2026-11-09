#!/usr/bin/env python3
"""Checks include/maicord/platform/capture/pipewire/abi.hpp against the installed PipeWire and SPA headers
(libpipewire-0.3-dev, libspa-0.2-dev): the analogue of tools/check_pulse_abi.py for Linux screen capture and stream audio.

Screen capture never links libpipewire: it declares the functions it resolves with dlsym, and the constants and struct
layouts it uses, itself. Nothing at build time proves those declarations match the library, so this script does:

  * every constant of abi.hpp whose comment names a SPA or PipeWire macro or enumerator (`// SPA_TYPE_Object`) is
    compiled against the real headers in a _Static_assert, so a value that differs fails;
  * the sizeof and offsetof numbers pinned by the static_asserts of abi.hpp are compiled against spa_hook, spa_pod,
    spa_chunk, spa_data, spa_meta, spa_buffer, spa_meta_header, pw_buffer, pw_stream_events, spa_dict, spa_dict_item,
    spa_interface, pw_core_methods, pw_registry_methods and pw_registry_events;
  * every function of the MAICORD_PIPEWIRE_FUNCTIONS table is found in the preprocessed headers, and its argument count
    and the kind (pointer, integer of a size, void) of its return type and every argument must be the declared one.

    check_pipewire_abi.py [--include DIR]... [--compiler CC]

With no --include the system headers are used (/usr/include, /usr/include/pipewire-0.3, /usr/include/spa-0.2).
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_openssl_abi as shared  # noqa: E402  (its declaration parsing is not OpenSSL specific)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "maicord", "platform", "capture", "pipewire", "abi.hpp")
INCLUDES = ("pipewire/pipewire.h", "spa/param/video/format-utils.h", "spa/param/audio/format-utils.h",
            "spa/param/buffers.h", "spa/buffer/meta.h")

shared.SCALARS.update({"i32": 4, "i64": 8, "u8": 1, "int32_t": 4, "int64_t": 8, "uint8_t": 1})

STRUCTS = {
    "Hook": "struct spa_hook", "Pod": "struct spa_pod", "Chunk": "struct spa_chunk", "Data": "struct spa_data",
    "Meta": "struct spa_meta", "SpaBuffer": "struct spa_buffer", "MetaHeader": "struct spa_meta_header",
    "Buffer": "struct pw_buffer", "StreamEvents": "struct pw_stream_events", "Dict": "struct spa_dict",
    "DictItem": "struct spa_dict_item", "Interface": "struct spa_interface", "RegistryEvents": "struct pw_registry_events",
    "CoreMethods": "struct pw_core_methods", "RegistryMethods": "struct pw_registry_methods",
    "NodeInfo": "struct pw_node_info", "NodeEvents": "struct pw_node_events", "NodeMethods": "struct pw_node_methods",
}


def kind(type_text):
    text = re.sub(r"\b(const|volatile|struct|enum)\b", " ", type_text)
    text = " ".join(text.split())
    # The enumerations (pw_direction, pw_stream_flags) are int-sized; the structs are only ever passed by pointer.
    if "*" not in text and not text.endswith("]") and re.fullmatch(r"(pw|spa)_\w+", text):
        return 4
    return shared.kind(text)


def read_header():
    text = open(HEADER, encoding="utf-8").read()
    rows = text[text.index("#define MAICORD_PIPEWIRE_FUNCTIONS"):text.index("struct Api")].replace("\\\n", " ")
    functions = [(match["name"], match["ret"].strip(),
                  [shared.parameter_type(p) for p in shared.split_arguments(match["args"])])
                 for match in re.finditer(r"F\((?P<ret>[^,]+),\s*(?P<name>\w+),\s*\((?P<args>.*?)\)\)", rows, re.S)]
    constants = [(match["macro"], match["value"].strip()) for match in re.finditer(
        r"inline constexpr (?P<type>[\w ]+?) k\w+ = (?P<value>[^;]+);\s*//\s*(?P<macro>(?:SPA|PW)_[A-Za-z0-9_]+)", text)]
    layouts = ["sizeof(%s) == %s" % (STRUCTS[match["type"]], match["size"])
               for match in re.finditer(r"sizeof\((?P<type>\w+)\) == (?P<size>\d+)", text)]
    layouts += ["offsetof(%s, %s) == %s" % (STRUCTS[match["type"]], match["field"], match["offset"])
                for match in re.finditer(r"__builtin_offsetof\((?P<type>\w+), (?P<field>\w+)\) == (?P<offset>\d+)", text)]
    return functions, constants, layouts


def main():
    compiler, includes = shared.command_line(
        __doc__, ["/usr/include", "/usr/include/pipewire-0.3", "/usr/include/spa-0.2"])
    functions, constants, layouts = read_header()
    if not functions or not constants or not layouts:
        raise SystemExit("found no table in " + HEADER)

    failures = shared.check_constants(compiler, includes, INCLUDES, constants, layouts)
    failures += shared.check_functions(shared.preprocess(compiler, includes, INCLUDES), functions, kind)
    return shared.summary("%d functions, %d constants and %d layouts" % (len(functions), len(constants), len(layouts)),
                          includes, failures)


if __name__ == "__main__":
    sys.exit(main())
