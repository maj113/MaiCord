#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kFriendSearchMaximumBytes = 128;
enum class FriendSearchError : u8 { none, invalid_query, invalid_name };
struct FriendSearchResult {
    FriendSearchError error;
    bool matched;
    constexpr bool ok() const noexcept { return error == FriendSearchError::none; }
};
// Current Friends-list semantics: independent substring search of username,
// relationship nickname and global display name, after full Unicode 17 default
// lowercase. Locale independent; no normalization, trimming or accent removal.
// Every input is valid UTF-8 and at most 128 bytes. Missing fields are empty.
// Empty query matches every valid input; malformed fields still report error.
// Uses fixed caller-thread scratch only; no allocation or retained name cache.
FriendSearchResult friend_search(Utf8View query, Utf8View username,
    Utf8View nickname, Utf8View display_name) noexcept;
struct FriendCompareResult {
    FriendSearchError error;
    i32 order;
    constexpr bool ok() const noexcept { return error == FriendSearchError::none; }
};
// Compare two chosen display fields after the same full default lowercase,
// using JavaScript string order: UTF-16 code units, rather than scalar/UTF-8
// order or locale collation. order is -1/0/+1; zero leaves identity tie-breaking
// to the caller. Both names obey the same 128-byte valid UTF-8 bound.
FriendCompareResult friend_name_compare(Utf8View left, Utf8View right) noexcept;
}
