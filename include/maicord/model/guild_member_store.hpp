#pragma once

#include "maicord/model/chat_state.hpp"

namespace maicord {

inline constexpr u32 kGuildMemberNicknameMaximumBytes = 128;
// Assigned role IDs retained inline for permission and hierarchy checks.
// Members with more roles keep only derived display data and need a fetch.
inline constexpr u32 kGuildMemberRoleIdCapacity = 16;

enum class GuildMemberStatus : u8 {
    unknown,
    online,
    idle,
    dnd,
    offline,
};

struct GuildMemberRecord {
    u64 guild_id;
    u64 list_id;
    u64 user_id;
    u64 group_role_id;
    u64 color_role_id;
    u32 role_color;
    u32 group_position;
    u32 list_index;
    u32 last_seen;
    u32 revision;
    u16 nickname_bytes;
    u16 generation;
    u16 role_count;
    GuildMemberStatus status;
    bool roles_known;
    bool occupied;
    bool truncated_roles;
    char nickname[kGuildMemberNicknameMaximumBytes];
    bool visible_pinned;
    u64 role_ids[kGuildMemberRoleIdCapacity];
    // Unix seconds; zero when not timed out.
    u32 communication_disabled_until;
    bool role_ids_complete;
    bool communication_disabled_known;
};

inline constexpr u32 kMemberListMaximumScopes = 8;
inline constexpr u32 kMemberListMaximumGroups = 256;
struct MemberListGroupRecord {
    u64 id;
    u32 index;
    u32 count;
    bool resident;
    bool summary_seen;
    u8 scope_slot;
    u32 last_seen;
};
struct MemberListScope {
    u64 guild_id;
    u64 list_id;
    u32 logical_count;
    u32 member_count;
    u32 online_count;
    u32 group_count;
    u32 last_seen;
    bool member_count_known;
    bool online_count_known;
    bool groups_known;
    bool groups_pending;
    bool authoritative;
    // Store clock when the list was last shown (visible range or pinned pages).
    u32 viewed;
};
// Every member list keeps its own rows, as the official client keeps one row array per
// list id: a list that stays subscribed in the background never takes rows
// from the list on screen, and a list shown again presents what it retained.
// A position names the user (an evicted member record is fetched again); the
// pool is shared and bounded, each list holds at most a page budget, and when
// the pool is full the least recently viewed list gives up its rows.
inline constexpr u32 kMemberListPositionCapacity = 2'048;
inline constexpr u32 kMemberListPositionsPerList = 512;
struct MemberListPosition {
    u64 user_id;
    u32 index;
    u8 scope_slot;
    bool occupied;
};
struct MemberListPositionTable {
    MemberListPosition entries[kMemberListPositionCapacity];
    u32 count;
    u32 evicted_lists;
};
struct MemberListPinnedRange {
    u64 guild_id;
    u64 list_id;
    u32 first;
    u32 last;
};
struct GuildMemberStore {
    GuildMemberRecord* records;
    u32* order;
    u32 capacity;
    u32 count;
    u32 clock;
    u32 revision;
    u32 mutations;
    u32 evictions;
    u32 rejected;
    // Status-only changes. Structural consumers key on revision; status is
    // projected in place (member_list_view_refresh_presence).
    u32 presence_revision;
    MemberListScope lists[kMemberListMaximumScopes];
    MemberListGroupRecord groups[kMemberListMaximumGroups];
    MemberListPinnedRange pins[5];
    u32 pin_count;
    // Advances whenever every member's visible pin is cleared (fills padding).
    u32 user_pin_epoch;
    MemberListPinnedRange visible;
    MemberListPositionTable positions;
};

enum class GuildMemberStoreError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    nickname_too_large,
    capacity,
};

struct GuildMemberStoreResult {
    const GuildMemberRecord* record;
    GuildMemberStoreError error;
    bool inserted;
    bool changed;
    bool evicted;

    constexpr bool ok() const noexcept {
        return error == GuildMemberStoreError::none;
    }
};

bool guild_member_store_initialize(GuildMemberStore* store,
                                           Slice<GuildMemberRecord> records,
                                           Slice<u32> order) noexcept;
// Fresh READY withdraws Gateway list positions while retaining useful cached
// guild identities, profile data, resolved roles, and presence. Resume retains
// list residency and must not invoke this reset.
void guild_member_store_reset_lists(GuildMemberStore* store) noexcept;
// A fresh Gateway session cannot retain assignment authority from a missed gap.
// RESUMED retains ordered dispatch authority and must not call this.
void guild_member_store_retire_role_authority(GuildMemberStore*) noexcept;
// Called after role metadata commits. Without retained assigned-role IDs,
// hierarchy/fallback changes require fresh member roles; returns that need.
// A nonzero color-only edit at unchanged rank can update the known winner.
bool guild_member_store_observe_role(GuildMemberStore* store, const ChatStateStore& state,
                                             u64 guild_id, u64 role_id,
                                             const RoleRecord* previous) noexcept;
