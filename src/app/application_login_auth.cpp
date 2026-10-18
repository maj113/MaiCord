#include "maicord/core/win32_process.hpp"
#include "application_login_auth.hpp"
#include "application_browser_auth.hpp"
#include "application_captcha_auth.hpp"

#include "application_account_module.hpp"
#include "application_account_logout.hpp"
#include "application_login_input.hpp"
#include "application_login_ui.hpp"
#include "application_login_geometry.hpp"
#include "application_state.hpp"

#include "maicord/app/account_paths.hpp"
#include "maicord/app/login_mfa_policy.hpp"
#include "maicord/app/login_continuations.hpp"
#include "maicord/protocol/login_continuations.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/platform/webauthn.hpp"
#include "maicord/protocol/remote_auth.hpp"
#include "maicord/protocol/webauthn_mfa.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {

void retain_auth_retry(const AccountLoginStatus& status) noexcept {
    if (status.http_status == 429) {
        // Unknown receipts cannot erase a known floor or authorize an invented retry.
        const u64 due = status.retry_at_ms;
        if (due && due <= clock_monotonic_milliseconds())
            return;
        if (!due) g_app.login.auth_retry_time_unknown = true;
        g_app.login.auth_retry_at_ms = maximum(g_app.login.auth_retry_at_ms, due);
        if (g_app.login.remote_restart_at_ms)
            g_app.login.remote_restart_at_ms =
                g_app.login.auth_retry_time_unknown
                    ? 0
                    : maximum(g_app.login.remote_restart_at_ms, g_app.login.auth_retry_at_ms);
    }
}

AccountLoginStatus stop_login_owner() noexcept {
    retain_auth_retry(g_app.account.login_status);
    const auto stopped = account_host_login_stop(&g_app.account, kAccountStopTimeoutMs);
    // Stop joins the worker and can observe a response not yet pumped by UI.
    retain_auth_retry(stopped);
    return stopped;
}

void invalidate_auth() noexcept {
    ++g_app.login.application_auth_epoch;
    g_app.login.webauthn_post_epoch = 0;
}

AccountLoginPhase mfa_pending_phase() noexcept {
    return g_app.login.reset_mode ? AccountLoginPhase::reset_mfa_pending : AccountLoginPhase::mfa_pending;
}

bool present(LoginStatus status) noexcept {
    g_app.login.status = status;
    return application_refresh_login_view();
}

bool present(LoginStatus status, LoginControl focus) noexcept {
    g_app.login.focus = focus;
    return present(status);
}

bool present_focus(LoginControl focus) noexcept {
    g_app.login.focus = focus;
    return application_refresh_login_view();
}

// Back to code entry, or the WebAuthn retry, after the server turned an MFA attempt down.
bool present_mfa_retry(LoginStatus code_status) noexcept {
    const bool webauthn = g_app.login.mfa_type == UserAuthMfaType::webauthn;
    return present(webauthn ? LoginStatus::webauthn_failed : code_status,
                   webauthn ? LoginControl::submit : LoginControl::password);
}

// An MFA request is in flight.
bool present_mfa_pending(LoginControl focus) noexcept {
    return present(g_app.login.reset_mode ? LoginStatus::reset_mfa_pending : LoginStatus::submitting,
                   focus);
}

bool webauthn_failed() noexcept {
    g_app.login.webauthn_attempted = false;
    return present(LoginStatus::webauthn_failed);
}

// The server asked this login owner to wait before another request.
bool request_throttled() noexcept {
    const u64 retry_at_ms = g_app.account.login_status.retry_at_ms;
    return retry_at_ms && clock_monotonic_milliseconds() < retry_at_ms;
}

bool phone_request_blocked() noexcept {
    const auto& host = g_app.account;
    return !g_app.login.visible || !login_phone_editable(g_app.login.status) || host.stop_failed ||
           !host.login_instance || request_throttled();
}

// Back and Escape can leave a pending flow, a rate limit or an in-flight WebAuthn ceremony.
bool back_available() noexcept {
    const auto& login = g_app.login;
    return login_requires_back(login.status, login.remote_status) || login_phone_mode(login.status) ||
           login_recovery_mode(login.status) || login.reset_mode || application_login_auth_cooldown_active() ||
           (login.webauthn_attempted && login.status == LoginStatus::submitting);
}

bool reset_failed() noexcept {
    auto& login = g_app.login;
    secure_edit_clear(&login.login);
    secure_edit_clear(&login.password);
    secure_edit_clear(&login.mfa);
    return present(LoginStatus::reset_failed, LoginControl::back);
}

void reset_mfa_progress() noexcept {
    auto& login = g_app.login;
    login.sms_requested = login.sms_sending = login.sms_failed = false;
    login.webauthn_attempted = false;
    application_login_reset_backup_acknowledgement();
}

struct VerificationInput {
    SecureBuffer token;
    ~VerificationInput() noexcept { secure_buffer_release(&token); }
};

bool login_control_mfa_type(LoginControl control, UserAuthMfaType* type) noexcept {
    switch (control) {
    case LoginControl::mfa_totp:
        *type = UserAuthMfaType::totp;
        return true;
    case LoginControl::mfa_sms:
        *type = UserAuthMfaType::sms;
        return true;
    case LoginControl::mfa_backup:
        *type = UserAuthMfaType::backup;
        return true;
    case LoginControl::mfa_webauthn:
        *type = UserAuthMfaType::webauthn;
        return true;
    default:
        return false;
    }
}

bool request_sms(bool allow_resend) noexcept {
    if (application_account_logout_locked())
        return false;
    if (application_login_auth_cooldown_active())
        return true;
    LoginRuntime& login = g_app.login;
    AccountHost& host = g_app.account;
    if (!host.login_instance || (login.mfa_capabilities & user_auth_mfa_capability_sms) == 0) {
        login.sms_sending = false;
        login.sms_failed = true;
        return application_refresh_login_view();
    }
    if (login.sms_sending) {
        return true;
    }
    if (login.sms_requested && !allow_resend) {
        return application_refresh_login_view();
    }
    host.login_status = host.api.login_send_sms(host.login_instance);
    retain_auth_retry(host.login_status);
    if (!host.login_status.ok() || host.login_status.phase != mfa_pending_phase()) {
        log_u64(LogLevel::error, "auth", "sms_send_error", static_cast<u64>(host.login_status.error));
        login.sms_sending = false;
        login.sms_requested = false;
        login.sms_failed = true;
        return application_refresh_login_view();
    }
    login.sms_sending = true;
    login.sms_failed = false;
    return present(login.reset_mode ? LoginStatus::reset_mfa_pending : LoginStatus::mfa_required,
                   LoginControl::password);
}

bool begin_webauthn() noexcept {
    if (application_account_logout_locked())
        return false;
    if (application_login_auth_cooldown_active())
        return true;
    if (!g_app.account.login_instance || !g_app.account.api.login_copy_webauthn_challenge)
        return present(LoginStatus::webauthn_failed);
    if (g_app.login.webauthn_attempted) {
        return true;
    }
    g_app.login.webauthn_attempted = true;
    invalidate_auth();
    g_app.login.webauthn_post_epoch = g_app.login.application_auth_epoch;
    if (!present(LoginStatus::webauthn_required, LoginControl::submit)) {
        return false;
    }
    if (!PostMessageW(g_app.window, kBeginWebAuthnMessage, static_cast<WPARAM>(g_app.login.webauthn_post_epoch), 0)) {
        log_u64(LogLevel::error, "auth", "webauthn_post_native_error", thread_last_error());
        return webauthn_failed();
    }
    return true;
}

} // namespace

