"""Verify public native DAVE KeyPackages using independent framing and OpenSSL.

Records contain BE64 test user ID, BE32 raw package length, package, SHA256 ref.
Only public QA outputs are accepted; no private account material is required.
"""
import argparse
import hashlib
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from mls_objects_oracle import Reader, extensions, framed, leaf, point, vector


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("records", type=Path)
    args = parser.parse_args()
    raw = args.records.read_bytes()
    reader = Reader(raw)
    expected_users = [1, 255, 256, 0x0102030405060708, 1 << 63, (1 << 64) - 1, 42, 42]
    signing = ec.derive_private_key(123, ec.SECP256R1()).public_key()
    expected_point = point(ec.derive_private_key(123, ec.SECP256R1()))
    points = set()
    sizes = []
    for user in expected_users:
        assert reader.num(8) == user
        package = reader.take(reader.num(4))
        reference = reader.take(32)
        r = Reader(package)
        assert r.num(2) == 1 and r.num(2) == 2
        init = r.vec()
        lf = leaf(r)
        assert extensions(r) == b""
        package_tbs = package[:r.p]
        package_signature = r.vec()
        assert r.p == len(package)
        ek, sk, credential, identity, certificates = lf[:5]
        assert sk == expected_point
        assert credential == b"\0\1" and identity == user.to_bytes(8, "big") and not certificates
        assert lf[5:10] == [b"\0\1", b"\0\2", b"", b"", b"\0\1"]
        assert lf[10:15] == [b"\1", b"\0" * 8, b"\xff" * 8, b"", b""]
        for public in (init, ek):
            assert len(public) == 65 and public[0] == 4 and public not in points
            ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), public)
            points.add(public)
        signing.verify(lf[15], framed(b"LeafNodeTBS", lf[17]), ec.ECDSA(hashes.SHA256()))
        signing.verify(package_signature, framed(b"KeyPackageTBS", package_tbs), ec.ECDSA(hashes.SHA256()))
        assert reference == hashlib.sha256(vector(b"MLS 1.0 KeyPackage Reference") + vector(package)).digest()
        assert len(package) <= 394
        sizes.append(len(package))
    assert reader.p == len(raw)
    print(f"OpenSSL verified {len(sizes)} native packages, {len(sizes)*2} signatures, "
          f"{len(points)} distinct encryption points; byte sizes={sizes}")
    print(f"public_records_sha256={hashlib.sha256(raw).hexdigest()}")


if __name__ == "__main__":
    main()
