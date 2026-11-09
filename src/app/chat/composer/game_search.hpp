#pragma once
#include "maicord/core/slice.hpp"

namespace maicord::app_detail {
// The composer's game search, the official client's @game inline input: typing
// "@game" and a space (or picking "@game" from the '@' list) starts a token,
// U+2064 "@game:" query U+2062 in the draft (model/chat/game_store.hpp), drawn
// as an "@game" chip around the query. While the caret is in it the query
// searches Discord's games (chat/game_lookup.hpp) and "Games matching @query"
// lists them; picking one replaces the token with <@$id>. Deleting the head
// or the end removes the token; at send an unpicked token reads "@query".

// Each composer sync, before the '@' list: repairs a half-deleted token, starts
// one for "@game" followed by a typed space, and while the caret is in a token
// shows its results (handled: the composer's other lists stay closed).
bool application_game_search_sync(bool* handled) noexcept;
// The '@' list's "@game" row was picked: the caret goes into the new token.
bool application_game_search_started() noexcept;
// A results row (popup_action_game_first + row) was picked.
bool application_game_search_pick(u32 row) noexcept;
// The icon URL of a results row's picture (its key's id), into url; 0 when none.
u32 application_game_search_icon_url(u64 media_id, Slice<char> url) noexcept;
} // namespace maicord::app_detail
