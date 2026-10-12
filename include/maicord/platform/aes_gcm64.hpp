#pragma once

#include "maicord/platform/aead.hpp"
#include "maicord/platform/secure_buffer.hpp"

namespace maicord {

enum class Gcm64Backend : u8 {
    automatic,
    pclmul,
    portable
};

// AES128-GCM with a fixed 96-bit nonce and 64-bit tag, as required by DAVE.
// Independent from the transport cipher: its 128-bit tag is never shortened.
// Zero-initialize; one worker performs operations/release; never copy a live key.
// One locked allocation holds the CNG AES-ECB key and GHASH subkey powers.
// PCLMUL/SSSE3 GHASH is selected once at initialization when available; the
// portable GHASH path uses fixed-iteration arithmetic.
// ECB is only the internal AES block primitive for GCM, not a wire cipher mode.
struct AesGcm64Key {
    void* handle;
    SecureBuffer storage;
    Gcm64Backend backend;
};

AeadResult aes_gcm64_initialize(AesGcm64Key* key, ConstSlice<u8> secret,
                                Gcm64Backend backend = Gcm64Backend::automatic) noexcept;
void aes_gcm64_release(AesGcm64Key* key) noexcept;

// Caller guarantees per-key nonce uniqueness and enforces DAVE key lifetime,
// replay and failed-authentication policy. Exactly 16 secret / 12 nonce / 8 tag
// bytes; length is not negotiated by accepting an arbitrary shorter tag.
// Same alias/capacity contract as aes_gcm; in-place payload is supported.
// Invalid inputs leave output unchanged. Authentication/platform failures wipe
// only the attempted payload prefix (and encryption tag). Decryption verifies
// the complete tag before producing any plaintext. No per-frame allocation.
AeadResult aes_gcm64_encrypt(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                             ConstSlice<u8> plaintext, Slice<u8> ciphertext, Slice<u8> tag) noexcept;
AeadResult aes_gcm64_decrypt(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                             ConstSlice<u8> ciphertext, ConstSlice<u8> tag, Slice<u8> plaintext) noexcept;

} // namespace maicord
