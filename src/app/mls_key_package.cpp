#include "maicord/app/mls_key_package.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/protocol/mls_reader.hpp"

namespace maicord {
namespace {
template <typename T> ConstSlice<u8> bytes(const T& value) noexcept {
    return {reinterpret_cast<const u8*>(&value), sizeof(value)};
}
template <u32 N> ConstSlice<u8> label(const char (&value)[N]) noexcept {
    return {reinterpret_cast<const u8*>(value), N - 1};
}
MlsPackageCryptoResult signature(MlsSignatureResult result) noexcept {
    return {0,
            result.native_status,
            {},
            result.ok()                                       ? MlsPackageCryptoError::none
            : result.error == MlsSignatureError::hash_failure ? MlsPackageCryptoError::hash_failure
            : result.error == MlsSignatureError::invalid_signature ||
                    result.error == MlsSignatureError::malformed_signature
                ? MlsPackageCryptoError::invalid_signature
                : MlsPackageCryptoError::key_failure};
}
u16 integer16(const u8* data) noexcept {
    return static_cast<u16>((static_cast<u16>(data[0]) << 8) | data[1]);
}
bool includes(ConstSlice<u8> values, u16 value) noexcept {
    for (u32 i = 0; i < values.count; i += 2)
        if (integer16(values.data + i) == value)
            return true;
    return false;
}
bool lists_defaults(ConstSlice<u8> values, u16 last_default) noexcept {
    for (u32 i = 0; i < values.count; i += 2) {
        const u16 value = integer16(values.data + i);
        if (value >= 1 && value <= last_default)
            return true;
    }
    return false;
}
// One bit per wire codepoint prevents repeated capability searches or quadratic
// duplicate scans. Only nonempty extension lists take this 8 KiB scratch.
// Consuming an allowed bit checks advertisement and uniqueness in one operation.
__declspec(noinline) bool extensions_valid(ConstSlice<u8> data, ConstSlice<u8> capabilities,
                                           bool leaf) noexcept {
    u64 allowed[1024]{};
    for (u32 i = 0; i < capabilities.count; i += 2) {
        const u16 id = integer16(capabilities.data + i);
        allowed[id >> 6] |= u64{1} << (id & 63);
    }
    if (leaf)
        allowed[0] |= u64{1} << 1; // application_id is implicitly supported.
    MlsReader reader{data};
    while (reader.used < data.count) {
        const auto id = static_cast<u16>(reader.integer(2));
        const auto value = reader.vector();
        if (reader.error != MlsWireError::none)
            return false;
        // RFC 9420 default extension locations: only application_id is valid
        // in a leaf; the remaining defaults belong to GroupInfo/GroupContext.
        if (id >= 1 && id <= 5 && !(leaf && id == 1))
            return false;
        const u64 bit = u64{1} << (id & 63);
        if (!(allowed[id >> 6] & bit))
            return false;
        allowed[id >> 6] &= ~bit;
        if (id == 1) {
            MlsReader application{value};
            application.vector();
            if (!application.result().ok() || application.used != value.count)
                return false;
        }
    }
    return true;
}
MlsPackageCryptoError dave_profile(const MlsKeyPackageView& view, u64 expected_user_id) noexcept {
    u64 user_id = 0;
    const auto leaf = mls_leaf_check_dave(view.leaf, &user_id);
    if (leaf != MlsPackageCryptoError::none)
        return leaf;
    if (user_id != expected_user_id)
        return MlsPackageCryptoError::identity_mismatch;
    if (view.extensions.count &&
        !extensions_valid(view.extensions, view.leaf.capabilities.extensions, false))
        return MlsPackageCryptoError::invalid_extensions;
    return MlsPackageCryptoError::none;
}
MlsPackageCryptoResult check_package(KdfSha256Workspace& workspace, ConstSlice<u8> input,
                                     MlsSignedKeyPackage* output, u64 expected_user_id) noexcept {
    if (!output || !byte_range_valid(input) || byte_ranges_overlap(input, bytes(*output)))
        return {0, 0, {}, MlsPackageCryptoError::invalid_input};
    if (!workspace.storage.data || !workspace.storage.locked)
        return {0, 0, {}, MlsPackageCryptoError::not_initialized};
    const ConstSlice<u8> spans[]{input, bytes(*output)};
    for (auto span : spans)
        if (byte_ranges_overlap(span, bytes(workspace)) ||
            byte_ranges_overlap(span, {workspace.storage.data, workspace.storage.capacity}))
            return {0, 0, {}, MlsPackageCryptoError::invalid_input};
    MlsSignedKeyPackage candidate{};
    const auto wire = mls_key_package_decode(input, &candidate.view);
    if (!wire.ok())
        return {0, 0, wire, MlsPackageCryptoError::decode_failure};
    auto& view = candidate.view;
    if (view.version != 1 || view.cipher_suite != 2)
        return {0, 0, {}, MlsPackageCryptoError::unsupported_suite};
    if (view.leaf.source != MlsLeafSource::key_package)
        return {0, 0, {}, MlsPackageCryptoError::invalid_leaf_source};
    if (expected_user_id) {
        const auto profile = dave_profile(view, expected_user_id);
        if (profile != MlsPackageCryptoError::none)
            return {0, 0, {}, profile};
    }
    if (view.init_key.count == view.leaf.encryption_key.count &&
        bytes_equal(view.init_key.data, view.leaf.encryption_key.data, view.init_key.count))
        return {0, 0, {}, MlsPackageCryptoError::key_reuse};
    P256Key init{}, leaf{}, signing{};
    auto imported = p256_import_public(&init, P256Kind::ecdh, view.init_key);
    if (imported.ok())
        imported = p256_import_public(&leaf, P256Kind::ecdh, view.leaf.encryption_key);
    if (imported.ok())
        imported = p256_import_public(&signing, P256Kind::ecdsa, view.leaf.signature_key);
    MlsPackageCryptoResult result{};
    if (!imported.ok())
        result = {0, imported.native_status, {}, MlsPackageCryptoError::key_failure};
    else
        result = signature(mls_verify_with_label(workspace, signing, label("LeafNodeTBS"), view.leaf.tbs,
                                                 {}, view.leaf.signature));
    if (result.ok())
        result = signature(mls_verify_with_label(workspace, signing, label("KeyPackageTBS"), view.tbs, {},
                                                 view.signature));
    if (result.ok()) {
        const auto hashed = mls_reference_hash(workspace, label("MLS 1.0 KeyPackage Reference"), input,
                                               {candidate.reference, 32});
        if (!hashed.ok())
            result = {0, hashed.native_status, {}, MlsPackageCryptoError::hash_failure};
    }
    p256_release(&init);
    p256_release(&leaf);
    p256_release(&signing);
    if (result.ok()) {
        *output = candidate;
        result.bytes = input.count;
        result.wire = wire;
    }
    return result;
}
} // namespace
MlsPackageCryptoError mls_leaf_check_dave(const MlsLeafView& leaf, u64* user_id) noexcept {
    if (leaf.credential.type != 1 || leaf.credential.identity.count != 8)
        return MlsPackageCryptoError::invalid_credential;
    u64 identity = 0;
    for (u32 i = 0; i != 8; ++i)
        identity = (identity << 8) | leaf.credential.identity[i];
    const auto& caps = leaf.capabilities;
    if (!includes(caps.versions, 1) || !includes(caps.cipher_suites, 2) || !includes(caps.credentials, 1) ||
        lists_defaults(caps.extensions, 5) || lists_defaults(caps.proposals, 7))
        return MlsPackageCryptoError::invalid_capabilities;
    if (leaf.extensions.count && !extensions_valid(leaf.extensions, caps.extensions, true))
        return MlsPackageCryptoError::invalid_extensions;
    if (user_id)
        *user_id = identity;
    return MlsPackageCryptoError::none;
}
MlsPackageCryptoResult mls_key_package_check_crypto(KdfSha256Workspace& workspace, ConstSlice<u8> input,
                                                    MlsSignedKeyPackage* output) noexcept {
    return check_package(workspace, input, output, 0);
}
MlsPackageCryptoResult mls_key_package_check_dave(KdfSha256Workspace& workspace, ConstSlice<u8> input,
                                                  u64 expected_user_id,
                                                  MlsSignedKeyPackage* output) noexcept {
    if (!expected_user_id)
        return {0, 0, {}, MlsPackageCryptoError::invalid_input};
    return check_package(workspace, input, output, expected_user_id);
}
} // namespace maicord
