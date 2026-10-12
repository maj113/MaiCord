#pragma once
#include "maicord/app/mls_key_package.hpp"

namespace maicord {
// Bound for the emitted DAVE profile only: 65-byte P256 points, eight-byte
// identity, fixed advertised capabilities, empty extensions, two DER signatures
// of at most 72 bytes. The generic MLS object codecs have no such fixed bound.
inline constexpr u32 dave_key_package_capacity = 394;
enum class DavePackageError : u8 {
    none,
    invalid_input,
    not_initialized,
    already_initialized,
    key_failure,
    key_reuse,
    encode_failure,
    signature_failure,
    hash_failure
};
struct DavePackageResult {
    u32 bytes;
    i32 native_status;
    DavePackageError error;
    constexpr bool ok() const noexcept {
        return error == DavePackageError::none;
    }
};
// Zero-initialize, worker-exclusive, no copies. Owns two CNG private keys needed
// to consume a Welcome; the session signing key has a separate lifetime.
// wire is a raw KeyPackage, without MLSMessage or voice opcode framing.
// Create requires an empty owner. Use a separate candidate for replacement,
// then transfer ownership only when the call state accepts it.
struct DaveKeyPackage {
    P256Key init_key, leaf_key;
    u64 user_id;
    u32 bytes;
    u8 wire[dave_key_package_capacity];
    u8 reference[32];
};
void dave_key_package_release(DaveKeyPackage* package) noexcept;
// Generates fresh encryption keys and both signatures, then commits the
// complete package and key handles atomically. All failures preserve output and
// release candidate handles. The signing key must be a live private ECDSA key.
// Inputs/output may not alias one another or the worker workspace/storage.
// No private scalars are exported; no network state is changed here.
DavePackageResult dave_key_package_create(KdfSha256Workspace& workspace, const P256Key& signing_key,
                                          u64 user_id, DaveKeyPackage* output) noexcept;
} // namespace maicord
