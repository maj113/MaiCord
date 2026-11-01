#include "account_module_internal.hpp"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "login/account_login_handoff.hpp"
#pragma clang diagnostic pop

namespace maicord {
namespace account_module_detail {

// Request body of the submit entries that build one. They run one at a time on the UI thread and
// the REST queue copies the body before they return.
u8 g_request_body[kMessageCreateMaximumBodyBytes];

} // namespace account_module_detail
namespace {

static_assert(kMessageEditMaximumBodyBytes <= sizeof(g_request_body) && kCdnMaximumTargetBytes * 2 <= sizeof(g_request_body));

} // namespace
namespace account_module_detail {

RestClient* module_rest(void* instance, bool details) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? user_account_coordinator_rest(&runtime->coordinator, details) : nullptr;
}

GatewayClient* module_gateway(void* instance) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
}

// The DETAILS lane for a request id; null without a runtime or with no id.
RestClient* request_lane(void* instance, u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && request_id ? user_account_coordinator_rest(&runtime->coordinator, true) : nullptr;
}

GatewayClientSelfStatusSnapshot WINAPI module_self_status_snapshot(void* instance) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? gateway_client_self_status_snapshot(&runtime->coordinator.gateway)
                   : GatewayClientSelfStatusSnapshot{};
}

RestClientSubmitResult WINAPI module_self_status_submit(void* instance, u64 profile_id,
                                                        u64 self_user_id,
                                                        SelfStatusSettingsOperation operation,
                                                        GatewayUserStatus status, u64 request_id,
                                                        u64 unix_now_ms) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? gateway_client_self_status_submit(&runtime->coordinator.gateway, &runtime->coordinator.rest,
                                                       profile_id, self_user_id, operation, status, request_id,
                                                       unix_now_ms)
                   : RestClientSubmitResult{RestClientSubmitError::invalid_input};
}

bool WINAPI module_self_status_complete(void* instance, u64 profile_id, u64 self_user_id, u64 request_id,
                                        const RestClientResponse* response, Utf8View body) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && gateway_client_self_status_complete(&runtime->coordinator.gateway, profile_id,
                                                          self_user_id, request_id, response, body);
}

AccountModuleStatus module_failure(AccountModuleError error, u32 native_error) noexcept {
    AccountModuleStatus status{};
    status.error = error;
    status.native_error = native_error;
    status.failed = true;
    return status;
}

// A media request that never reached the worker.
__declspec(noinline) AccountMediaRequestResult rejected_media_request() noexcept {
    AccountMediaRequestResult value{};
    value.state = AccountMediaRequestState::invalid;
    value.error = AccountMediaError::invalid_input;
    return value;
}

} // namespace account_module_detail
namespace {

AccountLoginStatus module_login_start_mode(void** instance, const wchar_t* profile_path,
                                           bool reset) noexcept {
    if (!instance || *instance || !profile_path)
        return login_invalid_input();
    auto* runtime = static_cast<AccountLoginRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountLoginRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime) {
        AccountLoginStatus status{};
        status.error = AccountLoginError::memory;
        status.native_error = thread_last_error();
        return status;
    }
    if (!copy_path(profile_path, runtime->profile_path)) {
        SecureZeroMemory(runtime, sizeof(*runtime));
        VirtualFree(runtime, 0, MEM_RELEASE);
        AccountLoginStatus status{};
        status.error = AccountLoginError::invalid_input;
        return status;
    }
    runtime->reset_mode = reset;
    *instance = runtime;
    return login_status(runtime);
}

} // namespace
namespace account_module_detail {

AccountLoginStatus WINAPI module_login_start(void** instance, const wchar_t* profile_path) noexcept {
    return module_login_start_mode(instance, profile_path, false);
}

AccountLoginStatus WINAPI module_login_start_reset(void** instance, const wchar_t* profile_path) noexcept {
    return module_login_start_mode(instance, profile_path, true);
}

} // namespace account_module_detail
namespace {

bool ensure_user_auth_coordinator(AccountLoginRuntime* runtime) noexcept {
    if (runtime->coordinator_started) {
        return true;
    }
    const UserAuthCoordinatorResult started = runtime->reset_mode
                                                  ? user_auth_coordinator_start_reset(&runtime->coordinator)
                                                  : user_auth_coordinator_start(&runtime->coordinator);
    if (!started.ok()) {
        runtime->error = AccountLoginError::coordinator;
        return false;
    }
    runtime->coordinator_started = true;
    return true;
}

} // namespace
namespace account_module_detail {

AccountLoginStatus WINAPI module_login_submit_password(void* instance, const SecureBuffer* login,
                                                       const SecureBuffer* password) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !login || !password || runtime->remote_mode || runtime->reset_mode ||
        runtime->browser_mode) {
        return login_invalid_input();
    }
    if (!ensure_user_auth_coordinator(runtime)) {
        return login_status(runtime);
    }
    runtime->last_event = UserAuthCoordinatorEventKind::none;
    runtime->error_code = 0;
    runtime->mfa_capabilities = 0;
    const UserAuthCoordinatorResult submitted = user_auth_coordinator_submit_password(
        &runtime->coordinator, UserAuthPasswordInput{login, password, nullptr, false});
    if (submitted.ok())
        runtime->recovery_mode = false;
    else
        runtime->error = AccountLoginError::coordinator;
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_submit_recovery(void* instance, const SecureBuffer* login) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !login || runtime->remote_mode || runtime->reset_mode || runtime->browser_mode ||
        runtime->profile_saved || account_profile_promotion_has_credential(runtime->promotion)) {
        return login_invalid_input();
    }
    if (!ensure_user_auth_coordinator(runtime))
        return login_status(runtime);
    const auto submitted = user_auth_coordinator_submit_recovery(&runtime->coordinator, login);
    runtime->error = submitted.ok() ? AccountLoginError::none : AccountLoginError::coordinator;
    if (submitted.ok()) {
        runtime->recovery_mode = true;
        runtime->last_event = UserAuthCoordinatorEventKind::none;
        runtime->error_code = 0;
        runtime->mfa_capabilities = 0;
        runtime->user_id = 0;
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_start_browser(void* instance, u64 generation,
                                                     u32 timeout_ms) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !generation || runtime->browser_mode || runtime->remote_mode || runtime->reset_mode ||
        runtime->coordinator_started || runtime->profile_saved || timeout_ms < 1000 ||
        timeout_ms > 300000) {
        return login_invalid_input();
    }
    if (!ensure_user_auth_coordinator(runtime))
        return login_status(runtime);
    runtime->browser_mode = true;
    if (!browser_login_start(&runtime->browser, generation, timeout_ms).ok())
        runtime->error = AccountLoginError::browser_auth;
    return login_status(runtime);
}

