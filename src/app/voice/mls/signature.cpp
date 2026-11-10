#include "maicord/app/voice/mls/signature.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/voice/mls_encoding.hpp"
#include "maicord/protocol/auth/p256_signature.hpp"


namespace maicord {
namespace {
// The framed content is hashed straight from the callers' fixed 32-byte (or larger) output buffers; the hash
// primitive checks the workspace and its spans itself.
KdfResult framed_hash(KdfSha256Workspace& workspace, const ConstSlice<u8>& label, const ConstSlice<u8>& a,
                      const ConstSlice<u8>& b, const Slice<u8>& output, bool sign) noexcept {
    static constexpr u8 prefix[]{'M', 'L', 'S', ' ', '1', '.', '0', ' '};
    u8 label_length[4], content_length[4], digest[32];
    const u32 nl = mls_vector_length(label.count + (sign ? 8 : 0), label_length);
    const u32 nc = mls_vector_length(a.count + b.count, content_length);
    const ConstSlice<u8> parts[]{
        {label_length, nl}, {prefix, sign ? 8u : 0u}, label, {content_length, nc}, a, b};
    const auto result = sha256(workspace, {parts, 6}, {digest, 32});
    if (result.ok())
        copy_bytes(output.data, digest, 32);
    secure_clear_bytes(digest, sizeof(digest));
    return result;
}
MlsSignatureResult hash_failure(const KdfResult& result) noexcept {
    return {0, result.native_status, MlsSignatureError::hash_failure};
}
} // namespace
KdfResult mls_signature_hash(KdfSha256Workspace& workspace, const ConstSlice<u8>& label,
                             const ConstSlice<u8>& a, const ConstSlice<u8>& b,
                             const Slice<u8>& digest) noexcept {
    return framed_hash(workspace, label, a, b, digest, true);
}
KdfResult mls_reference_hash(KdfSha256Workspace& workspace, const ConstSlice<u8>& label,
                             const ConstSlice<u8>& value, const Slice<u8>& digest) noexcept {
    return framed_hash(workspace, label, value, {}, digest, false);
}
MlsSignatureResult mls_sign_with_label(KdfSha256Workspace& workspace, const P256Key& key,
                                       const ConstSlice<u8>& label, const ConstSlice<u8>& a,
                                       const ConstSlice<u8>& b, const Slice<u8>& signature) noexcept {
    u8 digest[32], raw[64];
    MlsSignatureResult result;
    const auto hash = mls_signature_hash(workspace, label, a, b, {digest, 32});
    if (!hash.ok())
        result = hash_failure(hash);
    else {
        const auto signed_hash = p256_sign_digest(key, {digest, 32}, {raw, 64});
        if (!signed_hash.ok())
            result = {0, signed_hash.native_status, MlsSignatureError::key_failure};
        else {
            const auto encoded = p256_signature_to_der({raw, 64}, signature);
            result = {encoded.bytes, 0,
                      encoded.ok() ? MlsSignatureError::none : MlsSignatureError::key_failure};
        }
    }
    secure_clear_bytes(digest, sizeof(digest));
    secure_clear_bytes(raw, sizeof(raw));
    return result;
}
MlsSignatureResult mls_verify_with_label(KdfSha256Workspace& workspace, const P256Key& key,
                                         const ConstSlice<u8>& label, const ConstSlice<u8>& a,
                                         const ConstSlice<u8>& b,
                                         const ConstSlice<u8>& signature) noexcept {
    u8 digest[32], raw[64];
    MlsSignatureResult result;
    const auto decoded = p256_signature_from_der(signature, {raw, 64});
    if (!decoded.ok())
        result = {0, 0, MlsSignatureError::malformed_signature};
    else {
        const auto hash = mls_signature_hash(workspace, label, a, b, {digest, 32});
        if (!hash.ok())
            result = hash_failure(hash);
        else {
            const auto verified = p256_verify_digest(key, {digest, 32}, {raw, 64});
            result = {0, verified.native_status,
                      verified.ok() ? MlsSignatureError::none
                      : verified.error == P256Error::invalid_signature
                          ? MlsSignatureError::invalid_signature
                          : MlsSignatureError::key_failure};
        }
    }
    secure_clear_bytes(digest, sizeof(digest));
    secure_clear_bytes(raw, sizeof(raw));
    return result;
}
} // namespace maicord
