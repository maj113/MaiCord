#include "maicord/app/voice_connection.hpp"
#include "maicord/core/byte_order.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr auto kInfo = VoiceLogLevel::info;
constexpr auto kWarning = VoiceLogLevel::warning;
constexpr auto kError = VoiceLogLevel::error;
u64 status_value(MlsStatus status) noexcept {
    return static_cast<u64>(status);
}
u64 active_epoch(const DaveSession& session) noexcept {
    return session.group.initialized ? session.group.active.context.epoch : 0;
}
bool enqueue(VoiceConnection& connection, bool binary, u8 opcode, ConstSlice<u8> payload) noexcept {
    if (connection.dave_outbox_count == kVoiceDaveOutbox || !payload.count) {
        voice_log(kError, "dave_outbox_rejected", "opcode", binary ? opcode : 0, "queued",
                  connection.dave_outbox_count);
        return false;
    }
    const u32 index = (connection.dave_outbox_head + connection.dave_outbox_count) % kVoiceDaveOutbox;
    auto& buffer = connection.dave_outbox[index];
    const u32 bytes = payload.count + (binary ? 1 : 0);
    if (!secure_buffer_initialize(&buffer, bytes).ok()) {
        voice_log(kError, "dave_outbox_storage", "bytes", bytes);
        return false;
    }
    if (binary) {
        if (!voice_gateway_write_binary(opcode, payload, {buffer.data, buffer.capacity}).ok()) {
            secure_buffer_release(&buffer);
            return false;
        }
    } else
        copy_bytes(buffer.data, payload.data, payload.count);
    buffer.bytes = bytes;
    connection.dave_outbox_binary[index] = binary;
    ++connection.dave_outbox_count;
    return true;
}
bool enqueue_transition(VoiceConnection& connection, VoiceDaveOpcode opcode, u16 transition_id) noexcept {
    u8 text[64];
    const auto written = voice_dave_write_transition(opcode, transition_id, {text, sizeof(text)});
    return written.ok() && enqueue(connection, false, 0, {text, written.bytes});
}
// DAVE accepts only members the voice gateway announced (opcode 11) plus us.
u32 recognized_users(const VoiceConnection& connection, u64* users) noexcept {
    u32 count = 0;
    users[count++] = connection.dave_user_id;
    const auto& participants = connection.participants;
    for (u32 i = 0; i != participants.user_count && count != kDaveMaxMembers; ++i) {
        const auto& user = participants.users[i];
        if (user.connected && user.user_id && user.user_id != connection.dave_user_id)
            users[count++] = user.user_id;
    }
    return count;
}
// Keeps an opcode 25 package for the next group (Session Description, epoch
// 1 or recovery) when the session cannot take it now.
void retain_external_sender(VoiceConnection& connection, ConstSlice<u8> package) noexcept {
    if (package.count > sizeof(connection.dave_external_sender)) {
        voice_log(kWarning, "dave_external_sender_oversized", "bytes", package.count);
        return;
    }
    copy_bytes(connection.dave_external_sender, package.data, package.count);
    connection.dave_external_sender_bytes = package.count;
}
bool send_key_package(VoiceConnection& connection) noexcept {
    ConstSlice<u8> package{};
    const auto status = dave_session_key_package(&connection.dave_session, &package);
    if (status != MlsStatus::ok) {
        voice_log(kError, "dave_key_package_failed", "status", status_value(status));
        return false;
    }
    if (!enqueue(connection, true, static_cast<u8>(VoiceDaveOpcode::mls_key_package), package))
        return false;
    voice_log(kInfo, "dave_key_package_sent", "group_state",
              static_cast<u64>(connection.dave_session.group_state));
    return true;
}
// Opcode 26 never waits for opcode 25; an already received sender is applied
// first so the pending group can form immediately. A sender the session
// refuses is dropped: the gateway sends it again with the next group.
bool prepare_group_key_package(VoiceConnection& connection) noexcept {
    if (connection.dave_external_sender_bytes) {
        const auto status = dave_session_set_external_sender(
            &connection.dave_session,
            {connection.dave_external_sender, connection.dave_external_sender_bytes});
        secure_clear_bytes(connection.dave_external_sender, sizeof(connection.dave_external_sender));
        connection.dave_external_sender_bytes = 0;
        if (status != MlsStatus::ok)
            voice_log(kWarning, "dave_external_sender_rejected", "status", status_value(status));
    }
    return send_key_package(connection);
}
// Unrecoverable DAVE state: close with the official client's code and
// identify again on its MLS failure backoff. Later events are ignored.
bool request_reconnect(VoiceConnection& connection, u16 close_code) noexcept {
    if (connection.dave_reconnects != 0xff)
        ++connection.dave_reconnects;
    connection.dave_reconnect_requested = true;
    connection.dave_close_code = close_code;
    voice_log(kWarning, "dave_reconnect_requested", "close_code", close_code, "attempt", connection.dave_reconnects);
    return true;
}
// Official refusal points: Session Description ("init", allowed on stages),
// prepare transition and prepare epoch (never allowed).
enum class DowngradeAt : u8 { init, transition, epoch };
bool refuse_downgrade(VoiceConnection& connection, DowngradeAt at, u16 transition_id) noexcept {
    voice_log(kError, "dave_downgrade_refused", "at", static_cast<u64>(at), "transition", transition_id);
    return request_reconnect(connection, kVoiceCloseDaveDowngradeRefused);
}
// Reports an unusable commit/welcome/transition (opcode 31) and rejoins with
// a fresh key package; the gateway then re-adds us with a new proposal.
bool flag_invalid(VoiceConnection& connection, u16 transition_id) noexcept {
    auto& session = connection.dave_session;
    const u16 version = session.protocol_version;
    if (!version) {
        voice_log(kWarning, "dave_invalid_without_group", "transition", transition_id);
        return true;
    }
    if (++connection.dave_invalid_streak >= kVoiceDaveInvalidLimit) {
        voice_log(kError, "dave_invalid_limit", "transition", transition_id, "streak",
                  connection.dave_invalid_streak);
        connection.dave_invalid_streak = 0;
        return request_reconnect(connection, kVoiceCloseRepeatedMlsInvalid);
    }
    voice_log(kWarning, "dave_invalid_commit_welcome_sent", "transition", transition_id, "streak",
              connection.dave_invalid_streak);
    if (!enqueue_transition(connection, VoiceDaveOpcode::mls_invalid_commit_welcome, transition_id))
        return false;
    if (const auto status = dave_session_reinitialize(&session, version); status != MlsStatus::ok) {
        voice_log(kError, "dave_reinitialize_failed", "status", status_value(status));
        return false;
    }
    if (!prepare_group_key_package(connection))
        return false;
    connection.dave_recovering = true;
    return true;
}
bool execute(VoiceConnection& connection, u16 transition_id, u64 now_ms) noexcept {
    auto& session = connection.dave_session;
    // An execute we never prepared (lost to a reset, or meant for a group we
    // abandoned) is ignored, as libdave does; the gateway re-adds us.
    if (!dave_session_transition_prepared(session, transition_id)) {
        voice_log(kInfo, "dave_execute_unprepared", "transition", transition_id, "recovering",
                  connection.dave_recovering);
        return true;
    }
    const u16 before = session.protocol_version;
    const auto status = dave_session_execute_transition(&session, transition_id, now_ms);
    if (status != MlsStatus::ok) {
        voice_log(kWarning, "dave_execute_failed", "transition", transition_id, "status", status_value(status));
        return flag_invalid(connection, transition_id);
    }
    voice_log(kInfo, "dave_executed", "transition", transition_id, "version", session.protocol_version, "epoch",
              active_epoch(session));
    if (before != session.protocol_version)
        voice_log(session.protocol_version ? kInfo : kWarning, "dave_protocol_version", "from", before, "to",
                  session.protocol_version);
    connection.dave_recovering = false;
    connection.dave_invalid_streak = 0;
    connection.dave_reconnects = 0;
    return true;
}
bool acknowledge(VoiceConnection& connection, u16 transition_id, u64 now_ms) noexcept {
    return transition_id ? enqueue_transition(connection, VoiceDaveOpcode::transition_ready, transition_id)
                         : execute(connection, 0, now_ms);
}
// Opcode 21 (or 24 with a transition).
bool stage_and_acknowledge(VoiceConnection& connection, u16 transition_id, u16 version, u64 now_ms) noexcept {
    auto& session = connection.dave_session;
    if (!version)
        return refuse_downgrade(connection, DowngradeAt::transition, transition_id);
    const auto status = dave_session_prepare_transition(&session, transition_id, version, true);
    if (status != MlsStatus::ok) {
        // Unready: the gateway times the transition out and the next one or
        // an execute we did not prepare (ignored) follows.
        voice_log(kWarning, "dave_prepare_failed", "transition", transition_id, "version", version, "status",
                  status_value(status));
        return true;
    }
    voice_log(kInfo, "dave_prepare_transition", "transition", transition_id, "version", version);
    return acknowledge(connection, transition_id, now_ms);
}
} // namespace

