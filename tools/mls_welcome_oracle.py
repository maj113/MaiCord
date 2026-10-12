"""Published/independent Welcome decryption and GroupSecrets wire fixtures."""
import argparse
import hashlib
import hmac
import json
from pathlib import Path
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from mls_objects_oracle import Reader, SHA, framed, normal, text, vector
from mls_join_message_vectors import group_info
from mls_hpke_oracle import receiver_material, info
from hpke_derivation_oracle import derive, kem, public, schedule
from dave_key_derivation_oracle import label_expand

WELCOME_SHA = '06be9d5c99817ef2545e4b15b8e73fd9b604685a8e55b59ca168eda98e236502'


def psk(r):
    start = r.p
    kind = r.take(1)
    usage, identity, gid, epoch = b'\0', b'', b'', bytes(8)
    if kind == b'\1': identity = r.vec()
    else:
        assert kind == b'\2'
        usage = r.take(1)
        assert usage in [b'\1', b'\2', b'\3']
        gid, epoch = r.vec(), r.take(8)
    nonce = r.vec()
    return [kind, usage, identity, gid, epoch, nonce, r.b[start:r.p]]


def secrets(r):
    joiner, present = r.vec(), r.take(1)
    assert present in [b'\0', b'\1']
    path = r.vec() if present == b'\1' else b''
    psks = r.vec()
    fields = [joiner, present, path, psks]
    q = Reader(psks)
    while q.p < len(psks): fields.extend(psk(q))
    return [*fields, r.b[:r.p]]


