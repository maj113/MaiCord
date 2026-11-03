// MaiCordPerfProbe: the shipping client sources built with MAICORD_PERF_PROBE. The
// client loads MaiCordPerfAccount.dll (synthetic account, no network); this file
// drives a deterministic scenario through the real main loop and reports startup to
// first present, main-thread CPU per frame, wakes, context switches, client
// allocation calls, private working set and committed memory by owner on standard
// output. tools/perf/run_perf.ps1 aggregates several runs.
#include "perf_probe_internal.hpp"
#include "maicord/app/account/account_paths.hpp"
#include "../../src/app/chat/channel_session.hpp"
#include "../../src/app/shell/content_input.hpp"
#include "../../src/app/chat/messages/history.hpp"
#include "../../src/app/account/account_pump.hpp"
#include "../../src/app/shell/background.hpp"
#include "../../src/app/shell/frame_presentation.hpp"
#include "../../src/app/shell/tooltip.hpp"
#include "../../src/app/media/video.hpp"
#include "../../src/app/voice/call.hpp"
#include "../../src/app/shell/scheduler.hpp"
#include "../../src/app/account/activity.hpp"
#include "../../src/app/guild/invite_cards.hpp"
#include "../../src/app/chat/messages/components.hpp"
#include "../../src/app/account/login/action_captcha.hpp"
#include "../../src/app/settings/emoji_preferences.hpp"
#include "../../src/app/chat/messages/feedback.hpp"
#include "../../src/app/voice/stream.hpp"
#include "../../src/app/voice/stage.hpp"
#include "../../src/app/voice/soundboard.hpp"
#include "../../src/app/account/profile.hpp"
#include "../../src/app/chat/read_state.hpp"
#include "../../src/app/chat/scoped_read.hpp"
#include "../../src/app/notifications/notifications.hpp"
#include "../../src/app/chat/messages/pinned_messages.hpp"
#include "../../src/app/media/picker.hpp"
#include "../../src/app/guild/channel_overview.hpp"
#include "../../src/app/chat/channel_snapshots.hpp"
#include "../../src/app/chat/threads/actions.hpp"
#include "../../src/app/chat/threads/browser.hpp"
#include "../../src/app/chat/threads/creation.hpp"
#include "../../src/app/chat/threads/forum_preview.hpp"
#include "../../src/app/chat/composer/preview.hpp"
#include "../../src/app/chat/polls/poll.hpp"
#include "../../src/app/guild/member_moderation.hpp"
#include "../../src/app/settings/user_settings.hpp"
#include "../../src/app/guild/folders.hpp"
#include "../../src/app/settings/account_settings.hpp"
#include "../../src/app/notifications/controls.hpp"
#include "../../src/app/account/custom_status.hpp"
#include "../../src/app/chat/inbox.hpp"
#include "../../src/app/account/application_account_module.hpp"
#include "../../src/app/voice/voice.hpp"
#include "maicord/sys/process_info.hpp"
#include "maicord/core/win32_exception.hpp"

