#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord {
namespace {

inline constexpr u32 kRichReferenceContentMaximumBytes = 16 * 1024;
inline constexpr u32 kRichUrlMaximumBytes = 2 * 1024;
inline constexpr u32 kRichAttachmentNameMaximumBytes = 512;
inline constexpr u32 kRichAttachmentDescriptionMaximumBytes = 4 * 1024;
inline constexpr u32 kRichContentTypeMaximumBytes = 128;
inline constexpr u32 kRichEmbedTitleMaximumBytes = 1024;
inline constexpr u32 kRichEmbedDescriptionMaximumBytes = 16 * 1024;
inline constexpr u32 kRichEmbedFooterMaximumBytes = 8 * 1024;
inline constexpr u32 kRichComponentTextMaximumBytes = 4 * 1024;
inline constexpr u32 kRichComponentIdMaximumBytes = 256;

GatewayEventDecodeResult rich_failure(GatewayEventDecodeError error, u32 offset = 0,
                                      JsonScanError scan_error = JsonScanError::none,
                                      JsonReadError read_error = JsonReadError::none) noexcept {
    return GatewayEventDecodeResult{offset, error, scan_error, read_error};
}

bool value_u32(JsonValue value, u32* output) noexcept {
    u64 parsed = 0;
    if (!json_value_to_u64(value, &parsed) || parsed > ~0u) {
        return false;
    }
    *output = static_cast<u32>(parsed);
    return true;
}

// Parse decimal seconds as checked milliseconds without CRT floating conversion.
// JSON grammar has already been validated by the indexed reader. Sub-ms digits
// are discarded; an absent field remains distinct from a known zero duration.
bool duration_milliseconds(JsonValue value, u32* output) noexcept {
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

bool mark(u64* fields, u64 bit, GatewayEventDecodeError* error) noexcept {
    if ((*fields & bit) != 0) {
        *error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    *fields |= bit;
    return true;
}

bool bounded_string(JsonString source, u32 maximum_bytes, Slice<char> output, u32* used, Utf8View* value,
                    u32* truncations) noexcept {
    if (*used > output.count) {
        return false;
    }
    *value = Utf8View{};
    const u32 remaining = output.count - *used;
    const JsonStringDecodeResult decoded = json_string_decode_utf8(
        source, Slice<char>{output.data ? output.data + *used : nullptr,
                            output.data ? minimum(maximum_bytes, remaining) : 0});
    if (decoded.error == JsonStringDecodeError::output_too_small) {
        if (decoded.bytes > maximum_bytes || decoded.bytes > remaining) {
            *truncations |= gateway_message_rich_truncated_text;
            return true;
        }
        return false;
    }
    if (!decoded.ok()) {
        return false;
    }
    if (decoded.bytes == 0) {
        return true;
    }
    *value = Utf8View{output.data + *used, decoded.bytes};
    *used += decoded.bytes;
    return true;
}

bool ascii_prefix(Utf8View value, const char* prefix, u32 bytes) noexcept {
    if (!value.data || value.bytes < bytes) {
        return false;
    }
    for (u32 index = 0; index < bytes; ++index) {
        if (value.data[index] != prefix[index]) {
            return false;
        }
    }
    return true;
}

bool rich_url_valid(Utf8View value, bool attachment_allowed) noexcept {
    if (!value.data || value.bytes == 0 || value.bytes > kRichUrlMaximumBytes ||
        !utf8_validate(value).valid) {
        return false;
    }
    return ascii_prefix(value, "https://", 8) || ascii_prefix(value, "http://", 7) ||
           (attachment_allowed && ascii_prefix(value, "attachment://", 13));
}

struct RootContext {
    JsonValue message_reference;
    JsonValue referenced_message;
    JsonValue reactions;
    JsonValue attachments;
    JsonValue embeds;
    JsonValue stickers;
    JsonValue components;
    JsonValue interaction;
    JsonValue snapshots;
    JsonValue poll;
    JsonValue thread;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<RootContext*>(opaque);
    u64 bit = 0;
    JsonValue* target = nullptr;
    JsonValueKind expected = JsonValueKind::array;
    if (json_string_equals_ascii(name, utf8_literal("message_reference"))) {
        bit = gateway_message_field_message_reference;
        target = &context->message_reference;
        expected = JsonValueKind::object;
    } else if (json_string_equals_ascii(name, utf8_literal("referenced_message"))) {
        bit = gateway_message_field_referenced_message;
        target = &context->referenced_message;
        expected = value.kind;
        if (value.kind != JsonValueKind::object && value.kind != JsonValueKind::null_value) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("reactions"))) {
        bit = gateway_message_field_reactions;
        target = &context->reactions;
    } else if (json_string_equals_ascii(name, utf8_literal("attachments"))) {
        bit = gateway_message_field_attachments;
        target = &context->attachments;
    } else if (json_string_equals_ascii(name, utf8_literal("embeds"))) {
        bit = gateway_message_field_embeds;
        target = &context->embeds;
    } else if (json_string_equals_ascii(name, utf8_literal("sticker_items"))) {
        bit = gateway_message_field_sticker_items;
        target = &context->stickers;
    } else if (json_string_equals_ascii(name, utf8_literal("components"))) {
        bit = gateway_message_field_components;
        target = &context->components;
    } else if (json_string_equals_ascii(name, utf8_literal("message_snapshots"))) {
        bit = gateway_message_field_message_snapshots;
        target = &context->snapshots;
        expected = value.kind == JsonValueKind::null_value ? value.kind : JsonValueKind::array;
    } else if (json_string_equals_ascii(name, utf8_literal("poll"))) {
        bit = gateway_message_field_poll;
        target = &context->poll;
        expected = value.kind == JsonValueKind::null_value ? value.kind : JsonValueKind::object;
    } else if (json_string_equals_ascii(name, utf8_literal("interaction"))) {
        bit = gateway_message_field_interaction;
        target = &context->interaction;
        expected = value.kind == JsonValueKind::null_value ? value.kind : JsonValueKind::object;
    } else if (json_string_equals_ascii(name, utf8_literal("thread"))) {
        bit = gateway_message_field_thread;
        target = &context->thread;
        expected = value.kind == JsonValueKind::null_value ? value.kind : JsonValueKind::object;
    } else {
        return true;
    }
    if (!mark(&context->seen, bit, &context->error)) {
        return false;
    }
    if (value.kind != expected) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    *target = value;
    return true;
}

struct ReferenceContext {
    GatewayMessageReference value;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_reference(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReferenceContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("type"))) {
        if (!mark(&context->seen, 1ull << 0, &context->error)) {
            return false;
        }
        u32 type = 0;
        if (!value_u32(value, &type) || type > 1) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->value.type = static_cast<u8>(type);
    } else if (json_string_equals_ascii(name, utf8_literal("message_id"))) {
        if (!mark(&context->seen, 1ull << 1, &context->error) ||
            !json_value_to_snowflake(value, &context->value.message_id)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_message_id
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        if (!mark(&context->seen, 1ull << 2, &context->error) ||
            !json_value_to_snowflake(value, &context->value.channel_id)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_channel_id
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("guild_id"))) {
        if (!mark(&context->seen, 1ull << 3, &context->error) ||
            !json_value_to_snowflake(value, &context->value.guild_id)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    }
    return true;
}

struct AuthorContext {
    u64 id;
    JsonString username;
    JsonString global_name;
    JsonString avatar;
    u64 seen;
    GatewayEventDecodeError error;
    bool global_name_null;
    bool avatar_null;
};

bool visit_author(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<AuthorContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!mark(&context->seen, 1ull << 0, &context->error) ||
            !json_value_to_snowflake(value, &context->id)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_author_id
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("username"))) {
        if (!mark(&context->seen, 1ull << 1, &context->error) ||
            !json_value_as_string(value, &context->username)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_user_name
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("global_name"))) {
        if (!mark(&context->seen, 1ull << 2, &context->error)) {
            return false;
        }
        context->global_name_null = value.kind == JsonValueKind::null_value;
        if (!context->global_name_null && !json_value_as_string(value, &context->global_name)) {
            context->error = GatewayEventDecodeError::invalid_user_name;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("avatar"))) {
        if (!mark(&context->seen, 1ull << 3, &context->error)) {
            return false;
        }
        context->avatar_null = value.kind == JsonValueKind::null_value;
        if (!context->avatar_null && !json_value_as_string(value, &context->avatar)) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    }
    return true;
}

struct ReferencedContext {
    u64 id;
    u64 channel_id;
    JsonValue author;
    JsonString content;
    JsonValue embeds;
    JsonValue poll;
    u64 flags;
    u64 seen;
    u32 type;
    u8 content_flags;
    GatewayEventDecodeError error;
};

// Arrays are validated by the indexed reader: the first non-space byte after
// "[" says whether any element follows.
bool json_array_nonempty(JsonValue value) noexcept {
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
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!mark(&context->seen, 1ull << 0, &context->error) ||
            !json_value_to_snowflake(value, &context->id)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_message_id
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        if (!mark(&context->seen, 1ull << 1, &context->error) ||
            !json_value_to_snowflake(value, &context->channel_id)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_channel_id
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("author"))) {
        if (!mark(&context->seen, 1ull << 2, &context->error) || value.kind != JsonValueKind::object) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_author
                                 : context->error;
            return false;
        }
        context->author = value;
    } else if (json_string_equals_ascii(name, utf8_literal("content"))) {
        if (!mark(&context->seen, 1ull << 3, &context->error) ||
            !json_value_as_string(value, &context->content)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_message_content
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("attachments"))) {
        if (json_array_nonempty(value))
            context->content_flags |= gateway_reference_attachments;
    } else if (json_string_equals_ascii(name, utf8_literal("embeds"))) {
        if (json_array_nonempty(value)) {
            context->content_flags |= gateway_reference_embeds;
            context->embeds = value;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("sticker_items"))) {
        if (json_array_nonempty(value))
            context->content_flags |= gateway_reference_stickers;
    } else if (json_string_equals_ascii(name, utf8_literal("poll"))) {
        if (value.kind == JsonValueKind::object)
            context->poll = value;
    } else if (json_string_equals_ascii(name, utf8_literal("interaction")) ||
               json_string_equals_ascii(name, utf8_literal("interaction_metadata"))) {
        if (value.kind == JsonValueKind::object)
            context->content_flags |= gateway_reference_interaction;
    } else if (json_string_equals_ascii(name, utf8_literal("flags"))) {
        // Preview hints only: a malformed value never rejects the reply.
        if (!json_value_to_u64(value, &context->flags))
            context->flags = 0;
    } else if (json_string_equals_ascii(name, utf8_literal("type"))) {
        u64 type = 0;
        context->type = json_value_to_u64(value, &type) && type < 256 ? static_cast<u32>(type) : 0;
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
};
bool visit_referenced_question(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReferencedPreviewContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("question")) && value.kind == JsonValueKind::object)
        context->question = value;
    else if (json_string_equals_ascii(name, utf8_literal("text")) && value.kind == JsonValueKind::string)
        json_value_as_string(value, &context->text);
    return true;
}
bool visit_referenced_embed(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReferencedPreviewContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("type")) && value.kind == JsonValueKind::string)
        json_value_as_string(value, &context->type);
    else if (json_string_equals_ascii(name, utf8_literal("url")) && value.kind == JsonValueKind::string)
        json_value_as_string(value, &context->url);
    else if ((json_string_equals_ascii(name, utf8_literal("title")) ||
              json_string_equals_ascii(name, utf8_literal("description"))) &&
             value.kind == JsonValueKind::string)
        context->described = true;
    return true;
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

