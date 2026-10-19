#include "maicord/model/guild_ban_store.hpp"
#include "maicord/model/member_moderation.hpp"
#include "maicord/model/guild_settings_model.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace permission_compute_test {

u32 failures;
void check(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}

constexpr u64 kGuild = 100, kOwner = 1, kSelf = 7, kOther = 8, kChannel = 500, kThread = 501;
constexpr u64 kRoleModerator = 10, kRoleAdmin = 11, kRoleBanner = 12, kRoleLow = 13, kRoleHigh = 14;
constexpr PermissionSet kEveryone = kPermissionViewChannel | kPermissionSendMessages | kPermissionReadMessageHistory |
                                    kPermissionChangeNickname;

// Pure resolver vectors ---------------------------------------------------

RoleRecord role(u64 id, PermissionSet permissions, u32 position = 1, u64 guild = kGuild) noexcept {
    RoleRecord value{};
    value.id = id;
    value.guild_id = guild;
    value.permissions = permissions;
    value.position = position;
    return value;
}

void test_vectors() noexcept {
    const RoleRecord roles[] = {
        role(kGuild, kEveryone, 0),
        role(kRoleModerator, kPermissionManageMessages | kPermissionKickMembers, 5),
        role(kRoleAdmin, kPermissionAdministrator, 9),
        role(kRoleBanner, kPermissionBanMembers | kPermissionAttachFiles, 3),
        role(kRoleModerator, kPermissionAdministrator, 5, 999), // other guild, same ID
    };
    const PermissionOverwriteRecord overwrites[] = {
        {kGuild, 0, kPermissionSendMessages, 0},
        {kRoleModerator, kPermissionSendMessages, kPermissionReadMessageHistory | kPermissionAttachFiles, 0},
        {kRoleBanner, kPermissionAttachFiles, 0, 0},
        {kSelf, kPermissionReadMessageHistory, kPermissionManageMessages, 1},
        {kOther, kPermissionAdministrator, 0, 0}, // a role overwrite for an ID nobody holds
    };
    const PermissionGuild guild{kGuild, kOwner, {roles, 5}, false};
    const PermissionOptions plain{};
    const ConstSlice<PermissionOverwriteRecord> channel{overwrites, 5};

    const PermissionMember owner{kOwner, {}, 0};
    check(permission_compute(guild, owner, {}, plain) == kPermissionAll &&
              permission_compute(guild, owner, channel, plain) == kPermissionAll,
          "permission compute: the owner holds every bit at guild and channel level");

    const u64 admin_roles[] = {kRoleAdmin};
    const PermissionMember admin{kOther, {admin_roles, 1}, 0};
    check(permission_compute(guild, admin, channel, plain) == kPermissionAll,
          "permission compute: administrator ignores every channel overwrite");

    const PermissionMember plain_member{kOther, {}, 0};
    check(permission_compute_base(guild, plain_member) == kEveryone &&
              permission_compute(guild, plain_member, channel, plain) == (kEveryone & ~kPermissionSendMessages),
          "permission compute: everyone role and everyone overwrite apply without roles; foreign guild rows "
          "are ignored");

    const u64 moderator_roles[] = {kRoleModerator, 999'999};
    const PermissionMember self{kSelf, {moderator_roles, 2}, 0};
    const PermissionSet base = kEveryone | kPermissionManageMessages | kPermissionKickMembers;
    check(permission_compute_base(guild, self) == base,
          "permission compute: base is the union of everyone and assigned roles; unknown IDs are ignored");
    const PermissionSet expected = (base & ~kPermissionManageMessages) | kPermissionSendMessages |
                                   kPermissionReadMessageHistory;
    check(permission_compute(guild, self, channel, plain) == expected,
          "permission compute: everyone deny, then role allow, then member allow/deny in service order");

    const u64 both_roles[] = {kRoleModerator, kRoleBanner};
    const PermissionMember both{kOther, {both_roles, 2}, 0};
    check((permission_compute(guild, both, channel, plain) & kPermissionAttachFiles) != 0,
          "permission compute: role allow aggregate wins over another role's deny");

    const u64 now = 1'000'000;
    const PermissionMember timed{kSelf, {moderator_roles, 2}, now + 60};
    const PermissionMember expired{kSelf, {moderator_roles, 2}, now};
    check(permission_compute(guild, timed, channel, {now, false}) ==
                  (expected & kPermissionTimeoutAllowedMask) &&
              permission_compute(guild, timed, {}, {now, false}) == (base & kPermissionTimeoutAllowedMask) &&
              permission_compute(guild, timed, channel, plain) == expected &&
              permission_compute(guild, expired, channel, {now, false}) == expected,
          "permission compute: an active timeout keeps only view and history at guild and channel level; "
          "expired or clockless timeouts are not masked");
    const PermissionMember timed_admin{kOther, {admin_roles, 1}, now + 60};
    check(permission_compute(guild, timed_admin, channel, {now, false}) == kPermissionAll,
          "permission compute: administrators are never timeout-masked");
    const PermissionOverwriteRecord admin_allow[] = {
        {kSelf, kPermissionAdministrator | kPermissionManageMessages, 0, 1},
    };
    check(permission_compute_from_base(base, guild, timed, {admin_allow, 1}, {now, false}) ==
              (base | kPermissionAdministrator),
          "permission base: an administrator overwrite prevents the timeout mask without expansion to every bit");
    const PermissionMember quarantined{kSelf, {moderator_roles, 2}, 0, true};
    const PermissionMember quarantined_timed{kSelf, {moderator_roles, 2}, now + 60, true};
    const PermissionMember quarantined_owner{kOwner, {}, now + 60, true};
    const PermissionMember quarantined_admin{kOther, {admin_roles, 1}, now + 60, true};
    const PermissionOverwriteRecord denied_history[] = {{kSelf, kPermissionSendMessages,
                                                        kPermissionReadMessageHistory, 1}};
    check(permission_compute(guild, quarantined, {}, plain) == kPermissionQuarantineAllowedMask &&
              permission_compute(guild, quarantined, channel, plain) == kPermissionQuarantineAllowedMask &&
              permission_compute(guild, quarantined, {denied_history, 1}, plain) ==
                  (kPermissionViewChannel | kPermissionChangeNickname) &&
              permission_compute(guild, quarantined_timed, channel, {now, false}) ==
                  kPermissionTimeoutAllowedMask &&
              permission_compute(guild, quarantined_owner, channel, {now, false}) == kPermissionAll &&
              permission_compute(guild, quarantined_admin, channel, {now, false}) == kPermissionAll,
          "permission compute: clockless quarantine restricts after overwrites, retains nickname without "
          "granting denied permissions, composes with timeout and exempts owners and administrators");

    const PermissionGuild strict{kGuild, kOwner, {roles, 5}, true};
    check(permission_compute(strict, owner, {}, {0, true}) == (kPermissionAll & ~kPermissionElevatedMask) &&
              permission_compute(strict, self, {}, {0, true}) ==
                  (base & ~(kPermissionKickMembers | kPermissionManageMessages)) &&
              permission_compute(strict, self, {}, {0, false}) == base &&
              permission_compute(guild, self, {}, {0, true}) == base,
          "permission compute: the two-factor gate strips elevated bits for everyone including the owner, "
          "only when the guild requires it and the member lacks it");
    check(permission_has(kPermissionAdministrator, kPermissionBanMembers) &&
              !permission_has(kPermissionKickMembers, kPermissionKickMembers | kPermissionBanMembers) &&
              !(kPermissionKnownMask & (1ull << 47)) && (kPermissionKnownMask & kPermissionManageOfficialMessages),
          "permission bits: administrator implies everything; known mask skips the unassigned bit");
}

// Store-backed resolution -------------------------------------------------

struct Model {
    ChatStateStore state;
    GuildRecord guilds[2];
    ChannelRecord channels[4];
    RoleRecord roles[16];
    PermissionOverwriteRecord overwrites[16];
    u16 guild_order[2], channel_order[4];
    u8 names[8192];
    GuildMemberStore members;
    GuildMemberRecord member_records[8];
    u32 member_order[8];
};
Model model;

bool add_role(u64 id, PermissionSet permissions, u32 position) noexcept {
    RoleInput input{id, kGuild, permissions, utf8_literal("r"), position};
    return chat_state_store_upsert_role(&model.state, input);
}

bool build_model(u64 owner) noexcept {
    clear_bytes(&model, sizeof(model));
    auto& state = model.state;
    if (!chat_state_store_initialize(&state, {model.guilds, 2}, {model.guild_order, 2}, {model.channels, 4},
                                        {model.channel_order, 4}, {model.names, sizeof(model.names)}) ||
        !chat_state_store_bind_permissions(&state, {model.roles, 16}, {model.overwrites, 16}) ||
        !guild_member_store_initialize(&model.members, {model.member_records, 8}, {model.member_order, 8}))
        return false;
    state.self_user_id = kSelf;
    if (!chat_state_store_upsert_guild(&state, {kGuild, utf8_literal("g"), GuildMutationKind::create, owner})
             .ok() ||
        !chat_state_store_upsert_channel(&state, {kChannel, kGuild, 0, utf8_literal("c"), 0, 0, 0}).ok() ||
        !chat_state_store_upsert_channel(&state, {kThread, kGuild, kChannel, utf8_literal("t"), 0, 0, 11}).ok() ||
        !add_role(kGuild, kEveryone, 0) ||
        !add_role(kRoleModerator,
                  kPermissionManageMessages | kPermissionKickMembers | kPermissionBanMembers |
                      kPermissionModerateMembers | kPermissionManageNicknames,
                  5) ||
        !add_role(kRoleAdmin, kPermissionAdministrator, 2) || !add_role(kRoleLow, 0, 3) ||
        !add_role(kRoleHigh, 0, 9))
        return false;
    const PermissionOverwriteInput deny_send{kGuild, 0, kPermissionSendMessages, 0};
    const PermissionOverwriteInput moderator{kRoleModerator, kPermissionSendMessages, 0, 0};
    if (!chat_state_store_append_channel_overwrite(&state, kChannel, deny_send) ||
        !chat_state_store_append_channel_overwrite(&state, kChannel, moderator))
        return false;
    const u64 self_roles[] = {kRoleModerator};
    return chat_state_store_set_self_roles(&state, kGuild, owner, kSelf, {self_roles, 1});
}

bool add_member(u64 user, ConstSlice<u64> roles, u64 timeout = 0) noexcept {
    const auto added = guild_member_store_upsert(&model.members, model.state, kGuild, user, {}, roles, true,
                                                         GuildMemberStatus::online);
    return added.ok() && (!timeout || guild_member_store_set_timeout(&model.members, kGuild, user, timeout));
}

void test_store() noexcept {
    check(build_model(kOwner), "permission store: fixture builds");
    auto& state = model.state;
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    const auto* guild = chat_state_store_find_guild(state, kGuild);
    const auto* channel = chat_state_store_find_channel(state, kChannel);
    const u64 self_roles[] = {kRoleModerator};
    const PermissionMember self{kSelf, {self_roles, 1}, 0};
    const PermissionSet expected_channel = chat_state_store_member_permissions(state, kGuild, kChannel, self, {});
    check(guild && channel && guild->permissions_resolved && channel->permissions_resolved &&
              channel->effective_permissions == expected_channel &&
              (channel->effective_permissions & kPermissionSendMessages) &&
              guild->base_permissions == chat_state_store_member_permissions(state, kGuild, 0, self, {}) &&
              chat_state_store_member_permissions(state, kGuild, kThread, self, {}) == expected_channel,
          "permission store: the self resolver matches the shared resolver; threads resolve through the parent");
    const PermissionMember plain{kOther, {}, 0};
    check(!(chat_state_store_member_permissions(state, kGuild, kChannel, plain, {}) & kPermissionSendMessages) &&
              chat_state_store_member_permissions(state, kGuild, 999, plain, {}) == 0 &&
              chat_state_store_member_permissions(state, 999, 0, plain, {}) == 0,
          "permission store: other members resolve from their own roles; unknown channel or guild is zero");

    GuildSettings settings{};
    settings.known = guild_settings_mfa_level | guild_settings_afk_timeout;
    settings.mfa_level = 1;
    settings.afk_timeout_seconds = 300;
    state.self_mfa = SelfMfaState::disabled;
    check(chat_state_store_set_guild_settings(&state, kGuild, settings) &&
              !(guild->base_permissions & kPermissionKickMembers) &&
              !(channel->actions & channel_action_manage_messages) &&
              chat_state_store_self_mfa_blocked(state, kGuild) && guild->settings.afk_timeout_seconds == 300,
          "permission store: an elevated two-factor requirement re-resolves and strips moderation for a "
          "non-2FA account");
    state.self_mfa = SelfMfaState::enabled;
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    check((guild->base_permissions & kPermissionKickMembers) && !chat_state_store_self_mfa_blocked(state, kGuild),
          "permission store: enabling two-factor restores the elevated permissions");
    GuildSettings invalid{};
    invalid.known = guild_settings_mfa_level;
    invalid.mfa_level = 2;
    GuildSettings partial{};
    partial.known = guild_settings_verification_level;
    partial.verification_level = 3;
    check(!chat_state_store_set_guild_settings(&state, kGuild, invalid) && guild->settings.mfa_level == 1 &&
              chat_state_store_set_guild_settings(&state, kGuild, partial) &&
              guild->settings.verification_level == 3 && guild->settings.mfa_level == 1 &&
              guild->settings.afk_timeout_seconds == 300 &&
              chat_state_store_set_self_timeout(&state, kGuild, 12345) &&
              guild->settings.self_communication_disabled_until == 12345,
          "permission store: settings are partial observations; invalid levels are refused without change");
    GuildSettings quarantine{};
    quarantine.known = guild_settings_self_quarantine;
    quarantine.self_member = guild_self_member_quarantined;
    const bool quarantined = chat_state_store_set_guild_settings(&state, kGuild, quarantine) &&
                             guild->base_permissions == kPermissionQuarantineAllowedMask &&
                             channel->effective_permissions == kPermissionQuarantineAllowedMask &&
                             !(channel->actions & (channel_action_send_messages | channel_action_manage_messages));
    const auto settings_access = guild_settings_access(state, kGuild);
    const auto self_actions = moderation_capabilities(state, model.members, kGuild, kSelf, 0);
    const auto other_actions = moderation_capabilities(state, model.members, kGuild, kOther, 0);
    check(quarantined && settings_access.permissions == kPermissionQuarantineAllowedMask &&
              !settings_access.sections && self_actions.allowed == moderation_action_nickname &&
              !other_actions.visible && !other_actions.allowed,
          "permission store: quarantine hides server administration and other-member moderation while "
          "retaining the current member's nickname repair");
    GuildSettings pending{};
    pending.known = guild_settings_self_member;
    pending.self_member = guild_self_member_pending;
    const bool preserved = chat_state_store_set_guild_settings(&state, kGuild, pending) &&
                           guild_self_quarantined(guild->settings) &&
                           channel->effective_permissions == kPermissionQuarantineAllowedMask;
    quarantine.self_member = 0;
    check(quarantined && preserved && chat_state_store_set_guild_settings(&state, kGuild, quarantine) &&
              !guild_self_quarantined(guild->settings) &&
              (guild->settings.self_member & guild_self_member_pending) &&
              (channel->effective_permissions & kPermissionSendMessages) &&
              (channel->actions & channel_action_manage_messages),
          "permission store: quarantine changes recompute actions, pending-only updates preserve quarantine, "
          "and explicit release restores role permissions without clearing pending");
}

void test_moderation() noexcept {
    check(build_model(kOwner), "moderation: fixture builds");
    auto& state = model.state;
    state.self_mfa = SelfMfaState::enabled;
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    const u64 self_roles[] = {kRoleModerator}, low[] = {kRoleLow}, high[] = {kRoleHigh}, admin[] = {kRoleAdmin};
    u64 many[kGuildMemberRoleIdCapacity + 1]{};
    for (u32 i = 0; i <= kGuildMemberRoleIdCapacity; ++i)
        many[i] = kRoleLow;
    check(add_member(kSelf, {self_roles, 1}) && add_member(20, {low, 1}) && add_member(21, {high, 1}) &&
              add_member(22, {admin, 1}) && add_member(23, {}, 5'000) && add_member(kOwner, {}) &&
              add_member(24, {many, kGuildMemberRoleIdCapacity + 1}),
          "moderation: members cached");
    const auto* truncated = guild_member_store_find(model.members, kGuild, 24);
    check(truncated && !guild_member_store_role_ids_known(*truncated) &&
              !guild_member_store_role_ids(model.members, *truncated).count &&
              !guild_member_store_set_timeout(&model.members, kGuild, 99, 1),
          "member store: more roles than retained is unknown; timeouts of uncached members are refused");
    constexpr u8 everything = moderation_action_nickname | moderation_action_timeout | moderation_action_kick |
                              moderation_action_ban;
    const auto below = moderation_capabilities(state, model.members, kGuild, 20, 1'000);
    check(below.visible == everything && below.allowed == everything && below.block == ModerationBlock::none,
          "moderation: every held action applies to a member below our highest role");
    const auto above = moderation_capabilities(state, model.members, kGuild, 21, 1'000);
    check(above.visible == everything && !above.allowed && above.block == ModerationBlock::hierarchy,
          "moderation: a higher role blocks every action but keeps them visible");
    const auto admin_target = moderation_capabilities(state, model.members, kGuild, 22, 1'000);
    check(admin_target.allowed == (everything & ~moderation_action_timeout) &&
              admin_target.block == ModerationBlock::target_admin,
          "moderation: administrators below us can be removed but not timed out");
    const auto timed = moderation_capabilities(state, model.members, kGuild, 23, 1'000);
    const auto expired = moderation_capabilities(state, model.members, kGuild, 23, 6'000);
    check(timed.target_timed_out && (timed.allowed & moderation_action_end_timeout) &&
              !expired.target_timed_out && !(expired.visible & moderation_action_end_timeout),
          "moderation: ending a timeout is offered only while it is active");
    const auto owner_target = moderation_capabilities(state, model.members, kGuild, kOwner, 1'000);
    const auto unknown = moderation_capabilities(state, model.members, kGuild, 24, 1'000);
    check(!owner_target.allowed && owner_target.block == ModerationBlock::target_owner && !unknown.allowed &&
              unknown.block == ModerationBlock::unresolved,
          "moderation: the owner is never a target; unknown roles fail closed");
    const auto self = moderation_capabilities(state, model.members, kGuild, kSelf, 1'000);
    check(self.self_target && self.allowed == moderation_action_nickname && self.nickname_self,
          "moderation: only the own nickname is offered for ourselves");

    GuildSettings strict{};
    strict.known = guild_settings_mfa_level;
    strict.mfa_level = 1;
    state.self_mfa = SelfMfaState::disabled;
    chat_state_store_set_guild_settings(&state, kGuild, strict);
    const auto gated = moderation_capabilities(state, model.members, kGuild, 20, 1'000);
    check(gated.visible == everything && gated.allowed == moderation_action_nickname &&
              gated.block == ModerationBlock::two_factor,
          "moderation: the two-factor requirement leaves only nickname changes");

    check(build_model(kSelf), "moderation: owner fixture builds");
    chat_state_store_recompute_guild_permissions(&model.state, kGuild);
    check(add_member(21, {high, 1}) && add_member(24, {many, kGuildMemberRoleIdCapacity + 1}),
          "moderation: owner members cached");
    const auto owner_view = moderation_capabilities(model.state, model.members, kGuild, 21, 1'000);
    const auto owner_unknown = moderation_capabilities(model.state, model.members, kGuild, 24, 1'000);
    check(owner_view.allowed == everything && owner_unknown.allowed == (everything & ~moderation_action_timeout),
          "moderation: the owner bypasses hierarchy; timeouts still need the target's roles");
}

// Ban cache ---------------------------------------------------------------

GuildAdminBan ban(u64 id) noexcept {
    GuildAdminBan value{};
    value.user.id = id;
    value.user.username[0] = 'u';
    value.user.username_bytes = 1;
    return value;
}

void test_bans() noexcept {
    GuildBanRecord records[4]{};
    GuildBanStore store{};
    check(guild_ban_store_bind(&store, {records, 4}), "bans: bind");
    guild_ban_store_reset(&store, kGuild);
    const GuildAdminBan first[] = {ban(10), ban(20)};
    const GuildAdminBan unordered[] = {ban(15)};
    const GuildAdminBan second[] = {ban(30)};
    check(guild_ban_store_append_page(&store, kGuild, {first, 2}, false) &&
              !guild_ban_store_append_page(&store, kGuild, {unordered, 1}, false) &&
              !guild_ban_store_append_page(&store, 999, {second, 1}, false) && store.count == 2 &&
              guild_ban_store_next_after(store) == 20,
          "bans: pages continue the sorted prefix; foreign and out-of-order pages are refused");
    check(guild_ban_store_apply_add(&store, kGuild, 40, utf8_literal("late")) && store.count == 2 &&
              guild_ban_store_apply_add(&store, kGuild, 15, utf8_literal("mid")) && store.count == 3 &&
              guild_ban_store_find(store, 15) && guild_ban_store_apply_remove(&store, kGuild, 10) &&
              !guild_ban_store_find(store, 10) && store.count == 2,
          "bans: gateway additions inside the loaded prefix insert in order; later ones wait for paging");
    check(guild_ban_store_append_page(&store, kGuild, {second, 1}, true) && store.complete &&
              guild_ban_store_apply_add(&store, kGuild, 40, utf8_literal("late")) && store.count == 4 &&
              !guild_ban_store_apply_add(&store, kGuild, 50, {}) && store.count == 4,
          "bans: once complete every addition is resident until capacity");
}

} // namespace permission_compute_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_permission_compute_entry() noexcept {
    using namespace maicord::permission_compute_test;
    test_vectors();
    test_store();
    test_moderation();
    test_bans();
    constexpr char pass[] = "PASS permission compute and moderation policy\n";
    constexpr char fail[] = "FAIL permission compute and moderation policy\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