namespace maicord::perf {
namespace {

constexpr u32 kFrameSampleCapacity = 8192;

} // namespace

namespace perf_probe_detail {

PhaseStats g_phases[static_cast<u32>(Phase::count)];
u64* g_frame_cycles = nullptr; // per present, main-thread cycles since the previous present

} // namespace perf_probe_detail

namespace {

u32 g_frame_cycle_count = 0;

} // namespace

namespace perf_probe_detail {

Probe g_probe{};

} // namespace perf_probe_detail

namespace {

// Phase tick (see arm_phase_tick).
HANDLE g_tick_timer = nullptr;
HANDLE g_tick_thread = nullptr;
volatile LONG g_tick_stop = 0;
DWORD WINAPI probe_ticker(void*) noexcept {
    while (WaitForSingleObject(g_tick_timer, INFINITE) == WAIT_OBJECT_0 && !g_tick_stop)
        PostMessageW(g_app.window, WM_NULL, 0, 0);
    return 0;
}

u64 now_100ns() noexcept {
    FILETIME time{};
    GetSystemTimePreciseAsFileTime(&time);
    return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

} // namespace

namespace perf_probe_detail {

u64 qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<u64>(value.QuadPart);
}

} // namespace perf_probe_detail

namespace {

u64 elapsed_ms(u64 since_qpc) noexcept {
    return (qpc_now() - since_qpc) * 1000 / g_probe.qpc_frequency;
}
void fail(const char* reason) noexcept {
    if (!g_probe.failed) {
        g_probe.failed = true;
        g_probe.failure = reason;
    }
}

void write_stdout(const char* text, u32 bytes) noexcept;
void trace(const char* text) noexcept {
    char line[160];
    u32 used = 0;
    for (const char* prefix = "trace "; *prefix; ++prefix)
        line[used++] = *prefix;
    while (*text && used < sizeof(line) - 2)
        line[used++] = *text++;
    line[used++] = '\n';
    write_stdout(line, used);
}

TextWork sample_text_work() noexcept {
    TextWork work{};
    if (g_app.account_ui) {
        work.markdown_parses = g_app.account_ui->message_text.markdown_parses;
        work.markdown_hits = g_app.account_ui->message_text.markdown_cache_hits;
    }
    work.text_builds = g_app.renderer.content_text_prefix.base_builds + g_app.renderer.content_text_prefix.overlay_builds;
    work.shell_reuses = g_app.renderer.content_text_prefix.shell.reuses;
    work.paint_builds = g_app.renderer.paint_stream_builds;
    work.paint_upload_bytes = g_app.renderer.paint_upload_bytes;
    work.text_upload_bytes = g_app.renderer.text_upload_bytes;
    work.media_upload_bytes = g_app.renderer.media_upload_bytes;
    if (g_app.account_ui)
        work.member_media_resolves = g_app.account_ui->member_media_cache.resolves;
    work.shell_replays = g_app.renderer.shell_segment.replays;
    return work;
}

GpuWork sample_gpu_work() noexcept {
    GpuWork work{};
    work.api = g_gpu_api;
    const auto& r = g_app.renderer;
    work.full_replays = g_replay.full_replays;
    work.partial_replays = g_replay.partial_replays;
    work.replay_pixels = g_replay.partial_pixels;
    work.replay_failures = g_replay.failures + g_replay.skipped;
    work.replays = g_replay.replays;
    work.replay_cycles = static_cast<u64>(g_replay.cycles);
    work.painted = r.painted_frames;
    work.partial_pixels = r.partial_present_pixels;
    const auto& m = r.media_gpu;
    work.media_upload_bytes = m.png_uploaded_bytes + m.apng_uploaded_bytes +
                              m.straight_rgba8_uploaded_bytes + m.gif_uploaded_bytes +
                              m.animated_webp_uploaded_bytes + m.vp8_uploaded_bytes +
                              m.vp8_alpha_uploaded_bytes + m.jpeg_uploaded_bytes;
    work.media_frames = u64{m.png_uploads} + m.straight_rgba8_uploads + m.jpeg_uploads +
                        m.apng_frames + m.gif_frames + m.animated_webp_frames + m.vp8_atlas_uploads;
    work.animation_restores = r.animation_gpu.restores + r.preview_animation_gpu.restores;
    work.video_frames = m.video.frames;
    work.animation_copy_pixels = r.animation_gpu.copied_pixels + r.preview_animation_gpu.copied_pixels;
    work.glyph_pixels = r.glyph_text.gpu.dispatched_pixels;
    work.instance_uploads =
        static_cast<u64>(r.paint_instance_uploads) + r.text_instance_uploads + r.media_instance_uploads;
    return work;
}

void begin_phase(Phase phase) noexcept {
    PhaseStats& stats = g_phases[static_cast<u32>(phase)];
    sample_process(&stats.begin);
    stats.alloc_begin = g_alloc;
    stats.frame_cycles_offset = g_frame_cycle_count;
    stats.measured = true;
    g_probe.phase = phase;
    trace(kPhaseNames[static_cast<u32>(phase)]);
    g_probe.phase_start_qpc = stats.begin.qpc;
    stats.anim_advances_begin = g_app.animations.timeline.counters.due_advances;
    stats.text_begin = sample_text_work();
    stats.gpu_begin = sample_gpu_work();
    g_probe.actions = 0;
    g_probe.next_action_qpc = stats.begin.qpc;
}
void end_phase() noexcept {
    PhaseStats& stats = g_phases[static_cast<u32>(g_probe.phase)];
    sample_process(&stats.end);
    stats.alloc_end = g_alloc;
    stats.text_end = sample_text_work();
    stats.gpu_end = sample_gpu_work();
    const auto& timeline = g_app.animations.timeline;
    stats.anim_entries = timeline.entry_count;
    stats.anim_visible = 0;
    for (u32 i = 0; timeline.entries && i < timeline.capacity; ++i)
        stats.anim_visible += (timeline.entries[i].flags & animation_timeline_entry_occupied) &&
                              (timeline.entries[i].flags & animation_timeline_entry_visible);
    stats.anim_suspended = timeline.suspended;
    stats.anim_advances = timeline.counters.due_advances - stats.anim_advances_begin;
}
void next_phase(Phase phase) noexcept {
    end_phase();
    begin_phase(phase);
}

bool select_channel(u64 guild, u64 channel) noexcept {
    if (!g_app.account_ui)
        return false;
    auto& state = g_app.account_ui->chat_state;
    if (!chat_state_store_find_channel(state, channel))
        return false;
    if (guild && !app_detail::select_content_guild(guild))
        return false;
    if (!chat_state_store_select_channel(&state, channel).ok())
        return false;
    app_detail::application_friends_hide();
    if (!app_detail::synchronize_navigation_view() || !app_detail::synchronize_selected_channel() ||
        !app_detail::refresh_account_text())
        return false;
    app_detail::pump_message_history();
    app_detail::application_request_frame();
    return true;
}

// Wheel input posted to this process's own window (synthetic data only).
void post_wheel(float x_dip, float y_dip, i32 delta) noexcept {
    const float scale = static_cast<float>(maximum(g_app.renderer.dpi, 1u)) / 96.0f;
    POINT point{static_cast<LONG>(x_dip * scale), static_cast<LONG>(y_dip * scale)};
    ClientToScreen(g_app.window, &point);
    PostMessageW(g_app.window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<u16>(static_cast<i16>(delta))),
                 MAKELPARAM(static_cast<u16>(point.x), static_cast<u16>(point.y)));
}
float client_width_dip() noexcept {
    RECT client{};
    GetClientRect(g_app.window, &client);
    return static_cast<float>(client.right) * 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
}
float client_height_dip() noexcept {
    RECT client{};
    GetClientRect(g_app.window, &client);
    return static_cast<float>(client.bottom) * 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
}
float message_x() noexcept {
    const float left = 72.0f + static_cast<float>(g_app.channel_pane_width);
    const float right = client_width_dip() - static_cast<float>(g_app.member_pane_width);
    return (left + right) * 0.5f;
}
float member_x() noexcept {
    return client_width_dip() - static_cast<float>(g_app.member_pane_width) * 0.5f;
}

bool call_control(PerfAccountCommand command, u32 video_fps) noexcept {
    if (!g_probe.control)
        return false;
    PerfAccountControl control{};
    control.command = command;
    control.channel_id = perf_lab_voice_channel(0);
    control.guild_id = g_probe.lab_guild;
    control.video_fps = video_fps;
    return g_probe.control(&control);
}

constexpr u32 kAnimationChannelIndex = 2;
constexpr u32 kAnimationScrollNotches = 16;

} // namespace

namespace perf_probe_detail {

bool due(u64 interval_ms) noexcept {
    const u64 now = qpc_now();
    if (now < g_probe.next_action_qpc)
        return false;
    g_probe.next_action_qpc = now + interval_ms * g_probe.qpc_frequency / (1000 * kPerfScenarioSpeed);
    ++g_probe.actions;
    return true;
}

} // namespace perf_probe_detail

namespace {

// READY's state, not a startup shell drawn before it.
bool shell_ready() noexcept {
    return g_app.account_ui && !g_app.login.visible && !g_app.startup_shell_provisional &&
           g_app.account_ui->chat_state.guild_count >= kPerfGuilds;
}

} // namespace

namespace perf_probe_detail {

MemorySnapshot* g_snapshots[3] = {};

} // namespace perf_probe_detail

namespace {

constexpr u32 kRangeCapacity = 4096;

} // namespace

namespace perf_probe_detail {

Range* g_heap_ranges = nullptr;
u32 g_heap_range_count = 0;
u8* g_heap_range_heap = nullptr; // HeapTotals index of each heap range
Range* g_stack_ranges = nullptr;
u32 g_stack_range_count = 0;
Range g_probe_ranges[12];
u32 g_probe_range_count = 0;

u32 range_index(const Range* ranges, u32 count, u64 address) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (address >= ranges[i].begin && address < ranges[i].end)
            return i;
    return ~0u;
}
bool in_ranges(const Range* ranges, u32 count, u64 address) noexcept {
    return range_index(ranges, count, address) != ~0u;
}

