#include "message_rich_internal.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"

namespace maicord {
namespace message_rich_detail {

// Parse decimal seconds as checked milliseconds without CRT floating conversion.
// JSON grammar has already been validated by the indexed reader. Sub-ms digits
// are discarded; an absent field remains distinct from a known zero duration.
bool duration_milliseconds(const JsonValue& value, u32* output) noexcept {
    if (value.kind != JsonValueKind::number || !value.source.data || !value.source.bytes)
        return false;
    const auto source = value.source;
    u32 at = 0, digits = 0, before_decimal = 0, leading_zeroes = 0;
    char significant[10]{};
    bool decimal = false, nonzero = false;
    if (source.data[0] == '-')
        return false;
    while (at < source.bytes && source.data[at] != 'e' && source.data[at] != 'E') {
        const char byte = source.data[at++];
        if (byte == '.') {
            if (decimal)
                return false;
            decimal = true;
            continue;
        }
        if (byte < '0' || byte > '9')
            return false;
        if (!decimal)
            ++before_decimal;
        if (!nonzero && byte == '0')
            ++leading_zeroes;
        else {
            nonzero = true;
            const u32 position = digits - leading_zeroes;
            if (position < 10)
                significant[position] = byte;
        }
        ++digits;
    }
    i64 exponent = 0;
    if (at < source.bytes) {
        ++at;
        bool negative = false;
        if (at < source.bytes && (source.data[at] == '+' || source.data[at] == '-'))
            negative = source.data[at++] == '-';
        if (at == source.bytes)
            return false;
        while (at < source.bytes) {
            const char byte = source.data[at++];
            if (byte < '0' || byte > '9')
                return false;
            // Saturation only affects values already far outside representable ms.
            if (exponent < 1000000)
                exponent = exponent * 10 + byte - '0';
        }
        if (negative)
            exponent = -exponent;
    }
    if (!nonzero) {
        *output = 0;
        return true;
    }
    const i64 integer_digits = static_cast<i64>(before_decimal) - leading_zeroes + exponent + 3;
    if (integer_digits <= 0) {
        *output = 0;
        return true;
    }
    if (integer_digits > 10)
        return false;
    u64 milliseconds = 0;
    for (i64 index = 0; index < integer_digits; ++index)
        milliseconds = milliseconds * 10 + (significant[index] ? significant[index] - '0' : 0);
    if (milliseconds > ~0u)
        return false;
    *output = static_cast<u32>(milliseconds);
    return true;
}

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
struct InteractionContext {
    u64 id;
    JsonValue user;
    JsonString name;
    JsonString localized_name;
    u32 type;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_interaction(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<InteractionContext*>(opaque);
    const u64 bit = member_bit<"id", "type", "user", "name", "name_localized">(name);
    if (!bit)
        return true;
    bool valid = true;
    switch (bit) {
    case 1:
        valid = json_value_to_snowflake(value, &context->id);
        break;
    case 2:
        valid = value_u32(value, &context->type) && context->type <= 255;
        break;
    case 4:
        context->user = value;
        valid = value.kind == JsonValueKind::object;
        break;
    case 8:
        valid = json_value_as_string(value, &context->name);
        break;
    case 16:
        valid = value.kind == JsonValueKind::null_value ||
                json_value_as_string(value, &context->localized_name);
        break;
    default:
        break;
    }
    return accept_member(&context->seen, bit, valid, &context->error);
}

GatewayEventDecodeResult decode_interaction(const JsonValue& value, const JsonIndexedDocument& scratch, u32 depth,
                                            Slice<char> text, u32* used,
                                            GatewayMessageEvent* output) noexcept {
    if (value.kind == JsonValueKind::null_value)
        return decode_failure(GatewayEventDecodeError::none);
    InteractionContext context{};
    auto result = read_object(value, scratch, depth, visit_interaction, &context, &context.error);
    if (!result.ok())
        return result;
    if ((context.seen & 15) != 15)
        return decode_failure(GatewayEventDecodeError::missing_required_field);
    AuthorContext user{};
    result = read_object(context.user, scratch, depth, visit_author, &user, &user.error);
    if (!result.ok())
        return result;
    if (!user.id)
        return decode_failure(GatewayEventDecodeError::invalid_author_id);
    auto& interaction = output->interaction;
    interaction.id = context.id;
    interaction.user_id = user.id;
    interaction.type = static_cast<u8>(context.type);
    const auto selected =
        user.global_name.encoded.data && !user.global_name_null ? user.global_name : user.username;
    const auto command = context.localized_name.encoded.data ? context.localized_name : context.name;
    if ((selected.encoded.data && !bounded_string(selected, kGatewayMessageUserNameMaximumBytes, text, used,
                                                  &interaction.user_name, &output->rich_truncations)) ||
        (user.avatar.encoded.data && !user.avatar_null &&
         !bounded_string(user.avatar, kGatewayMessageAvatarHashMaximumBytes, text, used,
                         &interaction.user_avatar_hash, &output->rich_truncations)) ||
        !bounded_string(command, 256, text, used, &interaction.command_name, &output->rich_truncations)) {
        return decode_failure(GatewayEventDecodeError::invalid_data);
    }
    return decode_failure(GatewayEventDecodeError::none);
}

struct ThreadContext {
    JsonString name;
    JsonValue metadata;
    u64 id, last_message_id;
    u32 message_count, member_count;
    u64 seen;
    bool archived, locked;
    GatewayEventDecodeError error;
};

bool visit_thread_metadata(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ThreadContext*>(opaque);
    const u32 which = member_index<"archived", "locked">(name);
    if (which > 1)
        return true;
    const bool archived = which == 0;
    if (value.kind == JsonValueKind::null_value)
        return true;
    if (!json_value_to_bool(value, archived ? &context->archived : &context->locked)) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    return true;
}

bool visit_thread(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ThreadContext*>(opaque);
    const u64 bit = member_bit<"id", "name", "message_count", "member_count", "last_message_id", "thread_metadata">(name);
    if (!bit)
        return true;
    bool valid = true;
    switch (bit) {
    case 1:
        valid = json_value_to_snowflake(value, &context->id);
        break;
    case 2:
        valid = json_value_as_string(value, &context->name);
        break;
    case 4:
        valid = value.kind == JsonValueKind::null_value || value_u32(value, &context->message_count);
        break;
    case 8:
        valid = value.kind == JsonValueKind::null_value || value_u32(value, &context->member_count);
        break;
    case 16:
        valid = value.kind == JsonValueKind::null_value ||
                json_value_to_snowflake(value, &context->last_message_id);
        break;
    default:
        context->metadata = value;
        valid = value.kind == JsonValueKind::object || value.kind == JsonValueKind::null_value;
        break;
    }
    return accept_member(&context->seen, bit, valid, &context->error);
}

GatewayEventDecodeResult decode_thread(const JsonValue& value, const JsonIndexedDocument& scratch, u32 depth,
                                       Slice<char> text, u32* used, GatewayMessageEvent* output) noexcept {
    reset_value(&output->thread);
    if (value.kind == JsonValueKind::null_value)
        return decode_failure(GatewayEventDecodeError::none);
    ThreadContext context{};
    auto result = read_object(value, scratch, depth, visit_thread, &context, &context.error);
    if (!result.ok())
        return result;
    if ((context.seen & 3) != 3 || !context.id)
        return decode_failure(GatewayEventDecodeError::missing_required_field);
    if (context.metadata.kind == JsonValueKind::object) {
        result = read_object(context.metadata, scratch, depth, visit_thread_metadata, &context, &context.error);
        if (!result.ok())
            return result;
    }
    auto& thread = output->thread;
    thread.id = context.id;
    thread.last_message_id = context.last_message_id;
    thread.message_count = context.message_count;
    thread.member_count = context.member_count;
    thread.archived = context.archived;
    thread.locked = context.locked;
    thread.present = true;
    if (!bounded_string(context.name, kGatewayMessageUserNameMaximumBytes, text, used, &thread.name,
                        &output->rich_truncations))
        return decode_failure(GatewayEventDecodeError::invalid_data);
    return decode_failure(GatewayEventDecodeError::none);
}
#pragma clang attribute pop

struct ArrayGather {
    JsonValue* values;
    u32 capacity;
    u32 count;
    u32 truncation;
    u32* truncations;
    GatewayEventDecodeError error;
};

bool gather_objects(void* opaque, u32, JsonValue value) noexcept {
    auto* gather = static_cast<ArrayGather*>(opaque);
    if (value.kind != JsonValueKind::object) {
        gather->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    if (gather->count < gather->capacity) {
        gather->values[gather->count++] = value;
    } else {
        *gather->truncations |= gather->truncation;
    }
    return true;
}

GatewayEventDecodeResult gather_array(const JsonValue& value, const JsonIndexedDocument& scratch,
                                      u32 maximum_depth, JsonValue* values, u32 capacity, u32 truncation,
                                      u32* truncations, u32* count) noexcept {
    // The root reader has already validated this array, including whitespace.
    const char* first = value.source.data + 1;
    while (static_cast<u8>(*first) <= ' ')
        ++first;
    if (*first == ']') {
        *count = 0;
        return decode_failure(GatewayEventDecodeError::none);
    }
    ArrayGather gather{values, capacity, 0, truncation, truncations, GatewayEventDecodeError::none};
    JsonReadResult read = json_visit_indexed_array(scratch, value, maximum_depth, gather_objects, &gather);
    const u32 begin = static_cast<u32>(value.source.data - scratch.input.data);
    if (!read.ok() && read.error_offset >= begin)
        read.error_offset -= begin;
    if (!read.ok()) {
        return read_failure(gather.error, read);
    }
    *count = gather.count;
    return decode_failure(GatewayEventDecodeError::none);
}

struct EmojiContext {
    u64 id;
    JsonString name;
    u64 seen;
    GatewayEventDecodeError error;
    bool name_null;
    bool animated;
};

bool visit_emoji(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmojiContext*>(opaque);
    switch (member_index<"id", "name", "animated">(name)) {
    case 0:
        return accept_member(&context->seen, 1ull << 0,
                             value.kind == JsonValueKind::null_value ||
                                 json_value_to_snowflake(value, &context->id),
                             &context->error);
    case 1:
        context->name_null = value.kind == JsonValueKind::null_value;
        return accept_member(&context->seen, 1ull << 1,
                             context->name_null || json_value_as_string(value, &context->name),
                             &context->error);
    case 2:
        return accept_member(&context->seen, 1ull << 2, json_value_to_bool(value, &context->animated),
                             &context->error);
    default: break;
    }
    return true;
}

struct CountDetailsContext {
    u32 normal;
    u32 burst;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_count_details(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<CountDetailsContext*>(opaque);
    switch (member_index<"normal", "burst">(name)) {
    case 0:
        return accept_member(&context->seen, 1ull << 0, value_u32(value, &context->normal),
                             &context->error);
    case 1:
        return accept_member(&context->seen, 1ull << 1, value_u32(value, &context->burst), &context->error);
    default: break;
    }
    return true;
}

struct ReactionContext {
    u32 count;
    bool me;
    bool me_burst;
    JsonValue emoji;
    JsonValue count_details;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_reaction(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReactionContext*>(opaque);
    switch (member_index<"count", "me", "me_burst", "emoji", "count_details">(name)) {
    case 0:
        return accept_member(&context->seen, 1ull << 0, value_u32(value, &context->count), &context->error);
    case 1:
        return accept_member(&context->seen, 1ull << 1, json_value_to_bool(value, &context->me),
                             &context->error);
    case 2:
        return accept_member(&context->seen, 1ull << 2, json_value_to_bool(value, &context->me_burst),
                             &context->error);
    case 3:
        if (!accept_member(&context->seen, 1ull << 3, value.kind == JsonValueKind::object, &context->error))
            return false;
        context->emoji = value;
        break;
    case 4:
        if (!accept_member(&context->seen, 1ull << 4, value.kind == JsonValueKind::object, &context->error))
            return false;
        context->count_details = value;
        break;
    default: break;
    }
    return true;
}

GatewayEventDecodeResult decode_reactions(JsonValue array, const JsonIndexedDocument& scratch,
                                          u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                          GatewayMessageRichScratch output_scratch,
                                          GatewayMessageEvent* output) noexcept {
    DecodeScratch<JsonValue, kGatewayMessageReactionMaximum> values;
    const u32 capacity = minimum<u32>(output_scratch.reactions.count, kGatewayMessageReactionMaximum);
    u32 count = 0;
    GatewayEventDecodeResult result =
        gather_array(array, scratch, maximum_depth, values, capacity,
                     gateway_message_rich_truncated_reactions, &output->rich_truncations, &count);
    if (!result.ok()) {
        return result;
    }
    for (u32 index = 0; index < count; ++index) {
        ReactionContext reaction{};
        result =
            read_object(values[index], scratch, maximum_depth, visit_reaction, &reaction, &reaction.error);
        if (!result.ok()) {
            return result;
        }
        constexpr u64 required = (1ull << 0) | (1ull << 1) | (1ull << 3);
        if ((reaction.seen & required) != required || reaction.count == 0) {
            return decode_failure(GatewayEventDecodeError::missing_required_field,
                                  values[index].source_offset);
        }
        EmojiContext emoji{};
        result = read_object(reaction.emoji, scratch, maximum_depth, visit_emoji, &emoji, &emoji.error);
        if (!result.ok()) {
            return result;
        }
        if ((emoji.seen & (1ull << 1)) == 0 || (emoji.name_null && emoji.id == 0)) {
            return decode_failure(GatewayEventDecodeError::invalid_data, reaction.emoji.source_offset);
        }
        CountDetailsContext details{};
        if ((reaction.seen & (1ull << 4)) != 0) {
            result = read_object(reaction.count_details, scratch, maximum_depth, visit_count_details,
                                 &details, &details.error);
            if (!result.ok()) {
                return result;
            }
            if (details.normal > reaction.count || details.burst > reaction.count - details.normal) {
                return decode_failure(GatewayEventDecodeError::invalid_data,
                                      reaction.count_details.source_offset);
            }
        } else {
            details.normal = reaction.count;
        }
        GatewayMessageReaction& destination = output_scratch.reactions[index];
        reset_value<GatewayMessageReaction>(&destination);
        destination.emoji_id = emoji.id;
        destination.count = reaction.count;
        destination.normal_count = details.normal;
        destination.burst_count = details.burst;
        destination.me = reaction.me;
        destination.me_burst = reaction.me_burst;
        destination.animated = emoji.animated;
        if (!emoji.name_null &&
            (emoji.name.encoded.bytes == 0 ||
             !bounded_string(emoji.name, kGatewayMessageUserNameMaximumBytes, text_output, text_used,
                             &destination.name, &output->rich_truncations))) {
            return decode_failure(GatewayEventDecodeError::invalid_data, reaction.emoji.source_offset);
        }
    }
    output->reactions = ConstSlice<GatewayMessageReaction>{output_scratch.reactions.data, count};
    return decode_failure(GatewayEventDecodeError::none);
}

struct AttachmentContext {
    GatewayMessageAttachment value;
    JsonString waveform;
    JsonString filename;
    JsonString title;
    JsonString description;
    JsonString content_type;
    JsonString url;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_attachment(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<AttachmentContext*>(opaque);
    switch (member_index<"id", "filename", "title", "description", "content_type", "size", "url", "width", "height",
                         "flags", "ephemeral", "duration_secs", "waveform">(name)) {
    case 0:
        return accept_member(&context->seen, 1ull << 0, json_value_to_snowflake(value, &context->value.id),
                             &context->error);
    case 1:
        return accept_member(&context->seen, 1ull << 1, json_value_as_string(value, &context->filename),
                             &context->error);
    case 2:
        return accept_member(&context->seen, 1ull << 2, json_value_as_string(value, &context->title),
                             &context->error);
    case 3:
        return accept_member(&context->seen, 1ull << 3, json_value_as_string(value, &context->description),
                             &context->error);
    case 4:
        return accept_member(&context->seen, 1ull << 4, json_value_as_string(value, &context->content_type),
                             &context->error);
    case 5:
        return accept_member(&context->seen, 1ull << 5, json_value_to_u64(value, &context->value.size),
                             &context->error);
    case 6:
        return accept_member(&context->seen, 1ull << 6, json_value_as_string(value, &context->url),
                             &context->error);
    case 7:
        if (!accept_member(&context->seen, 1ull << 7,
                           value.kind == JsonValueKind::null_value ||
                               value_u32(value, &context->value.width),
                           &context->error))
            return false;
        context->value.has_width = context->value.width != 0;
        break;
    case 8:
        if (!accept_member(&context->seen, 1ull << 8,
                           value.kind == JsonValueKind::null_value ||
                               value_u32(value, &context->value.height),
                           &context->error))
            return false;
        context->value.has_height = context->value.height != 0;
        break;
    case 9:
        return accept_member(&context->seen, 1ull << 9, value_u32(value, &context->value.flags),
                             &context->error);
    case 10:
        return accept_member(&context->seen, 1ull << 10,
                             json_value_to_bool(value, &context->value.ephemeral), &context->error);
    case 11: {
        const bool null = value.kind == JsonValueKind::null_value;
        if (!accept_member(&context->seen, 1ull << 11,
                           null || duration_milliseconds(value, &context->value.duration_ms),
                           &context->error))
            return false;
        context->value.has_duration = !null;
        break;
    }
    case 12:
        return accept_member(&context->seen, 1ull << 12,
                             value.kind == JsonValueKind::null_value ||
                                 json_value_as_string(value, &context->waveform),
                             &context->error);
    default: break;
    }
    return true;
}

GatewayEventDecodeResult decode_attachments(JsonValue array, const JsonIndexedDocument& scratch,
                                            u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                            GatewayMessageRichScratch output_scratch,
                                            GatewayMessageEvent* output) noexcept {
    DecodeScratch<JsonValue, kGatewayMessageAttachmentMaximum> values;
    const u32 capacity = minimum<u32>(output_scratch.attachments.count, kGatewayMessageAttachmentMaximum);
    u32 count = 0;
    GatewayEventDecodeResult result =
        gather_array(array, scratch, maximum_depth, values, capacity,
                     gateway_message_rich_truncated_attachments, &output->rich_truncations, &count);
    if (!result.ok()) {
        return result;
    }
    for (u32 index = 0; index < count; ++index) {
        AttachmentContext context{};
        result =
            read_object(values[index], scratch, maximum_depth, visit_attachment, &context, &context.error);
        if (!result.ok()) {
            return result;
        }
        constexpr u64 required = (1ull << 0) | (1ull << 1) | (1ull << 5) | (1ull << 6);
        if ((context.seen & required) != required) {
            return decode_failure(GatewayEventDecodeError::missing_required_field,
                                  values[index].source_offset);
        }
        GatewayMessageAttachment& destination = output_scratch.attachments[index];
        destination = context.value;
        if (!bounded_string(context.filename, kRichAttachmentNameMaximumBytes, text_output, text_used,
                            &destination.filename, &output->rich_truncations) ||
            ((context.seen & (1ull << 2)) != 0 &&
             !bounded_string(context.title, kRichAttachmentNameMaximumBytes, text_output, text_used,
                             &destination.title, &output->rich_truncations)) ||
            ((context.seen & (1ull << 3)) != 0 &&
             !bounded_string(context.description, kRichAttachmentDescriptionMaximumBytes, text_output,
                             text_used, &destination.description, &output->rich_truncations)) ||
            ((context.seen & (1ull << 4)) != 0 &&
             !bounded_string(context.content_type, kRichContentTypeMaximumBytes, text_output, text_used,
                             &destination.content_type, &output->rich_truncations)) ||
            !bounded_string(context.url, kRichUrlMaximumBytes, text_output, text_used, &destination.url,
                            &output->rich_truncations)) {
            return decode_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
        if (context.waveform.encoded.bytes) {
            char encoded[kGatewayMessageWaveformMaximumBytes];
            const auto decoded = json_string_decode_utf8(context.waveform, {encoded, sizeof(encoded)});
            if (!decoded.ok() || !gateway_message_waveform_valid({encoded, decoded.bytes}) ||
                !bounded_string(context.waveform, kGatewayMessageWaveformMaximumBytes, text_output,
                                text_used, &destination.waveform, &output->rich_truncations))
                return decode_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
        if (destination.url.bytes != 0 && !rich_url_valid(destination.url, false)) {
            return decode_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
    }
    output->attachments = ConstSlice<GatewayMessageAttachment>{output_scratch.attachments.data, count};
    return decode_failure(GatewayEventDecodeError::none);
}

bool visit_embed_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmbedFieldContext*>(opaque);
    switch (member_index<"name", "value", "inline">(name)) {
    case 0:
        return accept_member(&context->seen, 1ull << 0, json_value_as_string(value, &context->name),
                             &context->error);
    case 1:
        return accept_member(&context->seen, 1ull << 1, json_value_as_string(value, &context->value),
                             &context->error);
    case 2:
        return accept_member(&context->seen, 1ull << 2, json_value_to_bool(value, &context->inline_field),
                             &context->error);
    default: break;
    }
    return true;
}

bool visit_embed_part(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmbedPartContext*>(opaque);
    const u32 index = member_index<"name", "text", "url", "proxy_icon_url">(name);
    if (index > 3)
        return true;
    JsonString* const target = index == 0   ? &context->name
                               : index == 1 ? &context->text
                               : index == 2 ? &context->url
                                            : &context->proxy_icon_url;
    return value.kind == JsonValueKind::null_value ||
           accept_member(&context->seen, 1ull << index, json_value_as_string(value, target), &context->error);
}

// Optional embed URLs degrade to absent instead of rejecting the message.
bool embed_part_url(const EmbedPartContext& part, u64 bit, JsonString source, Slice<char> text_output,
                    u32* text_used, Utf8View* destination, u32* truncations) noexcept {
    if ((part.seen & bit) == 0) {
        return true;
    }
    const u32 rollback = *text_used;
    if (!bounded_string(source, kRichUrlMaximumBytes, text_output, text_used, destination, truncations)) {
        return false;
    }
    if (destination->bytes != 0 && !rich_url_valid(*destination, false)) {
        reset_value<Utf8View>(destination);
        *text_used = rollback;
    }
    return true;
}

} // namespace message_rich_detail
} // namespace maicord
