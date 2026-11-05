#!/usr/bin/env python3
"""Checks include/maicord/platform/desktop/fido2/abi.hpp against the installed libfido2 headers (libfido2-dev): the
analogue of tools/check_speechd_abi.py for the security-key backend.

The backend never links libfido2: it declares the functions it resolves with dlsym, and the constants it uses,
itself. Nothing at build time proves those declarations match the library, so this script does:

  * every constant of abi.hpp whose comment names a libfido2 enumerator (`// FIDO_ERR_PIN_INVALID`) is compiled against the real
    header in a _Static_assert, so a value that differs fails;
  * every function of the MAICORD_FIDO2_FUNCTIONS table is found in the preprocessed header, and its argument count
    and the kind (pointer, integer of a size, void) of its return type and every argument must be the declared one.
    The opaque structs are pointers in both, and the enumerations are int-sized.

    check_fido2_abi.py [--include DIR]... [--compiler CC]

With no --include the system headers (/usr/include) are used; CI installs libfido2-dev and runs this beside the build.
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
HEADER = os.path.join(ROOT, "include", "maicord", "platform", "desktop", "fido2", "abi.hpp")
INCLUDES = ("fido.h",)

shared.SCALARS.update({"i32": 4, "u8": 1, "signed int": 4, "signed": 4, "bool": 1, "_Bool": 1, "uint8_t": 1})


def kind(type_text):
    text = re.sub(r"\b(const|volatile|struct|enum)\b", " ", type_text)
    text = " ".join(text.split())
    # fido_opt_t is an int-sized enumeration; the structs are only ever passed by pointer.
    if "*" not in text and re.fullmatch(r"fido_\w+_t", text):
        return 4
    return shared.kind(text)


def read_header():
    text = open(HEADER, encoding="utf-8").read()
    table = text[text.index("#define MAICORD_FIDO2_FUNCTIONS"):text.index("struct Api")]
    table = table.replace("\\\n", " ")
    functions = []
    for match in re.finditer(r"F\((?P<ret>[^,]+),\s*(?P<name>\w+),\s*\((?P<args>.*?)\)\)", table, re.S):
        functions.append((match["name"], match["ret"].strip(),
                          [shared.parameter_type(p) for p in shared.split_arguments(match["args"])]))
    constants = []
    for match in re.finditer(
            r"inline constexpr (?P<type>[\w ]+?) k\w+ = (?P<value>[^;]+);\s*//\s*(?P<macro>FIDO_[A-Z0-9_]+)", text):
        constants.append((match["macro"], match["value"].strip()))
    return functions, constants


def run(command, source):
    return subprocess.run(command + [source], capture_output=True, text=True)


def preprocess(compiler, includes):
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "check.c")
        with open(source, "w", encoding="utf-8") as handle:
            for header in INCLUDES:
                handle.write("#include <%s>\n" % header)
        result = run([compiler, "-E", "-P"] + ["-I" + d for d in includes], source)
        if result.returncode:
            raise SystemExit("preprocessing failed (is libfido2-dev installed?):\n" + result.stderr)
        return result.stdout


def check_compiled(compiler, includes, constants):
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "constants.c")
        with open(source, "w", encoding="utf-8") as handle:
            handle.write("#include <stddef.h>\n#include <stdint.h>\n")
            for header in INCLUDES:
                handle.write("#include <%s>\n" % header)
            for macro, value in constants:
                handle.write("_Static_assert((unsigned long long)(%s) == (unsigned long long)(%s), \"%s\");\n"
                             % (macro, value, macro))
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
    functions, constants = read_header()
    if not functions or not constants:
        raise SystemExit("found no table in " + HEADER)

    failures = check_compiled(arguments.compiler, includes, constants)
    text = preprocess(arguments.compiler, includes)
    for name, ret, table_arguments in functions:
        declared = shared.find_declaration(text, name)
        if not declared:
            print("%s: not declared by the libfido2 headers" % name)
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
    print("%d functions and %d constants checked against %s: %s" % (
        len(functions), len(constants), ", ".join(includes), "FAILED" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
