#pragma once

#include "account_login_handoff.hpp"
#include "maicord/app/captcha_bridge.hpp"
#include "maicord/core/win32_kernel.hpp"

// CAPTCHA continuation for the TU-local login owner: secret copy, replay and
// the loopback browser bridge. Shared by the account module and native tests.
namespace maicord {
namespace {

UserAuthCoordinatorSecretKind coordinator_captcha_kind(CaptchaChallengeSecretKind kind) noexcept {
    switch (kind) {
    case CaptchaChallengeSecretKind::service: return UserAuthCoordinatorSecretKind::captcha_service;
    case CaptchaChallengeSecretKind::sitekey: return UserAuthCoordinatorSecretKind::captcha_sitekey;
    case CaptchaChallengeSecretKind::rqdata: return UserAuthCoordinatorSecretKind::captcha_rqdata;
    case CaptchaChallengeSecretKind::rqtoken: return UserAuthCoordinatorSecretKind::captcha_rqtoken;
    case CaptchaChallengeSecretKind::session_id: return UserAuthCoordinatorSecretKind::captcha_session_id;
    case CaptchaChallengeSecretKind::user_flow: return UserAuthCoordinatorSecretKind::captcha_user_flow;
    default: return static_cast<UserAuthCoordinatorSecretKind>(0xFF);
    }
}

SecureBufferResult WINAPI module_login_copy_captcha_secret(void* instance, CaptchaChallengeSecretKind kind,
                                                           SecureBuffer* output) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !output || login_status(runtime).phase != AccountLoginPhase::captcha_required)
        return SecureBufferResult{0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
    if (runtime->remote_mode)
        return remote_auth_client_copy_captcha_secret(&runtime->remote_auth, kind, output);
    const UserAuthCoordinatorSecretKind mapped = coordinator_captcha_kind(kind);
    return mapped != static_cast<UserAuthCoordinatorSecretKind>(0xFF) && runtime->coordinator_started
               ? user_auth_coordinator_copy_secret(&runtime->coordinator, mapped, output)
               : SecureBufferResult{0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
}

// Replays the retained password, MFA, SMS, recovery, reset, phone or QR
// ticket request with the solution; the solution is copied synchronously.
AccountLoginStatus WINAPI module_login_submit_captcha(void* instance, const SecureBuffer* solution) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !solution || login_status(runtime).phase != AccountLoginPhase::captcha_required)
        return login_invalid_input();
    if (runtime->remote_mode) {
        runtime->remote_status = remote_auth_client_submit_captcha(&runtime->remote_auth, solution);
        if (!runtime->remote_status.ok())
            runtime->error = AccountLoginError::remote_auth;
    } else {
        const UserAuthCoordinatorResult submitted = user_auth_coordinator_submit_captcha(&runtime->coordinator, solution);
        if (!submitted.ok()) {
            runtime->error = AccountLoginError::coordinator;
        } else {
            runtime->error = AccountLoginError::none;
            runtime->last_event = UserAuthCoordinatorEventKind::none;
            runtime->error_code = 0;
        }
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_start_captcha_bridge(void* instance, u64 generation, u32 timeout_ms) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !generation || runtime->captcha_bridge.runtime || runtime->browser_mode ||
        runtime->error == AccountLoginError::stop || timeout_ms < 1000 || timeout_ms > kCaptchaBridgeDefaultTimeoutMs) {
        return login_invalid_input();
    }
    const AccountLoginStatus current = login_status(runtime);
    if (current.phase != AccountLoginPhase::captcha_required) {
        AccountLoginStatus status = current;
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    runtime->captcha_status = captcha_bridge_start_copied(
        &runtime->captcha_bridge, generation, timeout_ms, current.captcha_provider, current.captcha_invisible,
        [runtime](CaptchaChallengeSecretKind kind, SecureBuffer* output) noexcept {
            return module_login_copy_captcha_secret(runtime, kind, output);
        },
        ERROR_INVALID_DATA);
    if (!runtime->captcha_status.ok()) {
        runtime->captcha_status.generation = generation;
        runtime->captcha_status.phase = CaptchaBridgePhase::failed;
    }
    return login_status(runtime);
}

CaptchaBridgeStatus WINAPI module_login_captcha_bridge_status(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime) return {};
    if (runtime->captcha_bridge.runtime) runtime->captcha_status = captcha_bridge_status(&runtime->captcha_bridge);
    return runtime->captcha_status;
}

SecureBufferResult WINAPI module_login_copy_captcha_bridge_url(void* instance, u64 generation,
                                                               SecureBuffer* output) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || runtime->error == AccountLoginError::stop)
        return {0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
    return captcha_bridge_copy_url(&runtime->captcha_bridge, generation, output);
}

// Stops only the bridge; the CAPTCHA challenge stays available for a retry.
AccountLoginStatus WINAPI module_login_cancel_captcha_bridge(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime)
        return login_invalid_input();
    if (runtime->captcha_bridge.runtime) runtime->captcha_status = captcha_bridge_stop(&runtime->captcha_bridge, 5'000);
    AccountLoginStatus status = login_status(runtime);
    if (runtime->captcha_bridge.runtime) status.error = AccountLoginError::stop;
    return status;
}

// Takes a solved token, joins the listener and replays the pending request.
// Terminal bridges are joined so a retry can start a fresh single-use state.
void pump_login_captcha_bridge(AccountLoginRuntime* runtime) noexcept {
    if (!runtime->captcha_bridge.runtime || runtime->error == AccountLoginError::stop) return;
    const CaptchaBridgeStatus bridge = captcha_bridge_status(&runtime->captcha_bridge);
    runtime->captcha_status = bridge;
    if (bridge.phase == CaptchaBridgePhase::token_ready) {
        SecureBuffer token{};
        const bool taken = secure_buffer_initialize(&token, kUserAuthMaximumCaptchaKeyBytes).ok() &&
                           captcha_bridge_take_token(&runtime->captcha_bridge, bridge.generation, &token).ok();
        runtime->captcha_status = captcha_bridge_stop(&runtime->captcha_bridge, 5'000);
        const bool submitted = taken && login_status(runtime).phase == AccountLoginPhase::captcha_required &&
                               module_login_submit_captcha(runtime, &token).ok();
        secure_buffer_release(&token);
        if (!submitted) {
            runtime->captcha_status.phase = CaptchaBridgePhase::failed;
            runtime->captcha_status.error = CaptchaBridgeError::state;
        }
        return;
    }
    if (bridge.phase == CaptchaBridgePhase::expired || bridge.phase == CaptchaBridgePhase::failed ||
        bridge.phase == CaptchaBridgePhase::cancelled || bridge.phase == CaptchaBridgePhase::complete)
        runtime->captcha_status = captcha_bridge_stop(&runtime->captcha_bridge, 5'000);
}

AccountLoginStatus WINAPI module_login_pump_with_captcha(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (runtime) pump_login_captcha_bridge(runtime);
    return module_login_pump(instance);
}

HANDLE WINAPI module_login_notification_handle_with_captcha(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (runtime && runtime->captcha_bridge.runtime && runtime->error != AccountLoginError::stop &&
        runtime->error != AccountLoginError::profile) {
        const auto phase = captcha_bridge_status(&runtime->captcha_bridge).phase;
        if (phase != CaptchaBridgePhase::stopping)
            return captcha_bridge_notification_handle(&runtime->captcha_bridge);
    }
    return module_login_notification_handle(instance);
}

} // namespace
} // namespace maicord
