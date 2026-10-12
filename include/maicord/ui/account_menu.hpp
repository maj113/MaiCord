#pragma once

#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/presence.hpp"

namespace maicord {
inline constexpr u32 kUiAccountMenuIdentityBytes = 128;
inline constexpr u32 kUiAccountMenuItemCapacity = 13;
inline constexpr float kUiAccountMenuWidth = 280;

enum class UiAccountMenuAction : u32 {
    view_profile_header = 0x41430000,
    view_profile = 0x41430001,
    edit_profile,
    copy_user_id,
    status_online,
    status_idle,
    status_dnd,
    status_invisible,
    downloads,
    log_out,
    custom_status,
    clear_custom_status,
    switch_accounts,
};
struct UiAccountMenuIdentity {
    u64 user_id;
    Utf8View display_name, username;
    u32 avatar_variant;
    bool avatar_animated;
    // Authoritative self status; a guild member's presence must not stand in for a
    // chosen Invisible.
    UiPresence presence = UiPresence::unknown;
    bool status_available = false;
    bool status_pending = false;
    // False hides the Log out row (the host decides).
    bool logout_available = false;
    // Saved accounts can be switched or added (opens the Switch accounts panel).
    bool switch_available = false;
    // Copy user ID is offered only with Developer Mode on.
    bool developer_mode = true;
    // Custom status rows; the text is copied into the model.
    bool custom_status_available = false;
    Utf8View custom_status{};
};
// Stable caller-owned labels. Popup descriptors borrow this model until the
// popup closes; rebuild in place before republishing rather than copying it.
struct UiAccountMenuModel {
    PopupItem items[kUiAccountMenuItemCapacity];
    char display_name[kUiAccountMenuIdentityBytes];
    char username[kUiAccountMenuIdentityBytes];
    char custom_status[kUiAccountMenuIdentityBytes];
    u64 user_id;
    u32 count;
};
static_assert(sizeof(UiAccountMenuModel) <= 1280);
bool ui_account_menu_build(UiAccountMenuModel*, const UiAccountMenuIdentity&) noexcept;
PopupSceneResult ui_account_menu_open_popup(PopupScene*, const UiAccountMenuModel&, UiRect anchor,
                                            UiRect viewport) noexcept;
ConstSlice<PopupItem> ui_account_menu_items(const UiAccountMenuModel&) noexcept;
bool ui_account_menu_action_enabled(const UiAccountMenuModel&, u32 action) noexcept;
} // namespace maicord
