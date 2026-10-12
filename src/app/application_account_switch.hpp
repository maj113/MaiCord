#pragma once

#include "maicord/ui/account_switcher.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

// Saved accounts ("Switch accounts", up to five). Loads the inventory once,
// adopting the single-profile layout in place, and selects the active slot
// before any account path is resolved.
void application_account_switch_initialize() noexcept;
// The account menu offers Switch accounts (a running, idle account).
bool application_account_switch_available() noexcept;
// Builds a panel of the account menu popup.
bool application_account_switch_panel(UiAccountSwitcherModel* model, u64 owner_id,
                                      UiAccountSwitcherPanel panel) noexcept;
// Account rows and Add an account of a panel (the menu has closed).
bool application_account_switch_action(UiAccountSwitcherPanel panel, u32 action) noexcept;
HANDLE application_account_switch_notification() noexcept;
void application_account_switch_pump() noexcept;
// Esc on the login screen: leave add mode, or return to a saved account.
bool application_account_switch_login_escape() noexcept;
// Active logout finished: forget the slot's inventory entry and continue with
// the most recently used saved account. False shows the login screen.
bool application_account_switch_after_logout() noexcept;
// Joins a pending switch stop before the final account stop.
bool application_account_switch_shutdown() noexcept;

} // namespace maicord::app_detail