void application_login_reset_remote_session() noexcept {
    auto& login = g_app.login;
    login.remote_active = false;
    login.qr_ready = false;
    login.remote_status = LoginRemoteStatus::idle;
    login.remote_restart_at_ms = 0;
    login.remote_retry_attempt = 0;
    secure_clear_bytes(&login.qr_code, sizeof(login.qr_code));
    secure_clear_bytes(login.remote_user, sizeof(login.remote_user));
    login.remote_user_bytes = 0;
}

void application_login_reset_attempt() noexcept {
    reset_mfa_progress();
    application_captcha_auth_reset();
    g_app.login.password_revealed = false;
}

bool application_login_prepare_owner(AccountPaths* paths, AccountPathsError* error) noexcept {
    const auto resolved = account_paths_resolve(paths);
    const auto ensured = resolved.ok() ? account_paths_ensure_directories(*paths) : resolved;
    if (error)
        *error = ensured.error;
    return ensured.ok() && application_account_module_load();
}

bool application_login_auth_cooldown_active() noexcept {
    return g_app.login.auth_retry_time_unknown ||
        (g_app.login.auth_retry_at_ms && clock_monotonic_milliseconds() < g_app.login.auth_retry_at_ms);
}

void application_login_retain_auth_retry() noexcept {
    retain_auth_retry(g_app.account.login_status);
}

u64 application_login_auth_deadline() noexcept {
    return !application_account_logout_locked() && g_app.login.visible ? g_app.login.auth_retry_at_ms : 0;
}

bool application_login_tick_auth_cooldown(u64 now_ms) noexcept {
    if (application_account_logout_locked() || !g_app.login.auth_retry_at_ms ||
        now_ms < g_app.login.auth_retry_at_ms)
        return true;
    g_app.login.auth_retry_at_ms = 0;
    // Expiry changes presentation only. Credentials and MFA require a fresh action.
    return !g_app.login.visible || application_refresh_login_view();
}

void application_login_reset_backup_acknowledgement() noexcept {
    g_app.login.backup_acknowledged = false;
    g_app.login.backup_acknowledgement_seconds = 0;
    g_app.login.backup_acknowledgement_at_ms = 0;
}

void application_login_schedule_remote_restart(LoginRemoteStatus status) noexcept {
    if (application_account_logout_locked())
        return;
    LoginRuntime& login = g_app.login;
    const u32 shift = minimum<u32>(login.remote_retry_attempt, 4);
    const u32 base = minimum<u32>(kRemoteAuthRetryMinimumMs << shift, kRemoteAuthRetryMaximumMs);
    const u64 now_ms = clock_monotonic_milliseconds();
    const u32 jitter = static_cast<u32>((now_ms ^ (now_ms >> 17)) % 501);
    const u32 delay = minimum<u32>(base + jitter, kRemoteAuthRetryMaximumMs);
    if (login.remote_retry_attempt != 0xff) {
        ++login.remote_retry_attempt;
    }
    login.remote_status = status;
    login.remote_restart_at_ms = login.auth_retry_time_unknown
                                     ? 0
                                     : maximum(now_ms + delay, login.auth_retry_at_ms);
}

bool application_login_tick_backup_acknowledgement(u64 now_ms) noexcept {
    if (application_account_logout_locked())
        return true;
    LoginRuntime& login = g_app.login;
    if (login.status != RendererLoginStatus::mfa_backup_acknowledgement ||
        login.backup_acknowledgement_at_ms == 0) {
        return true;
    }
    const u8 seconds = static_cast<u8>(
        login_mfa_backup_acknowledgement_seconds(now_ms, login.backup_acknowledgement_at_ms));
    if (seconds == login.backup_acknowledgement_seconds) {
        return true;
    }
    login.backup_acknowledgement_seconds = seconds;
    if (seconds == 0) {
        login.backup_acknowledgement_at_ms = 0;
    }
    return application_refresh_login_view();
}

bool application_login_select_mfa(UserAuthMfaType type, bool allow_resend) noexcept {
    if (application_account_logout_locked())
        return false;
    if (application_login_auth_cooldown_active())
        return true;
    LoginRuntime& login = g_app.login;
    const u8 capability = login_mfa_capability(type);
    if (capability == login_mfa_capability_none || (login.mfa_capabilities & capability) == 0) {
        return true;
    }
    if (login.mfa_type != type) {
        invalidate_auth();
        secure_edit_clear(&login.mfa);
        login.webauthn_attempted = false;
        application_login_reset_backup_acknowledgement();
    }
    login.mfa_type = type;
    if (type == UserAuthMfaType::webauthn) {
        login.status = RendererLoginStatus::webauthn_required;
        login.focus = LoginControl::mfa_webauthn;
        return begin_webauthn();
    }
    if (type == UserAuthMfaType::backup && !login.backup_acknowledged) {
        if (login.backup_acknowledgement_at_ms == 0) {
            const u64 now_ms = clock_monotonic_milliseconds();
            login.backup_acknowledgement_at_ms = now_ms + kLoginMfaBackupAcknowledgementMilliseconds;
            login.backup_acknowledgement_seconds = static_cast<u8>(
                login_mfa_backup_acknowledgement_seconds(now_ms, login.backup_acknowledgement_at_ms));
        }
        return present(LoginStatus::mfa_backup_acknowledgement, LoginControl::submit);
    }
    login.status = RendererLoginStatus::mfa_required;
    login.focus = LoginControl::password;
    return type == UserAuthMfaType::sms ? request_sms(allow_resend) : application_refresh_login_view();
}

