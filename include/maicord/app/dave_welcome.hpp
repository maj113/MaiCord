#pragma once
#include "maicord/app/mls_hpke.hpp"
#include "maicord/protocol/mls_objects.hpp"

namespace maicord {
enum class DaveWelcomeError : u8 {
    none,
    invalid_input,
    not_initialized,
    already_initialized,
    decode_failure,
    unsupported_suite,
    missing_reference,
    duplicate_reference,
    malformed_ciphertext,
    storage_failure,
    hpke_failure,
    hpke_authentication_failed,
    invalid_group_secrets,
    psk_unavailable,
    kdf_failure,
    group_info_authentication_failed,
    group_info_failure
};
struct DaveWelcomeResult {
    u32 bytes; // Decrypted GroupInfo length on success, zero on failure.
    i32 native_status;
    u32 native_error;
    MlsDecodeResult wire;
    DaveWelcomeError error;
    constexpr bool ok() const noexcept {
        return error == DaveWelcomeError::none;
    }
};
// DECRYPTED, NOT GROUP-AUTHENTICATED. Zero initialize; worker-exclusive; no
// copies. Both plaintext objects reside in one locked allocation held here.
// Views borrow that allocation, never the input Welcome. Release erases it.
// Do not use secrets for media or consume pending KeyPackage keys until the
// ratchet tree, GroupInfo signer, confirmation and call/group policy are checked.
struct DaveWelcomeCandidate {
    SecureBuffer storage;
    MlsGroupSecretsView secrets;
    MlsGroupInfoView group_info;
};
void dave_welcome_release(DaveWelcomeCandidate* candidate) noexcept;
// Raw ciphersuite-2 Welcome. Reference must be the 32-byte hash of the
// caller's pending KeyPackage; init_key is its private ECDH key.
// Selects a unique matching recipient, authenticates HPKE (bound to encrypted
// GroupInfo), derives the no-PSK welcome key/nonce and authenticates GroupInfo
// AEAD, then checks plaintext framing/version/suite. Nonempty PSKs fail with
// psk_unavailable: there is no PSK store/resolver, and they are never ignored.
// Requires an empty output. Failure preserves output, wipes/frees local
// candidate plaintext and releases crypto handles. Does not consume init_key.
// Input/reference/output/key buffers must not overlap the workspace; output must
// not overlap any input or key buffer. Reference must not borrow the input Welcome.
DaveWelcomeResult dave_welcome_decrypt(KdfSha256Workspace& workspace, const P256Key& init_key,
                                       ConstSlice<u8> reference, ConstSlice<u8> input,
                                       DaveWelcomeCandidate* output) noexcept;
} // namespace maicord
