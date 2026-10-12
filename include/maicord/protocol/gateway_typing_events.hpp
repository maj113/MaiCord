#pragma once

#include "maicord/protocol/gateway_events.hpp"

namespace maicord {

struct GatewayTypingStartEvent {
    u64 channel_id;
    u64 guild_id;
    u64 user_id;
    u64 timestamp_seconds;
};

GatewayEventDecodeResult gateway_decode_typing_start(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth,
    GatewayTypingStartEvent* output) noexcept;

} // namespace maicord
