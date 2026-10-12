#include "maicord/protocol/gateway_session.hpp"
#include "maicord/protocol/websocket_message.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {

GatewaySessionEvent event(GatewaySessionEventKind kind) noexcept {
    GatewaySessionEvent result{};
    result.kind = kind;
    return result;
}

GatewaySessionEvent session_error(GatewaySessionError error) noexcept {
    GatewaySessionEvent result{};
    result.error = error;
    result.kind = GatewaySessionEventKind::error;
    return result;
}

bool spill_disjoint(const GatewaySession& session, Slice<u8> spill) noexcept {
    const ConstSlice<u8> candidate{spill.data, spill.count};
    const ConstSlice<u8> others[]{
        {session.input.data, session.input.capacity},
        {session.inline_compressed_storage.data, session.inline_compressed_storage.count},
        {session.message_storage.data, session.message_storage.count},
        {session.wire_message_storage.data, session.wire_message_storage.count},
        {reinterpret_cast<const u8*>(&session), sizeof(session)}};
    if (!candidate.data || !byte_range_valid(candidate)) return false;
    for (const auto other : others)
        if (byte_ranges_overlap(candidate, other)) return false;
    return true;
}

// Room for `bytes` more compressed bytes, spilling once past the initial storage.
bool reserve_compressed(GatewaySession* session, u32 bytes) noexcept {
    if (session->compressed_bytes > session->compressed_storage.count) return false;
    if (bytes <= session->compressed_storage.count - session->compressed_bytes) return true;
    if (!session->grow_compressed || bytes > session->maximum_compressed_bytes - session->compressed_bytes)
        return false;
    const u32 required = session->compressed_bytes + bytes;
    const bool spilled = session->compressed_storage.data != session->inline_compressed_storage.data;
    Slice<u8> backed{};
    if (!session->grow_compressed(session->grow_compressed_context, required, &backed) || backed.count < required ||
        (spilled && backed.data != session->compressed_storage.data) || !spill_disjoint(*session, backed))
        return false;
    if (!spilled) copy_bytes(backed.data, session->compressed_storage.data, session->compressed_bytes);
    session->compressed_storage = backed;
    return true;
}

bool append_compressed(GatewaySession* session,
                       ConstSlice<u8> payload) noexcept {
    if (!reserve_compressed(session, payload.count)) {
        return false;
    }
    copy_bytes(session->compressed_storage.data + session->compressed_bytes, payload.data, payload.count);
    session->compressed_bytes += payload.count;
    return true;
}

// Moves the received part of the current binary frame into compressed storage.
bool stream_frame_payload(GatewaySession* session) noexcept {
    const ConstSlice<u8> readable = byte_queue_readable(session->input);
    const u32 take = minimum(readable.count, session->stream_remaining);
    if (!take) return true;
    if (!append_compressed(session, {readable.data, take})) return false;
    session->stream_remaining -= take;
    return byte_queue_consume(&session->input, take);
}

u32 complete_sync_flush_bytes(const GatewaySession& session) noexcept {
    if (session.compressed_bytes < 4) {
        return 0;
    }
    const u32 end = session.compressed_bytes;
    return session.compressed_storage[end - 4] == 0x00 &&
                   session.compressed_storage[end - 3] == 0x00 &&
                   session.compressed_storage[end - 2] == 0xFF &&
                   session.compressed_storage[end - 1] == 0xFF
               ? end
               : 0;
}

void consume_compressed(GatewaySession* session, u32 bytes) noexcept {
    const u32 remaining = session->compressed_bytes - bytes;
    copy_bytes(session->compressed_storage.data, session->compressed_storage.data + bytes, remaining);
    SecureZeroMemory(session->compressed_storage.data + remaining, bytes);
    session->compressed_bytes = remaining;
    // A spilled payload is done: later payloads start in the initial storage.
    if (session->compressed_storage.data != session->inline_compressed_storage.data &&
        remaining <= session->inline_compressed_storage.count) {
        copy_bytes(session->inline_compressed_storage.data, session->compressed_storage.data, remaining);
        SecureZeroMemory(session->compressed_storage.data, remaining);
        session->compressed_storage = session->inline_compressed_storage;
        session->grow_compressed(session->grow_compressed_context, 0, nullptr);
    }
}

