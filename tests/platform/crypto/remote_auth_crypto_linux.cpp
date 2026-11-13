// remote_auth_crypto.hpp over OpenSSL (Linux): key generation, the SubjectPublicKeyInfo and its fingerprint, and
// OAEP-SHA-256 decryption of a message that the test encrypts to the exported public key with libcrypto itself.
#include "maicord/platform/crypto/remote_auth_crypto.hpp"
#include "maicord/platform/crypto/random.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/shared_library.hpp"
#include "../../support/test_output.hpp"
#include "../../support/test_sha256.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

// What the test needs of libcrypto to play the sender: parse the SPKI and encrypt with OAEP-SHA-256.
struct Sender {
    SharedLibrary library;
    void* (*d2i_PUBKEY)(void**, const u8**, long);
    void* (*EVP_PKEY_CTX_new)(void*, void*);
    int (*EVP_PKEY_encrypt_init)(void*);
    int (*EVP_PKEY_encrypt)(void*, u8*, usize*, const u8*, usize);
    int (*EVP_PKEY_CTX_set_rsa_padding)(void*, int);
    int (*EVP_PKEY_CTX_set_rsa_oaep_md)(void*, const void*);
    int (*EVP_PKEY_CTX_set_rsa_mgf1_md)(void*, const void*);
    const void* (*EVP_sha256)();
    void (*EVP_PKEY_CTX_free)(void*);
    void (*EVP_PKEY_free)(void*);

    bool open() noexcept {
        if (!shared_library_open(&library, "libcrypto.so.4") && !shared_library_open(&library, "libcrypto.so.3"))
            return false;
#define BIND(name) name = reinterpret_cast<decltype(name)>(dlsym(library.handle, #name))
        BIND(d2i_PUBKEY);
        BIND(EVP_PKEY_CTX_new);
        BIND(EVP_PKEY_encrypt_init);
        BIND(EVP_PKEY_encrypt);
        BIND(EVP_PKEY_CTX_set_rsa_padding);
        BIND(EVP_PKEY_CTX_set_rsa_oaep_md);
        BIND(EVP_PKEY_CTX_set_rsa_mgf1_md);
        BIND(EVP_sha256);
        BIND(EVP_PKEY_CTX_free);
        BIND(EVP_PKEY_free);
#undef BIND
        return d2i_PUBKEY && EVP_PKEY_CTX_new && EVP_PKEY_encrypt_init && EVP_PKEY_encrypt && EVP_PKEY_CTX_set_rsa_padding &&
               EVP_PKEY_CTX_set_rsa_oaep_md && EVP_PKEY_CTX_set_rsa_mgf1_md && EVP_sha256 && EVP_PKEY_CTX_free && EVP_PKEY_free;
    }

    // The 256-byte ciphertext of `message` to the key in `spki`; false on any failure.
    bool encrypt(ConstSlice<u8> spki, ConstSlice<u8> message, u8 (&ciphertext)[256]) noexcept {
        const u8* cursor = spki.data;
        void* key = d2i_PUBKEY(nullptr, &cursor, static_cast<long>(spki.count));
        if (!key)
            return false;
        void* context = EVP_PKEY_CTX_new(key, nullptr);
        usize written = sizeof(ciphertext);
        const bool good = context && EVP_PKEY_encrypt_init(context) == 1 && EVP_PKEY_CTX_set_rsa_padding(context, 4) > 0 &&
                          EVP_PKEY_CTX_set_rsa_oaep_md(context, EVP_sha256()) > 0 &&
                          EVP_PKEY_CTX_set_rsa_mgf1_md(context, EVP_sha256()) > 0 &&
                          EVP_PKEY_encrypt(context, ciphertext, &written, message.data, message.count) == 1 && written == 256;
        if (context)
            EVP_PKEY_CTX_free(context);
        EVP_PKEY_free(key);
        return good;
    }
};

}

