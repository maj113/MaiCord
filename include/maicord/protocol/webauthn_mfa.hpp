#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/base64url.hpp"

namespace maicord {

inline constexpr u32 kWebAuthnMfaMaximumChallengeBytes = 1024;
inline constexpr u32 kWebAuthnMfaMaximumRpIdBytes = 253;
inline constexpr u32 kWebAuthnMfaMaximumAllowedCredentials = 32;
inline constexpr u32 kWebAuthnMfaMaximumCredentialIdBytes = 1024;
inline constexpr u32 kWebAuthnMfaCredentialIdStorageBytes = 12 * 1024;
inline constexpr u32 kWebAuthnMfaMaximumClientDataBytes = 2048;

enum WebAuthnTransport : u32 {
    webauthn_transport_none = 0,
    webauthn_transport_usb = 1u << 0,
    webauthn_transport_nfc = 1u << 1,
    webauthn_transport_ble = 1u << 2,
    webauthn_transport_internal = 1u << 4,
    webauthn_transport_hybrid = 1u << 5,
};

enum class WebAuthnUserVerification : u8 {
    preferred,
    required,
    discouraged,
};

struct WebAuthnAllowedCredential {
    u32 id_offset;
    u32 id_bytes;
    u32 transports;
};

struct WebAuthnMfaRequestOptions {
    char rp_id[kWebAuthnMfaMaximumRpIdBytes + 1];
    u8 challenge[kWebAuthnMfaMaximumChallengeBytes];
    u8 credential_ids[kWebAuthnMfaCredentialIdStorageBytes];
    WebAuthnAllowedCredential
        credentials[kWebAuthnMfaMaximumAllowedCredentials];
    u32 challenge_bytes;
    u32 credential_id_bytes;
    u32 credential_count;
    u32 timeout_ms;
    WebAuthnUserVerification user_verification;
    bool requests_uvm;
    bool conditional;
};

enum class WebAuthnMfaError : u8 {
    none,
    invalid_input,
    scan_failed,
    invalid_json,
    duplicate_field,
    missing_required_field,
    invalid_field,
    relying_party_mismatch,
    value_too_large,
    too_many_credentials,
    base64,
    output_too_small,
};

struct WebAuthnMfaResult {
    u32 bytes;
    u32 error_offset;
    WebAuthnMfaError error;
    JsonScanError scan_error;
    JsonReadError read_error;
    Base64UrlError base64_error;

    constexpr bool ok() const noexcept {
        return error == WebAuthnMfaError::none;
    }
};

enum class WebAuthnAuthenticatorAttachment : u8 {
    unknown,
    platform,
    cross_platform,
};

struct WebAuthnMfaAssertionView {
    ConstSlice<u8> credential_id;
    ConstSlice<u8> client_data_json;
    ConstSlice<u8> authenticator_data;
    ConstSlice<u8> signature;
    ConstSlice<u8> user_handle;
    WebAuthnAuthenticatorAttachment attachment;
};

// The API returns the WebAuthn challenge as decoded JSON of the form
// {"publicKey": PublicKeyCredentialRequestOptions}. Only discord.com is
// accepted as the relying party; the challenge is never allowed to retarget a
// Windows credential ceremony.
WebAuthnMfaResult webauthn_mfa_decode_request(
    Utf8View json, Slice<u32> structural_scratch, u32 maximum_depth,
    WebAuthnMfaRequestOptions* output) noexcept;

// Produces the CollectedClientData bytes hashed by the Windows WebAuthn API.
WebAuthnMfaResult webauthn_mfa_write_client_data(
    const WebAuthnMfaRequestOptions& request,
    Slice<u8> output) noexcept;

// Produces the stringified PublicKeyCredential.toJSON-compatible value that
// the API accepts as the MFA `code`. Binary members use canonical unpadded
// base64url and the write is transactional.
WebAuthnMfaResult webauthn_mfa_write_assertion(
    const WebAuthnMfaAssertionView& assertion,
    Slice<u8> output) noexcept;

} // namespace maicord
