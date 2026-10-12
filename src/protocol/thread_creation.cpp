#include "maicord/protocol/thread_creation.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/thread_actions.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace {
bool archive_duration(u64 minutes) noexcept {
    return minutes == 60 || minutes == 1440 || minutes == 4320 || minutes == 10080;
}
bool whitespace(u32 scalar) noexcept {
    return scalar <= 32 || scalar == 0x85 || scalar == 0xa0 || scalar == 0x1680 ||
           (scalar >= 0x2000 && scalar <= 0x200a) || scalar == 0x2028 || scalar == 0x2029 ||
           scalar == 0x202f || scalar == 0x205f || scalar == 0x3000;
}
bool name_valid(Utf8View name) noexcept {
    if (!name.bytes || name.bytes > kThreadCreationNameMaximumBytes ||
        !byte_range_valid({reinterpret_cast<const u8*>(name.data), name.bytes}))
        return false;
    const auto utf = utf8_validate(name);
    if (!utf.valid || !utf.codepoints || utf.codepoints > kThreadCreationNameMaximumScalars)
        return false;
    bool nonblank = false;
    for (u32 at = 0; at < name.bytes;) {
        const auto scalar = utf8_decode_one(name.data + at, name.bytes - at);
        if (!scalar.valid || scalar.codepoint < 32 || (scalar.codepoint >= 127 && scalar.codepoint <= 159))
            return false;
        nonblank |= !whitespace(scalar.codepoint);
        at += scalar.width;
    }
    return nonblank;
}
struct Parse {
    ThreadCreationSnapshot value{};
    JsonIndexedDocument document;
    JsonValue metadata{}, member{};
    u32 seen{}, metadata_seen{};
    ThreadCreationError error{};
};
bool reject(Parse& parse, ThreadCreationError error) noexcept {
    parse.error = error;
    return false;
}
bool root_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(opaque);
    u32 bit{};
    if (json_string_equals_ascii(name, utf8_literal("id")))
        bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("parent_id")))
        bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("guild_id")))
        bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("type")))
        bit = 8;
    else if (json_string_equals_ascii(name, utf8_literal("name")))
        bit = 16;
    else if (json_string_equals_ascii(name, utf8_literal("thread_metadata")))
        bit = 32;
    else if (json_string_equals_ascii(name, utf8_literal("last_message_id")))
        bit = 64;
    else if (json_string_equals_ascii(name, utf8_literal("member")))
        bit = 128;
    else
        return true;
    if (parse.seen & bit)
        return reject(parse, ThreadCreationError::duplicate);
    parse.seen |= bit;
    bool valid = false;
    if (bit == 1)
        valid = json_value_to_snowflake(value, &parse.value.id);
    else if (bit == 2)
        valid = json_value_to_snowflake(value, &parse.value.parent_id);
    else if (bit == 4)
        valid = json_value_to_snowflake(value, &parse.value.guild_id);
    else if (bit == 8) {
        u64 type{};
        valid = json_value_to_u64(value, &type) && (type == 10 || type == 11);
        if (valid)
            parse.value.type = static_cast<u8>(type);
    } else if (bit == 16) {
        JsonString encoded{};
        valid = json_value_as_string(value, &encoded);
        if (valid) {
            const auto decoded =
                json_string_decode_utf8(encoded, {parse.value.name, sizeof(parse.value.name)});
            valid = decoded.ok() && name_valid({parse.value.name, decoded.bytes});
            if (valid)
                parse.value.name_bytes = decoded.bytes;
        }
    } else if (bit == 32) {
        valid = value.kind == JsonValueKind::object;
        if (valid)
            parse.metadata = value;
    } else if (bit == 64) {
        valid = value.kind == JsonValueKind::null_value ||
                json_value_to_snowflake(value, &parse.value.last_message_id);
        if (valid)
            parse.value.fields |= thread_creation_last_message_known;
    } else {
        valid = value.kind == JsonValueKind::object || value.kind == JsonValueKind::null_value;
        if (valid)
            parse.member = value;
    }
    return valid || reject(parse, ThreadCreationError::field);
}
bool metadata_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(opaque);
    u32 bit{};
    if (json_string_equals_ascii(name, utf8_literal("archived")))
        bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("locked")))
        bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("auto_archive_duration")))
        bit = 4;
    else if (json_string_equals_ascii(name, utf8_literal("archive_timestamp")))
        bit = 8;
    else if (json_string_equals_ascii(name, utf8_literal("create_timestamp")))
        bit = 16;
    else if (json_string_equals_ascii(name, utf8_literal("invitable")))
        bit = 32;
    else
        return true;
    if (parse.metadata_seen & bit)
        return reject(parse, ThreadCreationError::duplicate);
    parse.metadata_seen |= bit;
    bool valid = false;
    if (bit == 1 || bit == 2 || bit == 32) {
        bool state{};
        valid = json_value_to_bool(value, &state);
        if (valid && state && bit != 32)
            parse.value.fields |= bit == 1 ? thread_creation_archived : thread_creation_locked;
    } else if (bit == 4) {
        u64 duration{};
        valid = json_value_to_u64(value, &duration) && archive_duration(duration);
        if (valid)
            parse.value.auto_archive_duration = static_cast<u16>(duration);
    } else {
        JsonString timestamp{};
        u64 parsed{};
        valid = (bit == 16 && value.kind == JsonValueKind::null_value) ||
                (json_value_as_string(value, &timestamp) && parse_iso_timestamp(timestamp, &parsed));
        if (valid && bit == 8)
            parse.value.archive_timestamp_ms = parsed;
    }
    return valid || reject(parse, ThreadCreationError::field);
}
ThreadCreationError member_error(ThreadActionError error) noexcept {
    switch (error) {
    case ThreadActionError::capacity:
        return ThreadCreationError::capacity;
    case ThreadActionError::json:
        return ThreadCreationError::json;
    case ThreadActionError::duplicate:
        return ThreadCreationError::duplicate;
    case ThreadActionError::identity:
        return ThreadCreationError::identity;
    default:
        return ThreadCreationError::field;
    }
}
} // namespace

