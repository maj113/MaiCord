#pragma once

#include "maicord/app/account_module_api.hpp"

namespace maicord {

inline constexpr u32 kAccountHostNotificationHandleCapacity = 6;

inline constexpr u32 account_host_observation_phase = 1u << 0;
inline constexpr u32 account_host_observation_ready = 1u << 1;
inline constexpr u32 account_host_observation_session_saves = 1u << 2;
inline constexpr u32 account_host_observation_session_save_failures = 1u << 3;

enum class AccountHostError : u8 {
    none,
    invalid_input,
    module_path,
    module_load,
    module_symbol,
    api_mismatch,
    active,
    login_stop,
    account_stop,
    module_release,
};

struct AccountHostResult {
    u32 native_error;
    AccountHostError error;

    constexpr bool ok() const noexcept {
        return error == AccountHostError::none;
    }
};

struct AccountHostStopResult {
    AccountModuleStatus account_status;
    AccountLoginStatus login_status;
    u32 observations;
    u32 native_error;
    AccountHostError error;

    constexpr bool ok() const noexcept {
        return error == AccountHostError::none;
    }
};

struct AccountHost {
    HMODULE module;
    void* instance;
    void* login_instance;
    AccountModuleApi api;
    AccountModuleStatus status;
    AccountLoginStatus login_status;
    u32 last_ready;
    u32 last_session_saves;
    u32 last_session_save_failures;
    u8 last_phase;
    bool stop_failed;
};

static_assert(sizeof(AccountHost) == 1408);

bool account_host_api_valid(const AccountModuleApi& api) noexcept;
AccountHostResult account_host_module_path(Slice<wchar_t> output) noexcept;
AccountHostResult account_host_load(AccountHost* host) noexcept;
AccountHostResult account_host_release(AccountHost* host) noexcept;
u32 account_host_observe(AccountHost* host, const AccountModuleStatus& status) noexcept;
AccountModuleStartResult account_host_start(AccountHost* host, const wchar_t* profile_path,
                                            const wchar_t* session_path, u64 monotonic_now_ms,
                                            u64 unix_now_ms) noexcept;
AccountLoginStatus account_host_login_start(AccountHost* host, const wchar_t* profile_path) noexcept;
AccountLoginStatus account_host_login_start_reset(AccountHost* host, const wchar_t* profile_path) noexcept;
AccountLoginStatus account_host_login_submit_reset(AccountHost* host, const SecureBuffer* link_token,
                                                   const SecureBuffer* password) noexcept;
AccountLoginStatus account_host_login_submit_verification(AccountHost* host,
                                                          const SecureBuffer* link_token) noexcept;
AccountLoginStatus account_host_login_submit_recovery(AccountHost* host,
                                                      const SecureBuffer* login) noexcept;
AccountLoginStatus account_host_login_submit_phone_verify(AccountHost* host,
                                                          const SecureBuffer* code) noexcept;
AccountLoginStatus account_host_login_submit_phone_resend(AccountHost* host) noexcept;
SecureBufferResult account_host_login_take_phone_reset_token(AccountHost* host, u64 expected_generation,
                                                             SecureBuffer* output) noexcept;
AccountModuleStatus account_host_pump(AccountHost* host, u64 monotonic_now_ms, u64 unix_now_ms,
                                      u32* observations) noexcept;
AccountLoginStatus account_host_login_pump(AccountHost* host) noexcept;
AccountLoginStatus account_host_login_stop(AccountHost* host, u32 timeout_ms) noexcept;
AccountHostStopResult account_host_stop(AccountHost* host, u32 timeout_ms, u64 monotonic_now_ms,
                                        u64 unix_now_ms) noexcept;
u32 account_host_notification_handles(const AccountHost* host, HANDLE* output, u32 capacity) noexcept;
u32 account_host_wait_timeout(const AccountHost* host, u64 now_ms) noexcept;
bool account_host_wake_due(const AccountHost* host, u64 now_ms) noexcept;
u32 account_host_runtime_bytes() noexcept;

} // namespace maicord
