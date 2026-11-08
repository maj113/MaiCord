#include "maicord/app/plugins/call_ledger.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/core/log.hpp"

#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_kernel.hpp"

// The hang watchdog of the call ledger: one thread that reads the mapped call marker.
// It lives apart from the ledger so a program that only times calls does not link the log.
namespace maicord {
namespace {

// Reads the marker once per hang limit, and again when the call it found would reach the limit: a call that begins
// after one look cannot reach the limit before the next, so each one that does is flagged as it reaches it, and an
// idle ledger wakes the thread once per limit. The record is read the way begin writes it: in-call flag last, so a
// set flag means a complete record.
u32 watchdog_main(void* argument) noexcept {
    auto* ledger = static_cast<PluginCallLedger*>(argument);
    u32 wait_ms = ledger->hang_limit_ms;
    while (wait_one(ledger->watchdog_stop, wait_ms) == WAIT_TIMEOUT) {
        wait_ms = ledger->hang_limit_ms;
        volatile u8* view = ledger->view;
        if (view[20] != 1 || view[21] == 1) continue;
        u8 record[16];
        for (u32 i = 0; i < 8; ++i) record[i] = view[8 + i];
        for (u32 i = 0; i < 8; ++i) record[8 + i] = view[24 + i];
        if (view[20] != 1) continue;
        const u64 owner = byte_load_le64(record);
        const i64 now = clock_now();
        const i64 started = static_cast<i64>(byte_load_le64(record + 8));
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
        event_set(ledger->watchdog_stop);
        thread_wait(ledger->watchdog, INFINITE);
        thread_close(ledger->watchdog);
    }
    if (ledger->watchdog_stop) waitable_close(ledger->watchdog_stop);
    ledger->watchdog = nullptr;
    ledger->watchdog_stop = nullptr;
    ledger->close_watchdog = nullptr;
}

} // namespace

bool plugin_call_watchdog_start(PluginCallLedger* ledger, u32 limit_ms) noexcept {
    if (!ledger || !ledger->view || ledger->watchdog || !limit_ms) return false;
    ledger->hang_limit_ms = limit_ms;
    ledger->close_watchdog = stop_watchdog;
    ledger->watchdog_stop = event_create(true, false);
    if (ledger->watchdog_stop)
        ledger->watchdog = thread_start(watchdog_main, ledger, 64 * 1024, nullptr);
    if (ledger->watchdog) return true;
    stop_watchdog(ledger);
    return false;
}

} // namespace maicord
