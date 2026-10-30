#include "maicord/protocol/gateway_typing_events.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "gateway_decode.hpp"
#include "json_field_lists.hpp"

namespace maicord {
namespace {

using protocol_detail::accept_member;
using protocol_detail::decode_failure;
using protocol_detail::read_failure;

enum : u32 {
    typing_field_channel_id = 1u << 0,
    typing_field_guild_id = 1u << 1,
    typing_field_user_id = 1u << 2,
    typing_field_timestamp = 1u << 3,
};

struct TypingContext {
    GatewayTypingStartEvent event;
    GatewayEventDecodeError error;
    u32 fields;
};

bool visit_typing(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<TypingContext*>(opaque);
    auto& event = context->event;
    switch (protocol_detail::member_index<"channel_id", "guild_id", "user_id", "timestamp">(name)) {
    case 0:
        return accept_member(&context->fields, typing_field_channel_id,
                             json_value_to_snowflake(value, &event.channel_id), &context->error,
                             GatewayEventDecodeError::invalid_channel_id);
    case 1:
        return accept_member(&context->fields, typing_field_guild_id,
                             value.kind == JsonValueKind::null_value ||
                                 json_value_to_snowflake(value, &event.guild_id),
                             &context->error);
    case 2:
        return accept_member(&context->fields, typing_field_user_id,
                             json_value_to_snowflake(value, &event.user_id), &context->error);
    case 3:
        return accept_member(&context->fields, typing_field_timestamp,
                             json_value_to_u64(value, &event.timestamp_seconds) &&
                                 event.timestamp_seconds != 0,
                             &context->error);
    default: return true;
    }
}

} // namespace

GatewayEventDecodeResult gateway_decode_typing_start(
    const JsonIndexedDocument& document, JsonValue data, u32 maximum_depth,
    GatewayTypingStartEvent* output) noexcept {
    reset_value<GatewayTypingStartEvent>(output);
    TypingContext context{};
    const JsonReadResult read = json_visit_indexed_object(document, data, maximum_depth, visit_typing, &context);
    if (!read.ok()) {
        return read_failure(context.error, read);
    }
    constexpr u32 required = typing_field_channel_id | typing_field_user_id |
                             typing_field_timestamp;
    if ((context.fields & required) != required) {
        return decode_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    }
    *output = context.event;
    return GatewayEventDecodeResult{};
}

} // namespace maicord
