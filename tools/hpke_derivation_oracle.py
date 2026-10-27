"""Independent RFC 9180/MLSWG derivation and AES-GCM fixture verification."""
import argparse
import hashlib
import hmac
import json
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

KEM = b'KEM\x00\x10'
SUITE = b'HPKE\x00\x10\x00\x01\x00\x01'
ORDER = int('ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551', 16)
SOURCE_SHA = '61fc662f01996cd06d713dacf5e133167bd309a1f329442d53f1e21a47b3ede6'


def extract(suite, salt, label, data):
    return hmac.digest(salt, b'HPKE-v1' + suite + label + data, 'sha256')


def expand(suite, secret, label, info, length):
    info = length.to_bytes(2, 'big') + b'HPKE-v1' + suite + label + info
    output = block = b''
    for i in range(1, (length + 31)//32 + 1):
        block = hmac.digest(secret, block + info + bytes([i]), 'sha256')
        output += block
    return output[:length]


def public(scalar):
    return ec.derive_private_key(int.from_bytes(scalar, 'big'), ec.SECP256R1()).public_key().public_bytes(
        serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)


def derive(ikm):
    prk = extract(KEM, b'', b'dkp_prk', ikm)
    for i in range(256):
        sk = expand(KEM, prk, b'candidate', bytes([i]), 32)
        if 0 < int.from_bytes(sk, 'big') < ORDER:
            return sk, public(sk)
    raise ValueError('rejection limit')


def kem(sk, enc, recipient):
    private = ec.derive_private_key(int.from_bytes(sk, 'big'), ec.SECP256R1())
    peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), recipient)
    prk = extract(KEM, b'', b'eae_prk', private.exchange(ec.ECDH(), peer))
    return expand(KEM, prk, b'shared_secret', enc + recipient, 32)


def schedule(shared, info):
    context = b'\x00' + extract(SUITE, b'', b'psk_id_hash', b'') + extract(SUITE, b'', b'info_hash', info)
    secret = extract(SUITE, shared, b'secret', b'')
    return b''.join(expand(SUITE, secret, label, context, n) for label, n in [(b'key',16),(b'base_nonce',12)])


def pattern(n):
    return (bytes((i*13+7)&255 for i in range(256)) * ((n+255)//256))[:n]


def row(fields):
    return '    {' + ', '.join('"' + (v.hex() if isinstance(v, bytes) else v) + '"' for v in fields) + '},'


def load(path, expected):
    raw = path.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == expected
    return json.loads(raw)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--hpke', required=True, type=Path)
    parser.add_argument('--mls', required=True, type=Path)
    args = parser.parse_args()
    all_vectors = load(args.hpke, SOURCE_SHA)
    mls = next(v for v in load(args.mls, '05aa9a68bd2538ace72d8c53375984cc728ef62220ebf314df675708546d97a7') if v['cipher_suite']==2)
    lines = ['// Published CFRG HPKE b1f7cb0cdeab6906c61b3d6574e8bdfdbe1cd3fb / MLSWG cfd450286d1bfd9cd2519b95c80f9771f94a5b1a.',
             '// Verified independently by tools/hpke_derivation_oracle.py. Public test material only.',
             'struct HpkePairVector { const char *ikm, *scalar, *point; };', 'constexpr HpkePairVector hpke_pairs[]{']
    pairs = {}
    for v in all_vectors:
        if v['kem_id'] != 16:
            continue
        for suffix in ['R', 'E']:
            ikm = bytes.fromhex(v['ikm'+suffix])
            sk, pk = derive(ikm)
            assert sk.hex() == v['sk'+suffix+'m'] and pk.hex() == v['pk'+suffix+'m']
            pairs[ikm] = (sk, pk)
    for v in mls['epochs']:
        ikm = bytes.fromhex(v['external_secret'])
        sk, pk = derive(ikm)
        assert pk.hex() == v['external_pub']
        pairs[ikm] = (sk, pk)
    for ikm, (sk, pk) in pairs.items():
        lines.append(row([ikm,sk,pk]))
    lines += ['};', 'struct HpkeDerivedBoundary { u32 length; const char *scalar, *point; };', 'constexpr HpkeDerivedBoundary hpke_ikm_boundaries[]{']
    for n in [0,1,31,32,33,55,56,63,64,65,511,512,513,16384]:
        sk, pk = derive(pattern(n))
        lines.append(f'    {{{n}, "{sk.hex()}", "{pk.hex()}"}},')
    lines += ['};', 'struct HpkeKemVector { const char *ephemeral, *recipient, *recipient_point, *enc, *shared; };', 'constexpr HpkeKemVector hpke_kems[]{']
    base = None
    kem_count=0
    for v in all_vectors:
        if v['kem_id']!=16 or v['mode']!=0:
            continue
        sk, enc, recipient = [bytes.fromhex(v[k]) for k in ['skEm','enc','pkRm']]
        assert kem(sk, enc, recipient).hex() == v['shared_secret']
        lines.append(row([v[k] for k in ['skEm','skRm','pkRm','enc','shared_secret']]))
        kem_count+=1
        if v['kdf_id']==1 and v['aead_id']==1:
            base=v
    assert base
    shared=bytes.fromhex(base['shared_secret'])
    material=schedule(shared,bytes.fromhex(base['info']))
    assert material.hex()==base['key']+base['base_nonce']
    lines += ['};', f'constexpr char hpke_base_shared[] = "{shared.hex()}";',
              f'constexpr char hpke_base_info[] = "{base["info"]}";', f'constexpr char hpke_base_material[] = "{material.hex()}";',
              'struct HpkeScheduleBoundary { u32 length; const char *material; };', 'constexpr HpkeScheduleBoundary hpke_info_boundaries[]{']
    for n in [0,1,55,56,63,64,65,511,512,513,16384,4194305]:
        lines.append(f'    {{{n}, "{schedule(shared,pattern(n)).hex()}"}},')
    lines += ['};', 'struct HpkeEncryptionVector { const char *aad, *pt, *ct, *nonce; };', 'constexpr HpkeEncryptionVector hpke_encryptions[]{']
    for i, enc in enumerate(base['encryptions']):
        nonce=bytes(a^b for a,b in zip(material[16:28],i.to_bytes(12,'big')))
        assert nonce.hex()==enc['nonce']
        assert AESGCM(material[:16]).encrypt(nonce,bytes.fromhex(enc['pt']),bytes.fromhex(enc['aad'])).hex()==enc['ct']
        lines.append(row([enc[k] for k in ['aad','pt','ct','nonce']]))
    Path('tests/data/hpke_derivation_vectors.hpp').write_text('\n'.join(lines + ['};'])+'\n',encoding='utf8')
    print(f'source_sha256={SOURCE_SHA} key_pairs={len(pairs)} ikm_boundaries=14 kem_cases={kem_count} info_boundaries=12 encryptions={len(base["encryptions"])}')


if __name__=='__main__':
    main()
