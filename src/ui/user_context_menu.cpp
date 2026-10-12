#include "maicord/ui/user_context_menu.hpp"

namespace maicord {
namespace {
constexpr u32 action(UserContextAction value) noexcept {
    return static_cast<u32>(value);
}
PopupItem row(Utf8View label, UserContextAction value, u16 flags = 0) noexcept {
    PopupItem item{};
    item.label = label;
    item.action = action(value);
    item.flags = static_cast<u16>(popup_item_enabled | flags);
    return item;
}
PopupItem submenu(Utf8View label, UserContextAction value, u16 flags = 0) noexcept {
    auto item = row(label, value, flags);
    item.presentation = ui_menu_row_presentation(UiIcon::chevron_right, false, UiMenuIconPlacement::trailing);
    return item;
}
} // namespace

u32 user_context_menu_build(const UserContextFacts& f, Slice<PopupItem> output) noexcept {
    PopupItem items[kUserContextMaximumItems]{};
    u32 count = 0;
    bool group_open = false;
    const auto push = [&](PopupItem item) noexcept {
        if (group_open) {
            item.flags = static_cast<u16>(item.flags | popup_item_separator_before);
            group_open = false;
        }
        items[count++] = item;
    };
    const auto next_group = [&]() noexcept { group_open = count != 0; };
    const bool other = !f.self;
    const bool relation_rows = other && !f.bot && f.relations_known;

    push(row(utf8_literal("Profile"), UserContextAction::profile));
    if (f.can_mention)
        push(row(utf8_literal("Mention"), UserContextAction::mention));
    if (other && f.can_message && f.relation != UserContextRelation::blocked)
        push(row(utf8_literal("Message"), UserContextAction::message));
    if (other && f.can_call && !f.bot && f.relation != UserContextRelation::blocked)
        push(row(utf8_literal("Call"), UserContextAction::call));
    if (other)
        push(row(f.has_note ? utf8_literal("Edit note") : utf8_literal("Add note"), UserContextAction::add_note));

    next_group();
    if (other && f.has_apps)
        push(submenu(utf8_literal("Apps"), UserContextAction::apps_menu));
    // Invite to Server is hidden for bots, blocked users and yourself.
    if (other && !f.bot && f.can_invite && f.relation != UserContextRelation::blocked)
        push(submenu(utf8_literal("Invite to server"), UserContextAction::invite_menu));
    if (relation_rows) {
        switch (f.relation) {
        case UserContextRelation::friend_:
            push(row(utf8_literal("Remove friend"), UserContextAction::remove_friend, popup_item_danger));
            break;
        case UserContextRelation::incoming:
            push(row(utf8_literal("Accept friend request"), UserContextAction::accept_friend));
            break;
        case UserContextRelation::outgoing:
            push(row(utf8_literal("Cancel friend request"), UserContextAction::cancel_request));
            break;
        case UserContextRelation::none:
            push(row(utf8_literal("Add friend"), UserContextAction::add_friend));
            break;
        default:
            break;
        }
        push(f.relation == UserContextRelation::blocked
                 ? row(utf8_literal("Unblock"), UserContextAction::unblock)
                 : row(utf8_literal("Block"), UserContextAction::block, popup_item_danger));
    }
    if (other && f.can_report)
        push(row(utf8_literal("Report user"), UserContextAction::report, popup_item_danger));

    next_group();
    if (f.can_edit_roles)
        push(submenu(utf8_literal("Roles"), UserContextAction::roles_menu));

    next_group();
    if (f.developer_mode)
        push(row(utf8_literal("Copy user ID"), UserContextAction::copy_id));
    if (!output.data || output.count < count)
        return 0;
    for (u32 i = 0; i < count; ++i)
        output[i] = items[i];
    return count;
}

u32 user_context_roles_build(ConstSlice<UserContextRole> roles, Slice<PopupItem> output) noexcept {
    const u32 count = minimum<u32>(roles.count, kUserContextRoleItems);
    if (!output.data || output.count < count || (!roles.data && roles.count))
        return 0;
    for (u32 i = 0; i < count; ++i) {
        const auto& role = roles[i];
        PopupItem item{};
        item.label = role.name;
        item.action = action(UserContextAction::role_first) + i;
        item.flags = static_cast<u16>((role.editable ? popup_item_enabled : 0) | (role.held ? popup_item_checked : 0));
        item.presentation = ui_menu_row_presentation(UiIcon::mention);
        if (role.color) {
            item.media_flags = kPopupItemMediaIconTint;
            item.media_variant = role.color & 0xFFFFFFu;
        }
        output[i] = item;
    }
    return count;
}

u32 user_context_invites_build(ConstSlice<UserContextChoice> guilds, Slice<PopupItem> output) noexcept {
    const u32 count = minimum<u32>(guilds.count, kUserContextGuildItems);
    if (!output.data || output.count < count || (!guilds.data && guilds.count))
        return 0;
    for (u32 i = 0; i < count; ++i) {
        PopupItem item{};
        item.label = guilds[i].done ? utf8_literal("Invite sent") : guilds[i].label;
        item.detail = guilds[i].done ? guilds[i].label : Utf8View{};
        item.presentation.secondary_line = item.detail.bytes != 0;
        item.action = action(UserContextAction::invite_first) + i;
        item.flags = static_cast<u16>(guilds[i].done ? 0 : popup_item_enabled);
        output[i] = item;
    }
    return count;
}

u32 user_context_apps_build(ConstSlice<UserContextChoice> apps, bool loading, Slice<PopupItem> output) noexcept {
    const u32 count = minimum<u32>(apps.count, kUserContextAppItems);
    if (!output.data || output.count < maximum(count, 1u) || (!apps.data && apps.count))
        return 0;
    if (!count) {
        PopupItem item{};
        item.label = loading ? utf8_literal("Loading apps\xE2\x80\xA6") : utf8_literal("No apps");
        item.action = action(UserContextAction::app_first) - 1;
        output[0] = item;
        return 1;
    }
    for (u32 i = 0; i < count; ++i) {
        PopupItem item{};
        item.label = apps[i].label;
        item.detail = apps[i].detail;
        item.presentation = ui_menu_row_presentation(UiIcon::command, apps[i].detail.bytes != 0);
        item.action = action(UserContextAction::app_first) + i;
        item.flags = popup_item_enabled;
        output[i] = item;
    }
    return count;
}
} // namespace maicord
