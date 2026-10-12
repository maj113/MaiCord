#pragma once
#include "maicord/model/chat_state.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/core/heap_sort.hpp"
namespace maicord {
// The role hierarchy resolves equal positions by the older (lower) snowflake.
// Names only influence chooser display order, never membership color or rank.
inline bool role_rank_above(const RoleRecord& candidate, u32 position,
                                    u64 role_id) noexcept {
    return candidate.id && (!role_id || candidate.position > position ||
                            (candidate.position == position && candidate.id < role_id));
}

inline const RoleRecord* role_effective_color(const ChatStateStore& state, u64 guild_id,
                                                             ConstSlice<u64> assigned_roles) noexcept {
    if (!assigned_roles.data && assigned_roles.count)
        return nullptr;
    const RoleRecord* result = nullptr;
    for (u32 index = 0; index < assigned_roles.count; ++index) {
        const auto* role = chat_state_store_find_role(state, guild_id, assigned_roles[index]);
        if (role && role->color &&
            (!result || role_rank_above(*role, result->position, result->id)))
            result = role;
    }
    return result;
}

inline bool role_display_before(const ChatStateStore& state, const RoleRecord& left,
                                        const RoleRecord& right) noexcept {
    if (left.position != right.position)
        return left.position > right.position;
    const Utf8View left_name = chat_state_store_role_name(state, left);
    const Utf8View right_name = chat_state_store_role_name(state, right);
    const u32 bytes = minimum(left_name.bytes, right_name.bytes);
    for (u32 index = 0; index < bytes; ++index) {
        const u8 left_byte = static_cast<u8>(left_name.data[index]);
        const u8 right_byte = static_cast<u8>(right_name.data[index]);
        if (left_byte != right_byte)
            return left_byte < right_byte;
    }
    if (left_name.bytes != right_name.bytes)
        return left_name.bytes < right_name.bytes;
    return left.id < right.id;
}

inline const RoleRecord* role_display_next(
    const ChatStateStore& state, u64 guild_id, ConstSlice<RoleRecord> roles,
    const RoleRecord* previous = nullptr) noexcept {
    const RoleRecord* next = nullptr;
    for (u32 index = 0; index < roles.count; ++index) {
        const auto& role = roles[index];
        if (role.guild_id != guild_id || (previous && !role_display_before(state, *previous, role)))
            continue;
        if (!next || role_display_before(state, role, *next))
            next = &role;
    }
    return next;
}

// Sort only transient ordinal indices; the lookup backing and borrowed names stay
// in place. The comparator parameter also permits deterministic work-bound tests.
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
            (!policy.hierarchy_known || own.position > policy.highest_position ||
             (own.position == policy.highest_position && own.id < policy.highest_role_id))) {
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
           (policy.owner || (policy.hierarchy_known && (policy.highest_position > role.position ||
                                                        (policy.highest_position == role.position &&
                                                         policy.highest_role_id < role.id))));
}

inline bool role_editable(const ChatStateStore& state, u64 guild_id, u64 role_id) noexcept {
    const auto* role = chat_state_store_find_role(state, guild_id, role_id);
    return role && role_editable(role_edit_policy(state, guild_id), *role);
}
} // namespace maicord
