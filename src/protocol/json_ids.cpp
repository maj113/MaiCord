#include "maicord/protocol/json_ids.hpp"

namespace maicord {

bool json_value_to_member_list_id(JsonValue value, u64* output) noexcept {
    JsonString encoded{};
    if (!output || !json_value_as_string(value, &encoded)) return false;
    if (json_string_equals_ascii(encoded, utf8_literal("everyone"))) {
        *output = kMemberListEveryone;
        return true;
    }
    u64 id = 0;
    if (!json_string_to_u64(encoded, &id) || id >= kMemberListZeroHash) return false;
    *output = id != 0 ? id : kMemberListZeroHash;
    return true;
}

bool json_value_to_snowflake(JsonValue value, u64* output) noexcept {
    JsonString encoded{};
    return output && json_value_as_string(value, &encoded) &&
           json_string_to_u64(encoded, output) && *output != 0;
}

bool json_value_to_nullable_snowflake(JsonValue value, u64* output) noexcept {
    if (value.kind == JsonValueKind::null_value) {
        *output = 0;
        return true;
    }
    return json_value_to_snowflake(value, output);
}

bool json_value_to_u64_or_string(JsonValue value, u64* output) noexcept {
    JsonString encoded{};
    return json_value_to_u64(value, output) ||
           (json_value_as_string(value, &encoded) && json_string_to_u64(encoded, output));
}

} // namespace maicord
