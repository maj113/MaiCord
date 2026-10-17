#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/model/guild_settings_model.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
bool guild_context_target_valid(const ChatStateStore& state, u64 guild_id) noexcept {
    const auto* guild = chat_state_store_find_guild(state, guild_id);
    return guild && !guild->unavailable;
}
bool guild_context_target_current(const ChatStateStore& state, u64 guild_id, u16 generation,
                                  u32 revision) noexcept {
    const auto* guild = chat_state_store_find_guild(state, guild_id);
    return guild && guild->generation == generation && guild->revision == revision &&
           guild_context_target_valid(state, guild_id);
}
u32 guild_context_policy_build(const ChatStateStore& state, u64 guild_id, u32 copy_id_action,
                               Slice<PopupItem> output, UiScopedReadMenuState scoped,
                               u32 settings_action, bool developer_mode) noexcept {
    if (!copy_id_action || !output.data || !output.count || !guild_context_target_valid(state, guild_id) ||
        (settings_action && (settings_action == copy_id_action || settings_action == scoped.mark_action ||
                             settings_action == scoped.retry_action)))
        return 0;
    if ((scoped.mark_action && scoped.mark_action == copy_id_action) ||
        (scoped.retry_action &&
         (scoped.retry_action == copy_id_action || scoped.retry_action == scoped.mark_action)) ||
        (scoped.available && !scoped.mark_action) ||
        (scoped.available && scoped.retry_available && !scoped.retry_action))
        return 0;
    const auto* guild = chat_state_store_find_guild(state, guild_id);
    const bool mark = scoped.available && guild->permissions_resolved;
    const bool retry = mark && scoped.retry_available && !scoped.pending;
    // Server Settings is offered only with a permission one of its pages uses.
    const bool settings = settings_action && guild_settings_access(state, guild_id).sections != 0;
    const u32 count = static_cast<u32>(developer_mode) + static_cast<u32>(mark) + static_cast<u32>(retry) +
                      static_cast<u32>(settings);
    if (output.count < count)
        return 0;
    u32 used = 0;
    if (mark)
        output[used++] = {utf8_literal("Mark as read"), scoped.mark_action,
                          static_cast<u16>(scoped.pending ? 0u : popup_item_enabled)};
    if (retry)
        output[used++] = {utf8_literal("Retry mark as read"), scoped.retry_action, popup_item_enabled};
    if (settings) {
        auto& item = output[used];
        item = {utf8_literal("Server settings"), settings_action,
                static_cast<u16>(popup_item_enabled | (used ? popup_item_separator_before : 0u))};
        item.presentation = ui_menu_row_presentation(UiIcon::settings);
        ++used;
    }
    if (developer_mode)
        output[used++] = {utf8_literal("Copy server ID"), copy_id_action,
                          static_cast<u16>(popup_item_enabled | (mark || settings ? popup_item_separator_before : 0u))};
    return used;
}
bool guild_context_can_create_channel(const ChatStateStore& state, u64 guild_id, u64 category_id) noexcept {
    if (category_id) {
        const auto* category = chat_state_store_find_channel(state, category_id);
        if (!category || category->type != 4 || !category->guild_id || !category->permissions_resolved)
            return false;
        return (category->effective_permissions & kPermissionViewChannel) &&
               permission_has(category->effective_permissions, kPermissionManageChannels);
    }
    const auto access = guild_settings_access(state, guild_id);
    return access.resolved && permission_has(access.permissions, kPermissionManageChannels);
}

namespace {
bool text_like(u8 type) noexcept {
    return type == 0 || type == 5 || type == 15 || type == 16;
}
bool invite_target(const ChatStateStore& state, const ChannelRecord* channel, u64 guild_id) noexcept {
    return channel && channel->occupied && channel->guild_id == guild_id && channel->permissions_resolved &&
           (channel->type == 0 || channel->type == 2 || channel->type == 5 || channel->type == 13 ||
            channel->type == 15 || channel->type == 16) &&
           !(channel->flags & kChannelObfuscatedFlag) && chat_state_store_channel_visible(state, *channel) &&
           permission_has(channel->effective_permissions, kPermissionViewChannel) &&
           permission_has(channel->effective_permissions, kPermissionCreateInvite);
}
} // namespace

