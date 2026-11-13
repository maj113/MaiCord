#!/usr/bin/env python3
"""Checks include/maicord/platform/desktop/fido2/abi.hpp against the installed libfido2 headers (libfido2-dev): the
analogue of tools/check_speechd_abi.py for the security-key backend.

The backend never links libfido2: it declares the functions it resolves with dlsym, and the constants it uses,
itself. Nothing at build time proves those declarations match the library, so this script does:

  * every constant of abi.hpp whose comment names a libfido2 enumerator or COSE algorithm (`// FIDO_ERR_PIN_INVALID`,
    `// COSE_ES256`) is compiled against the real
    header in a _Static_assert, so a value that differs fails;
  * every function of the MAICORD_FIDO2_FUNCTIONS table is found in the preprocessed header, and its argument count
    and the kind (pointer, integer of a size, void) of its return type and every argument must be the declared one.
    The opaque structs are pointers in both, and the enumerations are int-sized.

    check_fido2_abi.py [--include DIR]... [--compiler CC]

With no --include the system headers (/usr/include) are used; CI installs libfido2-dev and runs this beside the build.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_openssl_abi as shared  # noqa: E402  (its declaration parsing is not OpenSSL specific)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "maicord", "platform", "desktop", "fido2", "abi.hpp")
INCLUDES = ("fido.h",)

shared.SCALARS.update({"i32": 4, "u8": 1, "signed int": 4, "signed": 4, "bool": 1, "_Bool": 1, "uint8_t": 1})


def kind(type_text):
    text = shared.plain(type_text)
    # fido_opt_t is an int-sized enumeration; the structs are only ever passed by pointer.
    if "*" not in text and re.fullmatch(r"fido_\w+_t", text):
        return 4
    return shared.kind(text)


if __name__ == "__main__":
    sys.exit(shared.check_library(__doc__, HEADER, "MAICORD_FIDO2_FUNCTIONS", "(?:FIDO_|COSE_)", INCLUDES, kind))
