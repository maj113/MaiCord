#pragma once

#include "maicord/protocol/guild/admin.hpp"

namespace maicord {

// Games named by id (game mentions <@$id>, the Server Profile's games) and the
// game search's results, as the official client caches them: games come from
// GET /games?game_ids= in batches of at most 20, an id the response leaves out
// is unknown, a failed batch is asked again after 15 s; search results are
// kept per normalized query for an hour (a failed search for a minute), and
// while a query is fetched the longest cached prefix's results show.
inline constexpr u32 kGameStoreCapacity = 128;
inline constexpr u32 kGameLookupBatchMaximum = 20;
inline constexpr u64 kGameLookupFailureRetryMs = 15'000;
inline constexpr u32 kGameSearchCacheCapacity = 32;
inline constexpr u32 kGameSearchResultMaximum = 25;
inline constexpr u32 kGameSearchQueryBytes = kGuildAdminGameQueryCodepoints * 4;
inline constexpr u64 kGameSearchStaleMs = 3'600'000;
inline constexpr u64 kGameSearchFailureStaleMs = 60'000;
// The search's debounce: a typed query is sent 200 ms after the last edit,
// and at most 500 ms after the previous send.
inline constexpr u64 kGameSearchDebounceMs = 200;
inline constexpr u64 kGameSearchMaximumWaitMs = 500;

enum class GameState : u8 {
    wanted,  // asked for, not yet sent
    pending, // in a sent batch
    known,
    unknown, // the server has no such game
    failed,  // the batch failed; asked again from retry_at
};

struct GameRecord {
    GuildAdminGame game;
    u64 retry_at;
    u64 used;
    GameState state;
    // The game object itself (not only a search result's name and icon).
    bool full;
    // A search result's one fetch of its game object (the cover): 1 wanted,
    // 2 sent, 3 done.
    u8 upgrade;
};

struct GameSearchEntry {
    u64 stored_at;
    u64 used;
    u64 ids[kGameSearchResultMaximum];
    u8 count;
    u8 query_bytes;
    bool pending;
    bool failed;
    char query[kGameSearchQueryBytes];
};

struct GameStore {
    Slice<GameRecord> records;
    Slice<GameSearchEntry> searches;
    u32 count;
    u32 search_count;
    // Changes whenever a record or a search result changes.
    u32 revision;
    u64 clock;
};

bool game_store_initialize(GameStore* store, Slice<GameRecord> records, Slice<GameSearchEntry> searches) noexcept;
const GameRecord* game_store_find(const GameStore& store, u64 id) noexcept;
// The known game's name, empty for any other state.
Utf8View game_store_name(const GameStore& store, u64 id) noexcept;
// Asks for a game: true when it is wanted now (new, or failed and due).
bool game_store_want(GameStore* store, u64 id, u64 now_ms) noexcept;
// True when a wanted game is waiting for a batch.
bool game_store_batch_due(const GameStore& store) noexcept;
// Moves up to ids.count wanted games to pending and lists them; returns how many (the caller sizes ids to at most
// kGameLookupBatchMaximum).
u32 game_store_take_batch(GameStore* store, Slice<u64> ids) noexcept;
// A game object (full) or a search result: a search result never replaces a
// game object's data.
void game_store_put(GameStore* store, const GuildAdminGame& game, bool full) noexcept;
// A batch answered (ok: ids it left out are unknown) or failed (retried later).
void game_store_settle(GameStore* store, ConstSlice<u64> asked, bool ok, u64 now_ms) noexcept;
// A batch that could not be sent: its games are wanted again.
void game_store_return(GameStore* store, ConstSlice<u64> asked) noexcept;

// Search. queries are normalized (guild_admin_game_query).
const GameSearchEntry* game_store_search_find(const GameStore& store, Utf8View query, u64 now_ms) noexcept;
// The longest cached prefix of query (query itself included) with results.
const GameSearchEntry* game_store_search_closest(const GameStore& store, Utf8View query, u64 now_ms) noexcept;
// The official client sends no search whose longest cached shorter prefix
// found nothing when that prefix has seven or more characters.
bool game_store_search_suppressed(const GameStore& store, Utf8View query, u64 now_ms) noexcept;
// Marks query as being fetched; false when there is nothing to fetch.
bool game_store_search_begin(GameStore* store, Utf8View query, u64 now_ms) noexcept;
// Results of a search (failed: none, kept a minute); ids beyond the cap drop.
void game_store_search_finish(GameStore* store, Utf8View query, ConstSlice<u64> ids, bool failed,
                              u64 now_ms) noexcept;
// Ids the official client never lists in search results.
bool game_store_search_excluded(u64 id) noexcept;

// Writes the game mention <@$id>; returns the bytes (0 when it does not fit).
u32 game_mention_write(u64 id, Slice<char> output) noexcept;

// The composer's game search token (the official client's @game inline
// input): U+2064 "@game:" query U+2062 in the draft. The marks never leave the
// composer: a token still unresolved at send reads "@query", as the official
// client sends it.
inline constexpr char kGameTokenHead[] = "\xE2\x81\xA4@game:";
inline constexpr u32 kGameTokenHeadBytes = sizeof(kGameTokenHead) - 1;
inline constexpr char kGameTokenEnd[] = "\xE2\x81\xA2";
inline constexpr u32 kGameTokenEndBytes = sizeof(kGameTokenEnd) - 1;
struct GameToken {
    u32 first;       // the head
    u32 query_first; // after the head
    u32 query_last;  // the end mark
    u32 last;        // after the end mark
};
// The token whose head starts at offset (its query on one line).
bool game_token_at(Utf8View text, u32 offset, GameToken* token) noexcept;
// The token whose query holds the caret.
bool game_token_around(Utf8View text, u32 caret, GameToken* token) noexcept;
// text with every token as "@query" (flatten) or only without the stray marks
// a deleted head or end leaves; false (output untouched) when nothing changes.
// offset (optional) is a position in text, moved to the same place in output.
bool game_tokens_rewrite(Utf8View text, bool flatten, Slice<char> output, u32* bytes,
                         u32* offset = nullptr) noexcept;

} // namespace maicord
