#include "maicord/ui/account/user_context_menu.hpp"
#include "maicord/core/name_list.hpp"

namespace maicord {
namespace {
constexpr u32 action(UserContextAction value) noexcept {
    return static_cast<u32>(value);
}
// The rows a user menu can hold, in menu order: each one's action (relative to the base), whether it is dangerous,
// opens a submenu or starts a group (a separator before the first row shown from there on). "Profile" and "View Full
// Profile", "Edit note" and "Add note", and the friend rows are alternatives of which the facts show one.
constexpr u8 kDanger = 1, kSubmenu = 2, kGroup = 4;
struct Row {
    u8 action, flags;
};
constexpr Row row(UserContextAction value, u8 flags = 0) noexcept {
    return {static_cast<u8>(action(value) - kUserContextActionBase), flags};
}
constexpr Row kRows[]{row(UserContextAction::profile),
                      row(UserContextAction::profile),
                      row(UserContextAction::mention),
                      row(UserContextAction::message),
                      row(UserContextAction::call),
                      row(UserContextAction::add_note),
                      row(UserContextAction::add_note),
                      row(UserContextAction::apps_menu, kSubmenu | kGroup),
                      row(UserContextAction::invite_menu, kSubmenu),
                      row(UserContextAction::remove_friend, kDanger),
                      row(UserContextAction::accept_friend),
                      row(UserContextAction::cancel_request),
                      row(UserContextAction::add_friend),
                      row(UserContextAction::unblock),
                      row(UserContextAction::block, kDanger),
                      row(UserContextAction::report, kDanger),
                      row(UserContextAction::roles_menu, kSubmenu | kGroup),
                      row(UserContextAction::copy_id, kGroup)};
constexpr char kLabels[] = "View Full Profile\0Profile\0Mention\0Message\0Call\0Edit note\0Add note\0Apps\0"
                           "Invite to server\0Remove friend\0Accept friend request\0Cancel friend request\0"
                           "Add friend\0Unblock\0Block\0Report user\0Roles\0Copy user ID\0";
constexpr u32 kRowCount = sizeof(kRows) / sizeof(kRows[0]);
static_assert(name_list_count(kLabels) == kRowCount);
} // namespace

u32 user_context_menu_build(const UserContextFacts& f, const Slice<PopupItem>& output) noexcept {
    const bool other = !f.self, unblocked = f.relation != UserContextRelation::blocked;
    const bool relation_rows = other && !f.bot && f.relations_known;
    // Whether each row of kRows shows. Invite to Server is hidden for bots, blocked users and yourself.
    const bool shown[kRowCount]{f.profile_row == UserContextProfileRow::view_full,
                                f.profile_row == UserContextProfileRow::profile,
                                f.can_mention,
                                other && f.can_message && unblocked,
                                other && f.can_call && !f.bot && unblocked,
                                other && f.has_note,
                                other && !f.has_note,
                                other && f.has_apps,
                                other && !f.bot && f.can_invite && unblocked,
                                relation_rows && f.relation == UserContextRelation::friend_,
                                relation_rows && f.relation == UserContextRelation::incoming,
                                relation_rows && f.relation == UserContextRelation::outgoing,
                                relation_rows && f.relation == UserContextRelation::none,
                                relation_rows && !unblocked,
                                relation_rows && unblocked,
                                other && f.can_report,
                                f.can_edit_roles,
                                f.developer_mode};
    PopupItem items[kUserContextMaximumItems];
    u32 count = 0;
    bool group_open = false;
    NameList label{kLabels};
    for (u32 i = 0; i < kRowCount; ++i, label.next()) {
        const Row spec = kRows[i];
        if (spec.flags & kGroup)
            group_open = count != 0;
        if (!shown[i])
            continue;
        PopupItem& item = items[count++];
        popup_item_set(&item, label.name(), kUserContextActionBase + spec.action,
                       static_cast<u16>(popup_item_enabled | (spec.flags & kDanger ? popup_item_danger : 0) |
                                        (group_open ? popup_item_separator_before : 0)));
        if (spec.flags & kSubmenu)
            item.presentation = ui_menu_row_presentation(UiIcon::chevron_right, false, UiMenuIconPlacement::trailing);
        group_open = false;
    }
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
