// The caption's connection indicator over its real sources: the Gateway's lifecycle events (READY, RESUMING, the
// reconnecting wait, a failed worker), the account module's worker restart, the account restart schedule (a stop, its
// countdown, a restart, the attempt cap, a stop that needs the user, Reconnect) and the shell that hides it. The
// account host, its restart and the sign-in screen are stand-ins; the clock is the real monotonic one, and each check
// samples the indicator at a time relative to it.
#include "../../support/test_output.hpp"
#include "account/application_account_reconnect.hpp"
#include "account/application_account_logout.hpp"
#include "account/application_account_module.hpp"
#include "account/application_account_switch.hpp"
#include "shell/state.hpp"
#include "maicord/app/account/account_reconnect.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/auth/account_requirement.hpp"
#include "maicord/ui/shell/connection_status.hpp"

namespace maicord::connection_indicator_test {
namespace {
u32 failures;
u32 stops, restarts;
bool restart_ok;
u64 ui_storage[8];
int first_instance, second_instance;

void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}

using app_detail::g_app;
using Status = UiConnectionStatus;

void observe(GatewayClientEventKind kind, u32 delay_ms = 0) noexcept {
    GatewayClientEvent event{};
    event.kind = kind;
    event.retry_delay_ms = delay_ms;
    app_detail::application_account_connection_observe(event);
}
const UiConnectionIndicator& at(u64 offset_ms) noexcept {
    return app_detail::application_connection_indicator(clock_monotonic_milliseconds() + offset_ms);
}

void gateway() noexcept {
    g_app.account_ui = reinterpret_cast<app_detail::AccountUiRuntime*>(ui_storage);
    g_app.account.instance = &first_instance;
    // The startup shell shows before READY: the first connect shows Connecting once the grace ends.
    check(at(0).shown == Status::hidden && at(kUiConnectionGraceMs + 100).shown == Status::connecting,
          "connection sources: a new session shows Connecting after the grace");
    observe(GatewayClientEventKind::ready);
    check(g_app.frame_pending && at(0).shown == Status::connected && at(kUiConnectionConfirmMs + 100).shown == Status::hidden &&
              !app_detail::application_connection_indicator_next_wake(),
          "connection sources: READY confirms Connected, then nothing shows or wakes");

    // The worker reconnects after a 5 s wait: the countdown follows the event's delay.
    g_app.frame_pending = false;
    observe(GatewayClientEventKind::closed);
    observe(GatewayClientEventKind::reconnecting, 5000);
    const auto& waiting = at(kUiConnectionGraceMs + 100);
    check(g_app.frame_pending && waiting.shown == Status::disconnected && waiting.seconds == 4,
          "connection sources: a reconnecting wait shows Disconnected with its countdown");
    observe(GatewayClientEventKind::websocket_opened);
    check(at(kUiConnectionGraceMs + 200).shown == Status::connecting,
          "connection sources: an attempt that starts shows Connecting");
    // The official client counts a resuming session as established.
    observe(GatewayClientEventKind::resuming);
    check(at(kUiConnectionGraceMs + 300).shown == Status::connected,
          "connection sources: RESUMING confirms Connected");
    observe(GatewayClientEventKind::resumed);
    check(at(kUiConnectionGraceMs + kUiConnectionConfirmMs + 400).shown == Status::hidden,
          "connection sources: RESUMED keeps the session up");

    // A failed worker: Connecting until the account module reports its restart, then the countdown to it.
    observe(GatewayClientEventKind::failed);
    check(at(kUiConnectionGraceMs + 100).shown == Status::connecting,
          "connection sources: a failed worker without a known restart shows Connecting");
    app_detail::application_account_connection_restart(clock_monotonic_milliseconds() + 60'000);
    const auto& restart = at(kUiConnectionGraceMs + 100);
    check(restart.shown == Status::disconnected && restart.seconds >= 58 && restart.seconds <= 59,
          "connection sources: the module's restart time is counted down");
    app_detail::application_account_connection_restart(0);
    check(at(kUiConnectionGraceMs + 100).shown == Status::connecting,
          "connection sources: the restarted worker's attempt shows Connecting");
    observe(GatewayClientEventKind::ready);
    check(at(kUiConnectionGraceMs + kUiConnectionConfirmMs + 200).shown == Status::hidden,
          "connection sources: the restarted worker's READY ends it");
}

void account() noexcept {
    // The session fails and the account stops: its restart schedule counts down, a due restart connects, and READY
    // ends it.
    observe(GatewayClientEventKind::reconnecting);
    check(at(kUiConnectionGraceMs + 100).shown == Status::connecting, "connection sources: the session is lost");
    app_detail::application_account_stop_recoverable("test_stop");
    const auto& stopped = at(0);
    check(stops == 1 && stopped.shown == Status::disconnected && stopped.seconds >= 1,
          "connection sources: a stopped account counts down to its restart");
    restart_ok = true;
    app_detail::application_account_reconnect_tick(app_detail::application_account_reconnect_next_wake());
    check(restarts == 1 && at(0).shown == Status::connecting,
          "connection sources: the restarted account shows Connecting");
    observe(GatewayClientEventKind::ready);
    check(at(kUiConnectionConfirmMs + 100).shown == Status::hidden, "connection sources: its READY ends it");

    // Restarts that keep failing stop at the cap: Connection failed, with no wake of its own.
    restart_ok = false;
    app_detail::application_account_stop_recoverable("test_stop");
    for (u32 i = 0; i < kAccountReconnectMaximumAttempts + 2 && app_detail::application_account_reconnect_next_wake(); ++i)
        app_detail::application_account_reconnect_tick(app_detail::application_account_reconnect_next_wake());
    check(at(0).shown == Status::failed && !app_detail::application_connection_indicator_next_wake(),
          "connection sources: restarts that stop at the cap show Connection failed");
    // Reconnect starts again at once.
    check(app_detail::application_account_reconnect_now() && at(0).shown == Status::connecting,
          "connection sources: Reconnect leaves Connection failed for Connecting");

    // A stop that needs the user: Disconnected without a countdown.
    restart_ok = true;
    app_detail::application_account_reconnect_tick(app_detail::application_account_reconnect_next_wake());
    observe(GatewayClientEventKind::ready);
    g_app.account.status.required_action = AccountRequirement::verify_email;
    app_detail::application_account_stop_recoverable("test_requirement");
    check(at(0).shown == Status::stopped && at(0).seconds == 0,
          "connection sources: a stop that needs the user shows Disconnected");
    g_app.account.status.required_action = AccountRequirement::none;

    // The sign-in screen covers the shell: nothing shows and nothing wakes.
    g_app.login.visible = true;
    check(at(0).shown == Status::hidden && !app_detail::application_connection_indicator_next_wake(),
          "connection sources: no indicator without the account shell");
}
} // namespace
} // namespace maicord::connection_indicator_test

namespace maicord::app_detail {
bool application_account_logout_blocked() noexcept {
    return false;
}
bool application_account_switch_busy() noexcept {
    return false;
}
bool application_stop_account() noexcept {
    ++connection_indicator_test::stops;
    g_app.account.instance = nullptr;
    return true;
}
bool application_account_restart() noexcept {
    using namespace connection_indicator_test;
    ++restarts;
    if (!restart_ok)
        return false;
    g_app.account.instance = g_app.account.instance == &second_instance ? &first_instance : &second_instance;
    return true;
}
} // namespace maicord::app_detail

extern "C" __declspec(noreturn) void WINAPI maicord_connection_indicator_entry() noexcept {
    using namespace maicord::connection_indicator_test;
    gateway();
    account();
    maicord::test_exit(failures, "connection_indicator");
}
