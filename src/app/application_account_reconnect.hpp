#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {
enum class GatewayClientEventKind : u8;
struct UiBannerNotice;
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
// READY and RESUMED connect; reconnecting, closed and failed lose the Gateway.
void application_account_connection_observe(GatewayClientEventKind kind) noexcept;
// The timeline's connection notice (zero id while connected). Text borrows
// `scratch` until the next call.
UiBannerNotice application_account_connection_notice(Slice<char> scratch) noexcept;
// The notice's Reconnect action.
bool application_account_reconnect_now() noexcept;

} // namespace maicord::app_detail
