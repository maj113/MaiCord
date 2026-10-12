#include "maicord/ui/channel_context_policy.hpp"
#include "maicord/model/permission_bits.hpp"

namespace maicord {
void test_channel_context_policy(void (*check)(bool, const char*) noexcept) noexcept {
    ChannelRecord channels[2]{};
    auto& channel = channels[0];
    u16 guild_order[1]{}, channel_order[2]{};
    u8 names[512]{};
    RoleRecord role{};
    PermissionOverwriteRecord overwrite{};
    ChatStateStore state{};
    GuildRecord guild{};
    constexpr u64 read_permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
    check(
        chat_state_store_initialize(&state, {&guild, 1}, {guild_order, 1}, {channels, 2},
                                       {channel_order, 2}, {names, sizeof(names)}) &&
            chat_state_store_bind_permissions(&state, {&role, 1}, {&overwrite, 1}) &&
            chat_state_store_upsert_guild(&state, {1, {}, GuildMutationKind::create, 99}).ok() &&
            chat_state_store_upsert_role(&state, {1, 1, read_permissions, {}}) &&
            chat_state_store_set_self_roles(&state, 1, 99, 42, {}) &&
            chat_state_store_upsert_channel(&state, {7, 1, 8, {}, 0, 0, 0}).ok() &&
            chat_state_store_upsert_channel(&state, {8, 1, 0, {}, 0, 0, 0}).ok(),
        "channel context fixture establishes real self roles and a readable thread parent");
    chat_state_store_recompute_guild_permissions(&state, 1);
    PopupItem items[2]{};
    constexpr u32 mark = 701, close = 702;
    const u8 readable_types[]{0, 5, 10, 11, 12};
    for (const u8 type : readable_types) {
        channel.type = type;
        if (type == 12)
            check(chat_state_store_set_thread_self_membership(&state, 7, 42, true),
                  "channel context private thread carries current-self joined proof");
        check(channel_context_policy_build(state, 7, mark, close, {items, 2}) == 1 &&
                  items[0].action == mark && items[0].flags == popup_item_enabled &&
                  utf8_equal(items[0].label, utf8_literal("Mark as read")),
              "channel context: readable text, announcement and thread channels expose only Mark as read");
    }
    channel.type = 1;
    channel.guild_id = 0;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}) == 2 && items[0].action == mark &&
              items[1].action == close && utf8_equal(items[1].label, utf8_literal("Close DM")),
          "channel context: direct DM orders Mark as read before Close DM");
    channel.type = 3;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}) == 1 && items[0].action == mark,
          "channel context: group DM can be marked read without becoming a close or leave action");
    channel.type = 1;
    channel.actions = channel_action_view;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}) == 1 && items[0].action == close,
          "channel context: DM retains existing close action without readable-history permission");
    channel.type = 0;
    channel.guild_id = 1;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}) == 0,
          "channel context: text channel without history permission has no read action");
    channel.actions |= channel_action_read_history;
    channel.permissions_resolved = false;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}) == 0,
          "channel context: unresolved guild permissions cannot expose a read action");
    channel.permissions_resolved = true;
    channel.effective_permissions = 0;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}) == 0,
          "channel context: hidden guild channel cannot expose a menu despite stale action bits");
    channel.effective_permissions = kPermissionViewChannel;
    channel.flags = kChannelObfuscatedFlag;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}) == 0,
          "channel context: obfuscated channel stays hidden from the native menu");
    channel.flags = 0;
    const u8 nonmessage_types[]{4, 15};
    for (const u8 type : nonmessage_types) {
        channel.type = type;
        check(channel_context_policy_build(state, 7, mark, close, {items, 2}) == 0,
              "channel context: category and forum containers get no empty read menu");
    }
    // Text in voice: voice and stage channels read like text channels.
    const u8 voice_types[]{2, 13};
    for (const u8 type : voice_types) {
        channel.type = type;
        check(channel_context_policy_build(state, 7, mark, close, {items, 2}) == 1 && items[0].action == mark,
              "channel context: readable voice and stage chats offer Mark as read");
    }
    channel.type = 1;
    channel.guild_id = 0;
    items[0] = {utf8_literal("unchanged"), 900, 0, 0};
    items[1] = items[0];
    check(channel_context_policy_build(state, 7, mark, close, {items, 1}) == 0 && items[0].action == 900 &&
              items[1].action == 900 && utf8_equal(items[0].label, utf8_literal("unchanged")),
          "channel context: insufficient output capacity leaves caller menu unchanged");
    check(channel_context_policy_build(state, 9, mark, close, {items, 2}) == 0 &&
              channel_context_policy_build(state, 7, 0, close, {items, 2}) == 0 &&
              channel_context_policy_build(state, 7, mark, mark, {items, 2}) == 0 &&
              channel_context_policy_build(state, 7, mark, close, {}) == 0 && items[0].action == 900,
          "channel context: missing identity, invalid action IDs and missing storage cannot publish "
          "partial items");

    constexpr u32 edit = 703;
    channel.type = 0;
    channel.guild_id = 1;
    channel.flags = 0;
    channel.permissions_resolved = true;
    channel.effective_permissions = kPermissionViewChannel | kPermissionManageChannels;
    channel.actions = channel_action_view | channel_action_manage_channel |
                      channel_action_read_history;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}, edit) == 2 &&
              items[0].action == mark && items[1].action == edit && items[1].flags == popup_item_enabled &&
              utf8_equal(items[1].label, utf8_literal("Edit channel")) &&
              ui_menu_row_icon(items[1].presentation) == UiIcon::settings,
          "channel context: manageable guild text channel appends Edit channel with original native cog");
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}) == 1 && items[0].action == mark,
          "channel context: omitted edit action preserves the old projection");
    channel.actions &= ~channel_action_read_history;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}, edit) == 1 &&
              items[0].action == edit,
          "channel context: management does not require readable-history permission");
    channel.permissions_resolved = false;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}, edit) == 0,
          "channel context: unresolved permissions deny edit despite stale action bits");
    channel.permissions_resolved = true;
    channel.effective_permissions = kPermissionViewChannel;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}, edit) == 0,
          "channel context: missing Manage Channels denies edit despite stale management action");
    channel.effective_permissions = kPermissionManageChannels;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}, edit) == 0,
          "channel context: management cannot expose a hidden channel");
    channel.effective_permissions |= kPermissionViewChannel;
    channel.actions = channel_action_view;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}, edit) == 0,
          "channel context: missing effective management action denies edit");
    channel.actions = channel_action_manage_channel;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}, edit) == 0,
          "channel context: missing effective View action denies edit");
    channel.actions |= channel_action_view;
    const u8 unsupported_edit_types[]{1, 3, 10, 11, 12};
    for (const u8 type : unsupported_edit_types) {
        channel.type = type;
        channel.guild_id = type == 1 || type == 3 ? 0 : 1;
        const u32 count = channel_context_policy_build(state, 7, mark, close, {items, 2}, edit);
        check(count == (type == 1 ? 1u : 0u) && (!count || items[0].action == close),
              "channel context: Channel Settings excludes private channels and threads");
    }
    const u8 settings_types[]{2, 4, 5, 13, 15};
    for (const u8 type : settings_types) {
        channel.type = type;
        channel.guild_id = 1;
        const u32 count = channel_context_policy_build(state, 7, mark, close, {items, 2}, edit);
        check(count == 1 && items[0].action == edit &&
                  utf8_equal(items[0].label, type == 4 ? utf8_literal("Edit category") : utf8_literal("Edit channel")),
              "channel context: voice, category, announcement, stage and forum channels open Channel Settings");
    }
    channel.type = 2;
    channel.effective_permissions = kPermissionViewChannel | kPermissionManageRoles;
    channel.actions = channel_action_view;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}, edit) == 1 && items[0].action == edit,
          "channel context: Manage Permissions alone opens Channel Settings");
    channel.effective_permissions = kPermissionViewChannel | kPermissionManageChannels;
    channel.actions = channel_action_view | channel_action_manage_channel;
    channel.type = 0;
    channel.guild_id = 0;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}, edit) == 0,
          "channel context: type-zero record without guild identity cannot expose edit");
    channel.guild_id = 1;
    channel.occupied = false;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}, edit) == 0,
          "channel context: retired record cannot expose edit");
    channel.occupied = true;
    channel.flags = kChannelObfuscatedFlag;
    check(channel_context_policy_build(state, 7, mark, close, {items, 2}, edit) == 0,
          "channel context: obfuscated manageable record cannot expose edit");
    channel.flags = 0;
    channel.generation = 4;
    channel.revision = 9;
    check(channel_context_target_current(state, 7, 4, 9) &&
              !channel_context_target_current(state, 7, 3, 9) &&
              !channel_context_target_current(state, 7, 4, 8),
          "channel context: edit host can fence stale generation and revision before dispatch");
    items[0] = {utf8_literal("unchanged"), 900, 0, 0};
    items[1] = items[0];
    channel.actions |= channel_action_read_history;
    check(channel_context_policy_build(state, 7, mark, close, {items, 1}, edit) == 0 &&
              items[0].action == 900 && items[1].action == 900 &&
              channel_context_policy_build(state, 7, mark, close, {items, 2}, mark) == 0 &&
              channel_context_policy_build(state, 7, mark, close, {items, 2}, close) == 0 &&
              items[0].action == 900,
          "channel context: edit capacity and duplicate action rejection preserve caller output");
    constexpr u32 unread = 704;
    PopupItem extended[3]{};
    channel.last_message_id = 900;
    channel.type = 0;
    channel.guild_id = 1;
    channel.occupied = true;
    channel.flags = 0;
    channel.permissions_resolved = true;
    channel.effective_permissions = kPermissionViewChannel | kPermissionReadMessageHistory |
                                    kPermissionManageChannels;
    channel.actions = channel_action_view | channel_action_read_history |
                      channel_action_manage_channel;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, edit, unread) == 3 &&
              extended[0].action == mark && extended[1].action == unread && extended[2].action == edit &&
              utf8_equal(extended[1].label, utf8_literal("Mark as unread")) &&
              extended[1].flags == popup_item_enabled,
          "channel context opts into unread between read and edit without dropping existing actions");
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, edit) == 2 &&
              extended[0].action == mark && extended[1].action == edit,
          "omitted unread action preserves legacy manageable-channel projection");
    channel.type = 1;
    channel.guild_id = 0;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, unread) == 3 &&
              extended[0].action == mark && extended[1].action == unread && extended[2].action == close,
          "readable DM keeps Close DM after both explicit read cursor actions");
    channel.last_message_id = 0;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, unread) == 2 &&
              extended[1].action == close,
          "empty channel has no fabricated unread action");
    channel.last_message_id = 900;
    channel.actions &= ~channel_action_read_history;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, unread) == 1 &&
              extended[0].action == close,
          "unread never bypasses revoked readable-history permission");
    channel.type = 0;
    channel.guild_id = 1;
    channel.actions |= channel_action_read_history;
    guild.unavailable = true;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, unread) == 1 &&
              extended[0].action == mark,
          "unavailable guild cannot receive the new unread action despite cached permissions");
    guild.unavailable = false;
    channel.effective_permissions &= ~kPermissionReadMessageHistory;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, unread) == 1 &&
              extended[0].action == mark,
          "unread permission projection rejects stale read action when effective history permission was "
          "removed");
    channel.effective_permissions |= kPermissionReadMessageHistory;
    extended[0] = {utf8_literal("unchanged"), 900, 0, 0};
    extended[1] = extended[0];
    check(channel_context_policy_build(state, 7, mark, close, {extended, 1}, 0, unread) == 0 &&
              extended[0].action == 900 && extended[1].action == 900,
          "unread extra row capacity denial leaves caller output intact");
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, edit, mark) == 0 &&
              channel_context_policy_build(state, 7, mark, close, {extended, 3}, edit, close) == 0 &&
              channel_context_policy_build(state, 7, mark, close, {extended, 3}, edit, edit) == 0 &&
              extended[0].action == 900,
          "unread action cannot alias existing read close or edit namespace");
    constexpr u32 retry_unread = 705;
    channel.type = 1;
    channel.guild_id = 0;
    channel.last_read_message_id = 100;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, 0, retry_unread) == 3 &&
              extended[0].action == mark && extended[1].action == retry_unread &&
              extended[2].action == close &&
              utf8_equal(extended[1].label, utf8_literal("Retry mark as unread")),
          "failed optimistic channel unread retains a separate reachable scoped Retry action after "
          "ordinary availability is absent");
    extended[0] = {utf8_literal("unchanged"), 900, 0, 0};
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, unread, unread) == 0 &&
              extended[0].action == 900,
          "retry unread cannot impersonate ordinary unread action namespace");
    channel.type = 4;
    channel.guild_id = 1;
    channel.permissions_resolved = true;
    channel.effective_permissions = kPermissionViewChannel;
    channel.actions = channel_action_view;
    constexpr u32 scoped_mark = 706, scoped_retry = 707;
    UiScopedReadMenuState scoped{scoped_mark, scoped_retry, true};
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, 0, 0, scoped) == 1 &&
              extended[0].action == scoped_mark && extended[0].flags == popup_item_enabled &&
              utf8_equal(extended[0].label, utf8_literal("Mark as read")),
          "visible category has its own scoped read action without becoming message-capable");
    scoped.pending = true;
    scoped.retry_available = true;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, 0, 0, scoped) == 1 &&
              !(extended[0].flags & popup_item_enabled),
          "pending category read is visible disabled and cannot expose Retry");
    scoped.pending = false;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, 0, 0, scoped) == 2 &&
              extended[0].action == scoped_mark && extended[1].action == scoped_retry &&
              utf8_equal(extended[1].label, utf8_literal("Retry mark as read")),
          "failed scoped category read offers distinct explicit Retry");
    extended[0].action = 900;
    extended[1].action = 900;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 1}, 0, 0, 0, scoped) == 0 &&
              extended[0].action == 900 && extended[1].action == 900 &&
              channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, 0, 0,
                                           {mark, scoped_retry, true}) == 0 &&
              channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, 0, 0,
                                           {scoped_mark, scoped_mark, true}) == 0,
          "category capacity and action alias failures are transactional");
    scoped.available = false;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, 0, 0, scoped) == 0,
          "category read is hidden when scoped host authority is unavailable");
    scoped.available = true;
    guild.unavailable = true;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, 0, 0, scoped) == 0,
          "unavailable guild cannot expose category mutation from stale cached permissions");
    guild.unavailable = false;
    channel.effective_permissions = 0;
    check(channel_context_policy_build(state, 7, mark, close, {extended, 3}, 0, 0, 0, scoped) == 0,
          "category view permission withdrawal fences scoped menu admission");
}
} // namespace maicord
