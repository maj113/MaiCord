#include "auth_internal.hpp"

namespace maicord::app_detail {

u64 application_login_recovery_deadline() noexcept {
    if (application_account_logout_locked())
        return 0;
    return g_app.login.visible && g_app.login.status == LoginStatus::recovery_rate_limited &&
                   !g_app.account.stop_failed && g_app.account.login_instance
               ? g_app.account.login_status.retry_at_ms
               : 0;
}

bool application_login_tick_recovery(u64 now_ms) noexcept {
    if (application_account_logout_locked())
        return true;
    const u64 due = application_login_recovery_deadline();
    if (!due || now_ms < due) return true;
    g_app.account.login_status.retry_at_ms = 0;
    return present(LoginStatus::recovery_failed, LoginControl::login);
}

namespace auth_detail {

bool quiesce_recovery_owner() noexcept {
    if (application_account_logout_locked())
        return false;
    auto& login = g_app.login;
    invalidate_auth();
    application_login_end_pointer_selection();
    application_login_reset_remote_session();
    application_login_reset_attempt();
    secure_edit_clear(&login.password);
    secure_edit_clear(&login.mfa);
    login.mfa_capabilities = 0;
    if (g_app.account.login_instance) {
        const auto stopped = stop_login_owner();
        if (g_app.account.login_instance)
            log_u64(LogLevel::warning, "auth", "recovery_owner_stop_error", static_cast<u64>(stopped.error));
    }
    if (g_app.account.login_instance || g_app.account.stop_failed) {
        secure_edit_clear(&login.login);
        login.status = LoginStatus::cancelling;
        login.focus = LoginControl::back;
        return false;
    }
    return true;
}

bool adopt_reset_token(SecureBuffer* token) noexcept {
    if (application_account_logout_locked())
        return false;
    auto& login = g_app.login;
    if (!token || !token->data || !token->locked || !token->bytes ||
        token->bytes > kLoginResetMaximumTokenBytes || token->bytes > token->capacity ||
        g_app.account.login_instance || g_app.account.module || g_app.account.stop_failed) return false;
    secure_text_store_unbind(&login.text_store);
    application_login_end_pointer_selection();
    secure_edit_clear(&login.login);
    secure_buffer_release(&login.login.storage);
    if (!secure_buffer_transfer(&login.login.storage, token)) return false;
    // The protected handoff allocation becomes the existing hidden edit owner.
    // No temporary URI, extra retained token bank or token copy is needed.
    login.login.password = true;
    login.reset_mode = true; login.reset_retry = false;
    login.status = LoginStatus::reset_editing; login.focus = LoginControl::password;
    return true;
}

void restore_identifier_bound() noexcept {
    auto& edit = g_app.login.login;
    // Wipe the complete handoff range before reducing its logical size.
    // Both the original 512-byte owner and this 4096-byte owner use one VM page.
    if (edit.storage.capacity == kLoginResetMaximumTokenBytes) {
        secure_edit_clear(&edit);
        edit.storage.capacity = kUserAuthMaximumLoginBytes;
    }
}

} // namespace auth_detail

bool application_login_begin_recovery() noexcept {
    if (application_account_logout_locked())
        return false;
    if (!g_app.login.visible || login_control_action(LoginControl::forgot_password, g_app.login.status,
                                                     g_app.login.remote_status) != LoginAction::recovery)
        return true;
    if (!quiesce_recovery_owner()) return application_refresh_login_view();
    return present(LoginStatus::recovery_editing, LoginControl::login);
}

bool application_login_begin_reset() noexcept {
    if (application_account_logout_locked())
        return false;
    auto& login = g_app.login;
    if (!login.visible || login.reset_mode || login_requires_back(login.status, login.remote_status)) return true;
    VerificationInput input{};
    const bool allocated = secure_buffer_initialize(&input.token, kLoginResetMaximumTokenBytes).ok();
    const auto consumed = allocated ? login_reset_consume_link(
        {reinterpret_cast<const char*>(login.login.storage.data), login.login.storage.bytes}, &input.token)
        : LoginResetLinkResult{0, LoginResetLinkError::invalid_input};
    secure_text_store_unbind(&login.text_store);
    secure_edit_clear(&login.login);
    if (!quiesce_recovery_owner()) return application_refresh_login_view();
    application_account_module_release();
    if (g_app.account.module)
        return present(LoginStatus::cancelling, LoginControl::back);
    if (!consumed.ok()) {
        login.login.password = false;
        return present(LoginStatus::rejected, LoginControl::login);
    }
    if (!adopt_reset_token(&input.token))
        return present(LoginStatus::rejected, LoginControl::login);
    return application_refresh_login_view();
}

bool application_login_submit_reset() noexcept {
    if (application_account_logout_locked())
        return false;
    if (application_login_auth_cooldown_active())
        return true;
    auto& login = g_app.login;
    auto& host = g_app.account;
    if (!login.visible || !login.reset_mode || !login_reset_editable(login.status) || host.stop_failed) return true;
    const auto password = secure_buffer_view(login.password.storage);
    const auto valid = utf8_validate({reinterpret_cast<const char*>(password.data), password.count});
    if (!login.password.storage.locked || !valid.valid || valid.codepoints < 8)
        return present_focus(LoginControl::password);
    if (!host.login_instance) {
        if (login.reset_retry || !login.login.storage.locked || !login.login.storage.bytes)
            return reset_failed();
        AccountPaths paths{};
        if (!application_login_prepare_owner(&paths))
            return reset_failed();
        invalidate_auth();
        host.login_status = account_host_login_start_reset(&host, paths.profile);
        if (!host.login_status.ok() || !host.login_instance)
            return reset_failed();
    }
    host.login_status = account_host_login_submit_reset(&host,
        login.reset_retry ? nullptr : &login.login.storage, &login.password.storage);
    retain_auth_retry(host.login_status);
    if (!host.login_status.ok() || host.login_status.phase != AccountLoginPhase::reset_pending) {
        secure_edit_clear(&login.login);
        secure_edit_clear(&login.password);
        return present(LoginStatus::reset_failed, LoginControl::back);
    }
    login.reset_retry = false;
    secure_edit_clear(&login.mfa);
    secure_edit_clear(&login.login);
    secure_edit_clear(&login.password);
    return present(LoginStatus::reset_pending, LoginControl::back);
}

u64 application_login_reset_deadline() noexcept {
    if (application_account_logout_locked())
        return 0;
    return g_app.login.visible && g_app.login.reset_mode &&
                   g_app.login.status == LoginStatus::reset_rate_limited && !g_app.account.stop_failed &&
                   g_app.account.login_instance
               ? g_app.account.login_status.retry_at_ms
               : 0;
}

bool application_login_tick_reset(u64 now_ms) noexcept {
    if (application_account_logout_locked())
        return true;
    const auto due = application_login_reset_deadline();
    if (!due || now_ms < due) return true;
    g_app.account.login_status.retry_at_ms = 0;
    auto& login = g_app.login;
    invalidate_auth();
    if (login.mfa_capabilities) {
        login.reset_retry = false;
        login.webauthn_attempted = false;
        if (login.sms_sending) {
            login.sms_sending = false; login.sms_requested = false; login.sms_failed = true;
        }
        return present_mfa_retry(LoginStatus::mfa_required);
    }
    login.reset_retry = true;
    return present(LoginStatus::reset_editing, LoginControl::password);
}

bool application_login_submit_recovery() noexcept {
    if (application_account_logout_locked())
        return false;
    if (application_login_auth_cooldown_active())
        return true;
    auto& login = g_app.login;
    auto& host = g_app.account;
    if (!login.visible || !login_recovery_editable(login.status) || host.stop_failed || request_throttled())
        return true;
    const auto& identifier = login.login.storage;
    const Utf8View value{reinterpret_cast<const char*>(identifier.data), identifier.bytes};
    if (!identifier.locked || !identifier.bytes || identifier.bytes > kUserAuthMaximumLoginBytes ||
        login.login.password || !user_auth_recovery_identifier_valid(value))
        return present(LoginStatus::recovery_failed, LoginControl::login);
    // A failed/previous request owner must finish before a new explicit request.
    // The identifier stays solely in its existing locked edit through handoff.
    if (!quiesce_recovery_owner()) return application_refresh_login_view();
    if (application_login_auth_cooldown_active())
        return application_refresh_login_view();
    AccountPaths paths{};
    if (!application_login_prepare_owner(&paths))
        return present(LoginStatus::recovery_failed);
    host.login_status = account_host_login_start(&host, paths.profile);
    if (!host.login_status.ok() || !host.login_instance)
        return present(LoginStatus::recovery_failed);
    const auto submitted = account_host_login_submit_recovery(&host, &login.login.storage);
    retain_auth_retry(submitted);
    if (!submitted.ok() || submitted.phase != AccountLoginPhase::recovery_pending)
        return present(LoginStatus::recovery_failed);
    secure_edit_clear(&login.login);
    return present(LoginStatus::recovery_pending, LoginControl::back);
}

bool application_login_submit_phone_verify() noexcept {
    if (application_account_logout_locked())
        return false;
    if (application_login_auth_cooldown_active())
        return true;
    auto& login = g_app.login;
    if (phone_request_blocked())
        return true;
    if (secure_text_store_composing(login.text_store)) return true;
    const Utf8View code{reinterpret_cast<const char*>(login.mfa.storage.data), login.mfa.storage.bytes};
    if (!login.mfa.storage.locked || !login_phone_code_valid(code))
        return present(LoginStatus::recovery_phone_input_invalid, LoginControl::phone_code);
    invalidate_auth();
    application_login_end_pointer_selection();
    const auto submitted = account_host_login_submit_phone_verify(&g_app.account, &login.mfa.storage);
    retain_auth_retry(submitted);
    secure_edit_clear(&login.mfa);
    login.sms_sending = false;
    return present(submitted.ok() && submitted.phase == AccountLoginPhase::phone_verify_pending
                       ? LoginStatus::recovery_phone_verify_pending
                       : LoginStatus::recovery_phone_failed,
                   LoginControl::back);
}

bool application_login_submit_phone_resend() noexcept {
    if (application_account_logout_locked())
        return false;
    if (application_login_auth_cooldown_active())
        return true;
    auto& login = g_app.login;
    if (phone_request_blocked())
        return true;
    invalidate_auth();
    application_login_end_pointer_selection();
    const auto submitted = account_host_login_submit_phone_resend(&g_app.account);
    retain_auth_retry(submitted);
    login.sms_sending = submitted.ok() && submitted.phase == AccountLoginPhase::phone_resend_pending;
    return present(login.sms_sending ? LoginStatus::recovery_phone_resend_pending
                                     : LoginStatus::recovery_phone_failed,
                   LoginControl::back);
}

bool application_login_run_phone_verify(u64 posted_epoch, u32 posted_revision) noexcept {
    if (application_account_logout_locked())
        return true;
    const auto& login = g_app.login;
    if (!login.visible || posted_epoch != login.application_auth_epoch || posted_revision != login.mfa.revision ||
        !login_phone_editable(login.status)) return true;
    if (secure_text_store_composing(login.text_store)) return true;
    const Utf8View code{reinterpret_cast<const char*>(login.mfa.storage.data), login.mfa.storage.bytes};
    return !login_phone_code_valid(code) || application_login_submit_phone_verify();
}

u64 application_login_phone_deadline() noexcept {
    if (application_account_logout_locked())
        return 0;
    const auto& login = g_app.login;
    return login.visible && (login.status == LoginStatus::recovery_phone_verify_rate_limited ||
        login.status == LoginStatus::recovery_phone_resend_rate_limited) &&
        !g_app.account.stop_failed && g_app.account.login_instance ? g_app.account.login_status.retry_at_ms : 0;
}

bool application_login_tick_phone(u64 now_ms) noexcept {
    if (application_account_logout_locked())
        return true;
    const u64 due = application_login_phone_deadline();
    if (!due || now_ms < due) return true;
    g_app.account.login_status.retry_at_ms = 0;
    g_app.login.sms_sending = false;
    return present(LoginStatus::recovery_phone_required, LoginControl::phone_code);
}

} // namespace maicord::app_detail
