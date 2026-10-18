#pragma once

#include "maicord/app/account_paths.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

struct ApplicationAccountLogout;

bool application_account_logout_available() noexcept;
bool application_account_logout_open() noexcept;
// Pure owner projections. blocked() gates login promotion and all account UI
// after durable intent; host_owned() additionally forbids every host access.
bool application_account_logout_blocked() noexcept;
bool application_account_logout_host_owned() noexcept;
// Login actions are refused while either projection holds.
inline bool application_account_logout_locked() noexcept {
    return application_account_logout_blocked() || application_account_logout_host_owned();
}
HANDLE application_account_logout_notification() noexcept;
u64 application_account_logout_next_wake() noexcept;
void application_account_logout_pump() noexcept;
// Call before any account load/decryption. False blocks automatic attach and
// starts marker recovery; recovery never sends another logout request.
bool application_account_logout_prepare_startup(const AccountPaths* paths = nullptr) noexcept;
bool application_account_logout_shutdown() noexcept;
void application_account_logout_release() noexcept;

} // namespace maicord::app_detail
