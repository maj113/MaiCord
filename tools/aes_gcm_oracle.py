"""Independent OpenSSL oracle for tests/aes_gcm.cpp; never used by MaiCord.

Run with Python's cryptography package available (QA dependency only).
Prints SHA-256 of every ciphertext and tag in deterministic matrix order.
The matrix uses a distinct 96-bit nonce for every encryption under each key.
"""

import hashlib

import cryptography
from cryptography.hazmat.backends.openssl.backend import backend
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

LENGTHS = (0, 1, 2, 7, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128,
           129, 255, 256, 257, 511, 512, 513, 1200, 1400, 4095, 4096,
           4097, 16383, 16384, 16385)
AAD_LENGTHS = (0, 1, 12, 15, 16, 17, 31, 32, 33, 64, 65)


def main():
    print(f"cryptography={cryptography.__version__} {backend.openssl_version_text()}")
    for key_bytes in (16, 32):
        key = bytes((i * 13 + 7 + key_bytes) & 255 for i in range(key_bytes))
        cipher = AESGCM(key)
        plaintext = bytes((i * 37 + (i >> 3) + 11) & 255 for i in range(max(LENGTHS)))
        aad = bytes((i * 19 + 5) & 255 for i in range(max(AAD_LENGTHS)))
        digest = hashlib.sha256()
        count = 0
        for size in LENGTHS:
            for aad_size in AAD_LENGTHS:
                count += 1
                nonce = count.to_bytes(4, 'big') + bytes(range(8))
                packet = cipher.encrypt(nonce, plaintext[:size], aad[:aad_size])
                assert cipher.decrypt(nonce, packet, aad[:aad_size]) == plaintext[:size]
                digest.update(packet)
        print(f"AES{key_bytes * 8}: cases={count} sha256={digest.hexdigest()}")


if __name__ == '__main__':
    main()
