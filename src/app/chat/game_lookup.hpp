#pragma once
#include "maicord/app/net/rest_client.hpp"
#include "maicord/model/chat/game_store.hpp"

namespace maicord::app_detail {
// The account's games (model/chat/game_store.hpp): game mentions, the
// composer's game search, the Server Profile's games and the game profile
// all read and fill one cache. Requests run on the details lane
// (MessageHistoryRequestOwner::game_lookup) through the guild admin
// endpoint's game routes: GET /games?game_ids= for wanted games (at most 20
// per batch, as the official client batches them), GET /games/autocomplete
// for a search, and the game profile's GET /games?game_ids={id}. The cache
// lives as long as the account's UI and is released with it.

// Null before the first game is asked for.
const GameStore* application_games() noexcept;
// The game's record, null when it was never asked for nor found by a search.
const GameRecord* game_lookup_find(u64 id) noexcept;
// Asks for a game; cheap to call every frame for every shown id.
void game_lookup_want(u64 id) noexcept;
// Changes whenever a game or a search result arrives (fold it into a repaint
// signature).
u32 game_lookup_revision() noexcept;
// Notes a search box's text after each edit. The query is sent with the
// official client's debounce (200 ms after the last edit, 500 ms at most
// after the previous send; the first edit at once).
void game_lookup_search(Utf8View typed) noexcept;
struct GameSearchResults {
    const u64* ids;
    u32 count;
    // The typed query's own results are not in yet: ids are the longest
    // cached prefix's.
    bool loading;
};
// The results for typed (resolve each id with game_lookup_find).
GameSearchResults game_lookup_results(Utf8View typed) noexcept;

// The game profile's request for id (the game object with its supplemental
// data); its body reaches application_game_profile_response. Replaces an
// earlier profile request.
void game_lookup_profile(u64 id) noexcept;
void game_lookup_profile_cancel() noexcept;

bool application_game_lookup_pending(u64 now) noexcept;
bool application_game_lookup_submit(u64 request_id, u64 now) noexcept;
bool application_game_lookup_response(const RestClientResponse& response, Utf8View body) noexcept;
u64 application_game_lookup_next_wake() noexcept;
void application_game_lookup_release() noexcept;
} // namespace maicord::app_detail
