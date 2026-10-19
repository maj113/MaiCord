#include "application_shutdown_metrics.hpp"

#include "application_accessibility.hpp"

#include "application_state.hpp"
#include "../render/shaped_text_cache.hpp"

#include "maicord/app/message_draft_session.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/process_info.hpp"

#if defined(MAICORD_STARTUP_PROBE)
#include "application_desktop.hpp"
#include "maicord/core/trace_record.hpp"
#endif

namespace maicord::app_detail {
namespace {

#if defined(MAICORD_STARTUP_PROBE)
struct StartupMark {
    const char* name;
    i64 at;
};
constexpr u32 kStartupMarkCapacity = 48;
struct StartupProbe {
    Clock clock;
    i64 correlation_qpc;
    u64 creation_offset_us;
    StartupMark marks[kStartupMarkCapacity];
    u32 mark_count;
    bool initialized, recorded;
};
StartupProbe g_startup_probe{};

u64 startup_probe_us(i64 at) noexcept {
    return g_startup_probe.creation_offset_us +
           clock_microseconds(g_startup_probe.clock, g_startup_probe.correlation_qpc, at);
}

u64 filetime_value(FILETIME value) noexcept {
    return static_cast<u64>(value.dwHighDateTime) << 32 | value.dwLowDateTime;
}

bool startup_probe_profile_marked() noexcept {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_STARTUP_BENCHMARK", enabled, 2) != 1 || enabled[0] != L'1')
        return false;
    wchar_t local[1024]{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 1000);
    if (!count || count >= 1000 ||
        !((count >= 3 && local[1] == L':' && (local[2] == L'\\' || local[2] == L'/')) ||
          (count >= 2 && local[0] == L'\\' && local[1] == L'\\')))
        return false;
    constexpr wchar_t marker[] = L"\\.maicord-test-profile";
    for (u32 i = 0; i < sizeof(marker) / sizeof(marker[0]); ++i)
        local[count + i] = marker[i];
    const DWORD attributes = GetFileAttributesW(local);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
#endif

// Process commit/working-set peaks, GPU residency and arena commit totals.
// Peaks cover the whole session; current values are sampled before teardown.
void log_memory_metrics() noexcept {
#define LOG_MEMORY(name, value) log_u64(LogLevel::info, "memory", name, static_cast<u64>(value))
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        LOG_MEMORY("private_bytes", counters.PrivateUsage);
        LOG_MEMORY("peak_private_bytes", counters.PeakPagefileUsage);
        LOG_MEMORY("working_set_bytes", counters.WorkingSetSize);
        LOG_MEMORY("peak_working_set_bytes", counters.PeakWorkingSetSize);
    }
    if (ID3D11Device* device = g_app.renderer.graphics.device) {
        IDXGIDevice* dxgi = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
            SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
                LOG_MEMORY("gpu_local_bytes", local.CurrentUsage);
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
                LOG_MEMORY("gpu_non_local_bytes", shared.CurrentUsage);
        }
        com_release(adapter3);
        com_release(adapter);
        com_release(dxgi);
    }
    const ArenaProcessCounters arenas = arena_process_counters();
    LOG_MEMORY("arena_initializations", arenas.initializations);
    LOG_MEMORY("arena_commits", arenas.commits);
    LOG_MEMORY("arena_committed_bytes", arenas.committed_bytes);
    LOG_MEMORY("message_scene_widget_peak_bytes",
               g_app.message_scene_storage.widgets.arena.peak + g_app.message_scene_storage.layout_nodes.arena.peak);
    LOG_MEMORY("message_scene_row_peak_bytes", g_app.message_scene.owned_rows.arena.peak);
    LOG_MEMORY("message_scene_measurement_peak_bytes", g_app.message_scene.owned_measurements.arena.peak);
    LOG_MEMORY("shell_navigation_widget_peak_bytes", g_app.shell_navigation_storage.widgets.arena.peak +
                                                         g_app.shell_navigation_storage.layout_nodes.arena.peak);
    LOG_MEMORY("member_list_widget_peak_bytes",
               g_app.member_list_storage.widgets.arena.peak + g_app.member_list_storage.layout_nodes.arena.peak);
    LOG_MEMORY("accessibility_committed_bytes", application_accessibility_committed_bytes());
    if (const AccountUiRuntime* ui = g_app.account_ui) {
        LOG_MEMORY("history_decode_peak_bytes", ui->history_decode_storage.arena.peak);
        LOG_MEMORY("message_rich_record_peak_bytes", ui->message_rich_record_storage.arena.peak);
        LOG_MEMORY("read_state_peak_bytes", ui->read_state_bank.arena.peak);
    }
#undef LOG_MEMORY
}

} // namespace

