#include "maicord/app/plugin_call_ledger.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
// Marker record: magic, owner, kind, in-call flag. 4 KiB mapping, 24 bytes used.
constexpr u8 kMagic[8]{'M', 'C', 'P', 'C', 'A', 'L', 'L', '1'};
constexpr u32 kMarkerBytes = 4096;

void store_idle(volatile u8* view) noexcept {
    for (u32 i = 0; i < 8; ++i) view[i] = kMagic[i];
    for (u32 i = 8; i < 24; ++i) view[i] = 0;
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
    if (!ledger->overrun_limit) ledger->overrun_limit = kPluginCallOverrunLimit;
}
void read_marker(const u8* bytes, PluginCallMarker* leftover) noexcept {
    bool magic = true;
    for (u32 i = 0; i < 8; ++i) magic = magic && bytes[i] == kMagic[i];
    if (!magic || bytes[20] != 1 || !leftover) return;
    leftover->owner_id = byte_load_le64(bytes + 8);
    leftover->kind = bytes[16] <= static_cast<u8>(PluginCallKind::last) ? static_cast<PluginCallKind>(bytes[16])
                                                                        : PluginCallKind::none;
    leftover->present = leftover->owner_id != 0;
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

void plugin_call_ledger_close(PluginCallLedger* ledger) noexcept {
    if (!ledger) return;
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
    if (!ledger) return {owner, 0, kind, false};
    if (!ledger->frequency) initialize_timing(ledger);
    PluginCallTicket ticket{owner, 0, kind, ledger->depth == 0};
    ++ledger->depth;
    // Only the outermost call owns the marker; nested host->plugin calls are
    // attributed to the plugin that is already on the stack.
    if (ticket.outer && ledger->view && owner) {
        u8 record[9];
        byte_store_le64(record, owner);
        record[8] = static_cast<u8>(kind);
        for (u32 i = 0; i < 9; ++i) ledger->view[8 + i] = record[i];
        ledger->view[20] = 1;
    }
    Win32LargeInteger now{};
    QueryPerformanceCounter(now.native());
    ticket.started = now.QuadPart;
    return ticket;
}

bool plugin_call_end(PluginCallLedger* ledger, const PluginCallTicket& ticket_view) noexcept {
    const PluginCallTicket ticket = ticket_view; // the ledger is written below
    if (!ledger) return false;
    Win32LargeInteger now{};
    QueryPerformanceCounter(now.native());
    if (ledger->depth) --ledger->depth;
    if (ticket.outer && ledger->view) ledger->view[20] = 0;
    if (!ticket.owner_id) return false;
    const i64 ticks = now.QuadPart > ticket.started ? now.QuadPart - ticket.started : 0;
    const u64 elapsed_us = static_cast<u64>(ticks) * 1'000'000ull / static_cast<u64>(ledger->frequency);
    auto* record = entry(ledger, ticket.owner_id, true);
    if (!record) return false;
    saturating_increment(record->calls);
    record->worst_us = maximum(record->worst_us, elapsed_us);
    const bool lifecycle = ticket.kind == PluginCallKind::query || ticket.kind == PluginCallKind::start ||
                           ticket.kind == PluginCallKind::stop;
    if (elapsed_us <= (lifecycle ? ledger->lifecycle_budget_us : ledger->budget_us)) return false;
    saturating_increment(record->overruns);
    if (record->disable_requested || record->overruns < ledger->overrun_limit) return false;
    record->disable_requested = true;
    return true;
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
