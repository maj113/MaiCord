#pragma once

#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/protocol/gateway_events.hpp"

namespace maicord {

struct GatewayVoiceStateUpdate {
    u64 guild_id;   // Zero selects a private call (JSON null).
    u64 channel_id; // Zero disconnects (JSON null).
    bool self_mute, self_deaf, self_video;
    // Client-local, never serialized: the channel is a stage, whose voice
    // connection may negotiate DAVE protocol version 0 (no E2EE).
    bool stage;
};
GatewayCommandWriteResult gateway_write_voice_state(const GatewayVoiceStateUpdate& state,
                                                    Slice<u8> output) noexcept;

enum GatewayVoiceFlag : u32 {
    gateway_voice_deaf = 1u << 0,
    gateway_voice_mute = 1u << 1,
    gateway_voice_self_deaf = 1u << 2,
    gateway_voice_self_mute = 1u << 3,
    gateway_voice_self_stream = 1u << 4,
    gateway_voice_self_video = 1u << 5,
    gateway_voice_suppress = 1u << 6,
    // Stage channels: request_to_speak_timestamp is non-null (hand raised, or
    // invited to speak while not suppressed).
    gateway_voice_request_to_speak = 1u << 7,
};

// All strings borrow the validated input document. In particular, session IDs
// and voice tokens must not be logged, persisted, or retained past that document.
// Decoders leave output unchanged on failure and never allocate a JSON DOM.
struct GatewayVoiceState {
    u64 guild_id, channel_id, user_id;
    JsonString session_id;
    u32 flags, present_flags;
    u64 request_to_speak_ms; // Unix milliseconds while gateway_voice_request_to_speak is set.
};
struct GatewayVoiceServer {
    u64 guild_id, channel_id;
    JsonString token, endpoint;
    bool unavailable; // Null endpoint: stop the old transport and await reallocation.
};
GatewayEventDecodeResult gateway_decode_voice_state(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                                    GatewayVoiceState* output) noexcept;
// Roster rows describe other users too: READY_SUPPLEMENTAL and PASSIVE_UPDATE_V2
// states may omit session_id, which only the local connection needs.
GatewayEventDecodeResult gateway_decode_voice_state_row(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                                        GatewayVoiceState* output) noexcept;
GatewayEventDecodeResult gateway_decode_voice_server(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                                     GatewayVoiceServer* output) noexcept;

} // namespace maicord
