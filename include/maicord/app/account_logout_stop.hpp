#pragma once

#include "maicord/app/account_host.hpp"

namespace maicord {

using AccountLogoutStopFunction = AccountHostStopResult (*)(AccountHost*, u32, u64, u64) noexcept;

// Stable caller-owned storage; do not move, copy, reset or destroy a live owner.
// The host and its module remain exclusively borrowed until take/join succeeds.
// While owned, the caller must gate every other host read, call and notification.
struct AccountLogoutStopOwner {
    HANDLE thread;
    AccountHost* host;
    AccountLogoutStopFunction stop;
    u32 timeout_ms;
    u64 monotonic_now_ms;
    u64 unix_now_ms;
    AccountHostStopResult result;
};
static_assert(sizeof(AccountLogoutStopOwner) <= 512);

enum class AccountLogoutStopStartError : u8 { none, invalid_input, active, thread_create };
struct AccountLogoutStopStartResult {
    u32 native_error;
    AccountLogoutStopStartError error;
    constexpr bool ok() const noexcept { return error == AccountLogoutStopStartError::none; }
};

AccountLogoutStopStartResult account_logout_stop_start(
    AccountLogoutStopOwner* owner, AccountHost* host, u32 timeout_ms, u64 monotonic_now_ms,
    u64 unix_now_ms, AccountLogoutStopFunction stop = account_host_stop) noexcept;
bool account_logout_stop_owned(const AccountLogoutStopOwner& owner) noexcept;
// Borrowed terminal notification, consumed only by take/join; never close it.
HANDLE account_logout_stop_notification(const AccountLogoutStopOwner& owner) noexcept;
// Nonblocking. A successful kernel join publishes the copied result and clears
// the owner. False leaves output and all live ownership intact, including after
// a timeout. Stop failure is a published result, permitting an explicit retry.
bool account_logout_stop_take(AccountLogoutStopOwner* owner, AccountHostStopResult* result) noexcept;
// For final shutdown only. A finite wait never terminates the worker. True means
// joined (or already idle), not that result.ok(); false retains all live storage
// and the borrowed host/module. Idle owners leave result unchanged.
bool account_logout_stop_join_for_shutdown(AccountLogoutStopOwner* owner, u32 timeout_ms,
                                           AccountHostStopResult* result = nullptr) noexcept;

} // namespace maicord
