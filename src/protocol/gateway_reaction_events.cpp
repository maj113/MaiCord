#include "maicord/protocol/gateway_reaction_events.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/core/byte_range.hpp"
#include "gateway_decode.hpp"

namespace maicord {
namespace {

using protocol_detail::accept_member;
using protocol_detail::decode_failure;
using protocol_detail::read_failure;

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
    u32 seen;
    bool name_null;
    bool animated;
};
constexpr u32 emoji_seen_id = 1, emoji_seen_name = 2, emoji_seen_animated = 4;

bool visit_reaction(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReactionContext*>(opaque);
    u64 type = 0;
    if (json_string_equals_ascii(name, utf8_literal("message_id"))) {
        return accept_member(&context->fields, reaction_field_message_id,
                             json_value_to_snowflake(value, &context->event.message_id), &context->error,
                             GatewayEventDecodeError::invalid_message_id);
    }
    if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        return accept_member(&context->fields, reaction_field_channel_id,
                             json_value_to_snowflake(value, &context->event.channel_id), &context->error,
                             GatewayEventDecodeError::invalid_channel_id);
    }
    if (json_string_equals_ascii(name, utf8_literal("user_id"))) {
        return accept_member(&context->fields, reaction_field_user_id,
                             json_value_to_snowflake(value, &context->event.user_id), &context->error);
    }
    if (json_string_equals_ascii(name, utf8_literal("emoji"))) {
        if (!accept_member(&context->fields, reaction_field_emoji, value.kind == JsonValueKind::object,
                           &context->error))
            return false;
        context->emoji = value;
    } else if (json_string_equals_ascii(name, utf8_literal("type"))) {
        if (!accept_member(&context->fields, reaction_field_type,
                           json_value_to_u64(value, &type) &&
                               type <= static_cast<u8>(MessageReactionType::vote),
                           &context->error))
            return false;
        context->event.type = static_cast<MessageReactionType>(type);
    } else if (json_string_equals_ascii(name, utf8_literal("burst"))) {
        return accept_member(&context->fields, reaction_field_burst,
                             json_value_to_bool(value, &context->burst), &context->error);
    }
    return true;
}

bool visit_emoji(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmojiContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        return accept_member(&context->seen, emoji_seen_id,
                             value.kind == JsonValueKind::null_value ||
                                 json_value_to_snowflake(value, &context->id),
                             &context->error);
    }
    if (json_string_equals_ascii(name, utf8_literal("name"))) {
        context->name_null = value.kind == JsonValueKind::null_value;
        return accept_member(&context->seen, emoji_seen_name,
                             context->name_null || json_value_as_string(value, &context->name),
                             &context->error);
    }
    if (json_string_equals_ascii(name, utf8_literal("animated"))) {
        return accept_member(&context->seen, emoji_seen_animated,
                             json_value_to_bool(value, &context->animated), &context->error);
    }
    return true;
}

GatewayEventDecodeResult finish_emoji(const EmojiContext& emoji, u32 offset, Slice<char> name_output,
                                      GatewayMessageReactionEvent* output) noexcept {
    if (!(emoji.seen & emoji_seen_name))
        return decode_failure(GatewayEventDecodeError::missing_required_field, offset);
    output->emoji_id = emoji.id;
    output->animated = emoji.animated;
    output->name = {};
    // Deleted custom emoji keep their ID even when their name is unavailable.
    if (emoji.name_null)
        return emoji.id ? GatewayEventDecodeResult{}
                        : decode_failure(GatewayEventDecodeError::invalid_data, offset);
    const auto size = json_string_decode_utf8(emoji.name, {});
    if ((size.error != JsonStringDecodeError::output_too_small && !(size.ok() && size.bytes == 0)) ||
        !size.bytes || size.bytes > kMessageReactionMaximumEmojiNameBytes || size.bytes > name_output.count)
        return decode_failure(GatewayEventDecodeError::invalid_data, offset);
    const auto name = json_string_decode_utf8(emoji.name, name_output);
    if (!name.ok() || name.bytes != size.bytes)
        return decode_failure(GatewayEventDecodeError::invalid_data, offset);
    output->name = {name_output.data, name.bytes};
    return {};
}

struct ReactionBatchRoot {
    ReactionContext header;
    JsonValue reactions;
    u32 seen;
};
constexpr u32 root_seen_reactions = 1, root_seen_guild = 2;

