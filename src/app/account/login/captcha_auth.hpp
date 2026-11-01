#pragma once
#include "maicord/core/base.hpp"

namespace maicord::app_detail {
// Explicit "Verify in browser" action for the pending login challenge.
bool application_login_begin_captcha_bridge() noexcept;
// Called after account_host_login_pump and before event dispatch. Launches the
// bridge URL once per explicit click and maps bridge phases to presentation.
bool application_captcha_auth_pump() noexcept;
// Presentation only; stopping the login owner stops its bridge.
void application_captcha_auth_reset() noexcept;
u64 application_login_captcha_deadline() noexcept;
bool application_login_tick_captcha(u64 now_ms) noexcept;
} // namespace maicord::app_detail