GatewayEventDecodeResult read_object(JsonValue value, const JsonIndexedDocument& scratch, u32 maximum_depth,
                                     JsonMemberVisitor visitor, void* context,
                                     GatewayEventDecodeError* schema_error) noexcept {
    if (value.kind != JsonValueKind::object) {
        return rich_failure(GatewayEventDecodeError::invalid_data, value.source_offset);
    }
    JsonReadResult read = json_visit_indexed_object(scratch, value, maximum_depth, visitor, context);
    // Diagnostic offsets are relative to the child object.
    const u32 begin = static_cast<u32>(value.source.data - scratch.input.data);
    if (!read.ok() && read.error_offset >= begin)
        read.error_offset -= begin;
    if (!read.ok()) {
        auto result = rich_failure(*schema_error != GatewayEventDecodeError::none
                                       ? *schema_error
                                       : GatewayEventDecodeError::invalid_json,
                                   read.error_offset, JsonScanError::none, read.error);
        // Schema keys only: no field values or message content in diagnostics.
        if (read.error == JsonReadError::visitor_rejected && read.error_offset < value.source.bytes) {
            u32 at = read.error_offset;
            if (value.source.data[at] == '\"')
                ++at;
            for (u32 n = 0; n != 31 && at < value.source.bytes; ++n, ++at) {
                const char ch = value.source.data[at];
                if (ch == '\"')
                    break;
                if (!((ch >= 'a' && ch <= 'z') || ch == '_'))
                    break;
                result.schema_field[n] = ch;
            }
        }
        return result;
    }
    return rich_failure(GatewayEventDecodeError::none);
}

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
            return rich_failure(GatewayEventDecodeError::missing_required_field,
                                root.message_reference.source_offset);
        }
        output->reference = reference.value;
        output->reference.state = GatewayMessageReferenceState::unknown;
    }
    if ((root.seen & gateway_message_field_referenced_message) == 0) {
        return rich_failure(GatewayEventDecodeError::none);
    }
    if ((root.seen & gateway_message_field_message_reference) == 0) {
        return rich_failure(GatewayEventDecodeError::invalid_data, root.referenced_message.source_offset);
    }
    if (root.referenced_message.kind == JsonValueKind::null_value) {
        output->reference.state = GatewayMessageReferenceState::deleted;
        return rich_failure(GatewayEventDecodeError::none);
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
        return rich_failure(GatewayEventDecodeError::invalid_data, root.referenced_message.source_offset);
    }
    AuthorContext author{};
    result = read_object(referenced.author, scratch, maximum_depth, visit_author, &author, &author.error);
    if (!result.ok()) {
        return result;
    }
    if ((author.seen & (1ull << 0)) == 0) {
        return rich_failure(GatewayEventDecodeError::invalid_author_id, referenced.author.source_offset);
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
        return rich_failure(GatewayEventDecodeError::invalid_user_name, referenced.author.source_offset);
    }
    if ((author.seen & (1ull << 3)) != 0 && !author.avatar_null &&
        !bounded_string(author.avatar, kGatewayMessageAvatarHashMaximumBytes, text_output, text_used,
                        &output->reference.author_avatar_hash, &output->rich_truncations)) {
        return rich_failure(GatewayEventDecodeError::invalid_data, referenced.author.source_offset);
    }
    constexpr u64 voice_message_flag = 1ull << 13, components_v2_flag = 1ull << 15;
    u8 content_flags = referenced.content_flags;
    if (referenced.flags & voice_message_flag)
        content_flags |= gateway_reference_voice_message;
    if (referenced.flags & components_v2_flag)
        content_flags |= gateway_reference_components_v2;
    output->reference.message_type = static_cast<u8>(referenced.type);
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
        return rich_failure(GatewayEventDecodeError::invalid_message_content,
                            root.referenced_message.source_offset);
    }
    if (!output->reference.content.bytes && referenced.poll.source.data) {
        preview.text = {};
        json_visit_indexed_object(scratch, referenced.poll, maximum_depth, visit_referenced_question, &preview);
        if (preview.question.source.data) {
            preview.text = {};
            json_visit_indexed_object(scratch, preview.question, maximum_depth, visit_referenced_question,
                                      &preview);
            if (preview.text.encoded.data &&
                bounded_string(preview.text, kGatewayMessagePollQuestionMaximumBytes, text_output, text_used,
                               &output->reference.content, &output->rich_truncations) &&
                output->reference.content.bytes)
                content_flags |= gateway_reference_poll_question;
        }
    }
    output->reference.content_flags = content_flags;
    return rich_failure(GatewayEventDecodeError::none);
}

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
    u64 bit = 0;
    if (json_string_equals_ascii(name, utf8_literal("id")))
        bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("type")))
        bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("user")))
        bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("name")))
        bit = 8;
    else if (json_string_equals_ascii(name, utf8_literal("name_localized")))
        bit = 16;
    else
        return true;
    if (!mark(&context->seen, bit, &context->error))
        return false;
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
    if (!valid)
        context->error = GatewayEventDecodeError::invalid_data;
    return valid;
}

GatewayEventDecodeResult decode_interaction(JsonValue value, const JsonIndexedDocument& scratch, u32 depth,
                                            Slice<char> text, u32* used,
                                            GatewayMessageEvent* output) noexcept {
    if (value.kind == JsonValueKind::null_value)
        return rich_failure(GatewayEventDecodeError::none);
    InteractionContext context{};
    auto result = read_object(value, scratch, depth, visit_interaction, &context, &context.error);
    if (!result.ok())
        return result;
    if ((context.seen & 15) != 15)
        return rich_failure(GatewayEventDecodeError::missing_required_field);
    AuthorContext user{};
    result = read_object(context.user, scratch, depth, visit_author, &user, &user.error);
    if (!result.ok())
        return result;
    if (!user.id)
        return rich_failure(GatewayEventDecodeError::invalid_author_id);
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
        return rich_failure(GatewayEventDecodeError::invalid_data);
    }
    return rich_failure(GatewayEventDecodeError::none);
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
    const bool archived = json_string_equals_ascii(name, utf8_literal("archived"));
    if (!archived && !json_string_equals_ascii(name, utf8_literal("locked")))
        return true;
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
    u64 bit = 0;
    if (json_string_equals_ascii(name, utf8_literal("id")))
        bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("name")))
        bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("message_count")))
        bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("member_count")))
        bit = 8;
    else if (json_string_equals_ascii(name, utf8_literal("last_message_id")))
        bit = 16;
    else if (json_string_equals_ascii(name, utf8_literal("thread_metadata")))
        bit = 32;
    else
        return true;
    if (!mark(&context->seen, bit, &context->error))
        return false;
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
    if (!valid)
        context->error = GatewayEventDecodeError::invalid_data;
    return valid;
}

GatewayEventDecodeResult decode_thread(JsonValue value, const JsonIndexedDocument& scratch, u32 depth,
                                       Slice<char> text, u32* used, GatewayMessageEvent* output) noexcept {
    output->thread = {};
    if (value.kind == JsonValueKind::null_value)
        return rich_failure(GatewayEventDecodeError::none);
    ThreadContext context{};
    auto result = read_object(value, scratch, depth, visit_thread, &context, &context.error);
    if (!result.ok())
        return result;
    if ((context.seen & 3) != 3 || !context.id)
        return rich_failure(GatewayEventDecodeError::missing_required_field);
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
        return rich_failure(GatewayEventDecodeError::invalid_data);
    return rich_failure(GatewayEventDecodeError::none);
}

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

