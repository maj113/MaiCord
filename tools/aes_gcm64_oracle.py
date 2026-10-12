"""Independent AES128-GCM/64 QA oracle; no production dependency.

OpenSSL encrypts full tags and verifies their eight-byte prefixes through its
low-level GCM decryptor. Includes empty/partial/block/batch-boundary payloads.
"""
import hashlib
import cryptography
from cryptography.hazmat.backends.openssl.backend import backend
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

LENGTHS = (0, 1, 2, 7, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128,
           129, 255, 256, 257, 511, 512, 513, 1007, 1008, 1009, 1023,
           1024, 1025, 1200, 1400, 2047, 2048, 2049, 4095, 4096,
           4097, 16383, 16384, 16385)
AAD_LENGTHS = (0, 1, 12, 15, 16, 17, 31, 32, 33, 64, 65, 127, 128, 129,
               255, 256, 257, 1023, 1024, 1025)

def main():
    print(f"cryptography={cryptography.__version__} {backend.openssl_version_text()}")
    key = bytes((i * 13 + 23) & 255 for i in range(16))
    plaintext = bytes((i * 37 + (i >> 3) + 11) & 255 for i in range(max(LENGTHS)))
    aad = bytes((i * 19 + 5) & 255 for i in range(max(AAD_LENGTHS)))
    digest = hashlib.sha256()
    count = 0
    for size in LENGTHS:
        for aad_size in AAD_LENGTHS:
            count += 1
            nonce = count.to_bytes(4, 'big') + bytes(range(8))
            packet = AESGCM(key).encrypt(nonce, plaintext[:size], aad[:aad_size])
            cipher, tag = packet[:-16], packet[-16:-8]
            decryptor = Cipher(algorithms.AES(key), modes.GCM(nonce, tag, min_tag_length=8)).decryptor()
            decryptor.authenticate_additional_data(aad[:aad_size])
            assert decryptor.update(cipher) + decryptor.finalize() == plaintext[:size]
            digest.update(cipher + tag)
    print(f"AES128-GCM64 cases={count} sha256={digest.hexdigest()}")

if __name__ == '__main__':
    main()
