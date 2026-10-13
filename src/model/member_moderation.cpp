#include "maicord/model/member_moderation.hpp"
#include "maicord/model/role_policy.hpp"

namespace maicord {

ModerationCapabilities moderation_capabilities(const ChatStateStore& state,
                                               const GuildMemberStore& members, u64 guild_id,
                                               u64 target_user_id, u64 now_seconds) noexcept {
    ModerationCapabilities result{};
    const auto* guild = chat_state_store_find_guild(state, guild_id);
    const u64 self = state.self_user_id;
    if (!guild || guild->unavailable || !self || !target_user_id) {
        result.block = ModerationBlock::unresolved;
        return result;
    }
    result.self_target = target_user_id == self;
    const auto* target = guild_member_store_find(members, guild_id, target_user_id);
    result.target_timed_out = target && target->communication_disabled_known &&
                              target->communication_disabled_until > now_seconds;
    if (!guild->permissions_resolved || !guild->owner_id) {
        result.block = ModerationBlock::unresolved;
        return result;
    }

    // Our own permissions without the two-factor strip, so held-but-gated
    // actions stay visible and explain themselves.
    u64 self_roles[kGuildRoleMaximum];
    u32 self_count = 0;
    for (u32 i = 0; i < state.role_count && self_count < kGuildRoleMaximum; ++i)
        if (state.roles[i].guild_id == guild_id && state.roles[i].id != guild_id && state.roles[i].self_assigned)
            self_roles[self_count++] = state.roles[i].id;
    const PermissionGuild scope = chat_state_store_permission_guild(state, guild_id);
    const PermissionMember me{self, {self_roles, self_count}, 0};
    const PermissionSet mine = permission_compute(scope, me, {}, {0, false});
    const bool owner = guild->owner_id == self;
    const bool mfa_blocked = chat_state_store_self_mfa_blocked(state, guild_id);

    if (result.self_target) {
        const bool nickname = permission_has(mine, kPermissionChangeNickname) ||
                              permission_has(mine, kPermissionManageNicknames);
        result.visible = result.allowed = nickname ? moderation_action_nickname : 0;
        result.nickname_self = nickname;
        return result;
    }

    if (permission_has(mine, kPermissionManageNicknames))
        result.visible |= moderation_action_nickname;
    if (permission_has(mine, kPermissionModerateMembers))
        result.visible |= result.target_timed_out ? moderation_action_timeout | moderation_action_end_timeout
                                                  : moderation_action_timeout;
    if (permission_has(mine, kPermissionKickMembers))
        result.visible |= moderation_action_kick;
    if (permission_has(mine, kPermissionBanMembers))
        result.visible |= moderation_action_ban;
    if (!result.visible)
        return result;

    if (target_user_id == guild->owner_id) {
        result.block = ModerationBlock::target_owner;
        return result;
    }
    // Bans need no membership; the hierarchy applies only to current members.
    const bool member_known = target && guild_member_store_role_ids_known(*target);
    if (!owner) {
        if (!member_known) {
            result.block = ModerationBlock::unresolved;
            return result;
        }
        const ConstSlice<u64> target_roles = guild_member_store_role_ids(*target);
        const auto* mine_top = chat_state_store_highest_role(state, guild_id, {self_roles, self_count});
        const auto* their_top = chat_state_store_highest_role(state, guild_id, target_roles);
        const bool above = mine_top && (!their_top || role_rank_above(*mine_top, their_top->position,
                                                                             their_top->id));
        if (!above) {
            result.block = ModerationBlock::hierarchy;
            return result;
        }
    }
    u8 allowed = result.visible;
    if (member_known) {
        const PermissionMember them{target_user_id, guild_member_store_role_ids(*target), 0};
        if (permission_compute_base(scope, them) & kPermissionAdministrator) {
            allowed &= static_cast<u8>(~moderation_action_timeout);
            result.block = ModerationBlock::target_admin;
        }
    } else if (allowed & moderation_action_timeout) {
        allowed &= static_cast<u8>(~moderation_action_timeout);
        result.block = ModerationBlock::unresolved;
    }
    if (mfa_blocked) {
        allowed &= static_cast<u8>(moderation_action_nickname);
        result.block = ModerationBlock::two_factor;
    }
    result.allowed = allowed;
    return result;
}

const Utf8View kModerationBanReasons[kModerationBanReasonCount]{
    utf8_literal("Suspicious or spam account"), utf8_literal("Compromised or hacked account"),
    utf8_literal("Breaking server rules"), utf8_literal("Other")};
const Utf8View kModerationBanDeleteLabels[sizeof(kModerationBanDeleteChoices) / sizeof(u32)]{
    utf8_literal("Don't Delete Any"),  utf8_literal("Previous Hour"),   utf8_literal("Previous 6 Hours"),
    utf8_literal("Previous 12 Hours"), utf8_literal("Previous 24 Hours"), utf8_literal("Previous 3 Days"),
    utf8_literal("Previous 7 Days")};

Utf8View moderation_ban_reason(u32 choice, bool typed, Utf8View text) noexcept {
    if (choice == kModerationBanReasonOther)
        return typed ? text : utf8_literal("other");
    return choice < kModerationBanReasonCount ? kModerationBanReasons[choice] : Utf8View{"", 0};
}

} // namespace maicord
