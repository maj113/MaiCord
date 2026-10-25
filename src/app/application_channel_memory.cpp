#include "application_channel_memory.hpp"
#include "maicord/core/memory.hpp"
#include "application_state.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {

// Navigation bursts coalesce into one write; the first deadline stays fixed.
constexpr u32 kWriteDelayMs = 3'000;
constexpr u32 kRetryDelayMs = 30'000;

struct Memory {
    ChannelMemory table;
    u64 profile_id;
    u64 write_at_ms;
    u32 written_revision;
    u32 observed_selection;
    wchar_t path[512];
};
Memory g_memory;

void observe(const ChatStateStore& state, u64 now_ms) noexcept {
    g_memory.observed_selection = state.selection_revision;
    if (channel_memory_observe(&g_memory.table, state) && !g_memory.write_at_ms)
        g_memory.write_at_ms = now_ms + kWriteDelayMs;
}

void write(u64 now_ms) noexcept {
    if (g_memory.table.revision == g_memory.written_revision) {
        g_memory.write_at_ms = 0;
        return;
    }
    if (channel_memory_save_file(g_memory.path, g_memory.table)) {
        g_memory.written_revision = g_memory.table.revision;
        g_memory.write_at_ms = 0;
        return;
    }
    log_write(LogLevel::warning, "nav", "channel_memory_save_failed");
    g_memory.write_at_ms = now_ms + kRetryDelayMs;
}

} // namespace

void application_channel_memory_start(u64 profile_id) noexcept {
    reset_value(&g_memory);
    if (!g_app.account_ui || !channel_memory_path(profile_id, g_memory.path, 512))
        return;
    g_memory.profile_id = profile_id;
    if (!channel_memory_load_file(g_memory.path, &g_memory.table))
        log_write(LogLevel::warning, "nav", "channel_memory_load_failed");
    g_memory.written_revision = g_memory.table.revision;
    ChatStateStore& state = g_app.account_ui->chat_state;
    g_memory.observed_selection = state.selection_revision;
    channel_memory_seed_restore(g_memory.table, &state);
    log_u64(LogLevel::info, "nav", "channel_memory_entries", g_memory.table.count);
}

void application_channel_memory_stop() noexcept {
    if (!g_memory.profile_id)
        return;
    if (g_app.account_ui)
        observe(g_app.account_ui->chat_state, clock_monotonic_milliseconds());
    write(clock_monotonic_milliseconds());
    reset_value(&g_memory);
}

void application_channel_memory_pump(u64 now_ms) noexcept {
    if (!g_memory.profile_id || !g_app.account_ui)
        return;
    const ChatStateStore& state = g_app.account_ui->chat_state;
    if (g_memory.observed_selection != state.selection_revision)
        observe(state, now_ms);
    if (g_memory.write_at_ms && now_ms >= g_memory.write_at_ms)
        write(now_ms);
}

ChannelMemoryEntry application_channel_memory_recall(u64 guild_id) noexcept {
    const ChannelMemoryEntry* found = g_memory.profile_id ? channel_memory_find(g_memory.table, guild_id) : nullptr;
    return found ? *found : ChannelMemoryEntry{};
}

u64 application_channel_memory_next_wake() noexcept {
    if (!g_memory.profile_id || !g_app.account_ui)
        return 0;
    return g_memory.observed_selection != g_app.account_ui->chat_state.selection_revision ? 1
                                                                                          : g_memory.write_at_ms;
}

} // namespace maicord::app_detail
