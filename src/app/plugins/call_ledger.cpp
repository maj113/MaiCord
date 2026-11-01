#include "maicord/app/plugins/call_ledger.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
// Marker record: magic, owner, kind, in-call flag, hung flag, start tick. 4 KiB mapping, 32 bytes used.
constexpr u8 kMagic[8]{'M', 'C', 'P', 'C', 'A', 'L', 'L', '1'};
constexpr u32 kMarkerBytes = 4096;

void store_idle(volatile u8* view) noexcept {
    for (u32 i = 0; i < 8; ++i) view[i] = kMagic[i];
    for (u32 i = 8; i < 32; ++i) view[i] = 0;
}
PluginCallLedgerEntry* entry(PluginCallLedger* ledger, u64 owner, bool create) noexcept {
    PluginCallLedgerEntry* empty = nullptr;
    for (auto& candidate : ledger->entries) {
        if (candidate.owner_id == owner) return &candidate;
        if (!empty && !candidate.owner_id) empty = &candidate;
    }
    if (!create || !empty) return nullptr;
    reset_value(empty);
    empty->owner_id = owner;
    return empty;
}
void initialize_timing(PluginCallLedger* ledger) noexcept {
    Win32LargeInteger frequency{};
    QueryPerformanceFrequency(frequency.native());
    ledger->frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    if (!ledger->budget_us) ledger->budget_us = kPluginCallBudgetUs;
    if (!ledger->lifecycle_budget_us) ledger->lifecycle_budget_us = kPluginCallLifecycleBudgetUs;
    if (!ledger->decorate_budget_us) ledger->decorate_budget_us = kPluginCallDecorateBudgetUs;
    if (!ledger->pre_budget_us) ledger->pre_budget_us = kPluginCallPreBudgetUs;
    if (!ledger->event_budget_us) ledger->event_budget_us = kPluginCallEventBudgetUs;
    if (!ledger->overrun_limit) ledger->overrun_limit = kPluginCallOverrunLimit;
}
PluginCallClass legacy_class(PluginCallKind kind) noexcept {
    return kind == PluginCallKind::query || kind == PluginCallKind::start || kind == PluginCallKind::stop
               ? PluginCallClass::lifecycle : PluginCallClass::gesture;
}
u64 class_budget(const PluginCallLedger& ledger, PluginCallClass call_class) noexcept {
    switch (call_class) {
    case PluginCallClass::lifecycle: return ledger.lifecycle_budget_us;
    case PluginCallClass::decorate: return ledger.decorate_budget_us;
    case PluginCallClass::pre: return ledger.pre_budget_us;
    case PluginCallClass::event: return ledger.event_budget_us;
    default: return ledger.budget_us;
    }
}
void read_marker(const u8* bytes, PluginCallMarker* leftover) noexcept {
    bool magic = true;
    for (u32 i = 0; i < 8; ++i) magic = magic && bytes[i] == kMagic[i];
    if (!magic || bytes[20] != 1 || !leftover) return;
    leftover->owner_id = byte_load_le64(bytes + 8);
    leftover->kind = bytes[16] <= static_cast<u8>(PluginCallKind::last) ? static_cast<PluginCallKind>(bytes[16])
                                                                        : PluginCallKind::none;
    leftover->present = leftover->owner_id != 0;
    leftover->hung = leftover->present && bytes[21] == 1;
}
} // namespace

bool plugin_call_ledger_peek(const wchar_t* path, PluginCallMarker* leftover) noexcept {
    if (leftover) reset_value(leftover);
    if (!path) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return true;
    u8 bytes[24];
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes, sizeof(bytes), &read, nullptr) && read == sizeof(bytes);
    CloseHandle(file);
    if (ok) read_marker(bytes, leftover);
    DeleteFileW(path);
    return true;
}

bool plugin_call_ledger_open(PluginCallLedger* ledger, const wchar_t* path, PluginCallMarker* leftover) noexcept {
    if (leftover) reset_value(leftover);
    if (!ledger || ledger->view) return false;
    initialize_timing(ledger);
    if (!path) return true;
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, kMarkerBytes, nullptr);
    void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMarkerBytes) : nullptr;
    if (!view) {
        if (mapping) CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }
    auto* bytes = static_cast<volatile u8*>(view);
    u8 copy[24];
    for (u32 i = 0; i < 24; ++i) copy[i] = bytes[i];
    read_marker(copy, leftover);
    store_idle(bytes);
    u32 length = 0;
    while (length < 1023 && path[length]) {
        ledger->path[length] = path[length];
        ++length;
    }
    ledger->path[length] = 0;
    ledger->file = file;
    ledger->mapping = mapping;
    ledger->view = bytes;
    return true;
}

bool plugin_call_ledger_default_path(wchar_t (&path)[1024]) noexcept {
    return data_path(path, 1024, L"plugin-call.bin").ok();
}