GatewayEventDecodeResult gather_array(JsonValue value, const JsonIndexedDocument& scratch,
                                      u32 maximum_depth, JsonValue* values, u32 capacity, u32 truncation,
                                      u32* truncations, u32* count) noexcept {
    // The root reader has already validated this array, including whitespace.
    const char* first = value.source.data + 1;
    while (static_cast<u8>(*first) <= ' ')
        ++first;
    if (*first == ']') {
        *count = 0;
        return rich_failure(GatewayEventDecodeError::none);
    }
    ArrayGather gather{values, capacity, 0, truncation, truncations, GatewayEventDecodeError::none};
    JsonReadResult read = json_visit_indexed_array(scratch, value, maximum_depth, gather_objects, &gather);
    const u32 begin = static_cast<u32>(value.source.data - scratch.input.data);
    if (!read.ok() && read.error_offset >= begin)
        read.error_offset -= begin;
    if (!read.ok()) {
        return rich_failure(gather.error != GatewayEventDecodeError::none
                                ? gather.error
                                : GatewayEventDecodeError::invalid_json,
                            read.error_offset, JsonScanError::none, read.error);
    }
    *count = gather.count;
    return rich_failure(GatewayEventDecodeError::none);
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
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!mark(&context->seen, 1ull << 0, &context->error)) {
            return false;
        }
        if (value.kind != JsonValueKind::null_value &&
            !json_value_to_snowflake(value, &context->id)) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("name"))) {
        if (!mark(&context->seen, 1ull << 1, &context->error)) {
            return false;
        }
        context->name_null = value.kind == JsonValueKind::null_value;
        if (!context->name_null && !json_value_as_string(value, &context->name)) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("animated"))) {
        if (!mark(&context->seen, 1ull << 2, &context->error) ||
            !json_value_to_bool(value, &context->animated)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
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
    if (json_string_equals_ascii(name, utf8_literal("normal"))) {
        if (!mark(&context->seen, 1ull << 0, &context->error) || !value_u32(value, &context->normal)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("burst"))) {
        if (!mark(&context->seen, 1ull << 1, &context->error) || !value_u32(value, &context->burst)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
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
    if (json_string_equals_ascii(name, utf8_literal("count"))) {
        if (!mark(&context->seen, 1ull << 0, &context->error) || !value_u32(value, &context->count)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("me"))) {
        if (!mark(&context->seen, 1ull << 1, &context->error) || !json_value_to_bool(value, &context->me)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("me_burst"))) {
        if (!mark(&context->seen, 1ull << 2, &context->error) ||
            !json_value_to_bool(value, &context->me_burst)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("emoji"))) {
        if (!mark(&context->seen, 1ull << 3, &context->error) || value.kind != JsonValueKind::object) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        context->emoji = value;
    } else if (json_string_equals_ascii(name, utf8_literal("count_details"))) {
        if (!mark(&context->seen, 1ull << 4, &context->error) || value.kind != JsonValueKind::object) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        context->count_details = value;
    }
    return true;
}

GatewayEventDecodeResult decode_reactions(JsonValue array, const JsonIndexedDocument& scratch,
                                          u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                          GatewayMessageRichScratch output_scratch,
                                          GatewayMessageEvent* output) noexcept {
    JsonValue values[kGatewayMessageReactionMaximum]{};
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
            return rich_failure(GatewayEventDecodeError::missing_required_field,
                                values[index].source_offset);
        }
        EmojiContext emoji{};
        result = read_object(reaction.emoji, scratch, maximum_depth, visit_emoji, &emoji, &emoji.error);
        if (!result.ok()) {
            return result;
        }
        if ((emoji.seen & (1ull << 1)) == 0 || (emoji.name_null && emoji.id == 0)) {
            return rich_failure(GatewayEventDecodeError::invalid_data, reaction.emoji.source_offset);
        }
        CountDetailsContext details{};
        if ((reaction.seen & (1ull << 4)) != 0) {
            result = read_object(reaction.count_details, scratch, maximum_depth, visit_count_details,
                                 &details, &details.error);
            if (!result.ok()) {
                return result;
            }
            if (details.normal > reaction.count || details.burst > reaction.count - details.normal) {
                return rich_failure(GatewayEventDecodeError::invalid_data,
                                    reaction.count_details.source_offset);
            }
        } else {
            details.normal = reaction.count;
        }
        GatewayMessageReaction& destination = output_scratch.reactions[index];
        destination = GatewayMessageReaction{};
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
            return rich_failure(GatewayEventDecodeError::invalid_data, reaction.emoji.source_offset);
        }
    }
    output->reactions = ConstSlice<GatewayMessageReaction>{output_scratch.reactions.data, count};
    return rich_failure(GatewayEventDecodeError::none);
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
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!mark(&context->seen, 1ull << 0, &context->error) ||
            !json_value_to_snowflake(value, &context->value.id)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("filename"))) {
        if (!mark(&context->seen, 1ull << 1, &context->error) ||
            !json_value_as_string(value, &context->filename)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("title"))) {
        if (!mark(&context->seen, 1ull << 2, &context->error) ||
            !json_value_as_string(value, &context->title)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("description"))) {
        if (!mark(&context->seen, 1ull << 3, &context->error) ||
            !json_value_as_string(value, &context->description)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("content_type"))) {
        if (!mark(&context->seen, 1ull << 4, &context->error) ||
            !json_value_as_string(value, &context->content_type)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("size"))) {
        if (!mark(&context->seen, 1ull << 5, &context->error) ||
            !json_value_to_u64(value, &context->value.size)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("url"))) {
        if (!mark(&context->seen, 1ull << 6, &context->error) ||
            !json_value_as_string(value, &context->url)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("width"))) {
        if (!mark(&context->seen, 1ull << 7, &context->error)) {
            return false;
        }
        if (value.kind != JsonValueKind::null_value) {
            if (!value_u32(value, &context->value.width)) {
                context->error = GatewayEventDecodeError::invalid_data;
                return false;
            }
            context->value.has_width = context->value.width != 0;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("height"))) {
        if (!mark(&context->seen, 1ull << 8, &context->error)) {
            return false;
        }
        if (value.kind != JsonValueKind::null_value) {
            if (!value_u32(value, &context->value.height)) {
                context->error = GatewayEventDecodeError::invalid_data;
                return false;
            }
            context->value.has_height = context->value.height != 0;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("flags"))) {
        if (!mark(&context->seen, 1ull << 9, &context->error) || !value_u32(value, &context->value.flags)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("duration_secs"))) {
        if (!mark(&context->seen, 1ull << 11, &context->error))
            return false;
        if (value.kind != JsonValueKind::null_value) {
            if (!duration_milliseconds(value, &context->value.duration_ms)) {
                context->error = GatewayEventDecodeError::invalid_data;
                return false;
            }
            context->value.has_duration = true;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("waveform"))) {
        if (!mark(&context->seen, 1ull << 12, &context->error))
            return false;
        if (value.kind != JsonValueKind::null_value && !json_value_as_string(value, &context->waveform)) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("ephemeral"))) {
        if (!mark(&context->seen, 1ull << 10, &context->error) ||
            !json_value_to_bool(value, &context->value.ephemeral)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    }
    return true;
}

GatewayEventDecodeResult decode_attachments(JsonValue array, const JsonIndexedDocument& scratch,
                                            u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                            GatewayMessageRichScratch output_scratch,
                                            GatewayMessageEvent* output) noexcept {
    JsonValue values[kGatewayMessageAttachmentMaximum]{};
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
            return rich_failure(GatewayEventDecodeError::missing_required_field,
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
            return rich_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
        if (context.waveform.encoded.bytes) {
            char encoded[kGatewayMessageWaveformMaximumBytes];
            const auto decoded = json_string_decode_utf8(context.waveform, {encoded, sizeof(encoded)});
            if (!decoded.ok() || !gateway_message_waveform_valid({encoded, decoded.bytes}) ||
                !bounded_string(context.waveform, kGatewayMessageWaveformMaximumBytes, text_output,
                                text_used, &destination.waveform, &output->rich_truncations))
                return rich_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
        if (destination.url.bytes != 0 && !rich_url_valid(destination.url, false)) {
            return rich_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
    }
    output->attachments = ConstSlice<GatewayMessageAttachment>{output_scratch.attachments.data, count};
    return rich_failure(GatewayEventDecodeError::none);
}

struct EmbedFieldContext {
    JsonString name;
    JsonString value;
    bool inline_field;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_embed_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmbedFieldContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("name"))) {
        if (!mark(&context->seen, 1ull << 0, &context->error) ||
            !json_value_as_string(value, &context->name)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("value"))) {
        if (!mark(&context->seen, 1ull << 1, &context->error) ||
            !json_value_as_string(value, &context->value)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("inline"))) {
        if (!mark(&context->seen, 1ull << 2, &context->error) ||
            !json_value_to_bool(value, &context->inline_field)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    }
    return true;
}

struct EmbedContext {
    JsonString title;
    JsonString description;
    JsonString url;
    JsonString type;
    JsonValue author;
    JsonValue footer;
    JsonValue fields;
    JsonValue image;
    JsonValue thumbnail;
    JsonValue video;
    JsonValue provider;
    JsonValue components;
    JsonString timestamp;
    u32 color;
    u32 flags;
    u64 seen;
    GatewayEventDecodeError error;
};

// author {name,url,icon_url,proxy_icon_url}, footer {text,icon_url,
// proxy_icon_url} and provider {name,url}; nulls are absent members.
struct EmbedPartContext {
    JsonString name;
    JsonString text;
    JsonString url;
    JsonString proxy_icon_url;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_embed_part(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmbedPartContext*>(opaque);
    u64 bit = 0;
    JsonString* target = nullptr;
    if (json_string_equals_ascii(name, utf8_literal("name"))) {
        bit = 1ull << 0;
        target = &context->name;
    } else if (json_string_equals_ascii(name, utf8_literal("text"))) {
        bit = 1ull << 1;
        target = &context->text;
    } else if (json_string_equals_ascii(name, utf8_literal("url"))) {
        bit = 1ull << 2;
        target = &context->url;
    } else if (json_string_equals_ascii(name, utf8_literal("proxy_icon_url"))) {
        bit = 1ull << 3;
        target = &context->proxy_icon_url;
    }
    if (!target || value.kind == JsonValueKind::null_value) {
        return true;
    }
    if (!mark(&context->seen, bit, &context->error) || !json_value_as_string(value, target)) {
        context->error = context->error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_data
                                                                         : context->error;
        return false;
    }
    return true;
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
        *destination = Utf8View{};
        *text_used = rollback;
    }
    return true;
}

bool visit_embed(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmbedContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("flags"))) {
        if (!mark(&context->seen, 1ull << 11, &context->error) ||
            (value.kind != JsonValueKind::null_value && !value_u32(value, &context->flags))) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("title"))) {
        if (!mark(&context->seen, 1ull << 0, &context->error) ||
            !json_value_as_string(value, &context->title)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("description"))) {
        if (!mark(&context->seen, 1ull << 1, &context->error) ||
            !json_value_as_string(value, &context->description)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("url"))) {
        if (!mark(&context->seen, 1ull << 2, &context->error) ||
            !json_value_as_string(value, &context->url)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("type"))) {
        if (!mark(&context->seen, 1ull << 3, &context->error) ||
            !json_value_as_string(value, &context->type)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("color"))) {
        if (!mark(&context->seen, 1ull << 4, &context->error) || !value_u32(value, &context->color) ||
            context->color > 0xFFFFFFu) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("author"))) {
        if (!mark(&context->seen, 1ull << 5, &context->error) || value.kind != JsonValueKind::object) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        context->author = value;
    } else if (json_string_equals_ascii(name, utf8_literal("footer"))) {
        if (!mark(&context->seen, 1ull << 6, &context->error) || value.kind != JsonValueKind::object) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        context->footer = value;
    } else if (json_string_equals_ascii(name, utf8_literal("fields"))) {
        if (!mark(&context->seen, 1ull << 7, &context->error) || value.kind != JsonValueKind::array) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        context->fields = value;
    } else if (json_string_equals_ascii(name, utf8_literal("image"))) {
        if (!mark(&context->seen, 1ull << 8, &context->error) || value.kind != JsonValueKind::object) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        context->image = value;
    } else if (json_string_equals_ascii(name, utf8_literal("thumbnail"))) {
        if (!mark(&context->seen, 1ull << 9, &context->error) || value.kind != JsonValueKind::object) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        context->thumbnail = value;
    } else if (json_string_equals_ascii(name, utf8_literal("video"))) {
        if (!mark(&context->seen, 1ull << 10, &context->error) || value.kind != JsonValueKind::object) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        context->video = value;
    } else if (json_string_equals_ascii(name, utf8_literal("provider"))) {
        if (value.kind == JsonValueKind::null_value) {
            return true;
        }
        if (!mark(&context->seen, 1ull << 12, &context->error) || value.kind != JsonValueKind::object) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        context->provider = value;
    } else if (json_string_equals_ascii(name, utf8_literal("components"))) {
        if (value.kind == JsonValueKind::array && !(context->seen & (1ull << 14))) {
            context->seen |= 1ull << 14;
            context->components = value;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("timestamp"))) {
        if (value.kind == JsonValueKind::null_value) {
            return true;
        }
        if (!mark(&context->seen, 1ull << 13, &context->error) ||
            !json_value_as_string(value, &context->timestamp)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    }
    return true;
}

