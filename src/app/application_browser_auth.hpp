#pragma once
#include "maicord/core/base.hpp"

namespace maicord::app_detail {
bool application_login_begin_browser_auth() noexcept;
// Called only after account_host_login_pump; returns true while the browser
// UI handles input. Authenticated results continue normal promotion.
bool application_browser_auth_pump(bool* handled) noexcept;
} // namespace maicord::app_detail