GuildMemberStoreResult guild_member_store_upsert(
    GuildMemberStore* store, const ChatStateStore& state, u64 guild_id, u64 user_id,
    Utf8View nickname, ConstSlice<u64> role_ids, bool roles_known, GuildMemberStatus status,
    u32 list_index = ~0u, u64 list_id = 0) noexcept;
struct GuildMemberInPlaceResult {
    bool applied;       // the row stays at list_index in its sorted group
    bool changed;       // a displayed field changed (published via presence_revision)
    bool label_changed; // nickname changed
};
// Lazy-list UPDATE of the member already resident at list_index whose sorted
// group (role group, position, offline/online) is unchanged. Changes publish
// presence_revision (in-place row refresh), not the structural revision.
// Not applied when the row moved or changes group; use invalidate + upsert.
GuildMemberInPlaceResult guild_member_store_update_in_place(
    GuildMemberStore* store, const ChatStateStore& state, u64 guild_id, u64 list_id, u32 list_index,
    u64 user_id, Utf8View nickname, ConstSlice<u64> role_ids, bool roles_known,
    GuildMemberStatus status) noexcept;
const GuildMemberRecord* guild_member_store_find(const GuildMemberStore& store,
                                                                u64 guild_id, u64 user_id) noexcept;
// Presence-only update of an existing guild member; never erases profile,
// roles or member-list membership. Uncached users are a bounded no-op.
bool guild_member_store_set_status(GuildMemberStore* store, u64 guild_id, u64 user_id,
                                           GuildMemberStatus status) noexcept;
Utf8View guild_member_store_nickname(const GuildMemberRecord& record) noexcept;
// Complete current assigned roles, or an empty slice when unknown/truncated.
bool guild_member_store_role_ids_known(const GuildMemberRecord& record) noexcept;
ConstSlice<u64> guild_member_store_role_ids(const GuildMemberRecord& record) noexcept;
// Records an observed communication_disabled_until (zero clears). Uncached
// members are a no-op that returns false.
bool guild_member_store_set_timeout(GuildMemberStore* store, u64 guild_id, u64 user_id,
                                            u64 until_seconds) noexcept;
bool guild_member_store_remove(GuildMemberStore* store, u64 guild_id, u64 user_id) noexcept;
// A lazy-member-list INVALIDATE/DELETE withdraws positional knowledge; it
// does not mean that the user left the guild. Preserve the cached identity,
// roles, color, and presence until a later SYNC/UPDATE refreshes the row.
bool guild_member_store_invalidate_list_range(GuildMemberStore* store, u64 guild_id,
                                                      u32 first, u32 last, u64 list_id = 0) noexcept;
struct UserStore;
void guild_member_store_clear_visible_range(GuildMemberStore* store) noexcept;
bool guild_member_store_set_visible_range(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                                  u32 first, u32 last) noexcept;
bool guild_member_store_visible_index(const GuildMemberStore& store, u64 guild_id,
                                              u64 list_id, u32 index) noexcept;
u32 guild_member_store_refresh_visible_users(const GuildMemberStore& members,
                                                     UserStore* users) noexcept;
void guild_member_store_clear_pins(GuildMemberStore* store) noexcept;
void guild_member_store_clear_user_pins(GuildMemberStore* store) noexcept;
// Drops only the positional range pins; members' visible pins are kept.
inline void guild_member_store_clear_range_pins(GuildMemberStore* store) noexcept {
    if (store)
        store->pin_count = 0;
}
// Caller bounds admissions to its materialized viewport (128 native rows).
bool guild_member_store_pin_user(GuildMemberStore* store, u64 guild_id,
                                         u64 user_id) noexcept;
bool guild_member_store_pin_range(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                          u32 first, u32 last) noexcept;
// Call only after validating an entire Gateway packet. Positional knowledge
// remains bounded independently of the reported logical member-list size.
const MemberListScope* guild_member_store_list(const GuildMemberStore& store,
                                                              u64 guild_id, u64 list_id) noexcept;
// The user a list holds at a position (zero when it holds none there).
u64 guild_member_store_list_user(const GuildMemberStore& store, u64 guild_id, u64 list_id,
                                 u32 index) noexcept;
// Users a list holds in [first, last]; returns how many were written.
u32 guild_member_store_list_users(const GuildMemberStore& store, u64 guild_id, u64 list_id, u32 first,
                                  u32 last, Slice<u64> output) noexcept;
u32 guild_member_store_list_position_count(const GuildMemberStore& store, u64 guild_id,
                                           u64 list_id) noexcept;
bool guild_member_store_observe_list(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                             u32 member_count, u32 online_count, bool members_known,
                                             bool online_known, bool groups_known, u32 group_count,
                                             u32 logical_count) noexcept;
bool guild_member_store_group_summary(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                              u32 index, u64 group_id, u32 count) noexcept;
bool guild_member_store_set_group(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                          u32 index, u64 group_id, u32 count,
                                          bool count_known = true) noexcept;
void guild_member_store_finish_groups(GuildMemberStore* store, u64 guild_id,
                                              u64 list_id) noexcept;
bool guild_member_store_shift_list(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                           u32 index, bool insert) noexcept;
void guild_member_store_extend_list(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                            u32 index) noexcept;

} // namespace maicord