void voice_connection_dave_reset(VoiceConnection* connection) noexcept {
    dave_session_release(&connection->dave_session);
    for (auto& buffer : connection->dave_outbox)
        secure_buffer_release(&buffer);
    connection->dave_outbox_head = connection->dave_outbox_count = 0;
    secure_clear_bytes(connection->dave_external_sender, sizeof(connection->dave_external_sender));
    connection->dave_external_sender_bytes = 0;
    connection->dave_recovering = false;
    connection->dave_invalid_streak = 0;
}
bool voice_connection_dave_description(VoiceConnection* connection) noexcept {
    const u16 version = connection->media.dave_protocol_version;
    voice_log(kInfo, "dave_session", "version", version, "stage", connection->dave_optional);
    dave_session_release(&connection->dave_session);
    if (!version && !connection->dave_optional)
        return refuse_downgrade(*connection, DowngradeAt::init, 0);
    connection->dave_recovering = false;
    connection->dave_invalid_streak = 0;
    if (const auto status = dave_session_initialize(&connection->dave_session, connection->dave_group_id,
                                                    connection->dave_user_id, connection->socket.setup_generation,
                                                    version);
        status != MlsStatus::ok) {
        voice_log(kError, "dave_initialize_failed", "status", status_value(status));
        return false;
    }
    // Version 0 (stages only): transport-only media.
    return !version || prepare_group_key_package(*connection);
}
bool voice_connection_dave_binary(VoiceConnection* connection, const VoiceGatewayBinary& frame,
                                  u64 now_ms) noexcept {
    auto& session = connection->dave_session;
    const auto opcode = static_cast<VoiceDaveOpcode>(frame.opcode);
    if (opcode == VoiceDaveOpcode::mls_external_sender) {
        if (session.initialized && session.protocol_version) {
            const auto status = dave_session_set_external_sender(&session, frame.data);
            if (status == MlsStatus::ok)
                voice_log(kInfo, "dave_external_sender", "group_state", static_cast<u64>(session.group_state));
            else {
                voice_log(kWarning, "dave_external_sender_deferred", "status", status_value(status));
                retain_external_sender(*connection, frame.data);
            }
            return true;
        }
        // Retain an early sender until Session Description (or an upgrade)
        // creates the group.
        retain_external_sender(*connection, frame.data);
        return true;
    }
    if (!session.initialized || !session.protocol_version) {
        voice_log(kInfo, "dave_binary_ignored", "opcode", frame.opcode, "version", session.protocol_version);
        return true;
    }
    u64 users[kDaveMaxMembers];
    const ConstSlice<u64> recognized{users, recognized_users(*connection, users)};
    if (opcode == VoiceDaveOpcode::mls_proposals) {
        ConstSlice<u8> output{};
        u32 commit_bytes = 0;
        // An invalid or out-of-order proposal batch is dropped (as libdave
        // does); the gateway resolves the epoch with another commit or batch.
        const auto status = dave_session_proposals(&session, frame.data, recognized, &output, &commit_bytes);
        if (status != MlsStatus::ok) {
            voice_log(kWarning, "dave_proposals_rejected", "status", status_value(status), "recognized",
                      recognized.count, "group_state", static_cast<u64>(session.group_state));
            return true;
        }
        if (!output.count) {
            voice_log(kInfo, "dave_proposals_no_commit", "recognized", recognized.count);
            return true;
        }
        voice_log(kInfo, "dave_commit_welcome_sent", "commit_bytes", commit_bytes, "welcome_bytes",
                  output.count - commit_bytes, "recognized", recognized.count);
        return enqueue(*connection, true, static_cast<u8>(VoiceDaveOpcode::mls_commit_welcome), output);
    }
    if (frame.data.count <= 2) {
        voice_log(kWarning, "dave_binary_truncated", "opcode", frame.opcode);
        return true;
    }
    const u16 transition_id = byte_load_be16(frame.data.data);
    const ConstSlice<u8> message{frame.data.data + 2, frame.data.count - 2};
    if (opcode == VoiceDaveOpcode::mls_announce_commit_transition) {
        const auto status = dave_session_commit(&session, transition_id, message);
        if (status == MlsStatus::ok) {
            voice_log(kInfo, "dave_commit_staged", "transition", transition_id, "epoch", active_epoch(session) + 1);
            return acknowledge(*connection, transition_id, now_ms);
        }
        // While rejoining, commits of the abandoned group are expected noise;
        // reporting them would replace the join key the Welcome is for.
        if (connection->dave_recovering) {
            voice_log(kInfo, "dave_commit_ignored_recovering", "transition", transition_id, "status",
                      status_value(status));
            return true;
        }
        voice_log(kWarning, "dave_commit_failed", "transition", transition_id, "status", status_value(status));
        return flag_invalid(*connection, transition_id);
    }
    if (opcode == VoiceDaveOpcode::mls_welcome) {
        const auto status = dave_session_welcome(&session, transition_id, message, recognized);
        if (status == MlsStatus::ok) {
            voice_log(kInfo, "dave_welcome_joined", "transition", transition_id, "epoch", active_epoch(session));
            return acknowledge(*connection, transition_id, now_ms);
        }
        voice_log(kWarning, "dave_welcome_failed", "transition", transition_id, "status", status_value(status),
                  "recognized", recognized.count);
        return flag_invalid(*connection, transition_id);
    }
    voice_log(kWarning, "dave_binary_unexpected", "opcode", frame.opcode);
    return true;
}
bool voice_connection_dave_json(VoiceConnection* connection, u8 opcode, JsonValue data, Slice<u32> scratch,
                                u64 now_ms) noexcept {
    auto& session = connection->dave_session;
    if (!session.initialized) {
        voice_log(kInfo, "dave_json_before_session", "opcode", opcode);
        return true;
    }
    VoiceDaveCommand command{};
    if (const auto decoded = voice_dave_decode_command(opcode, data, scratch, 64, &command); !decoded.ok()) {
        voice_log(kWarning, "dave_json_malformed", "opcode", opcode, "error", static_cast<u64>(decoded.error));
        return true;
    }
    switch (static_cast<VoiceDaveOpcode>(opcode)) {
    case VoiceDaveOpcode::prepare_transition:
        return stage_and_acknowledge(*connection, command.transition_id, command.protocol_version, now_ms);
    case VoiceDaveOpcode::execute_transition:
        return execute(*connection, command.transition_id, now_ms);
    case VoiceDaveOpcode::prepare_epoch:
        voice_log(kInfo, "dave_prepare_epoch", "epoch", command.epoch, "version", command.protocol_version);
        if (!command.protocol_version)
            return refuse_downgrade(*connection, DowngradeAt::epoch, command.transition_id);
        // Epoch 1 announces a new group (also a stage's upgrade from version 0);
        // later epochs are carried by the following commit/welcome unless the
        // server attaches a transition.
        if (command.epoch == 1) {
            if (const auto status = dave_session_reinitialize(&session, command.protocol_version);
                status != MlsStatus::ok) {
                voice_log(kError, "dave_reinitialize_failed", "status", status_value(status));
                return false;
            }
            return prepare_group_key_package(*connection);
        }
        return !command.has_transition || !command.transition_id ||
               stage_and_acknowledge(*connection, command.transition_id, command.protocol_version, now_ms);
    default:
        voice_log(kWarning, "dave_json_unexpected", "opcode", opcode);
        return true;
    }
}
bool voice_connection_dave_flush(VoiceConnection* connection, u64 now_ms) noexcept {
    while (connection->dave_outbox_count && connection->socket.sending == VoiceSocketOutput::none) {
        const u32 index = connection->dave_outbox_head;
        auto& buffer = connection->dave_outbox[index];
        const auto sent = voice_socket_send(&connection->socket,
                                            connection->dave_outbox_binary[index] ? WebSocketOpcode::binary
                                                                                  : WebSocketOpcode::text,
                                            secure_buffer_view(buffer), now_ms);
        if (sent == VoiceSocketSend::busy)
            return true;
        if (sent != VoiceSocketSend::accepted) {
            voice_log(kError, "dave_flush_failed", "result", static_cast<u64>(sent));
            return false;
        }
        secure_buffer_release(&buffer);
        connection->dave_outbox_head = (index + 1) % kVoiceDaveOutbox;
        --connection->dave_outbox_count;
    }
    return true;
}

