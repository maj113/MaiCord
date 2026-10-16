#include "application_browser_auth.hpp"
#include "application_captcha_auth.hpp"
#include "application_account_module.hpp"
#include "application_account_logout.hpp"
#include "application_login_auth.hpp"
#include "application_login_input.hpp"
#include "application_login_ui.hpp"
#include "application_state.hpp"
#include "maicord/app/account_paths.hpp"
#include "maicord/platform/browser_sign_in_launcher.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
bool logout_blocks_browser_auth() noexcept {
    return application_account_logout_blocked() || application_account_logout_host_owned();
}
bool stop_browser_owner(LoginStatus terminal) noexcept {
    auto& login = g_app.login;
    auto& host = g_app.account;
    login.browser_launch_epoch = 0;
    if (host.login_instance) {
        account_host_login_stop(&host, kAccountStopTimeoutMs);
        application_login_retain_auth_retry();
    }
    if (host.login_instance || host.stop_failed) {
        login.status = LoginStatus::cancelling;
        login.focus = LoginControl::back;
        return application_refresh_login_view();
    }
    application_account_module_release();
    login.status = host.module ? LoginStatus::cancelling : terminal;
    login.focus = LoginControl::back;
    return application_refresh_login_view();
}
} // namespace

bool application_login_begin_browser_auth() noexcept {
    if (logout_blocks_browser_auth())
        return false;
    if (application_login_auth_cooldown_active())
        return true;
    auto& login = g_app.login;
    auto& host = g_app.account;
    if (!login.visible || !login_browser_available(login.status, login.remote_status, login.reset_mode))
        return true;
    login.browser_return_to_qr = true;
    ++login.application_auth_epoch;
    login.webauthn_post_epoch = 0;
    login.browser_launch_epoch = 0;
    secure_text_store_unbind(&login.text_store);
    application_login_end_pointer_selection();
    secure_edit_clear(&login.login);
    secure_edit_clear(&login.password);
    secure_edit_clear(&login.mfa);
    login.remote_active = false;
    login.qr_ready = false;
    login.remote_restart_at_ms = 0;
    login.remote_retry_attempt = 0;
    login.remote_status = LoginRemoteStatus::idle;
    SecureZeroMemory(&login.qr_code, sizeof(login.qr_code));
    SecureZeroMemory(login.remote_user, sizeof(login.remote_user));
    login.remote_user_bytes = 0;
    login.mfa_capabilities = 0;
    login.sms_requested = login.sms_sending = login.sms_failed = false;
    login.webauthn_attempted = false;
    application_login_reset_backup_acknowledgement();
    application_captcha_auth_reset();
    login.password_revealed = false;
    login.status = LoginStatus::browser_starting;
    login.focus = LoginControl::back;
    if (host.login_instance) {
        account_host_login_stop(&host, kAccountStopTimeoutMs);
        application_login_retain_auth_retry();
    }
    if (host.login_instance || host.stop_failed) {
        login.status = LoginStatus::cancelling;
        return application_refresh_login_view();
    }
    application_account_module_release();
    if (application_login_auth_cooldown_active()) {
        login.status = LoginStatus::browser_failed;
        login.focus = LoginControl::back;
        return application_refresh_login_view();
    }
    if (host.module) {
        login.status = LoginStatus::cancelling;
        return application_refresh_login_view();
    }
    AccountPaths paths{};
    const auto resolved = account_paths_resolve(&paths);
    const auto ensured = resolved.ok() ? account_paths_ensure_directories(paths) : resolved;
    if (!ensured.ok() || !application_account_module_load())
        return stop_browser_owner(LoginStatus::browser_failed);
    host.login_status = account_host_login_start(&host, paths.profile);
    if (!host.login_instance || !host.login_status.ok())
        return stop_browser_owner(LoginStatus::browser_failed);
    host.login_status =
        host.api.login_start_browser(host.login_instance, login.application_auth_epoch, 120000);
    application_login_retain_auth_retry();
    if (!host.login_status.ok())
        return stop_browser_owner(LoginStatus::browser_failed);
    // Retain explicit click authorization until the asynchronous listener is
    // ready. Cancellation/replacement invalidates this epoch before teardown.
    login.browser_launch_epoch = login.application_auth_epoch;
    return application_refresh_login_view();
}

bool application_browser_auth_pump(bool* handled) noexcept {
    if (!handled)
        return false;
    *handled = false;
    if (logout_blocks_browser_auth()) {
        *handled = true;
        return true;
    }
    auto& login = g_app.login;
    auto& host = g_app.account;
    if (!host.login_status.browser_generation)
        return true;
    if (host.login_status.phase == AccountLoginPhase::authenticated)
        return true;
    *handled = true;
    if (host.login_status.phase == AccountLoginPhase::browser_expired)
        return stop_browser_owner(LoginStatus::browser_expired);
    if (!host.login_status.ok() || host.login_status.phase == AccountLoginPhase::browser_failed)
        return stop_browser_owner(LoginStatus::browser_failed);
    const auto status = host.login_status.phase == AccountLoginPhase::browser_starting
                            ? LoginStatus::browser_starting
                        : host.login_status.phase == AccountLoginPhase::browser_exchange_pending
                            ? LoginStatus::browser_exchange_pending
                            : LoginStatus::browser_waiting;
    if (host.login_status.phase == AccountLoginPhase::browser_waiting && login.browser_launch_epoch &&
        login.browser_launch_epoch == login.application_auth_epoch &&
        host.login_status.browser_generation == login.application_auth_epoch) {
        const u64 epoch = login.browser_launch_epoch;
        const auto module = host.module;
        const auto instance = host.login_instance;
        login.browser_launch_epoch = 0; // consume before a shell call can dispatch messages
        SecureBuffer url{};
        const bool copied = secure_buffer_initialize(&url, 256).ok() &&
                            host.api.login_copy_browser_url(instance, epoch, &url).ok();
        const bool current = !logout_blocks_browser_auth() && !application_login_auth_cooldown_active() &&
                             epoch == login.application_auth_epoch &&
                             host.module == module && host.login_instance == instance && login.visible &&
                             login.status != LoginStatus::cancelling;
        const bool opened = copied && current && browser_sign_in_launch(g_app.window, &url);
        secure_buffer_release(&url);
        if (logout_blocks_browser_auth() || epoch != login.application_auth_epoch ||
            host.module != module || host.login_instance != instance || !login.visible ||
            login.status == LoginStatus::cancelling)
            return true;
        if (!opened)
            return stop_browser_owner(LoginStatus::browser_failed);
    }
    if (login.status == status)
        return true;
    login.status = status;
    login.focus = LoginControl::back;
    return application_refresh_login_view();
}
} // namespace maicord::app_detail
