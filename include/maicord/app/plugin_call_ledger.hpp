#pragma once
#include "maicord/core/slice.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {

// What host code is running inside a plugin. Recorded in the call marker so a
// crash during that call can be attributed after the next start.
enum class PluginCallKind : u8 {
    none, query, start, stop, settings, menu, pre_send, observe, command, slot_action, timer,
    keybind, event, decorate, modal,
    last = modal,
};
inline constexpr u32 kPluginCallLedgerOwners = 8;
inline constexpr u32 kPluginCallOverrunLimit = 3;
inline constexpr u64 kPluginCallBudgetUs = 50'000;
inline constexpr u64 kPluginCallLifecycleBudgetUs = 500'000;

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
};

struct PluginCallTicket {
    u64 owner_id;
    i64 started;
    PluginCallKind kind;
    bool outer;
};

// UI-thread only, zero-initialized and caller-owned. The marker is a small
// mapped file: entering a call stores the owner and leaving clears one word,
// so neither side costs a system call and the record survives a process crash.
struct PluginCallLedger {
    HANDLE file, mapping;
    volatile u8* view;
    wchar_t path[1024];
    i64 frequency;
    u64 budget_us, lifecycle_budget_us;
    u32 depth;
    u32 overrun_limit;
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
// A clean close removes the marker file; only a process that dies keeps one.
void plugin_call_ledger_close(PluginCallLedger*) noexcept;
// A null ledger makes begin/end no-ops so hosts without accounting still work.
PluginCallTicket plugin_call_begin(PluginCallLedger*, u64 owner_id, PluginCallKind) noexcept;
// True when this call pushed the owner over the overrun limit.
bool plugin_call_end(PluginCallLedger*, PluginCallTicket) noexcept;
bool plugin_call_ledger_disable_requested(const PluginCallLedger*, u64 owner_id) noexcept;
// Returns one owner whose disable is pending and clears its request, or zero.
u64 plugin_call_ledger_take_disable(PluginCallLedger*) noexcept;
const PluginCallLedgerEntry* plugin_call_ledger_find(const PluginCallLedger&, u64 owner_id) noexcept;
// Fresh accounting for an owner that was enabled again.
void plugin_call_ledger_forget(PluginCallLedger*, u64 owner_id) noexcept;

} // namespace maicord