GatewaySessionEvent process_gateway_message(GatewaySession* session, Utf8View message, u32 heartbeat_jitter,
                                            u64 now_ms) noexcept {
    GatewaySessionEvent result{};
    GatewayDecodeResult decoded =
        gateway_decode_envelope(message, session->structural_scratch, 64, &result.envelope);
    // The frame (or one persistent zlib flush) has already been consumed.
    // Rescan the pinned JSON bytes only; never ask the transport to replay it.
    while (!decoded.ok() && decoded.error == GatewayDecodeError::scan_failed &&
           decoded.scan_error == JsonScanError::output_overflow && session->grow_index &&
           session->structural_scratch.count < kGatewaySessionMaximumStructuralCount) {
        const u32 current = session->structural_scratch.count;
        const u32 required = current > kGatewaySessionMaximumStructuralCount / 2
                                 ? kGatewaySessionMaximumStructuralCount : current * 2;
        Slice<u32> backed{};
        if (!session->grow_index(session->grow_index_context, required, &backed) ||
            backed.data != session->structural_scratch.data || backed.count < required ||
            backed.count > kGatewaySessionMaximumStructuralCount) break;
        session->structural_scratch = backed;
        result.envelope = {};
        decoded = gateway_decode_envelope(message, backed, 64, &result.envelope);
    }
    if (!decoded.ok()) {
        result.kind = GatewaySessionEventKind::error;
        result.error = GatewaySessionError::gateway_decode;
        result.gateway_error = decoded.error;
        return result;
    }

    if (result.envelope.opcode == 10) {
        const GatewayEventDecodeResult hello =
            gateway_decode_hello(result.envelope.data, session->structural_scratch, 16, &result.hello);
        if (!hello.ok()) {
            result.kind = GatewaySessionEventKind::error;
            result.error = GatewaySessionError::event_decode;
            result.event_error = hello.error;
            return result;
        }
        result.lifecycle_step =
            gateway_lifecycle_hello(&session->lifecycle, result.hello, heartbeat_jitter, now_ms);
        result.kind = GatewaySessionEventKind::hello;
    } else if (result.envelope.opcode == 11) {
        result.lifecycle_step = gateway_lifecycle_heartbeat_acknowledged(&session->lifecycle);
        result.kind = GatewaySessionEventKind::heartbeat_acknowledged;
    } else if (result.envelope.opcode == 1) {
        result.lifecycle_step = gateway_lifecycle_heartbeat_requested(&session->lifecycle);
        result.kind = GatewaySessionEventKind::heartbeat_requested;
    } else if (result.envelope.opcode == 7) {
        result.lifecycle_step = gateway_lifecycle_reconnect_requested(&session->lifecycle);
        result.kind = GatewaySessionEventKind::reconnect_requested;
    } else if (result.envelope.opcode == 9) {
        bool resumable = false;
        const GatewayEventDecodeResult invalid =
            gateway_decode_invalid_session(result.envelope.data, &resumable);
        if (!invalid.ok()) {
            result.kind = GatewaySessionEventKind::error;
            result.error = GatewaySessionError::event_decode;
            result.event_error = invalid.error;
            return result;
        }
        result.lifecycle_step = gateway_lifecycle_invalid_session(&session->lifecycle, resumable);
        result.kind = GatewaySessionEventKind::invalid_session;
    } else if (result.envelope.opcode == 0) {
        if (!result.envelope.has_sequence || !result.envelope.has_event_name) {
            result.kind = GatewaySessionEventKind::error;
            result.error = GatewaySessionError::gateway_decode;
            result.gateway_error = result.envelope.has_sequence ? GatewayDecodeError::invalid_event_name
                                                                : GatewayDecodeError::invalid_sequence;
            return result;
        }
        result.lifecycle_step = gateway_lifecycle_dispatch(&session->lifecycle, result.envelope.sequence);
        if (json_string_equals_ascii(result.envelope.event_name, utf8_literal("READY"))) {
            const GatewayEventDecodeResult ready =
                gateway_decode_ready(result.envelope.data, session->structural_scratch, 64, &result.ready);
            if (!ready.ok()) {
                result.kind = GatewaySessionEventKind::error;
                result.error = GatewaySessionError::event_decode;
                result.event_error = ready.error;
                return result;
            }
            result.lifecycle_step = gateway_lifecycle_session_ready(&session->lifecycle, true);
            result.kind = GatewaySessionEventKind::ready;
        } else if (json_string_equals_ascii(result.envelope.event_name, utf8_literal("RESUMED"))) {
            const GatewayEventDecodeResult resumed = gateway_decode_resumed(
                result.envelope.data, session->structural_scratch, 16, &result.resumed);
            if (!resumed.ok()) {
                result.kind = GatewaySessionEventKind::error;
                result.error = GatewaySessionError::event_decode;
                result.event_error = resumed.error;
                return result;
            }
            result.lifecycle_step = gateway_lifecycle_session_ready(&session->lifecycle, true);
            result.kind = GatewaySessionEventKind::resumed;
        } else {
            result.kind = GatewaySessionEventKind::dispatch;
        }
    } else {
        result.kind = GatewaySessionEventKind::other_gateway_event;
    }

    if (!result.lifecycle_step.ok()) {
        result.kind = GatewaySessionEventKind::error;
        result.error = GatewaySessionError::lifecycle;
    }
    return result;
}

