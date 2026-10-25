#include "application_account_logout.hpp"
#include "application_account_module.hpp"
#include "application_account_switch.hpp"
#include "application_state.hpp"

#include "maicord/app/account_reconnect.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/timestamp_format.hpp"
#include "maicord/ui/unread_banner_scene.hpp"

namespace maicord::app_detail {
namespace {
AccountReconnect g_reconnect{};
// The instance a restart started; any other attach owns the account.
void* g_restarted{};
// Notice ids change with the state so the card re-announces it.
constexpr u64 kConnectionNoticeId = 0xC0'4E'00'00ull;

bool shell_retained() noexcept {
    return g_app.account_ui && !g_app.login.visible && !application_account_logout_blocked() &&
           !application_account_switch_busy();
}
bool active() noexcept {
    return g_reconnect.phase != AccountReconnectPhase::idle || g_reconnect.gateway_down;
}
void forget() noexcept {
    if (!active())
        return;
    account_reconnect_reset(&g_reconnect);
    g_restarted = nullptr;
    application_request_frame();
}
void log_schedule() noexcept {
    TraceRecord trace("reconnect_scheduled");
    trace.field("attempt", g_reconnect.attempt);
    trace.field("phase", static_cast<u64>(g_reconnect.phase));
    trace.field("reason", static_cast<u64>(g_reconnect.reason));
    const u64 now = clock_monotonic_milliseconds();
    trace.field("delay_ms", g_reconnect.retry_at_ms > now ? g_reconnect.retry_at_ms - now : 0);
    log_write(LogLevel::warning, "account", trace.text);
}
} // namespace

void application_account_stop_recoverable(const char* stage) noexcept {
    // The shell stays up on cached state after this stop; never silent.
    log_write(LogLevel::error, "account", stage);
    const auto& status = g_app.account.status;
    const AccountStopReason reason = account_reconnect_classify(status.coordinator_error, status.required_action);
    application_stop_account();
    if (!shell_retained())
        return;
    if (!g_reconnect.jitter)
        g_reconnect.jitter = static_cast<u64>(clock_now()) ^ (clock_unix_milliseconds() << 20);
    // A stop that failed to join keeps its instance; restarts then fail until the cap.
    g_restarted = g_app.account.instance;
    account_reconnect_stopped(&g_reconnect, reason, clock_monotonic_milliseconds());
    log_schedule();
    application_request_frame();
}

void application_account_reconnect_tick(u64 now_ms) noexcept {
    if (!active())
        return;
    const AccountHost& host = g_app.account;
    if (!shell_retained() || (host.instance && host.instance != g_restarted)) {
        forget();
        return;
    }
    if (!account_reconnect_due(g_reconnect, now_ms))
        return;
    account_reconnect_starting(&g_reconnect);
    log_u64(LogLevel::info, "account", "reconnect_attempt", g_reconnect.attempt);
    if (application_account_restart()) {
        g_restarted = g_app.account.instance;
    } else {
        account_reconnect_start_failed(&g_reconnect, now_ms);
        log_schedule();
    }
    application_request_frame();
}

u64 application_account_reconnect_next_wake() noexcept {
    return account_reconnect_next_wake(g_reconnect);
}

void application_account_connection_observe(GatewayClientEventKind kind) noexcept {
    const auto banner = account_reconnect_banner(g_reconnect);
    switch (kind) {
    case GatewayClientEventKind::ready:
    case GatewayClientEventKind::resumed:
        account_reconnect_connected(&g_reconnect, clock_monotonic_milliseconds());
        g_restarted = nullptr;
        break;
    case GatewayClientEventKind::reconnecting:
    case GatewayClientEventKind::closed:
    case GatewayClientEventKind::failed:
        account_reconnect_gateway_lost(&g_reconnect);
        break;
    default:
        return;
    }
    if (banner != account_reconnect_banner(g_reconnect))
        application_request_frame();
}

UiBannerNotice application_account_connection_notice(Slice<char> scratch) noexcept {
    UiBannerNotice notice{};
    const auto banner = account_reconnect_banner(g_reconnect);
    if (banner == AccountConnectionBanner::none || !shell_retained())
        return notice;
    notice.id = kConnectionNoticeId + static_cast<u64>(banner);
    switch (banner) {
    case AccountConnectionBanner::reconnecting:
        notice.icon = UiIcon::refresh;
        notice.title = utf8_literal("Reconnecting\xE2\x80\xA6");
        notice.description = utf8_literal("You can keep reading. New messages appear once the connection is back.");
        notice.busy = true;
        break;
    case AccountConnectionBanner::waiting: {
        notice.icon = UiIcon::signal_zero;
        notice.title = utf8_literal("Connection lost");
        notice.description = utf8_literal("You can keep reading. MaiCord will try again shortly.");
        // The retry time is fixed text, so the card never ticks.
        const u64 now = clock_monotonic_milliseconds(), unix_now = clock_unix_milliseconds();
        const u64 at = (unix_now + (g_reconnect.retry_at_ms > now ? g_reconnect.retry_at_ms - now : 0)) / 1000;
        static constexpr char prefix[] = "You can keep reading. MaiCord will try again at ";
        constexpr u32 prefix_bytes = sizeof(prefix) - 1;
        if (scratch.count > prefix_bytes + 2) {
            const auto time = format_timestamp_local(at, 'T', unix_now / 1000,
                                                     {scratch.data + prefix_bytes, scratch.count - prefix_bytes - 1});
            if (time.bytes && prefix_bytes + time.bytes + 1 <= scratch.count) {
                copy_bytes(scratch.data, prefix, prefix_bytes);
                scratch[prefix_bytes + time.bytes] = '.';
                notice.description = {scratch.data, prefix_bytes + time.bytes + 1};
            }
        }
        notice.primary = utf8_literal("Reconnect");
        break;
    }
    case AccountConnectionBanner::offline:
        notice.icon = UiIcon::signal_zero;
        notice.warning = true;
        notice.title = utf8_literal("You're offline");
        notice.description =
            g_reconnect.reason == AccountStopReason::failure
                ? utf8_literal("MaiCord stopped retrying. Check your connection, then reconnect.")
                : utf8_literal("Discord needs you to sign in or finish an account step before reconnecting.");
        notice.primary = utf8_literal("Reconnect");
        break;
    case AccountConnectionBanner::none:
        break;
    }
    return notice;
}

bool application_account_reconnect_now() noexcept {
    const auto banner = account_reconnect_banner(g_reconnect);
    if ((banner != AccountConnectionBanner::waiting && banner != AccountConnectionBanner::offline) ||
        !shell_retained() || g_app.account.instance)
        return false;
    log_write(LogLevel::info, "account", "reconnect_requested");
    account_reconnect_manual(&g_reconnect, clock_monotonic_milliseconds());
    application_request_frame();
    return true;
}

} // namespace maicord::app_detail