void test_remote_auth_crypto(Check check) noexcept {
    RemoteAuthCrypto crypto{};
    u8 spki[kRemoteAuthPublicSpkiMaximumBytes], fingerprint[40];
    check(remote_auth_crypto_export_spki(crypto, {spki, sizeof(spki)}).error == RemoteAuthCryptoError::not_initialized &&
              remote_auth_crypto_fingerprint(crypto, {fingerprint, 32}).error == RemoteAuthCryptoError::not_initialized &&
              remote_auth_crypto_decrypt(crypto, {spki, 256}, {fingerprint, 32}).error == RemoteAuthCryptoError::not_initialized,
          "remote auth crypto is unusable before initialization");
    const auto initialized = remote_auth_crypto_initialize(&crypto);
    check(initialized.ok() && crypto.rsa_key, "remote auth RSA-2048 key generation");
    if (!crypto.rsa_key)
        return;
    check(remote_auth_crypto_initialize(&crypto).error == RemoteAuthCryptoError::invalid_input,
          "a live remote auth key is not replaced");

    // SubjectPublicKeyInfo: SEQUENCE { rsaEncryption with NULL, BIT STRING { RSAPublicKey 2048 bits, e = 65537 } }.
    static constexpr u8 kHeader[]{0x30, 0x82, 0x01, 0x22, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,
                                  0x0d, 0x01, 0x01, 0x01, 0x05, 0x00, 0x03, 0x82, 0x01, 0x0f, 0x00, 0x30, 0x82,
                                  0x01, 0x0a, 0x02, 0x82, 0x01, 0x01, 0x00};
    static constexpr u8 kExponent[]{0x02, 0x03, 0x01, 0x00, 0x01};
    memset(spki, 0xa5, sizeof(spki));
    const auto exported = remote_auth_crypto_export_spki(crypto, {spki, sizeof(spki)});
    check(exported.ok() && exported.bytes == 294 && bytes_equal(spki, kHeader, sizeof(kHeader)) &&
              bytes_equal(spki + 294 - sizeof(kExponent), kExponent, sizeof(kExponent)) && (spki[sizeof(kHeader)] & 0x80) != 0 &&
              test_filled(spki + 294, sizeof(spki) - 294, 0xa5),
          "the exported SPKI is the 294-byte rsaEncryption key with a NULL parameter");
    u8 short_spki[293];
    memset(short_spki, 0xa5, sizeof(short_spki));
    const auto short_export = remote_auth_crypto_export_spki(crypto, {short_spki, sizeof(short_spki)});
    check(short_export.error == RemoteAuthCryptoError::output_too_small && short_export.bytes == 294 &&
              test_filled(short_spki, sizeof(short_spki), 0xa5),
          "a short SPKI output reports the size and stays untouched");
    check(remote_auth_crypto_export_spki(crypto, {}).error == RemoteAuthCryptoError::output_too_small &&
              remote_auth_crypto_export_spki(crypto, {nullptr, 4}).error == RemoteAuthCryptoError::invalid_input,
          "SPKI export sizes into nothing and rejects a null output with a length");

    // The fingerprint is the SHA-256 of that encoding.
    TestSha256 hash;
    test_sha256_begin(&hash);
    test_sha256_update(&hash, spki, 294);
    u8 expected[32];
    test_sha256_finish(&hash, expected);
    memset(fingerprint, 0xa5, sizeof(fingerprint));
    const auto fingerprinted = remote_auth_crypto_fingerprint(crypto, {fingerprint, 40});
    check(fingerprinted.ok() && fingerprinted.bytes == 32 && bytes_equal(fingerprint, expected, 32) &&
              test_filled(fingerprint + 32, 8, 0xa5),
          "the fingerprint is the SHA-256 of the SPKI");
    check(remote_auth_crypto_fingerprint(crypto, {fingerprint, 31}).error == RemoteAuthCryptoError::output_too_small,
          "a short fingerprint output is rejected");

    // Decryption of OAEP-SHA-256 ciphertexts made by another library to the exported key.
    Sender sender{};
    const bool sender_ready = sender.open();
    check(sender_ready, "libcrypto provides the sender side of the OAEP test");
    if (sender_ready) {
        u8 message[32], ciphertext[256], plaintext[64];
        check(secure_random_fill({message, 32}).ok() && sender.encrypt({spki, 294}, {message, 32}, ciphertext),
              "OAEP ciphertext for the exported key");
        memset(plaintext, 0xa5, sizeof(plaintext));
        const auto decrypted = remote_auth_crypto_decrypt(crypto, {ciphertext, 256}, {plaintext, 64});
        check(decrypted.ok() && decrypted.bytes == 32 && bytes_equal(plaintext, message, 32) && test_filled(plaintext + 32, 32, 0xa5),
              "OAEP-SHA-256 decryption returns the message");
        u8 exact[32];
        check(remote_auth_crypto_decrypt(crypto, {ciphertext, 256}, {exact, 32}).ok() && bytes_equal(exact, message, 32),
              "decryption into an exact-size output");
        u8 small[31];
        memset(small, 0xa5, sizeof(small));
        const auto too_small = remote_auth_crypto_decrypt(crypto, {ciphertext, 256}, {small, 31});
        check(too_small.error == RemoteAuthCryptoError::output_too_small && too_small.bytes == 32 &&
                  test_filled(small, sizeof(small), 0xa5),
              "a short plaintext output reports the size and stays untouched");
        // A damaged ciphertext fails and the destination is cleared.
        constexpr u32 damaged[]{0, 100, 255};
        for (u32 index : damaged) {
            ciphertext[index] ^= 1;
            memset(plaintext, 0xa5, sizeof(plaintext));
            const auto bad = remote_auth_crypto_decrypt(crypto, {ciphertext, 256}, {plaintext, 64});
            check(bad.error == RemoteAuthCryptoError::platform_failure && test_filled(plaintext, 64, 0),
                  "a damaged ciphertext is rejected and the destination cleared");
            ciphertext[index] ^= 1;
        }
        check(remote_auth_crypto_decrypt(crypto, {ciphertext, 255}, {plaintext, 64}).error == RemoteAuthCryptoError::invalid_input &&
                  remote_auth_crypto_decrypt(crypto, {nullptr, 256}, {plaintext, 64}).error == RemoteAuthCryptoError::invalid_input,
              "a ciphertext that is not one RSA-2048 block is rejected");
        // A message of the largest OAEP size (256 - 2 * 32 - 2 = 190 bytes) round-trips.
        u8 largest[190], opened[190];
        check(secure_random_fill({largest, 190}).ok() && sender.encrypt({spki, 294}, {largest, 190}, ciphertext) &&
                  remote_auth_crypto_decrypt(crypto, {ciphertext, 256}, {opened, 190}).ok() && bytes_equal(opened, largest, 190),
              "the largest OAEP message round-trips");
        shared_library_close(&sender.library);
    }

    remote_auth_crypto_release(&crypto);
    check(!crypto.rsa_key && !crypto.rsa_algorithm && !crypto.sha256_algorithm, "release retires the key");
    remote_auth_crypto_release(&crypto);
    remote_auth_crypto_release(nullptr);
}

} // namespace maicord