bool output_disjoint(const GatewaySession& session, Slice<u8> candidate) noexcept {
    if (session.structural_scratch.count > ~0u / sizeof(u32)) return false;
    const ConstSlice<u8> output{candidate.data, candidate.count};
    const ConstSlice<u8> others[]{
        {session.input.data, session.input.capacity},
        {session.compressed_storage.data, session.compressed_storage.count},
        {session.wire_message_storage.data, session.wire_message_storage.count},
        {reinterpret_cast<const u8*>(session.structural_scratch.data),
            static_cast<u32>(session.structural_scratch.count * sizeof(u32))},
        {reinterpret_cast<const u8*>(&session), sizeof(session)},
        {reinterpret_cast<const u8*>(session.zlib_stream), sizeof(*session.zlib_stream)}};
    if (!output.data || !output.count || !byte_range_valid(output)) return false;
    for (const auto other : others)
        if (!byte_range_valid(other) || byte_ranges_overlap(output, other)) return false;
    return true;
}
bool grow_session_output(void* opaque, u32 required, Slice<u8>* backed) noexcept {
    auto* session = static_cast<GatewaySession*>(opaque);
    Slice<u8> candidate{};
    if (!session || !backed || !session->grow_output ||
        !session->grow_output(session->grow_output_context, required, &candidate) ||
        !output_disjoint(*session, candidate)) return false;
    *backed = candidate;
    return true;
}
GatewaySessionEvent process_compressed_message(
    GatewaySession* session, u32 compressed_bytes,
    u32 heartbeat_jitter, u64 now_ms) noexcept {
    const DeflateInputSpan span{session->compressed_storage.data,
                                compressed_bytes};
    const ZlibInflateResult inflated = session->grow_output ? zlib_stream_inflate_flush_growing(
        session->zlib_stream, {&span, 1}, &session->message_storage,
        session->maximum_decoded_message_bytes, grow_session_output, session) :
        zlib_stream_inflate_flush(session->zlib_stream, {&span, 1}, session->message_storage);
    if (!inflated.ok()) {
        GatewaySessionEvent result =
            session_error(GatewaySessionError::compression);
        result.compression_error = inflated.error;
        return result;
    }
    consume_compressed(session, compressed_bytes);
    return process_gateway_message(
        session,
        Utf8View{reinterpret_cast<const char*>(session->message_storage.data),
                 inflated.output_bytes},
        heartbeat_jitter, now_ms);
}

} // namespace

