#pragma once
#include "maicord/platform/secure_buffer.hpp"

namespace maicord {
enum class KdfError : u8 {
    none,
    invalid_input,
    not_initialized,
    output_capacity,
    storage,
    platform_failure
};
struct KdfResult {
    u32 bytes;
    i32 native_status;
    KdfError error;
    constexpr bool ok() const noexcept {
        return error == KdfError::none;
    }
};
// Shared by a single worker, not per sender/key. No copying or concurrent use.
// One locked CNG object buffer is reused, then erased after every operation;
// no secret or live hash handle survives an operation. Initialize/release once.
struct KdfSha256Workspace {
    SecureBuffer storage;
};
KdfResult kdf_sha256_initialize(KdfSha256Workspace* workspace) noexcept;
void kdf_sha256_release(KdfSha256Workspace* workspace) noexcept;
// Unkeyed SHA-256 using the same worker buffer and segmented input contract.
// Writes 32 bytes; not the same as HMAC with an empty key.
KdfResult sha256(KdfSha256Workspace& workspace, ConstSlice<ConstSlice<u8>> parts,
                 Slice<u8> output) noexcept;

// Segments avoid concatenation allocations. Inputs/segment descriptors/output
// cannot alias the workspace. Written output cannot alias any input or segment
// descriptor. Invalid inputs/capacity leave output unchanged; provider failures
// erase the attempted output prefix and report zero bytes. Empty HMAC keys are
// valid. HMAC/extract write 32 bytes and preserve excess destination capacity.
KdfResult hmac_sha256(KdfSha256Workspace& workspace, ConstSlice<u8> key, ConstSlice<ConstSlice<u8>> parts,
                      Slice<u8> output) noexcept;
// Prepend a protocol prefix without copying a caller's segment descriptors or
// payload. Same validation/erasure contract; prefix cannot alias output/storage.
KdfResult hmac_sha256_prefixed(KdfSha256Workspace& workspace, ConstSlice<u8> key, ConstSlice<u8> prefix,
                               ConstSlice<ConstSlice<u8>> parts, Slice<u8> output) noexcept;
KdfResult hkdf_sha256_extract(KdfSha256Workspace& workspace, ConstSlice<u8> salt,
                              ConstSlice<u8> input_key_material, Slice<u8> output) noexcept;
// Output.count is the requested length, from 0 through 255*32 bytes. Require a
// nonempty PRK. MLS's initial DAVE ratchet seed is 16 bytes, so this implements
// the Expand operation for that key length too; the caller decides where key material comes from.
KdfResult hkdf_sha256_expand(KdfSha256Workspace& workspace, ConstSlice<u8> prk,
                             ConstSlice<ConstSlice<u8>> info, Slice<u8> output) noexcept;
} // namespace maicord
