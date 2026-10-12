#include "maicord/model/member_list.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/model/role_policy.hpp"

namespace maicord {
namespace {

MemberListViewResult fail(MemberListView* projection,
                                       MemberListViewError error) noexcept {
    if (projection) saturating_increment(projection->rejected);
    return MemberListViewResult{error, projection ? projection->count : 0,
                                             projection ? projection->member_count : 0, false};
}

bool offline(const GuildMemberRecord& member) noexcept {
    return member.status == GuildMemberStatus::offline;
}

u64 group_id(const GuildMemberRecord& member) noexcept {
    if (offline(member)) {
        return kMemberListOfflineGroup;
    }
    return member.group_role_id;
}

int compare_text(Utf8View left, Utf8View right) noexcept {
    const u32 shared = minimum(left.bytes, right.bytes);
    for (u32 index = 0; index < shared; ++index) {
        const int a = ascii_lower(static_cast<u8>(left.data[index]));
        const int b = ascii_lower(static_cast<u8>(right.data[index]));
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    return left.bytes == right.bytes ? 0 : left.bytes < right.bytes ? -1 : 1;
}

Utf8View member_name(const GuildMemberRecord& member, const UserStore& users) noexcept {
    const Utf8View nickname = guild_member_store_nickname(member);
    if (nickname.bytes != 0) {
        return nickname;
    }
    const UserRecord* user = user_store_find(users, member.user_id);
    return user ? user_store_name(users, *user) : Utf8View{};
}

bool member_less(const GuildMemberStore& members, const UserStore& users, u32 left_slot,
                 u32 right_slot) noexcept {
    const GuildMemberRecord& left = members.records[left_slot];
    const GuildMemberRecord& right = members.records[right_slot];
    const u64 left_group = group_id(left);
    const u64 right_group = group_id(right);
    if (left_group != right_group) {
        if (left_group == kMemberListOfflineGroup) {
            return false;
        }
        if (right_group == kMemberListOfflineGroup) {
            return true;
        }
        if (left_group == kMemberListOnlineGroup) {
            return false;
        }
        if (right_group == kMemberListOnlineGroup) {
            return true;
        }
        return role_rank_before(left.guild_id, left.group_position, left_group, right.group_position,
                                right_group);
    }
    const int names = compare_text(member_name(left, users), member_name(right, users));
    return names != 0 ? names < 0 : left.user_id < right.user_id;
}

} // namespace

bool member_list_view_initialize(MemberListView* projection,
                                               Slice<MemberListRow> rows,
                                               Slice<u32> member_scratch) noexcept {
    if (!projection || !rows.data || rows.count == 0 || !member_scratch.data || member_scratch.count == 0 ||
        rows.count < member_scratch.count) {
        return false;
    }
    *projection = MemberListView{};
    projection->rows = rows;
    projection->member_scratch = member_scratch;
    return true;
}

MemberListViewResult member_list_view_rebuild(
    MemberListView* projection, const GuildMemberStore& members,
    const UserStore& users, const ChatStateStore& state, u64 guild_id, u64 list_id) noexcept {
    if (!projection || !projection->rows.data || !projection->member_scratch.data || !members.records ||
        !members.order || !users.records || !users.order || !state.guilds ||
        members.count > members.capacity) {
        return fail(projection, MemberListViewError::invalid_input);
    }
    if (!projection->thread_mode && !projection->presence_regroup && projection->guild_id == guild_id &&
        projection->list_id == list_id && projection->source_member_revision == members.revision &&
        projection->source_user_revision == users.revision &&
        projection->source_permission_mutations == state.permission_mutations) {
        return MemberListViewResult{MemberListViewError::none, projection->count,
                                                 projection->member_count, false};
    }
    projection->thread_mode = false; projection->source_threads = nullptr; projection->incomplete = false;
    projection->presence_regroup = false; projection->source_presence_revision = members.presence_revision;
    const auto* authoritative = guild_member_store_list(members, guild_id, list_id);
    if (authoritative && authoritative->authoritative) {
        u32 resident = 0, resident_members = 0;
        const auto scope = static_cast<u8>(authoritative - members.lists);
        // The list's own rows (another list of the guild cannot take them); a
        // row whose member record was evicted stays a placeholder until the
        // record is fetched again.
        for (const auto& position : members.positions.entries) {
            if (!position.occupied || position.scope_slot != scope || position.index >= authoritative->logical_count)
                continue;
            const auto* member = guild_member_store_find(members, guild_id, position.user_id);
            if (!member)
                continue;
            const auto slot = static_cast<u32>(member - members.records);
            if (slot >= members.capacity || !member->occupied)
                return fail(projection, MemberListViewError::corrupt_source);
            if (resident >= projection->rows.count) return fail(projection, MemberListViewError::capacity);
            projection->rows[resident++] = {member->user_id, group_id(*member), slot, member->role_color,
                member->group_position, member->status, MemberListRowKind::member, member->generation, position.index, 0};
            ++resident_members;
        }
        for (const auto& group : members.groups) {
            if (group.scope_slot != scope || !group.resident || group.index >= authoritative->logical_count) continue;
            if (resident >= projection->rows.count) return fail(projection, MemberListViewError::capacity);
            projection->rows[resident++] = {0, group.id, ~0u, 0, 0, GuildMemberStatus::unknown,
                MemberListRowKind::group, 0, group.index, group.count};
        }
        heap_sort(projection->rows.data, resident, [](const auto& left, const auto& right) noexcept {
            return left.logical_index < right.logical_index;
        });
        projection->guild_id = guild_id; projection->list_id = list_id;
        projection->count = authoritative->logical_count; projection->resident_count = resident;
        projection->member_count = authoritative->member_count_known ? authoritative->member_count : resident_members;
        projection->authoritative = true; projection->awaiting_sync = false; projection->source_members = &members;
        projection->source_member_revision = members.revision;
        projection->source_user_revision = users.revision;
        projection->source_permission_mutations = state.permission_mutations;
        saturating_increment(projection->revision); saturating_increment(projection->rebuilds);
        return {MemberListViewError::none, projection->count, projection->member_count, true};
    }
    if (projection->awaiting_sync && projection->guild_id == guild_id && projection->list_id == list_id) {
        projection->resident_count = 0; projection->member_count = 0;
        projection->authoritative = true; projection->source_members = nullptr;
        projection->source_member_revision = members.revision;
        projection->source_user_revision = users.revision;
        projection->source_permission_mutations = state.permission_mutations;
        saturating_increment(projection->revision); saturating_increment(projection->rebuilds);
        return {MemberListViewError::none, projection->count, 0, true};
    }
    u32 member_count = 0;
    for (u32 order = 0; order < members.count; ++order) {
        const u32 slot = members.order[order];
        if (slot >= members.capacity || !members.records[slot].occupied) {
            return fail(projection, MemberListViewError::corrupt_source);
        }
        // Opcode 37 member-list subscriptions are channel scoped. Cached
        // identity/role data remains useful after a channel switch, but an
        // invalid list index must not remain visible in the new channel.
        if (members.records[slot].guild_id == guild_id && members.records[slot].list_id == list_id &&
            members.records[slot].list_index != ~0u) {
            if (member_count >= projection->member_scratch.count) {
                return fail(projection, MemberListViewError::capacity);
            }
            projection->member_scratch[member_count++] = slot;
        }
    }
    heap_sort(projection->member_scratch.data, member_count,
              [&](u32 left, u32 right) noexcept {
                  return member_less(members, users, left, right);
              });
    u32 row_count = 0;
    u64 previous_group = ~0ull - 1ull;
    for (u32 index = 0; index < member_count; ++index) {
        const u32 slot = projection->member_scratch[index];
        const GuildMemberRecord& member = members.records[slot];
        const u64 current_group = group_id(member);
        if (index == 0 || current_group != previous_group) {
            if (row_count >= projection->rows.count) {
                return fail(projection, MemberListViewError::capacity);
            }
            projection->rows[row_count++] = MemberListRow{0,
                                                                 current_group,
                                                                 ~0u,
                                                                 0,
                                                                 member.group_position,
                                                                 member.status,
                                                                 MemberListRowKind::group};
            previous_group = current_group;
        }
        if (row_count >= projection->rows.count) {
            return fail(projection, MemberListViewError::capacity);
        }
        projection->rows[row_count++] = MemberListRow{member.user_id,
                                                             current_group,
                                                             slot,
                                                             member.role_color,
                                                             member.group_position,
                                                             member.status,
                                                             MemberListRowKind::member};
    }
    projection->guild_id = guild_id;
    projection->list_id = list_id;
    projection->count = row_count;
    projection->resident_count = row_count;
    projection->authoritative = false; projection->awaiting_sync = false; projection->source_members = nullptr;
    projection->member_count = member_count;
    projection->source_member_revision = members.revision;
    projection->source_user_revision = users.revision;
    projection->source_permission_mutations = state.permission_mutations;
    saturating_increment(projection->revision);
    saturating_increment(projection->rebuilds);
    return MemberListViewResult{MemberListViewError::none, row_count,
                                             member_count, true};
}

MemberListViewResult member_list_view_private(
    MemberListView* projection, const UserStore& users,
    const ChatStateStore& state, u64 channel_id) noexcept {
    if (!projection || !projection->rows.data || !state.private_channels)
        return fail(projection, MemberListViewError::invalid_input);
    const auto& store = *state.private_channels;
    const auto* channel = private_channel_store_find(store, channel_id);
    if (!projection->thread_mode && projection->guild_id == 0 && projection->list_id == channel_id &&
        projection->source_member_revision == store.revision && projection->source_user_revision == users.revision)
        return {MemberListViewError::none, projection->count, projection->member_count, false};
    projection->thread_mode = false; projection->source_threads = nullptr; projection->incomplete = false;
    const auto recipients = channel ? private_channel_store_recipients(store, *channel) : ConstSlice<u64>{};
    bool includes_self = false;
    for (u32 i = 0; i < recipients.count; ++i) includes_self |= recipients[i] == state.self_user_id;
    const u32 count = recipients.count + (channel && !includes_self && state.self_user_id ? 1u : 0u);
    if (count + 1 > projection->rows.count) return fail(projection, MemberListViewError::capacity);
    projection->rows[0] = {0, kMemberListPrivateGroup, ~0u, 0, 0, GuildMemberStatus::unknown, MemberListRowKind::group};
    for (u32 i = 0; i < count; ++i) projection->rows[i + 1] = {
        i < recipients.count ? recipients[i] : state.self_user_id, channel_id, ~0u, 0, 0,
        GuildMemberStatus::unknown, MemberListRowKind::member};
    projection->guild_id = 0; projection->list_id = channel_id;
    projection->count = count ? count + 1 : 0; projection->member_count = count;
    projection->resident_count = projection->count; projection->authoritative = false; projection->awaiting_sync = false; projection->source_members = nullptr;
    projection->source_member_revision = store.revision; projection->source_user_revision = users.revision;
    projection->source_permission_mutations = state.permission_mutations;
    ++projection->revision; ++projection->rebuilds;
    return {MemberListViewError::none, projection->count, count, true};
}

bool member_list_view_await_sync(MemberListView* projection,
    u64 guild_id, u64 list_id, u32 logical_count) noexcept {
    if (!projection || !projection->rows.data || !guild_id || !list_id || !logical_count) return false;
    const bool changed = !projection->awaiting_sync || projection->guild_id != guild_id ||
        projection->list_id != list_id || projection->count != logical_count || projection->resident_count != 0;
    projection->guild_id = guild_id; projection->list_id = list_id; projection->count = logical_count;
    projection->thread_mode = false; projection->source_threads = nullptr; projection->incomplete = false;
    projection->member_count = 0; projection->resident_count = 0;
    projection->authoritative = true; projection->awaiting_sync = true; projection->source_members = nullptr;
    saturating_increment_if(changed, projection->revision);
    return true;
}
MemberListPresenceResult member_list_view_refresh_presence(
    MemberListView* projection, const GuildMemberStore& members,
    u32 visible_first, u32 visible_count) noexcept {
    MemberListPresenceResult result{};
    if (!projection || projection->source_presence_revision == members.presence_revision)
        return result;
    projection->source_presence_revision = members.presence_revision;
    if (!projection->rows.data || !members.records || projection->awaiting_sync || !projection->guild_id)
        return result;
    const bool sorted = !projection->authoritative;
    const u32 resident = minimum(projection->resident_count, projection->rows.count);
    for (u32 index = 0; index < resident; ++index) {
        MemberListRow& row = projection->rows[index];
        if (row.kind != MemberListRowKind::member || row.member_slot >= members.capacity)
            continue;
        const GuildMemberRecord& member = members.records[row.member_slot];
        if (!member.occupied || member.user_id != row.user_id || member.guild_id != projection->guild_id ||
            (member.status == row.status && member.role_color == row.role_color))
            continue;
        // Thread rows group unknown status with offline, like their rebuild.
        const u64 group = projection->thread_mode && member.status == GuildMemberStatus::unknown
                              ? kMemberListOfflineGroup
                              : group_id(member);
        if (sorted && group != row.group_role_id) {
            result.regroup = true;
            continue;
        }
        row.status = member.status;
        row.role_color = member.role_color;
        row.group_role_id = group;
        ++result.updated;
        const u32 logical = projection->authoritative ? row.logical_index : index;
        result.visible = result.visible || (logical >= visible_first && logical - visible_first < visible_count);
    }
    if (result.regroup) {
        projection->presence_regroup = true;
        result.visible = true;
    }
    if (result.visible)
        saturating_increment(projection->revision);
    return result;
}

u32 member_list_view_count(const MemberListView& projection) noexcept {
    return projection.count;
}
const MemberListRow* member_list_view_row(
    const MemberListView& projection, u32 logical_index) noexcept {
    if (!projection.rows.data || projection.awaiting_sync || logical_index >= projection.count) return nullptr;
    if (projection.thread_mode) {
        if (!projection.source_threads || !projection.source_members ||
            projection.source_thread_revision != projection.source_threads->revision ||
            projection.source_thread_generation != projection.source_threads->generation ||
            !thread_member_store_owns(*projection.source_threads, projection.guild_id, projection.list_id) ||
            projection.source_member_revision != projection.source_members->revision ||
            logical_index >= projection.resident_count || logical_index >= projection.rows.count) return nullptr;
        const auto& row = projection.rows[logical_index];
        if (row.kind == MemberListRowKind::member) {
            if (!thread_member_store_contains(*projection.source_threads, row.user_id)) return nullptr;
            if (row.member_slot != ~0u) {
                const auto& store = *projection.source_members;
                if (row.member_slot >= store.capacity) return nullptr;
                const auto& member = store.records[row.member_slot];
                if (!member.occupied || member.generation != row.member_generation || member.user_id != row.user_id || member.guild_id != projection.guild_id) return nullptr;
            }
        }
        return &row;
    }
    if (!projection.authoritative) return logical_index < projection.rows.count ? &projection.rows[logical_index] : nullptr;
    if (projection.source_members && projection.source_member_revision != projection.source_members->revision) return nullptr;
    u32 first = 0, count = minimum(projection.resident_count, projection.rows.count);
    while (count) {
        const u32 step = count / 2, current = first + step;
        if (projection.rows[current].logical_index < logical_index) { first = current + 1; count -= step + 1; }
        else count = step;
    }
    if (first >= projection.resident_count || first >= projection.rows.count ||
        projection.rows[first].logical_index != logical_index) return nullptr;
    const auto& row = projection.rows[first];
    if (row.kind == MemberListRowKind::member && projection.source_members) {
        const auto& store = *projection.source_members;
        if (row.member_slot >= store.capacity) return nullptr;
        const auto& member = store.records[row.member_slot];
        // The list's own row names this member (the record's list_index may
        // belong to another list); revision and generation keep it current.
        if (!member.occupied || member.generation != row.member_generation || member.user_id != row.user_id ||
            member.guild_id != projection.guild_id) return nullptr;
    }
    return &row;
}

Utf8View member_list_row_label(const MemberListRow& row,
                                       const GuildMemberStore& members,
                                       const UserStore& users,
                                       const ChatStateStore& state) noexcept {
    if (row.kind == MemberListRowKind::group) {
        if (row.group_role_id == kMemberListPrivateGroup) return utf8_literal("Members");
        if (row.group_role_id == kMemberListOfflineGroup) {
            return utf8_literal("Offline");
        }
        if (row.group_role_id == kMemberListOnlineGroup) {
            return utf8_literal("Online");
        }
        const RoleRecord* role = chat_state_store_find_role(
            state,
            row.group_role_id == 0
                ? 0
                : (row.member_slot < members.capacity ? members.records[row.member_slot].guild_id : 0),
            row.group_role_id);
        if (!role) {
            // Group rows have no member slot. Guild role IDs are unique within
            // the selected projection, so a bounded state scan is sufficient.
            for (u32 index = 0; index < state.role_count; ++index) {
                if (state.roles[index].id == row.group_role_id) {
                    role = &state.roles[index];
                    break;
                }
            }
        }
        return role ? chat_state_store_role_name(state, *role) : Utf8View{};
    }
    if (row.member_slot >= members.capacity) {
        const auto nickname = state.private_channels ? private_channel_store_nickname(*state.private_channels, row.group_role_id, row.user_id) : Utf8View{};
        const auto* user = user_store_find(users, row.user_id);
        return nickname.bytes ? nickname : user ? user_store_name(users, *user) : utf8_literal("Unknown user");
    }
    return member_name(members.records[row.member_slot], users);
}

static_assert(sizeof(MemberListRow) == 40);

} // namespace maicord
