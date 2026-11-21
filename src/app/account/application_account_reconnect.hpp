#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {
struct GatewayClientEvent;
struct UiBannerNotice;
struct UiConnectionIndicator;
} // namespace maicord

namespace maicord::app_detail {

// Stops the account after a failure that is not the user's to fix. The cached
// shell stays usable; a restart is scheduled with backoff, or the banner offers
// Reconnect when the stop needs the user (sign-in, a pending requirement).
void application_account_stop_recoverable(const char* stage) noexcept;
// Starts a due restart. The main loop calls it every turn; it does nothing
// between retry deadlines.
void application_account_reconnect_tick(u64 now_ms) noexcept;
u64 application_account_reconnect_next_wake() noexcept;
// READY and RESUMED connect; reconnecting, closed and failed lose the Gateway. The connection indicator also counts
// RESUMING as connected, and counts down the wait a reconnecting event carries.
void application_account_connection_observe(const GatewayClientEvent& event) noexcept;
// The account module's restart time of a failed Gateway worker (AccountModuleStatus::retry_at_ms), after each pump.
void application_account_connection_restart(u64 restart_at_ms) noexcept;
// The caption's connection indicator (ui/shell/connection_status.hpp), advanced to `now_ms`. Hidden while no account
// shell shows; a new account session starts down.
const UiConnectionIndicator& application_connection_indicator(u64 now_ms) noexcept;
// Its next change without a new report (a visual deadline), zero when none.
u64 application_connection_indicator_next_wake() noexcept;
// The timeline's connection notice (zero id while connected). Text borrows
// `scratch` until the next call.
UiBannerNotice application_account_connection_notice(Slice<char> scratch) noexcept;
// The notice's Reconnect action.
bool application_account_reconnect_now() noexcept;

} // namespace maicord::app_detail
