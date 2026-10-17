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
// The _prefix decoders consume exactly one object prefix; optional trailing
// bytes belong to a parent message. The others reject trailing bytes. bytes is
// zero on failure and output is unchanged. Output may replace a previous view,
// but must not overlap any input bytes. No allocation or fixed object-size cap
// beyond the supplied input and MLS's 30-bit vector lengths.
MlsDecodeResult mls_leaf_decode_prefix(ConstSlice<u8> input, MlsLeafView* output) noexcept;
MlsDecodeResult mls_key_package_decode_prefix(ConstSlice<u8> input, MlsKeyPackageView* output) noexcept;
MlsDecodeResult mls_group_context_decode_prefix(ConstSlice<u8> input, MlsGroupContextView* output) noexcept;
MlsDecodeResult mls_key_package_decode(ConstSlice<u8> input, MlsKeyPackageView* output) noexcept;
MlsDecodeResult mls_encrypted_group_secrets_decode_prefix(ConstSlice<u8> input,
                                                          MlsEncryptedGroupSecretsView* output) noexcept;
MlsDecodeResult mls_welcome_decode(ConstSlice<u8> input, MlsWelcomeView* output) noexcept;
MlsDecodeResult mls_group_info_decode(ConstSlice<u8> input, MlsGroupInfoView* output) noexcept;
// Sensitive plaintext views: the backing storage must be protected and erased
// by the caller. Framing only; no PSK lookup, derivation or group joining.
MlsDecodeResult mls_group_secrets_decode(ConstSlice<u8> input, MlsGroupSecretsView* output) noexcept;
struct MlsEncodeResult {
    u32 bytes;
    MlsWireError error;
    constexpr bool ok() const noexcept {
        return error == MlsWireError::none;
    }
};
// Serializes the fields up to, excluding, the object's own signature vector,
// ignoring cached serialized/tbs views and the signature field. Only structure
// is checked. Inactive credential/source arms are ignored. Writes only the
// returned prefix and leaves output unchanged on failure. Output must not
// overlap the input object or any active field, and the input must not change
// during the call. A package TBS includes its signed leaf. Leaf update/commit
// callers must append their external group binding before signing.
MlsEncodeResult mls_encode_tbs(const MlsLeafView& input, Slice<u8> output) noexcept;
MlsEncodeResult mls_encode_tbs(const MlsKeyPackageView& input, Slice<u8> output) noexcept;
} // namespace maicord
