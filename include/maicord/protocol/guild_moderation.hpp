#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {

// Member safety requests of Server Settings: the members search, bulk bans
// and the server-side ban search. One request is one REST call on the
// DETAILS lane; targets and bodies are built here so the client, the module
// and the tests share one serializer. Views are borrowed for the call only.
enum class GuildModerationOperation : u8 {
    members_search, // POST /guilds/{g}/members-search
    bulk_ban,       // POST /guilds/{g}/bulk-ban/v2   {"user_ids","delete_message_seconds"}
    ban_search,     // GET  /guilds/{g}/bans/search?limit=10[&user_ids=..][&query=..]
    count,
};

inline constexpr u64 kGuildModerationRouteBase = 0x474D4F4400000000ull;
// A ban search: the path, its IDs and a query at the encoded reason bound.
inline constexpr u32 kGuildModerationMaximumTargetBytes = 128 + kRestAuditReasonMaximumEncodedBytes;
inline constexpr u32 kGuildModerationMaximumBodyBytes = 32 * 1024;
// A chunk of the members table is max(5 x page size, 250) members.
inline constexpr u32 kGuildMemberSearchChunkMinimum = 250;
inline constexpr u32 kGuildMemberSearchLimitMaximum = 1'000;
// The search box text, and a ban search's.
inline constexpr u32 kGuildMemberSearchTextBytes = 400;
inline constexpr u32 kGuildMemberSearchRoleMaximum = 250;
inline constexpr u32 kGuildMemberSearchInviteCodeBytes = 32;
// HTTP 202 ("still indexing") is retried this many times, then it fails.
inline constexpr u32 kGuildMemberSearchRetryMaximum = 3;
inline constexpr u32 kGuildBanSearchLimit = 10;
// The API takes at most 200 user IDs per bulk ban.
inline constexpr u32 kGuildBulkBanMaximumUsers = 200;
inline constexpr u32 kGuildModerationBanDeleteMaximumSeconds = 604'800;
// unusual_dm_activity_until counts the last two days.
inline constexpr u64 kGuildMemberSignalDmWindowMs = 2ull * 24 * 60 * 60 * 1000;

// Signals are ORed under or_query.safety_signals.
enum GuildMemberSearchSignal : u8 {
    guild_member_signal_unusual_dm = 1u << 0,
    guild_member_signal_timed_out = 1u << 1,
    guild_member_signal_unusual_account = 1u << 2,
    guild_member_signal_quarantined = 1u << 3,
    guild_member_signal_all = (1u << 4) - 1,
};

// Sort: zero leaves it out; 1 joined newest first, 2 joined oldest first,
// 3 account newest first, 4 account oldest first.
inline constexpr u8 kGuildMemberSearchSortMaximum = 4;
// Join source types: 1 bot, 2 integration, 3 discovery, 4 hub, 5 invite,
// 6 vanity URL, 7 manual verification, 8 linked channel.
inline constexpr u8 kGuildJoinSourceTypeMaximum = 8;

// A chunk boundary: a member's joined time (Unix ms) and user ID.
struct GuildMemberSearchCursor {
    u64 guild_joined_at_ms;
    u64 user_id;
};

// The members table's filters. Text is the search box, trimmed: split on
// commas, each piece trimmed, 17-19 digit pieces to user_id and the rest to
// usernames; used only when longer than one character. Times are Unix
// milliseconds: a zero lower bound leaves its range out (the upper bound
// alone is not sent) and a zero upper bound leaves lte out.
struct GuildMemberSearchQuery {
    Utf8View text;
    // Selected roles, in selection order.
    ConstSlice<u64> role_ids;
    u64 joined_after_ms, joined_before_ms;
    u64 created_after_ms, created_before_ms;
    // The client clock for the signal windows.
    u64 now_ms;
    Utf8View invite_code;
    // At most one; a zero joined time means none.
    GuildMemberSearchCursor after, before;
    u32 limit;
    u8 join_source_type;
    bool join_source_type_set;
    u8 sort;
    u8 signals;
};

struct GuildModerationRequest {
    GuildModerationOperation operation;
    u8 reserved[3];
    // bulk_ban only.
    u32 delete_message_seconds;
    u64 guild_id;
    const GuildMemberSearchQuery* search;
    // bulk_ban: the users, in selection order.
    ConstSlice<u64> user_ids;
    // ban_search: the search box text, parsed like the members search's.
    Utf8View query;
    // bulk_ban: X-Audit-Log-Reason. A null view sends nothing; any other is
    // sent, even empty.
    Utf8View reason;
};
static_assert(sizeof(GuildModerationRequest) == 72);

