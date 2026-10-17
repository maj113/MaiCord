#include "maicord/protocol/forum_post.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/byte_range.hpp"
#include "json_write.hpp"

namespace maicord {
static_assert(kForumPostMaximumResponseBytes == kThreadCreationForumMaximumResponseBytes);
namespace {
constexpr u32 obfuscated_channel_flag = 1u << 17;
bool archive_duration(u64 value) noexcept {
    return value == 60 || value == 1440 || value == 4320 || value == 10080;
}
bool whitespace(u32 value) noexcept {
    return value <= 32 || value == 0x85 || value == 0xa0 || value == 0x1680 ||
           (value >= 0x2000 && value <= 0x200a) || value == 0x2028 || value == 0x2029 || value == 0x202f ||
           value == 0x205f || value == 0x3000;
}
bool text_valid(Utf8View text, u32 maximum_bytes, u32 maximum_scalars, bool multiline,
                bool require_nonblank) noexcept {
    if (text.bytes > maximum_bytes ||
        !byte_range_valid({reinterpret_cast<const u8*>(text.data), text.bytes}))
        return false;
    const auto valid = utf8_validate(text);
    if (!valid.valid || valid.codepoints > maximum_scalars)
        return false;
    bool nonblank = false;
    for (u32 at = 0; at < text.bytes;) {
        const auto scalar = utf8_decode_one(text.data + at, text.bytes - at);
        if (!scalar.valid ||
            (scalar.codepoint < 32 &&
             !(multiline && (scalar.codepoint == 9 || scalar.codepoint == 10 || scalar.codepoint == 13))) ||
            (scalar.codepoint >= 127 && scalar.codepoint <= 159) ||
            (!multiline && (scalar.codepoint == 0x2028 || scalar.codepoint == 0x2029)))
            return false;
        nonblank |= !whitespace(scalar.codepoint);
        at += scalar.width;
    }
    return !require_nonblank || nonblank;
}
bool unique_ids(ConstSlice<u64> ids, u32 maximum_count) noexcept {
    if (ids.count > maximum_count || u64{ids.count} * sizeof(u64) > 0xffffffffull ||
        !byte_range_valid(
            {reinterpret_cast<const u8*>(ids.data), static_cast<u32>(u64{ids.count} * sizeof(u64))}))
        return false;
    for (u32 at = 0; at < ids.count; ++at) {
        if (!ids[at])
            return false;
        for (u32 before = 0; before < at; ++before)
            if (ids[at] == ids[before])
                return false;
    }
    return true;
}
bool ranges_valid(Utf8View input, Slice<u32> scratch, const void* output, u32 output_bytes) noexcept {
    if (!output || !input.bytes || input.bytes > kForumPostMaximumResponseBytes || !scratch.count ||
        u64{scratch.count} * sizeof(u32) > 0xffffffffull)
        return false;
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(input.data), input.bytes},
        {reinterpret_cast<const u8*>(scratch.data), static_cast<u32>(u64{scratch.count} * sizeof(u32))},
        {reinterpret_cast<const u8*>(output), output_bytes}};
    for (u32 at = 0; at < 3; ++at) {
        if (!byte_range_valid(ranges[at]))
            return false;
        for (u32 before = 0; before < at; ++before)
            if (byte_ranges_overlap(ranges[at], ranges[before]))
                return false;
    }
    return utf8_validate(input).valid;
}
ForumPostResult scan_document(Utf8View input, Slice<u32> scratch, JsonIndexedDocument* document) noexcept {
    const auto scanned = json_scan_structurals_avx1(input, scratch);
    if (!scanned.ok())
        return {0, scanned.error == JsonScanError::output_overflow ? ForumPostError::capacity
                                                                   : ForumPostError::json};
    *document = {input, {scratch.data, scanned.count}};
    return {};
}
struct ParentParse {
    ForumParentSnapshot value;
    JsonIndexedDocument document;
    JsonValue tags;
    u32 seen;
    ForumPostError error;
};
struct TagParse {
    ForumTagSnapshot value;
    u32 seen;
    bool emoji_id, emoji_name;
    ForumPostError error;
};
bool tag_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<TagParse*>(opaque);
    u32 bit{};
    if (json_string_equals_ascii(name, utf8_literal("id")))
        bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("name")))
        bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("moderated")))
        bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("emoji_id")))
        bit = 8;
    else if (json_string_equals_ascii(name, utf8_literal("emoji_name")))
        bit = 16;
    else
        return true;
    if (parse.seen & bit) {
        parse.error = ForumPostError::duplicate;
        return false;
    }
    parse.seen |= bit;
    bool valid = false;
    if (bit == 1)
        valid = json_value_to_snowflake(value, &parse.value.id);
    else if (bit == 4)
        valid = json_value_to_bool(value, &parse.value.moderated);
    else if (bit == 8) {
        u64 id{};
        valid = value.kind == JsonValueKind::null_value || json_value_to_snowflake(value, &id);
        parse.emoji_id = value.kind != JsonValueKind::null_value;
    } else {
        JsonString text{};
        valid =
            (bit == 16 && value.kind == JsonValueKind::null_value) || json_value_as_string(value, &text);
        if (valid && bit == 2) {
            const auto decoded =
                json_string_decode_utf8(text, {parse.value.name, sizeof(parse.value.name)});
            valid = decoded.ok() && text_valid({parse.value.name, decoded.bytes}, sizeof(parse.value.name),
                                               kForumPostTagNameMaximumScalars, false, false);
            if (valid)
                parse.value.name_bytes = decoded.bytes;
        }
        if (bit == 16)
            parse.emoji_name = value.kind != JsonValueKind::null_value;
    }
    if (!valid)
        parse.error = ForumPostError::field;
    return valid;
}
bool tag_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto& parse = *static_cast<ParentParse*>(opaque);
    if (index >= kForumPostAvailableTagMaximum) {
        parse.error = ForumPostError::capacity;
        return false;
    }
    if (value.kind != JsonValueKind::object) {
        parse.error = ForumPostError::field;
        return false;
    }
    TagParse tag{};
    const auto visited = json_visit_indexed_object(parse.document, value, 32, tag_field, &tag);
    if (!visited.ok() || (tag.seen & 7) != 7 || (tag.emoji_id && tag.emoji_name)) {
        parse.error = tag.error == ForumPostError::none ? ForumPostError::field : tag.error;
        return false;
    }
    if (tag.value.id == parse.value.id || tag.value.id == parse.value.guild_id) {
        parse.error = ForumPostError::identity;
        return false;
    }
    for (u32 before = 0; before < index; ++before)
        if (parse.value.tags[before].id == tag.value.id) {
            parse.error = ForumPostError::duplicate;
            return false;
        }
    parse.value.tags[index] = tag.value;
    parse.value.tag_count = static_cast<u8>(index + 1);
    return true;
}
bool parent_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<ParentParse*>(opaque);
    u32 bit{};
    if (json_string_equals_ascii(name, utf8_literal("id")))
        bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("guild_id")))
        bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("type")))
        bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("name")))
        bit = 8;
    else if (json_string_equals_ascii(name, utf8_literal("flags")))
        bit = 16;
    else if (json_string_equals_ascii(name, utf8_literal("available_tags")))
        bit = 32;
    else if (json_string_equals_ascii(name, utf8_literal("default_auto_archive_duration")))
        bit = 64;
    else if (json_string_equals_ascii(name, utf8_literal("default_sort_order")))
        bit = 128;
    else if (json_string_equals_ascii(name, utf8_literal("default_forum_layout")))
        bit = 256;
    else if (json_string_equals_ascii(name, utf8_literal("default_tag_setting")))
        bit = 512;
    else
        return true;
    if (parse.seen & bit) {
        parse.error = ForumPostError::duplicate;
        return false;
    }
    parse.seen |= bit;
    bool valid = false;
    if (bit >= 128) {
        // Post list presentation defaults. Null or an unknown future value stays
        // unknown (the client default applies); a wrong JSON type rejects.
        u64 scalar{};
        JsonString setting{};
        if (value.kind == JsonValueKind::null_value)
            valid = true;
        else if (bit == 512) {
            valid = json_value_as_string(value, &setting);
            const bool all = valid && json_string_equals_ascii(setting, utf8_literal("match_all"));
            if (all || (valid && json_string_equals_ascii(setting, utf8_literal("match_some")))) {
                parse.value.default_tag_setting = all ? 1 : 0;
                parse.value.fields |= forum_parent_default_tag_setting;
            }
        } else {
            valid = json_value_to_u64(value, &scalar);
            if (valid && bit == 128 && scalar <= 1) {
                parse.value.default_sort_order = static_cast<u8>(scalar);
                parse.value.fields |= forum_parent_default_sort_order;
            } else if (valid && bit == 256 && scalar <= 2) {
                parse.value.default_forum_layout = static_cast<u8>(scalar);
                parse.value.fields |= forum_parent_default_forum_layout;
            }
        }
    } else if (bit == 1)
        valid = json_value_to_snowflake(value, &parse.value.id);
    else if (bit == 2)
        valid = json_value_to_snowflake(value, &parse.value.guild_id);
    else if (bit == 4 || bit == 16 || bit == 64) {
        u64 scalar{};
        valid = json_value_to_u64(value, &scalar);
        if (bit == 4) {
            valid &= scalar == 15 || scalar == 16;
            if (valid)
                parse.value.type = static_cast<u8>(scalar);
        } else if (bit == 16) {
            valid &= scalar <= 0xffffffffull;
            if (valid) {
                parse.value.flags = static_cast<u32>(scalar);
                parse.value.fields |= forum_parent_flags;
            }
        } else {
            valid &= archive_duration(scalar);
            if (valid) {
                parse.value.default_auto_archive_duration = static_cast<u16>(scalar);
                parse.value.fields |= forum_parent_default_archive_duration;
            }
        }
    } else if (bit == 8) {
        JsonString text{};
        valid = json_value_as_string(value, &text);
        if (valid) {
            const auto decoded =
                json_string_decode_utf8(text, {parse.value.name, sizeof(parse.value.name)});
            valid = decoded.ok() && text_valid({parse.value.name, decoded.bytes}, sizeof(parse.value.name),
                                               100, false, true);
            if (valid)
                parse.value.name_bytes = decoded.bytes;
        }
    } else {
        valid = value.kind == JsonValueKind::array;
        if (valid) {
            parse.tags = value;
            parse.value.fields |= forum_parent_tags;
        }
    }
    if (!valid)
        parse.error = ForumPostError::field;
    return valid;
}
struct PostParse {
    ForumPostSnapshot value;
    JsonIndexedDocument document;
    JsonValue message, tags;
    u64 id;
    u32 seen;
    ForumPostError error;
};
bool post_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<PostParse*>(opaque);
    u32 bit{};
    if (json_string_equals_ascii(name, utf8_literal("id")))
        bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("owner_id")))
        bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("message")))
        bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("applied_tags")))
        bit = 8;
    else
        return true;
    if (parse.seen & bit) {
        parse.error = ForumPostError::duplicate;
        return false;
    }
    parse.seen |= bit;
    bool valid = false;
    if (bit == 1)
        valid = json_value_to_snowflake(value, &parse.id);
    else if (bit == 2)
        valid = json_value_to_snowflake(value, &parse.value.owner_id);
    else if (bit == 4) {
        valid = value.kind == JsonValueKind::object;
        if (valid)
            parse.message = value;
    } else {
        valid = value.kind == JsonValueKind::array;
        if (valid)
            parse.tags = value;
    }
    if (!valid)
        parse.error = ForumPostError::field;
    return valid;
}
bool applied_tag_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto& parse = *static_cast<PostParse*>(opaque);
    if (index >= kForumPostAppliedTagMaximum) {
        parse.error = ForumPostError::capacity;
        return false;
    }
    u64 id{};
    if (!json_value_to_snowflake(value, &id)) {
        parse.error = ForumPostError::field;
        return false;
    }
    for (u32 before = 0; before < index; ++before)
        if (parse.value.applied_tag_ids[before] == id) {
            parse.error = ForumPostError::duplicate;
            return false;
        }
    parse.value.applied_tag_ids[index] = id;
    parse.value.applied_tag_count = static_cast<u8>(index + 1);
    return true;
}
struct MessageScope {
    u64 guild;
    bool guild_seen;
    ForumPostError error;
    bool attachments_seen;
    JsonValue attachments;
};
bool message_scope_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& scope = *static_cast<MessageScope*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("attachments"))) {
        if (scope.attachments_seen) {
            scope.error = ForumPostError::duplicate;
            return false;
        }
        scope.attachments_seen = true;
        if (value.kind != JsonValueKind::array) {
            scope.error = ForumPostError::field;
            return false;
        }
        scope.attachments = value;
        return true;
    }
    if (!json_string_equals_ascii(name, utf8_literal("guild_id")))
        return true;
    if (scope.guild_seen) {
        scope.error = ForumPostError::duplicate;
        return false;
    }
    scope.guild_seen = true;
    u64 guild{};
    if (!json_value_to_snowflake(value, &guild) || guild != scope.guild) {
        scope.error = ForumPostError::identity;
        return false;
    }
    return true;
}
struct AttachmentParse {
    ForumPostAttachmentSnapshot value;
    JsonString texts[6];
    u32 seen;
    ForumPostError error;
};
bool attachment_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<AttachmentParse*>(opaque);
    const char* keys[]{"id",  "size",      "filename", "title",  "description", "content_type",
                       "url", "proxy_url", "width",    "height", "flags",       "ephemeral"};
    u32 index = 12;
    for (u32 at = 0; at < 12; ++at) {
        u32 bytes = 0;
        while (keys[at][bytes])
            ++bytes;
        if (json_string_equals_ascii(name, {keys[at], bytes})) {
            index = at;
            break;
        }
    }
    if (index == 12)
        return true;
    const u32 bit = 1u << index;
    if (parse.seen & bit) {
        parse.error = ForumPostError::duplicate;
        return false;
    }
    parse.seen |= bit;
    bool valid = false;
    if (index == 0)
        valid = json_value_to_snowflake(value, &parse.value.id);
    else if (index == 1)
        valid = json_value_to_u64(value, &parse.value.size) && parse.value.size;
    else if (index >= 2 && index <= 7)
        valid = json_value_as_string(value, &parse.texts[index - 2]);
    else if (index == 11)
        valid = json_value_to_bool(value, &parse.value.ephemeral);
    else if ((index == 8 || index == 9) && value.kind == JsonValueKind::null_value)
        valid = true;
    else {
        u64 scalar{};
        valid = json_value_to_u64(value, &scalar) && scalar <= 0xffffffffull;
        if (valid && index == 8) {
            parse.value.width = static_cast<u32>(scalar);
            parse.value.has_width = scalar != 0;
        }
        if (valid && index == 9) {
            parse.value.height = static_cast<u32>(scalar);
            parse.value.has_height = scalar != 0;
        }
        if (valid && index == 10)
            parse.value.flags = static_cast<u32>(scalar);
    }
    if (!valid)
        parse.error = ForumPostError::field;
    return valid;
}
bool attachment_url_valid(Utf8View text) noexcept {
    if (text.bytes <= 8 || !bytes_equal(text.data, "https://", 8))
        return false;
    u32 authority_end = 8;
    while (authority_end < text.bytes && text.data[authority_end] != '/' && text.data[authority_end] != '?')
        ++authority_end;
    if (authority_end == 8)
        return false;
    for (u32 at = 8; at < text.bytes; ++at) {
        const auto c = static_cast<u8>(text.data[at]);
        if (c <= 32 || c == 127 || c == '\\' || c == '#' || (at < authority_end && c == '@'))
            return false;
    }
    return true;
}
struct AttachmentListParse {
    JsonIndexedDocument document;
    ForumPostMediaSnapshot* storage;
    u64 ids[kAttachmentUploadMaximumCount];
    u32 count, text_bytes;
    ForumPostError error;
};
bool attachment_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto& list = *static_cast<AttachmentListParse*>(opaque);
    if (index >= kAttachmentUploadMaximumCount) {
        list.error = ForumPostError::capacity;
        return false;
    }
    if (value.kind != JsonValueKind::object) {
        list.error = ForumPostError::field;
        return false;
    }
    AttachmentParse parse{};
    if (!json_visit_indexed_object(list.document, value, 32, attachment_field, &parse).ok() ||
        (parse.seen & ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 6))) !=
            ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 6))) {
        list.error = parse.error == ForumPostError::none ? ForumPostError::field : parse.error;
        return false;
    }
    for (u32 before = 0; before < index; ++before)
        if (list.ids[before] == parse.value.id) {
            list.error = ForumPostError::duplicate;
            return false;
        }
    list.ids[index] = parse.value.id;
    ForumPostTextRange* ranges[]{&parse.value.filename,     &parse.value.title, &parse.value.description,
                                 &parse.value.content_type, &parse.value.url,   &parse.value.proxy_url};
    constexpr u32 maxima[]{2048, 1024, 4096, 128, 4096, 4096};
    // One reused small validation bank, never ten large per-file stack arrays.
    char decoded[4096];
    for (u32 at = 0; at < 6; ++at) {
        if (!(parse.seen & (1u << (at + 2))))
            continue;
        const auto text = json_string_decode_utf8(parse.texts[at], {decoded, maxima[at]});
        if (!text.ok()) {
            list.error = text.error == JsonStringDecodeError::output_too_small ? ForumPostError::capacity
                                                                               : ForumPostError::field;
            return false;
        }
        const Utf8View view{decoded, text.bytes};
        if (!text_valid(view, maxima[at], at == 2 ? 1024 : maxima[at], at == 2,
                        at == 0 || at == 4 || at == 5) ||
            ((at == 4 || at == 5) && !attachment_url_valid(view))) {
            list.error = ForumPostError::field;
            return false;
        }
        if (text.bytes > kForumPostAttachmentTextBytes - list.text_bytes) {
            list.error = ForumPostError::capacity;
            return false;
        }
        list.text_bytes += text.bytes;
        if (list.storage) {
            const u32 offset = list.storage->attachment_text_bytes;
            if (text.bytes > sizeof(list.storage->attachment_text) - offset) {
                list.error = ForumPostError::capacity;
                return false;
            }
            *ranges[at] = {offset, text.bytes};
            copy_bytes(list.storage->attachment_text + offset, decoded, text.bytes);
            list.storage->attachment_text_bytes += text.bytes;
        }
    }
    if (list.storage)
        list.storage->attachments[index] = parse.value;
    list.count = index + 1;
    return true;
}
ForumPostResult validate_attachments(const JsonIndexedDocument& document, const MessageScope& scope,
                                     u32 expected_count, ForumPostMediaSnapshot* storage) noexcept {
    if (!scope.attachments_seen)
        return expected_count ? ForumPostResult{0, ForumPostError::field} : ForumPostResult{};
    AttachmentListParse list{};
    list.document = document;
    list.storage = storage;
    if (!json_visit_indexed_array(document, scope.attachments, 32, attachment_item, &list).ok())
        return {0, list.error == ForumPostError::none ? ForumPostError::field : list.error};
    if (list.count != expected_count)
        return {0, ForumPostError::identity};
    if (storage)
        storage->attachment_count = static_cast<u8>(list.count);
    return {};
}
ForumPostError thread_error(ThreadCreationError error) noexcept {
    switch (error) {
    case ThreadCreationError::input:
        return ForumPostError::input;
    case ThreadCreationError::capacity:
        return ForumPostError::capacity;
    case ThreadCreationError::json:
        return ForumPostError::json;
    case ThreadCreationError::duplicate:
        return ForumPostError::duplicate;
    case ThreadCreationError::identity:
        return ForumPostError::identity;
    default:
        return ForumPostError::field;
    }
}
} // namespace