struct EmbedMediaContext {
    JsonString url;
    JsonString proxy_url;
    JsonString content_type;
    JsonString description;
    u32 width;
    u32 height;
    u32 flags;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_embed_media(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmbedMediaContext*>(opaque);
    u64 bit = 0;
    JsonString* text = nullptr;
    if (json_string_equals_ascii(name, utf8_literal("url"))) {
        bit = 1ull << 0;
        text = &context->url;
    } else if (json_string_equals_ascii(name, utf8_literal("proxy_url"))) {
        bit = 1ull << 1;
        text = &context->proxy_url;
    } else if (json_string_equals_ascii(name, utf8_literal("content_type"))) {
        bit = 1ull << 2;
        text = &context->content_type;
    } else if (json_string_equals_ascii(name, utf8_literal("description"))) {
        bit = 1ull << 3;
        text = &context->description;
    } else if (json_string_equals_ascii(name, utf8_literal("width"))) {
        bit = 1ull << 4;
        if (!mark(&context->seen, bit, &context->error) ||
            (value.kind != JsonValueKind::null_value && !value_u32(value, &context->width))) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        return true;
    } else if (json_string_equals_ascii(name, utf8_literal("height"))) {
        bit = 1ull << 5;
        if (!mark(&context->seen, bit, &context->error) ||
            (value.kind != JsonValueKind::null_value && !value_u32(value, &context->height))) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        return true;
    } else if (json_string_equals_ascii(name, utf8_literal("flags"))) {
        bit = 1ull << 6;
        if (!mark(&context->seen, bit, &context->error) || !value_u32(value, &context->flags)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        return true;
    }
    if (text && (!mark(&context->seen, bit, &context->error) || !json_value_as_string(value, text))) {
        context->error = context->error == GatewayEventDecodeError::none
                             ? GatewayEventDecodeError::invalid_data
                             : context->error;
        return false;
    }
    return true;
}

GatewayEventDecodeResult decode_embed_media(JsonValue object, const JsonIndexedDocument& scratch,
                                            u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                            GatewayMessageEmbedMedia* destination,
                                            u32* truncations) noexcept {
    EmbedMediaContext context{};
    GatewayEventDecodeResult result =
        read_object(object, scratch, maximum_depth, visit_embed_media, &context, &context.error);
    if (!result.ok()) {
        return result;
    }
    *destination = GatewayMessageEmbedMedia{};
    destination->width = context.width;
    destination->height = context.height;
    destination->flags = context.flags;
    destination->has_width = context.width != 0;
    destination->has_height = context.height != 0;
    if (((context.seen & (1ull << 0)) != 0 &&
         !bounded_string(context.url, kRichUrlMaximumBytes, text_output, text_used, &destination->url,
                         truncations)) ||
        ((context.seen & (1ull << 1)) != 0 &&
         !bounded_string(context.proxy_url, kRichUrlMaximumBytes, text_output, text_used,
                         &destination->proxy_url, truncations)) ||
        ((context.seen & (1ull << 2)) != 0 &&
         !bounded_string(context.content_type, kRichContentTypeMaximumBytes, text_output, text_used,
                         &destination->content_type, truncations)) ||
        ((context.seen & (1ull << 3)) != 0 &&
         !bounded_string(context.description, kRichAttachmentDescriptionMaximumBytes, text_output,
                         text_used, &destination->description, truncations))) {
        return rich_failure(GatewayEventDecodeError::invalid_data, object.source_offset);
    }
    if ((destination->url.bytes != 0 && !rich_url_valid(destination->url, true)) ||
        (destination->proxy_url.bytes != 0 && !rich_url_valid(destination->proxy_url, false))) {
        return rich_failure(GatewayEventDecodeError::invalid_data, object.source_offset);
    }
    return rich_failure(GatewayEventDecodeError::none);
}

GatewayMessageEmbedType embed_type(JsonString value) noexcept {
    char decoded[16]{};
    const JsonStringDecodeResult result = json_string_decode_utf8(value, Slice<char>{decoded, 16});
    if (!result.ok()) {
        return GatewayMessageEmbedType::unknown;
    }
    const Utf8View text{decoded, result.bytes};
    if (utf8_equal(text, utf8_literal("rich"))) {
        return GatewayMessageEmbedType::rich;
    }
    if (utf8_equal(text, utf8_literal("image"))) {
        return GatewayMessageEmbedType::image;
    }
    if (utf8_equal(text, utf8_literal("video"))) {
        return GatewayMessageEmbedType::video;
    }
    if (utf8_equal(text, utf8_literal("gifv"))) {
        return GatewayMessageEmbedType::gifv;
    }
    if (utf8_equal(text, utf8_literal("article"))) {
        return GatewayMessageEmbedType::article;
    }
    if (utf8_equal(text, utf8_literal("link"))) {
        return GatewayMessageEmbedType::link;
    }
    if (utf8_equal(text, utf8_literal("poll_result"))) {
        return GatewayMessageEmbedType::poll_result;
    }
    if (utf8_equal(text, utf8_literal("components"))) {
        return GatewayMessageEmbedType::components;
    }
    return GatewayMessageEmbedType::unknown;
}

// A components embed carries one Components V2 tree: containers (17) with an
// accent colour, sections (9) with a thumbnail accessory (11), text displays
// (10), media galleries (12) and separators (14). The embed card shows its
// text as one markdown description.
inline constexpr u32 kEmbedComponentTextMaximum = 16;
struct EmbedComponentWalk {
    const JsonIndexedDocument* document;
    u32 depth;
    JsonString texts[kEmbedComponentTextMaximum];
    bool paragraph_before[kEmbedComponentTextMaximum];
    u32 text_count;
    JsonValue thumbnail;
    JsonValue image;
    u32 accent;
    u32 nesting;
    bool accent_seen;
    bool separator_pending;
};
struct EmbedComponentNode {
    JsonString content;
    JsonValue components, accessory, items, media;
    u32 type, accent;
    bool content_seen, components_seen, accessory_seen, items_seen, media_seen, accent_seen;
};
bool visit_embed_component_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& node = *static_cast<EmbedComponentNode*>(opaque);
    u64 number = 0;
    if (json_string_equals_ascii(name, utf8_literal("type"))) {
        node.type = json_value_to_u64(value, &number) && number < 256 ? static_cast<u32>(number) : 0;
    } else if (json_string_equals_ascii(name, utf8_literal("content"))) {
        node.content_seen = value.kind == JsonValueKind::string && json_value_as_string(value, &node.content);
    } else if (json_string_equals_ascii(name, utf8_literal("components"))) {
        node.components_seen = value.kind == JsonValueKind::array;
        node.components = value;
    } else if (json_string_equals_ascii(name, utf8_literal("accessory"))) {
        node.accessory_seen = value.kind == JsonValueKind::object;
        node.accessory = value;
    } else if (json_string_equals_ascii(name, utf8_literal("items"))) {
        node.items_seen = value.kind == JsonValueKind::array;
        node.items = value;
    } else if (json_string_equals_ascii(name, utf8_literal("media"))) {
        node.media_seen = value.kind == JsonValueKind::object;
        node.media = value;
    } else if (json_string_equals_ascii(name, utf8_literal("accent_color"))) {
        node.accent_seen = json_value_to_u64(value, &number) && number <= 0xFFFFFFu;
        node.accent = static_cast<u32>(number);
    }
    return true;
}
bool visit_embed_gallery_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto& walk = *static_cast<EmbedComponentWalk*>(opaque);
    if (index != 0 || value.kind != JsonValueKind::object || walk.image.source.data)
        return true;
    EmbedComponentNode node{};
    if (json_visit_indexed_object(*walk.document, value, walk.depth, visit_embed_component_member, &node).ok() &&
        node.media_seen)
        walk.image = node.media;
    return true;
}
bool visit_embed_component_item(void* opaque, u32, JsonValue value) noexcept {
    auto& walk = *static_cast<EmbedComponentWalk*>(opaque);
    if (value.kind != JsonValueKind::object || walk.nesting > 3)
        return true;
    EmbedComponentNode node{};
    if (!json_visit_indexed_object(*walk.document, value, walk.depth, visit_embed_component_member, &node).ok())
        return true;
    if (node.type == 17 && node.accent_seen && !walk.accent_seen) {
        walk.accent = node.accent;
        walk.accent_seen = true;
    }
    if (node.type == 10 && node.content_seen && walk.text_count < kEmbedComponentTextMaximum) {
        walk.paragraph_before[walk.text_count] = walk.separator_pending;
        walk.texts[walk.text_count++] = node.content;
        walk.separator_pending = false;
    } else if (node.type == 14) {
        walk.separator_pending = walk.text_count != 0;
    } else if (node.type == 12 && node.items_seen) {
        json_visit_indexed_array(*walk.document, node.items, walk.depth, visit_embed_gallery_item, &walk);
    }
    if ((node.type == 17 || node.type == 9) && node.components_seen) {
        ++walk.nesting;
        json_visit_indexed_array(*walk.document, node.components, walk.depth, visit_embed_component_item, &walk);
        --walk.nesting;
    }
    if (node.type == 9 && node.accessory_seen && !walk.thumbnail.source.data) {
        EmbedComponentNode accessory{};
        if (json_visit_indexed_object(*walk.document, node.accessory, walk.depth, visit_embed_component_member,
                                      &accessory)
                .ok() &&
            accessory.type == 11 && accessory.media_seen)
            walk.thumbnail = accessory.media;
    }
    return true;
}
// Text displays joined by line breaks (a blank line across a separator) as
// one bounded description in the decoded text bank.
bool embed_component_description(const EmbedComponentWalk& walk, Slice<char> text_output, u32* text_used,
                                 Utf8View* description, u32* truncations) noexcept {
    *description = {};
    const u32 begin = *text_used;
    for (u32 i = 0; i < walk.text_count; ++i) {
        if (i) {
            const u32 breaks = walk.paragraph_before[i] ? 2u : 1u;
            if (!text_output.data || *text_used + breaks > text_output.count ||
                *text_used - begin + breaks > kRichEmbedDescriptionMaximumBytes) {
                *truncations |= gateway_message_rich_truncated_text;
                break;
            }
            for (u32 b = 0; b < breaks; ++b)
                text_output.data[(*text_used)++] = '\n';
        }
        Utf8View piece{};
        if (!bounded_string(walk.texts[i], kRichEmbedDescriptionMaximumBytes - (*text_used - begin), text_output,
                            text_used, &piece, truncations))
            return false;
    }
    if (*text_used > begin && text_output.data)
        *description = {text_output.data + begin, *text_used - begin};
    return true;
}