u64 guild_context_invite_channel(const ChatStateStore& state, u64 guild_id, u64 preferred_channel) noexcept {
    const auto* guild = chat_state_store_find_guild(state, guild_id);
    if (!guild || guild->unavailable)
        return 0;
    if (const auto* preferred = chat_state_store_find_channel(state, preferred_channel);
        invite_target(state, preferred, guild_id))
        return preferred->id;
    if (const auto* system = chat_state_store_find_channel(state, guild->settings.system_channel_id);
        invite_target(state, system, guild_id) && text_like(system->type))
        return system->id;
    // Lowest position first among text-like channels, then any voice channel.
    const ChannelRecord* best = nullptr;
    for (u32 pass = 0; pass < 2 && !best; ++pass)
        for (u32 order = 0; order < state.channel_count; ++order) {
            const auto& candidate = state.channels[state.channel_order[order]];
            if (!invite_target(state, &candidate, guild_id) || text_like(candidate.type) != (pass == 0))
                continue;
            if (!best || candidate.position < best->position ||
                (candidate.position == best->position && candidate.id < best->id))
                best = &candidate;
        }
    return best ? best->id : 0;
}

u32 guild_context_invite_item(const ChatStateStore& state, u64 guild_id, u64 preferred_channel, u32 action,
                              Slice<PopupItem> output) noexcept {
    if (!action || !output.data || !output.count || !guild_context_invite_channel(state, guild_id, preferred_channel))
        return 0;
    output[0] = {utf8_literal("Invite people"), action,
                 static_cast<u16>(popup_item_enabled | popup_item_separator_before)};
    output[0].presentation = ui_menu_row_presentation(UiIcon::link);
    return 1;
}

u32 guild_context_settings_item(const ChatStateStore& state, u64 guild_id, u32 action,
                                Slice<PopupItem> output) noexcept {
    if (!action || !output.data || !output.count || !guild_context_target_valid(state, guild_id) ||
        !guild_settings_access(state, guild_id).sections)
        return 0;
    output[0] = {utf8_literal("Server settings"), action,
                 static_cast<u16>(popup_item_enabled | popup_item_separator_before)};
    output[0].presentation = ui_menu_row_presentation(UiIcon::settings);
    return 1;
}

u32 guild_context_copy_id_item(const ChatStateStore& state, u64 guild_id, u32 action,
                               Slice<PopupItem> output) noexcept {
    if (!action || !output.data || !output.count || !guild_context_target_valid(state, guild_id))
        return 0;
    output[0] = {utf8_literal("Copy server ID"), action,
                 static_cast<u16>(popup_item_enabled | popup_item_separator_before)};
    return 1;
}

bool guild_context_owner(const ChatStateStore& state, u64 guild_id) noexcept {
    const auto* guild = chat_state_store_find_guild(state, guild_id);
    return guild && guild->owner_id && guild->owner_id == state.self_user_id;
}

u32 guild_context_leave_item(const ChatStateStore& state, u64 guild_id, u32 action,
                             Slice<PopupItem> output) noexcept {
    if (!action || !output.data || !output.count || !guild_context_target_valid(state, guild_id) ||
        guild_context_owner(state, guild_id))
        return 0;
    output[0] = {utf8_literal("Leave server"), action,
                 static_cast<u16>(popup_item_enabled | popup_item_danger | popup_item_separator_before)};
    output[0].presentation = ui_menu_row_presentation(UiIcon::log_out);
    return 1;
}

u32 guild_context_create_channel_items(const ChatStateStore& state, u64 guild_id, u64 category_id,
                                       u32 text_action, u32 voice_action, Slice<PopupItem> output) noexcept {
    if (!text_action || !output.data || !output.count || (!guild_id == !category_id) ||
        !guild_context_can_create_channel(state, guild_id, category_id))
        return 0;
    const bool voice = category_id && voice_action && output.count >= 2;
    output[0] = {category_id ? utf8_literal("Create text channel") : utf8_literal("Create channel"), text_action,
                 static_cast<u16>(popup_item_enabled | popup_item_separator_before)};
    output[0].presentation = ui_menu_row_presentation(UiIcon::channel);
    if (voice) {
        output[1] = {utf8_literal("Create voice channel"), voice_action, popup_item_enabled};
        output[1].presentation = ui_menu_row_presentation(UiIcon::speaker);
    }
    return voice ? 2 : 1;
}
} // namespace maicord

