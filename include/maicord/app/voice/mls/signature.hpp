#pragma once
#include "maicord/platform/crypto/hkdf_sha256.hpp"
#include "maicord/platform/crypto/p256.hpp"

namespace maicord {
enum class MlsSignatureError : u8 {
    none,
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
// The digest buffer holds at least 32 bytes. A hash failure preserves the output.
KdfResult mls_signature_hash(KdfSha256Workspace& workspace, const ConstSlice<u8>& label,
                             const ConstSlice<u8>& a, const ConstSlice<u8>& b,
                             const Slice<u8>& digest) noexcept;
KdfResult mls_reference_hash(KdfSha256Workspace& workspace, const ConstSlice<u8>& label,
                             const ConstSlice<u8>& value, const Slice<u8>& digest) noexcept;
// Ciphersuite 2 ECDSA. Sign takes the maximum DER capacity (72 bytes). Only result.bytes is the signature,
// no outer MLS vector length. Caller authenticates the key and serialized content/label.
MlsSignatureResult mls_sign_with_label(KdfSha256Workspace& workspace, const P256Key& key,
                                       const ConstSlice<u8>& label, const ConstSlice<u8>& a,
                                       const ConstSlice<u8>& b, const Slice<u8>& signature) noexcept;
MlsSignatureResult mls_verify_with_label(KdfSha256Workspace& workspace, const P256Key& key,
                                         const ConstSlice<u8>& label, const ConstSlice<u8>& a,
                                         const ConstSlice<u8>& b, const ConstSlice<u8>& signature) noexcept;
} // namespace maicord