GatewayEventDecodeResult decode_embeds(JsonValue array, const JsonIndexedDocument& scratch,
                                       u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                       GatewayMessageRichScratch output_scratch,
                                       GatewayMessageEvent* output) noexcept {
    JsonValue values[kGatewayMessageEmbedMaximum]{};
    const u32 capacity = minimum<u32>(output_scratch.embeds.count, kGatewayMessageEmbedMaximum);
    u32 count = 0;
    GatewayEventDecodeResult result =
        gather_array(array, scratch, maximum_depth, values, capacity, gateway_message_rich_truncated_embeds,
                     &output->rich_truncations, &count);
    if (!result.ok()) {
        return result;
    }
    u32 field_count = 0;
    for (u32 index = 0; index < count; ++index) {
        EmbedContext context{};
        result = read_object(values[index], scratch, maximum_depth, visit_embed, &context, &context.error);
        if (!result.ok()) {
            return result;
        }
        GatewayMessageEmbed& destination = output_scratch.embeds[index];
        destination = GatewayMessageEmbed{};
        destination.field_begin = static_cast<u16>(field_count);
        destination.color = context.color;
        destination.has_color = (context.seen & (1ull << 4)) != 0;
        destination.has_title = (context.seen & (1ull << 0)) != 0;
        destination.has_author = (context.seen & (1ull << 5)) != 0;
        destination.flags = static_cast<u8>(context.flags & 0xFFu);
        destination.type =
            (context.seen & (1ull << 3)) != 0 ? embed_type(context.type) : GatewayMessageEmbedType::unknown;
        if (((context.seen & (1ull << 0)) != 0 &&
             !bounded_string(context.title, kRichEmbedTitleMaximumBytes, text_output, text_used,
                             &destination.title, &output->rich_truncations)) ||
            ((context.seen & (1ull << 1)) != 0 &&
             !bounded_string(context.description, kRichEmbedDescriptionMaximumBytes, text_output, text_used,
                             &destination.description, &output->rich_truncations)) ||
            ((context.seen & (1ull << 2)) != 0 &&
             !bounded_string(context.url, kRichUrlMaximumBytes, text_output, text_used, &destination.url,
                             &output->rich_truncations))) {
            return rich_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
        if (destination.url.bytes != 0 && !rich_url_valid(destination.url, true)) {
            return rich_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
        if ((context.seen & (1ull << 5)) != 0) {
            EmbedPartContext author{};
            result = read_object(context.author, scratch, maximum_depth, visit_embed_part, &author,
                                 &author.error);
            if (!result.ok()) {
                return result;
            }
            if ((author.seen & 1ull) == 0 ||
                !bounded_string(author.name, kRichEmbedTitleMaximumBytes, text_output, text_used,
                                &destination.author_name, &output->rich_truncations) ||
                !embed_part_url(author, 1ull << 2, author.url, text_output, text_used, &destination.author_url,
                                &output->rich_truncations) ||
                !embed_part_url(author, 1ull << 3, author.proxy_icon_url, text_output, text_used,
                                &destination.author_icon, &output->rich_truncations)) {
                return rich_failure(GatewayEventDecodeError::invalid_data, context.author.source_offset);
            }
        }
        if ((context.seen & (1ull << 6)) != 0) {
            EmbedPartContext footer{};
            result = read_object(context.footer, scratch, maximum_depth, visit_embed_part, &footer,
                                 &footer.error);
            if (!result.ok()) {
                return result;
            }
            if ((footer.seen & (1ull << 1)) == 0 ||
                !bounded_string(footer.text, kRichEmbedFooterMaximumBytes, text_output, text_used,
                                &destination.footer_text, &output->rich_truncations) ||
                !embed_part_url(footer, 1ull << 3, footer.proxy_icon_url, text_output, text_used,
                                &destination.footer_icon, &output->rich_truncations)) {
                return rich_failure(GatewayEventDecodeError::invalid_data, context.footer.source_offset);
            }
        }
        if ((context.seen & (1ull << 12)) != 0) {
            EmbedPartContext provider{};
            result = read_object(context.provider, scratch, maximum_depth, visit_embed_part, &provider,
                                 &provider.error);
            if (!result.ok()) {
                return result;
            }
            if (((provider.seen & 1ull) != 0 &&
                 !bounded_string(provider.name, kRichEmbedTitleMaximumBytes, text_output, text_used,
                                 &destination.provider_name, &output->rich_truncations)) ||
                !embed_part_url(provider, 1ull << 2, provider.url, text_output, text_used,
                                &destination.provider_url, &output->rich_truncations)) {
                return rich_failure(GatewayEventDecodeError::invalid_data, context.provider.source_offset);
            }
        }
        if ((context.seen & (1ull << 13)) != 0 &&
            !parse_iso_timestamp(context.timestamp, &destination.timestamp_ms)) {
            destination.timestamp_ms = 0;
        }
        if ((context.seen & (1ull << 7)) != 0) {
            JsonValue field_values[25]{};
            const u32 remaining = output_scratch.embed_fields.count > field_count
                                      ? output_scratch.embed_fields.count - field_count
                                      : 0;
            const u32 field_capacity = minimum<u32>(25, remaining);
            u32 captured = 0;
            result = gather_array(context.fields, scratch, maximum_depth, field_values, field_capacity,
                                  gateway_message_rich_truncated_embed_fields, &output->rich_truncations,
                                  &captured);
            if (!result.ok()) {
                return result;
            }
            for (u32 field = 0; field < captured; ++field) {
                EmbedFieldContext parsed{};
                result = read_object(field_values[field], scratch, maximum_depth, visit_embed_field,
                                     &parsed, &parsed.error);
                if (!result.ok()) {
                    return result;
                }
                if ((parsed.seen & 3ull) != 3ull) {
                    return rich_failure(GatewayEventDecodeError::missing_required_field,
                                        field_values[field].source_offset);
                }
                GatewayMessageEmbedField& field_output = output_scratch.embed_fields[field_count];
                field_output = GatewayMessageEmbedField{};
                field_output.embed_index = static_cast<u16>(index);
                field_output.inline_field = parsed.inline_field;
                if (!bounded_string(parsed.name, kRichEmbedTitleMaximumBytes, text_output, text_used,
                                    &field_output.name, &output->rich_truncations) ||
                    !bounded_string(parsed.value, kRichComponentTextMaximumBytes, text_output, text_used,
                                    &field_output.value, &output->rich_truncations)) {
                    return rich_failure(GatewayEventDecodeError::invalid_data,
                                        field_values[field].source_offset);
                }
                ++field_count;
                ++destination.field_count;
            }
        }
        if ((context.seen & (1ull << 8)) != 0) {
            result = decode_embed_media(context.image, scratch, maximum_depth, text_output, text_used,
                                        &destination.image, &output->rich_truncations);
            if (!result.ok()) {
                return result;
            }
        }
        if ((context.seen & (1ull << 9)) != 0) {
            result = decode_embed_media(context.thumbnail, scratch, maximum_depth, text_output, text_used,
                                        &destination.thumbnail, &output->rich_truncations);
            if (!result.ok()) {
                return result;
            }
        }
        if ((context.seen & (1ull << 10)) != 0) {
            result = decode_embed_media(context.video, scratch, maximum_depth, text_output, text_used,
                                        &destination.video, &output->rich_truncations);
            if (!result.ok()) {
                return result;
            }
        }
        if ((context.seen & (1ull << 14)) != 0) {
            EmbedComponentWalk walk{};
            walk.document = &scratch;
            walk.depth = maximum_depth;
            json_visit_indexed_array(scratch, context.components, maximum_depth, visit_embed_component_item,
                                     &walk);
            if (!destination.description.bytes &&
                !embed_component_description(walk, text_output, text_used, &destination.description,
                                             &output->rich_truncations))
                return rich_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
            if (!destination.has_color && walk.accent_seen) {
                destination.color = walk.accent;
                destination.has_color = true;
            }
            // Malformed optional media degrades to absent, never rejects the message.
            if (walk.thumbnail.source.data && !destination.thumbnail.url.bytes &&
                !decode_embed_media(walk.thumbnail, scratch, maximum_depth, text_output, text_used,
                                    &destination.thumbnail, &output->rich_truncations)
                     .ok())
                destination.thumbnail = GatewayMessageEmbedMedia{};
            if (walk.image.source.data && !destination.image.url.bytes &&
                !decode_embed_media(walk.image, scratch, maximum_depth, text_output, text_used,
                                    &destination.image, &output->rich_truncations)
                     .ok())
                destination.image = GatewayMessageEmbedMedia{};
        }
    }
    output->embeds = ConstSlice<GatewayMessageEmbed>{output_scratch.embeds.data, count};
    output->embed_fields =
        ConstSlice<GatewayMessageEmbedField>{output_scratch.embed_fields.data, field_count};
    return rich_failure(GatewayEventDecodeError::none);
}

struct StickerContext {
    u64 id;
    JsonString name;
    u32 format_type;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_sticker(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<StickerContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!mark(&context->seen, 1ull << 0, &context->error) ||
            !json_value_to_snowflake(value, &context->id)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("name"))) {
        if (!mark(&context->seen, 1ull << 1, &context->error) ||
            !json_value_as_string(value, &context->name)) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("format_type"))) {
        if (!mark(&context->seen, 1ull << 2, &context->error) || !value_u32(value, &context->format_type) ||
            context->format_type == 0 || context->format_type > 4) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
    }
    return true;
}

