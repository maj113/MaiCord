"""Independent OpenSSL P-256 fixtures and native-signature verification."""
from pathlib import Path
import argparse
import hashlib
from cryptography.hazmat.primitives.asymmetric import ec, utils
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ORDER = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
def key(n): return ec.derive_private_key(n, ec.SECP256R1())
def public(k): return k.public_key().public_bytes(Encoding.X962, PublicFormat.UncompressedPoint)
def scalar(i): return int.from_bytes(hashlib.sha256(str(i).encode()).digest(), 'big') % (ORDER - 1) + 1

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify-native', type=Path)
    args = parser.parse_args()
    if args.verify_native:
        records = args.verify_native.read_bytes()
        assert records and len(records) % 161 == 0
        for offset in range(0, len(records), 161):
            point, digest, sig = records[offset:offset+65], records[offset+65:offset+97], records[offset+97:offset+161]
            verifier = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), point)
            r, s = int.from_bytes(sig[:32], 'big'), int.from_bytes(sig[32:], 'big')
            verifier.verify(utils.encode_dss_signature(r, s), digest, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
        print(f'OpenSSL verified {len(records)//161} native CNG signatures')
        return
    scalars = list(dict.fromkeys([1, 2, ORDER-1] + [1 << i for i in range(256)] + [scalar(i) for i in range(64)]))
    lines = ['// Independent synthetic OpenSSL vectors; tools/p256_oracle.py.', 'constexpr const char* p256_scalars[]{']
    digest = hashlib.sha256()
    for n in scalars:
        lines.append('    "' + n.to_bytes(32, 'big').hex() + '",')
        digest.update(public(key(n)))
    lines += ['};', f'constexpr char p256_public_digest[] = "{digest.hexdigest()}";',
              'struct P256Vector { const char *scalar, *point, *peer, *shared, *digest, *signature; };',
              'constexpr P256Vector p256_vectors[]{']
    indices = list(range(12))
    for i in range(12, 4096):
        if key(scalar(i)).exchange(ec.ECDH(), key(scalar(i+10000)).public_key())[0] == 0:
            indices.append(i)
            break
    assert len(indices) == 13
    for i in indices:
        n = scalar(i); private = key(n); peer = key(scalar(i+10000))
        hashed = hashlib.sha256(f'signature {i}'.encode()).digest()
        der = private.sign(hashed, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
        r, s = utils.decode_dss_signature(der)
        fields = [n.to_bytes(32, 'big'), public(private), public(peer), private.exchange(ec.ECDH(), peer.public_key()),
                  hashed, r.to_bytes(32, 'big') + s.to_bytes(32, 'big')]
        lines.append('    {' + ', '.join('"' + v.hex() + '"' for v in fields) + '},')
    lines.append('};\n')
    (Path(__file__).resolve().parents[1] / 'tests/data/p256_vectors.hpp').write_text('\n'.join(lines))
    print(f'public_scalar_cases={len(scalars)} sha256={digest.hexdigest()} ecdh_signature_cases=13')

if __name__ == '__main__': main()
