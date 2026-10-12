#include "maicord/model/member_list.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/heap_sort.hpp"

namespace maicord {
namespace {
const GuildMemberRecord* profile(const ThreadMemberStore& threads,
    const GuildMemberStore& members, u64 user_id) noexcept {
    return guild_member_store_find(members, threads.guild_id, user_id);
}
u64 group(const GuildMemberRecord* p) noexcept {
    return !p || p->status == GuildMemberStatus::offline || p->status == GuildMemberStatus::unknown
        ? kMemberListOfflineGroup : p->group_role_id;
}
Utf8View label(const GuildMemberRecord* p, const UserStore& users, const UserRecord& user) noexcept {
    const auto nick = p ? guild_member_store_nickname(*p) : Utf8View{};
    if (nick.bytes) return nick;
    return user_store_name(users, user);
}
bool less(const ThreadMemberStore& threads, const GuildMemberStore& members,
    const UserStore& users, u32 a, u32 b) noexcept {
    const auto& left_user = users.records[a]; const auto& right_user = users.records[b];
    const auto* left = profile(threads, members, left_user.id); const auto* right = profile(threads, members, right_user.id);
    const u64 lg = group(left), rg = group(right);
    if (lg != rg) {
        if (lg == kMemberListOfflineGroup) return false;
        if (rg == kMemberListOfflineGroup) return true;
        if (lg == kMemberListOnlineGroup) return false;
        if (rg == kMemberListOnlineGroup) return true;
        if (left->group_position != right->group_position) return left->group_position > right->group_position;
        return lg > rg;
    }
    const auto ln = label(left, users, left_user), rn = label(right, users, right_user);
    for (u32 i = 0; i < minimum(ln.bytes, rn.bytes); ++i) {
        const auto lc = ascii_lower(static_cast<u8>(ln.data[i])), rc = ascii_lower(static_cast<u8>(rn.data[i]));
        if (lc != rc) return lc < rc;
    }
    if (ln.bytes != rn.bytes) return ln.bytes < rn.bytes;
    return left_user.id < right_user.id;
}
}
MemberListViewResult member_list_view_thread(MemberListView* p,
    const ThreadMemberStore& threads, const GuildMemberStore& members,
    const UserStore& users, const ChatStateStore& state, u64 guild_id, u64 thread_id) noexcept {
    if (!p || !p->rows.data || !p->rows.count || !p->member_scratch.data || !p->member_scratch.count ||
        !members.records || !users.records || threads.active_bank > 1 || !threads.banks[threads.active_bank].data ||
        threads.count > threads.banks[threads.active_bank].count || !thread_member_store_owns(threads, guild_id, thread_id)) return {MemberListViewError::invalid_input, 0, 0, false};
    if (p->thread_mode && !p->presence_regroup && p->guild_id == guild_id && p->list_id == thread_id && p->source_threads == &threads &&
        p->source_thread_revision == threads.revision && p->source_thread_generation == threads.generation &&
        p->source_member_revision == members.revision && p->source_user_revision == users.revision &&
        p->source_permission_mutations == state.permission_mutations) return {MemberListViewError::none, p->count, p->member_count, false};
    p->presence_regroup = false; p->source_presence_revision = members.presence_revision;
    u32 known = 0; bool incomplete = threads.incomplete;
    if (threads.known) for (u32 i = 0; i < threads.count; ++i) {
        const auto* user = user_store_find(users, threads.banks[threads.active_bank][i]);
        if (!user || !user_store_username(users, *user).bytes) continue;
        if (known == p->member_scratch.count) { incomplete = true; continue; }
        // The stores stay unchanged through this sort and publication pass.
        p->member_scratch[known++] = static_cast<u32>(user - users.records);
    }
    heap_sort(p->member_scratch.data, known, [&](u32 a, u32 b) noexcept { return less(threads, members, users, a, b); });
    u32 rows = 0; u64 previous = 0; u32 group_row = 0;
    for (u32 i = 0; i < known; ++i) {
        const auto& user = users.records[p->member_scratch[i]]; const auto* member = profile(threads, members, user.id);
        const u64 current = group(member); const bool new_group = !i || current != previous;
        if ((new_group ? 2u : 1u) > p->rows.count - rows) { incomplete = true; break; }
        if (new_group) {
            group_row = rows;
            p->rows[rows++] = {0, current, ~0u, 0, member ? member->group_position : 0, GuildMemberStatus::unknown, MemberListRowKind::group};
            previous = current;
        }
        p->rows[rows] = {user.id, current,
            member ? static_cast<u32>(member - members.records) : ~0u, member ? member->role_color : 0,
            member ? member->group_position : 0, member ? member->status : GuildMemberStatus::unknown,
            MemberListRowKind::member, member ? member->generation : static_cast<u16>(0), rows, 0};
        ++rows; ++p->rows[group_row].group_count;
    }
    p->guild_id = guild_id; p->list_id = thread_id; p->count = rows; p->resident_count = rows;
    p->member_count = threads.known ? threads.count : 0;
    p->authoritative = false; p->awaiting_sync = !threads.known; p->thread_mode = true; p->incomplete = incomplete;
    p->source_threads = &threads; p->source_thread_revision = threads.revision; p->source_thread_generation = threads.generation;
    p->source_members = &members; p->source_member_revision = members.revision; p->source_user_revision = users.revision;
    p->source_permission_mutations = state.permission_mutations; saturating_increment(p->revision); saturating_increment(p->rebuilds);
    return {MemberListViewError::none, rows, p->member_count, true};
}
} // namespace maicord