GatewayEventDecodeResult decode_stickers(JsonValue array, const JsonIndexedDocument& scratch,
                                         u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                         GatewayMessageRichScratch output_scratch,
                                         GatewayMessageEvent* output) noexcept {
    JsonValue values[kGatewayMessageStickerMaximum]{};
    const u32 capacity = minimum<u32>(output_scratch.stickers.count, kGatewayMessageStickerMaximum);
    u32 count = 0;
    GatewayEventDecodeResult result =
        gather_array(array, scratch, maximum_depth, values, capacity,
                     gateway_message_rich_truncated_stickers, &output->rich_truncations, &count);
    if (!result.ok()) {
        return result;
    }
    for (u32 index = 0; index < count; ++index) {
        StickerContext parsed{};
        result = read_object(values[index], scratch, maximum_depth, visit_sticker, &parsed, &parsed.error);
        if (!result.ok()) {
            return result;
        }
        if ((parsed.seen & 7ull) != 7ull) {
            return rich_failure(GatewayEventDecodeError::missing_required_field,
                                values[index].source_offset);
        }
        GatewayMessageSticker& destination = output_scratch.stickers[index];
        destination = GatewayMessageSticker{};
        destination.id = parsed.id;
        destination.format_type = static_cast<u8>(parsed.format_type);
        if (!bounded_string(parsed.name, kGatewayMessageUserNameMaximumBytes, text_output, text_used,
                            &destination.name, &output->rich_truncations)) {
            return rich_failure(GatewayEventDecodeError::invalid_data, values[index].source_offset);
        }
    }
    output->stickers = ConstSlice<GatewayMessageSticker>{output_scratch.stickers.data, count};
    return rich_failure(GatewayEventDecodeError::none);
}

struct UrlObjectContext {
    JsonString url;
    JsonString proxy_url;
    u64 seen;
    GatewayEventDecodeError error;
};

// Unfurled media items carry both the source URL and the media proxy URL;
// the proxy is fetchable through the CDN pipeline, so it is preferred.
bool visit_url_object(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<UrlObjectContext*>(opaque);
    const bool url = json_string_equals_ascii(name, utf8_literal("url"));
    const bool proxy = !url && json_string_equals_ascii(name, utf8_literal("proxy_url"));
    if (url || proxy) {
        const u64 bit = url ? 1ull : 2ull;
        if (!mark(&context->seen, bit, &context->error) ||
            (value.kind != JsonValueKind::null_value &&
             !json_value_as_string(value, url ? &context->url : &context->proxy_url))) {
            context->error = context->error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : context->error;
            return false;
        }
        if (value.kind == JsonValueKind::null_value)
            context->seen &= ~bit;
    }
    return true;
}

struct ComponentContext {
    JsonString label;
    JsonString content;
    JsonString custom_id;
    JsonString url;
    JsonString placeholder;
    JsonString description;
    JsonString value;
    JsonString name;
    JsonValue children;
    JsonValue accessory;
    JsonValue media;
    JsonValue file;
    JsonValue emoji;
    JsonValue options;
    JsonValue items;
    u32 id;
    u32 type;
    u32 style;
    u32 accent_color;
    u32 min_values;
    u32 max_values;
    u32 spacing;
    bool disabled;
    bool has_accent_color;
    bool spoiler;
    bool divider;
    bool selected_default;
    u64 seen;
    GatewayEventDecodeError error;
};

enum ComponentSeen : u64 {
    component_seen_type = 1ull << 0,
    component_seen_id = 1ull << 1,
    component_seen_style = 1ull << 2,
    component_seen_disabled = 1ull << 3,
    component_seen_label = 1ull << 4,
    component_seen_content = 1ull << 5,
    component_seen_custom_id = 1ull << 6,
    component_seen_url = 1ull << 7,
    component_seen_children = 1ull << 8,
    component_seen_accessory = 1ull << 9,
    component_seen_media = 1ull << 10,
    component_seen_file = 1ull << 11,
    component_seen_accent = 1ull << 12,
    component_seen_emoji = 1ull << 13,
    component_seen_placeholder = 1ull << 14,
    component_seen_description = 1ull << 15,
    component_seen_value = 1ull << 16,
    component_seen_name = 1ull << 17,
    component_seen_options = 1ull << 18,
    component_seen_items = 1ull << 19,
    component_seen_min = 1ull << 20,
    component_seen_max = 1ull << 21,
    component_seen_spacing = 1ull << 22,
    component_seen_spoiler = 1ull << 23,
    component_seen_divider = 1ull << 24,
    component_seen_default = 1ull << 25,
};

bool component_invalid(ComponentContext* context) noexcept {
    if (context->error == GatewayEventDecodeError::none)
        context->error = GatewayEventDecodeError::invalid_data;
    return false;
}

// Optional members may be null; null is then treated as absent.
bool component_text(ComponentContext* context, u64 bit, JsonValue value, JsonString* output) noexcept {
    if (!mark(&context->seen, bit, &context->error))
        return component_invalid(context);
    if (value.kind == JsonValueKind::null_value) {
        context->seen &= ~bit;
        return true;
    }
    return json_value_as_string(value, output) || component_invalid(context);
}

bool component_bool(ComponentContext* context, u64 bit, JsonValue value, bool* output) noexcept {
    return (mark(&context->seen, bit, &context->error) &&
            (value.kind == JsonValueKind::null_value || json_value_to_bool(value, output))) ||
           component_invalid(context);
}

bool component_number(ComponentContext* context, u64 bit, JsonValue value, u32* output,
                      u32 maximum_value) noexcept {
    return (mark(&context->seen, bit, &context->error) &&
            (value.kind == JsonValueKind::null_value ||
             (value_u32(value, output) && *output <= maximum_value))) ||
           component_invalid(context);
}

bool component_value(ComponentContext* context, u64 bit, JsonValue value, JsonValueKind kind,
                     JsonValue* output) noexcept {
    if (!mark(&context->seen, bit, &context->error))
        return component_invalid(context);
    if (value.kind == JsonValueKind::null_value) {
        context->seen &= ~bit;
        return true;
    }
    if (value.kind != kind)
        return component_invalid(context);
    *output = value;
    return true;
}

template <usize Count> bool component_key(JsonString name, const char (&key)[Count]) noexcept {
    return json_string_equals_ascii(name, utf8_literal(key));
}

bool visit_component(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ComponentContext*>(opaque);
    if (component_key(name, "accent_color")) {
        if (!mark(&context->seen, component_seen_accent, &context->error))
            return component_invalid(context);
        context->has_accent_color = value.kind != JsonValueKind::null_value;
        if (context->has_accent_color &&
            (!value_u32(value, &context->accent_color) || context->accent_color > 0xFFFFFFu))
            return component_invalid(context);
    } else if (component_key(name, "type")) {
        if (!mark(&context->seen, component_seen_type, &context->error) || !value_u32(value, &context->type) ||
            context->type == 0 || context->type >= gateway_component_select_option)
            return component_invalid(context);
    } else if (component_key(name, "id")) {
        if (!mark(&context->seen, component_seen_id, &context->error) || !value_u32(value, &context->id))
            return component_invalid(context);
    } else if (component_key(name, "style")) {
        return component_number(context, component_seen_style, value, &context->style, 0xFFFFu);
    } else if (component_key(name, "disabled")) {
        return component_bool(context, component_seen_disabled, value, &context->disabled);
    } else if (component_key(name, "label")) {
        return component_text(context, component_seen_label, value, &context->label);
    } else if (component_key(name, "content")) {
        return component_text(context, component_seen_content, value, &context->content);
    } else if (component_key(name, "custom_id")) {
        return component_text(context, component_seen_custom_id, value, &context->custom_id);
    } else if (component_key(name, "url")) {
        return component_text(context, component_seen_url, value, &context->url);
    } else if (component_key(name, "placeholder")) {
        return component_text(context, component_seen_placeholder, value, &context->placeholder);
    } else if (component_key(name, "description")) {
        return component_text(context, component_seen_description, value, &context->description);
    } else if (component_key(name, "value")) {
        return component_text(context, component_seen_value, value, &context->value);
    } else if (component_key(name, "name")) {
        return component_text(context, component_seen_name, value, &context->name);
    } else if (component_key(name, "components")) {
        return component_value(context, component_seen_children, value, JsonValueKind::array,
                               &context->children);
    } else if (component_key(name, "accessory")) {
        return component_value(context, component_seen_accessory, value, JsonValueKind::object,
                               &context->accessory);
    } else if (component_key(name, "media")) {
        return component_value(context, component_seen_media, value, JsonValueKind::object, &context->media);
    } else if (component_key(name, "file")) {
        return component_value(context, component_seen_file, value, JsonValueKind::object, &context->file);
    } else if (component_key(name, "emoji")) {
        return component_value(context, component_seen_emoji, value, JsonValueKind::object, &context->emoji);
    } else if (component_key(name, "options")) {
        return component_value(context, component_seen_options, value, JsonValueKind::array,
                               &context->options);
    } else if (component_key(name, "items")) {
        return component_value(context, component_seen_items, value, JsonValueKind::array, &context->items);
    } else if (component_key(name, "min_values")) {
        return component_number(context, component_seen_min, value, &context->min_values, 25);
    } else if (component_key(name, "max_values")) {
        return component_number(context, component_seen_max, value, &context->max_values, 25);
    } else if (component_key(name, "spacing")) {
        return component_number(context, component_seen_spacing, value, &context->spacing, 0xFFFFu);
    } else if (component_key(name, "spoiler")) {
        return component_bool(context, component_seen_spoiler, value, &context->spoiler);
    } else if (component_key(name, "divider")) {
        return component_bool(context, component_seen_divider, value, &context->divider);
    } else if (component_key(name, "default")) {
        return component_bool(context, component_seen_default, value, &context->selected_default);
    }
    return true;
}

