#include "message_rich_internal.hpp"

namespace maicord {
namespace message_rich_detail {

struct ReferenceContext {
    GatewayMessageReference value;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_reference(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReferenceContext*>(opaque);
    switch (member_index<"type", "message_id", "channel_id", "guild_id">(name)) {
    case 0: {
        u32 type = 0;
        if (!accept_member(&context->seen, 1ull << 0, value_u32(value, &type) && type <= 1,
                           &context->error)) {
            return false;
        }
        context->value.type = static_cast<u8>(type);
        break;
    }
    case 1:
        return accept_member(&context->seen, 1ull << 1,
                             json_value_to_snowflake(value, &context->value.message_id), &context->error,
                             GatewayEventDecodeError::invalid_message_id);
    case 2:
        return accept_member(&context->seen, 1ull << 2,
                             json_value_to_snowflake(value, &context->value.channel_id), &context->error,
                             GatewayEventDecodeError::invalid_channel_id);
    case 3:
        return accept_member(&context->seen, 1ull << 3,
                             json_value_to_snowflake(value, &context->value.guild_id), &context->error);
    default: break;
    }
    return true;
}

bool visit_author(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<AuthorContext*>(opaque);
    switch (member_index<"id", "username", "global_name", "avatar">(name)) {
    case 0:
        return accept_member(&context->seen, 1ull << 0, json_value_to_snowflake(value, &context->id),
                             &context->error, GatewayEventDecodeError::invalid_author_id);
    case 1:
        return accept_member(&context->seen, 1ull << 1, json_value_as_string(value, &context->username),
                             &context->error, GatewayEventDecodeError::invalid_user_name);
    case 2:
        context->global_name_null = value.kind == JsonValueKind::null_value;
        return accept_member(&context->seen, 1ull << 2,
                             context->global_name_null ||
                                 json_value_as_string(value, &context->global_name),
                             &context->error, GatewayEventDecodeError::invalid_user_name);
    case 3:
        context->avatar_null = value.kind == JsonValueKind::null_value;
        return accept_member(&context->seen, 1ull << 3,
                             context->avatar_null || json_value_as_string(value, &context->avatar),
                             &context->error);
    default: break;
    }
    return true;
}

struct ReferencedContext {
    u64 id;
    u64 channel_id;
    JsonValue author;
    JsonString content;
    JsonValue embeds;
    // The attachments (a clip mention reads one's title) and the message's own reference (a forward).
    JsonValue attachments, message_reference;
    JsonValue poll;
    u64 flags;
    u64 seen;
    u32 type;
    u8 content_flags;
    GatewayEventDecodeError error;
};

// Arrays are validated by the indexed reader: the first non-space byte after
// "[" says whether any element follows.
bool json_array_nonempty(const JsonValue& value) noexcept {
    if (value.kind != JsonValueKind::array)
        return false;
    for (u32 at = 1; at < value.source.bytes; ++at) {
        const char byte = value.source.data[at];
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
            return byte != ']';
    }
    return false;
}

bool visit_referenced(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReferencedContext*>(opaque);
    switch (member_index<"id", "channel_id", "author", "content", "attachments", "embeds", "sticker_items", "poll",
                         "interaction", "interaction_metadata", "flags", "type", "message_reference">(name)) {
    case 0:
        return accept_member(&context->seen, 1ull << 0, json_value_to_snowflake(value, &context->id),
                             &context->error, GatewayEventDecodeError::invalid_message_id);
    case 1:
        return accept_member(&context->seen, 1ull << 1,
                             json_value_to_snowflake(value, &context->channel_id), &context->error,
                             GatewayEventDecodeError::invalid_channel_id);
    case 2:
        if (!accept_member(&context->seen, 1ull << 2, value.kind == JsonValueKind::object, &context->error,
                           GatewayEventDecodeError::invalid_author))
            return false;
        context->author = value;
        break;
    case 3:
        return accept_member(&context->seen, 1ull << 3, json_value_as_string(value, &context->content),
                             &context->error, GatewayEventDecodeError::invalid_message_content);
    case 4:
        if (json_array_nonempty(value)) {
            context->content_flags |= gateway_reference_attachments;
            context->attachments = value;
        }
        break;
    case 5:
        if (json_array_nonempty(value)) {
            context->content_flags |= gateway_reference_embeds;
            context->embeds = value;
        }
        break;
    case 6:
        if (json_array_nonempty(value))
            context->content_flags |= gateway_reference_stickers;
        break;
    case 7:
        if (value.kind == JsonValueKind::object)
            context->poll = value;
        break;
    case 8:
    case 9:
        if (value.kind == JsonValueKind::object)
            context->content_flags |= gateway_reference_interaction;
        break;
    case 10:
        // Preview hints only: a malformed value never rejects the reply.
        if (!json_value_to_u64(value, &context->flags))
            context->flags = 0;
        break;
    case 11: {
        u64 type = 0;
        context->type = json_value_to_u64(value, &type) && type < 256 ? static_cast<u32>(type) : 0;
        break;
    }
    case 12:
        if (value.kind == JsonValueKind::object)
            context->message_reference = value;
        break;
    default: break;
    }
    return true;
}

// The referenced poll's question.text, and whether the referenced message is
// a lone image/gifv link whose content is hidden behind its embed.
struct ReferencedPreviewContext {
    const JsonIndexedDocument* document;
    u32 depth;
    JsonValue question;
    JsonString text, type, url;
    u32 count;
    bool described;
    // A poll result message's embed carries the question in a "poll_question_text" field.
    JsonValue fields;
    JsonString field_name, field_value, poll_question;
};
bool visit_referenced_question(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReferencedPreviewContext*>(opaque);
    switch (member_index<"question", "text">(name)) {
    case 0:
        if (value.kind == JsonValueKind::object)
            context->question = value;
        break;
    case 1:
        if (value.kind == JsonValueKind::string)
            json_value_as_string(value, &context->text);
        break;
    default: break;
    }
    return true;
}
bool visit_referenced_embed(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReferencedPreviewContext*>(opaque);
    switch (member_index<"type", "url", "title", "description", "fields">(name)) {
    case 0:
        if (value.kind == JsonValueKind::string)
            json_value_as_string(value, &context->type);
        break;
    case 1:
        if (value.kind == JsonValueKind::string)
            json_value_as_string(value, &context->url);
        break;
    case 2:
    case 3:
        if (value.kind == JsonValueKind::string)
            context->described = true;
        break;
    case 4:
        if (value.kind == JsonValueKind::array)
            context->fields = value;
        break;
    default: break;
    }
    return true;
}
bool visit_referenced_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReferencedPreviewContext*>(opaque);
    switch (member_index<"name", "value">(name)) {
    case 0:
        if (value.kind == JsonValueKind::string)
            json_value_as_string(value, &context->field_name);
        break;
    case 1:
        if (value.kind == JsonValueKind::string)
            json_value_as_string(value, &context->field_value);
        break;
    default: break;
    }
    return true;
}
bool visit_referenced_field_item(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<ReferencedPreviewContext*>(opaque);
    context->field_name = {};
    context->field_value = {};
    if (value.kind == JsonValueKind::object)
        json_visit_indexed_object(*context->document, value, context->depth, visit_referenced_field, context);
    char name[24];
    const auto decoded = json_string_decode_utf8(context->field_name, Slice<char>{name, sizeof(name)});
    if (!decoded.ok() || !utf8_equal(Utf8View{name, decoded.bytes}, utf8_literal("poll_question_text")))
        return true;
    context->poll_question = context->field_value;
    return false;
}
bool visit_referenced_embed_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<ReferencedPreviewContext*>(opaque);
    ++context->count;
    if (index == 0 && value.kind == JsonValueKind::object)
        json_visit_indexed_object(*context->document, value, context->depth, visit_referenced_embed, context);
    return true;
}
bool json_strings_equal(JsonString encoded, JsonString other) noexcept {
    char left[512], right[512];
    const auto a = json_string_decode_utf8(encoded, Slice<char>{left, sizeof(left)});
    const auto b = json_string_decode_utf8(other, Slice<char>{right, sizeof(right)});
    return a.ok() && b.ok() && a.bytes == b.bytes && a.bytes && bytes_equal(left, right, a.bytes);
}


// A reply pings the quoted author when the replying message's mentions list that user.
struct MentionProbe {
    const JsonIndexedDocument* document;
    u32 depth;
    u64 target;
    bool found;
};

bool visit_mention_user(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* probe = static_cast<MentionProbe*>(opaque);
    u64 id = 0;
    if (member_index<"id">(name) == 0 && json_value_to_snowflake(value, &id) && id == probe->target)
        probe->found = true;
    return true;
}

bool visit_mention_item(void* opaque, u32, JsonValue value) noexcept {
    auto* probe = static_cast<MentionProbe*>(opaque);
    if (value.kind == JsonValueKind::object)
        json_visit_indexed_object(*probe->document, value, probe->depth, visit_mention_user, probe);
    return !probe->found;
}

// Media mentions, interactions, threads, Components V2 and forwarded snapshots are rare, small payloads: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// The "type" of a quoted message's own message_reference (1: it is a forward).
bool visit_quote_reference_type(void* opaque, JsonString name, JsonValue value) noexcept {
    u32 type = 0;
    if (json_string_equals_utf8(name, utf8_literal("type")) && value_u32(value, &type))
        *static_cast<u32*>(opaque) = type;
    return true;
}

// The reply's media_mention: the attachment it points into and the moment, in ms.
struct MediaMentionProbe {
    u64 attachment_id, timestamp_ms;
};
bool visit_media_mention(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* probe = static_cast<MediaMentionProbe*>(opaque);
    if (json_string_equals_utf8(name, utf8_literal("attachment_id")))
        json_value_to_snowflake(value, &probe->attachment_id);
    else if (json_string_equals_utf8(name, utf8_literal("timestamp")))
        json_value_to_u64(value, &probe->timestamp_ms);
    return true;
}

// The quoted attachment a media mention names: its title and whether it is a clip (flag 1 << 0).
struct QuotedAttachmentProbe {
    const JsonIndexedDocument* document;
    u32 depth;
    u64 target;
    JsonValue title;
    u64 flags;
    u64 id;
    bool found;
};
bool visit_attachment_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* probe = static_cast<QuotedAttachmentProbe*>(opaque);
    if (json_string_equals_utf8(name, utf8_literal("id")))
        json_value_to_snowflake(value, &probe->id);
    else if (json_string_equals_utf8(name, utf8_literal("title")))
        probe->title = value;
    else if (json_string_equals_utf8(name, utf8_literal("flags")))
        json_value_to_u64(value, &probe->flags);
    return true;
}
bool visit_attachment_item(void* opaque, u32, JsonValue value) noexcept {
    auto* probe = static_cast<QuotedAttachmentProbe*>(opaque);
    if (value.kind != JsonValueKind::object)
        return true;
    probe->title = {};
    probe->flags = probe->id = 0;
    json_visit_indexed_object(*probe->document, value, probe->depth, visit_attachment_member, probe);
    probe->found = probe->id == probe->target;
    return !probe->found;
}

