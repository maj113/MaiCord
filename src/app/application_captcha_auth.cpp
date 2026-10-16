#include "application_captcha_auth.hpp"
#include "application_account_logout.hpp"
#include "application_login_ui.hpp"
#include "application_login_auth.hpp"
#include "application_state.hpp"
#include "maicord/core/log.hpp"
#include "maicord/platform/browser_sign_in_launcher.hpp"

namespace maicord::app_detail {
namespace {
bool logout_blocks_captcha() noexcept {
    return application_account_logout_blocked() || application_account_logout_host_owned();
}
// The replayed request is pending again; show the state of the original flow.
void present_submitted() noexcept {
    auto& login = g_app.login;
    if (login.remote_status == LoginRemoteStatus::captcha_required) {
        login.remote_status = LoginRemoteStatus::mobile_pending;
        if (login.status == LoginStatus::captcha_required) login.status = LoginStatus::idle;
    } else if (login.status == LoginStatus::recovery_captcha_required) {
        login.status = LoginStatus::recovery_pending;
    } else if (login.status == LoginStatus::reset_captcha_required) {
        login.status = LoginStatus::reset_pending;
    } else if (login.status == LoginStatus::recovery_phone_captcha_required) {
        login.status = login.sms_sending ? LoginStatus::recovery_phone_resend_pending
                                         : LoginStatus::recovery_phone_verify_pending;
    } else if (login.status == LoginStatus::captcha_required) {
        login.status = login.reset_mode ? LoginStatus::reset_pending : LoginStatus::submitting;
    }
    login.focus = LoginControl::back;
}
bool finish(LoginCaptchaStatus status) noexcept {
    auto& login = g_app.login;
    login.captcha_generation = login.captcha_launch_epoch = login.captcha_deadline_ms = 0;
    login.captcha_status = status;
    if (status != LoginCaptchaStatus::none) login.focus = LoginControl::submit;
    return application_refresh_login_view();
}
} // namespace

void application_captcha_auth_reset() noexcept {
    auto& login = g_app.login;
    login.captcha_generation = login.captcha_launch_epoch = login.captcha_deadline_ms = 0;
    login.captcha_status = LoginCaptchaStatus::none;
    login.captcha_submitted = false;
}

bool application_login_begin_captcha_bridge() noexcept {
    if (logout_blocks_captcha())
        return false;
    if (application_login_auth_cooldown_active())
        return true;
    auto& login = g_app.login;
    auto& host = g_app.account;
    if (!login.visible || !login_captcha_mode(login.status, login.remote_status) || !host.login_instance ||
        host.stop_failed || login.captcha_status == LoginCaptchaStatus::starting)
        return true;
    // A closed tab cannot reuse its spent page; replace the bridge and state.
    if (login.captcha_generation) {
        login.captcha_generation = login.captcha_launch_epoch = 0;
        const auto cancelled = host.api.login_cancel_captcha_bridge(host.login_instance);
        if (cancelled.error == AccountLoginError::stop)
            return finish(LoginCaptchaStatus::failed);
    }
    ++login.application_auth_epoch;
    login.webauthn_post_epoch = 0;
    const u64 epoch = login.application_auth_epoch;
    const auto instance = host.login_instance;
    host.login_status = host.api.login_start_captcha_bridge(instance, epoch, kCaptchaBridgeDefaultTimeoutMs);
    application_login_retain_auth_retry();
    const auto bridge = host.api.login_captcha_bridge_status(instance);
    if (!host.login_status.ok() || bridge.generation != epoch || !bridge.ok() ||
        (bridge.phase != CaptchaBridgePhase::starting && bridge.phase != CaptchaBridgePhase::waiting)) {
        log_u64(LogLevel::warning, "auth", "captcha_bridge_start_error", static_cast<u64>(bridge.error));
        return finish(LoginCaptchaStatus::failed);
    }
    login.captcha_submitted = false;
    login.captcha_generation = login.captcha_launch_epoch = epoch;
    login.captcha_deadline_ms = bridge.deadline_ms;
    login.captcha_status = LoginCaptchaStatus::starting;
    login.focus = LoginControl::back;
    return application_refresh_login_view();
}

bool application_captcha_auth_pump() noexcept {
    if (logout_blocks_captcha())
        return true;
    auto& login = g_app.login;
    auto& host = g_app.account;
    if (!host.login_instance)
        return true;
    if (!login.captcha_generation) {
        // A fresh challenge after a submitted solution means it was refused.
        // The caller's event dispatch presents the challenge and refreshes.
        if (host.login_status.phase == AccountLoginPhase::captcha_required && login.captcha_submitted) {
            login.captcha_submitted = false;
            login.captcha_status = LoginCaptchaStatus::retry;
        } else if (host.login_status.phase != AccountLoginPhase::captcha_required &&
                   !login_captcha_mode(login.status, login.remote_status) && !login.captcha_submitted) {
            login.captcha_status = LoginCaptchaStatus::none;
        }
        return true;
    }
    const auto bridge = host.api.login_captcha_bridge_status(host.login_instance);
    if (bridge.generation != login.captcha_generation)
        return true;
    switch (bridge.phase) {
    case CaptchaBridgePhase::starting:
    case CaptchaBridgePhase::token_ready:
        return true;
    case CaptchaBridgePhase::waiting:
        break;
    case CaptchaBridgePhase::complete:
        if (!bridge.ok()) return finish(LoginCaptchaStatus::failed);
        login.captcha_submitted = true;
        present_submitted();
        return finish(LoginCaptchaStatus::none);
    case CaptchaBridgePhase::expired:
        return finish(LoginCaptchaStatus::expired);
    default:
        log_u64(LogLevel::warning, "auth", "captcha_bridge_error", static_cast<u64>(bridge.error));
        return finish(LoginCaptchaStatus::failed);
    }
    if (login.captcha_launch_epoch && login.captcha_launch_epoch == login.captcha_generation &&
        login.captcha_launch_epoch == login.application_auth_epoch) {
        const u64 epoch = login.captcha_launch_epoch;
        const auto module = host.module;
        const auto instance = host.login_instance;
        login.captcha_launch_epoch = 0; // consume before the shell can dispatch messages
        SecureBuffer url{};
        const bool copied = secure_buffer_initialize(&url, kCaptchaBridgeMaximumUrlBytes).ok() &&
                            host.api.login_copy_captcha_bridge_url(instance, epoch, &url).ok();
        const auto current = [&]() noexcept {
            return !logout_blocks_captcha() && !application_login_auth_cooldown_active() &&
                   epoch == login.captcha_generation && host.module == module &&
                   host.login_instance == instance && login.visible && login.status != LoginStatus::cancelling;
        };
        const bool opened = copied && current() && captcha_bridge_launch(g_app.window, &url);
        secure_buffer_release(&url);
        if (!current())
            return true;
        if (!opened) {
            host.api.login_cancel_captcha_bridge(instance);
            return finish(LoginCaptchaStatus::failed);
        }
    }
    if (login.captcha_status == LoginCaptchaStatus::waiting)
        return true;
    login.captcha_status = LoginCaptchaStatus::waiting;
    login.captcha_deadline_ms = bridge.deadline_ms;
    login.focus = LoginControl::submit;
    return application_refresh_login_view();
}

u64 application_login_captcha_deadline() noexcept {
    const auto& login = g_app.login;
    return !logout_blocks_captcha() && login.visible && login.captcha_generation && g_app.account.login_instance &&
                   !g_app.account.stop_failed
               ? login.captcha_deadline_ms
               : 0;
}

bool application_login_tick_captcha(u64 now_ms) noexcept {
    const u64 due = application_login_captcha_deadline();
    if (!due || now_ms < due)
        return true;
    // The worker enforces the same deadline; this keeps the UI exact even if
    // its notification is coalesced with other login traffic.
    g_app.account.api.login_cancel_captcha_bridge(g_app.account.login_instance);
    return finish(LoginCaptchaStatus::expired);
}
} // namespace maicord::app_detail
