#include "maicord/protocol/gateway_session.hpp"
#include "maicord/protocol/websocket_message.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"


namespace maicord {
namespace {
// Every member of a session event defaults to zero, so one fill replaces the member-wise value-initialization.
void event(GatewaySessionEvent* result, GatewaySessionEventKind kind) noexcept {
    clear_bytes(result, sizeof(*result));
    result->kind = kind;
}

// Turns a failed event decode into the session's error event.
bool event_decode_failed(GatewaySessionEvent& result, const GatewayEventDecodeResult& decoded) noexcept {
    if (decoded.ok())
        return false;
    result.kind = GatewaySessionEventKind::error;
    result.error = GatewaySessionError::event_decode;
    result.event_error = decoded.error;
    return true;
}

void session_error(GatewaySessionEvent* result, GatewaySessionError error) noexcept {
    clear_bytes(result, sizeof(*result));
    result->error = error;
    result->kind = GatewaySessionEventKind::error;
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

void consume_compressed(GatewaySession* session, u32 bytes) noexcept {
    const u32 remaining = session->compressed_bytes - bytes;
    copy_bytes(session->compressed_storage.data, session->compressed_storage.data + bytes, remaining);
    secure_clear_bytes(session->compressed_storage.data + remaining, bytes);
    session->compressed_bytes = remaining;
    // A spilled payload is done: later payloads start in the initial storage.
    if (session->compressed_storage.data != session->inline_compressed_storage.data &&
        remaining <= session->inline_compressed_storage.count) {
        copy_bytes(session->inline_compressed_storage.data, session->compressed_storage.data, remaining);
        secure_clear_bytes(session->compressed_storage.data, remaining);
        session->compressed_storage = session->inline_compressed_storage;
        session->grow_compressed(session->grow_compressed_context, 0, nullptr);
    }
}

void process_gateway_message(GatewaySession* session, Utf8View message, u32 heartbeat_jitter,
                             u64 now_ms, GatewaySessionEvent* out) noexcept {
    clear_bytes(out, sizeof(*out));
    GatewaySessionEvent& result = *out;
    GatewayDecodeResult decoded =
        gateway_decode_envelope(message, session->structural_scratch, 64, &result.envelope);
    // The frame (or one zstd-stream message) has already been consumed.
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
        clear_bytes(&result.envelope, sizeof(result.envelope));
        decoded = gateway_decode_envelope(message, backed, 64, &result.envelope);
    }
    if (!decoded.ok()) {
        result.kind = GatewaySessionEventKind::error;
        result.error = GatewaySessionError::gateway_decode;
        result.gateway_error = decoded.error;
        result.scan_error = decoded.scan_error;
        return;
    }

    if (result.envelope.opcode == 10) {
        const GatewayEventDecodeResult hello =
            gateway_decode_hello(result.envelope.data, session->structural_scratch, 16, &result.hello);
        if (event_decode_failed(result, hello)) {
            return;
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
        if (event_decode_failed(result, invalid)) {
            return;
        }
        result.lifecycle_step = gateway_lifecycle_invalid_session(&session->lifecycle, resumable);
        result.kind = GatewaySessionEventKind::invalid_session;
    } else if (result.envelope.opcode == 0) {
        if (!result.envelope.has_sequence || !result.envelope.has_event_name) {
            result.kind = GatewaySessionEventKind::error;
            result.error = GatewaySessionError::gateway_decode;
            result.gateway_error = result.envelope.has_sequence ? GatewayDecodeError::invalid_event_name
                                                                : GatewayDecodeError::invalid_sequence;
            return;
        }
        result.lifecycle_step = gateway_lifecycle_dispatch(&session->lifecycle, result.envelope.sequence);
        if (json_string_equals_ascii(result.envelope.event_name, utf8_literal("READY"))) {
            const GatewayEventDecodeResult ready =
                gateway_decode_ready(result.envelope.data, session->structural_scratch, 64, &result.ready);
            if (event_decode_failed(result, ready)) {
                return;
            }
            result.lifecycle_step = gateway_lifecycle_session_ready(&session->lifecycle, true);
            result.kind = GatewaySessionEventKind::ready;
        } else if (json_string_equals_ascii(result.envelope.event_name, utf8_literal("RESUMED"))) {
            const GatewayEventDecodeResult resumed = gateway_decode_resumed(
                result.envelope.data, session->structural_scratch, 16, &result.resumed);
            if (event_decode_failed(result, resumed)) {
                return;
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
}

bool output_disjoint(const GatewaySession& session, Slice<u8> candidate) noexcept {
    if (session.structural_scratch.count > ~0u / sizeof(u32)) return false;
    const ConstSlice<u8> output{candidate.data, candidate.count};
    const ConstSlice<u8> decoder{reinterpret_cast<const u8*>(session.zstd_stream), sizeof(*session.zstd_stream)};
    const ConstSlice<u8> zstd_storage{session.zstd_stream->storage.data, session.zstd_stream->maximum_storage};
    const ConstSlice<u8> others[]{
        {session.input.data, session.input.capacity},
        {session.compressed_storage.data, session.compressed_storage.count},
        {session.wire_message_storage.data, session.wire_message_storage.count},
        {reinterpret_cast<const u8*>(session.structural_scratch.data),
            static_cast<u32>(session.structural_scratch.count * sizeof(u32))},
        {reinterpret_cast<const u8*>(&session), sizeof(session)},
        decoder, zstd_storage};
    if (!output.data || !output.count || !byte_range_valid(output)) return false;
    for (const auto other : others)
        if (!byte_range_valid(other) || byte_ranges_overlap(output, other)) return false;
    return true;
}
bool grow_session_output(void* opaque, u32 required, Slice<u8>* backed) noexcept {
    auto* session = static_cast<GatewaySession*>(opaque);
    Slice<u8> candidate{};
    if (!session->grow_output ||
        !session->grow_output(session->grow_output_context, required, &candidate) ||
        !output_disjoint(*session, candidate))
        return false;
    *backed = candidate;
    return true;
}
// One complete zstd-stream message: decompress, then transcode the term.
void process_zstd_message(GatewaySession* session, u32 heartbeat_jitter, u64 now_ms,
                          GatewaySessionEvent* out) noexcept {
    const u32 compressed_bytes = session->compressed_bytes;
    session->compressed_message_ready = false;
    ConstSlice<u8> etf{};
    const ZstdDecodeResult decoded =
        zstd_stream_decompress(session->zstd_stream, {session->compressed_storage.data, compressed_bytes},
                               session->maximum_decoded_message_bytes, &etf);
    if (!decoded.ok()) {
        session_error(out, GatewaySessionError::compression);
        out->zstd_error = decoded.error;
        return;
    }
    consume_compressed(session, compressed_bytes);
    const EtfResult json = etf_to_json(etf, &session->message_storage, session->maximum_decoded_message_bytes,
                                       session->grow_output ? grow_session_output : nullptr, session);
    if (!json.ok()) {
        session_error(out, GatewaySessionError::etf);
        out->etf_error = json.error;
        return;
    }
    process_gateway_message(session,
                            Utf8View{reinterpret_cast<const char*>(session->message_storage.data), json.bytes},
                            heartbeat_jitter, now_ms, out);
}

} // namespace

bool gateway_session_initialize(GatewaySession* session, Slice<u8> input_storage, Slice<u8> message_storage,
                                Slice<u32> structural_scratch, ConstSlice<u8> client_key,
                                u64 maximum_frame_payload_bytes, u64 maximum_message_bytes,
                                bool resume_available, bool has_sequence, u64 last_sequence) noexcept {
    if (!input_storage.data || input_storage.count == 0 || !message_storage.data ||
        message_storage.count == 0 || !structural_scratch.data || structural_scratch.count == 0 ||
        !client_key.data || client_key.count != kWebSocketClientKeyBytes ||
        maximum_frame_payload_bytes == 0 || input_storage.count < 14 ||
        maximum_frame_payload_bytes > input_storage.count - 14 || maximum_message_bytes == 0 ||
        maximum_message_bytes > message_storage.count) {
        return false;
    }
    reset_value<GatewaySession>(session);
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
    return byte_queue_append(&session->input, plaintext);
}

bool gateway_session_enable_zstd_etf(GatewaySession* session, ZstdStreamDecoder* decoder,
                                     Slice<u8> compressed_storage) noexcept {
    if (session->opened || session->transport_compressed || !decoder || !decoder->initialized || decoder->failed ||
        !compressed_storage.data || compressed_storage.count == 0 || session->message_bytes != 0 ||
        session->compressed_bytes != 0)
        return false;
    session->zstd_stream = decoder;
    session->compressed_storage = compressed_storage;
    session->inline_compressed_storage = compressed_storage;
    session->maximum_compressed_bytes = compressed_storage.count;
    session->transport_compressed = true;
    return true;
}

bool gateway_session_enable_compressed_growth(GatewaySession* session, u32 maximum_compressed_bytes,
    GatewaySessionGrowCompressed grow, void* context) noexcept {
    if (session->opened || !session->transport_compressed || session->grow_compressed || !grow ||
        maximum_compressed_bytes < session->inline_compressed_storage.count ||
        maximum_compressed_bytes > kGatewaySessionMaximumDecodedMessageBytes)
        return false;
    session->grow_compressed = grow;
    session->grow_compressed_context = context;
    session->maximum_compressed_bytes = maximum_compressed_bytes;
    return true;
}

bool gateway_session_enable_output_growth(GatewaySession* session, Slice<u8> decoded_output,
    u32 maximum_decoded_bytes, EtfGrowOutput grow, void* context) noexcept {
    if (session->opened || !session->transport_compressed || !session->zstd_stream ||
        session->grow_output || !grow || maximum_decoded_bytes < decoded_output.count ||
        maximum_decoded_bytes > kGatewaySessionMaximumDecodedMessageBytes)
        return false;
    if (!output_disjoint(*session, decoded_output)) return false;
    session->message_storage = decoded_output;
    session->maximum_decoded_message_bytes = maximum_decoded_bytes;
    session->grow_output = grow;
    session->grow_output_context = context;
    return true;
}

void gateway_session_next(GatewaySessionEvent* out, GatewaySession* session, u32 heartbeat_jitter,
                          u64 now_ms) noexcept {
    if (!session->opened) {
        const WebSocketHandshakeValidation handshake = websocket_validate_opening_response(
            byte_queue_readable(session->input),
            ConstSlice<u8>{session->client_key, kWebSocketClientKeyBytes}, session->maximum_header_bytes,
            session->maximum_header_fields);
        if (handshake.state == WebSocketHandshakeState::need_more) {
            return event(out, GatewaySessionEventKind::need_input);
        }
        if (!handshake.ok()) {
            session_error(out, GatewaySessionError::handshake);
            out->http_error = handshake.http_error;
            out->handshake_error = handshake.error;
            return;
        }
        if (session->cookies)
            cookie_jar_store_response(session->cookies, session->cookie_request,
                                      {byte_queue_readable(session->input).data, handshake.header_bytes},
                                      clock_unix_milliseconds());
        if (!byte_queue_consume(&session->input, handshake.header_bytes)) {
            return session_error(out, GatewaySessionError::invalid_input);
        }
        session->opened = true;
        const GatewayLifecycleStep connected =
            gateway_lifecycle_connected(&session->lifecycle, session->lifecycle.resume_available);
        if (!connected.ok()) {
            return session_error(out, GatewaySessionError::lifecycle);
        }
        event(out, GatewaySessionEventKind::opened);
        out->lifecycle_step = connected;
        return;
    }

    for (;;) {
        if (session->transport_compressed) {
            if (session->stream_remaining) {
                if (!stream_frame_payload(session)) return session_error(out, GatewaySessionError::message_overflow);
                if (session->stream_remaining) return event(out, GatewaySessionEventKind::need_input);
                continue;
            }
            // A zstd-stream payload is one whole message.
            if (session->compressed_message_ready)
                return process_zstd_message(session, heartbeat_jitter, now_ms, out);
            // Binary data frames stream their payload; control and text frames
            // take the complete-frame path below.
            const auto header = websocket_parse_frame_header(byte_queue_readable(session->input),
                session->maximum_compressed_bytes, WebSocketDirection::server_to_client);
            if (header.state == WebSocketParseState::need_more) return event(out, GatewaySessionEventKind::need_input);
            if (!header.ok()) {
                session_error(out, GatewaySessionError::websocket);
                out->websocket_error = header.error;
                return;
            }
            if (header.frame.opcode == WebSocketOpcode::binary || header.frame.opcode == WebSocketOpcode::continuation) {
                const auto tracked = websocket_track_frame(&session->message_tracker, header.frame,
                                                           session->maximum_compressed_bytes);
                if (!tracked.ok()) {
                    session_error(out, GatewaySessionError::websocket);
                    out->websocket_error = tracked.error;
                    return;
                }
                if (tracked.message_opcode != WebSocketOpcode::binary)
                    return session_error(out, GatewaySessionError::unexpected_encoding);
                if (!byte_queue_consume(&session->input, header.frame.header_bytes))
                    return session_error(out, GatewaySessionError::invalid_input);
                session->compressed_message_ready = tracked.state == WebSocketMessageState::message_complete;
                session->stream_remaining = static_cast<u32>(header.frame.payload_bytes);
                continue;
            }
        }
        const auto packet = websocket_read_message(&session->input, &session->message_tracker,
            session->wire_message_storage, &session->message_bytes, session->maximum_frame_payload_bytes,
            session->maximum_message_bytes);
        if (packet.state == WebSocketReadState::need_input) return event(out, GatewaySessionEventKind::need_input);
        if (packet.state == WebSocketReadState::error) {
            session_error(out, GatewaySessionError::websocket);
            out->websocket_error = packet.error;
            return;
        }
        if (packet.state == WebSocketReadState::control) {
            clear_bytes(out, sizeof(*out));
            GatewaySessionEvent& result = *out;
            result.control_payload = packet.payload;
            if (packet.opcode == WebSocketOpcode::ping) result.kind = GatewaySessionEventKind::ping;
            else if (packet.opcode == WebSocketOpcode::pong) result.kind = GatewaySessionEventKind::pong;
            else {
                result.kind = GatewaySessionEventKind::closed;
                result.close_code = packet.payload.count >= 2 ?
                    static_cast<u16>((static_cast<u16>(packet.payload[0]) << 8) | packet.payload[1]) : 0;
                result.lifecycle_step = gateway_lifecycle_closed(&session->lifecycle, result.close_code);
            }
            return;
        }
        const auto message = packet.payload;
        // Binary messages of a compressed session were streamed above.
        if (session->transport_compressed)
            return session_error(out, GatewaySessionError::unexpected_encoding);
        if (packet.opcode != WebSocketOpcode::text) {
            return session_error(out, GatewaySessionError::unsupported_binary);
        }

        process_gateway_message(
            session, Utf8View{reinterpret_cast<const char*>(message.data), message.count}, heartbeat_jitter,
            now_ms, out);
        session->message_bytes = 0;
        return;
    }
}

GatewaySessionEvent gateway_session_next(GatewaySession* session, u32 heartbeat_jitter, u64 now_ms) noexcept {
    GatewaySessionEvent result;
    gateway_session_next(&result, session, heartbeat_jitter, now_ms);
    return result;
}

} // namespace maicord