bool application_login_run_webauthn(u64 posted_epoch) noexcept {
    if (application_account_logout_locked())
        return true;
    if (application_login_auth_cooldown_active())
        return true;
    AccountHost& host = g_app.account;
    if (!g_app.login.visible || !posted_epoch || posted_epoch != g_app.login.webauthn_post_epoch ||
        posted_epoch != g_app.login.application_auth_epoch || !host.login_instance || host.stop_failed ||
        g_app.login.status != RendererLoginStatus::webauthn_required || !g_app.login.webauthn_attempted ||
        login_requires_back(g_app.login.status, g_app.login.remote_status) ||
        g_app.login.mfa_type != UserAuthMfaType::webauthn) {
        return true;
    }
    SecureBuffer challenge{};
    SecureBuffer credential{};
    SecureBufferResult secured =
        secure_buffer_initialize(&challenge, kUserAuthMaximumWebauthnChallengeBytes);
    if (!secured.ok()) {
        log_u64(LogLevel::error, "auth", "webauthn_challenge_memory_error", secured.native_error);
        return webauthn_failed();
    }
    secured = host.api.login_copy_webauthn_challenge(host.login_instance, &challenge);
    if (!secured.ok()) {
        log_u64(LogLevel::error, "auth", "webauthn_challenge_copy_error", static_cast<u64>(secured.error));
        secure_buffer_release(&challenge);
        return webauthn_failed();
    }
    WebAuthnMfaRequestOptions request{};
    u32 structurals[kWebAuthnStructuralCapacity]{};
    const WebAuthnMfaResult decoded = webauthn_mfa_decode_request(
        Utf8View{reinterpret_cast<const char*>(challenge.data), challenge.bytes},
        Slice<u32>{structurals, kWebAuthnStructuralCapacity}, 16, &request);
    secure_buffer_release(&challenge);
    secure_clear_bytes(structurals, sizeof(structurals));
    if (!decoded.ok()) {
        log_u64(LogLevel::error, "auth", "webauthn_challenge_decode_error",
                static_cast<u64>(decoded.error));
        secure_clear_bytes(&request, sizeof(request));
        return webauthn_failed();
    }
    secured = secure_buffer_initialize(&credential, kUserAuthMaximumWebauthnCredentialBytes);
    if (!secured.ok()) {
        log_u64(LogLevel::error, "auth", "webauthn_credential_memory_error", secured.native_error);
        secure_clear_bytes(&request, sizeof(request));
        return webauthn_failed();
    }
    const auto owner = host.login_instance;
    const auto module = host.module;
    const auto generation = host.login_status.generation;
    const auto request_id = host.login_status.request_id;
    const auto phase = host.login_status.phase;
    const auto epoch = g_app.login.application_auth_epoch;
    const auto method = g_app.login.mfa_type;
    const bool reset = g_app.login.reset_mode;
    // Keep Back available during nested native message processing. No module
    // API pointer or challenge view is retained across the platform ceremony.
    if (!present_mfa_pending(LoginControl::back)) {
        secure_clear_bytes(&request, sizeof(request));
        secure_buffer_release(&credential);
        return false;
    }
    const WindowsWebAuthnResult asserted =
        windows_webauthn_authenticate(g_app.window, request, &credential);
    secure_clear_bytes(&request, sizeof(request));
    if (application_account_logout_locked() || application_login_auth_cooldown_active() ||
        host.login_instance != owner || host.module != module ||
        host.stop_failed || host.login_status.generation != generation ||
        host.login_status.request_id != request_id || host.login_status.phase != phase ||
        g_app.login.application_auth_epoch != epoch || g_app.login.webauthn_post_epoch != posted_epoch ||
        !g_app.login.visible || g_app.login.reset_mode != reset || g_app.login.mfa_type != method ||
        !g_app.login.webauthn_attempted ||
        g_app.login.status !=
            (reset ? RendererLoginStatus::reset_mfa_pending : RendererLoginStatus::submitting)) {
        secure_buffer_release(&credential);
        return true;
    }
    if (!asserted.ok()) {
        log_u64(LogLevel::error, "auth", "webauthn_platform_error", static_cast<u64>(asserted.error));
        log_u64(LogLevel::error, "auth", "webauthn_hresult", static_cast<u32>(asserted.hresult));
        secure_buffer_release(&credential);
        return webauthn_failed();
    }
    host.login_status =
        host.api.login_submit_mfa(host.login_instance, &credential, UserAuthMfaType::webauthn);
    retain_auth_retry(host.login_status);
    secure_buffer_release(&credential);
    if (!host.login_status.ok() || host.login_status.phase != mfa_pending_phase()) {
        log_u64(LogLevel::error, "auth", "webauthn_submit_error",
                static_cast<u64>(host.login_status.error));
        if (!g_app.login.reset_mode)
            return webauthn_failed();
        g_app.login.webauthn_attempted = false;
        return present(LoginStatus::reset_failed, LoginControl::back);
    }
    log_u64(LogLevel::info, "auth", "webauthn_api_version", asserted.api_version);
    return present_mfa_pending(LoginControl::none);
}

bool application_login_submit_mfa() noexcept {
    if (application_account_logout_locked())
        return false;
    if (application_login_auth_cooldown_active())
        return true;
    AccountHost& host = g_app.account;
    if (!host.login_instance)
        return present_focus(LoginControl::password);
    if (g_app.login.mfa_type == UserAuthMfaType::webauthn) {
        return begin_webauthn();
    }
    if (g_app.login.mfa_type == UserAuthMfaType::sms && g_app.login.sms_sending) {
        return true;
    }
    const LoginMfaCodeResult validated =
        login_mfa_validate_code(g_app.login.mfa_type, secure_buffer_view(g_app.login.mfa.storage));
    if (!validated.ok())
        return present(LoginStatus::mfa_input_invalid, LoginControl::password);
    host.login_status =
        host.api.login_submit_mfa(host.login_instance, &g_app.login.mfa.storage, g_app.login.mfa_type);
    retain_auth_retry(host.login_status);
    if (!host.login_status.ok() || host.login_status.phase != mfa_pending_phase())
        return present(g_app.login.reset_mode ? LoginStatus::reset_failed : LoginStatus::rejected);
    secure_edit_clear(&g_app.login.mfa);
    return present_mfa_pending(LoginControl::none);
}

