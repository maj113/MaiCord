#include "maicord/app/account_host.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/app/account_module_version.hpp"
#include "maicord/platform/secure_buffer.hpp"

namespace maicord {
namespace {

AccountHostResult failure(AccountHostError error, u32 native_error = 0) noexcept {
    return AccountHostResult{native_error, error};
}

// No module call was made. login_refused also reports a failed phase and a native error.
AccountLoginStatus login_invalid_input() noexcept {
    AccountLoginStatus status{};
    status.error = AccountLoginError::invalid_input;
    return status;
}

AccountLoginStatus login_refused() noexcept {
    AccountLoginStatus status = login_invalid_input();
    status.native_error = ERROR_INVALID_PARAMETER;
    status.phase = AccountLoginPhase::failed;
    return status;
}

bool host_empty(const AccountHost& host) noexcept {
    return !host.module && !host.instance && !host.login_instance;
}

} // namespace

bool account_host_api_valid(const AccountModuleApi& api) noexcept {
    if (api.version != kAccountModuleApiVersion || api.bytes != sizeof(AccountModuleApi))
        return false;
    // Every entry after the version and size header is a function pointer and
    // mandatory, except the optional profile lookup.
    static_assert((sizeof(AccountModuleApi) - 8) % sizeof(void*) == 0);
    for (usize offset = 8; offset < sizeof(AccountModuleApi); offset += sizeof(void*)) {
        void* entry;
        copy_bytes(&entry, reinterpret_cast<const u8*>(&api) + offset, sizeof(entry));
        if (!entry && offset != __builtin_offsetof(AccountModuleApi, profile_get))
            return false;
    }
    return true;
}

AccountHostResult account_host_attach(AccountHost* host, HMODULE module, AccountModuleGetApiFunction get_api) noexcept {
    if (!host || !module || !get_api) {
        return failure(AccountHostError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    if (host->module) {
        return account_host_api_valid(host->api)
                   ? AccountHostResult{}
                   : failure(AccountHostError::api_mismatch, ERROR_INVALID_STATE);
    }
    if (!host_empty(*host)) {
        return failure(AccountHostError::invalid_input, ERROR_INVALID_STATE);
    }
    clear_bytes(host, sizeof(*host));
    if (!get_api(kAccountModuleApiVersion, &host->api) || !account_host_api_valid(host->api)) {
        clear_bytes(host, sizeof(*host));
        return failure(AccountHostError::api_mismatch, ERROR_REVISION_MISMATCH);
    }
    host->module = module;
    host->last_phase = 0xff;
    host->last_ready = ~0u;
    return AccountHostResult{};
}

void account_host_move(AccountHost* destination, AccountHost* source) noexcept {
    copy_bytes(destination, source, sizeof(*source));
    clear_bytes(source, sizeof(*source));
}

AccountHostResult account_host_release(AccountHost* host) noexcept {
    if (!host) {
        return failure(AccountHostError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    if (host->instance || host->login_instance) {
        return failure(AccountHostError::active, ERROR_BUSY);
    }
    clear_bytes(host, sizeof(*host));
    return AccountHostResult{};
}

u32 account_host_observe(AccountHost* host, const AccountModuleStatus& status) noexcept {
    if (!host) {
        return 0;
    }
    u32 observations = 0;
    if (host->last_phase != status.coordinator_phase ||
        host->status.required_action != status.required_action) {
        host->last_phase = status.coordinator_phase;
        observations |= account_host_observation_phase;
    }
    if (host->last_ready != status.gateway_ready) {
        host->last_ready = status.gateway_ready;
        observations |= account_host_observation_ready;
    }
    if (host->last_session_saves != status.session_saves) {
        host->last_session_saves = status.session_saves;
        observations |= account_host_observation_session_saves;
    }
    if (host->last_session_save_failures != status.session_save_failures) {
        host->last_session_save_failures = status.session_save_failures;
        observations |= account_host_observation_session_save_failures;
    }
    host->status = status;
    return observations;
}

AccountModuleStartResult account_host_start(AccountHost* host, const wchar_t* profile_path,
                                            const wchar_t* session_path, u64 monotonic_now_ms,
                                            u64 unix_now_ms) noexcept {
    if (!host || !host->module || host->instance || !profile_path || !session_path ||
        !account_host_api_valid(host->api)) {
        AccountModuleStartResult invalid{};
        invalid.native_error = ERROR_INVALID_PARAMETER;
        invalid.error = AccountModuleError::invalid_input;
        return invalid;
    }
    AccountModuleStartResult started =
        host->api.start(profile_path, session_path, monotonic_now_ms, unix_now_ms);
    if (started.ok()) {
        host->instance = started.instance;
    }
    return started;
}

AccountLoginStatus account_host_login_start(AccountHost* host, const wchar_t* profile_path) noexcept {
    if (!host || !host->module || host->login_instance || host->stop_failed || !profile_path ||
        !host->api.login_start) {
        return login_refused();
    }
    host->login_status = host->api.login_start(&host->login_instance, profile_path);
    return host->login_status;
}

AccountLoginStatus account_host_login_start_reset(AccountHost* host, const wchar_t* profile_path) noexcept {
    if (!host || !host->module || host->login_instance || host->stop_failed || !profile_path ||
        !host->api.login_start_reset) {
        return login_refused();
    }
    host->login_status = host->api.login_start_reset(&host->login_instance, profile_path);
    return host->login_status;
}

AccountLoginStatus account_host_login_submit_reset(AccountHost* host, const SecureBuffer* link_token,
                                                   const SecureBuffer* password) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_submit_reset || !password) {
        return login_refused();
    }
    host->login_status = host->api.login_submit_reset(host->login_instance, link_token, password);
    return host->login_status;
}

AccountModuleStatus account_host_pump(AccountHost* host, u64 monotonic_now_ms, u64 unix_now_ms,
                                      u32* observations) noexcept {
    if (observations) {
        *observations = 0;
    }
    if (!host || !host->module || !host->instance || host->stop_failed || !host->api.pump) {
        AccountModuleStatus invalid{};
        invalid.native_error = ERROR_INVALID_PARAMETER;
        invalid.error = AccountModuleError::invalid_input;
        invalid.failed = true;
        return invalid;
    }
    const AccountModuleStatus status = host->api.pump(host->instance, monotonic_now_ms, unix_now_ms);
    const u32 changed = account_host_observe(host, status);
    if (observations) {
        *observations = changed;
    }
    return status;
}

AccountLoginStatus account_host_login_submit_verification(AccountHost* host,
                                                          const SecureBuffer* link_token) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_submit_verification || !link_token) {
        return login_refused();
    }
    host->login_status = host->api.login_submit_verification(host->login_instance, link_token);
    return host->login_status;
}

AccountLoginStatus account_host_login_submit_recovery(AccountHost* host,
                                                      const SecureBuffer* login) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_submit_recovery || !login) {
        return login_invalid_input();
    }
    host->login_status = host->api.login_submit_recovery(host->login_instance, login);
    return host->login_status;
}

