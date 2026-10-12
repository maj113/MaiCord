#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {

// Member-list IDs are opaque visibility-set identifiers, not snowflakes.
inline constexpr u64 kMemberListEveryone = ~0ull;
inline constexpr u64 kMemberListZeroHash = ~0ull - 1ull;
bool json_value_to_member_list_id(JsonValue value, u64* output) noexcept;

bool json_value_to_snowflake(JsonValue value, u64* output) noexcept;
// A snowflake, or null as zero.
bool json_value_to_nullable_snowflake(JsonValue value, u64* output) noexcept;
// An integer sent as a JSON number or as a decimal string.
bool json_value_to_u64_or_string(JsonValue value, u64* output) noexcept;

} // namespace maicord