bool plugin_call_ledger_open_default(PluginCallLedger* ledger, PluginCallMarker* leftover) noexcept {
    wchar_t path[1024];
    const bool located = data_path(path, 1024, L"plugin-call.bin", true).ok();
    return plugin_call_ledger_open(ledger, located ? path : nullptr, leftover);
}

namespace {
PluginCallTicket begin_call(PluginCallLedger* ledger, u64 owner, PluginCallKind kind,
                            PluginCallClass call_class) noexcept {
    if (!ledger) return {owner, 0, kind, call_class, false};
    if (!ledger->frequency) initialize_timing(ledger);
    PluginCallTicket ticket{owner, 0, kind, call_class, ledger->depth == 0};
    ++ledger->depth;
    Win32LargeInteger now{};
    QueryPerformanceCounter(now.native());
    ticket.started = now.QuadPart;
    // Only the outermost call owns the marker; nested host->plugin calls are
    // attributed to the plugin that is already on the stack.
    if (ticket.outer && ledger->view && owner) {
        u8 record[9];
        byte_store_le64(record, owner);
        record[8] = static_cast<u8>(kind);
        u8 started[8];
        byte_store_le64(started, static_cast<u64>(ticket.started));
        for (u32 i = 0; i < 9; ++i) ledger->view[8 + i] = record[i];
        for (u32 i = 0; i < 8; ++i) ledger->view[24 + i] = started[i];
        ledger->view[21] = 0;
        ledger->view[20] = 1;
    }
    return ticket;
}
} // namespace

void plugin_call_ledger_close(PluginCallLedger* ledger) noexcept {
    if (!ledger) return;
    if (ledger->close_watchdog) ledger->close_watchdog(ledger);
    if (ledger->view) {
        store_idle(ledger->view);
        UnmapViewOfFile(const_cast<u8*>(ledger->view));
    }
    if (ledger->mapping) CloseHandle(ledger->mapping);
    if (ledger->file) {
        CloseHandle(ledger->file);
        if (ledger->path[0]) DeleteFileW(ledger->path);
    }
    ledger->view = nullptr;
    ledger->mapping = nullptr;
    ledger->file = nullptr;
    ledger->path[0] = 0;
}

PluginCallTicket plugin_call_begin(PluginCallLedger* ledger, u64 owner, PluginCallKind kind) noexcept {
    return begin_call(ledger, owner, kind, legacy_class(kind));
}

PluginCallTicket plugin_call_begin(PluginCallLedger* ledger, u64 owner, PluginCallKind kind,
                                   PluginCallClass call_class) noexcept {
    return begin_call(ledger, owner, kind, call_class);
}

PluginCallOutcome plugin_call_finish(PluginCallLedger* ledger, const PluginCallTicket& ticket_view) noexcept {
    const PluginCallTicket ticket = ticket_view; // the ledger is written below
    if (!ledger) return {};
    Win32LargeInteger now{};
    QueryPerformanceCounter(now.native());
    if (ledger->depth) --ledger->depth;
    if (ticket.outer && ledger->view) {
        ledger->view[20] = 0;
        ledger->view[21] = 0;
    }
    if (!ticket.owner_id) return {};
    const i64 ticks = now.QuadPart > ticket.started ? now.QuadPart - ticket.started : 0;
    PluginCallOutcome outcome{static_cast<u64>(ticks) * 1'000'000ull / static_cast<u64>(ledger->frequency), false, false};
    auto* record = entry(ledger, ticket.owner_id, true);
    if (!record) return outcome;
    saturating_increment(record->calls);
    record->worst_us = maximum(record->worst_us, outcome.elapsed_us);
    if (outcome.elapsed_us <= class_budget(*ledger, ticket.call_class)) return outcome;
    outcome.overran = true;
    saturating_increment(record->overruns);
    if (record->disable_requested || record->overruns < ledger->overrun_limit) return outcome;
    record->disable_requested = true;
    outcome.disable = true;
    return outcome;
}

bool plugin_call_end(PluginCallLedger* ledger, const PluginCallTicket& ticket) noexcept {
    return plugin_call_finish(ledger, ticket).disable;
}

bool plugin_call_ledger_disable_requested(const PluginCallLedger* ledger, u64 owner) noexcept {
    if (!ledger || !owner) return false;
    for (const auto& candidate : ledger->entries)
        if (candidate.owner_id == owner) return candidate.disable_requested;
    return false;
}

u64 plugin_call_ledger_take_disable(PluginCallLedger* ledger) noexcept {
    if (!ledger) return 0;
    for (auto& candidate : ledger->entries)
        if (candidate.owner_id && candidate.disable_requested) {
            const u64 owner = candidate.owner_id;
            reset_value(&candidate);
            return owner;
        }
    return 0;
}

void plugin_call_ledger_forget(PluginCallLedger* ledger, u64 owner) noexcept {
    if (!ledger) return;
    for (auto& candidate : ledger->entries)
        if (owner && candidate.owner_id == owner) reset_value(&candidate);
}

} // namespace maicord
