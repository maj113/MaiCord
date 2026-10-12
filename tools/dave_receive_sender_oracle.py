"""Independent mixed-clear DAVE frames across distinct sender epochs."""
from pathlib import Path
import hashlib
import sys
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

sys.dont_write_bytecode = True
from dave_key_derivation_oracle import label_expand


def main():
    lines = ['// Synthetic independent OpenSSL fixtures; tools/dave_receive_sender_oracle.py.',
             'struct SenderVector { u32 epoch, counter; const char* wire; };',
             'constexpr SenderVector sender_vectors[]{']
    digest = hashlib.sha256()
    clear = set(range(3)) | set(range(19, 24))
    for epoch in range(3):
        seed = bytes((i + epoch * 31) & 255 for i in range(16))
        key = label_expand(seed, b'key', bytes(4), 16)
        body = bytes((i * 17 + epoch * 7 + 9) & 255 for i in range(48))
        aad = bytes(v for i, v in enumerate(body) if i in clear)
        plaintext = bytes(v for i, v in enumerate(body) if i not in clear)
        for counter in range(1, 5):
            encrypted = AESGCM(key).encrypt(bytes(8) + counter.to_bytes(4, 'little'), plaintext, aad)
            stream = iter(encrypted[:-16])
            wire = bytes(v if i in clear else next(stream) for i, v in enumerate(body))
            wire += encrypted[-16:-8] + bytes([counter, 0, 3, 19, 5, 16, 250, 250])
            digest.update(wire)
            lines.append(f'    {{{epoch}, {counter}, "{wire.hex()}"}},')
    lines.append('};\n')
    (Path(__file__).resolve().parents[1] / 'tests/data/dave_sender_vectors.hpp').write_text('\n'.join(lines))
    print('frames=12 sha256=' + digest.hexdigest())


if __name__ == '__main__':
    main()
