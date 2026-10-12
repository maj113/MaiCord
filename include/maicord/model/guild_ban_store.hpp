#pragma once

#include "maicord/protocol/guild_admin.hpp"

namespace maicord {

// Session cache of one guild's ban list, filled from REST pages and kept
// current by GUILD_BAN_ADD/REMOVE. Records stay sorted by user ID, which is
// also the service's paging order.
struct GuildBanRecord {
    u64 user_id;
    u16 name_bytes;
    u16 reason_bytes;
    char name[kGuildAdminUsernameBytes];
    char reason[kGuildAdminReasonBytes];
};

struct GuildBanStore {
    GuildBanRecord* records;
    u32 capacity;
    u32 count;
    u32 revision;
    u64 guild_id;
    // Every page has been loaded; otherwise ban events only touch resident rows
    // and additions beyond the last loaded ID wait for the next page.
    bool complete;
};

bool guild_ban_store_bind(GuildBanStore* store, Slice<GuildBanRecord> records) noexcept;
// Drops all rows and starts caching another guild (zero detaches).
void guild_ban_store_reset(GuildBanStore* store, u64 guild_id) noexcept;
// The next page continues after the highest resident user ID.
u64 guild_ban_store_next_after(const GuildBanStore& store) noexcept;
// Rejects foreign guilds, pages that do not continue the resident prefix and
// overflow, leaving the store unchanged.
bool guild_ban_store_append_page(GuildBanStore* store, u64 guild_id, ConstSlice<GuildAdminBan> page,
                                 bool last_page) noexcept;
bool guild_ban_store_apply_add(GuildBanStore* store, u64 guild_id, u64 user_id, Utf8View name) noexcept;
bool guild_ban_store_apply_remove(GuildBanStore* store, u64 guild_id, u64 user_id) noexcept;
const GuildBanRecord* guild_ban_store_find(const GuildBanStore& store, u64 user_id) noexcept;

} // namespace maicord
