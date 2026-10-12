#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
enum class MlsWireError : u8 {
    none,
    invalid_input,
    truncated,
    noncanonical_length,
    invalid_length_prefix,
    malformed,
    unsupported_credential,
    invalid_leaf_source,
    trailing_data,
    output_capacity,
    size_overflow,
    unsupported_version,
    unsupported_wire_format,
    version_mismatch,
    invalid_optional,
    invalid_psk_type,
    invalid_psk_usage
};
struct MlsDecodeResult {
    u32 bytes, error_offset;
    MlsWireError error;
    constexpr bool ok() const noexcept {
        return error == MlsWireError::none;
    }
};
// These are unverified views into the wire bytes. Decode only checks canonical
// framing/union tags/vector element boundaries, not cryptography, capabilities,
// identity, timestamps, extension semantics, duplicate extensions or group
// authorization. The input must stay alive and unchanged while any view is used.
struct MlsCredentialView {
    u16 type;
    ConstSlice<u8> identity;     // Basic credential, opaque application identity.
    ConstSlice<u8> certificates; // X.509: sequence of <V> certificate vectors.
};
struct MlsCapabilitiesView {
    // Network-order u16 sequences, not host-aligned integer pointers.
    ConstSlice<u8> versions, cipher_suites, extensions, proposals, credentials;
};
enum class MlsLeafSource : u8 {
    key_package = 1,
    update = 2,
    commit = 3
};
struct MlsLeafView {
    ConstSlice<u8> encryption_key, signature_key;
    MlsCredentialView credential;
    MlsCapabilitiesView capabilities;
    MlsLeafSource source;
    u64 not_before, not_after;
    ConstSlice<u8> parent_hash, extensions, signature;
    ConstSlice<u8> serialized, tbs; // tbs excludes signature and external binding.
};
struct MlsKeyPackageView {
    u16 version, cipher_suite;
    ConstSlice<u8> init_key;
    MlsLeafView leaf;
    ConstSlice<u8> extensions, signature, serialized, tbs;
};
struct MlsGroupContextView {
    u16 version, cipher_suite;
    ConstSlice<u8> group_id;
    u64 epoch;
    ConstSlice<u8> tree_hash, confirmed_transcript_hash, extensions, serialized;
};
struct MlsEncryptedGroupSecretsView {
    ConstSlice<u8> new_member, kem_output, ciphertext, serialized;
};
struct MlsWelcomeView {
    u16 cipher_suite;
    // Body of the EncryptedGroupSecrets<V> list, not a list of host pointers.
    ConstSlice<u8> secrets, encrypted_group_info, serialized;
};
struct MlsGroupInfoView {
    MlsGroupContextView context;
    ConstSlice<u8> extensions, confirmation_tag;
    u32 signer;
    ConstSlice<u8> signature, serialized, tbs;
};
struct MlsPreSharedKeyIdView {
    u8 type, usage; // External=1; resumption=2 with application/reinit/branch=1/2/3.
    ConstSlice<u8> id, group_id;
    u64 epoch;
    ConstSlice<u8> nonce, serialized;
};
struct MlsGroupSecretsView {
    ConstSlice<u8> joiner_secret;
    bool has_path_secret;
    ConstSlice<u8> path_secret, psks, serialized;
};
enum class MlsWireFormat : u16 {
    public_message = 1,
    private_message = 2,
    welcome = 3,
    group_info = 4,
    key_package = 5
};
struct MlsMessageView {
    u16 version;
    MlsWireFormat format;
    union {
        MlsWelcomeView welcome;
        MlsGroupInfoView group_info;
        MlsKeyPackageView key_package;
    };
    ConstSlice<u8> serialized;
};
// Consume exactly one object prefix; optional trailing bytes belong to a parent
// message. bytes is zero on failure and output is unchanged. Output may replace
// a previous view, but must not overlap any input bytes. No allocation or fixed
// object-size cap beyond the supplied input and MLS's 30-bit vector lengths.
MlsDecodeResult mls_leaf_decode_prefix(ConstSlice<u8> input, MlsLeafView* output) noexcept;
MlsDecodeResult mls_key_package_decode_prefix(ConstSlice<u8> input, MlsKeyPackageView* output) noexcept;
MlsDecodeResult mls_group_context_decode_prefix(ConstSlice<u8> input, MlsGroupContextView* output) noexcept;
MlsDecodeResult mls_leaf_decode(ConstSlice<u8> input, MlsLeafView* output) noexcept;
MlsDecodeResult mls_key_package_decode(ConstSlice<u8> input, MlsKeyPackageView* output) noexcept;
MlsDecodeResult mls_group_context_decode(ConstSlice<u8> input, MlsGroupContextView* output) noexcept;
MlsDecodeResult mls_encrypted_group_secrets_decode_prefix(ConstSlice<u8> input,
                                                          MlsEncryptedGroupSecretsView* output) noexcept;
