#include "maicord/protocol/gateway_reaction_events.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/core/byte_range.hpp"

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

GatewayEventDecodeResult finish_emoji(const EmojiContext& emoji, u32 offset, Slice<char> name_output,
                                      GatewayMessageReactionEvent* output) noexcept {
    if (!emoji.name_seen)
        return failure(GatewayEventDecodeError::missing_required_field, offset);
    output->emoji_id = emoji.id;
    output->animated = emoji.animated;
    output->name = {};
    // Deleted custom emoji keep their ID even when their name is unavailable.
    if (emoji.name_null)
        return emoji.id ? GatewayEventDecodeResult{}
                        : failure(GatewayEventDecodeError::invalid_data, offset);
    const auto size = json_string_decode_utf8(emoji.name, {});
    if ((size.error != JsonStringDecodeError::output_too_small && !(size.ok() && size.bytes == 0)) ||
        !size.bytes || size.bytes > kMessageReactionMaximumEmojiNameBytes || size.bytes > name_output.count)
        return failure(GatewayEventDecodeError::invalid_data, offset);
    const auto name = json_string_decode_utf8(emoji.name, name_output);
    if (!name.ok() || name.bytes != size.bytes)
        return failure(GatewayEventDecodeError::invalid_data, offset);
    output->name = {name_output.data, name.bytes};
    return {};
}

struct ReactionBatchRoot {
    ReactionContext header;
    JsonValue reactions;
    bool reactions_seen, guild_seen;
};

bool visit_batch_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& root = *static_cast<ReactionBatchRoot*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("channel_id")) ||
        json_string_equals_ascii(name, utf8_literal("message_id")))
        return visit_reaction(&root.header, name, value);
    if (json_string_equals_ascii(name, utf8_literal("reactions"))) {
        if (root.reactions_seen || value.kind != JsonValueKind::array) {
            root.header.error = root.reactions_seen ? GatewayEventDecodeError::duplicate_field
                                                    : GatewayEventDecodeError::invalid_data;
            return false;
        }
        root.reactions_seen = true;
        root.reactions = value;
    } else if (json_string_equals_ascii(name, utf8_literal("guild_id"))) {
        u64 guild_id = 0;
        if (root.guild_seen || !json_value_to_snowflake(value, &guild_id)) {
            root.header.error = root.guild_seen ? GatewayEventDecodeError::duplicate_field
                                                : GatewayEventDecodeError::invalid_data;
            return false;
        }
        root.guild_seen = true;
    }
    return true;
}

struct ReactionBatchGroup {
    JsonValue users, emoji;
    GatewayEventDecodeError error;
    bool users_seen, emoji_seen;
};

bool visit_batch_group(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& group = *static_cast<ReactionBatchGroup*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("users"))) {
        if (group.users_seen || value.kind != JsonValueKind::array) {
            group.error = group.users_seen ? GatewayEventDecodeError::duplicate_field
                                           : GatewayEventDecodeError::invalid_data;
            return false;
        }
        group.users_seen = true;
        group.users = value;
    } else if (json_string_equals_ascii(name, utf8_literal("emoji"))) {
        if (group.emoji_seen || value.kind != JsonValueKind::object) {
            group.error = group.emoji_seen ? GatewayEventDecodeError::duplicate_field
                                           : GatewayEventDecodeError::invalid_data;
            return false;
        }
        group.emoji_seen = true;
        group.emoji = value;
    }
    return true;
}

struct ReactionBatchVisit {
    JsonIndexedDocument document;
    Slice<char> name_scratch;
    GatewayMessageReactionEvent event;
    void* context;
    bool (*visit)(void*, const GatewayMessageReactionEvent&) noexcept;
    GatewayReactionBatchVisitResult result;
    u32 maximum_depth;
};

bool batch_read(ReactionBatchVisit& batch, JsonReadResult read,
                GatewayEventDecodeError error = GatewayEventDecodeError::none) noexcept {
    if (read.ok())
        return true;
    if (!batch.result.cancelled && batch.result.decoded.ok())
        batch.result.decoded =
            failure(error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json : error,
                    read.error_offset, JsonScanError::none, read.error);
    return false;
}

