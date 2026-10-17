#include "maicord/ui/account_menu.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
PopupSceneResult ui_account_menu_open_popup(PopupScene* scene, const UiAccountMenuModel& model,
                                            UiRect anchor, UiRect viewport) noexcept {
    const auto opened =
        popup_scene_open(scene, PopupKind::popover, model.user_id, ui_account_menu_items(model), anchor,
                         viewport, kUiAccountMenuWidth, true);
    if (opened.ok() && scene->active)
        popup_scene_set_panel_material(scene, PopupPanelMaterial::gradient);
    return opened;
}
namespace {
bool valid_identity_label(Utf8View value) noexcept {
    if (!value.bytes)
        return true;
    if (!value.data || value.bytes > kUiAccountMenuIdentityBytes || !utf8_validate(value).valid)
        return false;
    for (u32 i = 0; i < value.bytes; ++i)
        if (static_cast<u8>(value.data[i]) < 0x20)
            return false;
    return true;
}
PopupItem action(Utf8View label, UiAccountMenuAction value, UiIcon icon, u16 flags = 0) noexcept {
    PopupItem result{label, static_cast<u32>(value), static_cast<u16>(popup_item_enabled | flags), 0};
    result.presentation = ui_menu_row_presentation(icon);
    return result;
}
} // namespace
bool ui_account_menu_build(UiAccountMenuModel* model, const UiAccountMenuIdentity& identity) noexcept {
    if (!model || !identity.user_id || !valid_identity_label(identity.display_name) ||
        !valid_identity_label(identity.username) || identity.presence > UiPresence::offline)
        return false;
    const auto name = identity.display_name.bytes ? identity.display_name
                      : identity.username.bytes   ? identity.username
                                                  : utf8_literal("Your account");
    // Labels are staged in `names` before *model is reset, so identity may point
    // into the old model. A status longer than the row keeps a valid UTF-8 prefix.
    const u32 status_bytes = utf8_truncated_length(identity.custom_status, kUiAccountMenuIdentityBytes);
    char names[kUiAccountMenuIdentityBytes * 3]{};
    copy_bytes(names, name.data, name.bytes);
    if (identity.username.bytes)
        copy_bytes(names + kUiAccountMenuIdentityBytes, identity.username.data, identity.username.bytes);
    if (status_bytes)
        copy_bytes(names + 2 * kUiAccountMenuIdentityBytes, identity.custom_status.data, status_bytes);
    *model = {};
    model->user_id = identity.user_id;
    copy_bytes(model->display_name, names, name.bytes);
    if (identity.username.bytes)
        copy_bytes(model->username, names + kUiAccountMenuIdentityBytes, identity.username.bytes);
    copy_bytes(model->custom_status, names + 2 * kUiAccountMenuIdentityBytes, status_bytes);
    auto& header = model->items[0];
    header = {{model->display_name, name.bytes},
              static_cast<u32>(UiAccountMenuAction::view_profile_header),
              popup_item_enabled | popup_item_avatar,
              0};
    header.detail = {model->username, identity.username.bytes};
    header.presentation = {0, 1, 0};
    header.media_id = identity.user_id;
    header.media_variant = identity.avatar_variant;
    header.media_flags = identity.avatar_animated ? 1 : 0;
    header.status = static_cast<u8>(identity.presence);
    model->items[1] = action(utf8_literal("View profile"), UiAccountMenuAction::view_profile,
                             UiIcon::user_circle, popup_item_separator_before);
    model->items[2] = action(utf8_literal("Edit profile"), UiAccountMenuAction::edit_profile, UiIcon::edit);
    model->count = 3;
    if (identity.custom_status_available) {
        auto& status = model->items[model->count++];
        status = action(status_bytes ? utf8_literal("Edit custom status") : utf8_literal("Set custom status"),
                        UiAccountMenuAction::custom_status, UiIcon::emoji);
        if (status_bytes) {
            status.detail = {model->custom_status, status_bytes};
            status.presentation.secondary_line = true;
            model->items[model->count++] = action(utf8_literal("Clear custom status"),
                                                  UiAccountMenuAction::clear_custom_status, UiIcon::close);
        }
    }
    if (identity.developer_mode)
        model->items[model->count++] = action(utf8_literal("Copy user ID"), UiAccountMenuAction::copy_user_id,
                                              UiIcon::identifier, popup_item_separator_before);
    if (identity.status_available && identity.presence != UiPresence::unknown) {
        constexpr const char* labels[] = {"Online", "Idle", "Do Not Disturb", "Invisible"};
        constexpr u32 bytes[] = {6, 4, 14, 9};
        for (u32 i = 0; i < 4; ++i) {
            auto& item = model->items[model->count + i];
            item = action(
                {labels[i], bytes[i]},
                static_cast<UiAccountMenuAction>(static_cast<u32>(UiAccountMenuAction::status_online) + i),
                UiIcon::user_circle, i == 0 ? popup_item_separator_before : 0);
            item.status = static_cast<u8>(i + 1);
            if (static_cast<u8>(identity.presence) == item.status) {
                item.flags |= popup_item_checked;
                if (identity.status_pending) {
                    item.detail = utf8_literal("Updating status…");
                    item.presentation.secondary_line = true;
                }
            }
        }
        model->count += 4;
    }
    model->items[model->count++] = action(utf8_literal("Downloads"), UiAccountMenuAction::downloads,
                                          UiIcon::download, popup_item_separator_before);
    if (identity.switch_available)
        model->items[model->count++] = action(utf8_literal("Switch accounts"), UiAccountMenuAction::switch_accounts,
                                              UiIcon::members, popup_item_separator_before);
    if (identity.logout_available)
        model->items[model->count++] =
            action(utf8_literal("Log out"), UiAccountMenuAction::log_out, UiIcon::log_out,
                   static_cast<u16>((identity.switch_available ? 0u : u32{popup_item_separator_before}) |
                                    popup_item_danger));
    return true;
}
ConstSlice<PopupItem> ui_account_menu_items(const UiAccountMenuModel& model) noexcept {
    return {model.items, model.count <= kUiAccountMenuItemCapacity ? model.count : 0};
}
bool ui_account_menu_action_enabled(const UiAccountMenuModel& model, u32 value) noexcept {
    const auto items = ui_account_menu_items(model);
    for (u32 i = 0; i < items.count; ++i) {
        const auto& item = items[i];
        if (item.action == value && (item.flags & popup_item_enabled))
            return true;
    }
    return false;
}
} // namespace maicord
