#!/usr/bin/env python3
"""Checks src/platform/crypto/openssl/openssl.hpp against the installed OpenSSL headers (libssl-dev): the Linux
analogue of the sys-header conformance unit of the Windows build.

The OpenSSL backend never links libssl or libcrypto: it declares the functions it resolves with dlsym and the
constants it uses itself. Nothing at build time proves those declarations match the library, so this script does:

  * every constant of openssl.hpp whose comment names an OpenSSL macro (`// SSL_ERROR_SSL`) is compiled against the
    real headers in a _Static_assert, so a value that differs from the macro fails;
  * every function of the two X-macro tables is found in the preprocessed headers, and its argument count and the
    kind (pointer, 4-byte or 8-byte integer, void) of its return type and every argument must be the declared one.
    The opaque structs are void* in the table, which is the same kind as any pointer.

    check_openssl_abi.py [--include DIR]... [--compiler CC]

With no --include the system headers (/usr/include and /usr/include/x86_64-linux-gnu) are used; CI installs
libssl-dev and runs this beside the build. A header set of OpenSSL 3.0 or newer is required.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "src", "platform", "crypto", "openssl", "openssl.hpp")
INCLUDES = ("openssl/crypto.h", "openssl/err.h", "openssl/evp.h", "openssl/ssl.h", "openssl/bio.h", "openssl/bn.h",
            "openssl/x509.h", "openssl/x509v3.h", "openssl/x509_vfy.h", "openssl/rsa.h", "openssl/obj_mac.h",
            "openssl/core.h", "openssl/core_names.h", "openssl/safestack.h")

# Sizes in bytes of the scalar type names either side uses.
SCALARS = {
    "int": 4, "unsigned int": 4, "unsigned": 4, "uint32_t": 4, "u32": 4,
    "long": 8, "unsigned long": 8, "size_t": 8, "uint64_t": 8, "u64": 8, "usize": 8, "ossl_ssize_t": 8,
    "long long": 8, "unsigned long long": 8,
}


def kind(type_text):
    """'void', 'pointer' or the size in bytes of a C type as written in a declaration or in the table."""
    text = re.sub(r"\b(const|volatile|struct|enum|OSSL_DEPRECATEDIN_[0-9_]+\w*)\b", " ", type_text)
    text = " ".join(text.split())
    # A callback typedef (SSL_verify_cb) is a function pointer.
    if "*" in text or text.endswith("]") or re.fullmatch(r"\w+_cb", text):
        return "pointer"
    if text == "void":
        return "void"
    if text in SCALARS:
        return SCALARS[text]
    # An unknown scalar or typedef: stated, never guessed.
    raise ValueError("cannot classify the type '%s'" % type_text)


def split_arguments(text):
    """Top-level comma separated parameters of a parameter list, without the parentheses."""
    parts, depth, current = [], 0, ""
    for character in text:
        if character in "([":
            depth += 1
        elif character in ")]":
            depth -= 1
        if character == "," and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += character
    if current.strip():
        parts.append(current.strip())
    return [] if parts == ["void"] else parts


def parameter_type(parameter):
    """A parameter without its name: 'const unsigned char *tbs' becomes 'const unsigned char *'."""
    parameter = parameter.strip()
    if parameter.endswith("]"):  # OSSL_PARAM params[]
        return re.sub(r"\s*\w+\s*\[\s*\]$", " *", parameter)
    match = re.match(r"^(.*?[\s\*])(\w+)$", parameter)
    if match and not re.match(r"^(void|int|long|unsigned|char|size_t|short)$", parameter):
        return match.group(1)
    return parameter


def read_table():
    text = open(HEADER, encoding="utf-8").read()
    functions = []
    for match in re.finditer(r"^\s*F\((?P<ret>[^,]+),\s*(?P<name>\w+),\s*\((?P<args>[^)]*)\)\)", text, re.M):
        functions.append((match["name"], match["ret"].strip(), split_arguments(match["args"])))
    constants = []
    for match in re.finditer(
            r"inline constexpr (?P<type>[\w ]+?) k\w+ = (?P<value>[^;]+);\s*//\s*(?P<macro>[A-Z][A-Z0-9_]+)\b", text):
        constants.append((match["macro"], match["value"].strip(), match["type"]))
    return functions, constants


def preprocess(compiler, includes):
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "check.c")
        with open(source, "w", encoding="utf-8") as handle:
            for header in INCLUDES:
                handle.write("#include <%s>\n" % header)
        command = [compiler, "-E", "-P", "-DOPENSSL_API_COMPAT=0x30000000L"] + ["-I" + d for d in includes] + [source]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise SystemExit("preprocessing failed (is libssl-dev installed?):\n" + result.stderr)
        return result.stdout


def find_declaration(text, name):
    """The (return type, [parameter types]) of the first declaration of `name`, or None."""
    for match in re.finditer(r"(?P<ret>[^;{}()]*?)\b%s\s*\(" % re.escape(name), text):
        start = match.end()
        depth, index = 1, start
        while index < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[index], 0)
            index += 1
        if depth:
            continue
        following = text[index:index + 64].lstrip()
        if not (following.startswith(";") or following.startswith("__attribute__") or following.startswith("OSSL_")):
            continue
        ret = match["ret"].strip().split(";")[-1].strip()
        ret = re.sub(r"\b(extern|static|inline|__inline__|OSSL_DEPRECATEDIN_[0-9_]+\w*)\b", " ", ret).strip()
        return ret, [parameter_type(p) for p in split_arguments(text[start:index - 1])]
    return None


def check_constants(compiler, includes, constants):
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "constants.c")
        with open(source, "w", encoding="utf-8") as handle:
            handle.write("#include <stdint.h>\n")
            for header in INCLUDES:
                handle.write("#include <%s>\n" % header)
            for macro, value, _ in constants:
                value = re.sub(r"(?<=[0-9a-fA-F])(ull|ULL)\b", "ULL", value)
                handle.write("_Static_assert((unsigned long long)(%s) == (unsigned long long)(%s), \"%s\");\n"
                             % (macro, value, macro))
        command = [compiler, "-fsyntax-only", "-DOPENSSL_API_COMPAT=0x30000000L"] + ["-I" + d for d in includes] + [source]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            print(result.stderr)
            return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    arguments = parser.parse_args()
    includes = arguments.include or ["/usr/include", "/usr/include/x86_64-linux-gnu"]
    functions, constants = read_table()
    if not functions or not constants:
        raise SystemExit("found no table in " + HEADER)

    failures = check_constants(arguments.compiler, includes, constants)
    text = preprocess(arguments.compiler, includes)
    for name, ret, table_arguments in functions:
        declared = find_declaration(text, name)
        if not declared:
            print("%s: not declared by the OpenSSL headers" % name)
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