AccountLoginStatus account_host_login_pump(AccountHost* host) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed || !host->api.login_pump)
        return login_refused();
    host->login_status = host->api.login_pump(host->login_instance);
    return host->login_status;
}

AccountLoginStatus account_host_login_submit_phone_verify(AccountHost* host,
                                                          const SecureBuffer* code) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_submit_phone_verify || !code) {
        return login_invalid_input();
    }
    host->login_status = host->api.login_submit_phone_verify(host->login_instance, code);
    return host->login_status;
}
AccountLoginStatus account_host_login_submit_phone_resend(AccountHost* host) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_submit_phone_resend) {
        return login_invalid_input();
    }
    host->login_status = host->api.login_submit_phone_resend(host->login_instance);
    return host->login_status;
}
SecureBufferResult account_host_login_take_phone_reset_token(AccountHost* host, u64 expected_generation,
                                                             SecureBuffer* output) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_take_phone_reset_token)
        return {0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    const auto result =
        host->api.login_take_phone_reset_token(host->login_instance, expected_generation, output);
    if (result.ok()) {
        host->login_status.phase = AccountLoginPhase::phone_token_taken;
        host->login_status.generation = expected_generation == ~0ull ? 1 : expected_generation + 1;
        host->login_status.request_id = 0;
    }
    return result;
}

