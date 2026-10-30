#pragma once

#include "maicord/platform/remote_auth_crypto.hpp"
#include "maicord/platform/secure_buffer.hpp"

namespace maicord {

enum class Base64Error : u8;
enum class RemoteAuthError : u8;

inline constexpr u32 kRemoteAuthFingerprintEncodedBytes = 43;
inline constexpr u32 kRemoteAuthMaximumCommandBytes = 512;

enum class RemoteAuthHandshakePhase : u8 {
    empty,
    awaiting_hello,
    awaiting_nonce,
    awaiting_fingerprint,
    awaiting_mobile,
    ticket_ready,
    authenticated,
    cancelled,
    failed,
};

enum class RemoteAuthHandshakeEventKind : u8 {
    none,
    send_command,
    qr_ready,
    user_pending,
    ticket_ready,
    cancelled,
};

enum class RemoteAuthHandshakeError : u8 {
    none,
    invalid_input,
    invalid_state,
    protocol,
    crypto,
    output_too_small,
    unexpected_packet,
    invalid_nonce,
    fingerprint_mismatch,
    invalid_user_payload,
    heartbeat_timeout,
    session_timeout,
};

// A categorical, secret-free location for handshake failures. This is exposed
// to diagnostics so the caller can distinguish response shape, wire encoding,
// RSA-OAEP, and plaintext policy without retaining response or credential
// bytes.
enum class RemoteAuthHandshakeStage : u8 {
    none,
    ticket_parse,
    ticket_cipher_decode,
    ticket_decrypt,
    ticket_plaintext,
    ticket_complete,
};

struct RemoteAuthHandshakeResult {
    u32 bytes;
    RemoteAuthHandshakeError error;
    RemoteAuthError protocol_error;
    RemoteAuthCryptoError crypto_error;
    Base64Error base64_error;
    RemoteAuthHandshakeStage stage;

    constexpr bool ok() const noexcept {
        return error == RemoteAuthHandshakeError::none;
    }
};

struct RemoteAuthHandshakeEvent {
    RemoteAuthHandshakeEventKind kind;
    u32 command_bytes;
    u32 data_bytes;
    u64 next_wake_at_ms;
};

struct RemoteAuthHandshake {
    RemoteAuthCrypto crypto;
    u64 expires_at_ms;
    u64 next_heartbeat_at_ms;
    u32 heartbeat_interval_ms;
    // Fraction (of 2^32) of the first interval to wait before the first heartbeat, drawn at
    // initialize like the official client's randomised first beat.
    u32 first_heartbeat_fraction;
    char fingerprint[kRemoteAuthFingerprintEncodedBytes];
    RemoteAuthHandshakePhase phase;
    bool heartbeat_unacknowledged;
};

RemoteAuthHandshakeResult remote_auth_handshake_initialize(RemoteAuthHandshake* handshake) noexcept;
void remote_auth_handshake_release(RemoteAuthHandshake* handshake) noexcept;

// Consumes one validated server message. Command receives init, nonce proof,
// or heartbeat JSON. Data receives the verified fingerprint, decrypted user
// preview, or one-time login ticket according to event.kind. Both outputs are
// transactional.
RemoteAuthHandshakeResult remote_auth_handshake_handle(RemoteAuthHandshake* handshake, Utf8View body,
                                                       Slice<u32> structural_scratch, u32 maximum_depth,
                                                       u64 now_ms, Slice<char> command, Slice<char> data,
                                                       RemoteAuthHandshakeEvent* event) noexcept;

// Produces a heartbeat command when due and fails closed on session expiry or
// an unacknowledged prior heartbeat.
RemoteAuthHandshakeResult remote_auth_handshake_tick(RemoteAuthHandshake* handshake, u64 now_ms,
                                                     Slice<char> command,
                                                     RemoteAuthHandshakeEvent* event) noexcept;

// Handles the unauthenticated ticket-exchange response and decrypts the token
// directly into caller-owned locked storage. A valid ticket attempt clears
// that storage before parsing; every unsuccessful attempt leaves it empty.
RemoteAuthHandshakeResult remote_auth_handshake_finish_ticket(RemoteAuthHandshake* handshake, Utf8View body,
                                                              Slice<u32> structural_scratch,
                                                              u32 maximum_depth,
                                                              SecureBuffer* token) noexcept;

} // namespace maicord
