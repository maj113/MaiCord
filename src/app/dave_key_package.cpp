#include "maicord/app/dave_key_package.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/protocol/mls_encoding.hpp"

#include <Windows.h>

namespace maicord {
namespace {
template <typename T> ConstSlice<u8> bytes(const T& value) noexcept {
    return {reinterpret_cast<const u8*>(&value), sizeof(value)};
}
template <u32 N> ConstSlice<u8> label(const char (&value)[N]) noexcept {
    return {reinterpret_cast<const u8*>(value), N - 1};
}
DavePackageResult produce(KdfSha256Workspace& workspace, const P256Key& signing, u64 user_id,
                          DaveKeyPackage& candidate) noexcept {
    u8 init[65], leaf[65], signature_key[65], identity[8];
    auto key = p256_generate(&candidate.init_key, P256Kind::ecdh);
    if (key.ok())
        key = p256_generate(&candidate.leaf_key, P256Kind::ecdh);
    if (key.ok())
        key = p256_export_public(candidate.init_key, {init, 65});
    if (key.ok())
        key = p256_export_public(candidate.leaf_key, {leaf, 65});
    if (key.ok())
        key = p256_export_public(signing, {signature_key, 65});
    if (!key.ok())
        return {0, key.native_status, DavePackageError::key_failure};
    if (bytes_equal(init, leaf, 65))
        return {0, 0, DavePackageError::key_reuse};
    for (u32 i = 0; i != 8; ++i)
        identity[7 - i] = static_cast<u8>(user_id >> (8 * i));
    const u8 one[]{0, 1}, two[]{0, 2};
    MlsKeyPackageView view{};
    view.version = 1;
    view.cipher_suite = 2;
    view.init_key = {init, 65};
    view.leaf.encryption_key = {leaf, 65};
    view.leaf.signature_key = {signature_key, 65};
    view.leaf.credential = {1, {identity, 8}, {}};
    view.leaf.capabilities.versions = {one, 2};
    view.leaf.capabilities.cipher_suites = {two, 2};
    view.leaf.capabilities.credentials = {one, 2};
    view.leaf.source = MlsLeafSource::key_package;
    view.leaf.not_after = ~u64{0};
    auto encoded = mls_encode_tbs(view.leaf, {candidate.wire, sizeof(candidate.wire)});
    if (!encoded.ok())
        return {0, 0, DavePackageError::encode_failure};
    u8 signature[72];
    auto signed_result = mls_sign_with_label(workspace, signing, label("LeafNodeTBS"),
                                             {candidate.wire, encoded.bytes}, {}, {signature, 72});
    if (!signed_result.ok())
        return {0, signed_result.native_status,
                signed_result.error == MlsSignatureError::hash_failure
                    ? DavePackageError::hash_failure
                    : DavePackageError::signature_failure};
    view.leaf.signature = {signature, signed_result.bytes};
    encoded = mls_encode_tbs(view, {candidate.wire, sizeof(candidate.wire)});
    if (!encoded.ok())
        return {0, 0, DavePackageError::encode_failure};
    // The leaf signature is already in the package TBS. Reuse its temporary
    // buffer for the outer signature rather than retaining a second scratch.
    signed_result = mls_sign_with_label(workspace, signing, label("KeyPackageTBS"),
                                        {candidate.wire, encoded.bytes}, {}, {signature, 72});
    if (!signed_result.ok())
        return {0, signed_result.native_status,
                signed_result.error == MlsSignatureError::hash_failure
                    ? DavePackageError::hash_failure
                    : DavePackageError::signature_failure};
    u8 prefix[4];
    const u32 prefix_bytes = mls_vector_length(signed_result.bytes, prefix);
    if (!prefix_bytes || encoded.bytes > sizeof(candidate.wire) ||
        prefix_bytes + signed_result.bytes > sizeof(candidate.wire) - encoded.bytes)
        return {0, 0, DavePackageError::encode_failure};
    copy_bytes(candidate.wire + encoded.bytes, prefix, prefix_bytes);
    copy_bytes(candidate.wire + encoded.bytes + prefix_bytes, signature, signed_result.bytes);
    candidate.bytes = encoded.bytes + prefix_bytes + signed_result.bytes;
    const auto hashed = mls_reference_hash(workspace, label("MLS 1.0 KeyPackage Reference"),
                                           {candidate.wire, candidate.bytes}, {candidate.reference, 32});
    if (!hashed.ok())
        return {0, hashed.native_status, DavePackageError::hash_failure};
    candidate.user_id = user_id;
    return {candidate.bytes, 0, DavePackageError::none};
}
} // namespace
void dave_key_package_release(DaveKeyPackage* package) noexcept {
    if (!package)
        return;
    p256_release(&package->init_key);
    p256_release(&package->leaf_key);
    SecureZeroMemory(package, sizeof(*package));
}
DavePackageResult dave_key_package_create(KdfSha256Workspace& workspace, const P256Key& signing,
                                          u64 user_id, DaveKeyPackage* output) noexcept {
    if (!output || !user_id || byte_ranges_overlap(bytes(*output), bytes(signing)) ||
        byte_ranges_overlap(bytes(*output), bytes(workspace)) ||
        byte_ranges_overlap(bytes(signing), bytes(workspace)))
        return {0, 0, DavePackageError::invalid_input};
    if (!workspace.storage.data || !workspace.storage.locked)
        return {0, 0, DavePackageError::not_initialized};
    const ConstSlice<u8> storage{workspace.storage.data, workspace.storage.capacity};
    if (byte_ranges_overlap(bytes(*output), storage) || byte_ranges_overlap(bytes(signing), storage))
        return {0, 0, DavePackageError::invalid_input};
    if (output->init_key.handle || output->leaf_key.handle || output->bytes || output->user_id)
        return {0, 0, DavePackageError::already_initialized};
    if (!signing.handle || !signing.has_private || signing.kind != P256Kind::ecdsa)
        return {0, 0, DavePackageError::invalid_input};
    DaveKeyPackage candidate{};
    const auto result = produce(workspace, signing, user_id, candidate);
    if (result.ok()) {
        *output = candidate;
        // Transfer handles, then erase only the local descriptor/public bytes.
        SecureZeroMemory(&candidate, sizeof(candidate));
    } else
        dave_key_package_release(&candidate);
    return result;
}
} // namespace maicord