AccountLoginStatus account_host_login_stop(AccountHost* host, u32 timeout_ms) noexcept {
    if (!host || !host->module || !host->login_instance || timeout_ms == 0 || !host->api.login_stop)
        return login_refused();
    host->login_status = host->api.login_stop(&host->login_instance, timeout_ms);
    if (host->login_instance) {
        host->stop_failed = true;
    } else if (!host->instance) {
        host->stop_failed = false;
    }
    return host->login_status;
}

AccountHostStopResult account_host_stop(AccountHost* host, u32 timeout_ms, u64 monotonic_now_ms,
                                        u64 unix_now_ms) noexcept {
    AccountHostStopResult result{};
    if (!host || timeout_ms == 0) {
        result.native_error = ERROR_INVALID_PARAMETER;
        result.error = AccountHostError::invalid_input;
        return result;
    }
    if (!host->module) {
        if (!host_empty(*host)) {
            result.native_error = ERROR_INVALID_STATE;
            result.error = AccountHostError::invalid_input;
        }
        return result;
    }
    if (host->login_instance) {
        result.login_status = account_host_login_stop(host, timeout_ms);
        if (host->login_instance) {
            host->stop_failed = true;
            result.native_error = result.login_status.native_error;
            result.error = AccountHostError::login_stop;
            return result;
        }
    }
    if (host->instance) {
        result.account_status = host->api.stop(&host->instance, timeout_ms, monotonic_now_ms, unix_now_ms);
        result.observations = account_host_observe(host, result.account_status);
        if (host->instance) {
            host->stop_failed = true;
            result.native_error = result.account_status.native_error;
            result.error = AccountHostError::account_stop;
            return result;
        }
    } else {
        result.account_status = host->status;
    }
    const AccountHostResult released = account_host_release(host);
    if (!released.ok()) {
        result.native_error = released.native_error;
        result.error = released.error;
    }
    return result;
}

u32 account_host_notification_handles(const AccountHost* host, HANDLE* output, u32 capacity) noexcept {
    if (!host || !output || capacity == 0 || capacity > kAccountHostNotificationHandleCapacity ||
        !host->module) {
        return 0;
    }
    HANDLE staged[kAccountHostNotificationHandleCapacity]{};
    u32 count = 0;
    if (host->instance && !host->stop_failed) {
        count = host->api.notification_handles(host->instance, staged, capacity);
        if (count > capacity) {
            return 0;
        }
    }
    if (host->login_instance && !host->stop_failed && count < capacity) {
        const HANDLE login = host->api.login_notification_handle(host->login_instance);
        bool duplicate = false;
        for (u32 index = 0; index < count; ++index) {
            duplicate = duplicate || staged[index] == login;
        }
        if (login && !duplicate) {
            staged[count++] = login;
        }
    }
    copy_bytes(output, staged, count * static_cast<u32>(sizeof(HANDLE)));
    return count;
}

u32 account_host_wait_timeout(const AccountHost* host, u64 now_ms) noexcept {
    if (!host || !host->instance || host->stop_failed || host->status.next_wake_at_ms == 0) {
        return INFINITE;
    }
    if (host->status.next_wake_at_ms <= now_ms) {
        return 0;
    }
    const u64 remaining = host->status.next_wake_at_ms - now_ms;
    return remaining >= INFINITE ? INFINITE - 1 : static_cast<u32>(remaining);
}

bool account_host_wake_due(const AccountHost* host, u64 now_ms) noexcept {
    return host && host->instance && !host->stop_failed && host->status.next_wake_at_ms != 0 &&
           now_ms >= host->status.next_wake_at_ms;
}

u32 account_host_runtime_bytes() noexcept {
    return sizeof(AccountHost);
}

} // namespace maicord