def material(joiner):
    welcome = label_expand(hmac.digest(joiner, bytes(32), 'sha256'), b'welcome', b'', 32)
    return label_expand(welcome,b'key',b'',16), label_expand(welcome,b'nonce',b'',12)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--welcome', type=Path, required=True)
    parser.add_argument('--messages', type=Path, required=True)
    args = parser.parse_args()
    raw = args.welcome.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == WELCOME_SHA
    v = next(x for x in json.loads(raw) if x['cipher_suite'] == 2)
    scalar, package, wire, signer = [bytes.fromhex(v[x]) for x in ['init_priv','key_package','welcome','signer_pub']]
    assert package[:4] == b'\0\1\0\5' and wire[:4] == b'\0\1\0\3'
    reference = hashlib.sha256(vector(b'MLS 1.0 KeyPackage Reference')+vector(package[4:])).digest()
    wire = wire[4:]
    r = Reader(wire)
    assert r.num(2) == 2
    entries = Reader(r.vec())
    selected = []
    while entries.p < len(entries.b):
        ref, enc, ct = entries.vec(), entries.vec(), entries.vec()
        if ref == reference: selected.append((enc,ct))
    assert len(selected) == 1
    encrypted_info = r.vec()
    assert r.p == len(wire)
    m = receiver_material(scalar,public(scalar),selected[0][0],b'Welcome',encrypted_info)
    gs = AESGCM(m[:16]).decrypt(m[16:28],selected[0][1],b'')
    sr = Reader(gs); sf = secrets(sr)
    assert sr.p == len(gs) and not sf[3]
    key, nonce = material(sf[0])
    gi = AESGCM(key).decrypt(nonce,encrypted_info,b'')
    gr = Reader(gi); gf = group_info(gr)
    assert gr.p == len(gi)
    ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(),signer).verify(gf[-3],framed(b'GroupInfoTBS',gf[-1]),ec.ECDSA(hashes.SHA256()))
    cases = [('published',wire,gs,gi,'none')]

    def add(name, plain=gs, group=gi, error='none', bad_info_tag=False, recipient_position=0, duplicate=False):
        key, nonce = material(plain[1:33]) # Synthetic cases keep the leading one-byte joiner length.
        encrypted = AESGCM(key).encrypt(nonce,group,b'')
        if bad_info_tag: encrypted = encrypted[:-1]+bytes([encrypted[-1]^1])
        sk, enc = derive(hashlib.sha256(name.encode()).digest())
        m = schedule(kem(sk,enc,public(scalar)),info(b'Welcome',encrypted))
        ciphertext = AESGCM(m[:16]).encrypt(m[16:28],plain,b'')
        # Cross-check the synthetic sender with the independent recipient path.
        rm = receiver_material(scalar,public(scalar),enc,b'Welcome',encrypted)
        assert AESGCM(rm[:16]).decrypt(rm[16:28],ciphertext,b'') == plain
        entry = vector(reference)+vector(enc)+vector(ciphertext)
        other = vector(bytes(32))+vector(enc)+vector(ciphertext)
        entries = [other,other]
        entries.insert(recipient_position,entry)
        if duplicate: entries.append(entry)
        raw = b'\0\2'+vector(b''.join(entries))+vector(encrypted)
        cases.append((name,raw,plain,group,error))

    joiner = bytes(range(32))
    no_path = vector(joiner)+b'\0\0'
    path = vector(joiner)+b'\1'+vector(bytes(range(32,64)))+b'\0'
    for position in range(3): add(f'recipient_{position}',no_path,recipient_position=position)
    add('path_secret',path)
    add('duplicate_reference',no_path,error='duplicate_reference',duplicate=True)
    add('invalid_optional',vector(joiner)+b'\2\0',error='invalid_group_secrets')
    add('short_joiner',vector(joiner[:-1])+b'\0\0',error='invalid_group_secrets')
    add('short_path',vector(joiner)+b'\1'+vector(b'x'*31)+b'\0',error='invalid_group_secrets')
    add('trailing_secret',no_path+b'x',error='invalid_group_secrets')
    add('psk_unavailable',vector(joiner)+b'\0'+vector(b'\1'+vector(b'psk')+vector(bytes(32))),error='psk_unavailable')
    add('large_psk',vector(joiner)+b'\0'+vector(b'\1'+vector(b'x'*5000)+vector(bytes(32))),error='psk_unavailable')
    add('info_bad_tag',no_path,bad_info_tag=True,error='group_info_authentication_failed')
    add('info_bad_framing',no_path,group=b'\0',error='group_info_failure')
    add('info_wrong_suite',no_path,group=gi[:2]+b'\0\1'+gi[4:],error='unsupported_suite')
    add('info_wrong_version',no_path,group=b'\0\2'+gi[2:],error='unsupported_suite')
    for n in [63,64,16383,16384]:
        # Valid structural GroupInfo with extra opaque extension; no tree/signature
        # admission is asserted by this decryption-only candidate API.
        context = gf[7]
        extra = context+vector(b'\x0a\x0a'+vector(b'x'*n))+vector(gf[9])+gf[10]+vector(gf[11])
        add(f'large_info_{n}',no_path,group=extra)
    lines = ['// Published MLSWG Welcome plus Python/OpenSSL synthetic ciphertexts.',
             '// Test-only private/plaintext material, never account data.',
             f'constexpr char welcome_init_scalar[] = "{scalar.hex()}";',
             f'constexpr char welcome_package_reference[] = "{reference.hex()}";',
             'struct WelcomeCryptoVector { const char *name, *wire, *secrets, *info; DaveWelcomeError error; };',
             'constexpr WelcomeCryptoVector welcome_crypto_vectors[]{']
    for name,wire,gs,gi,error in cases:
        lines.append(f'{{"{name}", {text(wire)}, {text(gs)}, {text(gi)}, DaveWelcomeError::{error}}},')
    lines.append('};')
    Path('tests/data/mls_welcome_crypto_vectors.hpp').write_text('\n'.join(lines)+'\n',encoding='utf8')
    raw = args.messages.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == SHA
    corpus = [bytes.fromhex(v['group_secrets']) for v in json.loads(raw)]
    for usage in range(1,4):
        p = b'\2'+bytes([usage])+vector(b'x'*5000)+bytes(8)+vector(bytes(32))
        corpus.append(vector(joiner)+b'\0'+vector(p))
    digest = hashlib.sha256()
    for value in corpus:
        r = Reader(value); fields = secrets(r)
        assert r.p == len(value)
        digest.update(normal(fields))
    lines = ['// Published GroupSecrets plus larger resumption PSK identifiers.',
             'constexpr const char* group_secrets_vectors[]{', *[text(x)+',' for x in corpus], '};',
             f'constexpr char group_secrets_digest[] = "{digest.hexdigest()}";']
    Path('tests/data/mls_group_secrets_vectors.hpp').write_text('\n'.join(lines)+'\n',encoding='utf8')
    print(f'published_welcome_sha256={WELCOME_SHA} crypto_cases={len(cases)} group_secrets_cases={len(corpus)} normalized_sha256={digest.hexdigest()}')


if __name__ == '__main__': main()