SecureBufferResult WINAPI module_login_copy_browser_url(void* instance, u64 generation,
                                                        SecureBuffer* output) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !runtime->browser_mode || runtime->error != AccountLoginError::none)
        return {0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
    return browser_sign_in_receiver_copy_url(&runtime->browser.receiver, generation, output);
}

AccountLoginStatus WINAPI module_login_submit_verification(void* instance,
                                                           const SecureBuffer* link_token) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !link_token || runtime->remote_mode || runtime->reset_mode || runtime->browser_mode)
        return login_invalid_input();
    if (!ensure_user_auth_coordinator(runtime))
        return login_status(runtime);
    const UserAuthCoordinatorResult submitted =
        user_auth_coordinator_submit_verification(&runtime->coordinator, link_token);
    if (!submitted.ok()) {
        runtime->error = AccountLoginError::coordinator;
    } else {
        runtime->recovery_mode = false;
        runtime->last_event = UserAuthCoordinatorEventKind::none;
        runtime->error_code = 0;
        runtime->mfa_capabilities = 0;
        runtime->error = AccountLoginError::none;
    }
    return login_status(runtime);
}

} // namespace account_module_detail
namespace {

bool phone_recovery_ready(const AccountLoginRuntime* runtime) noexcept {
    return runtime && runtime->coordinator_started && runtime->recovery_mode && !runtime->remote_mode &&
           !runtime->reset_mode && !runtime->profile_saved &&
           !account_profile_promotion_has_credential(runtime->promotion);
}

AccountLoginStatus phone_submitted(AccountLoginRuntime* runtime, bool ok) noexcept {
    runtime->error = ok ? AccountLoginError::none : AccountLoginError::coordinator;
    if (ok) {
        runtime->last_event = UserAuthCoordinatorEventKind::none;
        runtime->error_code = 0;
    }
    return login_status(runtime);
}

} // namespace
namespace account_module_detail {

AccountLoginStatus WINAPI module_login_submit_phone_verify(void* instance,
                                                           const SecureBuffer* code) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!phone_recovery_ready(runtime))
        return login_invalid_input();
    return phone_submitted(runtime, user_auth_coordinator_submit_phone_verify(&runtime->coordinator, code).ok());
}
AccountLoginStatus WINAPI module_login_submit_phone_resend(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!phone_recovery_ready(runtime))
        return login_invalid_input();
    return phone_submitted(runtime, user_auth_coordinator_submit_phone_resend(&runtime->coordinator).ok());
}
SecureBufferResult WINAPI module_login_take_phone_reset_token(void* instance, u64 generation,
                                                              SecureBuffer* output) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!phone_recovery_ready(runtime) || !output)
        return {0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    return user_auth_coordinator_take_phone_reset_token(&runtime->coordinator, generation, output);
}