bool visit_batch_user(void* opaque, u32, JsonValue value) noexcept {
    auto& batch = *static_cast<ReactionBatchVisit*>(opaque);
    if (!json_value_to_snowflake(value, &batch.event.user_id)) {
        batch.result.decoded = failure(GatewayEventDecodeError::invalid_user, value.source_offset);
        return false;
    }
    if (batch.visit) {
        if (!batch.visit(batch.context, batch.event)) {
            batch.result.cancelled = true;
            return false;
        }
        ++batch.result.visited;
    }
    return true;
}

bool visit_batch_reaction(void* opaque, u32, JsonValue value) noexcept {
    auto& batch = *static_cast<ReactionBatchVisit*>(opaque);
    ReactionBatchGroup group{};
    const auto group_read =
        json_visit_indexed_object(batch.document, value, batch.maximum_depth, visit_batch_group, &group);
    if (!batch_read(batch, group_read, group.error))
        return false;
    if (!group.users_seen || !group.emoji_seen) {
        batch.result.decoded =
            failure(GatewayEventDecodeError::missing_required_field, value.source_offset);
        return false;
    }
    EmojiContext emoji{};
    const auto emoji_read =
        json_visit_indexed_object(batch.document, group.emoji, batch.maximum_depth, visit_emoji, &emoji);
    if (!batch_read(batch, emoji_read, emoji.error))
        return false;
    batch.result.decoded = finish_emoji(emoji, group.emoji.source_offset, batch.name_scratch, &batch.event);
    return batch.result.decoded.ok() &&
           batch_read(batch, json_visit_indexed_array(batch.document, group.users, batch.maximum_depth,
                                                      visit_batch_user, &batch));
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
    const auto finished = finish_emoji(emoji, context.emoji.source_offset, name_output, &context.event);
    if (!finished.ok())
        return finished;
    *output = context.event;
    return GatewayEventDecodeResult{};
}

GatewayReactionBatchVisitResult gateway_visit_message_reaction_add_many(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> name_scratch,
    void* context, bool (*visit)(void*, const GatewayMessageReactionEvent&) noexcept) noexcept {
    const ConstSlice<u8> source{reinterpret_cast<const u8*>(data.source.data), data.source.bytes};
    const ConstSlice<u8> names{reinterpret_cast<const u8*>(name_scratch.data), name_scratch.count};
    if (!visit || data.kind != JsonValueKind::object || !data.source.bytes || !maximum_depth ||
        !structural_scratch.data || !structural_scratch.count ||
        structural_scratch.count > ~u32{0} / sizeof(u32) || !byte_range_valid(source) ||
        !byte_range_valid(names))
        return {failure(GatewayEventDecodeError::invalid_input), 0, false};
    const ConstSlice<u8> indices{reinterpret_cast<const u8*>(structural_scratch.data),
                                 structural_scratch.count * static_cast<u32>(sizeof(u32))};
    if (!byte_range_valid(indices) || byte_ranges_overlap(source, indices) ||
        byte_ranges_overlap(source, names) || byte_ranges_overlap(indices, names))
        return {failure(GatewayEventDecodeError::invalid_input), 0, false};
    const auto scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok())
        return {failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error), 0, false};
    ReactionBatchVisit batch{};
    batch.document = {data.source, {structural_scratch.data, scan.count}};
    batch.name_scratch = name_scratch;
    batch.maximum_depth = maximum_depth;
    ReactionBatchRoot root{};
    const auto read =
        json_visit_indexed_object(batch.document, data, maximum_depth, visit_batch_root, &root);
    if (!batch_read(batch, read, root.header.error))
        return batch.result;
    constexpr u32 required = reaction_field_channel_id | reaction_field_message_id;
    if ((root.header.fields & required) != required || !root.reactions_seen)
        return {failure(GatewayEventDecodeError::missing_required_field, data.source_offset), 0, false};
    batch.event = root.header.event;
    batch.event.kind = GatewayMessageReactionEventKind::add;
    batch.event.type = MessageReactionType::normal;
    // No callback can observe a valid prefix of a malformed batch. Indexed
    // replay retains the same validated document and uses constant storage.
    const auto preflight = json_visit_indexed_array(batch.document, root.reactions, maximum_depth,
                                                    visit_batch_reaction, &batch);
    if (!batch_read(batch, preflight))
        return batch.result;
    batch.context = context;
    batch.visit = visit;
    const auto replay = json_visit_indexed_array(batch.document, root.reactions, maximum_depth,
                                                 visit_batch_reaction, &batch);
    batch_read(batch, replay);
    return batch.result;
}

} // namespace maicord