namespace {

bool submit_password() noexcept {
    if (application_account_logout_locked())
        return false;
    if (!g_app.login.visible || g_app.login.status == RendererLoginStatus::restoring_session ||
        g_app.login.status == RendererLoginStatus::submitting) {
        return true;
    }
    if (g_app.login.status == RendererLoginStatus::mfa_backup_acknowledgement) {
        const u64 now_ms = clock_monotonic_milliseconds();
        const u32 seconds =
            login_mfa_backup_acknowledgement_seconds(now_ms, g_app.login.backup_acknowledgement_at_ms);
        if (seconds != 0) {
            g_app.login.backup_acknowledgement_seconds = static_cast<u8>(seconds);
            return application_refresh_login_view();
        }
        application_login_reset_backup_acknowledgement();
        g_app.login.backup_acknowledged = true;
        return present(LoginStatus::mfa_required, LoginControl::password);
    }
    if (g_app.login.status == RendererLoginStatus::profile_save_failed)
        return present(LoginStatus::submitting, LoginControl::none) && application_login_pump();
    if (login_requires_back(g_app.login.status, g_app.login.remote_status)) {
        return application_refresh_login_view();
    }
    if (application_login_auth_cooldown_active())
        return true;
    if (g_app.login.status == RendererLoginStatus::mfa_required ||
        g_app.login.status == RendererLoginStatus::mfa_input_invalid ||
        g_app.login.status == RendererLoginStatus::mfa_code_rejected ||
        g_app.login.status == RendererLoginStatus::webauthn_required ||
        g_app.login.status == RendererLoginStatus::webauthn_failed) {
        return application_login_submit_mfa();
    }
    if (g_app.login.reset_mode) return application_login_submit_reset();
    if (g_app.login.login.storage.bytes == 0)
        return present_focus(LoginControl::login);
    if (login_reset_link_candidate({reinterpret_cast<const char*>(g_app.login.login.storage.data),
                                   g_app.login.login.storage.bytes})) return application_login_begin_reset();
    const bool verification = g_app.login.login.password || login_verification_link_candidate(
        {reinterpret_cast<const char*>(g_app.login.login.storage.data), g_app.login.login.storage.bytes});
    VerificationInput verification_input{};
    if (verification) {
        const bool allocated = secure_buffer_initialize(
            &verification_input.token, kLoginVerificationMaximumTokenBytes).ok();
        const LoginVerificationLinkResult consumed = allocated
            ? login_verification_consume_link(
                {reinterpret_cast<const char*>(g_app.login.login.storage.data), g_app.login.login.storage.bytes},
                &verification_input.token)
            : LoginVerificationLinkResult{0, LoginVerificationLinkError::invalid_input};
        // The URI never survives consumption, even when it is malformed.
        secure_edit_clear(&g_app.login.login);
        secure_edit_clear(&g_app.login.password);
        if (!consumed.ok())
            return present(LoginStatus::rejected, LoginControl::login);
    }
    if (!verification && g_app.login.password.storage.bytes == 0)
        return present_focus(LoginControl::password);
    if (g_app.login.remote_active) {
        AccountHost& host = g_app.account;
        if (!host.login_instance) {
            log_write(LogLevel::error, "auth", "remote_auth_instance_missing");
        } else {
            const AccountLoginStatus cancelled = host.api.login_cancel_remote_auth(host.login_instance);
            if (!cancelled.ok()) {
                log_u64(LogLevel::warning, "auth", "remote_auth_cancel_error",
                        static_cast<u64>(cancelled.error));
            }
            const AccountLoginStatus stopped = stop_login_owner();
            if (host.login_instance || !stopped.ok()) {
                log_u64(LogLevel::error, "auth", "remote_auth_password_handoff_error",
                        static_cast<u64>(stopped.error));
                g_app.login.remote_status = LoginRemoteStatus::failed;
                return present(LoginStatus::rejected);
            }
        }
        application_login_reset_remote_session();
    }
    if (application_login_auth_cooldown_active())
        return application_refresh_login_view();
    AccountPaths paths{};
    AccountPathsError path_error{};
    if (!application_login_prepare_owner(&paths, &path_error)) {
        log_u64(LogLevel::error, "auth", "login_storage_error", static_cast<u64>(path_error));
        return present(LoginStatus::rejected);
    }
    AccountHost& host = g_app.account;
    if (!host.login_instance) {
        invalidate_auth();
        host.login_status = account_host_login_start(&host, paths.profile);
        if (!host.login_status.ok() || !host.login_instance) {
            log_u64(LogLevel::error, "auth", "login_start_error",
                    static_cast<u64>(host.login_status.error));
            application_account_module_release();
            return present(LoginStatus::rejected);
        }
    }
    host.login_status = verification
        ? account_host_login_submit_verification(&host, &verification_input.token)
        : host.api.login_submit_password(host.login_instance, &g_app.login.login.storage,
                                        &g_app.login.password.storage);
    retain_auth_retry(host.login_status);
    secure_buffer_release(&verification_input.token);
    const AccountLoginPhase expected_phase = verification ? AccountLoginPhase::verification_pending
                                                          : AccountLoginPhase::password_pending;
    if (!host.login_status.ok() || host.login_status.phase != expected_phase) {
        log_u64(LogLevel::error, "auth", verification ? "verification_submit_error" : "password_submit_error",
                static_cast<u64>(host.login_status.error));
        return present(LoginStatus::rejected);
    }
    secure_edit_clear(&g_app.login.password);
    application_login_reset_attempt();
    g_app.login.mfa_capabilities = 0;
    log_u64(LogLevel::info, "auth", verification ? "verification_request_id" : "password_request_id",
            host.login_status.request_id);
    return present(LoginStatus::submitting, LoginControl::none);
}

} // namespace

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

namespace {
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
} // namespace

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
    SecureTextComposition composition{};
    if (secure_text_store_composition(login.text_store, &composition) && composition.active) return true;
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
    SecureTextComposition composition{};
    if (secure_text_store_composition(login.text_store, &composition) && composition.active) return true;
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

