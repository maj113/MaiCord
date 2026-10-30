#pragma once

#include "maicord/core/stable_storage.hpp"
#include "maicord/model/chat_state.hpp"

namespace maicord {

inline constexpr u32 kGuildMemberNicknameMaximumBytes = 128;
// Assigned role IDs retained inline. A member with more keeps the whole list
// in the store's role overflow (guild_member_store_enable_role_overflow).
inline constexpr u32 kGuildMemberRoleIdCapacity = 16;
// Guilds allow 250 roles; a member's list never holds @everyone.
inline constexpr u32 kGuildMemberRoleMaximum = 256;
// Overflow compaction sorts at most this many member records.
inline constexpr u32 kGuildMemberRoleOverflowMembers = 1'024;

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
    // Every assigned role is retained (inline, or in the overflow past 16).
    bool role_ids_complete;
    char nickname[kGuildMemberNicknameMaximumBytes];
    bool visible_pinned;
    bool communication_disabled_known;
    // The first assigned roles; the overflow span holds the whole list.
    u64 role_ids[kGuildMemberRoleIdCapacity];
    // Unix seconds; zero when not timed out.
    u32 communication_disabled_until;
    // Overflow offset + 1 of the role_count IDs; zero when inline.
    u32 role_overflow;
};

// Contiguous role lists of members past the inline capacity, reserved on
// first use and committed in pages. Released spans are reused at the tail or
// by compaction, which also returns pages.
struct GuildMemberRoleOverflow {
    StableStorage storage;
    u32 limit; // reserved IDs; zero disables overflow
    u32 used;
    u32 wasted;
    u32 compactions;
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
// Each list keeps its own rows, so a background subscription never takes rows
// from the list on screen. A position names the user only (an evicted member
// record is fetched again). The pool is shared and bounded; when full, the
// least recently viewed list gives up its rows.
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
    // Every occupied slot lies below this high-water mark, so scans stop here
    // and never touch (or fault in) the unused tail of a zeroed record array.
    u32 slot_extent;
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
    GuildMemberRoleOverflow role_overflow;
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

// `zeroed` is for zero-filled (freshly committed) arrays: it leaves their
// contents unchanged, so record pages are touched only as slots fill.
bool guild_member_store_initialize(GuildMemberStore* store,
                                           Slice<GuildMemberRecord> records,
                                           Slice<u32> order, bool zeroed = false) noexcept;
// Keeps complete role lists past the inline 16 for up to `members_with_maximum`
// members holding every role; address space is reserved on first use. Without
// it such members keep their display data but not their role IDs.
bool guild_member_store_enable_role_overflow(GuildMemberStore* store, u32 members_with_maximum) noexcept;
// Releases the role overflow; the records stay.
void guild_member_store_release(GuildMemberStore* store) noexcept;
// Fresh READY withdraws Gateway list positions but keeps cached guild
// identities, profile data, resolved roles and presence. RESUMED keeps list
// residency and must not call this.
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
// Startup shell, before READY: a member's nickname and name colour as last
// shown, with roles unknown, so the first observation with roles replaces the
// colour and visible-author hydration still asks for the member.
bool guild_member_store_restore_display(GuildMemberStore* store, const ChatStateStore& state, u64 guild_id,
                                        u64 user_id, Utf8View nickname, u32 role_color) noexcept;
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
// True when the store holds the complete assigned role list.
bool guild_member_store_role_ids_known(const GuildMemberRecord& record) noexcept;
// Every assigned role, or an empty slice when unknown or incomplete. The view
// lasts until the next store mutation.
ConstSlice<u64> guild_member_store_role_ids(const GuildMemberStore& store, const GuildMemberRecord& record) noexcept;
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
// Users a list holds in [first, last]; returns how many were written.
u32 guild_member_store_list_users(const GuildMemberStore& store, u64 guild_id, u64 list_id, u32 first,
                                  u32 last, Slice<u64> output) noexcept;
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