#if defined(MAICORD_STARTUP_PROBE)
bool application_startup_probe_initialize() noexcept {
    // Correlate the QPC clock with process creation before marker checks or
    // any application work. Report the successful Present return, not scanout.
    g_startup_probe.clock = clock_initialize();
    const i64 before = clock_now();
    FILETIME precise{}, created{}, exited{}, kernel{}, user{};
    GetSystemTimePreciseAsFileTime(&precise);
    const i64 after = clock_now();
    if (g_startup_probe.clock.frequency <= 0 ||
        !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ||
        filetime_value(precise) < filetime_value(created) || !startup_probe_profile_marked())
        return false;
    g_startup_probe.correlation_qpc = before + (after - before) / 2;
    g_startup_probe.creation_offset_us = (filetime_value(precise) - filetime_value(created)) / 10;
    g_startup_probe.initialized = true;
    return true;
}

void application_startup_probe_mark(const char* name) noexcept {
    if (!g_startup_probe.initialized || g_startup_probe.recorded)
        return;
    for (u32 i = 0; i < g_startup_probe.mark_count; ++i)
        if (g_startup_probe.marks[i].name == name)
            return;
    if (g_startup_probe.mark_count < kStartupMarkCapacity)
        g_startup_probe.marks[g_startup_probe.mark_count++] = {name, clock_now()};
}

void application_startup_probe_first_present() noexcept {
    if (!g_startup_probe.initialized || g_startup_probe.recorded)
        return;
    const i64 presented = clock_now();
    ULONG64 process_cycles = 0, main_cycles = 0;
    const bool process_cycles_ok = QueryProcessCycleTime(GetCurrentProcess(), &process_cycles) != FALSE;
    const bool main_cycles_ok = QueryThreadCycleTime(GetCurrentThread(), &main_cycles) != FALSE;
    g_startup_probe.recorded = true;
    FILETIME created{}, exited{}, process_kernel{}, process_user{}, main_kernel{}, main_user{};
    const bool process_times_ok =
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &process_kernel, &process_user) != FALSE;
    const bool main_times_ok =
        GetThreadTimes(GetCurrentThread(), &created, &exited, &main_kernel, &main_user) != FALSE;
    PROCESS_MEMORY_COUNTERS_EX2 memory{};
    memory.cb = sizeof(memory);
    bool memory_ok =
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory)) != FALSE;
    const bool pws_ok = memory_ok;
    if (!memory_ok) {
        // Older systems can still report commit/working set without EX2's
        // private working set. The separate status keeps that absence explicit.
        memory.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX);
        memory_ok =
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(PROCESS_MEMORY_COUNTERS_EX)) != FALSE;
    }
    TraceRecord line("MAICORD_STARTUP");
    line.field("ttff_us", startup_probe_us(presented));
    line.field("app_ttff_us", clock_microseconds(g_app.clock, g_app.process_start, presented));
    line.field("main_cycles", main_cycles);
    line.field("process_cycles", process_cycles);
    line.field("main_cpu_us", (filetime_value(main_kernel) + filetime_value(main_user)) / 10);
    line.field("process_cpu_us", (filetime_value(process_kernel) + filetime_value(process_user)) / 10);
    line.field("ws_bytes", static_cast<u64>(memory.WorkingSetSize));
    line.field("private_bytes", static_cast<u64>(memory.PrivateUsage));
    line.field("pws_bytes", pws_ok ? static_cast<u64>(memory.PrivateWorkingSetSize) : 0);
    line.field("cycles_ok", static_cast<u64>(process_cycles_ok && main_cycles_ok));
    line.field("times_ok", static_cast<u64>(process_times_ok && main_times_ok));
    line.field("memory_ok", static_cast<u64>(memory_ok));
    line.field("pws_ok", static_cast<u64>(pws_ok));
    line.field("backend_loaded", static_cast<u64>(g_app.account.module != nullptr));
    line.field("login_active", static_cast<u64>(g_app.account.login_instance != nullptr));
    line.field("present_qpc", static_cast<u64>(presented));
    line.field("qpc_hz", static_cast<u64>(g_startup_probe.clock.frequency));
    line.append("\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
    // Marks are microseconds since process creation, like ttff_us.
    for (u32 i = 0; i < g_startup_probe.mark_count; ++i) {
        TraceRecord mark("MAICORD_STARTUP_MARK");
        mark.field("name", g_startup_probe.marks[i].name);
        mark.field("us", startup_probe_us(g_startup_probe.marks[i].at));
        mark.append("\n");
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), mark.text, mark.used, &written, nullptr);
    }
    application_desktop_request_quit();
    PostMessageW(g_app.window, WM_CLOSE, 0, 0);
}
#endif