bool forum_post_request_valid(const ForumPostRequest& request) noexcept {
    if (!request.parent_id || !request.guild_id || request.parent_id == request.guild_id ||
        static_cast<u8>(request.operation) > static_cast<u8>(ForumPostOperation::attachment_url) ||
        request.upload_reserved || request.tag_count > kForumPostAppliedTagMaximum)
        return false;
    for (u8 value : request.reserved)
        if (value)
            return false;
    for (u32 at = request.tag_count; at < kForumPostAppliedTagMaximum; ++at)
        if (request.applied_tag_ids[at])
            return false;
    const bool no_draft = !request.title.data && !request.title.bytes && !request.content.data &&
                          !request.content.bytes && !request.auto_archive_duration && !request.tag_count;
    const bool no_references = !request.attachments.data && !request.attachments.count;
    const bool no_upload = !request.upload_filename.data && !request.upload_filename.bytes &&
                           !request.upload_bytes && !request.upload_id;
    if (request.operation == ForumPostOperation::parent_metadata)
        return no_draft && no_references && no_upload;
    if (request.operation == ForumPostOperation::attachment_url)
        return no_draft && no_references && request.upload_bytes &&
               attachment_upload_valid_filename(request.upload_filename);
    if (!no_upload || request.attachments.count > kAttachmentUploadMaximumCount ||
        (!request.attachments.count && request.attachments.data) ||
        !byte_range_valid({reinterpret_cast<const u8*>(request.attachments.data),
                           request.attachments.count * static_cast<u32>(sizeof(UploadedAttachment))}))
        return false;
    for (u32 at = 0; at < request.attachments.count; ++at) {
        const auto& file = request.attachments[at];
        if (!attachment_upload_valid_reference(file) ||
            !text_valid(file.filename, 1024, 1024, false, true) ||
            !text_valid(file.uploaded_filename, 4096, 4096, false, true) ||
            !text_valid(file.description, 4096, 1024, true, false))
            return false;
        for (u32 before = 0; before < at; ++before)
            if (request.attachments[at].id == request.attachments[before].id)
                return false;
    }
    return archive_duration(request.auto_archive_duration) &&
           unique_ids({request.applied_tag_ids, request.tag_count}, 5) &&
           text_valid(request.title, 400, 100, false, true) &&
           text_valid(request.content, 8000, 2000, true, !request.attachments.count);
}
ForumPostResult forum_post_write_target(const ForumPostRequest& request, Slice<u8> output) noexcept {
    if (!forum_post_request_valid(request) || !byte_range_valid({output.data, output.count}))
        return {0, ForumPostError::input};
    u8 temporary[kForumPostMaximumTargetBytes];
    if (request.operation == ForumPostOperation::attachment_url) {
        u8 body[2304];
        const auto written = attachment_upload_write(request.parent_id, request.upload_id,
                                                     request.upload_filename, request.upload_bytes,
                                                     {temporary, sizeof(temporary)}, {body, sizeof(body)});
        if (!written.ok || written.target_bytes > output.count)
            return {written.target_bytes, ForumPostError::capacity};
        copy_bytes(output.data, temporary, written.target_bytes);
        return {written.target_bytes};
    }
    ProtocolWriter writer{{temporary, sizeof(temporary)}, 0};
    if (!protocol_writer_append_literal(&writer, "/api/v10/channels/") ||
        !protocol_writer_append_u64(&writer, request.parent_id) ||
        (request.operation == ForumPostOperation::create &&
         !protocol_writer_append_literal(&writer, "/threads?use_nested_fields=true")))
        return {0, ForumPostError::capacity};
    if (writer.used > output.count)
        return {writer.used, ForumPostError::capacity};
    copy_bytes(output.data, temporary, writer.used);
    return {writer.used};
}
namespace {
// One emission path measures and writes the same bytes. The checked second pass
// writes directly into a disjoint caller bank, without a large stack candidate.
struct ForumBodyWriter {
    Slice<u8> output;
    u32 used;
    template <usize Count> bool literal(const char (&text)[Count]) noexcept {
        constexpr u32 size = static_cast<u32>(Count - 1);
        if (output.data)
            copy_bytes(output.data + used, text, size);
        used += size;
        return true;
    }
    bool number(u64 value) noexcept {
        if (output.data) {
            ProtocolWriter writer{output, used};
            if (!protocol_writer_append_u64(&writer, value))
                return false;
        }
        used += decimal_digits(value);
        return true;
    }
    bool string(Utf8View value) noexcept {
        u32 count{};
        if (!protocol_detail::measure_json_string(value, &count))
            return false;
        if (output.data)
            protocol_detail::write_json_string(output.data + used, value);
        used += count;
        return true;
    }
};
bool emit_body(const ForumPostRequest& request, ForumBodyWriter& writer) noexcept {
    if (request.operation == ForumPostOperation::parent_metadata)
        return true;
    if (request.operation == ForumPostOperation::attachment_url)
        return writer.literal("{\"files\":[{\"filename\":") && writer.string(request.upload_filename) &&
               writer.literal(",\"file_size\":") && writer.number(request.upload_bytes) &&
               writer.literal(",\"id\":\"") && writer.number(request.upload_id) &&
               writer.literal("\",\"is_clip\":false}]}");
    bool valid = writer.literal("{\"name\":") && writer.string(request.title) &&
                 writer.literal(",\"auto_archive_duration\":") &&
                 writer.number(request.auto_archive_duration) && writer.literal(",\"applied_tags\":[");
    for (u32 at = 0; valid && at < request.tag_count; ++at)
        valid = (!at || writer.literal(",")) && writer.literal("\"") &&
                writer.number(request.applied_tag_ids[at]) && writer.literal("\"");
    valid = valid && writer.literal("],\"message\":{\"content\":") && writer.string(request.content);
    if (valid && request.attachments.count) {
        valid = writer.literal(",\"attachments\":[");
        for (u32 at = 0; valid && at < request.attachments.count; ++at) {
            const auto& file = request.attachments[at];
            valid = (!at || writer.literal(",")) && writer.literal("{\"id\":\"") &&
                    writer.number(file.id) && writer.literal("\",\"filename\":") &&
                    writer.string(file.filename) && writer.literal(",\"uploaded_filename\":") &&
                    writer.string(file.uploaded_filename) &&
                    (!file.description.bytes ||
                     (writer.literal(",\"description\":") && writer.string(file.description))) &&
                    (!file.spoiler || writer.literal(",\"is_spoiler\":true")) && writer.literal("}");
        }
        valid = valid && writer.literal("]");
    }
    return valid && writer.literal("}}");
}
bool writer_inputs_disjoint(const ForumPostRequest& request, Slice<u8> output) noexcept {
    const ConstSlice<u8> destination{output.data, output.count};
    const ConstSlice<u8> fixed[]{
        {reinterpret_cast<const u8*>(&request), sizeof(request)},
        {reinterpret_cast<const u8*>(request.title.data), request.title.bytes},
        {reinterpret_cast<const u8*>(request.content.data), request.content.bytes},
        {reinterpret_cast<const u8*>(request.upload_filename.data), request.upload_filename.bytes},
        {reinterpret_cast<const u8*>(request.attachments.data),
         request.attachments.count * static_cast<u32>(sizeof(UploadedAttachment))}};
    for (auto range : fixed)
        if (byte_ranges_overlap(destination, range))
            return false;
    for (u32 index = 0; index < request.attachments.count; ++index) {
        const auto& file = request.attachments[index];
        const Utf8View texts[]{file.filename, file.uploaded_filename, file.description};
        for (auto text : texts)
            if (byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(text.data), text.bytes}))
                return false;
    }
    return true;
}
} // namespace
ForumPostResult forum_post_body_bytes(const ForumPostRequest& request) noexcept {
    if (!forum_post_request_valid(request))
        return {0, ForumPostError::input};
    ForumBodyWriter writer{};
    if (!emit_body(request, writer))
        return {0, ForumPostError::input};
    return {writer.used,
            writer.used > kForumPostMaximumBodyBytes ? ForumPostError::capacity : ForumPostError::none};
}
ForumPostResult forum_post_write_body(const ForumPostRequest& request, Slice<u8> output) noexcept {
    const auto size = forum_post_body_bytes(request);
    if (!size.ok())
        return size;
    if (!byte_range_valid({output.data, output.count}) || !writer_inputs_disjoint(request, output))
        return {0, ForumPostError::input};
    if (size.bytes > output.count)
        return {size.bytes, ForumPostError::capacity};
    if (!size.bytes)
        return {};
    if (request.operation == ForumPostOperation::attachment_url) {
        u8 target[kForumPostMaximumTargetBytes];
        const auto written =
            attachment_upload_write(request.parent_id, request.upload_id, request.upload_filename,
                                    request.upload_bytes, {target, sizeof(target)}, output);
        return written.ok && written.body_bytes == size.bytes
                   ? ForumPostResult{size.bytes}
                   : ForumPostResult{0, ForumPostError::capacity};
    }
    ForumBodyWriter writer{output, 0};
    return emit_body(request, writer) && writer.used == size.bytes
               ? ForumPostResult{writer.used}
               : ForumPostResult{0, ForumPostError::capacity};
}
ForumPostResult forum_parent_decode(Utf8View input, const ForumPostRequest& request, Slice<u32> scratch,
                                    ForumParentSnapshot* output) noexcept {
    if (!forum_post_request_valid(request) || request.operation != ForumPostOperation::parent_metadata ||
        !ranges_valid(input, scratch, output, sizeof(*output)))
        return {0, ForumPostError::input};
    const auto intent = request;
    ParentParse parse{};
    parse.value.default_auto_archive_duration = kForumPostDefaultArchiveDuration;
    const auto scanned = scan_document(input, scratch, &parse.document);
    if (!scanned.ok())
        return scanned;
    const auto root = json_visit_root_object(input, parse.document.structurals, 32, parent_field, &parse);
    if (!root.ok())
        return {0, parse.error == ForumPostError::none ? ForumPostError::json : parse.error};
    if ((parse.seen & 15) != 15)
        return {0, ForumPostError::field};
    if (parse.value.id != intent.parent_id || parse.value.guild_id != intent.guild_id)
        return {0, ForumPostError::identity};
    if ((parse.seen & 32) &&
        !json_visit_indexed_array(parse.document, parse.tags, 32, tag_item, &parse).ok())
        return {0, parse.error == ForumPostError::none ? ForumPostError::json : parse.error};
    *output = parse.value;
    return {input.bytes};
}
ForumPostResult forum_post_decode(Utf8View input, const ForumPostRequest& request, u64 self,
                                  Slice<u32> scratch, ForumPostSnapshot* output) noexcept {
    if (!self || !forum_post_request_valid(request) || request.operation != ForumPostOperation::create ||
        !ranges_valid(input, scratch, output, sizeof(*output)))
        return {0, ForumPostError::input};
    const auto intent = request;
    PostParse parse{};
    const auto scanned = scan_document(input, scratch, &parse.document);
    if (!scanned.ok())
        return scanned;
    const auto root = json_visit_root_object(input, parse.document.structurals, 32, post_field, &parse);
    if (!root.ok())
        return {0, parse.error == ForumPostError::none ? ForumPostError::json : parse.error};
    if ((parse.seen & 7) != 7 || (intent.tag_count && !(parse.seen & 8)))
        return {0, ForumPostError::field};
    if (parse.value.owner_id != self || parse.id == intent.parent_id || parse.id == intent.guild_id)
        return {0, ForumPostError::identity};
    if ((parse.seen & 8) &&
        !json_visit_indexed_array(parse.document, parse.tags, 32, applied_tag_item, &parse).ok())
        return {0, parse.error == ForumPostError::none ? ForumPostError::json : parse.error};
    if (parse.value.applied_tag_count != intent.tag_count)
        return {0, ForumPostError::identity};
    for (u32 at = 0; at < intent.tag_count; ++at) {
        bool found = false;
        for (u32 row = 0; row < parse.value.applied_tag_count; ++row)
            found |= intent.applied_tag_ids[at] == parse.value.applied_tag_ids[row];
        if (!found)
            return {0, ForumPostError::identity};
    }
    const ThreadCreationRequest thread_request{intent.parent_id,
                                               intent.guild_id,
                                               parse.id,
                                               intent.title,
                                               intent.auto_archive_duration,
                                               11,
                                               ThreadCreationOperation::reconcile,
                                               {}};
    const auto thread = thread_creation_decode(input, thread_request, self, scratch, &parse.value.thread,
                                               ThreadCreationEnvelope::forum_post);
    if (!thread.ok())
        return {0, thread_error(thread.error)};
    // The thread decoder leaves the document's index intact. Finish indexed
    // visitors before the message decoder rescans the nested object into the
    // same scratch.
    MessageScope scope{intent.guild_id, false, {}};
    if (!json_visit_indexed_object(parse.document, parse.message, 32, message_scope_field, &scope).ok())
        return {0, scope.error == ForumPostError::none ? ForumPostError::json : scope.error};
    const auto attachments = validate_attachments(parse.document, scope, intent.attachments.count, nullptr);
    if (!attachments.ok())
        return attachments;
    GatewayMessageEvent message{};
    const auto decoded = gateway_decode_message_create(
        parse.message, scratch, 32, {parse.value.content, sizeof(parse.value.content)}, &message);
    if (!decoded.ok())
        return {0, decoded.error == GatewayEventDecodeError::duplicate_field ? ForumPostError::duplicate
                                                                             : ForumPostError::field};
    if (message.channel_id != parse.id || message.author_id != self || !message.id ||
        message.id == intent.parent_id || message.id == intent.guild_id)
        return {0, ForumPostError::identity};
    if (!(message.fields & gateway_message_field_type) || message.type != 0 || message.author_bot ||
        !text_valid(message.content, 8000, 2000, true, false))
        return {0, ForumPostError::field};
    parse.value.message_id = message.id;
    parse.value.message_channel_id = message.channel_id;
    parse.value.message_author_id = message.author_id;
    parse.value.message_timestamp_ms = message.timestamp_ms;
    parse.value.message_flags = message.flags;
    parse.value.content_bytes = message.content.bytes;
    *output = parse.value;
    return {input.bytes};
}
namespace {
struct MediaMessageParse {
    JsonValue message;
    bool seen;
};
bool media_message_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<MediaMessageParse*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("message")))
        return true;
    if (parse.seen || value.kind != JsonValueKind::object)
        return false;
    parse.seen = true;
    parse.message = value;
    return true;
}
Utf8View attachment_text(const ForumPostMediaSnapshot& media, ForumPostTextRange range) noexcept {
    if (media.attachment_text_bytes > sizeof(media.attachment_text) ||
        range.offset > media.attachment_text_bytes ||
        range.bytes > media.attachment_text_bytes - range.offset)
        return {};
    return range.bytes ? Utf8View{media.attachment_text + range.offset, range.bytes} : Utf8View{};
}
} // namespace
ForumPostResult forum_post_media_decode(Utf8View input, const ForumPostRequest& request, u64 self,
                                        Slice<u32> scratch, ForumPostMediaSnapshot* temporary,
                                        ForumPostMediaSnapshot* output) noexcept {
    if (!self || !forum_post_request_valid(request) || request.operation != ForumPostOperation::create ||
        !ranges_valid(input, scratch, output, sizeof(*output)) || !temporary ||
        !ranges_valid(input, scratch, temporary, sizeof(*temporary)) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(temporary), sizeof(*temporary)},
                            {reinterpret_cast<const u8*>(output), sizeof(*output)}) ||
        !writer_inputs_disjoint(request, {reinterpret_cast<u8*>(temporary), sizeof(*temporary)}) ||
        !writer_inputs_disjoint(request, {reinterpret_cast<u8*>(output), sizeof(*output)}) ||
        !writer_inputs_disjoint(request, {reinterpret_cast<u8*>(scratch.data),
                                          static_cast<u32>(u64{scratch.count} * sizeof(u32))}))
        return {0, ForumPostError::input};
    clear_bytes(temporary, sizeof(*temporary));
    const auto core = forum_post_decode(input, request, self, scratch, &temporary->post);
    if (!core.ok())
        return core;
    JsonIndexedDocument document{};
    const auto scanned = scan_document(input, scratch, &document);
    if (!scanned.ok())
        return scanned;
    MediaMessageParse parse{};
    if (!json_visit_root_object(input, document.structurals, 32, media_message_field, &parse).ok() ||
        !parse.seen)
        return {0, ForumPostError::field};
    MessageScope scope{request.guild_id, false, {}};
    if (!json_visit_indexed_object(document, parse.message, 32, message_scope_field, &scope).ok())
        return {0, scope.error == ForumPostError::none ? ForumPostError::field : scope.error};
    const auto attachments = validate_attachments(document, scope, request.attachments.count, temporary);
    if (!attachments.ok())
        return attachments;
    copy_bytes(output, temporary, sizeof(*output));
    return {input.bytes};
}
GatewayMessageAttachment forum_post_attachment_view(const ForumPostMediaSnapshot& media,
                                                    u32 index) noexcept {
    if (media.attachment_count > kAttachmentUploadMaximumCount || index >= media.attachment_count ||
        media.attachment_text_bytes > sizeof(media.attachment_text))
        return {};
    const auto& file = media.attachments[index];
    const ForumPostTextRange ranges[]{file.filename,     file.title, file.description,
                                      file.content_type, file.url,   file.proxy_url};
    for (auto range : ranges)
        if (range.offset > media.attachment_text_bytes ||
            range.bytes > media.attachment_text_bytes - range.offset)
            return {};
    return {file.id,
            file.size,
            attachment_text(media, file.filename),
            attachment_text(media, file.title),
            attachment_text(media, file.description),
            attachment_text(media, file.content_type),
            attachment_text(media, file.url),
            file.width,
            file.height,
            file.flags,
            file.has_width,
            file.has_height,
            file.ephemeral};
}
bool forum_post_selection_valid(const ForumParentSnapshot& parent, ConstSlice<u64> selected,
                                bool manage_threads) noexcept {
    constexpr u8 authority = forum_parent_flags | forum_parent_tags;
    if (!parent.id || !parent.guild_id || parent.id == parent.guild_id ||
        (parent.type != 15 && parent.type != 16) || (parent.fields & authority) != authority ||
        parent.tag_count > 20 || (parent.flags & obfuscated_channel_flag) || !unique_ids(selected, 5) ||
        ((parent.flags & kForumRequireTagFlag) && !selected.count))
        return false;
    for (u32 row = 0; row < parent.tag_count; ++row) {
        if (!parent.tags[row].id || parent.tags[row].id == parent.id ||
            parent.tags[row].id == parent.guild_id ||
            !text_valid(forum_tag_name_view(parent.tags[row]), 80, 20, false, false) ||
            parent.tags[row].name_bytes > 80)
            return false;
        for (u32 before = 0; before < row; ++before)
            if (parent.tags[row].id == parent.tags[before].id)
                return false;
    }
    for (u32 at = 0; at < selected.count; ++at) {
        const ForumTagSnapshot* found = nullptr;
        for (u32 row = 0; row < parent.tag_count; ++row)
            if (selected[at] == parent.tags[row].id) {
                if (found)
                    return false;
                found = &parent.tags[row];
            }
        if (!found || (found->moderated && !manage_threads))
            return false;
    }
    return true;
}
Utf8View forum_tag_name_view(const ForumTagSnapshot& value) noexcept {
    return value.name_bytes <= sizeof(value.name) ? Utf8View{value.name, value.name_bytes} : Utf8View{};
}
Utf8View forum_post_content_view(const ForumPostSnapshot& value) noexcept {
    return value.content_bytes <= sizeof(value.content) ? Utf8View{value.content, value.content_bytes}
                                                        : Utf8View{};
}
} // namespace maicord
