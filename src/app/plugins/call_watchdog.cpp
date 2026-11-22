#include "maicord/app/plugins/call_ledger.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/core/log.hpp"

#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_kernel.hpp"

// The hang watchdog of the call ledger: one thread that reads the mapped call marker.
// It lives apart from the ledger so a program that only times calls does not link the log.
namespace maicord {
namespace {

// Reads the marker once per hang limit while calls run, and again when the call it found would reach the limit: a call
// that begins after one look cannot reach the limit before the next, so each one that does is flagged as it reaches
// it. A look that finds no call parks the thread until plugin_call_begin wakes it, so an idle ledger does not wake it.
// The record is read the way begin writes it: whole words, in-call flag last, so a set flag means a complete record.
u32 watchdog_main(void* argument) noexcept {
    auto* ledger = static_cast<PluginCallLedger*>(argument);
    volatile u8* view = ledger->view;
    for (u32 wait_ms = ledger->hang_limit_ms;;) {
        const u32 waited = wait_one(ledger->watchdog_wake, wait_ms);
        if (waited == WAIT_FAILED || ledger->watchdog_stopping) break;
        wait_ms = ledger->hang_limit_ms;
        // Begin woke the parked thread: its call cannot reach the limit before the next look.
        if (waited != WAIT_TIMEOUT) continue;
        if (view[20] != 1) {
            // Begin stores the in-call flag, then reads parked; this side stores parked, then reads the flag. The
            // process-wide barrier between them makes this side see begin's flag or begin see parked (an asymmetric
            // Dekker handshake, with no fence in begin). A wake that both sides send costs one spare look.
            ledger->watchdog_parked = true;
            thread_process_barrier();
            if (view[20] != 1)
                wait_ms = INFINITE;
            else
                ledger->watchdog_parked = false;
            continue;
        }
        if (view[21] == 1) continue;
        const u64 owner = *reinterpret_cast<volatile u64*>(view + 8);
        const i64 started = static_cast<i64>(*reinterpret_cast<volatile u64*>(view + 24));
        if (view[20] != 1) continue;
        const i64 now = clock_now();
        const u64 elapsed_ms = now > started ? static_cast<u64>(now - started) * 1000ull /
                                                   static_cast<u64>(ledger->frequency) : 0;
        if (elapsed_ms < ledger->hang_limit_ms) {
            wait_ms = ledger->hang_limit_ms - static_cast<u32>(elapsed_ms);
            continue;
        }
        view[21] = 1;
        if (view[20] != 1) view[21] = 0;
        else log_u64(LogLevel::warning, "plugins", "call_hung_owner", owner);
    }
    return 0;
}

void stop_watchdog(PluginCallLedger* ledger) noexcept {
    if (ledger->watchdog) {
        ledger->watchdog_stopping = true;
        event_set(ledger->watchdog_wake);
        thread_wait(ledger->watchdog, INFINITE);
        thread_close(ledger->watchdog);
    }
    if (ledger->watchdog_wake) waitable_close(ledger->watchdog_wake);
    ledger->watchdog = nullptr;
    ledger->watchdog_wake = nullptr;
    ledger->watchdog_parked = false;
    ledger->watchdog_stopping = false;
    ledger->close_watchdog = nullptr;
}

} // namespace

bool plugin_call_watchdog_start(PluginCallLedger* ledger, u32 limit_ms) noexcept {
    if (!ledger->view || ledger->watchdog || !limit_ms) return false;
    ledger->hang_limit_ms = limit_ms;
    ledger->close_watchdog = stop_watchdog;
    ledger->watchdog_wake = event_create(false, false);
    if (ledger->watchdog_wake)
        ledger->watchdog = thread_start(watchdog_main, ledger, 64 * 1024, nullptr);
    if (ledger->watchdog) return true;
    stop_watchdog(ledger);
    return false;
}

} // namespace maicord