bool application_login_pump() noexcept {
    if (application_account_logout_locked())
        return true;
    AccountHost& host = g_app.account;
    // A failed stop still owns its worker. Do not accept queued authentication
    // results after the user cancelled; Back retries stop, shutdown keeps it.
    if (g_app.login.status == RendererLoginStatus::cancelling) return true;
    if (!host.login_instance) {
        return true;
    }
    host.login_status = account_host_login_pump(&host);
    if (application_account_logout_locked())
        return true;
    if (host.login_status.event != UserAuthCoordinatorEventKind::none ||
        host.login_status.remote_event != RemoteAuthClientEventKind::none)
        retain_auth_retry(host.login_status);
    bool browser_handled = false;
    if (!application_browser_auth_pump(&browser_handled)) return false;
    if (application_account_logout_locked())
        return true;
    if (browser_handled)
        return true;
    if (!application_captcha_auth_pump()) return false;
    if (application_account_logout_locked() || !host.login_instance)
        return true;
    if (host.login_status.event == UserAuthCoordinatorEventKind::none &&
        host.login_status.remote_event == RemoteAuthClientEventKind::none && host.login_status.ok() &&
        !(host.login_status.phase == AccountLoginPhase::authenticated && host.login_status.profile_saved) &&
        !(login_phone_mode(g_app.login.status) && host.login_status.phase == AccountLoginPhase::phone_token_ready)) {
        return true;
    }
    if (login_phone_mode(g_app.login.status)) {
        auto& login = g_app.login;
        if (host.login_status.phase == AccountLoginPhase::phone_token_ready) {
            VerificationInput input{};
            const bool allocated = secure_buffer_initialize(&input.token, kLoginResetMaximumTokenBytes).ok();
            const u64 generation = host.login_status.generation;
            const auto taken = allocated ? account_host_login_take_phone_reset_token(&host, generation, &input.token) :
                SecureBufferResult{0, 0, SecureBufferError::allocation_failed};
            login.status = LoginStatus::recovery_phone_token_ready; login.focus = LoginControl::back;
            secure_text_store_unbind(&login.text_store);
            secure_edit_clear(&login.login);
            if (!quiesce_recovery_owner()) return application_refresh_login_view();
            application_account_module_release();
            if (host.login_instance || host.module || host.stop_failed)
                return present(LoginStatus::cancelling, LoginControl::back);
            if (!taken.ok() || !adopt_reset_token(&input.token))
                return present(LoginStatus::recovery_phone_failed, LoginControl::back);
            return application_refresh_login_view();
        }
        if (host.login_status.event == UserAuthCoordinatorEventKind::phone_code_sent &&
            host.login_status.phase == AccountLoginPhase::recovery_phone_required) {
            login.sms_sending = false;
            login.sms_requested = true;
            return present(LoginStatus::recovery_phone_resent, LoginControl::phone_code);
        }
        if (host.login_status.phase == AccountLoginPhase::phone_rejected) {
            const bool resend = login.sms_sending;
            if (!resend) secure_edit_clear(&login.mfa);
            login.sms_sending = false;
            return present(resend ? LoginStatus::recovery_phone_resend_failed
                                  : LoginStatus::recovery_phone_code_rejected,
                           LoginControl::phone_code);
        }
        if (host.login_status.phase == AccountLoginPhase::phone_rate_limited)
            return present(login.sms_sending ? LoginStatus::recovery_phone_resend_rate_limited
                                             : LoginStatus::recovery_phone_verify_rate_limited,
                           LoginControl::back);
        if (host.login_status.phase == AccountLoginPhase::captcha_required)
            return present(LoginStatus::recovery_phone_captcha_required, LoginControl::submit);
        if (host.login_status.phase == AccountLoginPhase::failed || !host.login_status.ok() ||
            host.login_status.phase == AccountLoginPhase::authenticated) {
            // Phone tokens never enter the session/profile/Gateway path.
            secure_edit_clear(&login.mfa);
            return present(LoginStatus::recovery_phone_failed, LoginControl::back);
        }
        return true;
    }
    if (g_app.login.reset_mode) {
        auto& login = g_app.login;
        if (host.login_status.phase == AccountLoginPhase::reset_rejected) {
            const bool retry = host.login_status.http_status == 400 && login.mfa_capabilities == 0;
            login.reset_retry = retry;
            return retry ? present(LoginStatus::reset_rejected, LoginControl::password) : reset_failed();
        }
        if (host.login_status.phase == AccountLoginPhase::reset_rate_limited)
            return present(LoginStatus::reset_rate_limited, LoginControl::back);
        if (host.login_status.phase == AccountLoginPhase::captcha_required)
            return present(LoginStatus::reset_captcha_required, LoginControl::submit);
        if (host.login_status.phase == AccountLoginPhase::mfa_required &&
            host.login_status.event == UserAuthCoordinatorEventKind::rejected &&
            !login.sms_sending &&
            login_mfa_invalid_error(host.login_status.error_code)) {
            secure_edit_clear(&login.mfa);
            login.webauthn_attempted = false;
            return present_mfa_retry(LoginStatus::mfa_code_rejected);
        }
        if (host.login_status.phase == AccountLoginPhase::failed || host.login_status.phase == AccountLoginPhase::rejected ||
            (!host.login_status.ok() && !(host.login_status.phase == AccountLoginPhase::authenticated &&
              host.login_status.error == AccountLoginError::profile)))
            return reset_failed();
    }
    if (login_recovery_mode(g_app.login.status)) {
        if (host.login_status.event == UserAuthCoordinatorEventKind::none &&
            (host.login_status.ok() || g_app.login.status != LoginStatus::recovery_pending)) return true;
        if (host.login_status.phase == AccountLoginPhase::recovery_sent)
            return present(LoginStatus::recovery_sent, LoginControl::back);
        if (host.login_status.phase == AccountLoginPhase::recovery_phone_required) {
            secure_edit_clear(&g_app.login.mfa);
            g_app.login.sms_requested = g_app.login.sms_sending = g_app.login.sms_failed = false;
            return present(LoginStatus::recovery_phone_required, LoginControl::phone_code);
        }
        if (host.login_status.phase == AccountLoginPhase::captcha_required)
            return present(LoginStatus::recovery_captcha_required, LoginControl::submit);
        if (host.login_status.phase == AccountLoginPhase::recovery_rate_limited)
            return present(!host.login_status.retry_at_ms ||
                                   host.login_status.retry_at_ms > clock_monotonic_milliseconds()
                               ? LoginStatus::recovery_rate_limited
                               : LoginStatus::recovery_failed,
                           LoginControl::back);
        if (host.login_status.phase == AccountLoginPhase::recovery_rejected ||
            host.login_status.phase == AccountLoginPhase::failed || !host.login_status.ok())
            return present(LoginStatus::recovery_failed, LoginControl::login);
        // Never attach/promote a recovery owner, even an inconsistent module.
        if (host.login_status.phase == AccountLoginPhase::authenticated)
            return present(LoginStatus::recovery_failed, LoginControl::back);
        return true;
    }
    const auto trace = [](const char* key, u64 value) noexcept {
        log_u64(LogLevel::info, "auth", key, value);
    };
    trace("login_phase", static_cast<u64>(host.login_status.phase));
    trace("login_http_status", host.login_status.http_status);
    trace("login_error", static_cast<u64>(host.login_status.error));
    trace("login_coordinator_error", static_cast<u64>(host.login_status.coordinator_error));
    trace("login_coordinator_decode_error", static_cast<u64>(host.login_status.coordinator_decode_error));
    trace("login_coordinator_decode_error_offset", host.login_status.coordinator_decode_error_offset);
    trace("login_coordinator_scan_error", static_cast<u64>(host.login_status.coordinator_scan_error));
    trace("login_coordinator_read_error", static_cast<u64>(host.login_status.coordinator_read_error));
    trace("login_coordinator_transport_error",
          static_cast<u64>(host.login_status.coordinator_transport_error));
    trace("login_coordinator_transition_error",
          static_cast<u64>(host.login_status.coordinator_transition_error));
    trace("login_coordinator_response_bytes", host.login_status.coordinator_response_bytes);
    trace("login_coordinator_response_kind", static_cast<u64>(host.login_status.coordinator_response_kind));
    trace("login_coordinator_content_type", static_cast<u64>(host.login_status.coordinator_content_type));
    trace("login_coordinator_content_encoding",
          static_cast<u64>(host.login_status.coordinator_content_encoding));
    trace("login_coordinator_body_kind", static_cast<u64>(host.login_status.coordinator_body_kind));
    trace("login_native_error", host.login_status.native_error);
    trace("login_error_code", host.login_status.error_code);
    trace("login_remote_event", static_cast<u64>(host.login_status.remote_event));
    trace("login_remote_error", static_cast<u64>(host.login_status.remote_error));
    trace("login_remote_handshake_error", static_cast<u64>(host.login_status.remote_handshake_error));
    trace("login_remote_handshake_stage", static_cast<u64>(host.login_status.remote_handshake_stage));
    trace("login_remote_protocol_error", static_cast<u64>(host.login_status.remote_protocol_error));
    trace("login_remote_crypto_error", static_cast<u64>(host.login_status.remote_crypto_error));
    trace("login_remote_base64_error", static_cast<u64>(host.login_status.remote_base64_error));
    trace("login_remote_ticket_response_bytes", host.login_status.remote_ticket_response_bytes);
    trace("login_remote_ticket_response_kind",
          static_cast<u64>(host.login_status.remote_ticket_response_kind));
    trace("login_remote_ticket_decode_error",
          static_cast<u64>(host.login_status.remote_ticket_decode_error));
    trace("login_remote_ticket_decode_error_offset", host.login_status.remote_ticket_decode_error_offset);
    trace("login_remote_ticket_scan_error", static_cast<u64>(host.login_status.remote_ticket_scan_error));
    trace("login_remote_ticket_read_error", static_cast<u64>(host.login_status.remote_ticket_read_error));
    trace("login_remote_ticket_error_code", host.login_status.remote_ticket_error_code);
    trace("login_remote_ticket_content_type",
          static_cast<u64>(host.login_status.remote_ticket_content_type));
    trace("login_remote_ticket_content_encoding",
          static_cast<u64>(host.login_status.remote_ticket_content_encoding));
    trace("login_remote_ticket_body_kind", static_cast<u64>(host.login_status.remote_ticket_body_kind));
    if (host.login_status.phase == AccountLoginPhase::authenticated && host.login_status.profile_saved) {
        if (application_account_logout_locked())
            return true;
        const bool browser_session = host.login_status.browser_generation != 0;
        invalidate_auth();
        const AccountLoginStatus stopped = stop_login_owner();
        if (!stopped.ok() || host.login_instance)
            return present(g_app.login.reset_mode || browser_session ? LoginStatus::cancelling
                                                                     : LoginStatus::rejected,
                           LoginControl::back);
        secure_edit_clear(&g_app.login.password);
        secure_edit_clear(&g_app.login.login);
        secure_edit_clear(&g_app.login.mfa);
        g_app.login.browser_launch_epoch = 0;
        g_app.login.browser_return_to_qr = false;
        application_login_reset_attempt();
        application_login_reset_remote_session();
        g_app.login.status = RendererLoginStatus::idle;
        application_account_attach_if_present();
        // Startup-marker recovery may block this late authenticated result.
        // Preserve that owner's presentation instead of reporting bad login.
        if (application_account_logout_locked())
            return true;
        if (!host.instance) {
            g_app.login.visible = true;
            return g_app.login.reset_mode ? present(LoginStatus::reset_failed, LoginControl::back)
                                          : present(LoginStatus::rejected, LoginControl::login);
        }
        restore_identifier_bound();
        g_app.login.auth_retry_at_ms = 0;
        g_app.login.auth_retry_time_unknown = false;
        g_app.login.login.password = false;
        g_app.login.reset_mode = false; g_app.login.reset_retry = false;
        application_request_frame();
        return true;
    }
    if (host.login_status.remote_event == RemoteAuthClientEventKind::qr_ready) {
        char url[kRemoteAuthQrUrlBytes]{};
        u32 url_bytes = 0;
        const bool copied = host.api.login_copy_remote_auth_qr(
            host.login_instance, Slice<char>{url, static_cast<u32>(sizeof(url))}, &url_bytes);
        const bool encoded =
            copied && url_bytes == kRemoteAuthQrUrlBytes &&
            qr_code_encode_v5_m(ConstSlice<u8>{reinterpret_cast<const u8*>(url), url_bytes},
                                &g_app.login.qr_code);
        secure_clear_bytes(url, sizeof(url));
        if (!encoded) {
            host.api.login_cancel_remote_auth(host.login_instance);
            g_app.login.qr_ready = false;
            return present(LoginStatus::rejected);
        }
        secure_clear_bytes(g_app.login.remote_user, sizeof(g_app.login.remote_user));
        g_app.login.remote_user_bytes = 0;
        g_app.login.qr_ready = true;
        g_app.login.remote_active = true;
        g_app.login.remote_status = LoginRemoteStatus::qr_ready;
        g_app.login.remote_retry_attempt = 0;
        g_app.login.remote_restart_at_ms = 0;
        return application_refresh_login_view();
    }
    if (host.login_status.remote_event == RemoteAuthClientEventKind::mobile_pending) {
        char payload[kRemoteAuthMaximumUserPayloadBytes]{};
        u32 payload_bytes = 0;
        const bool copied = host.api.login_copy_remote_auth_user(
            host.login_instance, Slice<char>{payload, static_cast<u32>(sizeof(payload))}, &payload_bytes);
        RemoteAuthUserPreview preview{};
        const RemoteAuthUserPreviewResult parsed =
            copied ? remote_auth_parse_user_preview(Utf8View{payload, payload_bytes}, &preview)
                   : RemoteAuthUserPreviewResult{0, RemoteAuthUserPreviewError::invalid_input};
        const bool displayable = parsed.ok() && preview.username.bytes != 0 &&
                                 preview.username.bytes <= sizeof(g_app.login.remote_user);
        if (displayable) {
            secure_clear_bytes(g_app.login.remote_user, sizeof(g_app.login.remote_user));
            copy_bytes(g_app.login.remote_user, preview.username.data, preview.username.bytes);
            g_app.login.remote_user_bytes = preview.username.bytes;
        }
        secure_clear_bytes(payload, sizeof(payload));
        if (!displayable) {
            host.api.login_cancel_remote_auth(host.login_instance);
            stop_login_owner();
            application_account_module_release();
            g_app.login.remote_active = false;
            g_app.login.qr_ready = false;
            return present(LoginStatus::rejected, LoginControl::login);
        }
        g_app.login.remote_active = true;
        g_app.login.remote_status = LoginRemoteStatus::mobile_pending;
        g_app.login.remote_restart_at_ms = 0;
        return application_refresh_login_view();
    }
    if (host.login_status.remote_event == RemoteAuthClientEventKind::failed ||
        host.login_status.remote_event == RemoteAuthClientEventKind::cancelled) {
        const bool expired =
            host.login_status.remote_error == RemoteAuthClientError::handshake &&
            host.login_status.remote_handshake_error == RemoteAuthHandshakeError::session_timeout;
        // The phone declined the pending login: not a network fault. Present
        // it as such and regenerate the QR code without backoff.
        const bool declined = host.login_status.remote_event == RemoteAuthClientEventKind::cancelled && !expired;
        if (declined) g_app.login.remote_retry_attempt = 0;
        if (g_app.login.remote_status == LoginRemoteStatus::captcha_required ||
            g_app.login.status == RendererLoginStatus::captcha_required) {
            application_captcha_auth_reset();
            if (g_app.login.status == RendererLoginStatus::captcha_required) {
                g_app.login.status = RendererLoginStatus::idle;
                g_app.login.focus = LoginControl::login;
            }
        }
        g_app.login.remote_active = false;
        secure_clear_bytes(g_app.login.remote_user, sizeof(g_app.login.remote_user));
        g_app.login.remote_user_bytes = 0;
        if (host.login_instance) {
            stop_login_owner();
        }
        application_account_module_release();
        application_login_schedule_remote_restart(expired ? LoginRemoteStatus::refreshing
                                                  : declined ? LoginRemoteStatus::cancelled
                                                             : LoginRemoteStatus::failed);
        return application_refresh_login_view();
    }
    if (host.login_status.http_status == 429 &&
        host.login_status.phase == AccountLoginPhase::mfa_required) {
        secure_edit_clear(&g_app.login.mfa);
        g_app.login.sms_sending = false;
        g_app.login.webauthn_attempted = false;
        return present_mfa_retry(LoginStatus::mfa_required);
    }
    if (host.login_status.event == UserAuthCoordinatorEventKind::sms_code_sent) {
        g_app.login.sms_sending = false;
        g_app.login.sms_requested = true;
        g_app.login.sms_failed = false;
    } else if (g_app.login.sms_sending &&
               host.login_status.event == UserAuthCoordinatorEventKind::rejected &&
               host.login_status.phase == AccountLoginPhase::mfa_required) {
        g_app.login.sms_sending = false;
        g_app.login.sms_requested = false;
        g_app.login.sms_failed = true;
        return present(LoginStatus::mfa_required, LoginControl::mfa_sms);
    }
    if (host.login_status.phase == AccountLoginPhase::mfa_required &&
        host.login_status.event == UserAuthCoordinatorEventKind::rejected && !g_app.login.sms_sending &&
        login_mfa_invalid_error(host.login_status.error_code)) {
        secure_edit_clear(&g_app.login.mfa);
        g_app.login.webauthn_attempted = false;
        return present_mfa_retry(LoginStatus::mfa_code_rejected);
    }
    if (host.login_status.phase == AccountLoginPhase::mfa_required) {
        const bool needs_initial_selection = g_app.login.mfa_capabilities == 0;
        if (host.login_status.mfa_capabilities != 0) {
            g_app.login.mfa_capabilities = host.login_status.mfa_capabilities;
        }
        if (g_app.login.mfa_capabilities == 0)
            return present(LoginStatus::rejected);
        if ((needs_initial_selection ||
             (g_app.login.mfa_capabilities & login_mfa_capability(g_app.login.mfa_type)) == 0) &&
            !login_mfa_first_supported(g_app.login.mfa_capabilities, &g_app.login.mfa_type))
            return present(LoginStatus::rejected);
        return application_login_select_mfa(g_app.login.mfa_type, false);
    }
    if (host.login_status.phase == AccountLoginPhase::captcha_required) {
        // Enter verifies in the browser; Back and Escape still leave.
        g_app.login.status = RendererLoginStatus::captcha_required;
        g_app.login.focus = LoginControl::submit;
        if (g_app.login.remote_active)
            g_app.login.remote_status = LoginRemoteStatus::captcha_required;
    } else if (host.login_status.error == AccountLoginError::profile &&
               host.login_status.phase == AccountLoginPhase::authenticated &&
               !host.login_status.profile_saved) {
        g_app.login.status = RendererLoginStatus::profile_save_failed;
        g_app.login.focus = LoginControl::submit;
    } else if (host.login_status.phase == AccountLoginPhase::rejected ||
               host.login_status.phase == AccountLoginPhase::failed || !host.login_status.ok()) {
        g_app.login.status = RendererLoginStatus::rejected;
        g_app.login.focus = g_app.login.login.storage.bytes == 0 ? LoginControl::login : LoginControl::password;
        if (host.login_instance) {
            stop_login_owner();
        }
        if (g_app.login.visible) {
            application_login_schedule_remote_restart(LoginRemoteStatus::refreshing);
        }
        reset_mfa_progress();
        g_app.login.mfa_capabilities = 0;
        secure_edit_clear(&g_app.login.mfa);
    }
    return application_refresh_login_view();
}

