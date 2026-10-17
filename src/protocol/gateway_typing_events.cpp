#include "maicord/protocol/gateway_typing_events.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord {
namespace {

enum TypingField : u32 {
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

GatewayEventDecodeResult failure(
    GatewayEventDecodeError error, u32 offset = 0,
    JsonScanError scan_error = JsonScanError::none,
    JsonReadError read_error = JsonReadError::none) noexcept {
    return GatewayEventDecodeResult{offset, error, scan_error, read_error};
}

bool mark(TypingContext* context, u32 field) noexcept {
    if ((context->fields & field) != 0) {
        context->error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    context->fields |= field;
    return true;
}

bool visit_typing(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<TypingContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        if (!mark(context, typing_field_channel_id) ||
            !json_value_to_snowflake(value, &context->event.channel_id)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_channel_id
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("guild_id"))) {
        if (!mark(context, typing_field_guild_id) ||
            (value.kind != JsonValueKind::null_value &&
             !json_value_to_snowflake(value, &context->event.guild_id))) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("user_id"))) {
        if (!mark(context, typing_field_user_id) ||
            !json_value_to_snowflake(value, &context->event.user_id)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("timestamp"))) {
        if (!mark(context, typing_field_timestamp) ||
            !json_value_to_u64(value, &context->event.timestamp_seconds) ||
            context->event.timestamp_seconds == 0) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    }
    return true;
}

} // namespace

GatewayEventDecodeResult gateway_decode_typing_start(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth,
    GatewayTypingStartEvent* output) noexcept {
    if (!output || !data.source.data || data.source.bytes == 0 ||
        !structural_scratch.data || structural_scratch.count == 0 ||
        maximum_depth == 0) {
        return failure(GatewayEventDecodeError::invalid_input);
    }
    *output = GatewayTypingStartEvent{};
    const JsonScanResult scan =
        json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return failure(GatewayEventDecodeError::scan_failed,
                       scan.error_offset, scan.error);
    }
    TypingContext context{};
    const JsonReadResult read = json_visit_value_object(
        data,
        ConstSlice<u32>{structural_scratch.data, scan.count}, maximum_depth,
        visit_typing, &context);
    if (!read.ok()) {
        return context.error != GatewayEventDecodeError::none
                   ? failure(context.error, read.error_offset,
                             JsonScanError::none, read.error)
                   : failure(GatewayEventDecodeError::invalid_json,
                             read.error_offset, JsonScanError::none,
                             read.error);
    }
    constexpr u32 required = typing_field_channel_id | typing_field_user_id |
                             typing_field_timestamp;
    if ((context.fields & required) != required) {
        return failure(GatewayEventDecodeError::missing_required_field,
                       data.source_offset);
    }
    *output = context.event;
    return GatewayEventDecodeResult{};
}

} // namespace maicord