bool visit_batch_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& root = *static_cast<ReactionBatchRoot*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("channel_id")) ||
        json_string_equals_ascii(name, utf8_literal("message_id")))
        return visit_reaction(&root.header, name, value);
    if (json_string_equals_ascii(name, utf8_literal("reactions"))) {
        if (!accept_member(&root.seen, root_seen_reactions, value.kind == JsonValueKind::array,
                           &root.header.error))
            return false;
        root.reactions = value;
    } else if (json_string_equals_ascii(name, utf8_literal("guild_id"))) {
        u64 guild_id = 0;
        return accept_member(&root.seen, root_seen_guild, json_value_to_snowflake(value, &guild_id),
                             &root.header.error);
    }
    return true;
}

struct ReactionBatchGroup {
    JsonValue users, emoji;
    GatewayEventDecodeError error;
    u32 seen;
};
constexpr u32 group_seen_users = 1, group_seen_emoji = 2;

bool visit_batch_group(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& group = *static_cast<ReactionBatchGroup*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("users"))) {
        if (!accept_member(&group.seen, group_seen_users, value.kind == JsonValueKind::array, &group.error))
            return false;
        group.users = value;
    } else if (json_string_equals_ascii(name, utf8_literal("emoji"))) {
        if (!accept_member(&group.seen, group_seen_emoji, value.kind == JsonValueKind::object,
                           &group.error))
            return false;
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
        batch.result.decoded = read_failure(error, read);
    return false;
}

bool visit_batch_user(void* opaque, u32, JsonValue value) noexcept {
    auto& batch = *static_cast<ReactionBatchVisit*>(opaque);
    if (!json_value_to_snowflake(value, &batch.event.user_id)) {
        batch.result.decoded = decode_failure(GatewayEventDecodeError::invalid_user, value.source_offset);
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
    if (group.seen != (group_seen_users | group_seen_emoji)) {
        batch.result.decoded =
            decode_failure(GatewayEventDecodeError::missing_required_field, value.source_offset);
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
    *output = GatewayMessageReactionEvent{};
    const JsonScanResult scan =
        json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    ReactionContext context{};
    context.event.kind = kind;
    context.event.type = MessageReactionType::normal;
    const JsonReadResult read = json_visit_value_object(
        data, ConstSlice<u32>{structural_scratch.data, scan.count},
        maximum_depth, visit_reaction, &context);
    if (!read.ok()) {
        return read_failure(context.error, read);
    }
    const u32 required = reaction_field_message_id | reaction_field_channel_id;
    const bool needs_user = kind == GatewayMessageReactionEventKind::add ||
                            kind == GatewayMessageReactionEventKind::remove;
    const bool needs_emoji = kind != GatewayMessageReactionEventKind::remove_all;
    if ((context.fields & required) != required ||
        (needs_user && (context.fields & reaction_field_user_id) == 0) ||
        (needs_emoji && (context.fields & reaction_field_emoji) == 0)) {
        return decode_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
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
        return decode_failure(GatewayEventDecodeError::scan_failed, emoji_scan.error_offset,
                              emoji_scan.error);
    }
    EmojiContext emoji{};
    const JsonReadResult emoji_read = json_visit_value_object(
        context.emoji,
        ConstSlice<u32>{structural_scratch.data, emoji_scan.count},
        maximum_depth, visit_emoji, &emoji);
    if (!emoji_read.ok()) {
        return read_failure(emoji.error, emoji_read);
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
        return {decode_failure(GatewayEventDecodeError::invalid_input), 0, false};
    const ConstSlice<u8> indices{reinterpret_cast<const u8*>(structural_scratch.data),
                                 structural_scratch.count * static_cast<u32>(sizeof(u32))};
    if (!byte_range_valid(indices) || byte_ranges_overlap(source, indices) ||
        byte_ranges_overlap(source, names) || byte_ranges_overlap(indices, names))
        return {decode_failure(GatewayEventDecodeError::invalid_input), 0, false};
    const auto scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok())
        return {decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error), 0,
                false};
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
    if ((root.header.fields & required) != required || !(root.seen & root_seen_reactions))
        return {decode_failure(GatewayEventDecodeError::missing_required_field, data.source_offset), 0,
                false};
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