bool gateway_session_initialize(GatewaySession* session, Slice<u8> input_storage, Slice<u8> message_storage,
                                Slice<u32> structural_scratch, ConstSlice<u8> client_key,
                                u64 maximum_frame_payload_bytes, u64 maximum_message_bytes,
                                bool resume_available, bool has_sequence, u64 last_sequence) noexcept {
    if (!session || !input_storage.data || input_storage.count == 0 || !message_storage.data ||
        message_storage.count == 0 || !structural_scratch.data || structural_scratch.count == 0 ||
        !client_key.data || client_key.count != kWebSocketClientKeyBytes ||
        maximum_frame_payload_bytes == 0 || input_storage.count < 14 ||
        maximum_frame_payload_bytes > input_storage.count - 14 || maximum_message_bytes == 0 ||
        maximum_message_bytes > message_storage.count) {
        return false;
    }
    *session = GatewaySession{};
    if (!byte_queue_initialize(&session->input, input_storage)) {
        return false;
    }
    session->message_storage = message_storage;
    session->wire_message_storage = message_storage;
    session->structural_scratch = structural_scratch;
    session->maximum_frame_payload_bytes = maximum_frame_payload_bytes;
    session->maximum_message_bytes = maximum_message_bytes;
    session->maximum_decoded_message_bytes = message_storage.count;
    session->maximum_header_bytes = input_storage.count < 32 * 1024 ? input_storage.count : 32 * 1024;
    session->maximum_header_fields = 128;
    for (u32 index = 0; index < kWebSocketClientKeyBytes; ++index) {
        session->client_key[index] = client_key[index];
    }
    gateway_lifecycle_initialize(&session->lifecycle);
    session->lifecycle.has_sequence = has_sequence;
    session->lifecycle.last_sequence = last_sequence;
    session->lifecycle.resume_available = resume_available && has_sequence;
    return true;
}

bool gateway_session_push(GatewaySession* session, ConstSlice<u8> plaintext) noexcept {
    return session && byte_queue_append(&session->input, plaintext);
}

bool gateway_session_enable_zlib_stream(
    GatewaySession* session, ZlibStreamInflater* inflater,
    Slice<u8> compressed_storage) noexcept {
    if (!session || session->opened || session->transport_compressed ||
        !inflater || !compressed_storage.data ||
        compressed_storage.count == 0 || session->message_bytes != 0 ||
        session->compressed_bytes != 0 ||
        !zlib_stream_initialize(inflater)) {
        return false;
    }
    session->zlib_stream = inflater;
    session->compressed_storage = compressed_storage;
    session->inline_compressed_storage = compressed_storage;
    session->maximum_compressed_bytes = compressed_storage.count;
    session->transport_compressed = true;
    return true;
}

bool gateway_session_enable_compressed_growth(GatewaySession* session, u32 maximum_compressed_bytes,
    GatewaySessionGrowCompressed grow, void* context) noexcept {
    if (!session || session->opened || !session->transport_compressed || session->grow_compressed || !grow ||
        maximum_compressed_bytes < session->inline_compressed_storage.count ||
        maximum_compressed_bytes > kGatewaySessionMaximumDecodedMessageBytes) return false;
    session->grow_compressed = grow;
    session->grow_compressed_context = context;
    session->maximum_compressed_bytes = maximum_compressed_bytes;
    return true;
}

bool gateway_session_enable_output_growth(GatewaySession* session, Slice<u8> decoded_output,
    u32 maximum_decoded_bytes, ZlibStreamGrowOutput grow, void* context) noexcept {
    if (!session || session->opened || !session->transport_compressed || !session->zlib_stream ||
        session->grow_output || !grow || maximum_decoded_bytes < decoded_output.count ||
        maximum_decoded_bytes > kGatewaySessionMaximumDecodedMessageBytes) return false;
    if (!output_disjoint(*session, decoded_output)) return false;
    session->message_storage = decoded_output;
    session->maximum_decoded_message_bytes = maximum_decoded_bytes;
    session->grow_output = grow;
    session->grow_output_context = context;
    return true;
}

