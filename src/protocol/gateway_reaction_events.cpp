#include "maicord/protocol/gateway_reaction_events.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord {
namespace {

enum ReactionField : u32 {
    reaction_field_message_id = 1u << 0,
    reaction_field_channel_id = 1u << 1,
    reaction_field_user_id = 1u << 2,
    reaction_field_emoji = 1u << 3,
    reaction_field_type = 1u << 4,
    reaction_field_burst = 1u << 5,
};

struct ReactionContext {
    GatewayMessageReactionEvent event;
    JsonValue emoji;
    GatewayEventDecodeError error;
    u32 fields;
    bool burst;
};

struct EmojiContext {
    u64 id;
    JsonString name;
    GatewayEventDecodeError error;
    bool id_seen;
    bool name_seen;
    bool name_null;
    bool animated_seen;
    bool animated;
};

GatewayEventDecodeResult failure(
    GatewayEventDecodeError error, u32 offset = 0,
    JsonScanError scan_error = JsonScanError::none,
    JsonReadError read_error = JsonReadError::none) noexcept {
    return GatewayEventDecodeResult{offset, error, scan_error, read_error};
}

bool mark(ReactionContext* context, u32 field) noexcept {
    if (!context || (context->fields & field) != 0) {
        if (context) {
            context->error = GatewayEventDecodeError::duplicate_field;
        }
        return false;
    }
    context->fields |= field;
    return true;
}

bool visit_reaction(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReactionContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("message_id"))) {
        if (!mark(context, reaction_field_message_id) ||
            !json_value_to_snowflake(value, &context->event.message_id)) {
            if (context->error == GatewayEventDecodeError::none) {
                context->error = GatewayEventDecodeError::invalid_message_id;
            }
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        if (!mark(context, reaction_field_channel_id) ||
            !json_value_to_snowflake(value, &context->event.channel_id)) {
            if (context->error == GatewayEventDecodeError::none) {
                context->error = GatewayEventDecodeError::invalid_channel_id;
            }
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("user_id"))) {
        if (!mark(context, reaction_field_user_id) ||
            !json_value_to_snowflake(value, &context->event.user_id)) {
            if (context->error == GatewayEventDecodeError::none) {
                context->error = GatewayEventDecodeError::invalid_data;
            }
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("emoji"))) {
        if (!mark(context, reaction_field_emoji) ||
            value.kind != JsonValueKind::object) {
            if (context->error == GatewayEventDecodeError::none) {
                context->error = GatewayEventDecodeError::invalid_data;
            }
            return false;
        }
        context->emoji = value;
    } else if (json_string_equals_ascii(name, utf8_literal("type"))) {
        u64 type = 0;
        if (!mark(context, reaction_field_type) ||
            !json_value_to_u64(value, &type) ||
            type > static_cast<u8>(MessageReactionType::vote)) {
            if (context->error == GatewayEventDecodeError::none) {
                context->error = GatewayEventDecodeError::invalid_data;
            }
            return false;
        }
        context->event.type = static_cast<MessageReactionType>(type);
    } else if (json_string_equals_ascii(name, utf8_literal("burst"))) {
        if (!mark(context, reaction_field_burst) ||
            !json_value_to_bool(value, &context->burst)) {
            if (context->error == GatewayEventDecodeError::none) {
                context->error = GatewayEventDecodeError::invalid_data;
            }
            return false;
        }
    }
    return true;
}

bool visit_emoji(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmojiContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (context->id_seen) {
            context->error = GatewayEventDecodeError::duplicate_field;
            return false;
        }
        context->id_seen = true;
        if (value.kind != JsonValueKind::null_value &&
            !json_value_to_snowflake(value, &context->id)) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("name"))) {
        if (context->name_seen ||
            (value.kind != JsonValueKind::null_value &&
             !json_value_as_string(value, &context->name))) {
            context->error = context->name_seen
                ? GatewayEventDecodeError::duplicate_field
                : GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->name_seen = true;
        context->name_null = value.kind == JsonValueKind::null_value;
    } else if (json_string_equals_ascii(name, utf8_literal("animated"))) {
        if (context->animated_seen ||
            !json_value_to_bool(value, &context->animated)) {
            context->error = context->animated_seen
                ? GatewayEventDecodeError::duplicate_field
                : GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->animated_seen = true;
    }
    return true;
}

} // namespace

