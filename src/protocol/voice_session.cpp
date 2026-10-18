#include "maicord/protocol/voice_session.hpp"
#include "maicord/core/memory.hpp"


namespace maicord {
namespace {
VoiceSessionEvent fail(VoiceSession* session, VoiceSessionError error) noexcept {
    session->failed = true;
    voice_heartbeat_disconnect(&session->heartbeat);
    VoiceSessionEvent event{};
    event.kind = VoiceSessionEventKind::error;
    event.error = error;
    return event;
}
bool overlaps(const void* a, u32 a_bytes, const void* b, u32 b_bytes) noexcept {
    const auto first = reinterpret_cast<usize>(a), second = reinterpret_cast<usize>(b);
    return first <= second ? second - first < a_bytes : first - second < b_bytes;
}
} // namespace

bool voice_session_initialize(VoiceSession* session, Slice<u8> input, Slice<u8> fragments,
                              Slice<u32> scratch, ConstSlice<u8> client_key, u32 maximum_frame_bytes,
                              u32 maximum_message_bytes, u64 now_ms, bool resume) noexcept {
    if (!input.data || input.count < 14 || !fragments.data || !scratch.data || !scratch.count ||
        scratch.count > ~u32{0} / sizeof(u32) || !client_key.data ||
        client_key.count != kWebSocketClientKeyBytes || !maximum_frame_bytes ||
        maximum_frame_bytes > input.count - 14 || !maximum_message_bytes ||
        maximum_message_bytes > fragments.count ||
        overlaps(input.data, input.count, fragments.data, fragments.count) ||
        overlaps(input.data, input.count, scratch.data, scratch.count * sizeof(u32)) ||
        overlaps(fragments.data, fragments.count, scratch.data, scratch.count * sizeof(u32)))
        return false;
    if (!voice_heartbeat_start(&session->heartbeat, now_ms, resume))
        return false;
    u8 key[kWebSocketClientKeyBytes];
    copy_bytes(key, client_key.data, sizeof(key));
    if (session->input.data)
        secure_clear_bytes(session->input.data, session->input.capacity);
    if (session->fragments.data)
        secure_clear_bytes(session->fragments.data, session->fragments.count);
    byte_queue_initialize(&session->input, input);
    session->fragments = fragments;
    session->scratch = scratch;
    session->tracker = {};
    session->fragment_bytes = 0;
    session->maximum_frame_bytes = maximum_frame_bytes;
    session->maximum_message_bytes = maximum_message_bytes;
    copy_bytes(session->client_key, key, sizeof(key));
    session->opened = session->failed = session->closed = false;
    return true;
}
bool voice_session_push(VoiceSession* session, ConstSlice<u8> decrypted) noexcept {
    return !session->failed && !session->closed && byte_queue_append(&session->input, decrypted);
}

VoiceSessionEvent voice_session_next(VoiceSession* session, u64 now_ms) noexcept {
    if (!session->input.data || session->failed)
        return fail(session, VoiceSessionError::invalid_input);
    if (session->closed)
        return {};
    if (session->heartbeat.phase == VoiceHeartbeatPhase::failed)
        return fail(session, VoiceSessionError::heartbeat);
    if (!session->opened) {
        const auto opening = websocket_validate_opening_response(
            byte_queue_readable(session->input), {session->client_key, kWebSocketClientKeyBytes},
            minimum(session->input.capacity, 16'384u), 64);
        if (opening.state == WebSocketHandshakeState::need_more)
            return {};
        if (!opening.ok()) {
            auto result = fail(session, VoiceSessionError::handshake);
            result.handshake_error = opening.error;
            return result;
        }
        if (!byte_queue_consume(&session->input, opening.header_bytes))
            return fail(session, VoiceSessionError::invalid_input);
        session->opened = true;
        VoiceSessionEvent result{};
        result.kind = VoiceSessionEventKind::opened;
        return result;
    }
    const auto packet = websocket_read_message(&session->input, &session->tracker, session->fragments,
                                               &session->fragment_bytes, session->maximum_frame_bytes,
                                               session->maximum_message_bytes);
    if (packet.state == WebSocketReadState::need_input)
        return {};
    if (packet.state == WebSocketReadState::error) {
        auto result = fail(session, VoiceSessionError::websocket);
        result.websocket_error = packet.error;
        return result;
    }
    VoiceSessionEvent result{};
    if (packet.state == WebSocketReadState::control) {
        result.control = packet.payload;
        if (packet.opcode == WebSocketOpcode::ping)
            result.kind = VoiceSessionEventKind::ping;
        else if (packet.opcode == WebSocketOpcode::pong)
            result.kind = VoiceSessionEventKind::pong;
        else {
            result.kind = VoiceSessionEventKind::closed;
            result.close_code = packet.payload.count >= 2
                                    ? static_cast<u16>((packet.payload[0] << 8) | packet.payload[1])
                                    : 0;
            session->closed = true;
            voice_heartbeat_disconnect(&session->heartbeat);
        }
        return result;
    }
    if (packet.opcode == WebSocketOpcode::binary) {
        if (!voice_gateway_decode_binary(packet.payload, &result.binary))
            return fail(session, VoiceSessionError::binary);
        voice_heartbeat_sequence(&session->heartbeat, session->heartbeat.generation,
                                 result.binary.sequence);
        result.kind = VoiceSessionEventKind::binary;
        return result;
    }
    for (;;) {
        const auto decoded = voice_gateway_decode_json(
            {reinterpret_cast<const char*>(packet.payload.data), packet.payload.count}, session->scratch,
            64, &result.envelope);
        if (decoded.ok())
            break;
        if (decoded.scan_error != JsonScanError::output_overflow || !session->grow_scratch ||
            session->scratch.count >= packet.payload.count)
            return fail(session, VoiceSessionError::json);
        const u32 required = session->scratch.count > packet.payload.count / 2 ? packet.payload.count
                                                                               : session->scratch.count * 2;
        const auto grown = session->grow_scratch(session->scratch_context, required);
        if (!grown.data || grown.count < required || grown.count <= session->scratch.count ||
            grown.count > ~u32{0} / sizeof(u32) ||
            overlaps(grown.data, grown.count * sizeof(u32), session->input.data, session->input.capacity) ||
            overlaps(grown.data, grown.count * sizeof(u32), session->fragments.data,
                     session->fragments.count))
            return fail(session, VoiceSessionError::json);
        session->scratch = grown;
    }
    const auto generation = session->heartbeat.generation;
    switch (result.envelope.opcode) {
    case 8:
        if (!gateway_decode_hello(result.envelope.data, session->scratch, 16, &result.hello).ok())
            return fail(session, VoiceSessionError::event);
        if (!voice_heartbeat_hello(&session->heartbeat, generation, result.hello.heartbeat_interval_ms,
                                   now_ms))
            return fail(session, VoiceSessionError::heartbeat);
        result.kind = VoiceSessionEventKind::hello;
        break;
    case 3:
        if (!voice_heartbeat_request(&session->heartbeat, generation))
            return fail(session, VoiceSessionError::heartbeat);
        result.kind = VoiceSessionEventKind::heartbeat_requested;
        break;
    case 6: {
        u64 nonce = 0;
        if (!voice_gateway_decode_heartbeat_ack(result.envelope.data, session->scratch, 16, &nonce).ok())
            return fail(session, VoiceSessionError::event);
        result.kind = voice_heartbeat_ack(&session->heartbeat, generation, nonce, now_ms)
                          ? VoiceSessionEventKind::heartbeat_acknowledged
                          : VoiceSessionEventKind::heartbeat_ack_ignored;
        if (session->heartbeat.phase == VoiceHeartbeatPhase::failed)
            return fail(session, VoiceSessionError::heartbeat);
        break;
    }
    default:
        result.kind = VoiceSessionEventKind::json;
        break;
    }
    if (result.envelope.has_sequence)
        voice_heartbeat_sequence(&session->heartbeat, generation, result.envelope.sequence);
    return result;
}

void voice_session_clear(VoiceSession* session) noexcept {
    voice_heartbeat_stop(&session->heartbeat);
    if (session->input.data)
        secure_clear_bytes(session->input.data, session->input.capacity);
    if (session->fragments.data)
        secure_clear_bytes(session->fragments.data, session->fragments.count);
    byte_queue_clear(&session->input, false);
    session->tracker = {};
    session->fragment_bytes = 0;
    session->closed = true;
}
} // namespace maicord
