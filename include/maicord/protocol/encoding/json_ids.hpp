#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {

// Member-list IDs are opaque visibility-set identifiers, not snowflakes.
inline constexpr u64 kMemberListEveryone = ~0ull;
inline constexpr u64 kMemberListZeroHash = ~0ull - 1ull;
bool json_value_to_member_list_id(const JsonValue& value, u64* output) noexcept;

bool json_value_to_snowflake(const JsonValue& value, u64* output) noexcept;
// A snowflake, or null as zero.
bool json_value_to_nullable_snowflake(const JsonValue& value, u64* output) noexcept;
// An integer sent as a JSON number or as a decimal string.
bool json_value_to_u64_or_string(const JsonValue& value, u64* output) noexcept;
// An ISO 8601 timestamp string as Unix milliseconds (parse_iso_timestamp); false for any other kind.
bool json_value_to_timestamp(const JsonValue& value, u64* timestamp_ms) noexcept;

} // namespace maicord
