#include "maicord/app/account/account_logout_stop.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/thread.hpp"

namespace maicord {
namespace {
u32 stop_account(void* context) noexcept {
    auto* owner = static_cast<AccountLogoutStopOwner*>(context);
    owner->result = owner->stop(owner->host, owner->timeout_ms, owner->monotonic_now_ms,
                               owner->unix_now_ms);
    return 0;
}

bool joined_result(AccountLogoutStopOwner* owner, AccountHostStopResult* result) noexcept {
    // The signaled thread handle is the publication barrier for result and all host writes.
    thread_close(owner->thread);
    if (result)
        *result = owner->result;
    reset_value(owner);
    return true;
}
} // namespace

AccountLogoutStopStartResult account_logout_stop_start(
    AccountLogoutStopOwner* owner, AccountHost* host, u32 timeout_ms, u64 monotonic_now_ms,
    u64 unix_now_ms, AccountLogoutStopFunction stop) noexcept {
    if (!owner || !host || !stop || !timeout_ms || timeout_ms == INFINITE)
        return {ERROR_INVALID_PARAMETER, AccountLogoutStopStartError::invalid_input};
    if (owner->thread || owner->host)
        return {ERROR_BUSY, AccountLogoutStopStartError::active};
    *owner = {nullptr, host, stop, timeout_ms, monotonic_now_ms, unix_now_ms, {}};
    // An ephemeral kernel thread needs no network worker, sockets or event bank.
    u32 error = 0;
    Thread* const thread = thread_start(stop_account, owner, 256u * 1024u, &error);
    if (!thread) {
        reset_value(owner);
        return {error, AccountLogoutStopStartError::thread_create};
    }
    owner->thread = thread;
    return {0, AccountLogoutStopStartError::none};
}

bool account_logout_stop_owned(const AccountLogoutStopOwner& owner) noexcept {
    return owner.thread != nullptr;
}
Waitable account_logout_stop_notification(const AccountLogoutStopOwner& owner) noexcept {
    return owner.thread ? thread_waitable(owner.thread) : nullptr;
}
bool account_logout_stop_take(AccountLogoutStopOwner* owner, AccountHostStopResult* result) noexcept {
    if (!owner || !result || !owner->thread || !thread_wait(owner->thread, 0))
        return false;
    return joined_result(owner, result);
}
bool account_logout_stop_join_for_shutdown(AccountLogoutStopOwner* owner, u32 timeout_ms,
                                           AccountHostStopResult* result) noexcept {
    if (!owner || timeout_ms == INFINITE)
        return false;
    if (!owner->thread)
        return owner->host == nullptr;
    if (!thread_wait(owner->thread, timeout_ms))
        return false;
    return joined_result(owner, result);
}
} // namespace maicord