namespace {
void log_decrypt_failure(VoiceConnection& connection, const DaveMediaResult& result, bool video,
                         u64 now_ms) noexcept {
    if (result.ok() || result.error == DaveMediaError::output_capacity)
        return;
    u64 suppressed = 0;
    if (voice_log_limit(&connection.decrypt_log, now_ms, &suppressed)) {
        const VoiceLogField fields[]{{"error", static_cast<u64>(result.error)},
                                     {"sender_error", static_cast<u64>(result.sender.error)},
                                     {"video", video},
                                     {"version", connection.dave_session.protocol_version},
                                     {"suppressed", suppressed}};
        voice_log_fields(kWarning, "dave_decrypt_failed", {fields, 5});
    }
}
} // namespace
DaveMediaResult voice_connection_dave_decrypt(VoiceConnection* connection,
                                              const VoiceReceivedPacket& packet, ConstSlice<u8> frame,
                                              Slice<u8> scratch, Slice<u8> output, u64 now_ms) noexcept {
    DaveMediaResult result{};
    if (!connection || packet.transport.layout.kind != MediaPacketKind::rtp ||
        !voice_connection_media_current(*connection, packet)) {
        result.error = DaveMediaError::invalid_input;
        return result;
    }
    if (!connection->dave_session.initialized) {
        result.error = DaveMediaError::not_ready;
        return result;
    }
    result = dave_session_decrypt(&connection->dave_session, packet.source.user_id, frame, now_ms, scratch, output);
    log_decrypt_failure(*connection, result, false, now_ms);
    return result;
}
DaveMediaResult voice_connection_dave_decrypt_frame(VoiceConnection* connection, u64 user_id, u32 ssrc,
                                                    ConstSlice<u8> frame, Slice<u8> scratch, Slice<u8> output,
                                                    u64 now_ms) noexcept {
    DaveMediaResult result{};
    const auto source =
        connection ? voice_participants_source(connection->participants, ssrc) : VoiceSsrcBinding{};
    if (!user_id || source.user_id != user_id ||
        (source.kind != VoiceSourceKind::video && source.kind != VoiceSourceKind::screen)) {
        result.error = DaveMediaError::invalid_input;
        return result;
    }
    if (!connection->dave_session.initialized) {
        result.error = DaveMediaError::not_ready;
        return result;
    }
    result = dave_session_decrypt(&connection->dave_session, user_id, frame, now_ms, scratch, output);
    log_decrypt_failure(*connection, result, true, now_ms);
    return result;
}
DaveSealResult voice_connection_dave_encrypt(VoiceConnection* connection, ConstSlice<u8> opus,
                                             Slice<u8> output) noexcept {
    if (!connection || !connection->dave_session.initialized)
        return {0, 0, 0, DaveSealError::not_initialized};
    return dave_session_encrypt(&connection->dave_session, opus, output);
}
bool voice_connection_dave_ready(const VoiceConnection& connection) noexcept {
    return connection.dave_session.initialized && dave_session_can_encrypt(connection.dave_session);
}
} // namespace maicord