void collect_heaps(MemorySnapshot& snapshot) noexcept {
    g_heap_range_count = 0;
    HANDLE heaps[128];
    const DWORD count = GetProcessHeaps(128, heaps);
    snapshot.heap_count = count;
    for (DWORD h = 0; h < count && h < 128; ++h) {
        if (!HeapLock(heaps[h]))
            continue;
        const u32 detail = h < kHeapDetailCapacity ? h : kHeapDetailCapacity - 1;
        HeapTotals& totals = snapshot.heaps[detail];
        if (h < kHeapDetailCapacity) {
            totals.handle = reinterpret_cast<u64>(heaps[h]);
            totals.creator = heaps[h] == GetProcessHeap() ? kProcessHeapCreator : owners_heap_creator(heaps[h]);
            snapshot.heap_detail_count = h + 1;
        }
        const u32 first_range = g_heap_range_count;
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heaps[h], &entry)) {
            if (entry.wFlags & PROCESS_HEAP_REGION) {
                if (g_heap_range_count < kRangeCapacity) {
                    const u64 begin = reinterpret_cast<u64>(entry.Region.lpFirstBlock);
                    g_heap_range_heap[g_heap_range_count] = static_cast<u8>(detail);
                    g_heap_ranges[g_heap_range_count++] = {begin & ~0xFFFFull,
                                                           reinterpret_cast<u64>(entry.Region.lpLastBlock)};
                }
                snapshot.heap_committed += entry.Region.dwCommittedSize;
                totals.committed += entry.Region.dwCommittedSize;
            } else if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                snapshot.heap_busy += entry.cbData;
                ++snapshot.heap_blocks;
                totals.busy += entry.cbData;
                ++totals.blocks;
                // Blocks above the heap's virtual-memory threshold get their own
                // allocation outside every region; count them as heap memory too.
                const u64 begin = reinterpret_cast<u64>(entry.lpData);
                if (entry.cbData >= 64 * 1024 &&
                    range_index(g_heap_ranges + first_range, g_heap_range_count - first_range, begin) == ~0u) {
                    totals.large += entry.cbData;
                    if (g_heap_range_count < kRangeCapacity) {
                        g_heap_range_heap[g_heap_range_count] = static_cast<u8>(detail);
                        g_heap_ranges[g_heap_range_count++] = {begin & ~0xFFFFull, begin + entry.cbData};
                    }
                }
            }
        }
        HeapUnlock(heaps[h]);
    }
}

void collect_stacks(MemorySnapshot& snapshot) noexcept {
    g_stack_range_count = 0;
    if (!g_query_system || !g_query_thread)
        return;
    ULONG needed = 0;
    if (g_query_system(5, g_spi_buffer, kSpiBytes, &needed) < 0)
        return;
    const u64 self = GetCurrentProcessId();
    for (u8* at = g_spi_buffer;;) {
        const auto* process = reinterpret_cast<const SpiProcess*>(at);
        if (reinterpret_cast<u64>(process->process_id) == self) {
            const auto* threads = reinterpret_cast<const SpiThread*>(process + 1);
            for (u32 t = 0; t < process->threads; ++t) {
                HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE,
                                           static_cast<DWORD>(reinterpret_cast<u64>(threads[t].thread_id)));
                if (!thread)
                    continue;
                struct {
                    LONG exit_status;
                    PVOID teb;
                    NtClientId client;
                    ULONG_PTR affinity;
                    LONG priority, base_priority;
                } basic{};
                if (g_query_thread(thread, 0, &basic, sizeof(basic), nullptr) >= 0 && basic.teb) {
                    const auto* tib = static_cast<const NT_TIB*>(basic.teb);
                    const u64 base = reinterpret_cast<u64>(tib->StackBase);
                    const u64 deallocation = *reinterpret_cast<const u64*>(static_cast<const u8*>(basic.teb) + 0x1478);
                    if (base > deallocation && g_stack_range_count < kRangeCapacity) {
                        g_stack_ranges[g_stack_range_count++] = {deallocation, base};
                        snapshot.stack_reserved += base - deallocation;
                        ++snapshot.stack_threads;
                    }
                }
                CloseHandle(thread);
            }
            break;
        }
        if (!process->next)
            break;
        at += process->next;
    }
}

u32 module_index(MemorySnapshot& snapshot, u64 allocation_base, MemoryCategory* category) noexcept {
    for (u32 i = 0; i < snapshot.module_count; ++i)
        if (snapshot.modules[i].base == allocation_base) {
            const wchar_t* name = snapshot.modules[i].name;
            *category = cat_image_system;
            auto has = [&](const wchar_t* needle) noexcept {
                for (const wchar_t* at = name; *at; ++at) {
                    u32 k = 0;
                    while (needle[k] && at[k] && ((at[k] | 32) == (needle[k] | 32)))
                        ++k;
                    if (!needle[k])
                        return true;
                }
                return false;
            };
            if (allocation_base == g_image_base)
                *category = cat_image_exe;
            else if (has(L"MaiCordPerfAccount"))
                *category = cat_image_synthetic;
            else if (has(L"d3d") || has(L"dxgi") || has(L"dcomp") || has(L"nvw") || has(L"nvl") ||
                     has(L"amd") || has(L"atidx") || has(L"igd") || has(L"igc") || has(L"intel") ||
                     has(L"dxcore") || has(L"gdi32full") || has(L"nvapi") || has(L"nvgpu") || has(L"ati"))
                *category = cat_image_graphics;
            return i;
        }
    if (snapshot.module_count == kModuleCapacity)
        return ~0u;
    ModuleTotals& module = snapshot.modules[snapshot.module_count];
    module = {};
    module.base = allocation_base;
    wchar_t path[MAX_PATH]{};
    if (!K32GetModuleFileNameExW(GetCurrentProcess(), reinterpret_cast<HMODULE>(allocation_base), path, MAX_PATH))
        K32GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(allocation_base), path, MAX_PATH);
    const wchar_t* file = path;
    for (const wchar_t* at = path; *at; ++at)
        if (*at == L'\\' || *at == L'/')
            file = at + 1;
    for (u32 i = 0; i < 47 && file[i]; ++i)
        module.name[i] = file[i];
    ++snapshot.module_count;
    return module_index(snapshot, allocation_base, category);
}

} // namespace perf_probe_detail

namespace {

void write_stdout(const char* text, u32 bytes) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, bytes, &written, nullptr);
}

