"""Independent libsodium oracle for tests/xchacha20_poly1305.cpp (QA only)."""

import hashlib

import nacl
from nacl.bindings import (crypto_aead_xchacha20poly1305_ietf_encrypt,
                           crypto_aead_xchacha20poly1305_ietf_decrypt)
from aes_gcm_oracle import LENGTHS, AAD_LENGTHS


def main():
    print(f"PyNaCl={nacl.__version__} backend=libsodium")
    plaintext = bytes((i * 37 + (i >> 3) + 11) & 255 for i in range(max(LENGTHS)))
    aad = bytes((i * 19 + 5) & 255 for i in range(max(AAD_LENGTHS)))
    keys = (bytes(32), bytes([255]) * 32, bytes(range(32)),
            bytes((i * 13 + 39) & 255 for i in range(32)))
    for key_index, key in enumerate(keys):
        digest = hashlib.sha256()
        count = 0
        for size in LENGTHS:
            for aad_size in AAD_LENGTHS:
                count += 1
                nonce = count.to_bytes(4, 'big') + bytes(
                    (i * 23 + count * (i + 1)) & 255 for i in range(4, 24))
                packet = crypto_aead_xchacha20poly1305_ietf_encrypt(
                    plaintext[:size], aad[:aad_size], nonce, key)
                assert crypto_aead_xchacha20poly1305_ietf_decrypt(
                    packet, aad[:aad_size], nonce, key) == plaintext[:size]
                digest.update(packet)
        print(f"key={key_index} cases={count} sha256={digest.hexdigest()}")


if __name__ == '__main__':
    main()
