#pragma once
#include "maicord/platform/crypto/hkdf_sha256.hpp"
#include "maicord/platform/crypto/p256.hpp"

namespace maicord {
enum class HpkeDerivationError : u8 {
    none,
    failed
};
struct HpkeDerivationResult {
    u32 bytes;
    i32 native_status;
    HpkeDerivationError error;
    constexpr bool ok() const noexcept {
        return error == HpkeDerivationError::none;
    }
};
// DHKEM(P-256, HKDF-SHA256), KEM 0x0010. Keeps the derived key in CNG.
// Output must be an empty object. Caller supplies IKM with sufficient entropy;
// this deterministic primitive is not an RNG. Callers pass the 65-byte encapsulation, 65-byte recipient point
// and 32-byte shared secret buffers; a failure leaves the outputs undefined.
HpkeDerivationResult hpke_derive_key_pair(KdfSha256Workspace& workspace, ConstSlice<u8> ikm,
                                          P256Key* output) noexcept;
// The sender MUST use a fresh ephemeral key for each independent encapsulation.
// No ephemeral scalar is exported. Decapsulation binds the received encapsulation and recipient point.
HpkeDerivationResult hpke_encap(KdfSha256Workspace& workspace, const P256Key& ephemeral,
                                ConstSlice<u8> recipient_point, Slice<u8> encapsulated,
                                Slice<u8> shared_secret) noexcept;
HpkeDerivationResult hpke_decap(KdfSha256Workspace& workspace, const P256Key& recipient,
                                ConstSlice<u8> encapsulated, Slice<u8> shared_secret) noexcept;
// Candidate material for HPKE Base mode (0), KDF 0x0001, AEAD 0x0001.
// Keep these values in caller-owned locked storage; clear them when done.
// This is not an AEAD context and does not manage nonce sequencing or roles.
struct HpkeBaseSecrets {
    u8 key[16], base_nonce[12];
};
void hpke_base_secrets_clear(HpkeBaseSecrets* secrets) noexcept;
// HPKE LabeledExtract("", "info_hash", info), NOT raw SHA-256(info).
// Segments avoid copying arbitrary protocol framing/labels/context.
KdfResult hpke_info_hash(KdfSha256Workspace& workspace, const ConstSlice<ConstSlice<u8>>& info,
                         const Slice<u8>& output) noexcept;
// Trusted prehashed input from hpke_info_hash (exactly 32 bytes) with a 32-byte shared secret. Temporaries holding
// private scalars, shared secrets or derived material are erased.
HpkeDerivationResult hpke_base_key_schedule_hashed_info(KdfSha256Workspace& workspace,
                                                        ConstSlice<u8> shared_secret,
                                                        ConstSlice<u8> info_hash,
                                                        HpkeBaseSecrets* output) noexcept;
} // namespace maicord
