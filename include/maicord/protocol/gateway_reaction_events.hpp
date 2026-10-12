#pragma once

#include "maicord/protocol/gateway_events.hpp"
#include "maicord/protocol/message_reaction.hpp"

namespace maicord {

enum class GatewayMessageReactionEventKind : u8 {
    add,
    remove,
    remove_all,
    remove_emoji,
};

struct GatewayMessageReactionEvent {
    u64 message_id;
    u64 channel_id;
    u64 user_id;
    u64 emoji_id;
    Utf8View name;
    MessageReactionType type;
    GatewayMessageReactionEventKind kind;
    bool animated;
};

GatewayEventDecodeResult gateway_decode_message_reaction(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> name_output, GatewayMessageReactionEventKind kind,
    GatewayMessageReactionEvent* output) noexcept;

} // namespace maicord
