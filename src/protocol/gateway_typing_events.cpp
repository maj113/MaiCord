#include "maicord/protocol/gateway_typing_events.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "gateway_decode.hpp"

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
    if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        return accept_member(&context->fields, typing_field_channel_id,
                             json_value_to_snowflake(value, &event.channel_id), &context->error,
                             GatewayEventDecodeError::invalid_channel_id);
    }
    if (json_string_equals_ascii(name, utf8_literal("guild_id"))) {
        return accept_member(&context->fields, typing_field_guild_id,
                             value.kind == JsonValueKind::null_value ||
                                 json_value_to_snowflake(value, &event.guild_id),
                             &context->error);
    }
    if (json_string_equals_ascii(name, utf8_literal("user_id"))) {
        return accept_member(&context->fields, typing_field_user_id,
                             json_value_to_snowflake(value, &event.user_id), &context->error);
    }
    if (json_string_equals_ascii(name, utf8_literal("timestamp"))) {
        return accept_member(&context->fields, typing_field_timestamp,
                             json_value_to_u64(value, &event.timestamp_seconds) &&
                                 event.timestamp_seconds != 0,
                             &context->error);
    }
    return true;
}

} // namespace

GatewayEventDecodeResult gateway_decode_typing_start(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth,
    GatewayTypingStartEvent* output) noexcept {
    reset_value<GatewayTypingStartEvent>(output);
    const JsonScanResult scan =
        json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    TypingContext context{};
    const JsonReadResult read = json_visit_value_object(
        data,
        ConstSlice<u32>{structural_scratch.data, scan.count}, maximum_depth,
        visit_typing, &context);
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