GatewayEventDecodeResult gateway_decode_message_reaction(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> name_output, GatewayMessageReactionEventKind kind,
    GatewayMessageReactionEvent* output) noexcept {
    if (!output || !data.source.data || data.source.bytes == 0 ||
        !structural_scratch.data || structural_scratch.count == 0 ||
        maximum_depth == 0 || (!name_output.data && name_output.count != 0) ||
        static_cast<u8>(kind) >
            static_cast<u8>(GatewayMessageReactionEventKind::remove_emoji)) {
        return failure(GatewayEventDecodeError::invalid_input);
    }
    *output = GatewayMessageReactionEvent{};
    const JsonScanResult scan =
        json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return failure(GatewayEventDecodeError::scan_failed,
                       scan.error_offset, scan.error);
    }
    ReactionContext context{};
    context.event.kind = kind;
    context.event.type = MessageReactionType::normal;
    const JsonReadResult read = json_visit_value_object(
        data, ConstSlice<u32>{structural_scratch.data, scan.count},
        maximum_depth, visit_reaction, &context);
    if (!read.ok()) {
        return context.error != GatewayEventDecodeError::none
            ? failure(context.error, read.error_offset, JsonScanError::none,
                      read.error)
            : failure(GatewayEventDecodeError::invalid_json,
                      read.error_offset, JsonScanError::none, read.error);
    }
    const u32 required = reaction_field_message_id | reaction_field_channel_id;
    const bool needs_user = kind == GatewayMessageReactionEventKind::add ||
                            kind == GatewayMessageReactionEventKind::remove;
    const bool needs_emoji = kind != GatewayMessageReactionEventKind::remove_all;
    if ((context.fields & required) != required ||
        (needs_user && (context.fields & reaction_field_user_id) == 0) ||
        (needs_emoji && (context.fields & reaction_field_emoji) == 0)) {
        return failure(GatewayEventDecodeError::missing_required_field,
                       data.source_offset);
    }
    if ((context.fields & reaction_field_type) == 0 &&
        (context.fields & reaction_field_burst) != 0 && context.burst) {
        context.event.type = MessageReactionType::burst;
    }
    if (!needs_emoji) {
        *output = context.event;
        return GatewayEventDecodeResult{};
    }

    const JsonScanResult emoji_scan =
        json_scan_structurals_avx1(context.emoji.source, structural_scratch);
    if (!emoji_scan.ok()) {
        return failure(GatewayEventDecodeError::scan_failed,
                       emoji_scan.error_offset, emoji_scan.error);
    }
    EmojiContext emoji{};
    const JsonReadResult emoji_read = json_visit_value_object(
        context.emoji,
        ConstSlice<u32>{structural_scratch.data, emoji_scan.count},
        maximum_depth, visit_emoji, &emoji);
    if (!emoji_read.ok()) {
        return emoji.error != GatewayEventDecodeError::none
            ? failure(emoji.error, emoji_read.error_offset,
                      JsonScanError::none, emoji_read.error)
            : failure(GatewayEventDecodeError::invalid_json,
                      emoji_read.error_offset, JsonScanError::none,
                      emoji_read.error);
    }
    if (!emoji.name_seen) {
        return failure(GatewayEventDecodeError::missing_required_field,
                       context.emoji.source_offset);
    }
    // Deleted/unavailable custom emoji retain their identity in reaction
    // events even when the server cannot provide their name.
    if (emoji.name_null) {
        if (emoji.id == 0) {
            return failure(GatewayEventDecodeError::invalid_data,
                           context.emoji.source_offset);
        }
        context.event.emoji_id = emoji.id;
        context.event.animated = emoji.animated;
        *output = context.event;
        return GatewayEventDecodeResult{};
    }
    const JsonStringDecodeResult size =
        json_string_decode_utf8(emoji.name, Slice<char>{nullptr, 0});
    if ((size.error != JsonStringDecodeError::output_too_small &&
         !(size.ok() && size.bytes == 0)) ||
        size.bytes == 0 ||
        size.bytes > kMessageReactionMaximumEmojiNameBytes ||
        size.bytes > name_output.count) {
        return failure(GatewayEventDecodeError::invalid_data,
                       context.emoji.source_offset);
    }
    const JsonStringDecodeResult name = json_string_decode_utf8(
        emoji.name, Slice<char>{name_output.data, name_output.count});
    if (!name.ok() || name.bytes != size.bytes) {
        return failure(GatewayEventDecodeError::invalid_data,
                       context.emoji.source_offset);
    }
    context.event.emoji_id = emoji.id;
    context.event.name = Utf8View{name_output.data, name.bytes};
    context.event.animated = emoji.animated;
    *output = context.event;
    return GatewayEventDecodeResult{};
}

} // namespace maicord
