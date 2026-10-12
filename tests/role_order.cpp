#include "maicord/model/member_list.hpp"
#include "maicord/model/role_policy.hpp"
#include "maicord/model/thread_member_store.hpp"

namespace maicord {
namespace role_order_probe {
constexpr u64 kGuild = 500;
constexpr u64 kThread = 70;

struct Fixture {
    ChatStateStore state;
    GuildRecord guilds[2];
    u16 guild_order[2];
    ChannelRecord channels[1];
    u16 channel_order[1];
    u8 names[64];
    RoleRecord roles[16];
    PermissionOverwriteRecord overwrites[1];
    GuildMemberStore members;
    GuildMemberRecord member_records[8];
    u32 member_order[8];
    UserStore users;
    UserRecord user_records[8];
    u32 user_order[8];
    u8 user_names[512];
    MemberListView projection;
    MemberListRow rows[16];
    u32 scratch[8];
    ThreadMemberStore threads;
    u64 banks[2][8];
};
Fixture fixture{};

bool add_role(u64 id, u32 position, u32 color = 0, bool hoist = false, bool icon = false) noexcept {
    RoleInput input{};
    input.id = id;
    input.guild_id = kGuild;
    input.position = position;
    input.color = color;
    input.hoist = hoist;
    input.has_icon = icon;
    return chat_state_store_upsert_role(&fixture.state, input);
}

bool initialize() noexcept {
    auto& f = fixture;
    return chat_state_store_initialize(&f.state, {f.guilds, 2}, {f.guild_order, 2}, {f.channels, 1},
                                       {f.channel_order, 1}, {f.names, 64}) &&
           chat_state_store_bind_permissions(&f.state, {f.roles, 16}, {f.overwrites, 1}) &&
           guild_member_store_initialize(&f.members, {f.member_records, 8}, {f.member_order, 8}) &&
           user_store_initialize(&f.users, {f.user_records, 8}, {f.user_order, 8}, {f.user_names, 512}) &&
           member_list_view_initialize(&f.projection, {f.rows, 16}, {f.scratch, 8}) &&
           thread_member_store_initialize(&f.threads, {f.banks[0], 8}, {f.banks[1], 8}) &&
           thread_member_store_select(&f.threads, kGuild, kThread);
}

bool next_permutation(u64* values, u32 count) noexcept {
    u32 pivot = count - 1;
    while (pivot > 0 && values[pivot - 1] >= values[pivot])
        --pivot;
    if (!pivot)
        return false;
    u32 swap_with = count - 1;
    while (values[swap_with] <= values[pivot - 1])
        --swap_with;
    const u64 held = values[pivot - 1];
    values[pivot - 1] = values[swap_with];
    values[swap_with] = held;
    for (u32 low = pivot, high = count - 1; low < high; ++low, --high) {
        const u64 swap = values[low];
        values[low] = values[high];
        values[high] = swap;
    }
    return true;
}

u64 id_of(const RoleRecord* role) noexcept {
    return role ? role->id : 0;
}
} // namespace role_order_probe

void test_role_order(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace role_order_probe;
    auto& f = fixture;
    // Inserted out of order; ids 505, 510 and 520 tie at position 5, 900 ties
    // @everyone (500) at position 0.
    const bool ready =
        initialize() && add_role(520, 5, 0, true, true) && add_role(507, 1, 0x778899, true, true) &&
        add_role(kGuild, 0, 0x0A0B0C) && add_role(510, 5, 0x445566, true, true) && add_role(503, 9) &&
        add_role(505, 5, 0x112233) && add_role(900, 0, 0x010203);
    check(ready, "role order: fixture builds out of order");

    const u64 ranked[] = {503, 505, 510, 520, 507, 900, kGuild};
    const RoleRecord* walked = nullptr;
    bool traversal = ready;
    for (u32 index = 0; index < 7; ++index) {
        walked = role_display_next(kGuild, {f.state.roles, f.state.role_count}, walked);
        traversal = walked && walked->id == ranked[index] && traversal;
    }
    traversal = traversal && !role_display_next(kGuild, {f.state.roles, f.state.role_count}, walked);
    u16 order[16]{};
    bool sorted = role_display_order({f.state.roles, f.state.role_count}, {order, 16}, role_before);
    for (u32 index = 0; index < 7; ++index)
        sorted = sorted && f.state.roles[order[index]].id == ranked[index];
    check(traversal && sorted,
          "role order: higher position first, equal positions by the older ID, @everyone last");

    bool total = true;
    for (u32 left = 0; left < 7; ++left) {
        const auto* a = chat_state_store_find_role(f.state, kGuild, ranked[left]);
        for (u32 right = 0; right < 7; ++right) {
            const auto* b = chat_state_store_find_role(f.state, kGuild, ranked[right]);
            total = total && a && b && role_before(*a, *b) == (left < right);
        }
    }
    check(total, "role order: the comparator is a strict total order over every pair");

    // The member's roles keep gateway order, which carries no rank.
    u64 wire[] = {503, 505, 507, 510, 520};
    u32 permutations = 0;
    bool every_order_agrees = true;
    do {
        const auto display = role_member_display(f.state, kGuild, {wire, 5});
        const auto stored = guild_member_store_upsert(&f.members, f.state, kGuild, 10, {}, {wire, 5}, true,
                                                      GuildMemberStatus::online);
        every_order_agrees = every_order_agrees && stored.ok() && stored.record && id_of(display.color) == 505 &&
                             id_of(display.icon) == 510 && id_of(display.hoist) == 510 &&
                             stored.record->color_role_id == 505 && stored.record->role_color == 0x112233 &&
                             stored.record->group_role_id == 510 && stored.record->group_position == 5;
        ++permutations;
    } while (next_permutation(wire, 5));
    check(permutations == 120 && every_order_agrees,
          "role order: colour, icon and hoisted group are the same for all 120 orders of an unsorted role list");

    // 503 outranks every other role yet has no colour, icon or hoist.
    const u64 below_top[] = {507, 520, 503, 510, 505};
    const auto picked = role_member_display(f.state, kGuild, {below_top, 5});
    check(id_of(picked.color) == 505 && id_of(picked.icon) == 510 && id_of(picked.hoist) == 510,
          "role order: colour and icon come from the best role that has one when the top role has neither");

    const u64 only_top[] = {503};
    const u64 with_everyone[] = {kGuild, 900};
    const auto none = role_member_display(f.state, kGuild, {only_top, 1});
    const auto empty = role_member_display(f.state, kGuild, {});
    const auto invalid = role_member_display(f.state, kGuild, {nullptr, 1});
    const auto last = role_member_display(f.state, kGuild, {with_everyone, 2});
    check(!none.color && !none.icon && !none.hoist && !empty.color && !invalid.color && !invalid.icon &&
              !invalid.hoist && id_of(last.color) == 900 && !last.icon && !last.hoist,
          "role order: a lone top role without traits yields none; @everyone never wins a tie");

    // Hoisted groups at equal position: the older role heads the list.
    const u64 member_roles[3][1] = {{520}, {510}, {507}};
    bool listed = true;
    for (u32 index = 0; index < 3; ++index)
        listed = listed &&
                 user_store_upsert(&f.users, index + 1, utf8_literal("Name"), utf8_literal("name")).ok() &&
                 guild_member_store_upsert(&f.members, f.state, kGuild, index + 1, {}, {member_roles[index], 1},
                                           true, GuildMemberStatus::online, index, 70)
                     .ok();
    const bool built = listed && member_list_view_rebuild(&f.projection, f.members, f.users, f.state, kGuild, 70).ok();
    const u64 expected_rows[6][2] = {{510, 0}, {0, 2}, {520, 0}, {0, 1}, {507, 0}, {0, 3}};
    bool grouped = built && f.projection.count == 6;
    for (u32 index = 0; grouped && index < 6; ++index) {
        const auto* row = member_list_view_row(f.projection, index);
        grouped = row && (row->kind == MemberListRowKind::group
                              ? row->group_role_id == expected_rows[index][0]
                              : row->user_id == expected_rows[index][1]);
    }
    check(grouped, "role order: member list groups at equal position order by the older role ID");

    bool threaded = thread_member_store_snapshot_begin(&f.threads, kGuild, kThread) &&
                    thread_member_store_snapshot_append(&f.threads, 1) &&
                    thread_member_store_snapshot_append(&f.threads, 2) &&
                    thread_member_store_snapshot_append(&f.threads, 3) && thread_member_store_snapshot_commit(&f.threads) &&
                    member_list_view_thread(&f.projection, f.threads, f.members, f.users, f.state, kGuild, kThread).ok();
    grouped = threaded && f.projection.count == 6;
    for (u32 index = 0; grouped && index < 6; ++index) {
        const auto* row = member_list_view_row(f.projection, index);
        grouped = row && (row->kind == MemberListRowKind::group
                              ? row->group_role_id == expected_rows[index][0]
                              : row->user_id == expected_rows[index][1]);
    }
    check(grouped, "role order: thread member list groups at equal position order by the older role ID");
}
} // namespace maicord
