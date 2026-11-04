#pragma once

#include "maicord/core/virtual_memory.hpp"
#include "maicord/os/thread.hpp"
#include "account_login_handoff.hpp"

// Profile staging, the pump and the stop of the TU-local login owner (see account_login_handoff.hpp).
// Only translation units that drive the owner include this; those that read its status do not.
namespace maicord {
namespace {

bool load_remote_user_id(AccountLoginRuntime* runtime) noexcept {
    char payload[kRemoteAuthMaximumUserPayloadBytes]{};
    u32 payload_bytes = 0;
    const bool copied = remote_auth_client_copy_user(
        runtime->remote_auth, Slice<char>{payload, static_cast<u32>(sizeof(payload))}, &payload_bytes);
    RemoteAuthUserPreview preview{};
    const RemoteAuthUserPreviewResult parsed =
        copied ? remote_auth_parse_user_preview(Utf8View{payload, payload_bytes}, &preview)
               : RemoteAuthUserPreviewResult{0, RemoteAuthUserPreviewError::invalid_input};
    secure_clear_bytes(payload, sizeof(payload));
    if (!parsed.ok() || preview.user_id == 0) {
        runtime->error = AccountLoginError::remote_auth;
        runtime->remote_status.native_error = ERROR_INVALID_DATA;
        runtime->remote_status.handshake_error = RemoteAuthHandshakeError::invalid_user_payload;
        return false;
    }
    runtime->user_id = preview.user_id;
    return true;
}

bool profile_failure(AccountLoginRuntime* runtime, u32 native_error) noexcept {
    runtime->error = AccountLoginError::profile;
    runtime->profile_native_error = native_error;
    if (runtime->remote_mode)
        runtime->remote_status.native_error = native_error;
    return false;
}

bool commit_login_profile(AccountLoginRuntime* runtime) noexcept {
    const AccountProfilePromotionResult committed =
        account_profile_promotion_commit(&runtime->promotion, runtime->profile_path);
    if (!committed.ok())
        return profile_failure(runtime, committed.native_error);
    runtime->error = AccountLoginError::none;
    runtime->profile_native_error = 0;
    runtime->profile_saved = true;
    runtime->authenticated_pending = false;
    return true;
}

bool stage_remote_profile(AccountLoginRuntime* runtime) noexcept {
    if (runtime->user_id == 0 && !load_remote_user_id(runtime)) {
        return false;
    }
    AccountProfile profile{};
    const SecureBufferResult initialized =
        secure_buffer_initialize(&profile.credential, kAccountProfileMaximumCredentialBytes);
    if (!initialized.ok())
        return profile_failure(runtime, initialized.native_error);
    profile.metadata = AccountProfileMetadata{runtime->user_id};
    const SecureBufferResult taken =
        remote_auth_client_take_credential(&runtime->remote_auth, &profile.credential);
    if (!taken.ok()) {
        account_profile_release(&profile);
        return profile_failure(runtime, taken.native_error);
    }
    const AccountProfilePromotionResult staged =
        account_profile_promotion_stage(&runtime->promotion, &profile);
    account_profile_release(&profile);
    if (!staged.ok())
        return profile_failure(runtime, staged.native_error);
    runtime->authenticated_pending = false;
    return commit_login_profile(runtime);
}

bool stage_coordinator_profile(AccountLoginRuntime* runtime) noexcept {
    if (!runtime->user_id) {
        if (runtime->browser_mode) {
            runtime->error = AccountLoginError::profile;
            return false;
        }
        const auto random = secure_random_fill({reinterpret_cast<u8*>(&runtime->user_id), sizeof(runtime->user_id)});
        if (!random.ok() || !runtime->user_id) {
            runtime->error = AccountLoginError::profile;
            return false;
        }
    }
    AccountProfile profile{};
    const auto promoted = user_auth_coordinator_take_profile(&runtime->coordinator, runtime->user_id, &profile, 5'000);
    if (!promoted.ok())
        return profile_failure(runtime, promoted.native_error);
    const auto staged = account_profile_promotion_stage(&runtime->promotion, &profile);
    account_profile_release(&profile);
    if (!staged.ok())
        return profile_failure(runtime, staged.native_error);
    runtime->authenticated_pending = false;
    return commit_login_profile(runtime);
}

AccountLoginStatus pump_remote_auth(AccountLoginRuntime* runtime) noexcept {
    runtime->last_remote_event = RemoteAuthClientEventKind::none;
    RemoteAuthClientEvent event{};
    while (remote_auth_client_pop_event(&runtime->remote_auth, &event)) {
        runtime->last_remote_event = event.kind;
        if (event.kind == RemoteAuthClientEventKind::authenticated)
            runtime->authenticated_pending = true;
    }
    runtime->remote_status = remote_auth_client_status(&runtime->remote_auth);
    if (runtime->last_remote_event == RemoteAuthClientEventKind::mobile_pending &&
        !load_remote_user_id(runtime)) {
        remote_auth_client_cancel(&runtime->remote_auth);
        return login_status(runtime);
    }
    const bool profile_retry = account_profile_promotion_has_credential(runtime->promotion);
    if ((runtime->authenticated_pending || profile_retry) &&
        !runtime->profile_saved) {
        if (!profile_retry && !thread_wait(runtime->remote_auth.thread, 0)) {
            // The completion event is signaled just before the outer thread
            // exits. Keep the credential owner and wait on its thread
            // handle; this is readiness, not an authentication rejection.
            return login_status(runtime);
        }
        if (!(profile_retry ? commit_login_profile(runtime) : stage_remote_profile(runtime))) {
            return login_status(runtime);
        }
        const RemoteAuthClientResult stopped = remote_auth_client_stop(&runtime->remote_auth, 5'000);
        runtime->remote_active = false;
        if (!stopped.ok()) {
            runtime->error = AccountLoginError::stop;
            runtime->profile_native_error = stopped.native_error;
            runtime->remote_status = stopped;
            return login_status(runtime);
        }
        reset_value<RemoteAuthClientResult>(&runtime->remote_status);
        runtime->remote_status.phase = RemoteAuthClientPhase::authenticated;
    } else if (runtime->last_remote_event == RemoteAuthClientEventKind::failed ||
               runtime->last_remote_event == RemoteAuthClientEventKind::cancelled) {
        const RemoteAuthClientResult terminal = runtime->remote_status;
        const RemoteAuthClientResult stopped = remote_auth_client_stop(&runtime->remote_auth, 5'000);
        runtime->remote_active = false;
        runtime->remote_status = terminal;
        if (!stopped.ok()) {
            runtime->error = AccountLoginError::stop;
            runtime->profile_native_error = stopped.native_error;
        }
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_pump(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime)
        return login_invalid_input();
    runtime->last_event = UserAuthCoordinatorEventKind::none;
    // A quarantined stop only tears down. Neither a pending browser
    // response nor a late QR completion may restart profile setup.
    if (runtime->error == AccountLoginError::stop)
        return login_status(runtime);
    if (runtime->remote_mode) {
        return pump_remote_auth(runtime);
    }
    if (!runtime->coordinator_started) {
        return login_status(runtime);
    }
    if (account_profile_promotion_has_credential(runtime->promotion) && !runtime->profile_saved) {
        commit_login_profile(runtime);
        return login_status(runtime);
    }
    if (runtime->authenticated_pending && !runtime->profile_saved) {
        stage_coordinator_profile(runtime);
        return login_status(runtime);
    }
    UserAuthCoordinatorEvent event{};
    UserAuthCoordinatorResult pumped{};
    if (runtime->browser_mode) {
        if (runtime->browser.authenticated) {
            // A transport join may fail before the staged profile exists.
            // Preserve the validated credential for a save retry.
            pumped.phase = UserAuthCoordinatorPhase::authenticated;
            event.kind = UserAuthCoordinatorEventKind::authenticated;
        } else {
            const auto browser = browser_login_pump(&runtime->browser, &runtime->coordinator);
            event = browser.event;
            pumped = browser.coordinator;
            if (browser.failed) {
                runtime->error = AccountLoginError::browser_auth;
                browser_login_stop(&runtime->browser, 5'000);
                return login_status(runtime);
            }
            if (!browser.accepted)
                return login_status(runtime);
        }
    } else {
        pumped = user_auth_coordinator_pump(&runtime->coordinator, &event);
    }
    runtime->last_event = event.kind;
    if (event.kind != UserAuthCoordinatorEventKind::none) {
        runtime->user_id = event.response.has_user_id ? event.response.user_id : runtime->user_id;
        runtime->error_code = event.response.has_error_code ? event.response.error_code : 0;
        if (event.response.mfa_capabilities != 0) {
            runtime->mfa_capabilities = event.response.mfa_capabilities;
        }
    }
    if (!pumped.ok()) {
        runtime->error = AccountLoginError::coordinator;
        return login_status(runtime);
    }
    if (event.kind == UserAuthCoordinatorEventKind::authenticated && runtime->recovery_mode) {
        runtime->error = AccountLoginError::coordinator;
        return login_status(runtime);
    }
    if (event.kind == UserAuthCoordinatorEventKind::authenticated && !runtime->profile_saved) {
        runtime->authenticated_pending = true;
        stage_coordinator_profile(runtime);
    }
    return login_status(runtime);
}

HANDLE WINAPI module_login_notification_handle(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    // Profile faults require a save retry. A joined QR
    // thread stays signaled and must not drive a busy retry loop here.
    if (runtime && (runtime->error == AccountLoginError::profile || runtime->error == AccountLoginError::stop ||
                    (runtime->authenticated_pending && runtime->error != AccountLoginError::none)))
        return nullptr;
    if (runtime && runtime->browser_mode)
        return browser_login_notification_handle(&runtime->browser, &runtime->coordinator);
    return runtime ? runtime->remote_active ? remote_auth_client_notification_handle(&runtime->remote_auth)
                     : runtime->coordinator_started
                         ? user_auth_coordinator_notification_handle(&runtime->coordinator)
                         : nullptr
                   : nullptr;
}

AccountLoginStatus WINAPI module_login_stop(void** instance, u32 timeout_ms) noexcept {
    if (!instance || !*instance || timeout_ms == 0)
        return login_invalid_input();
    auto* runtime = static_cast<AccountLoginRuntime*>(*instance);
    runtime->authenticated_pending = false;
    runtime->error = AccountLoginError::stop;
    runtime->profile_native_error = 0;
    account_profile_promotion_release(&runtime->promotion);
    bool retained = false;
    if (runtime->browser.receiver.runtime) {
        const auto stopped = browser_login_stop(&runtime->browser, timeout_ms);
        if (runtime->browser.receiver.runtime) {
            runtime->profile_native_error = stopped.native_error;
            runtime->browser.status = stopped;
            retained = true;
        }
    }
    if (runtime->captcha_bridge.runtime) {
        runtime->captcha_status = captcha_bridge_stop(&runtime->captcha_bridge, minimum<u32>(timeout_ms, 5'000));
        if (runtime->captcha_bridge.runtime) {
            if (!retained) runtime->profile_native_error = runtime->captcha_status.native_error;
            retained = true;
        }
    }
    if (runtime->remote_auth.runtime) {
        const RemoteAuthClientResult remote_stopped =
            remote_auth_client_stop(&runtime->remote_auth, timeout_ms);
        if (remote_stopped.http_status == 429) {
            runtime->remote_status.http_status = remote_stopped.http_status;
            runtime->remote_status.retry_at_ms = maximum(runtime->remote_status.retry_at_ms,
                                                        remote_stopped.retry_at_ms);
        }
        if (!remote_stopped.ok()) {
            if (!retained) runtime->profile_native_error = remote_stopped.native_error;
            runtime->remote_status = remote_stopped;
            retained = true;
        } else {
            runtime->remote_active = false;
        }
    }
    if (runtime->coordinator_started) {
        const UserAuthCoordinatorResult stopped =
            user_auth_coordinator_stop(&runtime->coordinator, timeout_ms);
        if (stopped.http_status == 429) {
            runtime->coordinator.state.http_status = 429;
            runtime->coordinator.state.retry_at_ms =
                maximum(runtime->coordinator.state.retry_at_ms, stopped.retry_at_ms);
        }
        if (!stopped.ok()) {
            if (!retained)
                runtime->profile_native_error = stopped.native_error;
            retained = true;
        } else {
            runtime->coordinator_started = false;
        }
    }
    // Every independent owner receives stop/secret erasure even if an earlier
    // join failed. Keep the enclosing address while any owner remains.
    if (retained)
        return login_status(runtime);
    const bool profile_saved = runtime->profile_saved;
    const bool remote_limited = runtime->remote_status.http_status == 429;
    const bool ordinary_limited = runtime->coordinator.state.http_status == 429;
    const u64 retry_at_ms = maximum(remote_limited ? runtime->remote_status.retry_at_ms : 0,
                                    ordinary_limited ? runtime->coordinator.state.retry_at_ms : 0);
    account_profile_promotion_release(&runtime->promotion);
    secure_clear_bytes(runtime, sizeof(*runtime));
    pages_release(runtime);
    *instance = nullptr;
    AccountLoginStatus status{};
    status.phase = AccountLoginPhase::stopped;
    status.profile_saved = profile_saved;
    if (remote_limited || ordinary_limited) {
        status.http_status = 429;
        status.retry_at_ms = retry_at_ms;
    }
    return status;
}

} // namespace
} // namespace maicord