bool application_login_begin_remote_auth() noexcept {
    if (application_account_logout_locked())
        return false;
    if (application_login_auth_cooldown_active()) {
        g_app.login.remote_restart_at_ms = g_app.login.auth_retry_time_unknown ? 0 : application_login_auth_deadline();
        return true;
    }
    if (login_browser_mode(g_app.login.status))
        return true;
    if (g_app.login.reset_mode || login_phone_mode(g_app.login.status) || login_recovery_mode(g_app.login.status)) return true;
    if (!g_app.login.visible) {
        return false;
    }
    if (g_app.login.status == RendererLoginStatus::restoring_session) return true;
    if (g_app.login.remote_active) {
        return true;
    }
    if (g_app.login.status == RendererLoginStatus::submitting ||
        g_app.login.status == RendererLoginStatus::mfa_required ||
        g_app.login.status == RendererLoginStatus::mfa_backup_acknowledgement ||
        g_app.login.status == RendererLoginStatus::mfa_input_invalid ||
        login_requires_back(g_app.login.status, g_app.login.remote_status) ||
        g_app.login.status == RendererLoginStatus::profile_save_failed ||
        g_app.login.status == RendererLoginStatus::webauthn_required) {
        return true;
    }
    g_app.login.remote_restart_at_ms = 0;
    AccountPaths paths{};
    AccountPathsError path_error{};
    if (!application_login_prepare_owner(&paths, &path_error)) {
        log_u64(LogLevel::error, "auth", "remote_auth_storage_error", static_cast<u64>(path_error));
        application_login_schedule_remote_restart(LoginRemoteStatus::failed);
        return application_refresh_login_view();
    }
    AccountHost& host = g_app.account;
    if (!host.login_instance) {
        invalidate_auth();
        host.login_status = account_host_login_start(&host, paths.profile);
        if (!host.login_status.ok() || !host.login_instance) {
            log_u64(LogLevel::error, "auth", "remote_auth_login_start_error",
                    static_cast<u64>(host.login_status.error));
            application_login_schedule_remote_restart(LoginRemoteStatus::failed);
            application_account_module_release();
            return application_refresh_login_view();
        }
    }
    host.login_status = host.api.login_start_remote_auth(host.login_instance);
    retain_auth_retry(host.login_status);
    if (!host.login_status.ok() || (host.login_status.phase != AccountLoginPhase::remote_starting &&
                                    host.login_status.phase != AccountLoginPhase::remote_qr_ready &&
                                    host.login_status.phase != AccountLoginPhase::remote_mobile_pending)) {
        log_u64(LogLevel::error, "auth", "remote_auth_start_error",
                static_cast<u64>(host.login_status.error));
        application_login_schedule_remote_restart(LoginRemoteStatus::failed);
        stop_login_owner();
        application_account_module_release();
        return application_refresh_login_view();
    }
    secure_clear_bytes(g_app.login.remote_user, sizeof(g_app.login.remote_user));
    g_app.login.remote_user_bytes = 0;
    g_app.login.remote_active = true;
    // Keep the phone's decline visible until the replacement QR code arrives.
    g_app.login.remote_status = g_app.login.remote_status == LoginRemoteStatus::cancelled ? LoginRemoteStatus::cancelled
                                : g_app.login.qr_ready ? LoginRemoteStatus::refreshing
                                                       : LoginRemoteStatus::connecting;
    return application_refresh_login_view();
}

