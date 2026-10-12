#include "maicord/protocol/thread_actions.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {
struct MemberParse {
    ThreadMemberSnapshot value{};
    ThreadActionError error{};
};
bool visit_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<MemberParse*>(opaque);
    u32 field{};
    if (json_string_equals_ascii(name, utf8_literal("id"))) field = thread_member_thread_id;
    else if (json_string_equals_ascii(name, utf8_literal("user_id"))) field = thread_member_user_id;
    else if (json_string_equals_ascii(name, utf8_literal("join_timestamp"))) field = thread_member_join_timestamp;
    else if (json_string_equals_ascii(name, utf8_literal("flags"))) field = thread_member_flags;
    else return true;
    if (parse.value.fields & field) {
        parse.error = ThreadActionError::duplicate;
        return false;
    }
    parse.value.fields |= field;
    bool valid{};
    if (field == thread_member_thread_id || field == thread_member_user_id) {
        u64 id{};
        const u64 expected = field == thread_member_thread_id ? parse.value.thread_id : parse.value.user_id;
        valid = json_value_to_snowflake(value, &id) && id == expected;
        if (!valid) parse.error = ThreadActionError::identity;
    } else if (field == thread_member_join_timestamp) {
        JsonString timestamp{};
        valid = json_value_as_string(value, &timestamp) &&
            parse_iso_timestamp(timestamp, &parse.value.join_timestamp_ms);
    } else {
        u64 flags{};
        valid = json_value_to_u64(value, &flags) && flags <= 0xffffffffull;
        if (valid) parse.value.flags = static_cast<u32>(flags);
    }
    if (!valid && parse.error == ThreadActionError::none)
        parse.error = ThreadActionError::field;
    return valid;
}
} // namespace

bool thread_action_request_valid(const ThreadActionRequest& request) noexcept {
    if (!request.thread_id || !request.self_user_id ||
        static_cast<u8>(request.operation) > static_cast<u8>(ThreadActionOperation::unarchive))
        return false;
    for (u8 byte : request.reserved) if (byte) return false;
    return true;
}

ThreadActionResult thread_action_write_target(
    const ThreadActionRequest& request, Slice<u8> output) noexcept {
    if (!thread_action_request_valid(request) || !byte_range_valid({output.data, output.count}))
        return {0, ThreadActionError::input};
    u8 temporary[kThreadActionMaximumTargetBytes]{};
    ProtocolWriter writer{{temporary, sizeof(temporary)}, 0};
    if (!protocol_writer_append_literal(&writer, "/api/v10/channels/") ||
        !protocol_writer_append_u64(&writer, request.thread_id) ||
        (request.operation != ThreadActionOperation::unarchive &&
         (!protocol_writer_append_literal(&writer, "/thread-members/") ||
          (request.operation == ThreadActionOperation::fetch_self
              ? !protocol_writer_append_u64(&writer, request.self_user_id)
              : !protocol_writer_append_literal(&writer, "@me")))))
        return {0, ThreadActionError::capacity};
    if (writer.used > output.count) return {writer.used, ThreadActionError::capacity};
    copy_bytes(output.data, temporary, writer.used);
    return {writer.used};
}

ThreadActionResult thread_action_write_body(
    const ThreadActionRequest& request, Slice<u8> output) noexcept {
    if (!thread_action_request_valid(request) || !byte_range_valid({output.data, output.count}))
        return {0, ThreadActionError::input};
    if (request.operation != ThreadActionOperation::unarchive) return {};
    constexpr auto body = utf8_literal("{\"archived\":false}");
    if (output.count < body.bytes) return {body.bytes, ThreadActionError::capacity};
    copy_bytes(output.data, body.data, body.bytes);
    return {body.bytes};
}

ThreadActionResult thread_member_decode(
    Utf8View input, u64 expected_thread_id, u64 expected_self_user_id,
    ThreadMemberSnapshot* output) noexcept {
    if (!output || !expected_thread_id || !expected_self_user_id || !input.bytes ||
        input.bytes > kThreadMemberMaximumBodyBytes ||
        !byte_range_valid({reinterpret_cast<const u8*>(input.data), input.bytes}) || !utf8_validate(input).valid)
        return {0, ThreadActionError::input};
    u32 offsets[4'096]{};
    const auto scanned = json_scan_structurals_avx1(input, {offsets, 4'096});
    if (!scanned.ok()) return {0, scanned.error == JsonScanError::output_overflow
        ? ThreadActionError::capacity : ThreadActionError::json};
    MemberParse parse{};
    parse.value.thread_id = expected_thread_id;
    parse.value.user_id = expected_self_user_id;
    const auto read = json_visit_root_object(input, {offsets, scanned.count}, 32, visit_member, &parse);
    if (!read.ok()) return {0, parse.error == ThreadActionError::none
        ? ThreadActionError::json : parse.error};
    constexpr u32 required = thread_member_join_timestamp | thread_member_flags;
    if ((parse.value.fields & required) != required) return {0, ThreadActionError::field};
    *output = parse.value;
    return {input.bytes};
}
} // namespace maicord
