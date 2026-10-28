#include "maicord/app/voice_connection.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/random.hpp"

namespace maicord {
namespace {
constexpr auto kInfo = VoiceLogLevel::info;
constexpr auto kWarning = VoiceLogLevel::warning;
constexpr auto kError = VoiceLogLevel::error;
// Every path that ends the call (the panel's failed state) records why.
void fail(VoiceConnection* connection, VoiceConnectionError error, const char* reason, u64 detail = 0) noexcept {
    connection->error = error;
    voice_log(kError, reason, "error", static_cast<u64>(error), "detail", detail, "stream",
              connection->stream_context);
}
void abort_call(VoiceConnection* connection, VoiceConnectionError error, const char* reason,
                u64 detail = 0) noexcept {
    fail(connection, error, reason, detail);
    voice_connection_stop(connection);
}
// The gateway socket is identified, open and not closing.
bool socket_live(const VoiceConnection& connection) noexcept {
    return connection.identify_submitted && connection.socket.phase == VoiceSocketPhase::open &&
           !connection.socket.pending_close && !connection.socket.stop_requested;
}
// The UDP transport is open and bound to the current Ready revision.
bool udp_ready(const VoiceConnection& connection) noexcept {
    return connection.ready_revision && connection.udp_revision == connection.ready_revision &&
           connection.udp.phase == VoiceUdpPhase::ready && connection.udp.socket.open;
}
bool current_media_transport(const VoiceConnection& connection, u64 setup_generation,
                             u64 ready_revision) noexcept {
    return setup_generation && ready_revision == connection.ready_revision &&
           setup_generation == connection.socket.setup_generation && socket_live(connection) &&
           connection.media.has_description && udp_ready(connection) &&
           connection.media.packets.generation == ready_revision;
}
void stop_media_io(VoiceConnection* connection) noexcept {
    connection->media_receive = nullptr;
    connection->media_context = nullptr;
    voice_udp_media_stop(&connection->media_io);
}
// Every stamped packet counts for transport-wide feedback, including probes
// and sources not announced yet: unreported packets read as losses.
void record_extensions(VoiceConnection& connection, ConstSlice<u8> bytes, const MediaPacketLayout& layout) noexcept {
    if (!layout.extension_bytes)
        return;
    const auto extensions = rtp_header_extensions(bytes, layout, kRtpTransportSequenceExtensionId);
    if ((extensions.ids | connection.rtp_extension_ids) != connection.rtp_extension_ids) {
        connection.rtp_extension_ids = static_cast<u16>(connection.rtp_extension_ids | extensions.ids);
        voice_log(kInfo, "rtp_extensions", "ids", connection.rtp_extension_ids, "profile", layout.extension_profile,
                  "stream", connection.stream_context);
    }
    if (extensions.found && extensions.value.count == 2)
        rtcp_transport_feedback_record(&connection.transport_feedback, byte_load_be16(extensions.value.data),
                                       layout.ssrc, clock_microseconds(clock_initialize(), 0, clock_now()));
}
bool receive_media(void* context, ConstSlice<u8> bytes, const VoicePacketResult& packet) noexcept {
    auto& connection = *static_cast<VoiceConnection*>(context);
    if (!connection.media_receive || !packet.ok() ||
        !current_media_transport(connection, connection.socket.setup_generation, connection.ready_revision))
        return false;
    VoiceReceivedPacket received{
        packet, {}, {}, connection.socket.setup_generation, connection.ready_revision};
    if (packet.layout.kind == MediaPacketKind::rtp) {
        record_extensions(connection, bytes, packet.layout);
        received.source = voice_participants_source(connection.participants, packet.layout.ssrc);
        if (!received.source.user_id) {
            ++connection.unannounced_rtp_packets;
            u64 suppressed = 0;
            if (voice_log_limit(&connection.unannounced_log, connection.socket.now_ms, &suppressed))
                voice_log(kInfo, "rtp_unannounced_source", "packets", connection.unannounced_rtp_packets,
                          "suppressed", suppressed);
            return true;
        }
        received.member = voice_participants_user(connection.participants, received.source.user_id);
        // A membership/announcement race drops the packet, never the call.
        if (received.member.user_id != received.source.user_id ||
            connection.participants.generation != received.setup_generation)
            return true;
    } else if (packet.layout.kind != MediaPacketKind::rtcp)
        return true;
    // The worker cannot receive another signaling update between lookup and
    // callback. Do not repeat the source lookup here; deferred work uses the
    // public currency check when it is actually consumed.
    return connection.media_receive(connection.media_context, bytes, received);
}
// A false DAVE step ends the socket: with a requested fresh session it
// reconnects (plan_reconnect), otherwise the call fails.
bool dave_step_failed(VoiceConnection* connection, u8 opcode) noexcept {
    if (!connection->dave_reconnect_requested)
        fail(connection, VoiceConnectionError::protocol, "dave_step_failed", opcode);
    return false;
}
} // namespace
bool voice_connection_event(VoiceConnection* connection, const VoiceSessionEvent& value) noexcept {
    if (!connection)
        return false;
    // A refused downgrade or repeated invalid MLS message is closing this
    // socket (4804/4803); nothing more from it is applied.
    if (connection->dave_close_code && value.kind != VoiceSessionEventKind::closed)
        return true;
    if (!value.ok()) {
        voice_log(kWarning, "gateway_session_error", "error", static_cast<u64>(value.error), "websocket",
                  static_cast<u64>(value.websocket_error));
        return false;
    }
    switch (value.kind) {
    case VoiceSessionEventKind::hello:
        voice_log(kInfo, "gateway_hello", "interval_ms", value.hello.heartbeat_interval_ms, "resume",
                  connection->resumed);
        return true;
    case VoiceSessionEventKind::closed:
        voice_log(kWarning, "gateway_close_frame", "code", value.close_code);
        return true;
    case VoiceSessionEventKind::binary: {
        voice_log(kInfo, "gateway_binary", "op", value.binary.opcode, "seq", value.binary.sequence, "bytes",
                  value.binary.data.count);
        const auto route = voice_dave_route_binary(&connection->dave, value.binary);
        if (route == VoiceDaveRoute::rejected) {
            // Unexpected direction or truncated: logged, never a reason to drop the call.
            voice_log(kWarning, "dave_binary_rejected", "op", value.binary.opcode);
            return true;
        }
        if (route == VoiceDaveRoute::deferred &&
            !voice_connection_dave_binary(connection, value.binary, connection->socket.now_ms))
            return dave_step_failed(connection, value.binary.opcode);
        return true;
    }
    case VoiceSessionEventKind::json:
        break;
    default:
        return true;
    }
    const u8 opcode = value.envelope.opcode;
    if (opcode != 5)
        voice_log(kInfo, "gateway_op", "op", opcode, "seq",
                  value.envelope.has_sequence ? value.envelope.sequence : 0);
    const auto route = voice_dave_route_json(&connection->dave, opcode);
    if (route == VoiceDaveRoute::rejected) {
        // Client-direction DAVE opcodes (23, 26, 28, 31) echoed or reported
        // by the server carry nothing we must act on.
        voice_log(kWarning, "dave_json_rejected", "op", opcode);
        return true;
    }
    if (route == VoiceDaveRoute::deferred) {
        if (!voice_connection_dave_json(connection, opcode, value.envelope.data, connection->socket.session.scratch,
                                        connection->socket.now_ms))
            return dave_step_failed(connection, opcode);
        return true;
    }
    if (opcode == 9 && connection->resumed)
        connection->reconnect_attempts = 0;
    if ((opcode == 2 || opcode == 4) && !connection->identify_submitted) {
        connection->media_error = VoiceMediaSetupError::order;
        fail(connection, VoiceConnectionError::media, "media_setup_order", opcode);
        return false;
    }
    const auto scratch = connection->socket.session.scratch;
    if (opcode == 14) {
        // Optional codec renegotiation; a malformed update keeps the call.
        VoiceSessionUpdate update{};
        if (voice_decode_session_update(value.envelope.data, scratch, 64, &update).ok()) {
            voice_media_session_update(&connection->media, update);
            if (update.has_video_codec)
                voice_log(kInfo, "session_update", "video_codec", static_cast<u64>(update.video));
        } else
            voice_log(kWarning, "session_update_malformed");
        return true;
    }
    if (opcode == 5 || opcode == 11 || opcode == 12 || opcode == 13 || opcode == 18 || opcode == 20) {
        // Membership, SSRC and activity announcements: a payload we cannot
        // apply (a bot's unusual fields, a stale or conflicting SSRC) loses
        // that announcement only, never the call.
        connection->participant_error =
            voice_connection_participant(connection, opcode, value.envelope.data, scratch, 64);
        if (!connection->participant_error.ok())
            voice_log(kWarning, "participant_event_ignored", "op", opcode, "error",
                      static_cast<u64>(connection->participant_error.error), "decode",
                      static_cast<u64>(connection->participant_error.decode.error));
        else if (opcode == 11 || opcode == 13)
            voice_log(kInfo, "participants", "op", opcode, "users", connection->participants.user_count);
        return true;
    }
    if (opcode == 2) {
        VoiceReady ready{};
        connection->media_error = voice_decode_ready(value.envelope.data, scratch, 64, &ready).ok()
                                      ? voice_connection_ready(connection, ready)
                                      : VoiceMediaSetupError::decode;
        if (connection->media_error == VoiceMediaSetupError::none)
            connection->reconnect_attempts = 0;
    } else if (opcode == 4) {
        connection->media_error = voice_connection_description(connection, value.envelope.data, scratch, 64,
                                                               connection->socket.now_ms);
        if (connection->media_error == VoiceMediaSetupError::none) {
            const VoiceLogField fields[]{{"mode", connection->media.selected_mode},
                                         {"dave_version", connection->media.dave_protocol_version},
                                         {"video_offer", connection->media.video_offer},
                                         {"video_codec", static_cast<u64>(connection->media.video_codec)}};
            voice_log_fields(kInfo, "session_description", {fields, 4});
            if (!voice_connection_dave_description(connection)) {
                fail(connection, VoiceConnectionError::protocol, "dave_session_failed");
                voice_media_clear(&connection->media);
                return false;
            }
        }
    }
    if (connection->media_error != VoiceMediaSetupError::none) {
        fail(connection, VoiceConnectionError::media, "media_setup_failed",
             static_cast<u64>(connection->media_error));
        voice_media_clear(&connection->media);
        return false;
    }
    return true;
}
namespace {
bool event(void* context, const VoiceSessionEvent& value) noexcept {
    return voice_connection_event(static_cast<VoiceConnection*>(context), value);
}
bool prepare(VoiceConnection* connection, const VoiceSetupCredentials& credentials, bool resume) noexcept {
    // Advertised capabilities must match what is implemented: DAVE v1 group
    // state and frame crypto; video receive only (the camera stream request
    // is what the official client always sends with video support). A Go Live
    // connection (stream context) requests the screen stream instead.
    static constexpr VoiceIdentifyStream camera[]{{VoiceStreamType::video, 100, 100}};
    static constexpr VoiceIdentifyStream screen[]{{VoiceStreamType::screen, 100, 100}};
    const VoiceGatewayIdentify identify{
        {credentials.server_id, credentials.channel_id, credentials.session, credentials.token},
        credentials.user_id,
        kDaveProtocolVersion,
        true, // video receive
        ConstSlice<VoiceIdentifyStream>{credentials.stream ? screen : camera, 1}};
    if (connection->rtc_connection_generation != credentials.generation) {
        u8 random[16];
        if (!secure_random_fill({random, sizeof(random)}).ok() ||
            !voice_rtc_connection_id({random, 16}, {connection->rtc_connection_id, kVoiceRtcConnectionIdBytes})) {
            fail(connection, VoiceConnectionError::storage, "rtc_connection_id_failed");
            return false;
        }
        connection->rtc_connection_generation = credentials.generation;
    }
    connection->sink_wants_sent = false;
    // DAVE's MLS group id is the voice channel; a fresh Identify starts a new call state.
    if (!resume)
        voice_connection_dave_reset(connection);
    connection->dave_group_id = credentials.channel_id;
    connection->dave_user_id = credentials.user_id;
    connection->stream_context = credentials.stream;
    connection->dave_optional = credentials.stage;
    connection->dave_close_code = 0;
    // Heartbeat state survives socket retirement and carries the resume sequence.
    const auto& heartbeat = connection->socket.session.heartbeat;
    const i32 sequence_ack = heartbeat.has_sequence ? static_cast<i32>(heartbeat.sequence) : -1;
    const auto write = [&](Slice<u8> output) noexcept {
        return resume ? voice_gateway_write_resume(identify.session, sequence_ack, output)
                      : voice_gateway_write_identify(identify, output);
    };
    const auto measured = write({});
    if (!measured.bytes) {
        fail(connection, VoiceConnectionError::command, "identify_encode_failed");
        return false;
    }
    if (!secure_buffer_initialize(&connection->command, measured.bytes).ok()) {
        fail(connection, VoiceConnectionError::storage, "identify_storage_failed");
        return false;
    }
    const auto encoded = write({connection->command.data, connection->command.capacity});
    if (!encoded.ok()) {
        secure_buffer_release(&connection->command);
        fail(connection, VoiceConnectionError::command, "identify_encode_failed");
        return false;
    }
    connection->command.bytes = encoded.bytes;
    voice_log(kInfo, resume ? "gateway_resume" : "gateway_identify", "stream", credentials.stream, "seq_ack",
              resume && heartbeat.has_sequence ? heartbeat.sequence : 0);
    return true;
}
} // namespace
VoiceMediaSetupError voice_connection_ready(VoiceConnection* connection, const VoiceReady& ready) noexcept {
    if (!connection || !connection->identify_submitted ||
        connection->socket.phase != VoiceSocketPhase::open || connection->ready_revision == ~u64{0})
        return VoiceMediaSetupError::order;
    const auto result = voice_media_ready(&connection->media, ready);
    if (result == VoiceMediaSetupError::none) {
        ++connection->ready_revision;
        voice_participants_clear_sources(&connection->participants);
        connection->protocol_deadline_ms = 0;
    }
    return result;
}
bool voice_connection_drained(const VoiceConnection& connection) noexcept {
    return (connection.socket.phase == VoiceSocketPhase::empty ||
            connection.socket.phase == VoiceSocketPhase::closed) &&
           voice_udp_drained(connection.udp) && voice_udp_media_drained(connection.media_io);
}
u64 voice_connection_deadline(const VoiceConnection& connection) noexcept {
    return earliest_deadline(
        earliest_deadline(connection.protocol_deadline_ms, connection.reconnect_deadline_ms),
        earliest_deadline(voice_socket_deadline(connection.socket), connection.udp.deadline_ms));
}
void voice_connection_stop(VoiceConnection* connection) noexcept {
    if (!connection)
        return;
    secure_buffer_release(&connection->command);
    stop_media_io(connection);
    voice_media_clear(&connection->media);
    voice_participants_release(&connection->participants);
    voice_socket_stop(&connection->socket);
    voice_udp_close(&connection->udp);
    connection->identify_submitted = false;
    connection->protocol_deadline_ms = 0;
    connection->reconnect_deadline_ms = 0;
    connection->reconnect_pending = connection->resumed = false;
    connection->reconnect_attempts = 0;
    reset_value(&connection->dave);
    connection->sink_want_count = 0;
    connection->sink_wants_configured = connection->sink_wants_sent = false;
    connection->dave_reconnects = 0;
    connection->dave_reconnect_requested = false;
    connection->dave_close_code = 0;
    voice_connection_dave_reset(connection);
}
namespace {
u32 offered_cipher(const VoiceConnection& connection) noexcept {
    // Both primitives are implemented. AES is available on the Windows 10
    // target; XChaCha includes a portable path when its CNG provider is absent.
    return (connection.media.offered_modes & voice_transport_aes256_gcm_rtpsize)
               ? voice_transport_aes256_gcm_rtpsize
           : (connection.media.offered_modes & voice_transport_xchacha20_poly1305_rtpsize)
               ? voice_transport_xchacha20_poly1305_rtpsize
               : 0;
}
bool can_select(const VoiceConnection& connection) noexcept {
    return socket_live(connection) && connection.media.has_ready && !connection.media.selected_mode &&
           !connection.media.has_description && udp_ready(connection);
}
} // namespace
VoiceProtocolCommand voice_connection_protocol_command(const VoiceConnection& connection,
                                                       Slice<u8> output) noexcept {
    if (!can_select(connection))
        return {};
    const auto& external = connection.udp.external;
    VoiceProtocolCommand command{connection.ready_revision, offered_cipher(connection), {}};
    if (!external.address_bytes || external.address_bytes >= sizeof(external.address)) {
        command.write.error = GatewayCommandError::invalid_input;
        return command;
    }
    VoiceCodecOffer offers[kVoiceVideoMaximumCodecs];
    const u32 count = voice_video_decode_offers(connection.video_decodable, {offers, kVoiceVideoMaximumCodecs});
    for (u32 i = 0; i != count; ++i)
        command.video_offer |= voice_video_codec_bit(offers[i].codec);
    command.write = voice_gateway_write_select_protocol_media(
        {{{external.address, external.address_bytes}, external.port, command.mode},
         ConstSlice<VoiceCodecOffer>{offers, count},
         {connection.rtc_connection_id, kVoiceRtcConnectionIdBytes}},
        output);
    return command;
}
bool voice_connection_protocol_submitted(VoiceConnection* connection, const VoiceProtocolCommand& command,
                                         u64 now_ms) noexcept {
    if (!connection || !command.write.ok() || !command.write.bytes || !can_select(*connection) ||
        deadline_due(now_ms, connection->protocol_deadline_ms) ||
        command.ready_revision != connection->ready_revision || command.mode != offered_cipher(*connection) ||
        !voice_media_protocol_selected(&connection->media, command.mode))
        return false;
    connection->media.video_offer = command.video_offer;
    return true;
}
VoiceMediaSetupError voice_connection_description(VoiceConnection* connection, JsonValue data,
                                                  Slice<u32> scratch, u32 maximum_depth,
                                                  u64 now_ms) noexcept {
    if (!connection || !socket_live(*connection) || !udp_ready(*connection) ||
        deadline_due(now_ms, connection->protocol_deadline_ms))
        return VoiceMediaSetupError::order;
    // Must stay consistent with the actual Identify advertisement in prepare.
    const auto result = voice_media_description(&connection->media, data, scratch, maximum_depth,
                                                kDaveProtocolVersion, connection->ready_revision);
    if (result == VoiceMediaSetupError::none)
        connection->protocol_deadline_ms = 0;
    return result;
}
bool voice_connection_owns(const VoiceConnection& connection,
                           const NetworkCompletion& completion) noexcept {
    if (completion.kind == NetworkOperationKind::datagram_receive ||
        completion.kind == NetworkOperationKind::datagram_send)
        return completion.datagram == &connection.udp.socket;
    return completion.kind == NetworkOperationKind::resolve
               ? completion.resolver == &connection.socket.tls.resolver
               : (completion.kind == NetworkOperationKind::connect ||
                  completion.kind == NetworkOperationKind::send ||
                  completion.kind == NetworkOperationKind::receive) &&
                     completion.connection == &connection.socket.tls.connection;
}
VoiceParticipantsResult voice_connection_participant(VoiceConnection* connection, u8 opcode, JsonValue data,
                                                     Slice<u32> scratch, u32 maximum_depth) noexcept {
    if (!connection || !socket_live(*connection))
        return {VoiceParticipantsError::invalid_input};
    return voice_participants_event(&connection->participants, connection->socket.setup_generation, opcode,
                                    data, scratch, maximum_depth);
}
bool voice_connection_media_start(VoiceConnection* connection, Slice<u8> receive_buffer,
                                  Slice<u8> send_buffer, VoiceMappedMediaReceive receive,
                                  void* context, u32 receive_slots) noexcept {
    if (!connection || !receive || !socket_live(*connection) || !connection->media.has_description ||
        connection->ready_revision != connection->udp_revision)
        return false;
    const bool started = voice_udp_media_begin(&connection->media_io, &connection->udp,
                                               &connection->media.packets, connection->ready_revision,
                                               receive_buffer, send_buffer, receive_media, connection,
                                               receive_slots);
    if (started) {
        connection->media_receive = receive;
        connection->media_context = context;
        connection->unannounced_rtp_packets = 0;
        rtcp_transport_feedback_reset(&connection->transport_feedback);
        connection->rtp_extension_ids = 0;
        voice_log(kInfo, "media_started", "slots", receive_slots, "stream", connection->stream_context);
    }
    if (!started && connection->media_io.error != VoiceUdpMediaError::none)
        abort_call(connection, VoiceConnectionError::datagram, "media_start_failed",
                   static_cast<u64>(connection->media_io.error));
    return started;
}
bool voice_connection_media_current(const VoiceConnection& connection,
                                    const VoiceReceivedPacket& packet) noexcept {
    if (!packet.transport.ok() ||
        !current_media_transport(connection, packet.setup_generation, packet.ready_revision))
        return false;
    if (packet.transport.layout.kind == MediaPacketKind::rtcp)
        return !packet.source.user_id && !packet.member.user_id;
    if (packet.transport.layout.kind != MediaPacketKind::rtp || !packet.source.user_id ||
        packet.source.ssrc != packet.transport.layout.ssrc ||
        packet.member.user_id != packet.source.user_id ||
        connection.participants.generation != packet.setup_generation)
        return false;
    const auto current = voice_participants_source(connection.participants, packet.source.ssrc);
    return current.user_id == packet.source.user_id && current.revision == packet.source.revision &&
           current.primary_ssrc == packet.source.primary_ssrc && current.kind == packet.source.kind;
}
VoiceUdpMediaSend voice_connection_media_send(VoiceConnection* connection, u64 ready_revision,
                                              ConstSlice<u8> plaintext) noexcept {
    if (!connection || !socket_live(*connection) || !connection->media.has_description ||
        connection->ready_revision != ready_revision || connection->udp_revision != ready_revision)
        return VoiceUdpMediaSend::invalid;
    const auto result = voice_udp_media_send(&connection->media_io, ready_revision, plaintext);
    if (result == VoiceUdpMediaSend::failed)
        abort_call(connection, VoiceConnectionError::datagram, "media_send_failed",
                   static_cast<u64>(connection->media_io.error));
    return result;
}
namespace {
bool resume_pending(const VoiceConnection& connection) noexcept {
    return connection.reconnect_pending && connection.reconnect_action == VoiceCloseAction::resume;
}
// Decides once per authenticated socket, before its state is cleared. Local
// stops and handler/protocol failures never reconnect.
void plan_reconnect(VoiceConnection* connection, u64 now_ms) noexcept {
    const auto& socket = connection->socket;
    const bool dave = connection->dave_reconnect_requested;
    connection->dave_reconnect_requested = false;
    const bool remote_end = socket.close_code || socket.peer_closed || socket.error == VoiceSocketError::transport ||
                            socket.error == VoiceSocketError::heartbeat || socket.error == VoiceSocketError::timeout;
    if (connection->reconnect_pending || !connection->identify_submitted)
        return;
    voice_log(kWarning, "gateway_socket_ended", "close_code", socket.close_code, "socket_error",
              static_cast<u64>(socket.error), "peer_closed", socket.peer_closed);
    // Unrecoverable DAVE state (we closed with 4803/4804): a fresh session
    // (Identify) on the official client's MLS failure backoff, 1 s doubling
    // to 10 s, reset when a transition executes.
    if (dave && connection->error == VoiceConnectionError::none) {
        const u32 shift = connection->dave_reconnects > 4 ? 4u : connection->dave_reconnects - 1u;
        const u64 delay = minimum(kVoiceDaveReconnectMinimumMs << shift, kVoiceDaveReconnectMaximumMs);
        connection->reconnect_action = VoiceCloseAction::reconnect_fresh;
        connection->reconnect_pending = true;
        connection->reconnect_deadline_ms = deadline_after(now_ms, delay);
        voice_log(kWarning, "reconnect_scheduled", "action", static_cast<u64>(connection->reconnect_action),
                  "close_code", connection->dave_close_code, "delay_ms", delay);
        return;
    }
    if (!remote_end || connection->error != VoiceConnectionError::none ||
        connection->reconnect_attempts >= kVoiceReconnectAttempts ||
        socket.error == VoiceSocketError::protocol || socket.error == VoiceSocketError::storage ||
        socket.error == VoiceSocketError::random) {
        if (remote_end && connection->error == VoiceConnectionError::none)
            voice_log(kError, "reconnect_not_attempted", "attempts", connection->reconnect_attempts,
                      "socket_error", static_cast<u64>(socket.error));
        return;
    }
    auto action = voice_gateway_close_action(socket.close_code);
    if (action == VoiceCloseAction::resume && !connection->media.has_description)
        action = VoiceCloseAction::reconnect_fresh;
    if (action == VoiceCloseAction::stop) {
        voice_log(kWarning, "close_code_ends_call", "close_code", socket.close_code);
        return;
    }
    connection->reconnect_action = action;
    connection->reconnect_pending = true;
    connection->reconnect_deadline_ms = deadline_after(now_ms, 500u << connection->reconnect_attempts);
    ++connection->reconnect_attempts;
    voice_log(kWarning, "reconnect_scheduled", "action", static_cast<u64>(action), "attempt",
              connection->reconnect_attempts);
}
void retire_socket(VoiceConnection* connection, u64 now_ms) noexcept {
    plan_reconnect(connection, now_ms);
    connection->protocol_deadline_ms = 0;
    connection->identify_submitted = false;
    secure_buffer_release(&connection->command);
    // Resume keeps the negotiated UDP transport, cipher and participants.
    if (resume_pending(*connection))
        return;
    stop_media_io(connection);
    voice_media_clear(&connection->media);
    voice_participants_release(&connection->participants);
    voice_udp_close(&connection->udp);
    voice_connection_dave_reset(connection);
}
} // namespace
VoiceSocketSend voice_connection_speaking(VoiceConnection* connection, u32 flags, u64 now_ms) noexcept {
    if (!connection || !connection->identify_submitted || !connection->media.has_ready)
        return VoiceSocketSend::invalid;
    u8 payload[96];
    const auto encoded = voice_gateway_write_speaking(flags, connection->media.ssrc, {payload, sizeof(payload)});
    if (!encoded.ok())
        return VoiceSocketSend::invalid;
    return voice_socket_send(&connection->socket, WebSocketOpcode::text, {payload, encoded.bytes}, now_ms);
}
bool voice_connection_set_sink_wants(VoiceConnection* connection, u8 any_quality,
                                     ConstSlice<VoiceSinkWant> wants) noexcept {
    if (!connection || wants.count > kVoiceSinkWantsMaximum || (wants.count && !wants.data) ||
        !voice_gateway_write_media_sink_wants(any_quality, wants, {}).bytes)
        return false;
    bool same = connection->sink_wants_configured && connection->sink_any_quality == any_quality &&
                connection->sink_want_count == wants.count;
    for (u32 i = 0; same && i != wants.count; ++i) {
        bool found = false;
        for (u32 j = 0; !found && j != connection->sink_want_count; ++j)
            found = connection->sink_wants[j].ssrc == wants[i].ssrc &&
                    connection->sink_wants[j].quality == wants[i].quality;
        same = found;
    }
    if (same)
        return true;
    copy_bytes(connection->sink_wants, wants.data, wants.count * static_cast<u32>(sizeof(VoiceSinkWant)));
    connection->sink_want_count = wants.count;
    connection->sink_any_quality = any_quality;
    connection->sink_wants_configured = true;
    connection->sink_wants_sent = false;
    return true;
}
namespace {
// Op 15 after Session Description, when the socket has no output in flight.
bool flush_sink_wants(VoiceConnection* connection, u64 now_ms) noexcept {
    if (connection->sink_wants_sent || !connection->media.has_description ||
        connection->socket.sending != VoiceSocketOutput::none)
        return true;
    u8 payload[64 + kVoiceSinkWantsMaximum * 16];
    const u8 any = connection->sink_wants_configured ? connection->sink_any_quality : kVoiceSinkWantsDefaultQuality;
    const auto encoded = voice_gateway_write_media_sink_wants(
        any, {connection->sink_wants, connection->sink_wants_configured ? connection->sink_want_count : 0},
        {payload, sizeof(payload)});
    if (!encoded.ok())
        return false;
    const auto sent = voice_socket_send(&connection->socket, WebSocketOpcode::text, {payload, encoded.bytes}, now_ms);
    if (sent == VoiceSocketSend::accepted)
        connection->sink_wants_sent = true;
    return sent == VoiceSocketSend::accepted || sent == VoiceSocketSend::busy;
}
// Connection phase transitions and receive-side packet rejections.
void log_progress(VoiceConnection* connection, u64 now_ms) noexcept {
    if (connection->socket.phase != connection->logged_socket_phase) {
        voice_log(kInfo, "socket_phase", "phase", static_cast<u64>(connection->socket.phase), "from",
                  static_cast<u64>(connection->logged_socket_phase), "stream", connection->stream_context);
        connection->logged_socket_phase = connection->socket.phase;
    }
    if (connection->udp.phase != connection->logged_udp_phase) {
        voice_log(kInfo, "udp_phase", "phase", static_cast<u64>(connection->udp.phase), "from",
                  static_cast<u64>(connection->logged_udp_phase));
        connection->logged_udp_phase = connection->udp.phase;
    }
    if (connection->media.has_description != connection->logged_description) {
        connection->logged_description = connection->media.has_description;
        voice_log(kInfo, connection->logged_description ? "transport_keyed" : "transport_cleared");
    }
    const u64 rejected = connection->media_io.rejected_packets;
    if (rejected < connection->logged_rejected_packets)
        connection->logged_rejected_packets = 0;
    u64 suppressed = 0;
    if (rejected != connection->logged_rejected_packets &&
        voice_log_limit(&connection->rejected_log, now_ms, &suppressed)) {
        voice_log(kWarning, "udp_packets_rejected", "count", rejected - connection->logged_rejected_packets,
                  "packet_error", static_cast<u64>(connection->media_io.packet_error));
        connection->logged_rejected_packets = rejected;
    }
}
} // namespace
void voice_connection_drive(VoiceConnection* connection, const VoiceConnectionSetup& setup,
                            NetworkLoop* loop, u64 now_ms) noexcept {
    if (!connection)
        return;
    log_progress(connection, now_ms);
    VoiceSetupCredentials credentials{};
    if (!voice_setup_credentials(setup, &credentials)) {
        voice_connection_stop(connection);
        return;
    }
    if (connection->socket.setup_generation == credentials.generation &&
        deadline_due(now_ms, connection->protocol_deadline_ms)) {
        abort_call(connection, VoiceConnectionError::protocol_timeout, "select_protocol_timeout");
        return;
    }
    if (connection->socket.setup_generation != credentials.generation) {
        voice_connection_stop(connection);
        if (!voice_connection_drained(*connection))
            return;
    }
    // An ended WebSocket leaves UDP alive only for a pending resume; otherwise
    // all transports drain before reuse.
    const bool socket_ended = connection->socket.phase == VoiceSocketPhase::closed ||
                              connection->socket.phase == VoiceSocketPhase::closing;
    if (socket_ended) {
        retire_socket(connection, now_ms);
        if (!resume_pending(*connection) && !voice_udp_drained(connection->udp))
            return;
    }
    if (resume_pending(*connection) && connection->socket.phase != VoiceSocketPhase::closing) {
        if (!deadline_due(now_ms, connection->reconnect_deadline_ms))
            return;
        connection->reconnect_pending = false;
        connection->reconnect_deadline_ms = 0;
        GatewayEndpoint endpoint{};
        if (voice_gateway_endpoint(credentials.endpoint, &endpoint) != GatewayEndpointError::none) {
            abort_call(connection, VoiceConnectionError::endpoint, "endpoint_invalid");
            return;
        }
        if (!prepare(connection, credentials, true)) {
            voice_connection_stop(connection);
            return;
        }
        if (!voice_socket_begin(&connection->socket, loop, endpoint, credentials.generation, 128 * 1024,
                                event, connection, now_ms, true)) {
            abort_call(connection, VoiceConnectionError::socket, "socket_begin_failed", 1);
            return;
        }
        connection->resumed = true;
    } else if (voice_connection_drained(*connection)) {
        if (connection->reconnect_pending) {
            if (!deadline_due(now_ms, connection->reconnect_deadline_ms))
                return;
            // A fresh session re-identifies with the same credential generation.
            connection->reconnect_pending = false;
            connection->reconnect_deadline_ms = 0;
            connection->attempted_generation = 0;
        }
        if (connection->attempted_generation == credentials.generation) {
            secure_buffer_release(&connection->command);
            if (connection->error == VoiceConnectionError::none &&
                (connection->socket.error != VoiceSocketError::none || connection->socket.close_code >= 4000))
                fail(connection, VoiceConnectionError::socket, "socket_ended", connection->socket.close_code);
            return;
        }
        connection->attempted_generation = credentials.generation;
        connection->resumed = false;
        connection->error = VoiceConnectionError::none;
        connection->media_error = VoiceMediaSetupError::none;
        reset_value(&connection->participant_error);
        GatewayEndpoint endpoint{};
        if (voice_gateway_endpoint(credentials.endpoint, &endpoint) != GatewayEndpointError::none) {
            fail(connection, VoiceConnectionError::endpoint, "endpoint_invalid");
            return;
        }
        if (!prepare(connection, credentials, false))
            return;
        if (!voice_socket_begin(&connection->socket, loop, endpoint, credentials.generation, 128 * 1024,
                                event, connection, now_ms)) {
            abort_call(connection, VoiceConnectionError::socket, "socket_begin_failed");
            return;
        }
    }
    voice_socket_tick(&connection->socket, now_ms);
    if (connection->socket.phase == VoiceSocketPhase::closing ||
        connection->socket.phase == VoiceSocketPhase::closed) {
        retire_socket(connection, now_ms);
        if (connection->socket.error != VoiceSocketError::none && !connection->reconnect_pending &&
            connection->error == VoiceConnectionError::none)
            fail(connection, VoiceConnectionError::socket, "socket_failed",
                 static_cast<u64>(connection->socket.error));
        return;
    }
    if (connection->media.has_ready && connection->identify_submitted &&
        connection->socket.phase == VoiceSocketPhase::open) {
        if (connection->udp_revision != connection->ready_revision) {
            stop_media_io(connection);
            voice_udp_close(&connection->udp);
            if (!voice_udp_drained(connection->udp) || !voice_udp_media_drained(connection->media_io))
                return;
            connection->udp_revision = connection->ready_revision;
            if (!voice_udp_begin(
                    &connection->udp, loop, reinterpret_cast<const sockaddr*>(&connection->media.endpoint),
                    static_cast<i32>(connection->media.endpoint_bytes), connection->media.ssrc, now_ms)) {
                abort_call(connection, VoiceConnectionError::datagram, "udp_begin_failed");
                return;
            }
        }
        voice_udp_tick(&connection->udp, now_ms);
        if (connection->udp.error != VoiceUdpError::none) {
            abort_call(connection, VoiceConnectionError::datagram, "udp_discovery_failed",
                       static_cast<u64>(connection->udp.error));
            return;
        }
        if (connection->udp.phase == VoiceUdpPhase::ready && !connection->media.has_description) {
            if (!connection->media.selected_mode && !offered_cipher(*connection)) {
                abort_call(connection, VoiceConnectionError::protocol, "no_supported_cipher",
                           connection->media.offered_modes);
                return;
            }
            if (!connection->protocol_deadline_ms)
                connection->protocol_deadline_ms = deadline_after(now_ms, 20'000);
            if (now_ms >= connection->protocol_deadline_ms) {
                abort_call(connection, VoiceConnectionError::protocol_timeout,
                           "session_description_timeout");
                return;
            }
            // Avoid re-encoding during backpressure; socket completion/timers
            // wake the worker, with the protocol deadline bounding the wait.
            if (!connection->media.selected_mode && connection->socket.sending == VoiceSocketOutput::none) {
                u8 payload[1024];
                const auto command =
                    voice_connection_protocol_command(*connection, {payload, sizeof(payload)});
                if (!command.ready_revision || !command.write.ok()) {
                    abort_call(connection, VoiceConnectionError::protocol, "select_protocol_encode_failed");
                    return;
                }
                const auto sent = voice_socket_send(&connection->socket, WebSocketOpcode::text,
                                                    {payload, command.write.bytes}, now_ms);
                if (sent == VoiceSocketSend::accepted) {
                    if (!voice_connection_protocol_submitted(connection, command, now_ms))
                        abort_call(connection, VoiceConnectionError::protocol, "select_protocol_rejected");
                } else if (sent != VoiceSocketSend::busy)
                    abort_call(connection, VoiceConnectionError::socket, "select_protocol_send_failed",
                               static_cast<u64>(sent));
            }
        }
    }
    if (connection->dave_close_code) {
        if (connection->socket.phase == VoiceSocketPhase::open && !connection->socket.pending_close &&
            voice_socket_close(&connection->socket, connection->dave_close_code, now_ms))
            voice_log(kWarning, "gateway_close_sent", "code", connection->dave_close_code);
        return;
    }
    if (!connection->identify_submitted && connection->socket.phase == VoiceSocketPhase::open) {
        const auto sent = voice_socket_send(&connection->socket, WebSocketOpcode::text,
                                            secure_buffer_view(connection->command), now_ms);
        if (sent == VoiceSocketSend::accepted) {
            connection->identify_submitted = true;
            secure_buffer_release(&connection->command);
        } else if (sent != VoiceSocketSend::busy)
            abort_call(connection, VoiceConnectionError::socket, "identify_send_failed",
                       static_cast<u64>(sent));
    } else if (connection->identify_submitted && connection->socket.phase == VoiceSocketPhase::open &&
               (!voice_connection_dave_flush(connection, now_ms) || !flush_sink_wants(connection, now_ms)))
        abort_call(connection, VoiceConnectionError::socket, "signaling_send_failed");
}
void voice_connection_completion(VoiceConnection* connection, const VoiceConnectionSetup& setup,
                                 NetworkLoop* loop, const NetworkCompletion& completion,
                                 u64 now_ms) noexcept {
    if (!connection || !voice_connection_owns(*connection, completion))
        return;
    // A queued late response must not install a key before the next drive sees
    // the expired negotiation deadline. Retire first, then drain completion.
    if (deadline_due(now_ms, connection->protocol_deadline_ms))
        abort_call(connection, VoiceConnectionError::protocol_timeout, "select_protocol_timeout", 1);
    // Poll has already retired resolver.pending. Stop may therefore finish
    // synchronously; do not begin a new resolver in the same owner until this
    // old completion has been delivered/discarded.
    if (!voice_setup_current(setup, connection->socket.setup_generation))
        voice_connection_stop(connection);
    if (completion.kind == NetworkOperationKind::datagram_receive ||
        completion.kind == NetworkOperationKind::datagram_send) {
        // As with resolver replacement, consume the old completion before any
        // new Ready revision can reuse this datagram owner.
        if (connection->udp_revision != connection->ready_revision) {
            stop_media_io(connection);
            voice_udp_close(&connection->udp);
        }
        const bool was_media = !voice_udp_media_drained(connection->media_io);
        voice_udp_media_completion(&connection->media_io, completion);
        if (was_media && connection->media_io.stopping) {
            if (connection->media_io.error != VoiceUdpMediaError::none)
                fail(connection, VoiceConnectionError::datagram, "media_io_failed",
                     static_cast<u64>(connection->media_io.error) |
                         static_cast<u64>(connection->media_io.packet_error) << 8 |
                         static_cast<u64>(connection->media_io.native_error) << 16);
            voice_connection_stop(connection);
        }
        voice_udp_completion(&connection->udp, completion, now_ms);
    } else
        voice_socket_completion(&connection->socket, loop, completion, now_ms);
    voice_connection_drive(connection, setup, loop, now_ms);
    log_progress(connection, now_ms);
}
} // namespace maicord
