#pragma once

#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/thread_member_store.hpp"

namespace maicord {

inline constexpr u64 kMemberListOnlineGroup = 0;
inline constexpr u64 kMemberListOfflineGroup = ~0ull;
inline constexpr u64 kMemberListPrivateGroup = ~0ull - 1;

enum class MemberListRowKind : u8 {
    group,
    member,
};

struct MemberListRow {
    u64 user_id;
    u64 group_role_id;
    u32 member_slot;
    u32 role_color;
    u32 group_position;
    GuildMemberStatus status;
    MemberListRowKind kind;
    u16 member_generation;
    u32 logical_index;
    u32 group_count;
};

struct MemberListView {
    Slice<MemberListRow> rows;
    Slice<u32> member_scratch;
    u64 guild_id;
    u64 list_id;
    u32 count;
    u32 member_count;
    u32 revision;
    u32 source_member_revision;
    u32 source_user_revision;
    u32 source_permission_mutations;
    u32 rebuilds;
    u32 rejected;
    u32 resident_count;
    bool authoritative;
    bool awaiting_sync;
    const GuildMemberStore* source_members;
    const ThreadMemberStore* source_threads;
    u32 source_thread_revision;
    u32 source_thread_generation;
    u32 source_presence_revision;
    bool thread_mode;
    bool incomplete;
    // A presence change moved a sorted row between groups; rebuild next.
    bool presence_regroup;
};

struct MemberListPresenceResult {
    u32 updated;
    bool visible;
    bool regroup;
};

enum class MemberListViewError : u8 {
    none,
    invalid_input,
    capacity,
    corrupt_source,
};

struct MemberListViewResult {
    MemberListViewError error;
    u32 rows;
    u32 members;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == MemberListViewError::none;
    }
};

bool member_list_view_initialize(
    MemberListView* projection,
    Slice<MemberListRow> rows, Slice<u32> member_scratch) noexcept;
MemberListViewResult member_list_view_rebuild(
    MemberListView* projection,
    const GuildMemberStore& members, const UserStore& users,
    const ChatStateStore& state, u64 guild_id, u64 list_id) noexcept;
// Preserve selected-list scroll geometry after fresh READY while all rows are
// unknown. The next authoritative group snapshot ends this temporary hint.
bool member_list_view_await_sync(MemberListView* projection,
    u64 guild_id, u64 list_id, u32 logical_count) noexcept;
// Applies status and role-color member changes (presence_revision) to
// resident rows in place. The revision is published only when an updated row
// lies in [visible_first, visible_first + visible_count); a row whose sorted
// group changes requests a rebuild instead.
MemberListPresenceResult member_list_view_refresh_presence(
    MemberListView* projection, const GuildMemberStore& members,
    u32 visible_first, u32 visible_count) noexcept;
u32 member_list_view_count(const MemberListView& projection) noexcept;
const MemberListRow* member_list_view_row(
    const MemberListView& projection, u32 logical_index) noexcept;
Utf8View member_list_row_label(
    const MemberListRow& row, const GuildMemberStore& members,
    const UserStore& users, const ChatStateStore& state) noexcept;
MemberListViewResult member_list_view_private(
    MemberListView* projection, const UserStore& users,
    const ChatStateStore& state, u64 channel_id) noexcept;
MemberListViewResult member_list_view_thread(
    MemberListView* projection, const ThreadMemberStore& threads,
    const GuildMemberStore& members, const UserStore& users,
    const ChatStateStore& state, u64 guild_id, u64 thread_id) noexcept;

} // namespace maicord
