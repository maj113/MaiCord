#pragma once
#include "maicord/app/mls_signature.hpp"
#include "maicord/protocol/mls_objects.hpp"

namespace maicord {
enum class MlsPackageCryptoError : u8 {
    none,
    invalid_input,
    not_initialized,
    decode_failure,
    unsupported_suite,
    invalid_leaf_source,
    key_reuse,
    key_failure,
    invalid_signature,
    hash_failure,
    invalid_credential,
    identity_mismatch,
    invalid_capabilities,
    invalid_extensions
};
struct MlsPackageCryptoResult {
    u32 bytes;
    i32 native_status;
    MlsDecodeResult wire;
    MlsPackageCryptoError error;
    constexpr bool ok() const noexcept {
        return error == MlsPackageCryptoError::none;
    }
};
struct MlsSignedKeyPackage {
    MlsKeyPackageView view;
    u8 reference[32];
};
// Raw KeyPackage, MLS 1.0 / ciphersuite 2. Validates all three public
// keys, distinct init/leaf encryption keys, key_package leaf source, both
// labeled signatures, and computes the complete KeyPackageRef. Output is
// staged; every failure preserves the previous view/reference. No allocation
// outside native CNG key operations and the worker KDF workspace.
// This is CRYPTOGRAPHIC verification only. Caller MUST still validate the
// credential/identity binding, lifetime, extension/capability semantics, group
// requirements, outer MLSMessage version and key uniqueness/reuse policy.
// Returned views borrow the input; neither input nor output may overlap
// the workspace, and the output must not overlap input.
MlsPackageCryptoResult mls_key_package_check_crypto(KdfSha256Workspace& workspace, ConstSlice<u8> input,
                                                    MlsSignedKeyPackage* output) noexcept;
// DAVE receiver profile plus the complete crypto check above, decoding once.
// expected_user_id must be nonzero and come from the authenticated call/proposal
// context, never from this package's own credential. Checks Basic/eight-byte
// identity, version/suite/credential capabilities, forbidden default capability
// listings and unique, advertised extensions. Unknown extension contents are
// ignored. DAVE members do NOT validate package lifetime. These checks do not
// authenticate the delivery/proposal or bind a signing key to an external
// identity service; group-required capabilities, credential compatibility,
// key uniqueness and outer message version remain caller checks.
// Cheap profile failures occur before public-key imports/signature verification.
// Same borrowed-view, alias and atomic-output contract as the crypto-only API.
MlsPackageCryptoResult mls_key_package_check_dave(KdfSha256Workspace& workspace, ConstSlice<u8> input,
                                                  u64 expected_user_id,
                                                  MlsSignedKeyPackage* output) noexcept;
// The LeafNode half of the DAVE profile, shared with ratchet-tree leaves:
// Basic eight-byte identity (big-endian snowflake, returned), required
// version/suite/credential capabilities, no default listings and advertised,
// unique leaf extensions. No signature or key validation.
MlsPackageCryptoError mls_leaf_check_dave(const MlsLeafView& leaf, u64* user_id) noexcept;
} // namespace maicord
