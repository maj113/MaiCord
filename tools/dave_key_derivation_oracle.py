"""Independent cryptography HKDF/MLS/DAVE fixtures; QA dependencies only."""
import hashlib
from pathlib import Path
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDFExpand
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


def expand(secret, info, size):
    return HKDFExpand(algorithm=hashes.SHA256(), length=size, info=info).derive(secret)


def variable(value):
    if value < 64:
        return bytes([value])
    if value < 16384:
        return (value | 0x4000).to_bytes(2, 'big')
    return (value | 0x80000000).to_bytes(4, 'big')


def label_expand(secret, label, context, size):
    label = b'MLS 1.0 ' + label
    info = size.to_bytes(2, 'big') + variable(len(label)) + label + variable(len(context)) + context
    return expand(secret, info, size)


def main():
    lines = ['// Generated independently by tools/dave_key_derivation_oracle.py.']
    sizes = (0, 1, 16, 31, 32, 33, 63, 64, 65, 255, 8160)
    info_sizes = (0, 1, 31, 32, 33, 63, 64, 65, 255, 1025)
    digest = hashlib.sha256()
    info = bytes((i * 17 + 5) & 255 for i in range(max(info_sizes)))
    for key_size in (16, 32, 80):
        key = bytes((i * 13 + key_size) & 255 for i in range(key_size))
        for info_size in info_sizes:
            for size in sizes:
                digest.update(expand(key, info[:info_size], size))
    lines.append(f'constexpr char hkdf_matrix_digest[] = "{digest.hexdigest()}";')
    print('HKDF cases=330 sha256=' + digest.hexdigest())
    secret = bytes(range(32))
    labels = bytes(97 + i % 26 for i in range(16377))
    contexts = bytes((i * 29 + 7) & 255 for i in range(16385))
    digest = hashlib.sha256()
    for label_size in (0, 1, 55, 56, 57, 16375, 16376, 16377):
        for context_size in (0, 1, 63, 64, 65, 16383, 16384, 16385):
            for size in (16, 32, 65):
                digest.update(label_expand(secret, labels[:label_size], contexts[:context_size], size))
    lines.append(f'constexpr char mls_label_digest[] = "{digest.hexdigest()}";')
    print('MLS labels cases=192 sha256=' + digest.hexdigest())
    digest = hashlib.sha256()
    secret = bytes(range(16))
    for generation in range(257):
        context = generation.to_bytes(4, 'big')
        key = label_expand(secret, b'key', context, 16)
        secret = label_expand(secret, b'secret', context, 32)
        digest.update(key + secret)
    lines.append(f'constexpr char dave_chain_digest[] = "{digest.hexdigest()}";')
    print('DAVE steps=257 sha256=' + digest.hexdigest())
    # Encrypt an actual frame with the final derived key, rather than another
    # independently chosen fixture key. Clear spans [0,3) and [20,24).
    nonce = bytes(8) + (42).to_bytes(4, 'little')
    body = bytes((i * 11 + 3) & 255 for i in range(64))
    clear = set(range(3)) | set(range(20, 24))
    aad = bytes(v for i, v in enumerate(body) if i in clear)
    cipher = AESGCM(key).encrypt(nonce, bytes(v for i, v in enumerate(body) if i not in clear), aad)
    stream = iter(cipher[:-16])
    wire = bytes(v if i in clear else next(stream) for i, v in enumerate(body))
    wire += cipher[-16:-8] + bytes([42, 0, 3, 20, 4, 16, 0xfa, 0xfa])
    lines.append(f'constexpr char dave_derived_wire[] = "{wire.hex()}";')
    lines.append('struct DaveStepVector { u32 generation; const char* key; const char* next; };')
    lines.append('constexpr DaveStepVector dave_step_vectors[]{')
    for generation in (0x7fffffff, 0xfffffffe, 0xffffffff):
        context = generation.to_bytes(4, 'big')
        secret = bytes(range(32))
        key = label_expand(secret, b'key', context, 16)
        next_secret = label_expand(secret, b'secret', context, 32)
        lines.append(f'    {{{generation}u, "{key.hex()}", "{next_secret.hex()}"}},')
    lines.append('};\n')
    (Path(__file__).resolve().parents[1] / 'tests/data/dave_key_vectors.hpp').write_text('\n'.join(lines))


if __name__ == '__main__':
    main()
