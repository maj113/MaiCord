#pragma once

#include "maicord/ui/popup_scene.hpp"

namespace maicord {

// "Switch accounts" and "Manage accounts" panels of the account menu.
inline constexpr u32 kUiAccountSwitcherAccounts = 5;
inline constexpr u32 kUiAccountSwitcherItemCapacity = kUiAccountSwitcherAccounts + 3;
inline constexpr u32 kUiAccountSwitcherNameBytes = 128;
inline constexpr u32 kUiAccountSwitcherLabelBytes = 160;

enum class UiAccountSwitcherAction : u32 {
    // + account index: switch to (switch panel) or log out of (manage panel).
    account_first = 0x41530000,
    add_account = 0x41530010,
    manage_accounts,
    back,
};

enum class UiAccountSwitcherPanel : u8 { switch_accounts, manage_accounts };

struct UiAccountSwitcherAccount {
    u64 user_id;
    Utf8View display_name, username;
    u32 avatar_variant;
    bool avatar_animated;
    bool active;
    bool needs_login;
};

// Popup descriptors borrow these labels until the popup closes.
struct UiAccountSwitcherModel {
    PopupItem items[kUiAccountSwitcherItemCapacity];
    char labels[kUiAccountSwitcherAccounts][kUiAccountSwitcherLabelBytes];
    char details[kUiAccountSwitcherAccounts][kUiAccountSwitcherNameBytes];
    u64 owner_id;
    u32 count;
    UiAccountSwitcherPanel panel;
};
static_assert(sizeof(UiAccountSwitcherModel) <= 2048);

bool ui_account_switcher_build(UiAccountSwitcherModel* model, u64 owner_id, UiAccountSwitcherPanel panel,
                               ConstSlice<UiAccountSwitcherAccount> accounts, bool can_add) noexcept;
ConstSlice<PopupItem> ui_account_switcher_items(const UiAccountSwitcherModel& model) noexcept;
bool ui_account_switcher_action_enabled(const UiAccountSwitcherModel& model, u32 action) noexcept;
// Account index for an account row action, or ~0u.
u32 ui_account_switcher_account_index(u32 action) noexcept;

} // namespace maicord
