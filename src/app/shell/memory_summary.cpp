#include "memory_summary.hpp"
#include "state.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/log_event.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/time.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/render/d3d11/renderer.hpp"

namespace maicord::app_detail {
namespace {
constexpr u64 kMemorySummaryIntervalMs = 10ull * 60ull * 1000ull;
constexpr u64 kMemorySummaryMinimumGapMs = 60ull * 1000ull;
constexpr u32 kMemorySummaryTopTags = 4;
u64 g_last_summary_ms;

u64 kib(i64 bytes) noexcept {
    return bytes > 0 ? static_cast<u64>(bytes) >> 10 : 0;
}

// The atlas surfaces the renderer holds now: media and previews (with their backups), the animation frame caches and
// the glyph atlas.
u64 gpu_atlas_bytes(const D3D11Renderer& r) noexcept {
    const GpuPagedTexture* const surfaces[] = {r.media_gpu.surface,  r.media_gpu.preview_surface,
                                               r.media_gpu.backup,   r.media_gpu.preview_backup,
                                               r.animation_gpu.surface, r.preview_animation_gpu.surface,
                                               r.glyph_text.gpu.surface};
    u64 bytes = 0;
    for (const GpuPagedTexture* surface : surfaces)
        bytes += gpu_paged_texture_bytes(surface);
    return bytes;
}
} // namespace

void application_memory_summary(const char* reason) noexcept {
    const u64 now_ms = clock_monotonic_milliseconds();
    const bool startup = !g_last_summary_ms;
    if (!startup && now_ms - g_last_summary_ms < kMemorySummaryMinimumGapMs)
        return;
    g_last_summary_ms = now_ms ? now_ms : 1;
    constexpr u32 tag_count = static_cast<u32>(ArenaTag::count);
    ArenaAccount accounts[tag_count];
    ArenaAccount total{};
    for (u32 tag = 0; tag < tag_count; ++tag) {
        accounts[tag] = arena_account(static_cast<ArenaTag>(tag));
        total.arenas += accounts[tag].arenas;
        total.reserved += accounts[tag].reserved;
        total.committed += accounts[tag].committed;
    }
    LogEvent line("summary");
    line.field("reason", reason)
        .field("arenas", static_cast<u64>(total.arenas))
        .field("committed_kib", kib(total.committed))
        .field("reserved_kib", kib(total.reserved));
    // The largest tags by committed bytes, "tag:committed_kib/peak_kib".
    line.append(" top=");
    bool listed[tag_count]{};
    for (u32 rank = 0; rank < kMemorySummaryTopTags; ++rank) {
        u32 best = tag_count;
        for (u32 tag = 0; tag < tag_count; ++tag)
            if (!listed[tag] && accounts[tag].committed > 0 &&
                (best == tag_count || accounts[tag].committed > accounts[best].committed))
                best = tag;
        if (best == tag_count)
            break;
        listed[best] = true;
        char number[24];
        if (rank)
            line.append(",");
        Utf8View name = name_list_at(kArenaTagNames, best);
        char label[16];
        u32 used = 0;
        for (; used < name.bytes && used + 1 < sizeof(label); ++used)
            label[used] = name.data[used];
        label[used] = 0;
        line.append(label);
        line.append(":");
        number[decimal_write(number, kib(accounts[best].committed))] = 0;
        line.append(number);
        line.append("/");
        number[decimal_write(number, kib(accounts[best].peak_committed))] = 0;
        line.append(number);
    }
    if (g_app.renderer_ready)
        line.field("gpu_atlas_kib", gpu_atlas_bytes(g_app.renderer) >> 10);
    line.write(LogLevel::info, "memory");
}

void application_memory_summary_tick(u64 now_ms) noexcept {
    if (g_last_summary_ms && now_ms - g_last_summary_ms >= kMemorySummaryIntervalMs)
        application_memory_summary("periodic");
}

} // namespace maicord::app_detail
