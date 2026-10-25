#include "maicord/app/mls_signature.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/mls_encoding.hpp"
#include "byte_views.hpp"


namespace maicord {
namespace {
KdfResult validate(const KdfSha256Workspace& workspace, ConstSlice<u8> label, ConstSlice<u8> a,
                   ConstSlice<u8> b, ConstSlice<u8> destination, u32 prefix) noexcept {
    if (!workspace.storage.data || !workspace.storage.locked)
        return {0, 0, KdfError::not_initialized};
    if (label.count > 0x3fffffff - prefix || a.count > 0x3fffffff || b.count > 0x3fffffff - a.count)
        return {0, 0, KdfError::invalid_input};
    const ConstSlice<u8> spans[]{label, a, b, destination};
    for (auto span : spans)
        if (!byte_range_valid(span) || aliases_workspace(workspace, span))
            return {0, 0, KdfError::invalid_input};
    if (byte_ranges_overlap(destination, label) || byte_ranges_overlap(destination, a) ||
        byte_ranges_overlap(destination, b))
        return {0, 0, KdfError::invalid_input};
    return {};
}
KdfResult framed_hash(KdfSha256Workspace& workspace, ConstSlice<u8> label, ConstSlice<u8> a,
                      ConstSlice<u8> b, Slice<u8> output, bool sign) noexcept {
    auto result = validate(workspace, label, a, b, {output.data, output.count}, sign ? 8 : 0);
    if (!result.ok())
        return result;
    if (output.count < 32)
        return {0, 0, KdfError::output_capacity};
    static constexpr u8 prefix[]{'M', 'L', 'S', ' ', '1', '.', '0', ' '};
    u8 label_length[4], content_length[4], digest[32];
    const u32 nl = mls_vector_length(label.count + (sign ? 8 : 0), label_length);
    const u32 nc = mls_vector_length(a.count + b.count, content_length);
    const ConstSlice<u8> parts[]{
        {label_length, nl}, {prefix, sign ? 8u : 0u}, label, {content_length, nc}, a, b};
    result = sha256(workspace, {parts, 6}, {digest, 32});
    if (result.ok())
        copy_bytes(output.data, digest, 32);
    secure_clear_bytes(digest, sizeof(digest));
    return result;
}
MlsSignatureResult hash_error(KdfResult result) noexcept {
    return {0, result.native_status,
            result.error == KdfError::not_initialized ? MlsSignatureError::not_initialized
            : result.error == KdfError::invalid_input ? MlsSignatureError::invalid_input
                                                      : MlsSignatureError::hash_failure};
}
MlsSignatureResult preflight(const KdfSha256Workspace& workspace, const P256Key& key, ConstSlice<u8> label,
                             ConstSlice<u8> a, ConstSlice<u8> b, ConstSlice<u8> signature,
                             bool sign) noexcept {
    auto checked = validate(workspace, label, a, b, signature, 8);
    if (!checked.ok())
        return hash_error(checked);
    const ConstSlice<u8> spans[]{label,
                                 a,
                                 b,
                                 signature,
                                 {workspace.storage.data, workspace.storage.capacity},
                                 object_bytes(workspace)};
    for (auto span : spans)
        if (byte_ranges_overlap(span, object_bytes(key)))
            return {0, 0, MlsSignatureError::invalid_input};
    if (!key.handle)
        return {0, 0, MlsSignatureError::not_initialized};
    if (key.kind != P256Kind::ecdsa || (sign && !key.has_private))
        return {0, 0, MlsSignatureError::key_failure};
    if (sign && signature.count < kP256DerSignatureMaximum)
        return {0, 0, MlsSignatureError::output_capacity};
    return {};
}
} // namespace
KdfResult mls_signature_hash(KdfSha256Workspace& workspace, ConstSlice<u8> label, ConstSlice<u8> a,
                             ConstSlice<u8> b, Slice<u8> digest) noexcept {
    return framed_hash(workspace, label, a, b, digest, true);
}
KdfResult mls_reference_hash(KdfSha256Workspace& workspace, ConstSlice<u8> label, ConstSlice<u8> value,
                             Slice<u8> digest) noexcept {
    return framed_hash(workspace, label, value, {}, digest, false);
}
MlsSignatureResult mls_sign_with_label(KdfSha256Workspace& workspace, const P256Key& key,
                                       ConstSlice<u8> label, ConstSlice<u8> a, ConstSlice<u8> b,
                                       Slice<u8> signature) noexcept {
    auto result = preflight(workspace, key, label, a, b, {signature.data, signature.count}, true);
    if (!result.ok())
        return result;
    u8 digest[32], raw[64];
    const auto hash = mls_signature_hash(workspace, label, a, b, {digest, 32});
    if (!hash.ok())
        result = hash_error(hash);
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
                                         ConstSlice<u8> label, ConstSlice<u8> a, ConstSlice<u8> b,
                                         ConstSlice<u8> signature) noexcept {
    auto result = preflight(workspace, key, label, a, b, signature, false);
    if (!result.ok())
        return result;
    u8 digest[32], raw[64];
    const auto decoded = p256_signature_from_der(signature, {raw, 64});
    if (!decoded.ok())
        result = {0, 0, MlsSignatureError::malformed_signature};
    else {
        const auto hash = mls_signature_hash(workspace, label, a, b, {digest, 32});
        if (!hash.ok())
            result = hash_error(hash);
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
