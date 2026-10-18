#pragma once
#include "maicord/model/chat_state.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/core/heap_sort.hpp"
namespace maicord {
// The one order of a guild's roles: higher position first, equal positions by
// the older (smaller) id, @everyone last. Member role arrays arrive in wire
// order, so every list and every winner is ranked with this.
inline bool role_rank_before(u64 guild_id, u32 position, u64 id, u32 other_position,
                             u64 other_id) noexcept {
    const bool everyone = id == guild_id;
    const bool other_everyone = other_id == guild_id;
    if (everyone || other_everyone)
        return other_everyone && !everyone;
    return position != other_position ? position > other_position : id < other_id;
}

inline bool role_before(const RoleRecord& left, const RoleRecord& right) noexcept {
    return role_rank_before(left.guild_id, left.position, left.id, right.position, right.id);
}

// Profile role pills: linked roles follow all others; each group keeps the
// hierarchy order. Colour, icon and hoist selection are unaffected.
inline bool role_pill_before(const RoleRecord& left, const RoleRecord& right) noexcept {
    return left.linked != right.linked ? right.linked != 0 : role_before(left, right);
}

// A candidate against the retained winner; a zero winner id means none yet.
inline bool role_rank_above(const RoleRecord& candidate, u32 position,
                                    u64 role_id) noexcept {
    return candidate.id &&
           (!role_id || role_rank_before(candidate.guild_id, candidate.position, candidate.id,
                                         position, role_id));
}

// The roles that give a member their name colour, role icon and hoisted list
// group. Each is the best role that has the trait, so a top role without a
// colour or icon never hides the lower role that has one.
struct MemberRoleDisplay {
    const RoleRecord* color;
    const RoleRecord* icon;
    const RoleRecord* hoist;
};

inline MemberRoleDisplay role_member_display(const ChatStateStore& state, u64 guild_id,
                                             ConstSlice<u64> role_ids) noexcept {
    MemberRoleDisplay display{};
    if (!role_ids.data && role_ids.count)
        return display;
    const auto better = [](const RoleRecord* held, const RoleRecord& role) noexcept {
        return !held || role_before(role, *held);
    };
    for (u32 index = 0; index < role_ids.count; ++index) {
        const auto* role = chat_state_store_find_role(state, guild_id, role_ids[index]);
        if (!role)
            continue;
        if (role->color && better(display.color, *role))
            display.color = role;
        if (role->has_icon && better(display.icon, *role))
            display.icon = role;
        if (role->hoist && better(display.hoist, *role))
            display.hoist = role;
    }
    return display;
}

// The guild's role after `previous` in role_before order, for traversals that
// keep no index.
inline const RoleRecord* role_display_next(u64 guild_id, ConstSlice<RoleRecord> roles,
                                           const RoleRecord* previous = nullptr) noexcept {
    const RoleRecord* next = nullptr;
    for (u32 index = 0; index < roles.count; ++index) {
        const auto& role = roles[index];
        if (role.guild_id != guild_id || (previous && !role_before(*previous, role)))
            continue;
        if (!next || role_before(role, *next))
            next = &role;
    }
    return next;
}

// Sort only transient ordinal indices; the lookup backing stays in place. The
// comparator parameter also permits deterministic work-bound tests.
template <typename Before>
inline bool role_display_order(ConstSlice<RoleRecord> roles, Slice<u16> order,
                                       Before before) noexcept {
    if (roles.count > order.count || roles.count > 65'536 ||
        (roles.count != 0 && (!roles.data || !order.data)))
        return false;
    for (u32 index = 0; index < roles.count; ++index)
        order[index] = static_cast<u16>(index);
    heap_sort(order.data, roles.count,
              [&](u16 left, u16 right) noexcept { return before(roles[left], roles[right]); });
    return true;
}

struct RoleEditPolicy {
    u64 guild_id;
    u64 highest_role_id;
    u32 highest_position;
    bool allowed;
    bool owner;
    bool hierarchy_known;
};

inline RoleEditPolicy role_edit_policy(const ChatStateStore& state,
                                                      u64 guild_id) noexcept {
    RoleEditPolicy policy{};
    policy.guild_id = guild_id;
    const auto* guild = chat_state_store_find_guild(state, guild_id);
    if (!guild || !state.self_user_id)
        return policy;
    if (guild->owner_id == state.self_user_id) {
        policy.allowed = policy.owner = true;
        return policy;
    }
    if (!guild->permissions_resolved || !guild->member_roles_known ||
        !(guild->base_permissions & (kPermissionManageRoles | kPermissionAdministrator)))
        return policy;
    policy.allowed = true;
    for (u32 i = 0; i < state.role_count; ++i) {
        const auto& own = state.roles[i];
        if (own.guild_id == guild_id && own.self_assigned &&
            (!policy.hierarchy_known || role_rank_before(guild_id, own.position, own.id,
                                                         policy.highest_position, policy.highest_role_id))) {
            policy.highest_role_id = own.id;
            policy.highest_position = own.position;
            policy.hierarchy_known = true;
        }
    }
    return policy;
}

inline bool role_editable(const RoleEditPolicy& policy,
                                  const RoleRecord& role) noexcept {
    return policy.allowed && role.id && role.guild_id == policy.guild_id && role.id != policy.guild_id &&
           !role.managed &&
           (policy.owner || (policy.hierarchy_known &&
                             role_rank_before(policy.guild_id, policy.highest_position, policy.highest_role_id,
                                              role.position, role.id)));
}

inline bool role_editable(const ChatStateStore& state, u64 guild_id, u64 role_id) noexcept {
    const auto* role = chat_state_store_find_role(state, guild_id, role_id);
    return role && role_editable(role_edit_policy(state, guild_id), *role);
}
} // namespace maicord