enum class GuildModerationError : u8 {
    none,
    input,
    capacity,
    json,
    duplicate,
    field,
    identity,
};

struct GuildModerationResult {
    u32 bytes;
    u32 count;
    GuildModerationError error;
    constexpr bool ok() const noexcept {
        return error == GuildModerationError::none;
    }
};

RestMethod guild_moderation_method(GuildModerationOperation) noexcept;
bool guild_moderation_request_valid(const GuildModerationRequest&) noexcept;
RestRouteKey guild_moderation_route(const GuildModerationRequest&) noexcept;
GuildModerationResult guild_moderation_write_target(const GuildModerationRequest&, Slice<u8> output) noexcept;
// Zero bytes for ban_search. Output is cleared on failure.
GuildModerationResult guild_moderation_write_body(const GuildModerationRequest&, Slice<u8> output) noexcept;
// Encoded X-Audit-Log-Reason value (bulk_ban only); zero bytes when none.
GuildModerationResult guild_moderation_write_reason(const GuildModerationRequest&, Slice<u8> output) noexcept;
// A bulk ban carries the header, possibly with an empty value.
bool guild_moderation_reason_header(const GuildModerationRequest&) noexcept;
// The chunk size of a members table page size: max(5 x page size, 250).
constexpr u32 guild_member_search_limit(u32 page_size) noexcept {
    return page_size * 5 > kGuildMemberSearchChunkMinimum ? page_size * 5 : kGuildMemberSearchChunkMinimum;
}
// The service's snowflake test for search pieces (17 to 19 digits).
constexpr bool guild_member_search_snowflake(Utf8View text) noexcept {
    if (text.bytes < 17 || text.bytes > 19)
        return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (text.data[i] < '0' || text.data[i] > '9')
            return false;
    return true;
}
// The text without its leading and trailing ASCII white space, as String.prototype.trim does for the search inputs.
Utf8View guild_member_search_trim(Utf8View text) noexcept;
// The decimal snowflake of a Unix millisecond time ((ms - epoch) << 22, "0"
// at or before the epoch). Returns the digits written.
u32 guild_member_search_snowflake_at(u64 unix_ms, Slice<char> output) noexcept;

// Response models. Strings are owned and not NUL-terminated.
inline constexpr u32 kGuildModerationNameBytes = 128;
inline constexpr u32 kGuildModerationHashBytes = 34;
inline constexpr u32 kGuildModerationMemberRoles = 32;

struct GuildSearchMember {
    u64 user_id;
    // Unix ms; zero when absent.
    u64 joined_at_ms;
    u64 communication_disabled_until_ms;
    u64 unusual_dm_activity_until_ms;
    u64 inviter_id;
    u64 user_flags;
    u64 role_ids[kGuildModerationMemberRoles];
    u32 member_flags;
    // The member's roles, also those past the stored ones.
    u16 role_count;
    u16 discriminator;
    u8 username_bytes, global_name_bytes, nick_bytes, avatar_bytes, invite_code_bytes;
    // INVITE when a code is present but the type is absent or zero.
    u8 join_source_type;
    bool bot;
    bool pending;
    char username[kGuildModerationNameBytes];
    char global_name[kGuildModerationNameBytes];
    char nick[kGuildModerationNameBytes];
    char avatar[kGuildModerationHashBytes];
    char invite_code[kGuildMemberSearchInviteCodeBytes];
};

struct GuildMemberSearchPage {
    Slice<GuildSearchMember> members;
    u32 member_count;
    u32 total_result_count;
    bool total_known;
};
// A 200 reply. Rows beyond member_count are unspecified after failure.
GuildModerationResult guild_member_search_decode(Utf8View body, Slice<u32> scratch,
                                                 GuildMemberSearchPage* page) noexcept;
// A 202 reply's retry_after (seconds) in milliseconds; zero when absent.
u64 guild_member_search_retry_after_ms(Utf8View body, Slice<u32> scratch) noexcept;

// GUILD_BULK_BAN_UPDATE {guild_id, bulk_ban:{banned_users, failed_users}}.
struct GuildBulkBanUpdate {
    u64 guild_id;
    u32 banned_count, failed_count;
};
bool guild_bulk_ban_update_decode(Utf8View body, Slice<u32> scratch, GuildBulkBanUpdate* output) noexcept;

} // namespace maicord
