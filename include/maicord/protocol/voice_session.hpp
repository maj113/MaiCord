#pragma once

#include "maicord/protocol/voice_gateway.hpp"
#include "maicord/protocol/voice_heartbeat.hpp"
#include "maicord/protocol/websocket_handshake.hpp"
#include "maicord/protocol/websocket_message.hpp"

namespace maicord {
enum class VoiceSessionEventKind : u8 {
    need_input,
    opened,
    hello,
    heartbeat_requested,
    heartbeat_acknowledged,
    heartbeat_ack_ignored,
    json,
    binary,
    ping,
    pong,
    closed,
    error
};
enum class VoiceSessionError : u8 {
    none,
    invalid_input,
    handshake,
    websocket,
    json,
    binary,
    event,
    heartbeat
};
struct VoiceSessionEvent {
    VoiceGatewayEnvelope envelope;
    VoiceGatewayBinary binary;
    ConstSlice<u8> control;
    GatewayHello hello;
    u16 close_code;
    VoiceSessionEventKind kind;
    VoiceSessionError error;
    WebSocketHandshakeError handshake_error;
    WebSocketError websocket_error;
    bool ok() const noexcept {
        return kind != VoiceSessionEventKind::error;
    }
};
struct VoiceSession {
    ByteQueue input;
    Slice<u8> fragments;
    Slice<u32> scratch;
    // Optional owner growth, invoked only for structural overflow before any
    // event is published. Returned storage must be disjoint from packet buffers.
    Slice<u32> (*grow_scratch)(void*, u32 required) noexcept;
    void* scratch_context;
    WebSocketMessageTracker tracker;
    VoiceHeartbeat heartbeat;
    u32 fragment_bytes, maximum_frame_bytes, maximum_message_bytes;
    u8 client_key[kWebSocketClientKeyBytes];
    bool opened, failed, closed;
};
// Caller-owned disjoint buffers; no allocation. Reinitialize only after the old
// socket's operations drain. Old buffers must remain alive until reinitialization
// or clear erases them. Resume retains sequence through heartbeat state.
bool voice_session_initialize(VoiceSession* session, Slice<u8> input, Slice<u8> fragments,
                              Slice<u32> scratch, ConstSlice<u8> client_key, u32 maximum_frame_bytes,
                              u32 maximum_message_bytes, u64 now_ms, bool resume) noexcept;
bool voice_session_push(VoiceSession* session, ConstSlice<u8> decrypted) noexcept;
VoiceSessionEvent voice_session_next(VoiceSession* session, u64 now_ms) noexcept;
void voice_session_clear(VoiceSession* session) noexcept;
// All event views expire on push/next/clear. Consume on the socket worker only:
// binary MLS material and JSON Session Description can contain secrets.
} // namespace maicord