struct ComponentEmojiContext {
    u64 id;
    JsonString name;
    bool animated;
    u64 seen;
    GatewayEventDecodeError error;
};

bool visit_component_emoji(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ComponentEmojiContext*>(opaque);
    const bool null_value = value.kind == JsonValueKind::null_value;
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        JsonString text{};
        if (!mark(&context->seen, 1ull << 0, &context->error) ||
            (!null_value && (!json_value_as_string(value, &text) || !json_string_to_u64(text, &context->id)))) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("name"))) {
        if (!mark(&context->seen, 1ull << 1, &context->error) ||
            (!null_value && !json_value_as_string(value, &context->name))) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
        if (null_value)
            context->seen &= ~(1ull << 1);
    } else if (json_string_equals_ascii(name, utf8_literal("animated"))) {
        if (!mark(&context->seen, 1ull << 2, &context->error) ||
            (!null_value && !json_value_to_bool(value, &context->animated))) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    }
    return true;
}

struct ComponentWork {
    JsonValue value;
    u16 parent;
    u16 order;
    u16 pseudo_type; // 0 for wire components; otherwise an option or gallery item
};

bool queue_component_children(JsonValue array, const JsonIndexedDocument& scratch, u32 maximum_depth,
                              u32 capacity, u16 parent, u16 pseudo_type, ComponentWork* work, u32* work_count,
                              GatewayMessageEvent* output, GatewayEventDecodeResult* result) noexcept {
    JsonValue children[kGatewayMessageComponentMaximum]{};
    const u32 remaining = capacity > *work_count ? capacity - *work_count : 0;
    u32 child_count = 0;
    *result = gather_array(array, scratch, maximum_depth, children, remaining,
                           gateway_message_rich_truncated_components, &output->rich_truncations, &child_count);
    if (!result->ok())
        return false;
    for (u32 child = 0; child < child_count; ++child)
        work[(*work_count)++] = ComponentWork{children[child], parent, static_cast<u16>(child), pseudo_type};
    return true;
}

GatewayEventDecodeResult decode_components(JsonValue array, const JsonIndexedDocument& scratch,
                                           u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                           GatewayMessageRichScratch output_scratch,
                                           GatewayMessageEvent* output) noexcept {
    ComponentWork work[kGatewayMessageComponentMaximum]{};
    JsonValue root_values[kGatewayMessageComponentMaximum]{};
    const u32 capacity = minimum<u32>(output_scratch.components.count, kGatewayMessageComponentMaximum);
    u32 root_count = 0;
    GatewayEventDecodeResult result =
        gather_array(array, scratch, maximum_depth, root_values, capacity,
                     gateway_message_rich_truncated_components, &output->rich_truncations, &root_count);
    if (!result.ok()) {
        return result;
    }
    u32 work_count = root_count;
    for (u32 index = 0; index < root_count; ++index) {
        work[index] = ComponentWork{root_values[index], 0xFFFFu, static_cast<u16>(index), 0};
    }
    u32 output_count = 0;
    for (u32 cursor = 0; cursor < work_count && output_count < capacity; ++cursor) {
        ComponentContext parsed{};
        result = read_object(work[cursor].value, scratch, maximum_depth, visit_component, &parsed,
                             &parsed.error);
        if (!result.ok()) {
            return result;
        }
        const u16 pseudo = work[cursor].pseudo_type;
        if (pseudo) {
            if ((parsed.seen & component_seen_type) != 0)
                return rich_failure(GatewayEventDecodeError::invalid_data, work[cursor].value.source_offset);
            parsed.type = pseudo;
        } else if ((parsed.seen & component_seen_type) == 0) {
            return rich_failure(GatewayEventDecodeError::missing_required_field,
                                work[cursor].value.source_offset);
        }
        const u16 destination_index = static_cast<u16>(output_count);
        GatewayMessageComponent& destination = output_scratch.components[output_count++];
        destination = GatewayMessageComponent{};
        destination.id = parsed.id;
        destination.parent = work[cursor].parent;
        destination.order = work[cursor].order;
        destination.type = static_cast<u16>(parsed.type);
        // Separator spacing: 1 small (default), 2 large.
        destination.style = static_cast<u16>(
            parsed.type == gateway_component_separator
                ? ((parsed.seen & component_seen_spacing) && parsed.spacing ? parsed.spacing : 1u)
                : parsed.style);
        destination.accent_color = parsed.accent_color;
        destination.has_accent_color = parsed.has_accent_color;
        destination.disabled = parsed.disabled;
        const bool select = gateway_component_is_select(destination.type);
        if (select) {
            destination.min_values =
                static_cast<u8>((parsed.seen & component_seen_min) ? parsed.min_values : 1u);
            destination.max_values =
                static_cast<u8>((parsed.seen & component_seen_max) ? parsed.max_values : 1u);
            if (destination.max_values == 0 || destination.min_values > destination.max_values)
                return rich_failure(GatewayEventDecodeError::invalid_data, work[cursor].value.source_offset);
        }
        // A separator draws its divider unless the payload disables it.
        const bool divider = destination.type == gateway_component_separator &&
                             !((parsed.seen & component_seen_divider) && !parsed.divider);
        destination.flags = static_cast<u8>((parsed.spoiler ? gateway_message_component_spoiler : 0u) |
                                            (divider ? gateway_message_component_divider : 0u) |
                                            (parsed.selected_default ? gateway_message_component_default : 0u));
        // Type-specific field reuse (see GatewayMessageComponent).
        JsonString label = parsed.label, content = parsed.content, custom_id = parsed.custom_id;
        bool label_seen = (parsed.seen & component_seen_label) != 0;
        bool content_seen = (parsed.seen & component_seen_content) != 0;
        bool custom_seen = (parsed.seen & component_seen_custom_id) != 0;
        if (select) {
            content = parsed.placeholder;
            content_seen = (parsed.seen & component_seen_placeholder) != 0;
        } else if (destination.type == gateway_component_select_option) {
            content = parsed.description;
            content_seen = (parsed.seen & component_seen_description) != 0;
            custom_id = parsed.value;
            custom_seen = (parsed.seen & component_seen_value) != 0;
        } else if (destination.type == gateway_component_thumbnail ||
                   destination.type == gateway_component_gallery_item) {
            content = parsed.description;
            content_seen = (parsed.seen & component_seen_description) != 0;
        } else if (destination.type == gateway_component_file && !label_seen) {
            label = parsed.name;
            label_seen = (parsed.seen & component_seen_name) != 0;
        }
        if ((label_seen && !bounded_string(label, kRichAttachmentNameMaximumBytes, text_output, text_used,
                                           &destination.label, &output->rich_truncations)) ||
            (content_seen && !bounded_string(content, kRichComponentTextMaximumBytes, text_output, text_used,
                                             &destination.content, &output->rich_truncations)) ||
            (custom_seen && !bounded_string(custom_id, kRichComponentIdMaximumBytes, text_output, text_used,
                                            &destination.custom_id, &output->rich_truncations)) ||
            ((parsed.seen & component_seen_url) != 0 &&
             !bounded_string(parsed.url, kRichUrlMaximumBytes, text_output, text_used, &destination.url,
                             &output->rich_truncations))) {
            return rich_failure(GatewayEventDecodeError::invalid_data, work[cursor].value.source_offset);
        }
        if (destination.url.bytes != 0 && !rich_url_valid(destination.url, true)) {
            return rich_failure(GatewayEventDecodeError::invalid_data, work[cursor].value.source_offset);
        }
        if (parsed.seen & component_seen_emoji) {
            ComponentEmojiContext emoji{};
            result = read_object(parsed.emoji, scratch, maximum_depth, visit_component_emoji, &emoji,
                                 &emoji.error);
            if (!result.ok())
                return result;
            destination.emoji_id = emoji.id;
            if (emoji.animated)
                destination.flags |= gateway_message_component_emoji_animated;
            if ((emoji.seen & 2u) && !bounded_string(emoji.name, kRichAttachmentNameMaximumBytes, text_output,
                                                     text_used, &destination.emoji_name,
                                                     &output->rich_truncations))
                return rich_failure(GatewayEventDecodeError::invalid_data, parsed.emoji.source_offset);
        }
        const JsonValue indirect_url = (parsed.seen & component_seen_media) != 0 ? parsed.media : parsed.file;
        if (destination.url.bytes == 0 && indirect_url.source.data) {
            UrlObjectContext url{};
            result = read_object(indirect_url, scratch, maximum_depth, visit_url_object, &url, &url.error);
            if (!result.ok()) {
                return result;
            }
            const bool proxy = (url.seen & 2ull) != 0;
            if ((url.seen & 3ull) != 0 &&
                !bounded_string(proxy ? url.proxy_url : url.url, kRichUrlMaximumBytes, text_output, text_used,
                                &destination.url, &output->rich_truncations)) {
                return rich_failure(GatewayEventDecodeError::invalid_data, indirect_url.source_offset);
            }
            // attachment:// names resolve against the message's files.
            if (destination.url.bytes != 0 && !rich_url_valid(destination.url, true))
                return rich_failure(GatewayEventDecodeError::invalid_data, indirect_url.source_offset);
        }
        if ((parsed.seen & component_seen_children) != 0 &&
            !queue_component_children(parsed.children, scratch, maximum_depth, capacity, destination_index, 0,
                                      work, &work_count, output, &result))
            return result;
        if ((parsed.seen & component_seen_options) != 0 &&
            destination.type == gateway_component_string_select &&
            !queue_component_children(parsed.options, scratch, maximum_depth, capacity, destination_index,
                                      gateway_component_select_option, work, &work_count, output, &result))
            return result;
        if ((parsed.seen & component_seen_items) != 0 && destination.type == gateway_component_media_gallery &&
            !queue_component_children(parsed.items, scratch, maximum_depth, capacity, destination_index,
                                      gateway_component_gallery_item, work, &work_count, output, &result))
            return result;
        if ((parsed.seen & component_seen_accessory) != 0) {
            if (work_count < capacity) {
                work[work_count++] = ComponentWork{parsed.accessory, destination_index, 0xFFFFu, 0};
            } else {
                output->rich_truncations |= gateway_message_rich_truncated_components;
            }
        }
    }
    output->components = ConstSlice<GatewayMessageComponent>{output_scratch.components.data, output_count};
    return rich_failure(GatewayEventDecodeError::none);
}