bool application_login_cancel_blocked() noexcept {
    if (application_account_logout_locked())
        return true;
    auto& login = g_app.login;
    if (!login.visible || !back_available())
        return true;
    invalidate_auth();
    login.browser_launch_epoch = 0;
    // Leaving a password-path challenge keeps the typed identifier; the
    // login owner (and with it the CAPTCHA bridge) is still stopped below.
    const bool keep_identifier = !login.reset_mode && !login.login.password &&
                                 !login_recovery_mode(login.status) && !login_phone_mode(login.status) &&
                                 login_captcha_mode(login.status, login.remote_status);
    application_login_reset_attempt();
    secure_text_store_unbind(&login.text_store);
    application_login_end_pointer_selection();
    if (!keep_identifier) secure_edit_clear(&login.login);
    secure_edit_clear(&login.password);
    secure_edit_clear(&login.mfa);
    application_login_reset_remote_session();
    login.mfa_capabilities = 0;
    login.status = RendererLoginStatus::cancelling;
    login.focus = LoginControl::back;
    if (g_app.account.login_instance) {
        const auto stopped = stop_login_owner();
        if (g_app.account.login_instance) {
            log_u64(LogLevel::warning, "auth", "login_cancel_stop_error", static_cast<u64>(stopped.error));
            return application_refresh_login_view();
        }
    }
    // Successful stop destroys the coordinator/remote owner and its protected
    // challenge. A later login gets a new owner, never the cancelled event queue.
    application_account_module_release();
    if (g_app.account.module) {
        log_write(LogLevel::warning, "auth", "login_cancel_release_pending");
        return application_refresh_login_view();
    }
    login.status = RendererLoginStatus::idle;
    restore_identifier_bound();
    login.reset_mode = false; login.reset_retry = false; login.login.password = false;
    login.focus = keep_identifier && login.login.storage.bytes ? LoginControl::password : LoginControl::login;
    if (login.browser_return_to_qr) {
        login.browser_return_to_qr = false;
        application_login_schedule_remote_restart(LoginRemoteStatus::refreshing);
    }
    return application_refresh_login_view();
}