// "m:ss", or "h:mm:ss" from an hour on: where in the clip the mention points.
u32 format_media_time(u64 milliseconds, char* output) noexcept {
    const u64 seconds = milliseconds / 1000;
    const u32 hours = static_cast<u32>(seconds / 3600), minutes = static_cast<u32>(seconds / 60 % 60),
              rest = static_cast<u32>(seconds % 60);
    u32 used = 0;
    const auto digits = [&](u32 value, bool pad) noexcept {
        char text[10];
        u32 count = 0;
        do
            text[count++] = static_cast<char>('0' + value % 10);
        while (value /= 10);
        if (pad && count < 2)
            text[count++] = '0';
        while (count)
            output[used++] = text[--count];
    };
    if (hours) {
        digits(hours, false);
        output[used++] = ':';
    }
    digits(minutes, hours != 0);
    output[used++] = ':';
    digits(rest, true);
    return used;
}
#pragma clang attribute pop

GatewayEventDecodeResult decode_reference(const RootContext& root, const JsonIndexedDocument& scratch,
                                          u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                          GatewayMessageEvent* output) noexcept {
    if ((root.seen & gateway_message_field_message_reference) != 0) {
        ReferenceContext reference{};
        GatewayEventDecodeResult result = read_object(root.message_reference, scratch, maximum_depth,
                                                      visit_reference, &reference, &reference.error);
        if (!result.ok()) {
            return result;
        }
        if ((reference.seen & (1ull << 2)) == 0) {
            return decode_failure(GatewayEventDecodeError::missing_required_field,
                                  root.message_reference.source_offset);
        }
        output->reference = reference.value;
        output->reference.state = GatewayMessageReferenceState::unknown;
    }
    if ((root.seen & gateway_message_field_referenced_message) == 0) {
        return decode_failure(GatewayEventDecodeError::none);
    }
    if ((root.seen & gateway_message_field_message_reference) == 0) {
        return decode_failure(GatewayEventDecodeError::invalid_data, root.referenced_message.source_offset);
    }
    if (root.referenced_message.kind == JsonValueKind::null_value) {
        output->reference.state = GatewayMessageReferenceState::deleted;
        return decode_failure(GatewayEventDecodeError::none);
    }
    ReferencedContext referenced{};
    GatewayEventDecodeResult result = read_object(root.referenced_message, scratch, maximum_depth,
                                                  visit_referenced, &referenced, &referenced.error);
    if (!result.ok()) {
        return result;
    }
    constexpr u64 required = (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3);
    if ((referenced.seen & required) != required ||
        (output->reference.message_id != 0 && output->reference.message_id != referenced.id) ||
        output->reference.channel_id != referenced.channel_id) {
        return decode_failure(GatewayEventDecodeError::invalid_data, root.referenced_message.source_offset);
    }
    AuthorContext author{};
    result = read_object(referenced.author, scratch, maximum_depth, visit_author, &author, &author.error);
    if (!result.ok()) {
        return result;
    }
    if ((author.seen & (1ull << 0)) == 0) {
        return decode_failure(GatewayEventDecodeError::invalid_author_id, referenced.author.source_offset);
    }
    output->reference.message_id = referenced.id;
    output->reference.channel_id = referenced.channel_id;
    output->reference.author_id = author.id;
    output->reference.state = GatewayMessageReferenceState::available;
    const JsonString selected =
        (author.seen & (1ull << 2)) != 0 && !author.global_name_null ? author.global_name : author.username;
    if (selected.encoded.data &&
        !bounded_string(selected, kGatewayMessageUserNameMaximumBytes, text_output, text_used,
                        &output->reference.author_name, &output->rich_truncations)) {
        return decode_failure(GatewayEventDecodeError::invalid_user_name, referenced.author.source_offset);
    }
    if ((author.seen & (1ull << 3)) != 0 && !author.avatar_null &&
        !bounded_string(author.avatar, kGatewayMessageAvatarHashMaximumBytes, text_output, text_used,
                        &output->reference.author_avatar_hash, &output->rich_truncations)) {
        return decode_failure(GatewayEventDecodeError::invalid_data, referenced.author.source_offset);
    }
    constexpr u64 voice_message_flag = 1ull << 13, components_v2_flag = 1ull << 15;
    u8 content_flags = referenced.content_flags;
    if (referenced.flags & voice_message_flag)
        content_flags |= gateway_reference_voice_message;
    if (referenced.flags & components_v2_flag)
        content_flags |= gateway_reference_components_v2;
    output->reference.message_type = static_cast<u8>(referenced.type);
    if (root.mentions.source.data) {
        MentionProbe probe{&scratch, maximum_depth, author.id, false};
        json_visit_indexed_array(scratch, root.mentions, maximum_depth, visit_mention_item, &probe);
        if (probe.found)
            content_flags |= gateway_reference_mentions_author;
    }
    ReferencedPreviewContext preview{&scratch, maximum_depth, {}, {}, {}, {}, 0, false};
    bool simple_embed = false;
    if (referenced.content.encoded.bytes && referenced.embeds.source.data) {
        json_visit_indexed_array(scratch, referenced.embeds, maximum_depth, visit_referenced_embed_item, &preview);
        char type[8]{};
        const auto decoded = json_string_decode_utf8(preview.type, Slice<char>{type, sizeof(type)});
        const Utf8View kind{type, decoded.ok() ? decoded.bytes : 0u};
        simple_embed = preview.count == 1 && !preview.described &&
                       (utf8_equal(kind, utf8_literal("image")) || utf8_equal(kind, utf8_literal("gifv"))) &&
                       json_strings_equal(referenced.content, preview.url);
    }
    // A lone image/gifv link is hidden behind its simple embed; the
    // preview then falls back to "Click to see attachment".
    if (!simple_embed &&
        !bounded_string(referenced.content, kRichReferenceContentMaximumBytes, text_output, text_used,
                        &output->reference.content, &output->rich_truncations)) {
        return decode_failure(GatewayEventDecodeError::invalid_message_content,
                              root.referenced_message.source_offset);
    }
    if (!output->reference.content.bytes && referenced.poll.source.data) {
        json_visit_indexed_object(scratch, referenced.poll, maximum_depth, visit_referenced_question, &preview);
        if (preview.question.source.data) {
            reset_value(&preview.text);
            json_visit_indexed_object(scratch, preview.question, maximum_depth, visit_referenced_question,
                                      &preview);
            if (preview.text.encoded.data &&
                bounded_string(preview.text, kGatewayMessagePollQuestionMaximumBytes, text_output, text_used,
                               &output->reference.content, &output->rich_truncations) &&
                output->reference.content.bytes)
                content_flags |= gateway_reference_poll_question;
        }
    }
    // A poll result message has no text: its embed names the poll in a field.
    if (!output->reference.content.bytes && referenced.type == 46 && referenced.embeds.source.data) {
        json_visit_indexed_array(scratch, referenced.embeds, maximum_depth, visit_referenced_embed_item, &preview);
        if (preview.fields.source.data)
            json_visit_indexed_array(scratch, preview.fields, maximum_depth, visit_referenced_field_item, &preview);
        if (preview.poll_question.encoded.data &&
            bounded_string(preview.poll_question, kGatewayMessagePollQuestionMaximumBytes, text_output, text_used,
                           &output->reference.content, &output->rich_truncations) &&
            output->reference.content.bytes)
            content_flags |= gateway_reference_poll_question;
    }
    u8 quote_flags = 0;
    if (referenced.message_reference.source.data && referenced.message_reference.kind == JsonValueKind::object) {
        u32 quoted_type = 0;
        json_visit_indexed_object(scratch, referenced.message_reference, maximum_depth, visit_quote_reference_type,
                                  &quoted_type);
        if (quoted_type == 1)
            quote_flags |= gateway_quote_forwarded;
    }
    // A clip mention replaces the quoted text by the clip's title and the moment: "<title>\x1f@<time>".
    if (root.media_mention.source.data && root.media_mention.kind == JsonValueKind::object && text_output.data) {
        MediaMentionProbe mention{};
        json_visit_indexed_object(scratch, root.media_mention, maximum_depth, visit_media_mention, &mention);
        QuotedAttachmentProbe attachment{&scratch, maximum_depth, mention.attachment_id, {}, 0, 0, false};
        if (referenced.attachments.kind == JsonValueKind::array)
            json_visit_indexed_array(scratch, referenced.attachments, maximum_depth, visit_attachment_item,
                                     &attachment);
        char composed[kRichAttachmentNameMaximumBytes + 32];
        u32 bytes = 0;
        JsonString title{};
        if (attachment.found && json_value_as_string(attachment.title, &title)) {
            const auto decoded = json_string_decode_utf8(title, {composed, kRichAttachmentNameMaximumBytes});
            bytes = decoded.ok() ? decoded.bytes : 0u;
        }
        composed[bytes++] = '\x1f';
        composed[bytes++] = '@';
        bytes += format_media_time(mention.timestamp_ms, composed + bytes);
        if (bytes <= text_output.count - *text_used) {
            copy_bytes(text_output.data + *text_used, composed, bytes);
            output->reference.content = {text_output.data + *text_used, bytes};
            *text_used += bytes;
            quote_flags |= gateway_quote_media_mention | (attachment.found && (attachment.flags & 1) ? gateway_quote_media_clip : 0);
        }
    }
    output->reference.quote_flags = quote_flags;
    output->reference.content_flags = content_flags;
    return decode_failure(GatewayEventDecodeError::none);
}

} // namespace message_rich_detail
} // namespace maicord
