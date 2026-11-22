#include "maicord/app/account/account_logout_stop.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
struct HeldStop {
    AccountHost host;
    AccountLogoutStopOwner owner;
    HANDLE entered;
    HANDLE allowed;
    volatile LONG calls;
    DWORD worker_id;
    u32 received_timeout;
    bool fail;
};

AccountHostStopResult held_stop(AccountHost* host, u32 timeout) noexcept {
    auto* held = static_cast<HeldStop*>(host->instance);
    InterlockedIncrement(&held->calls);
    held->worker_id = GetCurrentThreadId();
    held->received_timeout = timeout;
    SetEvent(held->entered);
    const DWORD released = WaitForSingleObject(held->allowed, 10'000);
    AccountHostStopResult result{};
    result.account_status.profile_id = 42;
    result.account_status.session_saves = 7;
    result.login_status.user_id = 99;
    result.login_status.generation = 13;
    result.observations = account_host_observation_session_saves;
    if (held->fail || released != WAIT_OBJECT_0) {
        host->stop_failed = true;
        result.native_error = WAIT_TIMEOUT;
        result.error = AccountHostError::account_stop;
    } else {
        // Model the actual stop contract: success retires the host, failure
        // retains the same instance/module for an explicit caller retry.
        *host = {};
    }
    return result;
}

bool idle(const AccountLogoutStopOwner& owner) noexcept {
    return !account_logout_stop_owned(owner) && !account_logout_stop_notification(owner);
}
void cleanup(HeldStop* held, void (*check)(bool, const char*) noexcept) noexcept {
    if (!held) return;
    if (held->allowed) SetEvent(held->allowed);
    const bool joined = account_logout_stop_join_for_shutdown(&held->owner, 3'000);
    check(joined, "logout stop: fixture cleanup joins before releasing callback storage and events");
    if (!joined) return; // Preserve every borrowed object if a worker remains live.
    if (held->entered) CloseHandle(held->entered);
    if (held->allowed) CloseHandle(held->allowed);
    VirtualFree(held, 0, MEM_RELEASE);
}
} // namespace

void test_account_logout_stop(void (*check)(bool, const char*) noexcept) noexcept {
    auto* held = static_cast<HeldStop*>(VirtualAlloc(nullptr, sizeof(HeldStop),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(held != nullptr, "logout stop: bounded stable fixture owner allocation");
    if (!held) return;
    held->entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    held->allowed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    held->host.module = GetModuleHandleW(nullptr);
    held->host.instance = held;
    held->host.login_instance = held;
    held->fail = true;
    const HMODULE retained_module = held->host.module;
    check(held->entered && held->allowed && retained_module, "logout stop: local real-thread gate fixture initialized");
    if (!held->entered || !held->allowed || !retained_module) { cleanup(held, check); return; }
    AccountHostStopResult result{};
    result.native_error = 91;
    // A value no stop returns: the calls below must leave it alone.
    result.error = AccountHostError::module_symbol;
    check(!account_logout_stop_take(&held->owner, &result) &&
        account_logout_stop_join_for_shutdown(&held->owner, 0, &result) && result.native_error == 91 &&
        result.error == AccountHostError::module_symbol,
        "logout stop: idle poll and bounded idempotent shutdown preserve unpublished outputs");
    const auto started = account_logout_stop_start(&held->owner, &held->host, 250, held_stop);
    check(started.ok(), "logout stop: start returns while the fake host stop is still gated");
    if (!started.ok()) { cleanup(held, check); return; }
    const HANDLE completion = account_logout_stop_notification(held->owner);
    const bool entered = WaitForSingleObject(held->entered, 3'000) == WAIT_OBJECT_0;
    check(entered && account_logout_stop_owned(held->owner) && completion &&
        WaitForSingleObject(completion, 0) == WAIT_TIMEOUT &&
        held->worker_id != GetCurrentThreadId() && read_acquire(&held->calls) == 1 &&
        held->received_timeout == 250,
        "logout stop: actual background thread alone receives exact immutable stop arguments without a UI wait");
    const auto duplicate = account_logout_stop_start(&held->owner, &held->host, 500, held_stop);
    const bool poll_kept = !account_logout_stop_take(&held->owner, &result);
    check(duplicate.error == AccountLogoutStopStartError::active && poll_kept &&
        !account_logout_stop_join_for_shutdown(&held->owner, 0, &result) &&
        !account_logout_stop_join_for_shutdown(&held->owner, 1, &result) &&
        account_logout_stop_owned(held->owner) && account_logout_stop_notification(held->owner) == completion &&
        result.native_error == 91 &&
        result.error == AccountHostError::module_symbol && read_acquire(&held->calls) == 1,
        "logout stop: duplicate start, pending poll and cancelled bounded joins retain all context and unpublished result");
    SetEvent(held->allowed);
    const bool terminal = WaitForSingleObject(completion, 3'000) == WAIT_OBJECT_0;
    check(terminal && account_logout_stop_owned(held->owner) &&
        account_logout_stop_notification(held->owner) == completion,
        "logout stop: terminal thread notification keeps host ownership until successful take");
    const bool taken = terminal && account_logout_stop_take(&held->owner, &result);
    check(taken && idle(held->owner) && result.error == AccountHostError::account_stop &&
        result.native_error == WAIT_TIMEOUT && result.account_status.profile_id == 42 &&
        result.account_status.session_saves == 7 && result.login_status.user_id == 99 &&
        result.login_status.generation == 13 && result.observations == account_host_observation_session_saves &&
        held->host.instance == held && held->host.login_instance == held &&
        held->host.module == retained_module && held->host.stop_failed,
        "logout stop: joined failed stop publishes complete snapshots and retains same host instance and module for retry");
    if (!taken) { cleanup(held, check); return; }
    check(!account_logout_stop_take(&held->owner, &result) && result.error == AccountHostError::account_stop,
        "logout stop: result can be taken exactly once");
    ResetEvent(held->entered);
    ResetEvent(held->allowed);
    held->fail = false;
    const bool retry = account_logout_stop_start(&held->owner, &held->host, 250, held_stop).ok();
    check(retry && WaitForSingleObject(held->entered, 3'000) == WAIT_OBJECT_0 &&
        read_acquire(&held->calls) == 2 && !account_logout_stop_join_for_shutdown(&held->owner, 0, &result),
        "logout stop: explicit retry uses same retained host and bounded shutdown cannot abandon it");
    if (!retry) { cleanup(held, check); return; }
    SetEvent(held->allowed);
    const bool joined = account_logout_stop_join_for_shutdown(&held->owner, 3'000, &result);
    check(joined && result.ok() && result.account_status.profile_id == 42 && idle(held->owner) &&
        !held->host.instance && !held->host.login_instance && !held->host.module,
        "logout stop: final bounded join publishes successful retry before releasing host borrow");
    if (!joined) { cleanup(held, check); return; }
    bool fast_stops = true;
    for (u32 i = 0; i < 8; ++i) {
        // Empty real hosts stop immediately; exercise publication even when
        // account_host_stop returns before CreateThread's caller is scheduled.
        if (!account_logout_stop_start(&held->owner, &held->host, 250).ok() ||
            WaitForSingleObject(account_logout_stop_notification(held->owner), 3'000) != WAIT_OBJECT_0 ||
            !account_logout_stop_take(&held->owner, &result) || !result.ok() || !idle(held->owner)) {
            fast_stops = false;
            break;
        }
    }
    check(fast_stops, "logout stop: fast actual account_host_stop completion publishes only after terminal join across repeated starts");
    cleanup(held, check);
}
} // namespace maicord
