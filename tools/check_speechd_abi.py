#!/usr/bin/env python3
"""Checks include/maicord/platform/audio/speechd/abi.hpp against the installed libspeechd headers (libspeechd-dev): the
analogue of tools/check_pulse_abi.py for the text-to-speech backend.

The backend never links libspeechd: it declares the functions it resolves with dlsym, and the constants it uses,
itself. Nothing at build time proves those declarations match the library, so this script does:

  * every constant of abi.hpp whose comment names a libspeechd enumerator (`// SPD_TEXT`) is compiled against the real
    header in a _Static_assert, so a value that differs fails;
  * every function of the MAICORD_SPEECHD_FUNCTIONS table is found in the preprocessed header, and its argument count
    and the kind (pointer, integer of a size, void) of its return type and every argument must be the declared one.
    The opaque structs are pointers in both, and the enumerations are int-sized.

    check_speechd_abi.py [--include DIR]... [--compiler CC]

With no --include the system headers (/usr/include) are used; CI installs libspeechd-dev and runs this beside the build.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_openssl_abi as shared  # noqa: E402  (its declaration parsing is not OpenSSL specific)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "maicord", "platform", "audio", "speechd", "abi.hpp")
INCLUDES = ("speech-dispatcher/libspeechd.h",)

shared.SCALARS.update({"i32": 4, "signed int": 4, "signed": 4})


def kind(type_text):
    text = re.sub(r"\b(const|volatile|struct|enum)\b", " ", type_text)
    text = " ".join(text.split())
    # The enumerations (SPDConnectionMode, SPDPriority) are int-sized; the structs are only ever passed by pointer.
    if "*" not in text and re.fullmatch(r"SPD\w+", text):
        return 4
    return shared.kind(text)


def main():
    compiler, includes = shared.command_line(__doc__, ["/usr/include"])
    functions, constants = shared.read_dlsym_table(HEADER, "MAICORD_SPEECHD_FUNCTIONS", "SPD_")
    if not functions or not constants:
        raise SystemExit("found no table in " + HEADER)

    failures = shared.check_constants(compiler, includes, INCLUDES, constants)
    failures += shared.check_functions(shared.preprocess(compiler, includes, INCLUDES), functions, kind)
    return shared.summary("%d functions and %d constants" % (len(functions), len(constants)), includes, failures)


if __name__ == "__main__":
    sys.exit(main())