namespace maicord {
u32 guild_context_create_category_item(const ChatStateStore& state, u64 guild_id, u32 action,
                                       Slice<PopupItem> output) noexcept {
    if (!action || !output.data || !output.count || !guild_context_can_create_channel(state, guild_id, 0))
        return 0;
    output[0] = {utf8_literal("Create category"), action, popup_item_enabled};
    return 1;
}

u32 guild_context_hide_muted_item(const ChatStateStore& state, u64 guild_id, u32 action,
                                  Slice<PopupItem> output) noexcept {
    const auto* guild = chat_state_store_find_guild(state, guild_id);
    if (!action || !output.data || !output.count || !guild || guild->unavailable)
        return 0;
    output[0] = {utf8_literal("Hide muted channels"), action,
                 static_cast<u16>(popup_item_enabled | (guild->hide_muted_channels ? popup_item_checked : 0u))};
    return 1;
}

u32 guild_header_menu_build(const ChatStateStore& state, u64 guild_id, u64 preferred_channel,
                            const GuildMenuActions& actions, u16 capabilities, Slice<PopupItem> output) noexcept {
    if (!output.data || !guild_context_target_valid(state, guild_id))
        return 0;
    PopupItem rows[kGuildHeaderMenuMaximumItems]{};
    u32 count = 0;
    bool group = false; // the next row starts a separated group
    const auto add = [&](u32 produced) noexcept {
        if (!produced)
            return;
        auto& first = rows[count];
        first.flags = static_cast<u16>(group && count ? first.flags | popup_item_separator_before
                                                      : first.flags & ~popup_item_separator_before);
        for (u32 i = 1; i < produced; ++i)
            rows[count + i].flags &= static_cast<u16>(~popup_item_separator_before);
        count += produced;
        group = false;
    };
    const auto row = [&](Utf8View label, u32 action, UiIcon icon = UiIcon::count, u16 flags = 0) noexcept {
        if (!action)
            return 0u;
        rows[count] = {label, action, static_cast<u16>(popup_item_enabled | flags)};
        if (icon != UiIcon::count)
            rows[count].presentation = ui_menu_row_presentation(icon, false, UiMenuIconPlacement::trailing);
        return 1u;
    };
    const auto trailing = [&](UiIcon icon) noexcept {
        rows[count].presentation = ui_menu_row_presentation(icon, false, UiMenuIconPlacement::trailing);
    };
    // Invite, settings and creation.
    if (guild_context_invite_item(state, guild_id, preferred_channel, actions.invite, {rows + count, 1})) {
        trailing(UiIcon::link);
        add(1);
    }
    if (guild_context_settings_item(state, guild_id, actions.settings, {rows + count, 1})) {
        trailing(UiIcon::settings);
        add(1);
    }
    if (guild_context_can_create_channel(state, guild_id, 0)) {
        add(row(utf8_literal("Create channel"), actions.create_channel));
        add(guild_context_create_category_item(state, guild_id, actions.create_category, {rows + count, 1}));
    }
    if (capabilities & guild_menu_capability_create_event)
        add(row(utf8_literal("Create event"), actions.create_event));
    // Channel opt-in, notifications and privacy.
    group = true;
    if (capabilities & guild_menu_capability_show_all)
        add(row(utf8_literal("Show all channels"), actions.show_all_channels, UiIcon::count,
                static_cast<u16>((capabilities & guild_menu_capability_show_all_checked) ? popup_item_checked : 0u)));
    if (capabilities & guild_menu_capability_notifications)
        add(row(utf8_literal("Notification settings"), actions.notifications, UiIcon::chevron_right));
    add(row(utf8_literal("Privacy settings"), actions.privacy, UiIcon::shield));
    // This server's profile and channel list.
    group = true;
    if (capabilities & guild_menu_capability_profile)
        add(row(utf8_literal("Edit per-server profile"), actions.profile, UiIcon::edit));
    add(guild_context_hide_muted_item(state, guild_id, actions.hide_muted, {rows + count, 1}));
    group = true;
    if (guild_context_leave_item(state, guild_id, actions.leave, {rows + count, 1})) {
        trailing(UiIcon::log_out);
        add(1);
    }
    group = true;
    if (capabilities & guild_menu_capability_developer)
        add(guild_context_copy_id_item(state, guild_id, actions.copy_id, {rows + count, 1}));
    if (!count || output.count < count)
        return 0;
    for (u32 i = 0; i < count; ++i)
        output[i] = rows[i];
    return count;
}
} // namespace maicord
