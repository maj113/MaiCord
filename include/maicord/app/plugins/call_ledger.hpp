#pragma once
#include "maicord/core/slice.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {

// What host code is running inside a plugin. Recorded in the call marker so a
// crash during that call can be attributed after the next start.
enum class PluginCallKind : u8 {
    none, query, start, stop, settings, menu, pre_send, observe, command, slot_action, timer,
    keybind, event, decorate, modal, pre_edit,
    last = pre_edit,
};
// Which budget a call runs under. Every legacy kind keeps the gesture or lifecycle
// budget it always had; the hook runtime names the class of each hook.
enum class PluginCallClass : u8 { gesture, lifecycle, decorate, pre, event };
inline constexpr u32 kPluginCallLedgerOwners = 32;
inline constexpr u32 kPluginCallOverrunLimit = 3;
inline constexpr u64 kPluginCallBudgetUs = 50'000;
inline constexpr u64 kPluginCallLifecycleBudgetUs = 500'000;
inline constexpr u64 kPluginCallDecorateBudgetUs = 1'000;
inline constexpr u64 kPluginCallPreBudgetUs = 10'000;
inline constexpr u64 kPluginCallEventBudgetUs = 10'000;
// A single call inside a plugin this long is reported by the watchdog thread.
inline constexpr u32 kPluginCallHangMilliseconds = 10'000;

struct PluginCallLedgerEntry {
    u64 owner_id;
    u64 worst_us;
    u32 calls, overruns;
    bool disable_requested;
};

struct PluginCallMarker {
    u64 owner_id;
    PluginCallKind kind;
    bool present;
    // The watchdog saw the call outlast kPluginCallHangMilliseconds before the process died.
    bool hung;
};

struct PluginCallTicket {
    u64 owner_id;
    i64 started;
    PluginCallKind kind;
    PluginCallClass call_class;
    bool outer;
};

// What one finished call cost.
struct PluginCallOutcome {
    u64 elapsed_us;
    // This call ran past its class budget (a strike).
    bool overran;
    // This call's strike pushed the owner over the limit; a disable is pending.
    bool disable;
};

// UI-thread only, zero-initialized and caller-owned. The marker is a small
// mapped file: entering a call stores the owner and leaving clears one word,
// so neither side costs a system call and the record survives a process crash.
// budget_us and lifecycle_budget_us are the gesture and lifecycle budgets.
struct PluginCallLedger {
    HANDLE file, mapping;
    volatile u8* view;
    wchar_t path[1024];
    i64 frequency;
    u64 budget_us, lifecycle_budget_us, decorate_budget_us, pre_budget_us, event_budget_us;
    u32 depth;
    u32 overrun_limit;
    // The hang watchdog (call_watchdog.cpp): its thread, the event that stops it, its settings, and
    // the function close calls to stop it. Null until plugin_call_watchdog_start.
    HANDLE watchdog, watchdog_stop;
    u32 hang_limit_ms, hang_poll_ms;
    void (*close_watchdog)(PluginCallLedger*) noexcept;
    PluginCallLedgerEntry entries[kPluginCallLedgerOwners];
};

// Opens or creates the marker. A record left in a call by a previous process is
// reported through leftover and then cleared. Without a path only timing works.
bool plugin_call_ledger_open(PluginCallLedger*, const wchar_t* path, PluginCallMarker* leftover) noexcept;
// %LOCALAPPDATA%\MaiCord\plugin-call.bin; creates the MaiCord folder if needed.
bool plugin_call_ledger_open_default(PluginCallLedger*, PluginCallMarker* leftover) noexcept;
bool plugin_call_ledger_default_path(wchar_t (&path)[1024]) noexcept;
// Startup check without creating anything: reports and removes a marker that a
// previous process left inside a call.
bool plugin_call_ledger_peek(const wchar_t* path, PluginCallMarker* leftover) noexcept;
// A clean close stops the watchdog and removes the marker file; only a process
// that dies keeps one.
void plugin_call_ledger_close(PluginCallLedger*) noexcept;
// A null ledger makes begin/end no-ops so hosts without accounting still work.
// The kind alone selects the gesture or lifecycle budget; hooks name their class.
PluginCallTicket plugin_call_begin(PluginCallLedger*, u64 owner_id, PluginCallKind) noexcept;
PluginCallTicket plugin_call_begin(PluginCallLedger*, u64 owner_id, PluginCallKind, PluginCallClass) noexcept;
// True when this call pushed the owner over the overrun limit.
bool plugin_call_end(PluginCallLedger*, const PluginCallTicket&) noexcept;
PluginCallOutcome plugin_call_finish(PluginCallLedger*, const PluginCallTicket&) noexcept;
// Starts the thread that reads the marker every poll_ms. A call inside a plugin
// for limit_ms is logged with its owner and recorded in the marker, so the next
// start knows the plugin hung even if the user killed the process. It cannot
// interrupt the plugin. Needs an open marker file.
bool plugin_call_watchdog_start(PluginCallLedger*, u32 limit_ms = kPluginCallHangMilliseconds,
                                u32 poll_ms = 1000) noexcept;
bool plugin_call_ledger_disable_requested(const PluginCallLedger*, u64 owner_id) noexcept;
// Returns one owner whose disable is pending and clears its request, or zero.
u64 plugin_call_ledger_take_disable(PluginCallLedger*) noexcept;
// Fresh accounting for an owner that was enabled again.
void plugin_call_ledger_forget(PluginCallLedger*, u64 owner_id) noexcept;

} // namespace maicord