bool thread_creation_request_valid(const ThreadCreationRequest& request) noexcept {
    if (!request.parent_id || !request.guild_id || !request.source_message_id ||
        request.parent_id == request.guild_id || request.source_message_id == request.parent_id ||
        request.source_message_id == request.guild_id ||
        (request.thread_type != 10 && request.thread_type != 11) ||
        static_cast<u8>(request.operation) > static_cast<u8>(ThreadCreationOperation::reconcile) ||
        !archive_duration(request.auto_archive_duration) || !name_valid(request.name))
        return false;
    for (u8 byte : request.reserved)
        if (byte)
            return false;
    return true;
}
ThreadCreationResult thread_creation_write_target(const ThreadCreationRequest& request,
                                                  Slice<u8> output) noexcept {
    if (!thread_creation_request_valid(request) || !byte_range_valid({output.data, output.count}))
        return {0, ThreadCreationError::input};
    u8 temporary[kThreadCreationMaximumTargetBytes];
    ProtocolWriter writer{{temporary, sizeof(temporary)}, 0};
    if (!protocol_writer_append_literal(&writer, "/api/v10/channels/") ||
        !protocol_writer_append_u64(&writer, request.operation == ThreadCreationOperation::from_message
                                                 ? request.parent_id
                                                 : request.source_message_id) ||
        (request.operation == ThreadCreationOperation::from_message &&
         (!protocol_writer_append_literal(&writer, "/messages/") ||
          !protocol_writer_append_u64(&writer, request.source_message_id) ||
          !protocol_writer_append_literal(&writer, "/threads"))))
        return {0, ThreadCreationError::capacity};
    if (writer.used > output.count)
        return {writer.used, ThreadCreationError::capacity};
    copy_bytes(output.data, temporary, writer.used);
    return {writer.used};
}
ThreadCreationResult thread_creation_write_body(const ThreadCreationRequest& request,
                                                Slice<u8> output) noexcept {
    if (!thread_creation_request_valid(request) || !byte_range_valid({output.data, output.count}))
        return {0, ThreadCreationError::input};
    if (request.operation == ThreadCreationOperation::reconcile)
        return {};
    u8 temporary[kThreadCreationMaximumBodyBytes];
    ProtocolWriter writer{{temporary, sizeof(temporary)}, 0};
    if (!protocol_writer_append_literal(&writer, "{\"name\":") ||
        !protocol_writer_append_json_string(&writer, request.name) ||
        !protocol_writer_append_literal(&writer, ",\"type\":") ||
        !protocol_writer_append_u64(&writer, request.thread_type) ||
        !protocol_writer_append_literal(&writer, ",\"auto_archive_duration\":") ||
        !protocol_writer_append_u64(&writer, request.auto_archive_duration) ||
        !protocol_writer_append_byte(&writer, '}'))
        return {0, ThreadCreationError::capacity};
    if (writer.used > output.count)
        return {writer.used, ThreadCreationError::capacity};
    copy_bytes(output.data, temporary, writer.used);
    return {writer.used};
}
namespace {
ThreadCreationResult decode_bounded_response(Utf8View input, const ThreadCreationRequest& request,
                                             u64 self_user_id, Slice<u32> structural_scratch,
                                             ThreadCreationSnapshot* output,
                                             u32 maximum_response_bytes) noexcept {
    if (maximum_response_bytes != kThreadCreationMaximumResponseBytes &&
        maximum_response_bytes != kThreadCreationForumMaximumResponseBytes)
        return {0, ThreadCreationError::input};
    if (!output || !self_user_id || !thread_creation_request_valid(request) || !input.bytes ||
        input.bytes > maximum_response_bytes || !structural_scratch.count ||
        u64{structural_scratch.count} * sizeof(u32) > 0xffffffffull)
        return {0, ThreadCreationError::input};
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(input.data), input.bytes},
                                  {reinterpret_cast<const u8*>(structural_scratch.data),
                                   static_cast<u32>(u64{structural_scratch.count} * sizeof(u32))},
                                  {reinterpret_cast<const u8*>(output), sizeof(*output)}};
    for (u32 at = 0; at < 3; ++at) {
        if (!byte_range_valid(ranges[at]))
            return {0, ThreadCreationError::input};
        for (u32 before = 0; before < at; ++before)
            if (byte_ranges_overlap(ranges[at], ranges[before]))
                return {0, ThreadCreationError::input};
    }
    if (!utf8_validate(input).valid)
        return {0, ThreadCreationError::input};
    const auto query = request;
    const auto scanned = json_scan_structurals_avx1(input, structural_scratch);
    if (!scanned.ok())
        return {0, scanned.error == JsonScanError::output_overflow ? ThreadCreationError::capacity
                                                                   : ThreadCreationError::json};
    Parse parse{};
    parse.document = {input, {structural_scratch.data, scanned.count}};
    const auto root = json_visit_root_object(input, parse.document.structurals, 32, root_field, &parse);
    if (!root.ok())
        return {0, parse.error == ThreadCreationError::none ? ThreadCreationError::json : parse.error};
    if ((parse.seen & 63) != 63)
        return {0, ThreadCreationError::field};
    if (parse.value.id != query.source_message_id || parse.value.parent_id != query.parent_id ||
        parse.value.guild_id != query.guild_id || parse.value.type != query.thread_type)
        return {0, ThreadCreationError::identity};
    const auto metadata =
        json_visit_indexed_object(parse.document, parse.metadata, 32, metadata_field, &parse);
    if (!metadata.ok())
        return {0, parse.error == ThreadCreationError::none ? ThreadCreationError::json : parse.error};
    if ((parse.metadata_seen & 15) != 15)
        return {0, ThreadCreationError::field};
    if ((parse.seen & 128) && parse.member.kind == JsonValueKind::object) {
        ThreadMemberSnapshot member{};
        const auto decoded =
            thread_member_decode(parse.member.source, query.source_message_id, self_user_id, &member);
        if (!decoded.ok())
            return {0, member_error(decoded.error)};
        parse.value.fields |= thread_creation_self_joined;
        parse.value.self_join_timestamp_ms = member.join_timestamp_ms;
        parse.value.self_flags = member.flags;
    }
    *output = parse.value;
    return {input.bytes};
}
} // namespace
ThreadCreationResult thread_creation_decode(Utf8View input, const ThreadCreationRequest& request,
                                            u64 self_user_id, Slice<u32> scratch,
                                            ThreadCreationSnapshot* output) noexcept {
    return decode_bounded_response(input, request, self_user_id, scratch, output,
                                   kThreadCreationMaximumResponseBytes);
}
ThreadCreationResult thread_creation_decode(Utf8View input, const ThreadCreationRequest& request,
                                            u64 self_user_id, Slice<u32> scratch,
                                            ThreadCreationSnapshot* output,
                                            ThreadCreationEnvelope envelope) noexcept {
    if (envelope != ThreadCreationEnvelope::channel && envelope != ThreadCreationEnvelope::forum_post)
        return {0, ThreadCreationError::input};
    return decode_bounded_response(input, request, self_user_id, scratch, output,
                                   envelope == ThreadCreationEnvelope::forum_post
                                       ? kThreadCreationForumMaximumResponseBytes
                                       : kThreadCreationMaximumResponseBytes);
}
Utf8View thread_creation_name_view(const ThreadCreationSnapshot& value) noexcept {
    return value.name_bytes <= sizeof(value.name) ? Utf8View{value.name, value.name_bytes} : Utf8View{};
}
} // namespace maicord