void application_log_shutdown_metrics() noexcept {
    log_memory_metrics();
#define LOG_RENDER(name, value) log_u64(LogLevel::info, "render", name, static_cast<u64>(value))
#define LOG_MEDIA(name, value) log_u64(LogLevel::info, "media", name, static_cast<u64>(value))
#define LOG_UI(name, value) log_u64(LogLevel::info, "ui", name, static_cast<u64>(value))
#define LOG_MODEL(name, value) log_u64(LogLevel::info, "model", name, static_cast<u64>(value))
#define LOG_DRAFT(name, value) log_u64(LogLevel::info, "draft", name, static_cast<u64>(value))
#define LOG_APP(name, value) log_u64(LogLevel::info, "app", name, static_cast<u64>(value))

    LOG_RENDER("painted_frames", g_app.renderer.painted_frames);
    LOG_RENDER("video_gpu_frames", g_app.renderer.media_gpu.video.frames);
    LOG_RENDER("video_gpu_converted_pixels", g_app.renderer.media_gpu.video.converted_pixels);
    if (g_app.renderer.painted_frames != 0) {
        LOG_RENDER("paint_cpu_average_us",
                   g_app.renderer.paint_cpu_us_total / g_app.renderer.painted_frames);
        LOG_RENDER("paint_cpu_max_us", g_app.renderer.paint_cpu_us_max);
        LOG_RENDER("draw_calls_max", g_app.renderer.draw_calls_max);
    }
    LOG_RENDER("gpu_timestamps_enabled", g_app.renderer.gpu_timer.enabled ? 1 : 0);
    LOG_RENDER("gpu_timestamp_samples", g_app.renderer.gpu_timer.samples);
    if (g_app.renderer.gpu_timer.samples != 0) {
        LOG_RENDER("gpu_paint_average_us",
                   g_app.renderer.gpu_timer.total_microseconds / g_app.renderer.gpu_timer.samples);
        LOG_RENDER("gpu_paint_max_us", g_app.renderer.gpu_timer.maximum_microseconds);
    }
    LOG_RENDER("gpu_timestamp_skipped", g_app.renderer.gpu_timer.skipped);
    LOG_RENDER("gpu_timestamp_disjoint", g_app.renderer.gpu_timer.disjoint);
    LOG_RENDER("gpu_timestamp_errors", g_app.renderer.gpu_timer.errors);
    LOG_RENDER("frame_clock_timeouts", g_app.frame_clock_timeouts);
    LOG_RENDER("idle_begin_frame_presents", g_app.renderer.idle_begin_frame_presents);
    LOG_RENDER("composer_layout_builds", g_app.renderer.composer_layout.builds);
    LOG_RENDER("composer_layout_hits", g_app.renderer.composer_layout.hits);
    LOG_RENDER("composer_layout_rejected", g_app.renderer.composer_layout.rejected);
    LOG_RENDER("content_text_builds", g_app.renderer.content_text_retention.builds);
    LOG_RENDER("content_text_hits", g_app.renderer.content_text_retention.hits);
    LOG_RENDER("content_text_misses", g_app.renderer.content_text_retention.misses);
    LOG_RENDER("content_text_invalidations", g_app.renderer.content_text_retention.invalidations);
    if (const auto* shaped = g_app.renderer.shaped_text_cache) {
        LOG_RENDER("shaped_text_cache_hits", shaped->stats.hits);
        LOG_RENDER("shaped_text_cache_misses", shaped->stats.misses);
        LOG_RENDER("shaped_text_cache_flips", shaped->stats.flips);
    }
    LOG_RENDER("stale_interaction_text_suppressions",
               g_app.renderer.stale_interaction_text_suppressions);
    LOG_RENDER("message_hit_layout_builds", g_app.renderer.message_hit_test.builds);
    LOG_RENDER("message_hit_layout_hits", g_app.renderer.message_hit_test.hits);
    LOG_RENDER("message_hit_layout_rejected", g_app.renderer.message_hit_test.rejected);
    LOG_RENDER("resize_clipped_updates", g_app.renderer.resize_clipped_updates);
    LOG_RENDER("resize_buffer_reallocations", g_app.renderer.graphics.resize_buffer_reallocations);
    LOG_RENDER("glyph_cache_builds", g_app.renderer.glyph_text.cache_builds);
    LOG_RENDER("text_instance_uploads", g_app.renderer.text_instance_uploads);
    LOG_RENDER("paint_instance_uploads", g_app.renderer.paint_instance_uploads);
    LOG_RENDER("media_instance_uploads", g_app.renderer.media_instance_uploads);
    LOG_RENDER("paint_stream_builds", g_app.renderer.paint_stream_builds);
    LOG_RENDER("paint_stream_reuses", g_app.renderer.paint_stream_reuses);
    LOG_RENDER("media_source_refreshes", g_app.renderer.media_source_refreshes);
    LOG_RENDER("media_atlas_resizes", g_app.renderer.media_atlas_resizes);
    LOG_RENDER("media_atlas_trims", g_app.renderer.media_atlas_trims);
    LOG_RENDER("media_source_upload_skips", g_app.renderer.media_source_upload_skips);
    LOG_RENDER("full_present_draws", g_app.renderer.full_present_draws);
    LOG_RENDER("partial_present_draws", g_app.renderer.partial_present_draws);
    LOG_RENDER("partial_present_pixels", g_app.renderer.partial_present_pixels);
    LOG_RENDER("unchanged_present_skips", g_app.renderer.unchanged_present_skips);
    LOG_RENDER("glyph_raster_dispatches", g_app.renderer.glyph_text.gpu.dispatch_count);
    LOG_RENDER("media_draws_total", g_app.renderer.media_draws_total);
    LOG_RENDER("media_draw_rejected", g_app.renderer.media_rejected);
    LOG_RENDER("preview_media_uploads", g_app.renderer.preview_media_uploads);
    LOG_RENDER("preview_media_uploaded_pixels", g_app.renderer.preview_media_uploaded_pixels);
    LOG_RENDER("media_uploads", g_app.renderer.media_gpu.uploads);
    LOG_RENDER("media_uploaded_bytes", g_app.renderer.media_gpu.uploaded_bytes);
    LOG_RENDER("png_gpu_uploads", g_app.renderer.media_gpu.png_uploads);
    LOG_RENDER("png_gpu_uploaded_bytes", g_app.renderer.media_gpu.png_uploaded_bytes);
    LOG_RENDER("png_gpu_dispatched_pixels", g_app.renderer.media_gpu.png_dispatched_pixels);
    LOG_RENDER("apng_gpu_frames", g_app.renderer.media_gpu.apng_frames);
    LOG_RENDER("apng_gpu_dispatched_pixels", g_app.renderer.media_gpu.apng_dispatched_pixels);
    LOG_RENDER("apng_gpu_source_blends", g_app.renderer.media_gpu.apng_source_blends);
    LOG_RENDER("apng_gpu_over_blends", g_app.renderer.media_gpu.apng_over_blends);
    LOG_RENDER("apng_gpu_background_disposals", g_app.renderer.media_gpu.apng_background_disposals);
    LOG_RENDER("apng_gpu_previous_disposals", g_app.renderer.media_gpu.apng_previous_disposals);
    LOG_RENDER("gif_gpu_frames", g_app.renderer.media_gpu.gif_frames);
    LOG_RENDER("gif_gpu_uploaded_bytes", g_app.renderer.media_gpu.gif_uploaded_bytes);
    LOG_RENDER("gif_gpu_dispatched_pixels", g_app.renderer.media_gpu.gif_dispatched_pixels);
    LOG_RENDER("gif_gpu_background_disposals", g_app.renderer.media_gpu.gif_background_disposals);
    LOG_RENDER("gif_gpu_previous_disposals", g_app.renderer.media_gpu.gif_previous_disposals);
    LOG_RENDER("animated_webp_gpu_frames", g_app.renderer.media_gpu.animated_webp_frames);
    LOG_RENDER("animated_webp_gpu_uploaded_bytes", g_app.renderer.media_gpu.animated_webp_uploaded_bytes);
    LOG_RENDER("animated_webp_gpu_dispatched_pixels",
               g_app.renderer.media_gpu.animated_webp_dispatched_pixels);
    LOG_RENDER("animated_webp_gpu_background_disposals",
               g_app.renderer.media_gpu.animated_webp_background_disposals);

    LOG_MEDIA("visible_collections", g_app.message_media.collections);
    LOG_MEDIA("visible_source_rows_visited", g_app.message_media.source_rows_visited);
    LOG_MEDIA("visible_rows_collected", g_app.message_media.rows_collected);
    LOG_MEDIA("primary_candidates", g_app.message_media.primary_candidates);
    LOG_MEDIA("decoration_candidates", g_app.message_media.decoration_candidates);
    LOG_MEDIA("duplicate_assets_skipped", g_app.message_media.duplicate_assets_skipped);
    LOG_MEDIA("dedupe_saturations", g_app.message_media.dedupe_saturations);
    LOG_MEDIA("controller_rejected", g_app.message_media.rejected);
    LOG_MEDIA("animation_publishes", g_app.animations.timeline.counters.publishes);
    LOG_MEDIA("animation_due_advances", g_app.animations.timeline.counters.due_advances);
    LOG_MEDIA("animation_late_advances", g_app.animations.timeline.counters.late_advances);
    LOG_MEDIA("animation_completed", g_app.animations.timeline.counters.completed_animations);
    LOG_MEDIA("animation_pauses", g_app.animations.timeline.counters.pauses);
    LOG_MEDIA("animation_resumes", g_app.animations.timeline.counters.resumes);

    LOG_RENDER("animation_frame_cache_entries", g_app.renderer.animation_gpu.cache.entry_count);
    LOG_RENDER("animation_frame_cache_captures", g_app.renderer.animation_gpu.cache.counters.captures);
    LOG_RENDER("animation_frame_cache_hits", g_app.renderer.animation_gpu.cache.counters.hits);
    LOG_RENDER("animation_frame_cache_misses", g_app.renderer.animation_gpu.cache.counters.misses);
    LOG_RENDER("animation_frame_cache_evictions", g_app.renderer.animation_gpu.cache.counters.evictions);
    LOG_RENDER("animation_frame_cache_rejected", g_app.renderer.animation_gpu.cache.counters.rejected);
    LOG_RENDER("animation_frame_visibility_rebases",
               g_app.renderer.animation_gpu.cache.allocations.visibility_rebases);
    LOG_RENDER("animation_frame_gpu_captures", g_app.renderer.animation_gpu.captures);
    LOG_RENDER("animation_frame_gpu_restores", g_app.renderer.animation_gpu.restores);
    LOG_RENDER("animation_frame_gpu_materializations", g_app.renderer.animation_gpu.materializations);
    LOG_RENDER("animation_frame_gpu_copied_pixels", g_app.renderer.animation_gpu.copied_pixels);
    LOG_RENDER("animation_frame_gpu_failures", g_app.renderer.animation_gpu.failures);
    LOG_RENDER("preview_animation_cache_entries", g_app.renderer.preview_animation_gpu.cache.entry_count);
    LOG_RENDER("preview_animation_cache_captures", g_app.renderer.preview_animation_gpu.captures);
    LOG_RENDER("preview_animation_cache_restores", g_app.renderer.preview_animation_gpu.restores);
    LOG_RENDER("preview_animation_cache_evictions", g_app.renderer.preview_animation_gpu.cache.counters.evictions);
    LOG_RENDER("preview_animation_cache_failures", g_app.renderer.preview_animation_gpu.failures);
    LOG_RENDER("media_padding_clears", g_app.renderer.media_gpu.cleared_regions);
    LOG_RENDER("media_upload_rejected", g_app.renderer.media_gpu.rejected);
    if (g_app.renderer.media_atlas) {
        LOG_RENDER("media_resident_entries", g_app.renderer.media_atlas->entry_count);
        LOG_RENDER("media_resident_insertions", g_app.renderer.media_atlas->insertions);
        LOG_RENDER("media_resident_reuse", g_app.renderer.media_atlas->reuse_requests);
        LOG_RENDER("media_resident_stale", g_app.renderer.media_atlas->stale_entries);
        LOG_RENDER("media_resident_evictions", g_app.renderer.media_atlas->allocations.evicted_entries);
        LOG_RENDER("media_resident_resets", g_app.renderer.media_atlas->allocations.full_resets);
        LOG_RENDER("media_visibility_rebases", g_app.renderer.media_atlas->allocations.visibility_rebases);
    }
    if (g_app.renderer.preview_media_atlas) {
        LOG_RENDER("preview_resident_entries", g_app.renderer.preview_media_atlas->entry_count);
        LOG_RENDER("preview_resident_insertions", g_app.renderer.preview_media_atlas->insertions);
        LOG_RENDER("preview_resident_evictions",
                   g_app.renderer.preview_media_atlas->allocations.evicted_entries);
        LOG_RENDER("preview_resident_resets", g_app.renderer.preview_media_atlas->allocations.full_resets);
        LOG_RENDER("preview_visibility_rebases",
                   g_app.renderer.preview_media_atlas->allocations.visibility_rebases);
    }

    LOG_UI("message_virtual_items", g_app.message_view.list.count);
    LOG_UI("message_height_updates", g_app.message_view.list.height_updates);
    LOG_UI("message_scroll_units", g_app.message_view.timeline.scroll_units);
    LOG_UI("message_follow_bottom", g_app.message_view.timeline.follow_bottom ? 1 : 0);
    LOG_UI("message_resident_evictions", g_app.message_view.timeline.evicted);
    LOG_UI("message_newer_deferred", g_app.message_view.timeline.deferred);
    LOG_UI("message_history_pages", g_app.message_view.timeline.history_pages);
    LOG_UI("message_history_inserted", g_app.message_view.timeline.history_inserted);
    LOG_MODEL("message_text_compactions", g_app.message_view.store.compactions);
    if (g_app.account_ui) {
        LOG_MODEL("guilds_resident", g_app.account_ui->chat_state.guild_count);
        LOG_MODEL("channels_resident", g_app.account_ui->chat_state.channel_count);
        LOG_MODEL("state_name_compactions", g_app.account_ui->chat_state.compactions);
        LOG_MODEL("state_rejected", g_app.account_ui->chat_state.rejected);
        LOG_DRAFT("persist_saves", g_app.account_ui->message_draft_session.persist_saves);
        LOG_DRAFT("persist_failures", g_app.account_ui->message_draft_session.persist_failures);
        LOG_DRAFT("persisted_bytes", g_app.account_ui->message_draft_session.persisted_bytes);
        message_draft_session_persist(g_app.account_ui->message_draft_session,
                                      g_app.account_ui->message_drafts,
                                      clock_monotonic_milliseconds(), true);
    }

    LOG_APP("message_events_drained", g_app.message_pump.drained);
    LOG_APP("message_events_applied", g_app.message_pump.applied);
    LOG_APP("message_events_deferred", g_app.message_pump.deferred);
    LOG_APP("message_history_catchup_required", g_app.message_pump.catchup_required ? 1 : 0);
    LOG_APP("message_history_catchup_state", g_app.message_catchup.state);
    LOG_APP("message_history_catchup_requests", g_app.message_catchup.requests);
    LOG_APP("message_history_catchup_pages", g_app.message_catchup.pages);
    LOG_APP("message_history_catchup_failures", g_app.message_catchup.failures);
    LOG_APP("message_history_older_state", g_app.message_older.state);
    LOG_APP("message_history_older_demands", g_app.message_older.demands);
    LOG_APP("message_history_older_requests", g_app.message_older.requests);
    LOG_APP("message_history_older_pages", g_app.message_older.pages);
    LOG_APP("message_history_older_received", g_app.message_older.received);
    LOG_APP("message_history_older_inserted", g_app.message_older.inserted);
    LOG_APP("message_history_older_evicted", g_app.message_older.evicted);
    LOG_APP("message_history_older_failures", g_app.message_older.failures);
    LOG_APP("message_events_rejected", g_app.message_pump.rejected);

#undef LOG_APP
#undef LOG_DRAFT
#undef LOG_MODEL
#undef LOG_UI
#undef LOG_MEDIA
#undef LOG_RENDER
}

} // namespace maicord::app_detail
