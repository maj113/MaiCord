#include "maicord/app/remote_auth_handshake.hpp"

#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/base64url.hpp"


namespace maicord {
namespace {

RemoteAuthHandshakeResult result(RemoteAuthHandshakeError error, u32 bytes = 0,
                                 RemoteAuthError protocol_error = RemoteAuthError::none,
                                 RemoteAuthCryptoError crypto_error = RemoteAuthCryptoError::none,
                                 Base64Error base64_error = Base64Error::none,
                                 RemoteAuthHandshakeStage stage = RemoteAuthHandshakeStage::none) noexcept {
    return RemoteAuthHandshakeResult{bytes, error, protocol_error, crypto_error, base64_error, stage};
}

u64 next_wake(const RemoteAuthHandshake& handshake) noexcept {
    if (handshake.expires_at_ms == 0) {
        return handshake.next_heartbeat_at_ms;
    }
    if (handshake.next_heartbeat_at_ms == 0) {
        return handshake.expires_at_ms;
    }
    return minimum(handshake.expires_at_ms, handshake.next_heartbeat_at_ms);
}

// Ends the handshake; the arguments are those of result().
template <typename... Arguments>
RemoteAuthHandshakeResult fail(RemoteAuthHandshake* handshake, Arguments... arguments) noexcept {
    handshake->phase = RemoteAuthHandshakePhase::failed;
    return result(arguments...);
}

bool constant_time_equal(ConstSlice<u8> first, ConstSlice<u8> second) noexcept {
    if (first.count != second.count || (!first.data && first.count != 0) ||
        (!second.data && second.count != 0)) {
        return false;
    }
    u8 difference = 0;
    for (u32 index = 0; index < first.count; ++index) {
        difference |= first[index] ^ second[index];
    }
    return difference == 0;
}

bool valid_user_payload(ConstSlice<u8> payload) noexcept {
    RemoteAuthUserPreview preview{};
    return remote_auth_parse_user_preview(
               Utf8View{reinterpret_cast<const char*>(payload.data), payload.count}, &preview)
        .ok();
}

RemoteAuthHandshakeResult crypto_failure(
    RemoteAuthHandshake* handshake, RemoteAuthCryptoError error,
    RemoteAuthHandshakeStage stage = RemoteAuthHandshakeStage::none) noexcept {
    return fail(handshake, RemoteAuthHandshakeError::crypto, 0, RemoteAuthError::none, error,
                Base64Error::none, stage);
}

} // namespace

RemoteAuthHandshakeResult remote_auth_handshake_initialize(RemoteAuthHandshake* handshake) noexcept {
    if (!handshake || handshake->phase != RemoteAuthHandshakePhase::empty ||
        handshake->crypto.rsa_algorithm || handshake->crypto.rsa_key ||
        handshake->crypto.sha256_algorithm) {
        return result(RemoteAuthHandshakeError::invalid_input);
    }
    const RemoteAuthCryptoResult initialized = remote_auth_crypto_initialize(&handshake->crypto);
    if (!initialized.ok()) {
        reset_value<RemoteAuthHandshake>(handshake);
        return result(RemoteAuthHandshakeError::crypto, 0, RemoteAuthError::none, initialized.error);
    }
    u8 fingerprint[kRemoteAuthSha256Bytes]{};
    const RemoteAuthCryptoResult fingerprinted = remote_auth_crypto_fingerprint(
        handshake->crypto, Slice<u8>{fingerprint, static_cast<u32>(sizeof(fingerprint))});
    const Base64UrlResult encoded =
        fingerprinted.ok()
            ? base64url_encode(ConstSlice<u8>{fingerprint, static_cast<u32>(sizeof(fingerprint))},
                               Slice<char>{handshake->fingerprint, kRemoteAuthFingerprintEncodedBytes})
            : Base64UrlResult{};
    secure_clear_bytes(fingerprint, sizeof(fingerprint));
    if (!fingerprinted.ok() || !encoded.ok() || encoded.bytes != kRemoteAuthFingerprintEncodedBytes) {
        const RemoteAuthCryptoError crypto_error = fingerprinted.error;
        remote_auth_crypto_release(&handshake->crypto);
        reset_value<RemoteAuthHandshake>(handshake);
        return result(RemoteAuthHandshakeError::crypto, 0, RemoteAuthError::none, crypto_error);
    }
    handshake->phase = RemoteAuthHandshakePhase::awaiting_hello;
    return result(RemoteAuthHandshakeError::none);
}

void remote_auth_handshake_release(RemoteAuthHandshake* handshake) noexcept {
    if (!handshake) {
        return;
    }
    remote_auth_crypto_release(&handshake->crypto);
    secure_clear_bytes(handshake, sizeof(*handshake));
}

RemoteAuthHandshakeResult remote_auth_handshake_handle(RemoteAuthHandshake* handshake, Utf8View body,
                                                       Slice<u32> structural_scratch, u32 maximum_depth,
                                                       u64 now_ms, Slice<char> command, Slice<char> data,
                                                       RemoteAuthHandshakeEvent* event) noexcept {
    if (!handshake || !event || now_ms == 0 || (!command.data && command.count != 0) ||
        (!data.data && data.count != 0) || handshake->phase == RemoteAuthHandshakePhase::empty ||
        handshake->phase == RemoteAuthHandshakePhase::authenticated ||
        handshake->phase == RemoteAuthHandshakePhase::cancelled ||
        handshake->phase == RemoteAuthHandshakePhase::failed) {
        return result(RemoteAuthHandshakeError::invalid_state);
    }
    reset_value<RemoteAuthHandshakeEvent>(event);
    char packet_text[kRemoteAuthMaximumTicketBytes]{};
    RemoteAuthPacket packet{};
    const RemoteAuthDecodeResult decoded = remote_auth_decode_server_packet(
        body, structural_scratch, maximum_depth,
        Slice<char>{packet_text, static_cast<u32>(sizeof(packet_text))}, &packet);
    if (!decoded.ok()) {
        return fail(handshake, RemoteAuthHandshakeError::protocol, 0, decoded.error);
    }
    if (handshake->expires_at_ms != 0 && now_ms >= handshake->expires_at_ms) {
        return fail(handshake, RemoteAuthHandshakeError::session_timeout);
    }

    if (packet.kind == RemoteAuthPacketKind::cancel) {
        handshake->phase = RemoteAuthHandshakePhase::cancelled;
        event->kind = RemoteAuthHandshakeEventKind::cancelled;
        return result(RemoteAuthHandshakeError::none);
    }
    if (packet.kind == RemoteAuthPacketKind::heartbeat_ack) {
        if (handshake->heartbeat_interval_ms == 0) {
            return fail(handshake, RemoteAuthHandshakeError::unexpected_packet);
        }
        handshake->heartbeat_unacknowledged = false;
        event->next_wake_at_ms = next_wake(*handshake);
        return result(RemoteAuthHandshakeError::none);
    }

    if (packet.kind == RemoteAuthPacketKind::hello) {
        if (handshake->phase != RemoteAuthHandshakePhase::awaiting_hello) {
            return fail(handshake, RemoteAuthHandshakeError::unexpected_packet);
        }
        u8 spki[kRemoteAuthPublicSpkiMaximumBytes]{};
        char encoded_spki[432]{};
        const RemoteAuthCryptoResult exported = remote_auth_crypto_export_spki(
            handshake->crypto, Slice<u8>{spki, static_cast<u32>(sizeof(spki))});
        const Base64Result encoded =
            exported.ok() ? base64_encode(ConstSlice<u8>{spki, exported.bytes},
                                          Slice<char>{encoded_spki, static_cast<u32>(sizeof(encoded_spki))})
                          : Base64Result{};
        secure_clear_bytes(spki, sizeof(spki));
        if (!exported.ok()) {
            return crypto_failure(handshake, exported.error);
        }
        if (!encoded.ok()) {
            return fail(handshake, RemoteAuthHandshakeError::protocol);
        }
        const RemoteAuthWriteResult written =
            remote_auth_write_init(Utf8View{encoded_spki, encoded.bytes}, command);
        secure_clear_bytes(encoded_spki, sizeof(encoded_spki));
        if (!written.ok()) {
            return fail(handshake, written.error == RemoteAuthWriteError::output_too_small
                                       ? RemoteAuthHandshakeError::output_too_small
                                       : RemoteAuthHandshakeError::protocol,
                                   written.bytes);
        }
        handshake->expires_at_ms = deadline_after(now_ms, packet.timeout_ms);
        handshake->heartbeat_interval_ms = packet.heartbeat_interval_ms;
        handshake->next_heartbeat_at_ms = deadline_after(now_ms, packet.heartbeat_interval_ms);
        handshake->heartbeat_unacknowledged = false;
        handshake->phase = RemoteAuthHandshakePhase::awaiting_nonce;
        event->kind = RemoteAuthHandshakeEventKind::send_command;
        event->command_bytes = written.bytes;
        event->next_wake_at_ms = next_wake(*handshake);
        return result(RemoteAuthHandshakeError::none, written.bytes);
    }

    if (packet.kind == RemoteAuthPacketKind::nonce_proof) {
        if (handshake->phase != RemoteAuthHandshakePhase::awaiting_nonce) {
            return fail(handshake, RemoteAuthHandshakeError::unexpected_packet);
        }
        u8 ciphertext[kRemoteAuthRsaBytes]{};
        u8 nonce[kRemoteAuthMaximumUserPayloadBytes]{};
        char encoded_nonce[256]{};
        const Base64Result cipher =
            base64_decode(Utf8View{packet_text, packet.text_bytes},
                          Slice<u8>{ciphertext, static_cast<u32>(sizeof(ciphertext))});
        const RemoteAuthCryptoResult decrypted =
            cipher.ok()
                ? remote_auth_crypto_decrypt(handshake->crypto, ConstSlice<u8>{ciphertext, cipher.bytes},
                                             Slice<u8>{nonce, static_cast<u32>(sizeof(nonce))})
                : RemoteAuthCryptoResult{};
        secure_clear_bytes(ciphertext, sizeof(ciphertext));
        if (!cipher.ok() || !decrypted.ok() || decrypted.bytes == 0 ||
            decrypted.bytes > kRemoteAuthMaximumNonceBytes) {
            secure_clear_bytes(nonce, sizeof(nonce));
            return fail(handshake, !cipher.ok() || decrypted.ok() ? RemoteAuthHandshakeError::invalid_nonce
                                                                  : RemoteAuthHandshakeError::crypto,
                                   0, RemoteAuthError::none, decrypted.error);
        }
        const Base64UrlResult encoded =
            base64url_encode(ConstSlice<u8>{nonce, decrypted.bytes},
                             Slice<char>{encoded_nonce, static_cast<u32>(sizeof(encoded_nonce))});
        secure_clear_bytes(nonce, sizeof(nonce));
        if (!encoded.ok()) {
            return fail(handshake, RemoteAuthHandshakeError::invalid_nonce);
        }
        const RemoteAuthWriteResult written =
            remote_auth_write_nonce_proof(Utf8View{encoded_nonce, encoded.bytes}, command);
        secure_clear_bytes(encoded_nonce, sizeof(encoded_nonce));
        if (!written.ok()) {
            return fail(handshake, written.error == RemoteAuthWriteError::output_too_small
                                       ? RemoteAuthHandshakeError::output_too_small
                                       : RemoteAuthHandshakeError::protocol,
                                   written.bytes);
        }
        handshake->phase = RemoteAuthHandshakePhase::awaiting_fingerprint;
        event->kind = RemoteAuthHandshakeEventKind::send_command;
        event->command_bytes = written.bytes;
        event->next_wake_at_ms = next_wake(*handshake);
        return result(RemoteAuthHandshakeError::none, written.bytes);
    }

    if (packet.kind == RemoteAuthPacketKind::pending_remote_init) {
        if (handshake->phase != RemoteAuthHandshakePhase::awaiting_fingerprint) {
            return fail(handshake, RemoteAuthHandshakeError::unexpected_packet);
        }
        if (!constant_time_equal(
                ConstSlice<u8>{reinterpret_cast<const u8*>(packet_text), packet.text_bytes},
                ConstSlice<u8>{reinterpret_cast<const u8*>(handshake->fingerprint),
                               kRemoteAuthFingerprintEncodedBytes})) {
            return fail(handshake, RemoteAuthHandshakeError::fingerprint_mismatch);
        }
        if (!copy_bytes(data.data, data.count, handshake->fingerprint,
                        kRemoteAuthFingerprintEncodedBytes)) {
            return fail(handshake, RemoteAuthHandshakeError::output_too_small,
                        kRemoteAuthFingerprintEncodedBytes);
        }
        handshake->phase = RemoteAuthHandshakePhase::awaiting_mobile;
        event->kind = RemoteAuthHandshakeEventKind::qr_ready;
        event->data_bytes = kRemoteAuthFingerprintEncodedBytes;
        event->next_wake_at_ms = next_wake(*handshake);
        return result(RemoteAuthHandshakeError::none, kRemoteAuthFingerprintEncodedBytes);
    }

    if (packet.kind == RemoteAuthPacketKind::pending_ticket) {
        if (handshake->phase != RemoteAuthHandshakePhase::awaiting_mobile) {
            return fail(handshake, RemoteAuthHandshakeError::unexpected_packet);
        }
        u8 ciphertext[kRemoteAuthRsaBytes]{};
        u8 plaintext[kRemoteAuthMaximumUserPayloadBytes]{};
        const Base64Result cipher =
            base64_decode(Utf8View{packet_text, packet.text_bytes},
                          Slice<u8>{ciphertext, static_cast<u32>(sizeof(ciphertext))});
        const RemoteAuthCryptoResult decrypted =
            cipher.ok()
                ? remote_auth_crypto_decrypt(handshake->crypto, ConstSlice<u8>{ciphertext, cipher.bytes},
                                             Slice<u8>{plaintext, static_cast<u32>(sizeof(plaintext))})
                : RemoteAuthCryptoResult{};
        secure_clear_bytes(ciphertext, sizeof(ciphertext));
        if (!cipher.ok() || !decrypted.ok()) {
            secure_clear_bytes(plaintext, sizeof(plaintext));
            return crypto_failure(handshake, decrypted.error);
        }
        if (!valid_user_payload(ConstSlice<u8>{plaintext, decrypted.bytes})) {
            secure_clear_bytes(plaintext, sizeof(plaintext));
            return fail(handshake, RemoteAuthHandshakeError::invalid_user_payload);
        }
        if (!copy_bytes(data.data, data.count, plaintext, decrypted.bytes)) {
            secure_clear_bytes(plaintext, sizeof(plaintext));
            return fail(handshake, RemoteAuthHandshakeError::output_too_small, decrypted.bytes);
        }
        secure_clear_bytes(plaintext, sizeof(plaintext));
        event->kind = RemoteAuthHandshakeEventKind::user_pending;
        event->data_bytes = decrypted.bytes;
        event->next_wake_at_ms = next_wake(*handshake);
        return result(RemoteAuthHandshakeError::none, decrypted.bytes);
    }

    if (packet.kind == RemoteAuthPacketKind::pending_login) {
        if (handshake->phase != RemoteAuthHandshakePhase::awaiting_mobile) {
            return fail(handshake, RemoteAuthHandshakeError::unexpected_packet);
        }
        if (!copy_bytes(data.data, data.count, packet_text, packet.text_bytes)) {
            return fail(handshake, RemoteAuthHandshakeError::output_too_small, packet.text_bytes);
        }
        handshake->phase = RemoteAuthHandshakePhase::ticket_ready;
        event->kind = RemoteAuthHandshakeEventKind::ticket_ready;
        event->data_bytes = packet.text_bytes;
        event->next_wake_at_ms = next_wake(*handshake);
        return result(RemoteAuthHandshakeError::none, packet.text_bytes);
    }

    return fail(handshake, RemoteAuthHandshakeError::unexpected_packet);
}

RemoteAuthHandshakeResult remote_auth_handshake_tick(RemoteAuthHandshake* handshake, u64 now_ms,
                                                     Slice<char> command,
                                                     RemoteAuthHandshakeEvent* event) noexcept {
    if (!handshake || !event || now_ms == 0 || (!command.data && command.count != 0) ||
        handshake->phase < RemoteAuthHandshakePhase::awaiting_nonce ||
        handshake->phase > RemoteAuthHandshakePhase::ticket_ready) {
        return result(RemoteAuthHandshakeError::invalid_state);
    }
    reset_value<RemoteAuthHandshakeEvent>(event);
    if (handshake->expires_at_ms != 0 && now_ms >= handshake->expires_at_ms) {
        return fail(handshake, RemoteAuthHandshakeError::session_timeout);
    }
    if (handshake->next_heartbeat_at_ms == 0 || now_ms < handshake->next_heartbeat_at_ms) {
        event->next_wake_at_ms = next_wake(*handshake);
        return result(RemoteAuthHandshakeError::none);
    }
    if (handshake->heartbeat_unacknowledged) {
        return fail(handshake, RemoteAuthHandshakeError::heartbeat_timeout);
    }
    const RemoteAuthWriteResult written = remote_auth_write_heartbeat(command);
    if (!written.ok()) {
        return fail(handshake, RemoteAuthHandshakeError::output_too_small, written.bytes);
    }
    handshake->heartbeat_unacknowledged = true;
    handshake->next_heartbeat_at_ms = deadline_after(now_ms, handshake->heartbeat_interval_ms);
    event->kind = RemoteAuthHandshakeEventKind::send_command;
    event->command_bytes = written.bytes;
    event->next_wake_at_ms = next_wake(*handshake);
    return result(RemoteAuthHandshakeError::none, written.bytes);
}

RemoteAuthHandshakeResult remote_auth_handshake_finish_ticket(RemoteAuthHandshake* handshake, Utf8View body,
                                                              Slice<u32> structural_scratch,
                                                              u32 maximum_depth,
                                                              SecureBuffer* token) noexcept {
    if (!handshake || handshake->phase != RemoteAuthHandshakePhase::ticket_ready || !token ||
        !token->data || !token->locked || token->capacity == 0) {
        return result(RemoteAuthHandshakeError::invalid_state);
    }
    // A new ticket attempt replaces the output credential. Erase it before
    // parsing too, so shape/encoding failures cannot leave an older token live.
    secure_buffer_clear(token);
    char encrypted_token[512]{};
    u32 encrypted_token_bytes = 0;
    const RemoteAuthDecodeResult decoded = remote_auth_decode_ticket_response(
        body, structural_scratch, maximum_depth,
        Slice<char>{encrypted_token, static_cast<u32>(sizeof(encrypted_token))}, &encrypted_token_bytes);
    if (!decoded.ok()) {
        secure_clear_bytes(encrypted_token, sizeof(encrypted_token));
        return fail(handshake, RemoteAuthHandshakeError::protocol, 0, decoded.error,
                    RemoteAuthCryptoError::none, decoded.base64_error,
                    RemoteAuthHandshakeStage::ticket_parse);
    }
    u8 ciphertext[kRemoteAuthRsaBytes]{};
    const Base64Result cipher = base64_decode(Utf8View{encrypted_token, encrypted_token_bytes},
                                              Slice<u8>{ciphertext, static_cast<u32>(sizeof(ciphertext))});
    secure_clear_bytes(encrypted_token, sizeof(encrypted_token));
    if (!cipher.ok()) {
        secure_clear_bytes(ciphertext, sizeof(ciphertext));
        return fail(handshake, RemoteAuthHandshakeError::protocol, 0, RemoteAuthError::none,
                    RemoteAuthCryptoError::none, cipher.error,
                    RemoteAuthHandshakeStage::ticket_cipher_decode);
    }
    const RemoteAuthCryptoResult decrypted =
        remote_auth_crypto_decrypt(handshake->crypto, ConstSlice<u8>{ciphertext, cipher.bytes},
                                   Slice<u8>{token->data, token->capacity});
    secure_clear_bytes(ciphertext, sizeof(ciphertext));
    if (!decrypted.ok() || decrypted.bytes == 0) {
        secure_buffer_clear(token);
        return crypto_failure(handshake, decrypted.error, RemoteAuthHandshakeStage::ticket_decrypt);
    }
    for (u32 index = 0; index < decrypted.bytes; ++index) {
        if (token->data[index] < 0x21 || token->data[index] > 0x7E) {
            secure_buffer_clear(token);
            return fail(handshake, RemoteAuthHandshakeError::protocol, 0, RemoteAuthError::none,
                        RemoteAuthCryptoError::none, Base64Error::none,
                        RemoteAuthHandshakeStage::ticket_plaintext);
        }
    }
    token->bytes = decrypted.bytes;
    handshake->phase = RemoteAuthHandshakePhase::authenticated;
    return result(RemoteAuthHandshakeError::none, decrypted.bytes, RemoteAuthError::none,
                  RemoteAuthCryptoError::none, Base64Error::none,
                  RemoteAuthHandshakeStage::ticket_complete);
}

} // namespace maicord