bool profile_isolated() noexcept {
    wchar_t local[1024]{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 1000);
    if (!count || count >= 1000)
        return false;
    constexpr wchar_t marker[] = L"\\.maicord-test-profile";
    for (u32 i = 0; marker[i]; ++i)
        local[count + i] = marker[i];
    const DWORD attributes = GetFileAttributesW(local);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

bool probe_initialize() noexcept {
    // Never touch a real profile: the build runner's temporary profile marks itself.
    if (!profile_isolated()) {
        constexpr char text[] = "perf probe: LOCALAPPDATA is not an isolated test profile\n";
        write_stdout(text, sizeof(text) - 1);
        return false;
    }
    g_virtual_alloc = VirtualAlloc;
    g_virtual_free = VirtualFree;
    g_heap_alloc = HeapAlloc;
    g_heap_realloc = HeapReAlloc;
    g_heap_free = HeapFree;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_probe.qpc_frequency = static_cast<u64>(frequency.QuadPart);
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    g_probe.process_create_100ns = (static_cast<u64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    g_main_thread_id = GetCurrentThreadId();
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_query_system = reinterpret_cast<NtQuerySystemInformationFunction>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    g_query_thread = reinterpret_cast<NtQueryInformationThreadFunction>(GetProcAddress(ntdll, "NtQueryInformationThread"));
    // Probe-owned buffers (excluded from client attribution).
    struct Buffer {
        void** target;
        u64 bytes;
    };
    Buffer buffers[] = {
        {reinterpret_cast<void**>(&g_spi_buffer), kSpiBytes},
        {reinterpret_cast<void**>(&g_tracked), sizeof(TrackedRegion) * u64{kTrackedCapacity}},
        {reinterpret_cast<void**>(&g_frame_cycles), sizeof(u64) * u64{kFrameSampleCapacity}},
        {reinterpret_cast<void**>(&g_heap_ranges), sizeof(Range) * u64{kRangeCapacity} * 2},
        {reinterpret_cast<void**>(&g_snapshots[0]), sizeof(MemorySnapshot) * 3},
        {reinterpret_cast<void**>(&g_heap_range_heap), u64{kRangeCapacity}},
    };
    for (Buffer& buffer : buffers) {
        *buffer.target = probe_alloc(buffer.bytes);
        if (!*buffer.target)
            return false;
        const u64 begin = reinterpret_cast<u64>(*buffer.target);
        g_probe_ranges[g_probe_range_count++] = {begin, begin + ((buffer.bytes + 65535) & ~65535ull)};
    }
    g_stack_ranges = g_heap_ranges + kRangeCapacity;
    g_snapshots[1] = g_snapshots[0] + 1;
    g_snapshots[2] = g_snapshots[0] + 2;
    {
        u64 begin = 0, end = 0;
        startup_probe_range(&begin, &end);
        if (end)
            g_probe_ranges[g_probe_range_count++] = {begin, end};
        wchar_t owners_flag[2]{};
        if (GetEnvironmentVariableW(L"MAICORD_PERF_OWNERS", owners_flag, 2) == 1 && owners_flag[0] == L'1') {
            if (!owners_initialize(probe_alloc, &begin, &end))
                return false;
            g_probe_ranges[g_probe_range_count++] = {begin, end};
        }
    }
    trace("buffers");
    g_probe.hooks = install_import_hooks();
    trace("hooks");
    wchar_t profile_flag[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_PERF_PROFILE", profile_flag, 2) == 1 && profile_flag[0] == L'1') {
        const u64 bytes = sizeof(PhaseProfile) * static_cast<u64>(Phase::count);
        g_profiles = static_cast<PhaseProfile*>(probe_alloc(bytes));
        if (g_profiles && DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                                          &g_main_thread, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                          THREAD_QUERY_INFORMATION, FALSE, 0)) {
            const u64 begin = reinterpret_cast<u64>(g_profiles);
            g_probe_ranges[g_probe_range_count++] = {begin, begin + ((bytes + 65535) & ~65535ull)};
            profile_refresh_modules();
            g_sampler = CreateThread(nullptr, 64 * 1024, sampler_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                     nullptr);
            if (g_sampler)
                SetThreadPriority(g_sampler, THREAD_PRIORITY_TIME_CRITICAL);
            trace("profiler");
        }
    }
    // The synthetic account starts only when a saved profile file exists.
    AccountPaths paths{};
    if (!account_paths_resolve(&paths).ok())
        return false;
    CreateDirectoryW(paths.application_directory, nullptr);
    CreateDirectoryW(paths.account_directory, nullptr);
    HANDLE profile = CreateFileW(paths.profile, GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (profile == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(profile);
    g_probe.lab_guild = perf_guild_id(0);
    g_probe.media_channel = perf_lab_text_channel(0);
    g_probe.text_channel = perf_lab_text_channel(1);
    g_tick_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (g_tick_timer)
        g_tick_thread = CreateThread(nullptr, 64 * 1024, probe_ticker, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                     nullptr);
    begin_phase(Phase::startup);
    g_probe.initialized = true;
    return true;
}

// Every account deadline the scheduler folds into its next wake.
void dump_account_deadlines() noexcept {
    using namespace app_detail;
    const u64 now_ms = clock_monotonic_milliseconds();
    auto relative = [now_ms](u64 at) noexcept -> u64 { return at ? (at > now_ms ? at - now_ms : 0) + 1 : 0; };
    if (!g_app.account_ui)
        return;
    static Out line;
    line.used = 0;
    line.s("trace deadlines");
    line.kv("profile_next_wake", relative(application_profile_next_wake()));
    line.kv("commands_next_wake", relative(application_commands_next_wake()));
    line.kv("friends_next_wake", relative(application_friends_next_wake()));
    line.kv("forum_preview_next_wake", relative(application_forum_preview_next_wake()));
    line.kv("composer_preview_next_wake", relative(application_composer_preview_next_wake()));
    line.kv("composer_spellcheck_next_wake_composer_s", relative(composer_spellcheck_next_wake(g_app.composer_spellcheck)));
    line.kv("search_next_wake", relative(application_search_next_wake()));
    line.kv("pinned_messages_next_wake", relative(application_pinned_messages_next_wake()));
    line.kv("thread_browser_next_wake", relative(application_thread_browser_next_wake()));
    line.kv("thread_creation_next_wake", relative(application_thread_creation_next_wake()));
    line.kv("forum_post_next_wake", relative(application_forum_post_next_wake()));
    line.kv("poll_next_wake", relative(application_poll_next_wake()));
    line.kv("member_moderation_next_wake", relative(application_member_moderation_next_wake()));
    line.kv("account_settings_next_wake", relative(application_account_settings_next_wake()));
    line.kv("media_picker_next_wake", relative(application_media_picker_next_wake()));
    line.kv("gif_favorites_next_wake", relative(application_gif_favorites_next_wake()));
    line.kv("emoji_preferences_next_wake", relative(application_emoji_preferences_next_wake()));
    line.kv("action_captcha_next_wake", relative(application_action_captcha_next_wake()));
    line.kv("self_status_next_wake", relative(application_self_status_next_wake()));
    line.kv("user_settings_next_wake", relative(application_user_settings_next_wake()));
    line.kv("guild_folders_request_wake", relative(application_guild_folders_request_wake()));
    line.kv("custom_status_next_wake", relative(application_custom_status_next_wake()));
    line.kv("activity_next_wake", relative(application_activity_next_wake()));
    line.kv("notification_controls_next_wake", relative(application_notification_controls_next_wake()));
    line.kv("inbox_next_wake", relative(application_inbox_next_wake()));
    line.kv("stream_preview_next_wake", relative(application_stream_preview_next_wake()));
    line.kv("stage_next_wake_details_request_owner_Me", relative(application_stage_next_wake(g_app.details_request_owner == MessageHistoryRequestOwner::none)));
    line.kv("soundboard_next_wake_details_request_own", relative(application_soundboard_next_wake(g_app.details_request_owner == MessageHistoryRequestOwner::none)));
    line.kv("message_components_next_wake", relative(application_message_components_next_wake()));
    line.kv("invite_cards_next_wake", relative(application_invite_cards_next_wake()));
    line.kv("channel_overview_next_wake", relative(application_channel_overview_next_wake()));
    line.kv("read_state_next_wake", relative(application_read_state_next_wake()));
    line.kv("scoped_read_next_wake", relative(application_scoped_read_next_wake()));
    line.kv("notifications_next_wake_g_app", relative(application_notifications_next_wake(&g_app)));
    line.kv("voice_next_wake", relative(application_voice_next_wake()));
    line.kv("presence_next_wake", relative(application_presence_next_wake()));
    line.kv("media_url_next_wake_media_urls", relative(media_url_next_wake(g_app.media_urls)));
    line.kv("account_text_retry_wake", relative(application_account_text_retry_wake()));
    line.kv("private_conversation_retry", relative(g_app.account_ui->private_conversation.retry_at_ms));
    line.kv("chat_state_restore_deadline_ms", relative(g_app.account_ui->chat_state.restore_deadline_ms));
    line.kv("message_draft_session_persist_at_ms", relative(g_app.account_ui->message_draft_session.persist_at_ms));
    line.kv("message_composer_controller_next_wake_at", relative(message_composer_controller_next_wake_at( g_app.account_ui->message_composer_controller)));
    line.kv("typing_store_next_wake_at_typing_store", relative(typing_store_next_wake_at(g_app.account_ui->typing_store)));
    line.kv("message_catchup_retry_at_ms", relative(g_app.message_catchup.retry_at_ms));
    line.kv("message_older_retry_at_ms", relative(g_app.message_older.retry_at_ms));
    line.kv("attachment_refresh_retry_at_ms", relative(g_app.attachment_refresh.retry_at_ms));
    line.kv("reply_refresh_retry_at_ms", relative(g_app.reply_refresh.retry_at_ms));
    line.kv("message_delete_retry_at_ms", relative(g_app.message_delete.retry_at_ms));
    line.kv("message_edit_retry_at_ms", relative(g_app.message_edit.retry_at_ms));
    line.kv("message_reaction_next_wake", relative(application_message_reaction_next_wake()));
    line.kv("thread_actions_next_wake", relative(application_thread_actions_next_wake()));
    line.kv("channel_prefetch_next_wake", relative(application_channel_prefetch_next_wake()));
    line.kv("member_list_subscription_retry_at_ms", relative(g_app.member_list_subscription.retry_at_ms));
    line.kv("author_hydration_next_wake", relative(application_author_hydration_next_wake()));
    line.kv("thread_member_list_subscription_retry_at", relative(g_app.thread_member_list_subscription.retry_at_ms));
    line.kv("reaction_viewer_controller_next_wake_at", relative(reaction_viewer_controller_next_wake_at(g_app.reaction_viewer_controller)));
    line.kv("message_media_retry_at_ms", relative(g_app.message_media.retry_at_ms));
    line.s("\n");
    write_stdout(line.text, line.used);
}

// Names the deadline that keeps the loop polling when a phase spins.
void dump_spin_state() noexcept {
    const u64 now_ms = clock_monotonic_milliseconds();
    const u64 now_us = clock_microseconds(g_app.clock, g_app.process_start, clock_now());
    auto relative = [now_ms](u64 at) noexcept -> u64 { return at ? (at > now_ms ? at - now_ms : 0) + 1 : 0; };
    static Out line;
    line.used = 0;
    line.s("trace spin");
    line.kv("frame_pending", g_app.frame_pending);
    line.kv("scan_pending", g_app.message_media.scan_pending);
    line.kv("media_retry", relative(g_app.message_media.retry_at_ms));
    line.kv("preview_retry", relative(g_app.message_media.preview_retry_at_ms));
    line.kv("account_wake", relative(g_app.account.status.next_wake_at_ms));
    line.kv("media_trim", relative(renderer_media_next_trim_ms(g_app.renderer)));
    line.kv("timeline_ms", animation_timeline_wait_timeout_ms(&g_app.animations.timeline, now_us));
    line.kv("text_effect", g_app.renderer.text_effect_animated);
    line.kv("text_effect_next", relative(g_app.renderer.text_effect_next_ms));
    line.kv("spinner", relative(app_detail::application_loading_spinner_wake()));
    line.kv("video", relative(app_detail::application_video_next_wake()));
    line.kv("tooltip", relative(app_detail::application_tooltip_next_wake()));
    line.kv("surface", relative(app_detail::application_surface_motion_next_wake()));
    line.kv("background", relative(app_detail::application_background_next_wake(now_ms)));
    line.kv("presence", relative(app_detail::application_presence_next_wake()));
    line.kv("smooth", app_detail::application_smooth_scroll_active());
    line.kv("nonvisual_due", app_detail::application_nonvisual_wake_due(now_ms));
    line.kv("visual_requested", app_detail::application_visual_frame_requested(now_ms, now_us));
    line.kv("permit", g_app.renderer.begin_frame_permit);
    line.kv("rate_next", g_app.frame_rate.next_update_us ? 1 : 0);
    line.s("\n");
    write_stdout(line.text, line.used);
    dump_account_deadlines();
}

void probe_observe_wait(u32 timeout_ms, bool timeout, bool frame_signal, bool message, bool handle) noexcept {
    if (!g_probe.initialized)
        return;
    PhaseStats& stats = g_phases[static_cast<u32>(g_probe.phase)];
    ++stats.wakes;
    if (timeout_ms == 0 && timeout) {
        ++stats.zero_timeouts;
        if (++g_probe.zero_streak == 2'000 && !stats.spin_dumped) {
            stats.spin_dumped = true;
            dump_spin_state();
        }
    } else {
        g_probe.zero_streak = 0;
    }
    if (timeout)
        ++stats.wake_timeouts;
    else if (frame_signal)
        ++stats.wake_frames;
    else if (message)
        ++stats.wake_messages;
    else if (handle)
        ++stats.wake_handles;
}

// The scenario advances only when the client's loop wakes. An idle client
// (no animation, no timers) can sleep indefinitely, so the probe arms one
// timer for its own next deadline (a phase end or the next scripted input)
// and posts a single WM_NULL then; wakes the client makes itself re-arm it
// without extra messages.
u64 phase_deadline_qpc() noexcept {
    const u64 start = g_probe.phase_start_qpc;
    const auto at = [&](u64 ms) noexcept { return start + ms * g_probe.qpc_frequency / (1000 * kPerfScenarioSpeed); };
    const auto action = [&](u32 limit, u64 end_ms) noexcept {
        return g_probe.actions < limit ? g_probe.next_action_qpc : at(end_ms);
    };
    switch (g_probe.phase) {
    case Phase::startup: return at(30'000);
    case Phase::history_wait: return g_probe.history_100ns ? at(1'500) : at(15'000);
    case Phase::idle_static: return at(4'000);
    case Phase::scroll_timeline: return action(90, 5'500);
    case Phase::channel_switch: return action(24, 6'500);
    case Phase::member_list: return action(60, 4'000);
    case Phase::churn: return at(5'000);
    case Phase::settings: {
        const u64 next = g_probe.actions < 6 ? g_probe.next_action_qpc : at(3'800);
        return next > at(600) ? next : at(600);
    }
    case Phase::animation_scroll: return action(kAnimationScrollNotches, 3'000);
    case Phase::idle_animated: return at(10'000);
    case Phase::idle_fx: return at(3'000);
    case Phase::call_open: return at(2'000);
    case Phase::call_video_30: return at(10'000);
    case Phase::call_video_5: return at(10'000);
    case Phase::idle_final: return at(4'000);
    case Phase::idle_hidden: return at(4'000);
    default: return 0;
    }
}

void arm_phase_tick() noexcept {
    const u64 deadline = phase_deadline_qpc();
    if (!g_tick_timer || !deadline)
        return;
    const u64 now = qpc_now();
    // Relative due time in 100 ns units, at least 1 ms past the deadline.
    const u64 delay_us = deadline > now ? (deadline - now) * 1'000'000 / g_probe.qpc_frequency : 0;
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>((delay_us + 1'000) * 10);
    SetWaitableTimer(g_tick_timer, &due, 0, nullptr, nullptr, FALSE);
}

bool probe_step_phases(int* exit_code) noexcept;
bool probe_step(int* exit_code) noexcept {
    const bool finished = probe_step_phases(exit_code);
    if (finished || g_probe.phase == Phase::done) {
        if (g_tick_thread) {
            InterlockedExchange(&g_tick_stop, 1);
            LARGE_INTEGER now{};
            SetWaitableTimer(g_tick_timer, &now, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(g_tick_thread, INFINITE);
            CloseHandle(g_tick_thread);
            g_tick_thread = nullptr;
        }
    } else {
        arm_phase_tick();
    }
    return finished;
}

bool probe_step_phases(int* exit_code) noexcept {
    if (!g_probe.initialized)
        return false;
    ++g_probe.step_counter;
    // Presents since the last turn: record the main-thread cycles spent per frame.
    const u64 presents = static_cast<u64>(g_app.renderer.full_present_draws) + g_app.renderer.partial_present_draws;
    PhaseStats& stats = g_phases[static_cast<u32>(g_probe.phase)];
    if (presents != g_probe.present_counter) {
        ULONG64 cycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cycles);
        cycles -= static_cast<u64>(g_replay.cycles);
        if (g_probe.present_counter == 0 && !g_probe.first_present_100ns)
            g_probe.first_present_100ns = now_100ns();
        if (g_frame_cycle_count < kFrameSampleCapacity && g_probe.last_present_cycles) {
            g_frame_cycles[g_frame_cycle_count++] = cycles - g_probe.last_present_cycles;
            ++stats.frame_count;
        }
        stats.presents += presents - g_probe.present_counter;
        const u64 now_ms = clock_monotonic_milliseconds();
        stats.next_pending += g_app.frame_pending;
        stats.next_timeline += animation_timeline_next_wake_at_us(&g_app.animations.timeline) != 0;
        {
            const auto& timeline = g_app.animations.timeline;
            constexpr u32 kPlaying = animation_timeline_entry_occupied | animation_timeline_entry_visible;
            u32 playing = 0;
            for (u32 i = 0; timeline.entries && i < timeline.capacity; ++i)
                playing += (timeline.entries[i].flags & kPlaying) == kPlaying;
            if (playing > stats.anim_visible_peak)
                stats.anim_visible_peak = playing;
        }
        stats.next_effect += g_app.renderer.text_effect_animated ||
                             profile_scene_animating(g_app.renderer.profile_scene, now_ms);
        stats.next_spinner += app_detail::application_loading_spinner_wake() != 0;
        stats.next_video += app_detail::application_video_next_wake() != 0;
        stats.next_tooltip += app_detail::application_tooltip_next_wake() != 0;
        stats.next_surface += app_detail::application_surface_motion_next_wake() != 0;
        stats.next_background += app_detail::application_background_next_wake(now_ms) != 0;
        stats.next_smooth += app_detail::application_smooth_scroll_active();
        stats.next_caret += g_app.message_composer_caret_blink_at_ms != 0;
        stats.next_interaction += app_detail::application_message_interaction_next_wake(now_ms) != 0;
        stats.next_timestamp +=
            g_app.account_ui && g_app.account_ui->message_text.next_timestamp_update_unix_seconds != 0;
        stats.draw_calls += g_app.renderer.draw_calls_last_frame;
        g_probe.last_present_cycles = cycles;
        g_probe.present_counter = presents;
        if (g_probe.context_hooks && g_app.renderer.has_presented && !g_app.renderer.login_visible) {
            const bool full = g_app.renderer.full_present_draws != g_replay.full_presents;
            g_replay.full_presents = g_app.renderer.full_present_draws;
            gpu_replay(full);
        }
    }
    if (!g_probe.context_hooks && g_app.renderer.graphics.context) {
        // Count context calls; draw streams are replayed by gpu_replay.
        g_probe.context_hooks = install_context_hooks(g_app.renderer.graphics.context, false);
        if (static_cast<void*>(g_app.renderer.graphics.context1) == static_cast<void*>(g_app.renderer.graphics.context))
            g_probe.context_hooks += install_context_hooks(g_app.renderer.graphics.context1, true);
        if (!g_probe.context_hooks)
            g_probe.context_hooks = ~0u;
    }
    if (!g_probe.control && g_app.account.module) {
        trace("module");
        g_probe.control = reinterpret_cast<PerfAccountControlFunction>(
            GetProcAddress(g_app.account.module, kPerfAccountControlExport));
        g_probe.counters = reinterpret_cast<PerfAccountCountersFunction>(
            GetProcAddress(g_app.account.module, kPerfAccountCountersExport));
    }
    const u64 phase_ms = elapsed_ms(g_probe.phase_start_qpc) * kPerfScenarioSpeed;
    constexpr u64 kStartupTimeoutMs = 30'000;
    switch (g_probe.phase) {
    case Phase::startup:
        if (shell_ready() && g_probe.first_present_100ns) {
            g_probe.ready_100ns = now_100ns();
            if (!select_channel(g_probe.lab_guild, g_probe.media_channel))
                fail("select_media_channel");
            next_phase(Phase::history_wait);
        } else if (phase_ms > kStartupTimeoutMs) {
            fail(g_probe.first_present_100ns ? "ready_timeout" : "first_present_timeout");
            next_phase(Phase::report);
        }
        break;
    case Phase::history_wait:
        // History applied and visible media decoded; then a short settling window.
        if (!g_probe.history_100ns && g_app.message_view.store.count >= 20)
            g_probe.history_100ns = now_100ns();
        if (g_probe.history_100ns && phase_ms > 1'500) {
            take_snapshot(0);
            if (!select_channel(g_probe.lab_guild, g_probe.text_channel))
                fail("select_text_channel");
            next_phase(Phase::idle_static);
        } else if (phase_ms > 15'000) {
            fail("history_timeout");
            next_phase(Phase::report);
        }
        break;
    case Phase::idle_static:
        // Text-only channel: nothing animates, nothing arrives.
        if (phase_ms > 4'000) {
            if (!select_channel(g_probe.lab_guild, g_probe.media_channel))
                fail("select_media_channel_scroll");
            next_phase(Phase::scroll_timeline);
        }
        break;
    case Phase::scroll_timeline:
        // 60 notches up (older history loads on the way), then 30 down.
        if (g_probe.actions < 90 && due(45))
            post_wheel(message_x(), client_height_dip() * 0.5f, g_probe.actions <= 60 ? WHEEL_DELTA : -WHEEL_DELTA);
        if (g_probe.actions >= 90 && phase_ms > 5'500) {
            take_snapshot(1);
            next_phase(Phase::channel_switch);
        }
        break;
    case Phase::channel_switch:
        if (g_probe.actions < 24 && due(250)) {
            const u32 index = 2 + g_probe.actions % (kPerfLabCategories * kPerfLabTextPerCategory - 2);
            if (!select_channel(g_probe.lab_guild, perf_lab_text_channel(index)))
                fail("channel_switch");
        }
        if (g_probe.actions >= 24 && phase_ms > 6'500) {
            if (!select_channel(g_probe.lab_guild, g_probe.media_channel))
                fail("select_media_channel_members");
            next_phase(Phase::member_list);
        }
        break;
    case Phase::member_list:
        if (g_probe.actions < 60 && due(50))
            post_wheel(member_x(), client_height_dip() * 0.5f, g_probe.actions < 45 ? -WHEEL_DELTA : WHEEL_DELTA);
        if (g_probe.actions >= 60 && phase_ms > 4'000) {
            next_phase(Phase::churn);
            if (g_probe.control) {
                PerfAccountControl control{};
                control.command = PerfAccountCommand::start_churn;
                control.typing_per_second = 8 * kPerfScenarioSpeed;
                control.presence_per_second = 40 * kPerfScenarioSpeed;
                control.messages_per_second = 2 * kPerfScenarioSpeed;
                control.channel_id = g_probe.media_channel;
                control.guild_id = g_probe.lab_guild;
                if (!g_probe.control(&control))
                    fail("start_churn");
            }
        }
        break;
    case Phase::churn:
        if (phase_ms > 5'000) {
            if (g_probe.control) {
                PerfAccountControl control{};
                control.command = PerfAccountCommand::stop_churn;
                g_probe.control(&control);
            }
            next_phase(Phase::settings);
            if (!app_detail::application_settings_open())
                fail("settings_open");
        }
        break;
    case Phase::settings: {
        static const char* const pages[] = {"appearance", "notifications", "accessibility", "voice", "keybinds",
                                            "content.dms"};
        if (g_probe.actions < 6 && phase_ms > 600 && due(450)) {
            const char* key = pages[g_probe.actions - 1];
            u32 bytes = 0;
            while (key[bytes])
                ++bytes;
            app_detail::application_settings_open_key({key, bytes});
        }
        if (g_probe.actions >= 6 && phase_ms > 3'800) {
            app_detail::application_settings_close(false);
            if (!select_channel(g_probe.lab_guild, perf_lab_text_channel(kAnimationChannelIndex)))
                fail("select_animation_channel");
            next_phase(Phase::animation_scroll);
        }
        break;
    }
    case Phase::animation_scroll:
        // A mixed channel without churn or typing: its newest messages are
        // static, so scroll back to the GIFs and animated emoji.
        if (g_probe.actions < kAnimationScrollNotches && due(45))
            post_wheel(message_x(), client_height_dip() * 0.5f, WHEEL_DELTA);
        if (g_probe.actions >= kAnimationScrollNotches && phase_ms > 3'000)
            next_phase(Phase::idle_animated);
        break;
    case Phase::idle_animated:
        // Two playing animations, nothing else: the steady animation cost.
        // Animation frame rates are not scaled, so it runs a full second.
        if (phase_ms > 10'000) {
            if (!select_channel(g_probe.lab_guild, g_probe.text_channel))
                fail("select_text_channel_final");
            // The text channel under the built-in Stars effect: the steady
            // cost of an animated backdrop (shell and title bar).
            g_probe.gradient = g_app.preferences.gradient;
            const auto stars = background_effect_settings_defaults(BackgroundEffect::stars);
            g_app.preferences.gradient.effect = BackgroundEffect::stars;
            g_app.preferences.gradient.speed = stars.speed;
            g_app.preferences.gradient.strength = stars.strength;
            g_app.preferences.gradient.density = stars.density;
            app_detail::application_request_frame();
            next_phase(Phase::idle_fx);
        }
        break;
    case Phase::idle_fx:
        if (phase_ms > 3'000) {
            g_app.preferences.gradient = g_probe.gradient;
            app_detail::application_request_frame();
            collect_threads(true);
            // A call with two cameras in the content column.
            if (!call_control(PerfAccountCommand::start_call, 30) ||
                !app_detail::application_call_open(perf_lab_voice_channel(0), 0))
                fail("call_open");
            next_phase(Phase::call_open);
        }
        break;
    case Phase::call_open:
        if (phase_ms > 2'000)
            next_phase(Phase::call_video_30);
        break;
    // Steady call video at camera rates. Picture rates are not scaled, so
    // each runs a full second.
    case Phase::call_video_30:
        if (phase_ms > 10'000) {
            if (!call_control(PerfAccountCommand::start_call, 5))
                fail("call_rate");
            next_phase(Phase::call_video_5);
        }
        break;
    case Phase::call_video_5:
        if (phase_ms > 10'000) {
            call_control(PerfAccountCommand::stop_call, 0);
            app_detail::application_call_close();
            next_phase(Phase::idle_final);
        }
        break;
    case Phase::idle_final:
        if (phase_ms > 4'000) {
            // End-of-session memory before minimizing (which may trim the working set).
            end_phase();
            RECT client{};
            GetClientRect(g_app.window, &client);
            g_probe.client_width = static_cast<u32>(client.right);
            g_probe.client_height = static_cast<u32>(client.bottom);
            take_snapshot(2);
            collect_threads(false);
            report_account_ui_fields();
            begin_phase(Phase::idle_hidden);
            ShowWindow(g_app.window, SW_SHOWMINNOACTIVE);
            // A minimized client may not wake for many seconds; the phase
            // tick posts one message at its end (counted as one message wake).
        }
        break;
    case Phase::idle_hidden:
        if (phase_ms > 4'000)
            next_phase(Phase::report);
        break;
    case Phase::report: {
        end_phase();
        if (g_sampler) {
            InterlockedExchange(&g_sampler_stop, 1);
            WaitForSingleObject(g_sampler, INFINITE);
            CloseHandle(g_sampler);
            g_sampler = nullptr;
        }
        g_probe.final_messages = g_app.message_view.store.count;
        if (g_app.account_ui) {
            g_probe.final_guilds = g_app.account_ui->chat_state.guild_count;
            g_probe.final_channels = g_app.account_ui->chat_state.channel_count;
            g_probe.final_members = g_app.account_ui->member_list.count;
        }
        if (g_probe.counters)
            g_probe.counters(&g_probe.module_counters);
        g_probe.phase = Phase::done;
        *exit_code = g_probe.failed ? 90 : 0;
        return true;
    }
    default:
        return true;
    }
    return false;
}

void probe_finish(int exit_code) noexcept {
    if (!g_probe.initialized)
        return;
    calibrate_cycles();
    Out& o = g_out;
    o.s("MAICORD_PERF v1");
    o.kv("exit", static_cast<u64>(exit_code));
    o.kv("hooks", g_probe.hooks);
    o.kv("context_hooks", g_probe.context_hooks);
    o.kv("dpi", g_app.renderer.dpi);
    o.kv("width", g_probe.client_width);
    o.kv("height", g_probe.client_height);
    o.kv("messages", g_probe.final_messages);
    o.kv("guilds", g_probe.final_guilds);
    o.kv("channels", g_probe.final_channels);
    o.kv("members", g_probe.final_members);
    o.kvf("tsc_mhz", cycles_per_us);
    if (g_probe.failed) {
        o.s(" failure=");
        o.s(g_probe.failure);
    }
    o.s("\n");
    const auto ms_since_create = [](u64 at) noexcept -> u64 {
        return at > g_probe.process_create_100ns ? (at - g_probe.process_create_100ns) / 10'000 : 0;
    };
    o.s("startup");
    o.kv("first_present_ms", ms_since_create(g_probe.first_present_100ns));
    o.kv("ready_ms", ms_since_create(g_probe.ready_100ns));
    o.kv("history_ms", ms_since_create(g_probe.history_100ns));
    o.s("\n");
    for (u32 p = 0; p < static_cast<u32>(Phase::report); ++p)
        report_phase(static_cast<Phase>(p));
    for (u32 s = 0; s < 3; ++s)
        report_snapshot(s);
    if (g_snapshots[2] && g_snapshots[2]->valid) {
        report_modules(*g_snapshots[2]);
        report_sites(*g_snapshots[2]);
    }
    report_threads();
    report_account_ui_fields_text();
    for (u32 p = 0; p < static_cast<u32>(Phase::report); ++p)
        report_profile(static_cast<Phase>(p));
    const PerfAccountCounters& m = g_probe.module_counters;
    o.s("synthetic");
    o.kv("ready_json_kb", m.ready_json_bytes / 1024);
    o.kv("ready_events", m.ready_events);
    o.kv("gateway_events", m.gateway_events);
    o.kv("gateway_payload_kb", m.gateway_payload_bytes / 1024);
    o.kv("rest_responses", m.rest_responses);
    o.kv("history_messages", m.history_messages);
    o.kv("media_frames", m.media_frames);
    o.kv("media_pixel_kb", m.media_pixel_bytes / 1024);
    o.kv("member_syncs", m.member_list_syncs);
    o.kv("stub_calls", m.stub_calls);
    o.kv("last_stub", m.last_stub_slot);
    o.kv("module_commit_kb", m.module_committed_bytes / 1024);
    o.s("\n");
    o.s("alloc_total");
    o.kv("va_calls", static_cast<u64>(g_alloc.virtual_alloc_calls));
    o.kv("va_commit_kb", static_cast<u64>(g_alloc.virtual_commit_bytes) / 1024);
    o.kv("va_commit_waste_kb", static_cast<u64>(g_alloc.virtual_commit_waste) / 1024);
    o.kv("va_reserve_kb", static_cast<u64>(g_alloc.virtual_reserve_bytes) / 1024);
    o.kv("vf_calls", static_cast<u64>(g_alloc.virtual_free_calls));
    o.kv("heap_calls", static_cast<u64>(g_alloc.heap_alloc_calls + g_alloc.heap_realloc_calls));
    o.kv("heap_kb", static_cast<u64>(g_alloc.heap_alloc_bytes) / 1024);
    o.kv("tracked_overflow", g_tracked_overflow);
    o.s("\nEND\n");
    write_stdout(o.text, o.used);
}

} // namespace maicord::perf
