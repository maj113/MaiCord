#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/base64url.hpp"

namespace maicord {

inline constexpr u32 kRemoteAuthRsaBytes = 256;
inline constexpr u32 kRemoteAuthFingerprintBytes = 32;
inline constexpr u32 kRemoteAuthMaximumNonceBytes = 190;
inline constexpr u32 kRemoteAuthMaximumTicketBytes = 512;
inline constexpr u32 kRemoteAuthMaximumPacketBytes = 4096;
inline constexpr u32 kRemoteAuthMaximumUserPayloadBytes = 190;

enum class RemoteAuthPacketKind : u8 {
    none,
    hello,
    nonce_proof,
    pending_remote_init,
    pending_ticket,
    pending_login,
    cancel,
    heartbeat_ack,
};

struct RemoteAuthPacket {
    RemoteAuthPacketKind kind;
    u32 timeout_ms;
    u32 heartbeat_interval_ms;
    u32 text_bytes;
};

enum class RemoteAuthError : u8 {
    none,
    invalid_input,
    packet_too_large,
    scan_failed,
    invalid_json,
    duplicate_field,
    missing_field,
    invalid_field,
    unsupported_operation,
    output_too_small,
};

struct RemoteAuthDecodeResult {
    u32 error_offset;
    RemoteAuthError error;
    JsonScanError scan_error;
    JsonReadError read_error;
    Base64Error base64_error;

    constexpr bool ok() const noexcept {
        return error == RemoteAuthError::none;
    }
};

enum class RemoteAuthWriteError : u8 {
    none,
    invalid_input,
    output_too_small,
};

struct RemoteAuthWriteResult {
    u32 bytes;
    RemoteAuthWriteError error;

    constexpr bool ok() const noexcept {
        return error == RemoteAuthWriteError::none;
    }
};

enum class RemoteAuthUserPreviewError : u8 {
    none,
    invalid_input,
    payload_too_large,
    invalid_utf8,
    missing_field,
    invalid_user_id,
    invalid_discriminator,
    invalid_avatar,
    invalid_username,
};

struct RemoteAuthUserPreview {
    u64 user_id;
    Utf8View discriminator;
    Utf8View avatar;
    Utf8View username;
};

struct RemoteAuthUserPreviewResult {
    u32 error_offset;
    RemoteAuthUserPreviewError error;

    constexpr bool ok() const noexcept {
        return error == RemoteAuthUserPreviewError::none;
    }
};

// Decodes one complete Remote Auth gateway v2 server packet. text receives the
// single packet-specific string: encrypted nonce, fingerprint, encrypted user
// payload, or login ticket. The output packet and text remain untouched on
// failure.
RemoteAuthDecodeResult remote_auth_decode_server_packet(
    Utf8View body, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> text, RemoteAuthPacket* packet) noexcept;

// Parses the decrypted `id:discriminator:avatar:username` mobile preview.
// Returned views borrow the input and output remains untouched on failure.
RemoteAuthUserPreviewResult remote_auth_parse_user_preview(
    Utf8View payload, RemoteAuthUserPreview* preview) noexcept;

// Client gateway v2 packet writers. Inputs use the canonical wire encoding:
// padded standard Base64 for SPKI and unpadded Base64url for the decrypted
// nonce proof.
RemoteAuthWriteResult remote_auth_write_init(Utf8View encoded_public_key,
                                             Slice<char> output) noexcept;
RemoteAuthWriteResult remote_auth_write_nonce_proof(Utf8View nonce,
                                                    Slice<char> output) noexcept;
RemoteAuthWriteResult remote_auth_write_heartbeat(Slice<char> output) noexcept;

RemoteAuthWriteResult remote_auth_write_ticket_request(Utf8View ticket,
                                                       Slice<char> output) noexcept;
RemoteAuthWriteResult remote_auth_write_qr_url(Utf8View fingerprint,
                                               Slice<char> output) noexcept;
RemoteAuthDecodeResult remote_auth_decode_ticket_response(
    Utf8View body, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> encrypted_token, u32* encrypted_token_bytes) noexcept;

} // namespace maicord
