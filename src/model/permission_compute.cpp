#include "maicord/model/permission_compute.hpp"
#include "maicord/model/role_policy.hpp"

namespace maicord {
namespace {

bool has_role(const PermissionMember& member, u64 role_id) noexcept {
    for (u32 i = 0; i < member.role_ids.count; ++i)
        if (member.role_ids[i] == role_id)
            return true;
    return false;
}

ConstSlice<RoleRecord> store_roles(const ChatStateStore& store) noexcept {
    return store.roles ? ConstSlice<RoleRecord>{store.roles, store.role_count}
                       : ConstSlice<RoleRecord>{};
}

} // namespace

bool permission_member_timed_out(const PermissionMember& member, u64 now_seconds) noexcept {
    return now_seconds && member.communication_disabled_until > now_seconds;
}

PermissionSet permission_compute_base(const PermissionGuild& guild, const PermissionMember& member) noexcept {
    if (guild.owner_id && member.user_id == guild.owner_id)
        return kPermissionAll;
    PermissionSet base = 0;
    for (u32 i = 0; i < guild.roles.count; ++i) {
        const auto& role = guild.roles[i];
        if (role.guild_id == guild.guild_id && (role.id == guild.guild_id || has_role(member, role.id)))
            base |= role.permissions;
    }
    return (base & kPermissionAdministrator) ? kPermissionAll : base;
}

PermissionSet permission_apply_overwrites(PermissionSet base, u64 guild_id, const PermissionMember& member,
                                          ConstSlice<PermissionOverwriteRecord> overwrites) noexcept {
    if (base & kPermissionAdministrator)
        return kPermissionAll;
    const PermissionOverwriteRecord* everyone = nullptr;
    const PermissionOverwriteRecord* own = nullptr;
    PermissionSet role_allow = 0, role_deny = 0;
    for (u32 i = 0; i < overwrites.count; ++i) {
        const auto& overwrite = overwrites[i];
        if (overwrite.type == 0 && overwrite.id == guild_id)
            everyone = &overwrite;
        else if (overwrite.type == 0 && has_role(member, overwrite.id)) {
            role_allow |= overwrite.allow;
            role_deny |= overwrite.deny;
        } else if (overwrite.type == 1 && member.user_id && overwrite.id == member.user_id)
            own = &overwrite;
    }
    PermissionSet permissions = base;
    if (everyone)
        permissions = (permissions & ~everyone->deny) | everyone->allow;
    permissions = (permissions & ~role_deny) | role_allow;
    if (own)
        permissions = (permissions & ~own->deny) | own->allow;
    return permissions;
}

PermissionSet permission_compute_from_base(PermissionSet permissions, const PermissionGuild& guild,
                                           const PermissionMember& member,
                                           ConstSlice<PermissionOverwriteRecord> overwrites,
                                           const PermissionOptions& options) noexcept {
    const bool owner = guild.owner_id && member.user_id == guild.owner_id;
    if (!owner && permissions != kPermissionAll) {
        permissions = permission_apply_overwrites(permissions, guild.guild_id, member, overwrites);
        if (!(permissions & kPermissionAdministrator)) {
            if (permission_member_timed_out(member, options.now_seconds))
                permissions &= kPermissionTimeoutAllowedMask;
            if (member.quarantined)
                permissions &= kPermissionQuarantineAllowedMask;
        }
    }
    if (guild.elevated_requires_mfa && options.member_lacks_mfa)
        permissions &= ~kPermissionElevatedMask;
    return permissions;
}

PermissionSet permission_compute(const PermissionGuild& guild, const PermissionMember& member,
                                 ConstSlice<PermissionOverwriteRecord> overwrites,
                                 const PermissionOptions& options) noexcept {
    return permission_compute_from_base(permission_compute_base(guild, member), guild, member,
                                        overwrites, options);
}

PermissionGuild chat_state_store_permission_guild(const ChatStateStore& store, u64 guild_id) noexcept {
    const auto* guild = chat_state_store_find_guild(store, guild_id);
    if (!guild)
        return {};
    return {guild->id, guild->owner_id, store_roles(store),
            (guild->settings.known & guild_settings_mfa_level) && guild->settings.mfa_level == 1};
}

PermissionSet chat_state_store_member_permissions(const ChatStateStore& store, u64 guild_id,
                                                     u64 channel_id, const PermissionMember& member,
                                                     const PermissionOptions& options) noexcept {
    const PermissionGuild guild = chat_state_store_permission_guild(store, guild_id);
    if (!guild.guild_id || !member.user_id)
        return 0;
    ConstSlice<PermissionOverwriteRecord> overwrites{};
    if (channel_id) {
        const auto* channel = chat_state_store_find_channel(store, channel_id);
        if (channel && channel_is_thread(channel->type))
            channel = chat_state_store_find_channel(store, channel->parent_id);
        if (!channel || channel->guild_id != guild_id)
            return 0;
        if (store.overwrites && channel->overwrite_count &&
            channel->overwrite_offset + channel->overwrite_count <= store.overwrite_count)
            overwrites = {store.overwrites + channel->overwrite_offset, channel->overwrite_count};
    }
    return permission_compute(guild, member, overwrites, options);
}

bool chat_state_store_self_mfa_blocked(const ChatStateStore& store, u64 guild_id) noexcept {
    const auto* guild = chat_state_store_find_guild(store, guild_id);
    return guild && (guild->settings.known & guild_settings_mfa_level) && guild->settings.mfa_level == 1 &&
           store.self_mfa == SelfMfaState::disabled;
}

const RoleRecord* chat_state_store_highest_role(const ChatStateStore& store, u64 guild_id,
                                                          ConstSlice<u64> role_ids) noexcept {
    const RoleRecord* highest = nullptr;
    for (u32 i = 0; i < role_ids.count; ++i) {
        const auto* role = role_ids[i] != guild_id ? chat_state_store_find_role(store, guild_id, role_ids[i])
                                                   : nullptr;
        if (role && (!highest || role_rank_above(*role, highest->position, highest->id)))
            highest = role;
    }
    return highest;
}

} // namespace maicord