struct SnapshotWrapperContext {
    JsonValue message;
    GatewayEventDecodeError error;
    bool seen;
};
bool visit_snapshot_wrapper(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<SnapshotWrapperContext*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("message")))
        return true;
    if (context->seen || value.kind != JsonValueKind::object) {
        context->error = context->seen ? GatewayEventDecodeError::duplicate_field
                                       : GatewayEventDecodeError::invalid_data;
        return false;
    }
    context->seen = true;
    context->message = value;
    return true;
}
struct SnapshotArrayContext {
    JsonValue value;
    GatewayEventDecodeError error;
    bool present;
};
bool visit_snapshot_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<SnapshotArrayContext*>(opaque);
    if (index != 0 || value.kind != JsonValueKind::object) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    context->present = true;
    context->value = value;
    return true;
}
GatewayEventDecodeResult extract_snapshot(JsonValue value, const JsonIndexedDocument& document,
                                          u32 maximum_depth, JsonValue* body, bool* present) noexcept {
    *body = {};
    *present = false;
    if (value.kind == JsonValueKind::null_value)
        return rich_failure(GatewayEventDecodeError::none);
    SnapshotArrayContext array{};
    const auto read = json_visit_indexed_array(document, value, maximum_depth, visit_snapshot_item, &array);
    if (!read.ok())
        return rich_failure(array.error == GatewayEventDecodeError::none
                                ? GatewayEventDecodeError::invalid_json
                                : array.error,
                            read.error_offset);
    if (!array.present)
        return rich_failure(GatewayEventDecodeError::none);
    SnapshotWrapperContext wrapper{};
    auto result =
        read_object(array.value, document, maximum_depth, visit_snapshot_wrapper, &wrapper, &wrapper.error);
    if (!result.ok())
        return result;
    if (!wrapper.seen)
        return rich_failure(GatewayEventDecodeError::missing_required_field);
    RootContext child{};
    result = read_object(wrapper.message, document, maximum_depth, visit_root, &child, &child.error);
    if (!result.ok())
        return result;
    if (child.seen & (gateway_message_field_message_snapshots | gateway_message_field_message_reference |
                      gateway_message_field_referenced_message | gateway_message_field_reactions |
                      gateway_message_field_interaction))
        return rich_failure(GatewayEventDecodeError::invalid_data);
    *body = wrapper.message;
    *present = true;
    return rich_failure(GatewayEventDecodeError::none);
}
GatewayEventDecodeResult decode_snapshot(JsonValue value, const JsonIndexedDocument& document,
                                         Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> text,
                                         u32* used, const GatewayMessageSnapshotScratch& scratch,
                                         GatewayMessageSnapshot* output) noexcept {
    *output = {};
    JsonValue body_value{};
    bool present = false;
    auto result = extract_snapshot(value, document, maximum_depth, &body_value, &present);
    if (!result.ok() || !present)
        return result;
    GatewayMessageEvent body{};
    result = gateway_decode_message_snapshot_core(
        body_value, structural_scratch, maximum_depth,
        {text.data ? text.data + *used : nullptr, text.count - *used}, scratch.mention_users, &body);
    if (!result.ok())
        return result;
    *used += body.transient_bytes;
    // Core decoding reuses the same structural scratch. Index the child again
    // before rich parsing; the parent's descriptors have already been decoded.
    const auto scan = json_scan_structurals_avx1(body_value.source, structural_scratch);
    if (!scan.ok())
        return rich_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    const JsonIndexedDocument child_document{body_value.source, {structural_scratch.data, scan.count}};
    RootContext child{};
    const auto child_read = json_visit_value_object(body_value, child_document.structurals,
                                                   maximum_depth, visit_root, &child);
    if (!child_read.ok())
        return rich_failure(child.error == GatewayEventDecodeError::none
                                ? GatewayEventDecodeError::invalid_json
                                : child.error,
                            child_read.error_offset);
    if (child.seen & (gateway_message_field_message_snapshots | gateway_message_field_message_reference |
                      gateway_message_field_referenced_message | gateway_message_field_reactions |
                      gateway_message_field_interaction))
        return rich_failure(GatewayEventDecodeError::invalid_data);
    const GatewayMessageRichScratch media{
        {}, scratch.attachments, scratch.embeds, scratch.embed_fields, scratch.stickers, scratch.components,
        {}};
    if (child.seen & gateway_message_field_attachments)
        result =
            decode_attachments(child.attachments, child_document, maximum_depth, text, used, media, &body);
    if (result.ok() && (child.seen & gateway_message_field_embeds))
        result = decode_embeds(child.embeds, child_document, maximum_depth, text, used, media, &body);
    if (result.ok() && (child.seen & gateway_message_field_sticker_items))
        result = decode_stickers(child.stickers, child_document, maximum_depth, text, used, media, &body);
    if (result.ok() && (child.seen & gateway_message_field_components))
        result =
            decode_components(child.components, child_document, maximum_depth, text, used, media, &body);
    if (!result.ok())
        return result;
    // A forwarded snapshot never carries its source's thread.
    body.fields |= static_cast<u32>(child.seen) & ~static_cast<u32>(gateway_message_field_thread);
    *output = {body.content,
               body.timestamp_ms,
               body.edited_timestamp_ms,
               body.attachments,
               body.embeds,
               body.embed_fields,
               body.stickers,
               body.components,
               body.mention_users,
               body.flags,
               body.fields,
               body.rich_truncations,
               body.type,
               true};
    return rich_failure(GatewayEventDecodeError::none);
}

} // namespace

GatewayEventDecodeResult gateway_extract_message_snapshot(JsonValue array, Slice<u32> structural_scratch,
                                                          u32 maximum_depth, JsonValue* body,
                                                          bool* present) noexcept {
    if (!body || !present || !array.source.data || !structural_scratch.data || !structural_scratch.count ||
        !maximum_depth || (array.kind != JsonValueKind::array && array.kind != JsonValueKind::null_value))
        return rich_failure(GatewayEventDecodeError::invalid_input);
    const auto scan = json_scan_structurals_avx1(array.source, structural_scratch);
    if (!scan.ok())
        return rich_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    return extract_snapshot(array, {array.source, {structural_scratch.data, scan.count}}, maximum_depth,
                            body, present);
}

GatewayEventDecodeResult gateway_decode_message_rich_fields(JsonValue data, Slice<u32> structural_scratch,
                                                            u32 maximum_depth, Slice<char> text_output,
                                                            u32* text_used,
                                                            const GatewayMessageRichScratch& scratch,
                                                            GatewayMessageEvent* output) noexcept {
    if (!output || !text_used || !data.source.data || data.source.bytes == 0 ||
        data.kind != JsonValueKind::object || !structural_scratch.data || structural_scratch.count == 0 ||
        maximum_depth == 0 || (!text_output.data && text_output.count != 0) ||
        *text_used > text_output.count || (!scratch.reactions.data && scratch.reactions.count != 0) ||
        (!scratch.attachments.data && scratch.attachments.count != 0) ||
        (!scratch.embeds.data && scratch.embeds.count != 0) ||
        (!scratch.embed_fields.data && scratch.embed_fields.count != 0) ||
        (!scratch.stickers.data && scratch.stickers.count != 0) ||
        (!scratch.components.data && scratch.components.count != 0) ||
        (!scratch.snapshot.attachments.data && scratch.snapshot.attachments.count) ||
        (!scratch.snapshot.embeds.data && scratch.snapshot.embeds.count) ||
        (!scratch.snapshot.embed_fields.data && scratch.snapshot.embed_fields.count) ||
        (!scratch.snapshot.stickers.data && scratch.snapshot.stickers.count) ||
        (!scratch.snapshot.components.data && scratch.snapshot.components.count) ||
        (!scratch.snapshot.mention_users.data && scratch.snapshot.mention_users.count)) {
        return rich_failure(GatewayEventDecodeError::invalid_input);
    }
    const JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return rich_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    const JsonIndexedDocument document{data.source, {structural_scratch.data, scan.count}};
    RootContext root{};
    const JsonReadResult read =
        json_visit_value_object(data, ConstSlice<u32>{structural_scratch.data, scan.count},
                               maximum_depth, visit_root, &root);
    if (!read.ok()) {
        return rich_failure(root.error != GatewayEventDecodeError::none
                                ? root.error
                                : GatewayEventDecodeError::invalid_json,
                            read.error_offset, JsonScanError::none, read.error);
    }
    GatewayEventDecodeResult result =
        decode_reference(root, document, maximum_depth, text_output, text_used, output);
    if (result.ok() && (root.seen & gateway_message_field_interaction) != 0) {
        result =
            decode_interaction(root.interaction, document, maximum_depth, text_output, text_used, output);
    }
    if (result.ok() && (root.seen & gateway_message_field_thread) != 0)
        result = decode_thread(root.thread, document, maximum_depth, text_output, text_used, output);
    if (!result.ok()) {
        return result;
    }
    if ((root.seen & gateway_message_field_reactions) != 0) {
        result = decode_reactions(root.reactions, document, maximum_depth, text_output, text_used, scratch,
                                  output);
    }
    if (result.ok() && (root.seen & gateway_message_field_attachments) != 0) {
        result = decode_attachments(root.attachments, document, maximum_depth, text_output, text_used,
                                    scratch, output);
    }
    if (result.ok() && (root.seen & gateway_message_field_embeds) != 0) {
        result =
            decode_embeds(root.embeds, document, maximum_depth, text_output, text_used, scratch, output);
    }
    if (result.ok() && (root.seen & gateway_message_field_sticker_items) != 0) {
        result = decode_stickers(root.stickers, document, maximum_depth, text_output, text_used, scratch,
                                 output);
    }
    if (result.ok() && (root.seen & gateway_message_field_components) != 0) {
        result = decode_components(root.components, document, maximum_depth, text_output, text_used,
                                   scratch, output);
    }
    if (result.ok() && (root.seen & gateway_message_field_poll))
        result = gateway_decode_message_poll(root.poll, document, maximum_depth, text_output, text_used,
                                             scratch.poll, &output->poll);
    if (result.ok() && (root.seen & gateway_message_field_message_snapshots))
        result = decode_snapshot(root.snapshots, document, structural_scratch, maximum_depth, text_output,
                                 text_used, scratch.snapshot, &output->snapshot);
    if (!result.ok()) {
        return result;
    }
    output->fields |= static_cast<u32>(root.seen);
    output->transient_bytes = *text_used;
    return rich_failure(GatewayEventDecodeError::none);
}

} // namespace maicord
