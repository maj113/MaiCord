#pragma once

#include "maicord/app/gateway_client.hpp"
#include "maicord/core/base.hpp"

namespace maicord {

struct AccountModuleStatus;
struct AccountHostStopResult;

namespace app_detail {

bool application_account_module_load() noexcept;
void application_account_module_release() noexcept;
bool application_account_return_to_login(bool rejected) noexcept;
// Starts the saved account's worker before any window or UI exists; attach
// publishes it. False when attach must take the full (UI-backed) path.
bool application_account_prestart() noexcept;
// Stops a worker that prestart started but attach never published.
void application_account_prestart_cancel() noexcept;
// A prestarted account waits for attach, which replaces the sign-in form with
// the restoring view (and synchronizes that view itself).
bool application_account_prestart_pending() noexcept;
bool application_account_attach_if_present() noexcept;
bool synchronize_member_subscription(u64 now_ms) noexcept;
void synchronize_visible_author_hydration(u64 now_ms, bool pins_reset = false) noexcept;
u64 application_author_hydration_next_wake() noexcept;
GatewayClientPopResult application_pop_account_gateway(void* source, GatewayClientEvent* event,
                                                       Slice<u8> payload_output) noexcept;
// Categorical metadata only; does not inspect payload or change model/UI state.
void application_log_gateway_event(const GatewayClientEvent& event) noexcept;
void application_log_account_observations(const AccountModuleStatus& status, u32 observations) noexcept;
bool application_stop_account() noexcept;
// Main-thread quiescence retains borrowed account UI storage until stop joins.
void application_prepare_account_stop() noexcept;
void application_account_stop_observed(const AccountHostStopResult& result, bool had_account) noexcept;
bool application_account_publish_logged_out(bool unconfirmed) noexcept;
// Closes popups and deactivates the account scenes, then rebuilds the empty message view. False when
// that rebuild fails.
bool application_hide_account_scenes() noexcept;

} // namespace app_detail
} // namespace maicord