AccountLoginStatus WINAPI module_login_submit_mfa(void* instance, const SecureBuffer* code,
                                                  UserAuthMfaType type) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !code || runtime->remote_mode || !runtime->coordinator_started)
        return login_invalid_input();
    runtime->last_event = UserAuthCoordinatorEventKind::none;
    runtime->error_code = 0;
    const UserAuthCoordinatorResult submitted =
        user_auth_coordinator_submit_mfa(&runtime->coordinator, UserAuthMfaInput{code, nullptr, type});
    if (!submitted.ok()) {
        runtime->error = AccountLoginError::coordinator;
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_submit_reset(void* instance, const SecureBuffer* link_token,
                                                    const SecureBuffer* password) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !password || !runtime->reset_mode || runtime->remote_mode || runtime->profile_saved ||
        account_profile_promotion_has_credential(runtime->promotion)) {
        return login_invalid_input();
    }
    if (!ensure_user_auth_coordinator(runtime))
        return login_status(runtime);
    const auto submitted = user_auth_coordinator_submit_reset(&runtime->coordinator, link_token, password);
    runtime->error = submitted.ok() ? AccountLoginError::none : AccountLoginError::coordinator;
    if (submitted.ok()) {
        runtime->last_event = UserAuthCoordinatorEventKind::none;
        runtime->error_code = 0;
        runtime->mfa_capabilities = 0;
        runtime->user_id = 0;
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_send_sms(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || runtime->remote_mode || !runtime->coordinator_started)
        return login_invalid_input();
    runtime->last_event = UserAuthCoordinatorEventKind::none;
    const UserAuthCoordinatorResult submitted =
        user_auth_coordinator_submit_sms_send(&runtime->coordinator);
    if (!submitted.ok()) {
        runtime->error = AccountLoginError::coordinator;
    }
    return login_status(runtime);
}

AccountLoginStatus WINAPI module_login_start_remote_auth(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || runtime->remote_mode || runtime->reset_mode || runtime->browser_mode ||
        runtime->profile_saved ||
        (runtime->coordinator_started &&
         user_auth_coordinator_status(&runtime->coordinator).phase != UserAuthCoordinatorPhase::idle)) {
        return login_invalid_input();
    }
    if (runtime->coordinator_started) {
        const UserAuthCoordinatorResult stopped = user_auth_coordinator_stop(&runtime->coordinator, 5'000);
        if (!stopped.ok()) {
            AccountLoginStatus status{};
            status.error = AccountLoginError::stop;
            status.coordinator_error = stopped.error;
            status.native_error = stopped.native_error;
            return status;
        }
        reset_value<UserAuthCoordinator>(&runtime->coordinator);
        runtime->coordinator_started = false;
    }
    runtime->last_event = UserAuthCoordinatorEventKind::none;
    runtime->last_remote_event = RemoteAuthClientEventKind::none;
    runtime->remote_status = remote_auth_client_start(&runtime->remote_auth);
    runtime->remote_mode = true;
    runtime->remote_active = runtime->remote_status.ok();
    if (!runtime->remote_status.ok()) {
        runtime->error = AccountLoginError::remote_auth;
    }
    return login_status(runtime);
}

bool WINAPI module_login_copy_remote_auth_qr(void* instance, Slice<char> output, u32* bytes) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    return runtime && runtime->remote_mode && runtime->remote_active &&
           remote_auth_client_copy_qr_url(runtime->remote_auth, output, bytes);
}

bool WINAPI module_login_copy_remote_auth_user(void* instance, Slice<char> output, u32* bytes) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    return runtime && runtime->remote_mode && runtime->remote_active &&
           remote_auth_client_copy_user(runtime->remote_auth, output, bytes);
}

AccountLoginStatus WINAPI module_login_cancel_remote_auth(void* instance) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !runtime->remote_mode || !runtime->remote_active)
        return login_invalid_input();
    runtime->remote_status = remote_auth_client_cancel(&runtime->remote_auth);
    return login_status(runtime);
}

SecureBufferResult WINAPI module_login_copy_webauthn_challenge(void* instance,
                                                               SecureBuffer* output) noexcept {
    auto* runtime = static_cast<AccountLoginRuntime*>(instance);
    if (!runtime || !output) {
        return SecureBufferResult{0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    }
    return runtime->coordinator_started
               ? user_auth_coordinator_copy_secret(
                     &runtime->coordinator, UserAuthCoordinatorSecretKind::webauthn_challenge, output)
               : SecureBufferResult{0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
}

} // namespace account_module_detail
} // namespace maicord
