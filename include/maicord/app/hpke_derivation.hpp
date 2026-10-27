#pragma once
#include "maicord/platform/hkdf_sha256.hpp"
#include "maicord/platform/p256.hpp"

namespace maicord {
enum class HpkeDerivationError : u8 {
    none,
    invalid_input,
    not_initialized,
    output_capacity,
    key_failure,
    kdf_failure,
    rejection_limit
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
// Output must be an empty, worker-exclusive object. Caller supplies IKM
// with sufficient entropy; this deterministic primitive is not an RNG.
HpkeDerivationResult hpke_derive_key_pair(KdfSha256Workspace& workspace, ConstSlice<u8> ikm,
                                          P256Key* output) noexcept;
// The sender MUST use a fresh ephemeral key for each independent encapsulation.
// No ephemeral scalar is exported. Both outputs are written together only after
// success. Decapsulation binds the received encapsulation and recipient point.
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
KdfResult hpke_info_hash(KdfSha256Workspace& workspace, ConstSlice<ConstSlice<u8>> info,
                         Slice<u8> output) noexcept;
// Trusted prehashed input from hpke_info_hash (exactly 32 bytes). The output
// is written atomically and must not overlap the inputs or the workspace.
HpkeDerivationResult hpke_base_key_schedule_hashed_info(KdfSha256Workspace& workspace,
                                                        ConstSlice<u8> shared_secret,
                                                        ConstSlice<u8> info_hash,
                                                        HpkeBaseSecrets* output) noexcept;
// Derive/encap/decap/schedule stage results; ALL failures preserve output.
// Success writes only required prefixes (65/32). Schedule may replace input
// storage; other outputs must not overlap inputs or each other. All external
// spans must avoid the workspace, key handles and the output key handle. Temporaries
// holding private scalars, shared secrets or derived material are erased.
} // namespace maicord
