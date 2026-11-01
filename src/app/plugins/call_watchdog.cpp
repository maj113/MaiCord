#include "maicord/app/plugins/call_ledger.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/log.hpp"

#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_kernel.hpp"

// The hang watchdog of the call ledger: one thread that reads the mapped call marker.
// It lives apart from the ledger so a program that only times calls does not link the log.
namespace maicord {
namespace {

// Reads the marker every poll and flags a call that has been inside a plugin too long. The record
// is read the way begin writes it: in-call flag last, so a set flag means a complete record.
DWORD WINAPI watchdog_main(LPVOID argument) noexcept {
    auto* ledger = static_cast<PluginCallLedger*>(argument);
    while (WaitForSingleObject(ledger->watchdog_stop, ledger->hang_poll_ms) == WAIT_TIMEOUT) {
        volatile u8* view = ledger->view;
        if (view[20] != 1 || view[21] == 1) continue;
        u8 record[16];
        for (u32 i = 0; i < 8; ++i) record[i] = view[8 + i];
        for (u32 i = 0; i < 8; ++i) record[8 + i] = view[24 + i];
        if (view[20] != 1) continue;
        const u64 owner = byte_load_le64(record);
        Win32LargeInteger now{};
        QueryPerformanceCounter(now.native());
        const i64 started = static_cast<i64>(byte_load_le64(record + 8));
        const u64 elapsed_ms = now.QuadPart > started ? static_cast<u64>(now.QuadPart - started) * 1000ull /
                                                            static_cast<u64>(ledger->frequency) : 0;
        if (elapsed_ms < ledger->hang_limit_ms) continue;
        view[21] = 1;
        if (view[20] != 1) view[21] = 0;
        else log_u64(LogLevel::warning, "plugins", "call_hung_owner", owner);
    }
    return 0;
}

void stop_watchdog(PluginCallLedger* ledger) noexcept {
    if (ledger->watchdog) {
        SetEvent(ledger->watchdog_stop);
        WaitForSingleObject(ledger->watchdog, INFINITE);
        CloseHandle(ledger->watchdog);
    }
    if (ledger->watchdog_stop) CloseHandle(ledger->watchdog_stop);
    ledger->watchdog = nullptr;
    ledger->watchdog_stop = nullptr;
    ledger->close_watchdog = nullptr;
}

} // namespace

bool plugin_call_watchdog_start(PluginCallLedger* ledger, u32 limit_ms, u32 poll_ms) noexcept {
    if (!ledger || !ledger->view || ledger->watchdog || !limit_ms || !poll_ms) return false;
    ledger->hang_limit_ms = limit_ms;
    ledger->hang_poll_ms = poll_ms;
    ledger->close_watchdog = stop_watchdog;
    ledger->watchdog_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ledger->watchdog_stop)
        ledger->watchdog = CreateThread(nullptr, 64 * 1024, watchdog_main, ledger, 0, nullptr);
    if (ledger->watchdog) return true;
    stop_watchdog(ledger);
    return false;
}

} // namespace maicord
