#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
namespace {
// Server header popout: owner, administrator and plain member through real
// role permissions.
struct HeaderFixture {
    ChatStateStore state;
    GuildRecord guilds[1];
    ChannelRecord channels[2];
    RoleRecord roles[4];
    PermissionOverwriteRecord overwrites[2];
    u16 guild_order[1], channel_order[2];
    u8 names[512];
};
HeaderFixture header_fixture;
bool header_build(u64 owner, PermissionSet self_permissions) noexcept {
    auto& f = header_fixture;
    f = {};
    constexpr u64 guild = 7, self = 5, role = 70;
    if (!chat_state_store_initialize(&f.state, {f.guilds, 1}, {f.guild_order, 1}, {f.channels, 2},
                                     {f.channel_order, 2}, {f.names, sizeof(f.names)}) ||
        !chat_state_store_bind_permissions(&f.state, {f.roles, 4}, {f.overwrites, 2}))
        return false;
    f.state.self_user_id = self;
    f.state.self_mfa = SelfMfaState::enabled;
    const u64 roles[]{role};
    if (!chat_state_store_upsert_guild(&f.state, {guild, utf8_literal("Lab"), GuildMutationKind::create, owner}).ok() ||
        !chat_state_store_upsert_channel(&f.state, {70, guild, 0, utf8_literal("general"), 0, 0, 0}).ok() ||
        !chat_state_store_upsert_role(&f.state, {guild, guild, kPermissionViewChannel | kPermissionSendMessages,
                                                 utf8_literal("@everyone"), 0}) ||
        !chat_state_store_upsert_role(&f.state, {role, guild, self_permissions, utf8_literal("role"), 1}) ||
        !chat_state_store_set_self_roles(&f.state, guild, owner, self, {roles, 1}))
        return false;
    chat_state_store_recompute_guild_permissions(&f.state, guild);
    return true;
}
bool header_rows(u16 capabilities, ConstSlice<u32> expected, u32 separators) noexcept {
    constexpr GuildMenuActions actions{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    PopupItem rows[kGuildHeaderMenuMaximumItems]{};
    const u32 count = guild_header_menu_build(header_fixture.state, 7, 70, actions, capabilities,
                                              {rows, kGuildHeaderMenuMaximumItems});
    if (count != expected.count)
        return false;
    for (u32 i = 0; i < count; ++i) {
        if (rows[i].action != expected[i] || !(rows[i].flags & popup_item_enabled) ||
            ((rows[i].flags & popup_item_separator_before) != 0) != ((separators >> i) & 1u))
            return false;
    }
    return true;
}
void test_guild_header_menu(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr u16 all = guild_menu_capability_create_event | guild_menu_capability_notifications |
                        guild_menu_capability_profile | guild_menu_capability_developer;
    // Row actions: invite 1, settings 2, create channel 3, create category 4, create event 5,
    // notifications 6, privacy 7, profile 8, hide muted 9, leave 10, copy ID 11.
    const u32 owner[]{1, 2, 3, 4, 5, 6, 7, 8, 9, 11};
    check(header_build(5, 0) && header_rows(all, {owner, 10}, (1u << 5) | (1u << 7) | (1u << 9)),
          "header popout: the owner gets every server row in the official order and groups, without Leave");
    const u32 admin[]{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    check(header_build(9, kPermissionAdministrator) &&
              header_rows(all, {admin, 11}, (1u << 5) | (1u << 7) | (1u << 9) | (1u << 10)),
          "header popout: an administrator also gets Leave server");
    const u32 member[]{6, 7, 9, 10};
    check(header_build(9, 0) &&
              header_rows(guild_menu_capability_notifications, {member, 4}, (1u << 2) | (1u << 3)),
          "header popout: a plain member gets no invite, settings or creation rows");
    const u32 nickname[]{1, 7, 8, 9, 10};
    check(header_build(9, kPermissionCreateInvite | kPermissionChangeNickname) &&
              header_rows(guild_menu_capability_profile, {nickname, 5}, (1u << 1) | (1u << 2) | (1u << 4)),
          "header popout: Create Invite and nickname rows follow their own capabilities");
    PopupItem rows[kGuildHeaderMenuMaximumItems]{};
    constexpr GuildMenuActions actions{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    check(chat_state_store_set_guild_hide_muted(&header_fixture.state, 7, true) &&
              guild_header_menu_build(header_fixture.state, 7, 0, actions, 0, {rows, kGuildHeaderMenuMaximumItems}) ==
                  4 &&
              rows[2].action == 9 && (rows[2].flags & popup_item_checked) && rows[3].action == 10 &&
              (rows[3].flags & popup_item_danger),
          "header popout: Hide muted channels shows its state and Leave server is a danger row");
    rows[0].action = 999;
    check(guild_header_menu_build(header_fixture.state, 7, 0, actions, 0, {rows, 3}) == 0 && rows[0].action == 999 &&
              guild_header_menu_build(header_fixture.state, 8, 0, actions, 0, {rows, 11}) == 0,
          "header popout: short storage or an unknown server publishes nothing");
    constexpr GuildMenuActions opt_in_actions{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const u32 community[]{12, 6, 7, 9, 10};
    PopupItem opt_rows[kGuildHeaderMenuMaximumItems]{};
    const u16 shown_all = guild_menu_capability_notifications | guild_menu_capability_show_all |
                          guild_menu_capability_show_all_checked;
    bool labels = header_build(9, 0) && guild_header_menu_build(header_fixture.state, 7, 0, opt_in_actions, shown_all,
                                          {opt_rows, kGuildHeaderMenuMaximumItems}) == 5;
    for (u32 i = 0; labels && i < 5; ++i)
        labels = opt_rows[i].action == community[i];
    check(labels && opt_rows[0].action == 12 && (opt_rows[0].flags & popup_item_checked) &&
              opt_rows[2].action == 7 && opt_rows[2].presentation.icon_plus_one == static_cast<u8>(static_cast<u32>(UiIcon::shield) + 1) &&
              guild_header_menu_build(header_fixture.state, 7, 0, opt_in_actions,
                                      guild_menu_capability_show_all, {opt_rows, kGuildHeaderMenuMaximumItems}) == 4 &&
              opt_rows[0].action == 12 && !(opt_rows[0].flags & popup_item_checked),
          "header popout: community opt-in adds Show all channels before Notification settings, checked while "
          "every channel shows; Privacy settings carries the shield");
}
} // namespace

void test_guild_context_policy(void (*check)(bool, const char*) noexcept) noexcept {
    ChatStateStore state{};
    GuildRecord guild{};
    ChannelRecord channel{};
    u16 guild_order[1]{}, channel_order[1]{};
    u8 names[128]{};
    check(chat_state_store_initialize(&state, {&guild, 1}, {guild_order, 1}, {&channel, 1},
                                         {channel_order, 1}, {names, sizeof(names)}) &&
              chat_state_store_upsert_guild(&state, {1, {}, GuildMutationKind::create, 99}).ok(),
          "guild scoped read policy fixture establishes a real live guild");
    PopupItem items[3]{};
    constexpr u32 copy = 801, mark = 802, retry = 803;
    check(guild_context_policy_build(state, 1, copy, {items, 3}) == 1 && items[0].action == copy &&
              items[0].flags == popup_item_enabled,
          "guild default policy preserves single enabled Copy server ID row");
    guild.permissions_resolved = true;
    UiScopedReadMenuState scoped{mark, retry, true, false, false};
    check(guild_context_policy_build(state, 1, copy, {items, 3}, scoped) == 2 && items[0].action == mark &&
              items[0].flags == popup_item_enabled && items[1].action == copy &&
              items[1].flags == (popup_item_enabled | popup_item_separator_before),
          "available server read action precedes separated Copy server ID with normal themed row");
    scoped.pending = true;
    scoped.retry_available = true;
    check(guild_context_policy_build(state, 1, copy, {items, 3}, scoped) == 2 && items[0].action == mark &&
              !(items[0].flags & popup_item_enabled) && items[1].action == copy &&
              (items[1].flags & popup_item_enabled),
          "pending server operation disables mutation and hides Retry while copy remains usable");
    scoped.pending = false;
    check(guild_context_policy_build(state, 1, copy, {items, 3}, scoped) == 3 && items[1].action == retry &&
              items[1].flags == popup_item_enabled,
          "failed current server intent receives a separate explicit Retry row");
    items[0] = {utf8_literal("unchanged"), 999, 0};
    items[1] = items[0];
    items[2] = items[0];
    check(guild_context_policy_build(state, 1, copy, {items, 2}, scoped) == 0 && items[0].action == 999 &&
              items[1].action == 999 && items[2].action == 999,
          "server read plus Retry capacity denial leaves every caller row unchanged");
    check(guild_context_policy_build(state, 1, copy, {items, 3}, {copy, retry, true}) == 0 &&
              guild_context_policy_build(state, 1, copy, {items, 3}, {mark, mark, true}) == 0 &&
              guild_context_policy_build(state, 1, copy, {items, 3}, {mark, copy, true}) == 0 &&
              guild_context_policy_build(state, 1, copy, {items, 3}, {0, retry, true}) == 0 &&
              guild_context_policy_build(state, 1, copy, {items, 3}, {mark, 0, true, false, true}) == 0 &&
              items[0].action == 999,
          "server mutation IDs cannot alias copy or each other and required IDs are prevalidated");
    scoped.available = false;
    check(guild_context_policy_build(state, 1, copy, {items, 3}, scoped) == 1 && items[0].action == copy &&
              items[0].flags == popup_item_enabled,
          "withdrawn server authority removes read and Retry affordances without disabling copy");
    scoped.available = true;
    guild.permissions_resolved = false;
    check(guild_context_policy_build(state, 1, copy, {items, 3}, scoped) == 1 && items[0].action == copy,
          "unresolved guild authority cannot expose scoped read even with stale host availability");
    guild.permissions_resolved = true;
    constexpr u32 settings = 804;
    scoped.retry_available = false;
    state.self_user_id = 99;
    PopupItem with_settings[4]{};
    check(guild_context_policy_build(state, 1, copy, {with_settings, 4}, scoped, settings) == 3 &&
              with_settings[1].action == settings &&
              with_settings[1].flags == (popup_item_enabled | popup_item_separator_before) &&
              with_settings[2].action == copy,
          "the owner gets Server settings between the read action and Copy server ID");
    state.self_user_id = 5;
    check(guild_context_policy_build(state, 1, copy, {with_settings, 4}, scoped, settings) == 2 &&
              with_settings[1].action == copy &&
              guild_context_policy_build(state, 1, copy, {with_settings, 4}, scoped, copy) == 0,
          "members without a settings permission get no entry and the action cannot alias copy");
    state.self_user_id = 99;
    check(guild_context_policy_build(state, 1, copy, {with_settings, 4}, scoped, settings, false) == 2 &&
              with_settings[0].action == mark && with_settings[1].action == settings &&
              guild_context_policy_build(state, 1, copy, {items, 3}, {}, 0, false) == 0 &&
              guild_context_target_valid(state, 1),
          "Copy server ID is offered only with Developer Mode on");
    state.self_user_id = 0;
    const auto generation = guild.generation;
    const auto revision = guild.revision;
    check(guild_context_target_current(state, 1, generation, revision) &&
              !guild_context_target_current(state, 1, static_cast<u16>(generation + 1), revision) &&
              !guild_context_target_current(state, 1, generation, revision + 1),
          "initial server menu source fences generation and revision independently");
    guild.unavailable = true;
    items[0].action = 999;
    check(guild_context_policy_build(state, 1, copy, {items, 3}, scoped) == 0 && items[0].action == 999 &&
              !guild_context_target_current(state, 1, generation, revision),
          "unavailable server removes entire native menu without publishing partial mutation rows");
    guild.unavailable = false;
    check(guild_context_policy_build(state, 0, copy, {items, 3}, scoped) == 0 &&
              guild_context_policy_build(state, 99, copy, {items, 3}, scoped) == 0 &&
              guild_context_policy_build(state, 1, 0, {items, 3}, scoped) == 0 &&
              guild_context_policy_build(state, 1, copy, {}, scoped) == 0,
          "Home unknown guild invalid copy ID and absent storage reject server menu admission");
    test_guild_header_menu(check);
}
} // namespace maicord
