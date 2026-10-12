#include "maicord/model/chat_state.hpp"
#include "maicord/model/role_policy.hpp"

namespace maicord {
namespace {
struct RoleFixture {
    ChatStateStore state{};
    GuildRecord guilds[32]{};
    ChannelRecord channels[1]{};
    PermissionOverwriteRecord overwrites[1]{};
    u16 guild_order[32]{};
    u16 channel_order[1]{};
    u8 names[64]{};

    bool initialize(RoleRecord* roles, u32 count) noexcept {
        return chat_state_store_initialize(&state, {guilds, 32}, {guild_order, 32},
                   {channels, 1}, {channel_order, 1}, {names, 64}) &&
               chat_state_store_bind_permissions(&state, {roles, count}, {overwrites, 1});
    }
};

bool roles_sorted(const ChatStateStore& state) noexcept {
    for (u32 index = 1; index < state.role_count; ++index) {
        const auto& before = state.roles[index - 1];
        const auto& after = state.roles[index];
        if (before.guild_id > after.guild_id ||
            (before.guild_id == after.guild_id && before.id >= after.id)) return false;
    }
    return true;
}

u64 guild_key(u32 guild) noexcept { return 1000ull + guild * 17ull; }
u64 role_key(u32 role) noexcept { return ~0ull - 4ull * (255u - role); }
u64 permissions_key(u32 guild, u32 role) noexcept { return 1ull + guild * 257ull + role; }
} // namespace

void test_role_lookup_performance(void (*check)(bool, const char*) noexcept) noexcept {
    // Full shipping role capacity, independently generated expectations. The affine
    // permutation visits every input once and interleaves guilds and role IDs.
    static RoleRecord records[8192];
    RoleFixture fixture{};
    check(fixture.initialize(records, 8192), "role lookup: full-capacity fixture binds");
    auto& state = fixture.state;
    bool inserted = true;
    for (u32 input = 0; input < 8192; ++input) {
        const u32 key = (input * 4051u + 31u) & 8191u;
        const u32 guild = key / 256, role = key % 256;
        inserted = chat_state_store_upsert_role(&state,
            {role_key(role), guild_key(guild), permissions_key(guild, role), {}, role, key}) && inserted;
    }
    check(inserted && state.role_count == 8192 && roles_sorted(state),
          "role lookup: shuffled mixed-guild input uses existing backing in strict key order");
    bool found_all = true, misses = true;
    for (u32 guild = 0; guild < 32; ++guild) {
        for (u32 role = 0; role < 256; ++role) {
            const auto* found = chat_state_store_find_role(state, guild_key(guild), role_key(role));
            found_all = found && found->permissions == permissions_key(guild, role) &&
                        found->position == role && found->color == guild * 256u + role && found_all;
            misses = !chat_state_store_find_role(state, guild_key(guild), role_key(role) - 1) && misses;
        }
        misses = !chat_state_store_find_role(state, guild_key(guild) + 1, role_key(255)) && misses;
    }
    check(found_all && misses && !chat_state_store_find_role(state, 0, role_key(0)) &&
          !chat_state_store_find_role(state, guild_key(0), 0),
          "role lookup: every late/mixed-guild role and interior/absent-guild miss resolves independently");
    u64 assigned_roles[kGuildRoleMaximum]{};
    for (u32 role = 0; role < kGuildRoleMaximum; ++role) assigned_roles[role] = role_key(255 - role);
    bool assigned_all = chat_state_store_upsert_guild(&state,
        {guild_key(31), {}, GuildMutationKind::create, 999}).ok() &&
        chat_state_store_set_self_roles(&state, guild_key(31), 999, 42, {assigned_roles, 256});
    for (u32 role = 0; role < 256; ++role) {
        assigned_all = chat_state_store_self_has_role(state, guild_key(31), role_key(role)) &&
                       !chat_state_store_self_has_role(state, guild_key(30), role_key(role)) && assigned_all;
    }
    check(assigned_all, "role lookup: all 256 self roles resolve in the late guild without assigning matching IDs elsewhere");
    check(!chat_state_store_upsert_role(&state, {1, 1, 1, {}}) && state.role_count == 8192 &&
          roles_sorted(state) && chat_state_store_upsert_role(&state,
              {role_key(255), guild_key(31), 987654, {}, 3, 0x123456, true, true}),
          "role lookup: full-capacity rejection preserves order while existing late role can update");
    const auto* updated = chat_state_store_find_role(state, guild_key(31), role_key(255));
    check(updated && updated->permissions == 987654 && updated->position == 3 &&
          updated->color == 0x123456 && updated->hoist && updated->managed,
          "role lookup: permission/position/color/flags update the correct mixed-guild record");

    bool deleted = chat_state_store_remove_role(&state, guild_key(0), role_key(0)) &&
                   chat_state_store_remove_role(&state, guild_key(15), role_key(127)) &&
                   chat_state_store_remove_role(&state, guild_key(31), role_key(255));
    chat_state_store_remove_guild_roles(&state, guild_key(20));
    deleted = deleted && state.role_count == 7933 && roles_sorted(state) &&
              !chat_state_store_find_role(state, guild_key(0), role_key(0)) &&
              !chat_state_store_find_role(state, guild_key(15), role_key(127)) &&
              !chat_state_store_find_role(state, guild_key(31), role_key(255));
    bool survivors = true;
    for (u32 guild = 0; guild < 32; ++guild) {
        for (u32 role = 0; role < 256; ++role) {
            const bool removed = guild == 20 || (guild == 0 && role == 0) ||
                (guild == 15 && role == 127) || (guild == 31 && role == 255);
            const auto* found = chat_state_store_find_role(state, guild_key(guild), role_key(role));
            survivors = (removed ? found == nullptr : found &&
                found->permissions == permissions_key(guild, role)) && survivors;
        }
    }
    check(deleted && survivors, "role lookup: first/middle/last and whole-guild deletion preserve all survivors");
    bool reused = true;
    for (u32 role = 0; role < 259; ++role) {
        reused = chat_state_store_upsert_role(&state, {1ull + role, 1, 9000ull + role, {}}) && reused;
    }
    const auto* refilled = chat_state_store_find_role(state, 1, 259);
    check(reused && state.role_count == 8192 && roles_sorted(state) &&
          refilled && refilled->permissions == 9258 &&
          !chat_state_store_find_role(state, guild_key(20), role_key(255)),
          "role lookup: reclaimed slots refill before all resident guilds without stale identities");
    const RoleRecord* previous = nullptr;
    bool displayed = true;
    for (u32 step = 0; step < 259; ++step) {
        previous = role_display_next(1, {state.roles, state.role_count}, previous);
        // All positions are equal: ascending ids, with @everyone (id 1) last.
        displayed = previous && previous->guild_id == 1 && previous->id == (step < 258 ? step + 2 : 1) &&
                    displayed;
        if (!previous) break;
    }
    check(displayed && !role_display_next(1, {state.roles, state.role_count}, previous),
          "role chooser: complete display traversal exceeds 256 roles and excludes mixed-guild backing records");
    chat_state_store_clear(&state);
    bool cleared = state.role_count == 0 && state.roles == records && state.role_capacity == 8192;
    for (u32 index = 0; index < 8192; ++index) cleared = records[index].id == 0 && cleared;
    cleared = cleared && !chat_state_store_find_role(state, 1, 259);
    const bool reinserted = chat_state_store_upsert_role(&state, {role_key(255), guild_key(31), 42, {}});
    const auto* reset_role = chat_state_store_find_role(state, guild_key(31), role_key(255));
    check(cleared && reinserted && reset_role && reset_role->permissions == 42,
          "role lookup: reset clears live/index state and permits late-key reuse");

    RoleRecord named_records[8]{};
    RoleFixture named{};
    const bool ready = named.initialize(named_records, 8) &&
        chat_state_store_upsert_guild(&named.state,
            {50, {}, GuildMutationKind::create, 999}).ok() &&
        chat_state_store_upsert_guild(&named.state,
            {60, {}, GuildMutationKind::create, 999}).ok() &&
        chat_state_store_upsert_role(&named.state,
            {90, 60, 16, utf8_literal("other"), 8, 0xFF0000, true, false}) &&
        chat_state_store_upsert_role(&named.state,
            {70, 50, 2, utf8_literal("named"), 7, 0x0000FF, true, false}) &&
        chat_state_store_upsert_role(&named.state,
            {50, 50, 1, utf8_literal("all")});
    const u64 assigned[]{70};
    check(ready && chat_state_store_set_self_roles(&named.state, 50, 999, 42, {assigned, 1}),
          "role lookup: named records and self assignment initialize out of order");
    const u64 duplicate[]{70, 70}, foreign[]{70, 90}, zero[]{70, 0};
    check(!chat_state_store_set_self_roles(&named.state, 50, 999, 42, {duplicate, 2}) &&
          !chat_state_store_set_self_roles(&named.state, 50, 999, 42, {foreign, 2}) &&
          !chat_state_store_set_self_roles(&named.state, 50, 999, 42, {zero, 2}) &&
          chat_state_store_self_has_role(named.state, 50, 70) &&
          !chat_state_store_self_has_role(named.state, 60, 90),
          "role lookup: invalid self-role replacements preserve assignment and reject foreign guild records");
    check(chat_state_store_set_self_roles(&named.state, 50, 999, 42, {}) &&
          !chat_state_store_self_has_role(named.state, 50, 70) &&
          chat_state_store_set_self_roles(&named.state, 50, 999, 42, {assigned, 1}),
          "role lookup: empty self-role replacement clears old flags before late reassignment");
    bool renamed = ready;
    for (u32 iteration = 0; iteration < 12; ++iteration) {
        renamed = chat_state_store_upsert_role(&named.state,
            {70, 50, 4, iteration % 2 ? utf8_literal("green") : utf8_literal("fresh"),
             99, 0x00FF00, false, true}) && renamed;
    }
    const auto* named_role = chat_state_store_find_role(named.state, 50, 70);
    const auto* other = chat_state_store_find_role(named.state, 60, 90);
    check(renamed && named.state.compactions > 0 && roles_sorted(named.state) && named_role &&
          named_role->self_assigned && named_role->position == 99 && named_role->color == 0x00FF00 &&
          named_role->managed && !named_role->hoist &&
          utf8_equal(chat_state_store_role_name(named.state, *named_role), utf8_literal("green")) &&
          other && other->permissions == 16 &&
          utf8_equal(chat_state_store_role_name(named.state, *other), utf8_literal("other")),
          "role lookup: repeated rename/name-bank compaction preserve keys, neighbors and self assignment");
    bool display_order = chat_state_store_upsert_role(&named.state,
        {65, 50, 0, utf8_literal("zeta"), 99}) &&
        chat_state_store_upsert_role(&named.state, {60, 50, 0, utf8_literal("alpha"), 99});
    // Equal positions rank by ID, not by name (alpha, green, zeta); @everyone is last.
    const u64 display_ids[]{60, 65, 70, 50};
    previous = nullptr;
    for (u32 index = 0; index < 4; ++index) {
        previous = role_display_next(50, {named.state.roles, named.state.role_count}, previous);
        display_order = previous && previous->id == display_ids[index] && display_order;
        if (!previous) break;
    }
    check(display_order && !role_display_next(50, {named.state.roles, named.state.role_count}, previous),
          "role chooser: position then ID order, @everyone last, survives lookup-order storage and excludes foreign guild");
    u16 named_order[4]{};
    bool named_sorted = role_display_order({named.state.roles, 4}, {named_order, 4}, role_before);
    for (u32 index = 0; index < 4; ++index)
        named_sorted = named.state.roles[named_order[index]].id == display_ids[index] && named_sorted;
    check(named_sorted, "role chooser: transient heap order uses the shared role comparator without reordering records");
    chat_state_store_recompute_guild_permissions(&named.state, 50);
    const auto* guild = chat_state_store_find_guild(named.state, 50);
    check(guild && guild->permissions_resolved && guild->base_permissions == 5 &&
          chat_state_store_self_has_role(named.state, 50, 70) &&
          !chat_state_store_self_has_role(named.state, 60, 70),
          "role lookup: permission union and self membership retain guild scoping after sorted updates");
    const u32 count = named.state.role_count;
    const bool rejected = !chat_state_store_upsert_role(&named.state,
        {1, 1, 0, utf8_literal("a name larger than the entire name bank")});
    named_role = chat_state_store_find_role(named.state, 50, 70);
    check(rejected && named.state.role_count == count && roles_sorted(named.state) &&
          named_role && named_role->self_assigned,
          "role lookup: rejected name allocation leaves sorted records and flags intact");
    check(chat_state_store_remove_guild(&named.state, 50, false) &&
          !chat_state_store_find_role(named.state, 50, 70) &&
          chat_state_store_find_role(named.state, 60, 90) && roles_sorted(named.state),
          "role lookup: guild lifecycle removal leaves later guild lookups valid");

    // Reuse the fixture's existing backing for a single guild at shipping maximum.
    // Unique shuffled positions give an independent descending-rank oracle.
    check(fixture.initialize(records, 8192), "role chooser: large single-guild ordering fixture rebinds");
    state.role_count = 8192;
    for (u32 index = 0; index < 8192; ++index) {
        records[index].id = index + 1;
        records[index].guild_id = 7;
        records[index].position = (index * 4051u + 31u) & 8191u;
    }
    u16 order[8192];
    u32 comparisons = 0;
    bool sorted = role_display_order({records, 8192}, {order, 8192},
        [&comparisons](const RoleRecord& left, const RoleRecord& right) noexcept {
            ++comparisons;
            return role_before(left, right);
        });
    // Record 7 is the guild's @everyone and ranks last whatever its position.
    const u32 everyone_position = records[6].position;
    u32 expected_position = 8191;
    for (u32 rank = 0; rank < 8191; ++rank, --expected_position) {
        if (expected_position == everyone_position) --expected_position;
        sorted = order[rank] < 8192 && records[order[rank]].position == expected_position && sorted;
    }
    sorted = order[8191] < 8192 && records[order[8191]].id == 7 && sorted;
    bool backing_unchanged = true;
    for (u32 index = 0; index < 8192; ++index)
        backing_unchanged = records[index].id == index + 1 && records[index].guild_id == 7 &&
            records[index].position == ((index * 4051u + 31u) & 8191u) && backing_unchanged;
    // A binary heap uses at most two comparisons per level; build plus extraction
    // is bounded here by 2*N*(log2(N)+1), independent of input timing or key order.
    check(sorted && backing_unchanged && comparisons < 2u * 8192u * 14u,
          "role chooser: 8192 shuffled roles sort within 229376 comparator calls and preserve lookup backing");

    u64 window_ids[16]{};
    u32 eligible = 0, window_count = 0;
    for (u32 rank = 0; rank < 8192; ++rank) {
        const auto& role = records[order[rank]];
        // Deterministic assigned/managed exclusions exercise deep eligible offset.
        if (role.id % 7 == 0 || role.position % 5 == 0) continue;
        if (eligible++ < 4000) continue;
        window_ids[window_count++] = role.id;
        if (window_count == 16) break;
    }
    u32 inverse = 1;
    while (((inverse * 4051u) & 8191u) != 1) ++inverse;
    eligible = 0;
    u32 expected_count = 0;
    bool window_matches = window_count == 16;
    for (u32 descending = 8192; descending != 0; --descending) {
        const u32 position = descending - 1;
        const u64 id = (((position + 8192u - 31u) * inverse) & 8191u) + 1ull;
        if (id % 7 == 0 || position % 5 == 0) continue;
        if (eligible++ < 4000) continue;
        window_matches = window_ids[expected_count++] == id && window_matches;
        if (expected_count == 16) break;
    }
    check(window_matches && expected_count == 16,
          "role chooser: deep 16-row window matches independent rank oracle after assigned/managed exclusions");
    u16 too_small[1]{0xA5A5};
    check(!role_display_order({records, 8192}, {too_small, 1}, role_before) && too_small[0] == 0xA5A5,
          "role chooser: insufficient transient index capacity rejects before writing for complete fallback traversal");

    bool policy_fixture = chat_state_store_upsert_guild(&state,
        {7, {}, GuildMutationKind::create, 999}).ok();
    state.self_user_id = 42;
    auto& policy_guild = fixture.guilds[0];
    policy_guild.member_roles_known = policy_guild.permissions_resolved = true;
    policy_guild.base_permissions = 1ull << 28;
    for (u32 index = 0; index < 5; ++index) records[index].position = 1000;
    for (u32 index = 0; index < 3; ++index) records[index].self_assigned = true;
    records[3].managed = true;
    const auto policy = role_edit_policy(state, 7);
    bool eligibility = policy_fixture && policy.allowed && !policy.owner && policy.hierarchy_known &&
        policy.highest_position == 1000 && policy.highest_role_id == 1;
    for (u32 index = 0; index < 8192; ++index) {
        const auto& target = records[index];
        bool outranked = false;
        // Independent pairwise oracle uses the three supplied assigned records,
        // rather than reconstructing the summary's highest-role algorithm.
        for (u32 own = 0; own < 3; ++own) outranked = outranked ||
            records[own].position > target.position ||
            (records[own].position == target.position && records[own].id < target.id);
        const bool expected = target.id != 7 && !target.managed && outranked;
        eligibility = role_editable(policy, target) == expected && eligibility;
    }
    auto foreign_role = records[4]; foreign_role.guild_id = 8;
    check(eligibility && !role_editable(policy, records[0]) &&
          role_editable(policy, records[2]) && !role_editable(policy, records[3]) &&
          !role_editable(policy, foreign_role),
          "role chooser: once-per-layout hierarchy summary matches all 8192 independent eligibility decisions and lower-ID ties");
    policy_guild.owner_id = state.self_user_id;
    policy_guild.permissions_resolved = policy_guild.member_roles_known = false;
    const auto owner_policy = role_edit_policy(state, 7);
    check(owner_policy.owner && role_editable(owner_policy, records[8191]) &&
          !role_editable(owner_policy, records[6]) &&
          !role_editable(owner_policy, records[3]) && !role_editable(owner_policy, foreign_role),
          "role chooser: owner bypass retains everyone/managed/foreign-guild exclusions with unknown permissions");
    policy_guild.owner_id = 999;
    check(!role_edit_policy(state, 7).allowed,
          "role chooser: unknown member roles or permissions fail closed before hierarchy traversal");
    policy_guild.permissions_resolved = policy_guild.member_roles_known = true;
    policy_guild.base_permissions = 0;
    check(!role_edit_policy(state, 7).allowed,
          "role chooser: permission revocation rejects edit policy");
    policy_guild.base_permissions = kPermissionAdministrator;
    check(role_editable(role_edit_policy(state, 7), records[4]) &&
          !role_editable(role_edit_policy(state, 7), records[0]),
          "role chooser: administrator permission still respects role hierarchy and equal-position self role");
    state.self_user_id = 0;
    check(!role_edit_policy(state, 7).allowed,
          "role chooser: missing self identity prevents owner/permission policy admission");
}
} // namespace maicord
