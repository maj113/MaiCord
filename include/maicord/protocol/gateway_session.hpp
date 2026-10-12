#pragma once

#include "maicord/core/byte_queue.hpp"
#include "maicord/media/deflate.hpp"
#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/protocol/gateway_lifecycle.hpp"
#include "maicord/protocol/websocket_frame.hpp"
#include "maicord/protocol/websocket_handshake.hpp"

namespace maicord {

enum class GatewaySessionEventKind : u8 {
    need_input,
    opened,
    hello,
    ready,
    resumed,
    dispatch,
    heartbeat_requested,
    heartbeat_acknowledged,
    reconnect_requested,
    invalid_session,
    ping,
    pong,
    closed,
    other_gateway_event,
    error,
};

enum class GatewaySessionError : u8 {
    none,
    invalid_input,
    input_overflow,
    handshake,
    websocket,
    message_overflow,
    unsupported_binary,
    unexpected_encoding,
    compression,
    gateway_decode,
    event_decode,
    lifecycle,
};

// The optional growth callback extends only the committed part of the structural
// index. It must keep the base address and the message bytes unchanged.
inline constexpr u32 kGatewaySessionMaximumStructuralCount = 1024 * 1024;
inline constexpr u32 kGatewaySessionMaximumDecodedMessageBytes = 8 * 1024 * 1024;
using GatewaySessionGrowIndex = bool (*)(void*, u32, Slice<u32>*) noexcept;
// Compressed spill: storage of at least `required` bytes at a base that stays
// fixed while one payload grows (the session copies its bytes over once).
// required == 0 releases the spill after that payload was inflated.
using GatewaySessionGrowCompressed = bool (*)(void*, u32 required, Slice<u8>*) noexcept;

struct GatewaySession {
    ByteQueue input;
    Slice<u8> message_storage;
    Slice<u8> wire_message_storage;
    Slice<u32> structural_scratch;
    GatewaySessionGrowIndex grow_index;
    void* grow_index_context;
    ZlibStreamGrowOutput grow_output;
    void* grow_output_context;
    u32 maximum_decoded_message_bytes;
    GatewayLifecycle lifecycle;
    WebSocketMessageTracker message_tracker;
    ZlibStreamInflater* zlib_stream;
    Slice<u8> compressed_storage;
    // Binary frame payloads stream from `input` into compressed storage, so
    // neither a frame nor a whole payload is bounded by the input queue.
    Slice<u8> inline_compressed_storage;
    GatewaySessionGrowCompressed grow_compressed;
    void* grow_compressed_context;
    u32 maximum_compressed_bytes;
    u32 stream_remaining;
    u64 maximum_frame_payload_bytes;
    u64 maximum_message_bytes;
    u32 message_bytes;
    u32 compressed_bytes;
    u32 maximum_header_bytes;
    u32 maximum_header_fields;
    u8 client_key[kWebSocketClientKeyBytes];
    bool transport_compressed;
    bool opened;
};

struct GatewaySessionEvent {
    GatewayEnvelope envelope;
    GatewayReady ready;
    GatewayHello hello;
    GatewayResumed resumed;
    ConstSlice<u8> control_payload;
    GatewayLifecycleStep lifecycle_step;
    u16 close_code;
    HttpError http_error;
    WebSocketHandshakeError handshake_error;
    WebSocketError websocket_error;
    GatewayDecodeError gateway_error;
    GatewayEventDecodeError event_error;
    ZlibInflateError compression_error;
    GatewaySessionError error;
    GatewaySessionEventKind kind;

    constexpr bool ok() const noexcept {
        return kind != GatewaySessionEventKind::error;
    }
};

bool gateway_session_initialize(GatewaySession* session, Slice<u8> input_storage, Slice<u8> message_storage,
                                Slice<u32> structural_scratch, ConstSlice<u8> client_key,
                                u64 maximum_frame_payload_bytes, u64 maximum_message_bytes,
                                bool resume_available, bool has_sequence, u64 last_sequence) noexcept;
bool gateway_session_push(GatewaySession* session, ConstSlice<u8> plaintext) noexcept;
bool gateway_session_enable_zlib_stream(
    GatewaySession* session, ZlibStreamInflater* inflater,
    Slice<u8> compressed_storage) noexcept;
// Register before opening, after zlib is enabled. Wire/frame limits stay
// fixed; only the separate decoded buffer can grow, at a stable base address.
bool gateway_session_enable_output_growth(GatewaySession* session, Slice<u8> decoded_output,
    u32 maximum_decoded_bytes, ZlibStreamGrowOutput grow, void* context) noexcept;
// Register before opening, after zlib is enabled: compressed payloads larger
// than the initial compressed storage spill into owner storage, up to
// maximum_compressed_bytes, and return to the initial storage afterwards.
bool gateway_session_enable_compressed_growth(GatewaySession* session, u32 maximum_compressed_bytes,
    GatewaySessionGrowCompressed grow, void* context) noexcept;
// Zero-copy event views remain valid until the next push/next call on this session.
GatewaySessionEvent gateway_session_next(GatewaySession* session, u32 heartbeat_jitter,
                                         u64 now_ms) noexcept;

} // namespace maicord
