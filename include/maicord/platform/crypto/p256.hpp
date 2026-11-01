#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
enum class P256Kind : u8 {
    ecdh = 1,
    ecdsa = 2
};
enum class P256Error : u8 {
    none,
    invalid_input,
    not_initialized,
    wrong_kind,
    private_required,
    output_capacity,
    invalid_key,
    invalid_signature,
    platform_failure
};
struct P256Result {
    u32 bytes;
    i32 native_status;
    P256Error error;
    constexpr bool ok() const noexcept {
        return error == P256Error::none;
    }
};
// CNG holds the key material; no borrowed input buffers are kept after import.
// Zero-initialize before use, use from one worker only, and do not copy. Never overwrite a live key:
// prepare a separate candidate key and swap it in at the application level.
struct P256Key {
    void* handle;
    P256Kind kind;
    bool has_private;
};
void p256_release(P256Key* key) noexcept;
P256Result p256_generate(P256Key* key, P256Kind kind) noexcept;
P256Result p256_import_public(P256Key* key, P256Kind kind, ConstSlice<u8> public_key) noexcept;
// Scalar is 32 big-endian bytes in [1,n). The optional public point is
// 65-byte SEC1 uncompressed and must match the scalar. When omitted, CNG
// computes it during import; there is no custom scalar arithmetic or lookup table.
P256Result p256_import_private(P256Key* key, P256Kind kind, ConstSlice<u8> scalar,
                               ConstSlice<u8> public_key = {}) noexcept;
P256Result p256_export_public(const P256Key& key, Slice<u8> output) noexcept;
P256Result p256_export_private(const P256Key& key, Slice<u8> output) noexcept;
// Raw shared secret is returned in network big-endian order, not CNG's little
// endian representation. This is the DH primitive; HPKE's KDF is separate.
P256Result p256_ecdh(const P256Key& private_key, const P256Key& peer, Slice<u8> output) noexcept;
// Digest must be 32 bytes (SHA-256). Signature is 64-byte IEEE P1363 r||s,
// not MLS's DER encoding; the protocol layer does the framing/DER conversion.
P256Result p256_sign_digest(const P256Key& key, ConstSlice<u8> digest, Slice<u8> signature) noexcept;
P256Result p256_verify_digest(const P256Key& key, ConstSlice<u8> digest, ConstSlice<u8> signature) noexcept;
// Output is written only on success, and only the required prefix (65/32/64 bytes);
// every failure leaves it unchanged. Output cannot overlap a key or the sign
// input. Temporary private/scalar/shared/signature buffers are erased.
} // namespace maicord
