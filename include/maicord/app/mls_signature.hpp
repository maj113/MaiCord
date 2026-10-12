#pragma once
#include "maicord/platform/hkdf_sha256.hpp"
#include "maicord/platform/p256.hpp"
#include "maicord/protocol/p256_signature.hpp"

namespace maicord {
enum class MlsSignatureError : u8 {
    none,
    invalid_input,
    not_initialized,
    output_capacity,
    malformed_signature,
    hash_failure,
    key_failure,
    invalid_signature
};
struct MlsSignatureResult {
    u32 bytes;
    i32 native_status;
    MlsSignatureError error;
    constexpr bool ok() const noexcept {
        return error == MlsSignatureError::none;
    }
};
// SHA-256 SignContent: canonical vector("MLS 1.0 " + label), vector(a||b).
// Two borrowed content spans avoid a concatenation allocation. Full MLS vector
// lengths supported; no fixed label scratch limit. RefHash has NO added prefix.
// Hash/output failures preserve output; tails are untouched. External inputs/
// outputs must not overlap the workspace (owner or storage), or each other.
KdfResult mls_signature_hash(KdfSha256Workspace& workspace, ConstSlice<u8> label, ConstSlice<u8> a,
                             ConstSlice<u8> b, Slice<u8> digest) noexcept;
KdfResult mls_reference_hash(KdfSha256Workspace& workspace, ConstSlice<u8> label, ConstSlice<u8> value,
                             Slice<u8> digest) noexcept;
// Ciphersuite 2 ECDSA. Sign requires maximum DER capacity (72 bytes) before
// doing cryptographic work. Only result.bytes is the signature, no outer MLS
// vector length. Every failure preserves output. Inputs/output cannot overlap
// the key owner. Caller authenticates the key and serialized content/label.
MlsSignatureResult mls_sign_with_label(KdfSha256Workspace& workspace, const P256Key& key,
                                       ConstSlice<u8> label, ConstSlice<u8> a, ConstSlice<u8> b,
                                       Slice<u8> signature) noexcept;
MlsSignatureResult mls_verify_with_label(KdfSha256Workspace& workspace, const P256Key& key,
                                         ConstSlice<u8> label, ConstSlice<u8> a, ConstSlice<u8> b,
                                         ConstSlice<u8> signature) noexcept;
} // namespace maicord
