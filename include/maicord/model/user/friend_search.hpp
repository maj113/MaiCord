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
// Substring match of the query against each of username, nickname and display
// name, after full Unicode 17 default lowercase (locale independent; no
// normalization or trimming). Every input must be valid UTF-8 of at most 128
// bytes; missing fields are empty. An empty query matches every valid input;
// malformed fields still report error.
FriendSearchResult friend_search(const Utf8View& query, const Utf8View& username,
    const Utf8View& nickname, const Utf8View& display_name) noexcept;
// A query folded once for friend_search over many names. `valid` is false for a query that friend_search
// reports as invalid_query.
struct FriendSearchQuery {
    u32 scalars[kFriendSearchMaximumBytes];
    u32 count;
    bool valid;
};
bool friend_search_query(const Utf8View& query, FriendSearchQuery* output) noexcept;
FriendSearchResult friend_search_folded(const FriendSearchQuery& query, const Utf8View& username,
    const Utf8View& nickname, const Utf8View& display_name) noexcept;
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