GatewaySessionEvent gateway_session_next(GatewaySession* session, u32 heartbeat_jitter,
                                         u64 now_ms) noexcept {
    if (!session) {
        return session_error(GatewaySessionError::invalid_input);
    }
    if (!session->opened) {
        const WebSocketHandshakeValidation handshake = websocket_validate_opening_response(
            byte_queue_readable(session->input),
            ConstSlice<u8>{session->client_key, kWebSocketClientKeyBytes}, session->maximum_header_bytes,
            session->maximum_header_fields);
        if (handshake.state == WebSocketHandshakeState::need_more) {
            return event(GatewaySessionEventKind::need_input);
        }
        if (!handshake.ok()) {
            GatewaySessionEvent result = session_error(GatewaySessionError::handshake);
            result.http_error = handshake.http_error;
            result.handshake_error = handshake.error;
            return result;
        }
        if (!byte_queue_consume(&session->input, handshake.header_bytes)) {
            return session_error(GatewaySessionError::invalid_input);
        }
        session->opened = true;
        const GatewayLifecycleStep connected =
            gateway_lifecycle_connected(&session->lifecycle, session->lifecycle.resume_available);
        if (!connected.ok()) {
            return session_error(GatewaySessionError::lifecycle);
        }
        GatewaySessionEvent opened = event(GatewaySessionEventKind::opened);
        opened.lifecycle_step = connected;
        return opened;
    }

    for (;;) {
        if (session->transport_compressed) {
            if (session->stream_remaining) {
                if (!stream_frame_payload(session)) return session_error(GatewaySessionError::message_overflow);
                if (session->stream_remaining) return event(GatewaySessionEventKind::need_input);
                continue;
            }
            // A payload ends at a message boundary whose bytes end in Z_SYNC_FLUSH.
            const u32 pending = session->message_tracker.fragmented ? 0 : complete_sync_flush_bytes(*session);
            if (pending != 0) {
                return process_compressed_message(
                    session, pending, heartbeat_jitter, now_ms);
            }
            // Binary data frames stream their payload; control and text frames
            // take the complete-frame path below.
            const auto header = websocket_parse_frame_header(byte_queue_readable(session->input),
                session->maximum_compressed_bytes, WebSocketDirection::server_to_client);
            if (header.state == WebSocketParseState::need_more) return event(GatewaySessionEventKind::need_input);
            if (!header.ok()) {
                auto result = session_error(GatewaySessionError::websocket);
                result.websocket_error = header.error;
                return result;
            }
            if (header.frame.opcode == WebSocketOpcode::binary || header.frame.opcode == WebSocketOpcode::continuation) {
                const auto tracked = websocket_track_frame(&session->message_tracker, header.frame,
                                                           session->maximum_compressed_bytes);
                if (!tracked.ok()) {
                    auto result = session_error(GatewaySessionError::websocket);
                    result.websocket_error = tracked.error;
                    return result;
                }
                if (tracked.message_opcode != WebSocketOpcode::binary)
                    return session_error(GatewaySessionError::unexpected_encoding);
                if (!byte_queue_consume(&session->input, header.frame.header_bytes))
                    return session_error(GatewaySessionError::invalid_input);
                session->stream_remaining = static_cast<u32>(header.frame.payload_bytes);
                continue;
            }
        }
        const auto packet = websocket_read_message(&session->input, &session->message_tracker,
            session->wire_message_storage, &session->message_bytes, session->maximum_frame_payload_bytes,
            session->maximum_message_bytes);
        if (packet.state == WebSocketReadState::need_input) return event(GatewaySessionEventKind::need_input);
        if (packet.state == WebSocketReadState::error) {
            auto result = session_error(GatewaySessionError::websocket);
            result.websocket_error = packet.error;
            return result;
        }
        if (packet.state == WebSocketReadState::control) {
            GatewaySessionEvent result{};
            result.control_payload = packet.payload;
            if (packet.opcode == WebSocketOpcode::ping) result.kind = GatewaySessionEventKind::ping;
            else if (packet.opcode == WebSocketOpcode::pong) result.kind = GatewaySessionEventKind::pong;
            else {
                result.kind = GatewaySessionEventKind::closed;
                result.close_code = packet.payload.count >= 2 ?
                    static_cast<u16>((static_cast<u16>(packet.payload[0]) << 8) | packet.payload[1]) : 0;
                result.lifecycle_step = gateway_lifecycle_closed(&session->lifecycle, result.close_code);
            }
            return result;
        }
        const auto message = packet.payload;
        if (session->transport_compressed) {
            if (packet.opcode != WebSocketOpcode::binary) {
                return session_error(
                    GatewaySessionError::unexpected_encoding);
            }
            if (!append_compressed(session, message)) {
                return session_error(GatewaySessionError::message_overflow);
            }
            session->message_bytes = 0;
            continue;
        }
        if (packet.opcode != WebSocketOpcode::text) {
            return session_error(GatewaySessionError::unsupported_binary);
        }

        GatewaySessionEvent result = process_gateway_message(
            session, Utf8View{reinterpret_cast<const char*>(message.data), message.count}, heartbeat_jitter,
            now_ms);
        session->message_bytes = 0;
        return result;
    }
}

} // namespace maicord
