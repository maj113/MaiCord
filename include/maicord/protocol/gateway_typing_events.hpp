#pragma once

#include "maicord/protocol/gateway_events.hpp"

namespace maicord {

struct GatewayTypingStartEvent {
    u64 channel_id;
    u64 guild_id;
    u64 user_id;
    u64 timestamp_seconds;
};

// Decodes a TYPING_START value of an indexed document (no scan).
GatewayEventDecodeResult gateway_decode_typing_start(
    const JsonIndexedDocument& document, JsonValue data, u32 maximum_depth,
    GatewayTypingStartEvent* output) noexcept;

} // namespace maicord
