#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/encoding/base64url.hpp"

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
// Windows credential ceremony. The options are decoded in place: a refusal
// up to and including the relying party leaves `output` alone, a later one
// leaves it partly written (callers wipe it).
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

// Registering a security key (POST /users/@me/mfa/webauthn/credentials answers {"ticket", "challenge"}; `challenge` is a
// string holding {"publicKey": PublicKeyCredentialCreationOptions}). The relying party and the challenge, timeout and
// user verification of `request` read as in a sign-in; `request.credentials` lists the credentials the key must not
// already hold (excludeCredentials).
inline constexpr u32 kWebAuthnMfaMaximumUserIdBytes = 64;
inline constexpr u32 kWebAuthnMfaMaximumEntityNameBytes = 128;
inline constexpr u32 kWebAuthnMfaMaximumAlgorithms = 8;
// COSE algorithm identifiers (ES256, EdDSA, RS256) the ceremonies can ask a key for.
inline constexpr i32 kWebAuthnCoseEs256 = -7;
inline constexpr i32 kWebAuthnCoseEdDsa = -8;
inline constexpr i32 kWebAuthnCoseRs256 = -257;

enum class WebAuthnResidentKey : u8 {
    discouraged,
    preferred,
    required,
};

struct WebAuthnMfaCreationOptions {
    WebAuthnMfaRequestOptions request;
    char rp_name[kWebAuthnMfaMaximumEntityNameBytes + 1];
    u8 user_id[kWebAuthnMfaMaximumUserIdBytes];
    char user_name[kWebAuthnMfaMaximumEntityNameBytes + 1];
    char user_display_name[kWebAuthnMfaMaximumEntityNameBytes + 1];
    // The acceptable algorithms in the server's order (pubKeyCredParams); ES256 and RS256 when it lists none.
    i32 algorithms[kWebAuthnMfaMaximumAlgorithms];
    u32 algorithm_count;
    u32 user_id_bytes;
    WebAuthnResidentKey resident_key;
    // authenticatorSelection.authenticatorAttachment; unknown asks for no particular kind.
    WebAuthnAuthenticatorAttachment attachment;
    // attestation: "none" (also when absent) asks for no attestation statement.
    bool attestation_none;
};

// The creation challenge's decoded JSON; the relying party must be discord.com. Decoded in place, as a request is.
WebAuthnMfaResult webauthn_mfa_decode_creation(
    Utf8View json, Slice<u32> structural_scratch, u32 maximum_depth,
    WebAuthnMfaCreationOptions* output) noexcept;

// CollectedClientData of a creation ("webauthn.create"), hashed by the authenticator.
WebAuthnMfaResult webauthn_mfa_write_creation_client_data(
    const WebAuthnMfaCreationOptions& creation,
    Slice<u8> output) noexcept;

// An attestation object (CBOR {"fmt", "attStmt", "authData"}) from what a key returned: its format name (at most 23
// bytes), its attestation statement as a CBOR map (empty: an empty map) and its raw authenticator data.
struct WebAuthnMfaAttestationObjectView {
    Utf8View format;
    ConstSlice<u8> attestation_statement;
    ConstSlice<u8> authenticator_data;
};
WebAuthnMfaResult webauthn_mfa_write_attestation_object(
    const WebAuthnMfaAttestationObjectView& object,
    Slice<u8> output) noexcept;

// Zeroes the AAGUID of authenticator data that carries attested credential data (what an "none" attestation sends).
void webauthn_mfa_clear_aaguid(u8* authenticator_data, u32 bytes) noexcept;

// The COSE algorithm (entry 3) of the credential public key in authenticator data that carries attested credential
// data; 0 when it holds none or the key does not parse.
i32 webauthn_mfa_credential_algorithm(ConstSlice<u8> authenticator_data) noexcept;

struct WebAuthnMfaAttestationView {
    ConstSlice<u8> credential_id;
    ConstSlice<u8> client_data_json;
    ConstSlice<u8> attestation_object;
    ConstSlice<u8> authenticator_data;
    // COSE algorithm of the credential's public key.
    i32 public_key_algorithm;
    // WebAuthnTransport bits the key was reached by.
    u32 transports;
    WebAuthnAuthenticatorAttachment attachment;
};

// The stringified PublicKeyCredential.toJSON value of a creation, the `credential` of the registration request.
// Binary members use unpadded base64url; the size is checked before anything is written.
WebAuthnMfaResult webauthn_mfa_write_attestation(
    const WebAuthnMfaAttestationView& attestation,
    Slice<u8> output) noexcept;

} // namespace maicord
