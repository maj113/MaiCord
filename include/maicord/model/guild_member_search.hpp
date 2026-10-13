#pragma once

#include "maicord/protocol/guild_moderation.hpp"

namespace maicord {

// The Members page's search state, after the official member safety store:
// results accumulate per filter set and pages are cut from them locally;
// a page change asks for a new chunk only when the page or the next one
// crosses a chunk boundary. Pure; the application owns transport.
inline constexpr u32 kGuildMemberSearchPageSizes[]{12, 25, 50, 100};
inline constexpr u32 kGuildMemberSearchCapacity = 2'000;

enum class GuildMemberSearchMode : u8 {
    // {limit}: no cursor (also the first page and a reset).
    first,
    // The last request's cursor as after (normally a repeat, so nothing is sent).
    repeat,
    // The latest reply's last member, moving forward into the next chunk.
    after,
    // The latest reply's first member, moving back into an earlier chunk.
    before,
};
// page and previous_page are 1-based; previous_page is the page of the last
// request (0 for none).
GuildMemberSearchMode guild_member_search_mode(u32 page, u32 previous_page, u32 page_size,
                                               bool cursors_known) noexcept;

enum class GuildMemberSearchPhase : u8 {
    idle,
    loading,
    // HTTP 202: the server is indexing; a retry waits for retry_at_ms.
    indexing,
    loaded,
    failed,
};

struct GuildMemberSearchStore {
    GuildSearchMember* members;
    u32 capacity, count;
    u32 total;
    bool total_known;
    u32 page, page_size;
    // The latest reply's first and last member.
    GuildMemberSearchCursor before, after;
    bool cursors_known;
    // The last request: its body, page and cursor.
    u64 last_body_hash;
    u32 last_body_bytes;
    u32 last_page;
    GuildMemberSearchCursor last_cursor;
    u32 retries;
    u64 retry_at_ms;
    GuildMemberSearchPhase phase;
};

bool guild_member_search_bind(GuildMemberSearchStore*, Slice<GuildSearchMember> members) noexcept;
// A new filter set: no results, page 1, and the previous request forgotten
// only when `forget` (opening the page; a filter change keeps it).
void guild_member_search_reset(GuildMemberSearchStore*, bool forget) noexcept;
// The cursor and limit of the current page's request.
void guild_member_search_plan(const GuildMemberSearchStore&, GuildMemberSearchQuery* query) noexcept;
// Whether a planned body (its hash and size) differs from the last request,
// and records it as sent when it does.
bool guild_member_search_begin(GuildMemberSearchStore*, const GuildMemberSearchQuery& planned, u64 body_hash,
                               u32 body_bytes) noexcept;
// A 200 reply: members are added (known users replaced) and the reply's
// first and last member become the cursors.
void guild_member_search_apply(GuildMemberSearchStore*, const GuildMemberSearchPage& page) noexcept;
// A 202 reply: true while retries remain (the same body is sent again at
// retry_at_ms); after the third retry the search fails.
bool guild_member_search_indexing(GuildMemberSearchStore*, u64 retry_after_ms, u64 now_ms) noexcept;
void guild_member_search_failed(GuildMemberSearchStore*) noexcept;
// The members of the current page in the sort order (0 and 1 joined newest
// first, 2 joined oldest first, 3 account newest, 4 account oldest).
// Returns the number written.
u32 guild_member_search_page_rows(const GuildMemberSearchStore&, u8 sort, Slice<u32> scratch,
                                  Slice<const GuildSearchMember*> output) noexcept;
// The count the footer shows: the server's total unless it is unknown or
// below one chunk, then the members held.
u32 guild_member_search_count(const GuildMemberSearchStore&) noexcept;
// Pages for the count (at least 1).
u32 guild_member_search_pages(const GuildMemberSearchStore&) noexcept;
// A new page size keeps the first member of the current page in view.
void guild_member_search_set_page_size(GuildMemberSearchStore*, u32 page_size) noexcept;

} // namespace maicord