MlsDecodeResult mls_encrypted_group_secrets_decode(ConstSlice<u8> input,
                                                   MlsEncryptedGroupSecretsView* output) noexcept;
MlsDecodeResult mls_welcome_decode_prefix(ConstSlice<u8> input, MlsWelcomeView* output) noexcept;
MlsDecodeResult mls_welcome_decode(ConstSlice<u8> input, MlsWelcomeView* output) noexcept;
MlsDecodeResult mls_group_info_decode_prefix(ConstSlice<u8> input, MlsGroupInfoView* output) noexcept;
MlsDecodeResult mls_group_info_decode(ConstSlice<u8> input, MlsGroupInfoView* output) noexcept;
// Implemented MLSMessage arms: Welcome, GroupInfo and KeyPackage. Public/private
// handshake/application codecs are not implemented here and fail.
// Enforces MLS 1.0 and matching nested versions; no crypto/group validation.
MlsDecodeResult mls_message_decode_prefix(ConstSlice<u8> input, MlsMessageView* output) noexcept;
MlsDecodeResult mls_message_decode(ConstSlice<u8> input, MlsMessageView* output) noexcept;
// Sensitive plaintext views: the backing storage must be protected and erased
// by the caller. Framing only; no PSK lookup, derivation or group joining.
MlsDecodeResult mls_psk_id_decode_prefix(ConstSlice<u8> input, MlsPreSharedKeyIdView* output) noexcept;
MlsDecodeResult mls_psk_id_decode(ConstSlice<u8> input, MlsPreSharedKeyIdView* output) noexcept;
MlsDecodeResult mls_group_secrets_decode_prefix(ConstSlice<u8> input, MlsGroupSecretsView* output) noexcept;
MlsDecodeResult mls_group_secrets_decode(ConstSlice<u8> input, MlsGroupSecretsView* output) noexcept;
struct MlsEncodeResult {
    u32 bytes;
    MlsWireError error;
    constexpr bool ok() const noexcept {
        return error == MlsWireError::none;
    }
};
// Serializes the fields, ignoring any cached serialized/tbs views. Only structure
// is checked; signatures must already match the supplied fields. Inactive
// credential/source arms are ignored. Size queries validate without allocating.
// Encode leaves output unchanged on failure and keeps its unused tail.
// Output must not overlap the input object or any active field, and the input
// must not change during the call.
MlsEncodeResult mls_encoded_size(const MlsLeafView& input) noexcept;
MlsEncodeResult mls_encoded_size(const MlsKeyPackageView& input) noexcept;
MlsEncodeResult mls_encoded_size(const MlsGroupContextView& input) noexcept;
MlsEncodeResult mls_encode(const MlsLeafView& input, Slice<u8> output) noexcept;
MlsEncodeResult mls_encode(const MlsKeyPackageView& input, Slice<u8> output) noexcept;
MlsEncodeResult mls_encode(const MlsGroupContextView& input, Slice<u8> output) noexcept;
// Same failure/aliasing rules, excluding only the object's own signature vector.
// A package TBS includes its signed leaf. Leaf update/commit callers must append
// their external group binding before signing. The signature field is ignored.
MlsEncodeResult mls_encode_tbs(const MlsLeafView& input, Slice<u8> output) noexcept;
MlsEncodeResult mls_encode_tbs(const MlsKeyPackageView& input, Slice<u8> output) noexcept;
MlsEncodeResult mls_encoded_size(const MlsEncryptedGroupSecretsView& input) noexcept;
MlsEncodeResult mls_encoded_size(const MlsWelcomeView& input) noexcept;
MlsEncodeResult mls_encoded_size(const MlsGroupInfoView& input) noexcept;
MlsEncodeResult mls_encoded_size(const MlsMessageView& input) noexcept;
MlsEncodeResult mls_encode(const MlsEncryptedGroupSecretsView& input, Slice<u8> output) noexcept;
MlsEncodeResult mls_encode(const MlsWelcomeView& input, Slice<u8> output) noexcept;
MlsEncodeResult mls_encode(const MlsGroupInfoView& input, Slice<u8> output) noexcept;
MlsEncodeResult mls_encode(const MlsMessageView& input, Slice<u8> output) noexcept;
MlsEncodeResult mls_encode_tbs(const MlsGroupInfoView& input, Slice<u8> output) noexcept;
} // namespace maicord
