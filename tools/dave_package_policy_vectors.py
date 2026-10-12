"""OpenSSL-signed structural KeyPackages for DAVE receiver policy tests.

Each package has valid signatures, even when its DAVE profile is unacceptable.
Expected policy outcomes follow RFC 9420 7.2/13.4 and DAVE Key Packages, not the
native implementation. Test keys are public, fixed fixtures.
"""
from pathlib import Path
import hashlib

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from mls_objects_oracle import framed, point, vector, text


def u16(values):
    return b"".join(x.to_bytes(2, "big") for x in values)


def main():
    signing = ec.derive_private_key(123, ec.SECP256R1())
    encryption = point(ec.derive_private_key(456, ec.SECP256R1()))
    init = point(ec.derive_private_key(789, ec.SECP256R1()))
    cases = []

    def add(name, error="none", **changes):
        p = dict(identity=(42).to_bytes(8, "big"), credential=1,
                 versions=[1], suites=[2], credentials=[1], extensions=[], proposals=[],
                 leaf_ext=[], package_ext=[], not_before=0, not_after=(1 << 64) - 1)
        p.update(changes)
        credential = p["credential"].to_bytes(2, "big") + vector(p["identity"])
        caps = b"".join(vector(u16(p[key])) for key in
                        ["versions", "suites", "extensions", "proposals", "credentials"])
        def exts(entries):
            return vector(b"".join(code.to_bytes(2, "big") + vector(data) for code, data in entries))
        leaf_tbs = (vector(encryption) + vector(point(signing)) + credential + caps + b"\1" +
                    p["not_before"].to_bytes(8, "big") + p["not_after"].to_bytes(8, "big") + exts(p["leaf_ext"]))
        leaf_sig = signing.sign(framed(b"LeafNodeTBS", leaf_tbs), ec.ECDSA(hashes.SHA256()))
        package_tbs = b"\0\1\0\2" + vector(init) + leaf_tbs + vector(leaf_sig) + exts(p["package_ext"])
        package_sig = signing.sign(framed(b"KeyPackageTBS", package_tbs), ec.ECDSA(hashes.SHA256()))
        signing.public_key().verify(leaf_sig, framed(b"LeafNodeTBS", leaf_tbs), ec.ECDSA(hashes.SHA256()))
        signing.public_key().verify(package_sig, framed(b"KeyPackageTBS", package_tbs), ec.ECDSA(hashes.SHA256()))
        cases.append((name, error, package_tbs + vector(package_sig)))

    add("baseline")
    add("mismatched_user", "identity_mismatch", identity=(43).to_bytes(8, "big"))
    add("zero_user", "identity_mismatch", identity=b"\0" * 8)
    for count in [0, 7, 9]:
        add(f"identity_length_{count}", "invalid_credential", identity=b"x" * count)
    add("x509_is_not_dave_basic", "invalid_credential", credential=2, identity=vector(b"\x30\0"))
    for key in ["versions", "suites", "credentials"]:
        add(f"missing_{key}", "invalid_capabilities", **{key: []})
    add("unknown_capabilities", versions=[0xAAAA, 1], suites=[2, 0xBABA], credentials=[0xCACA, 1],
        proposals=[0xDADA], extensions=[0xEAEA])
    add("repeated_capability_values", versions=[1, 1], suites=[2, 2], credentials=[1, 1],
        extensions=[0xAAAA, 0xAAAA], leaf_ext=[(0xAAAA, b"opaque")])
    for code in range(1, 6):
        add(f"listed_default_extension_{code}", "invalid_capabilities", extensions=[code])
    for code in range(1, 8):
        add(f"listed_default_proposal_{code}", "invalid_capabilities", proposals=[code])
    add("same_unknown_in_both_scopes", extensions=[0x0A0A],
        leaf_ext=[(0x0A0A, b"leaf")], package_ext=[(0x0A0A, b"package")])
    for scope in ["leaf_ext", "package_ext"]:
        add(f"unadvertised_{scope}", "invalid_extensions", **{scope: [(0x0A0A, b"unknown")]})
        add(f"duplicate_{scope}", "invalid_extensions", extensions=[0x0A0A],
            **{scope: [(0x0A0A, b"a"), (0x0A0A, b"b")]})
        add(f"large_unknown_{scope}", extensions=[0xFFFF], **{scope: [(0xFFFF, b"x" * 5000)]})
    add("implicit_application_id", leaf_ext=[(1, vector(b"device id"))])
    add("empty_application_id", leaf_ext=[(1, vector(b""))])
    add("duplicate_application_id", "invalid_extensions", leaf_ext=[(1, vector(b"a")), (1, vector(b"b"))])
    for value, name in [(b"", "missing"), (b"\x40\0", "noncanonical"), (b"\0x", "trailing")]:
        add(f"application_id_{name}", "invalid_extensions", leaf_ext=[(1, value)])
    add("application_id_in_package", "invalid_extensions", package_ext=[(1, vector(b"id"))])
    for code in range(2, 6):
        for scope in ["leaf_ext", "package_ext"]:
            add(f"default_location_{code}_{scope}", "invalid_extensions", **{scope: [(code, b"")]})
    add("expired_lifetime_ignored", not_before=1, not_after=2)
    add("future_lifetime_ignored", not_before=(1 << 64) - 2)
    codes = [6, 63, 64, 127, 128, 255, 256, 32767, 32768, 65535]
    add("bitset_boundaries", extensions=codes, leaf_ext=[(x, b"") for x in codes])
    codes = list(range(0x1000, 0x1400))
    add("many_unknown_extensions", extensions=codes, leaf_ext=[(x, b"") for x in codes])
    add("late_duplicate", "invalid_extensions", extensions=codes,
        leaf_ext=[(x, b"") for x in codes] + [(codes[0], b"")])
    lines = ["// OpenSSL-signed policy cases. Generated by tools/dave_package_policy_vectors.py.",
             "struct DavePolicyVector { const char* name; MlsPackageCryptoError error; const char* wire; };",
             "constexpr DavePolicyVector dave_policy_vectors[]{"]
    for name, error, wire in cases:
        lines.append(f'    {{"{name}", MlsPackageCryptoError::{error}, {text(wire)}}},')
    lines.append("};")
    Path("tests/data/dave_package_policy_vectors.hpp").write_text("\n".join(lines) + "\n", encoding="utf8")
    digest = hashlib.sha256(b"".join(x[2] for x in cases)).hexdigest()
    print(f"signed_policy_cases={len(cases)} valid={sum(x[1]=='none' for x in cases)} corpus_sha256={digest}")


if __name__ == "__main__":
    main()