namespace {
bool activate_login_control(LoginControl control) noexcept {
    if (application_account_logout_locked())
        return true;
    if (control == LoginControl::submit && g_app.login.auth_retry_time_unknown) {
        LoginView view{};
        view.status = g_app.login.status;
        view.captcha_status = g_app.login.captcha_status;
        view.auth_rate_limited = view.auth_retry_time_unknown = true;
        if (login_view_auth_retry_resume(view)) {
            // Retire the paused request before clearing its unknown receipt.
            // A failed join keeps both the owner and admission restrictions.
            if (!application_login_cancel_blocked()) return false;
            if (g_app.account.login_instance || g_app.account.module || g_app.account.stop_failed) return true;
            g_app.login.auth_retry_time_unknown = false;
            g_app.login.remote_restart_at_ms = 0;
            return application_refresh_login_view();
        }
    }
    switch (login_control_action(control, g_app.login.status, g_app.login.remote_status,
                                 g_app.login.reset_mode)) {
    case LoginAction::recovery: return application_login_begin_recovery();
    case LoginAction::recovery_submit: return application_login_submit_recovery();
    case LoginAction::phone_verify: return application_login_submit_phone_verify();
    case LoginAction::phone_resend: return application_login_submit_phone_resend();
    case LoginAction::back: return application_login_cancel_blocked();
    case LoginAction::submit: return submit_password();
    case LoginAction::remote_auth: return application_login_begin_remote_auth();
    case LoginAction::browser_sign_in: return application_login_begin_browser_auth();
    case LoginAction::captcha_verify: return application_login_begin_captcha_bridge();
    case LoginAction::mfa: {
        UserAuthMfaType type{};
        return login_control_mfa_type(control, &type) && application_login_select_mfa(type, true);
    }
    default: return true;
    }
}
} // namespace

bool application_login_handle_click(LPARAM position) noexcept {
    if (application_account_logout_locked())
        return true;
    if (!g_app.login.visible) {
        return false;
    }
    if (g_app.login.status == RendererLoginStatus::restoring_session) return true;
    const i32 x = static_cast<short>(LOWORD(position));
    const i32 y = static_cast<short>(HIWORD(position));
    const float scale = 96.0f / static_cast<float>(g_app.renderer.dpi);
    if (login_password_reveal_hit_test(g_app.login_scene, static_cast<float>(x) * scale,
                                       static_cast<float>(y) * scale)) {
        g_app.login.password_revealed = !g_app.login.password_revealed;
        g_app.login.focus = LoginControl::password;
        if (!application_refresh_login_view())
            log_write(LogLevel::error, "ui", "login_reveal_refresh_failed");
        return true;
    }
    const LoginControl hit = login_scene_hit_test(g_app.login_scene, static_cast<float>(x) * scale,
                                                  static_cast<float>(y) * scale);
    if (hit == LoginControl::login || hit == LoginControl::password || hit == LoginControl::phone_code) {
        const bool extend_selection = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool handled = application_login_position_edit(hit, x, y, extend_selection);
        if (handled && application_active_login_edit()) {
            auto& widgets = g_app.login_scene.widgets;
            if (ui_widget_manager_capture(&widgets, widgets.focused)) {
                SetCapture(g_app.window);
                if (GetCapture() == g_app.window) g_app.login.pointer_selection = hit;
                else ui_widget_manager_capture(&widgets, ui_widget_invalid_handle());
            }
        }
        if (!handled) {
            log_write(LogLevel::error, "ui", "login_pointer_position_failed");
        }
    } else if (!activate_login_control(hit)) {
        log_write(LogLevel::error, "ui", "login_activation_failed");
    }
    return true;
}

bool application_login_handle_key(WPARAM key, bool repeated) noexcept {
    if (application_account_logout_locked())
        return true;
    if (!g_app.login.visible) {
        return false;
    }
    if (repeated && (key == VK_RETURN || key == VK_SPACE))
        return true;
    if (g_app.login.status == RendererLoginStatus::restoring_session) return true;
    if (key == VK_CAPITAL) {
        // The Caps Lock hint follows the toggle state while typing a password.
        if (!application_refresh_login_view()) log_write(LogLevel::error, "ui", "login_caps_refresh_failed");
        return false;
    }
    if (key == VK_ESCAPE && back_available())
        return application_login_cancel_blocked();
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;
    if (key == VK_TAB) {
        const LoginControl next = login_scene_move_focus(&g_app.login_scene, shift);
        if (next == LoginControl::none) {
            return false;
        }
        g_app.login.focus = next;
        return application_refresh_login_view();
    }
    if (key == VK_RETURN || key == VK_SPACE) {
        if (key == VK_SPACE)
            return g_app.login.focus == LoginControl::login || g_app.login.focus == LoginControl::password || g_app.login.focus == LoginControl::phone_code
                ? false : activate_login_control(g_app.login.focus);
        if (login_requires_back(g_app.login.status, g_app.login.remote_status))
            return activate_login_control(g_app.login.focus);
        if (g_app.login.focus == LoginControl::login) {
            if (login_recovery_mode(g_app.login.status)) return activate_login_control(LoginControl::login);
            if (g_app.login.reset_mode) return true;
            if (g_app.login.login.password || login_verification_link_candidate(
                    {reinterpret_cast<const char*>(g_app.login.login.storage.data), g_app.login.login.storage.bytes}) ||
                login_reset_link_candidate({reinterpret_cast<const char*>(g_app.login.login.storage.data), g_app.login.login.storage.bytes}))
                return submit_password();
            g_app.login.focus = LoginControl::password;
            return application_refresh_login_view();
        }
        return activate_login_control(g_app.login.focus);
    }
    SecureEdit* edit = application_active_login_edit();
    if (!edit) {
        return key == VK_ESCAPE;
    }
    if (control && key == 'V') {
        return application_login_paste();
    }
    const bool phone = login_phone_mode(g_app.login.status);
    const auto move = [&](SecureEditMove movement) noexcept {
        return phone ? application_phone_move(edit, movement, shift) : secure_edit_move(edit, movement, shift);
    };
    SecureEditResult result = SecureEditResult::unchanged;
    if (control && key == 'A') {
        result = phone ? application_phone_select_all(edit) : secure_edit_select_all(edit);
    } else {
        switch (key) {
        case VK_BACK:
            result = phone ? application_phone_backspace(edit) : secure_edit_backspace(edit);
            break;
        case VK_DELETE:
            result = phone ? application_phone_delete_forward(edit) : secure_edit_delete_forward(edit);
            break;
        case VK_LEFT:
            result = move(SecureEditMove::previous);
            break;
        case VK_RIGHT:
            result = move(SecureEditMove::next);
            break;
        case VK_HOME:
            result = move(SecureEditMove::home);
            break;
        case VK_END:
            result = move(SecureEditMove::end);
            break;
        default:
            return false;
        }
    }
    if (result == SecureEditResult::changed && edit == &g_app.login.mfa)
        application_login_code_edited(&g_app.login.status);
    return result != SecureEditResult::invalid_input && application_refresh_login_view();
}

} // namespace maicord::app_detail
