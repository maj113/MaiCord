#pragma once

#include "perf_probe.hpp"
#include "perf_owners.hpp"
#include "perf_startup.hpp"
#include "../../src/app/account/account_text_retry.hpp"
#include "maicord/core/win32_image.hpp"

namespace maicord::perf {
namespace perf_probe_detail {

using app_detail::g_app;

struct Out {
    char text[256 * 1024];
    u32 used;
    void s(const char* value) noexcept {
        while (*value && used + 1 < sizeof(text))
            text[used++] = *value++;
    }
    void n(u64 value) noexcept {
        char digits[24];
        u32 count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (count && used + 1 < sizeof(text))
            text[used++] = digits[--count];
    }
    // Fixed point with one decimal.
    void f1(double value) noexcept {
        if (value < 0)
            value = 0;
        const u64 scaled = static_cast<u64>(value * 10.0 + 0.5);
        n(scaled / 10);
        s(".");
        n(scaled % 10);
    }
    void kv(const char* key, u64 value) noexcept {
        s(" ");
        s(key);
        s("=");
        n(value);
    }
    void kvf(const char* key, double value) noexcept {
        s(" ");
        s(key);
        s("=");
        f1(value);
    }
    void hex(u64 value) noexcept {
        char digits[17];
        u32 count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value & 15];
            value >>= 4;
        } while (value);
        s("0x");
        while (count && used + 1 < sizeof(text))
            text[used++] = digits[--count];
    }
    void wide(const wchar_t* value) noexcept {
        while (*value && used + 1 < sizeof(text)) {
            const wchar_t c = *value++;
            text[used++] = c < 128 && c > 32 ? static_cast<char>(c) : '_';
        }
    }
};

extern Out g_out;

using VirtualAllocFunction = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
using VirtualFreeFunction = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
using HeapAllocFunction = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapReAllocFunction = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
using HeapFreeFunction = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID);

extern VirtualAllocFunction g_virtual_alloc;

extern VirtualFreeFunction g_virtual_free;

extern HeapAllocFunction g_heap_alloc;

extern HeapReAllocFunction g_heap_realloc;

extern HeapFreeFunction g_heap_free;

void* probe_alloc(u64 bytes) noexcept;

struct AllocCounters {
    volatile LONG64 virtual_alloc_calls;
    volatile LONG64 virtual_commit_bytes;
    volatile LONG64 virtual_reserve_bytes;
    volatile LONG64 virtual_commit_waste;
    volatile LONG64 virtual_free_calls;
    volatile LONG64 virtual_release_calls;
    volatile LONG64 heap_alloc_calls;
    volatile LONG64 heap_alloc_bytes;
    volatile LONG64 heap_free_calls;
    volatile LONG64 heap_realloc_calls;
};

extern AllocCounters g_alloc;

// Immediate-context calls counted by patching the context's vtable (probe
// image only; slot indices follow the d3d11.h/d3d11_1.h vtable order). Media
// decoder threads that share the device context are counted too. Draw and
// UpdateSubresource are not hooked: the runtime rewrites those slots itself.
// Scissors that repeat the previous rectangle are counted separately.
struct GpuApiCounters {
    volatile LONG64 shader_binds, resource_binds, sampler_binds, constant_binds, input_binds, output_binds,
        raster_binds, scissors, repeated_scissors, maps, map_discards, map_no_overwrites, copies, copy_pixels,
        dispatches, dispatch_groups, compute_binds, clears, flushes;
};

extern GpuApiCounters g_gpu_api;

u32 install_context_hooks(void* context, bool context1) noexcept;

// Draw-stream replay. After each present the probe draws the same retained
// stream again, with the presented damage, into an offscreen target of the back
// buffer's size and format, so the main-thread cost of submitting one draw
// stream is measured apart from `Present` and its wait for the back buffer.
// Nothing is uploaded; the replay's main-thread cycles are excluded from the
// CPU columns.
struct GpuReplay {
    ID3D11Texture2D* texture;
    ID3D11RenderTargetView* target;
    u32 width, height;
    u64 full_presents;
    volatile LONG64 cycles; // main-thread cycles spent replaying
    u64 full_replays, partial_replays, partial_pixels, skipped, failures, replays;
};

extern GpuReplay g_replay;

void gpu_replay(bool full) noexcept;

constexpr u32 kSiteFrames = 4;
struct TrackedRegion {
    u64 base;
    u64 bytes;
    u64 frames[kSiteFrames];
};
constexpr u32 kTrackedCapacity = 32'768;

extern TrackedRegion* g_tracked;

extern u32 g_tracked_count;

extern u32 g_tracked_overflow;

extern SRWLOCK g_tracked_lock;

extern u64 g_image_base;

u32 tracked_lower_bound(u64 base) noexcept;

u32 install_import_hooks() noexcept;

struct NtUnicodeString {
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
};
struct NtClientId {
    HANDLE process;
    HANDLE thread;
};
struct SpiProcess {
    ULONG next;
    ULONG threads;
    LARGE_INTEGER private_working_set;
    ULONG hard_faults;
    ULONG thread_high_watermark;
    ULONGLONG cycle_time;
    LARGE_INTEGER create_time, user_time, kernel_time;
    NtUnicodeString image_name;
    LONG base_priority;
    HANDLE process_id;
    HANDLE parent_id;
    ULONG handle_count;
    ULONG session_id;
    ULONG_PTR key;
    SIZE_T peak_virtual, virtual_size;
    ULONG page_faults;
    SIZE_T peak_working_set, working_set;
    SIZE_T quota_peak_paged, quota_paged, quota_peak_nonpaged, quota_nonpaged;
    SIZE_T pagefile, peak_pagefile, private_pages;
    LARGE_INTEGER io[6];
};

struct SpiThread {
    LARGE_INTEGER kernel_time, user_time, create_time;
    ULONG wait_time;
    PVOID start_address;
    HANDLE process_id;
    HANDLE thread_id;
    LONG priority;
    LONG base_priority;
    ULONG context_switches;
    ULONG state;
    ULONG wait_reason;
};

using NtQuerySystemInformationFunction = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationThreadFunction = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

extern NtQuerySystemInformationFunction g_query_system;

extern NtQueryInformationThreadFunction g_query_thread;

extern u8* g_spi_buffer;

constexpr u32 kSpiBytes = 8 * 1024 * 1024;

struct ProcessSample {
    u64 qpc;
    u64 main_cycles;
    u64 process_cycles;
    u64 user_100ns, kernel_100ns;
    u64 context_switches;
    u64 main_context_switches;
    u64 private_working_set;
    u64 private_bytes;
    u64 working_set;
    u64 page_faults;
    u32 threads;
    u32 handles;
    u32 gdi, user;
};

extern DWORD g_main_thread_id;

bool sample_process(ProcessSample* out) noexcept;

// Per-thread owner (Win32 start address), CPU cycles and context switches,
// with the context switches of the final idle phase for idle attribution, and
// the stack reservation and its committed pages (latest sample).
struct ThreadRow {
    u64 id;
    u64 start;
    u64 cycles;
    u32 context_switches;
    u32 idle_baseline;
    u32 idle_switches;
    bool seen_at_idle;
    u64 module_base;
    wchar_t module[40];
    u64 stack_reserved, stack_committed;
};

constexpr u32 kThreadCapacity = 96;

extern ThreadRow g_threads[kThreadCapacity];

extern u32 g_thread_count;

void collect_threads(bool idle_baseline) noexcept;

enum class Phase : u32 {
    startup,
    history_wait,
    idle_static,
    scroll_timeline,
    channel_switch,
    member_list,
    churn,
    settings,
    animation_scroll,
    idle_animated,
    idle_fx,
    call_open,
    call_video_30,
    call_video_5,
    idle_final,
    idle_hidden,
    report,
    done,
    count
};
const char* const kPhaseNames[] = {"startup",     "history",     "idle_static", "scroll",    "switch",
                                   "member_list", "churn",       "settings",    "anim_scroll", "idle_anim",
                                   "idle_fx",     "call_open",   "call_30fps",  "call_5fps",   "idle_final",
                                   "idle_hidden", "report",      "done"};

// Retained text and paint work: parser runs against cache hits, content text
// streams rebuilt (and how many kept the shell's glyphs), paint streams rebuilt,
// GPU instance bytes uploaded, member rows resolved for media and shell
// segments replayed instead of built.
struct TextWork {
    u64 markdown_parses, markdown_hits, text_builds, shell_reuses, paint_builds;
    u64 paint_upload_bytes, text_upload_bytes, media_upload_bytes, member_media_resolves, shell_replays;
};
// GPU work: immediate-context calls, replayed draw streams, renderer paint CPU time and texture traffic.
struct GpuWork {
    GpuApiCounters api;
    u64 full_replays, partial_replays, replay_pixels, replay_failures, replays, replay_cycles, painted,
        partial_pixels, media_upload_bytes, animation_copy_pixels, glyph_pixels, instance_uploads;
    // Media atlas: decoder input bytes uploaded and frames written (every
    // codec path), animation cache restores, video conversions.
    u64 media_frames, animation_restores, video_frames;
};
struct PhaseStats {
    ProcessSample begin, end;
    AllocCounters alloc_begin, alloc_end;
    TextWork text_begin, text_end;
    GpuWork gpu_begin, gpu_end;
    // Animation timeline at the end of the phase: entries, playing entries,
    // and whether presentation was suspended (inactive window, reduced motion).
    u32 anim_entries, anim_visible;
    bool anim_suspended;
    // Timeline frame advances during the phase and the most entries playing at once.
    u64 anim_advances_begin, anim_advances;
    u32 anim_visible_peak;
    u64 wakes, wake_timeouts, wake_frames, wake_messages, wake_handles;
    u64 presents;
    u64 draw_calls;
    u32 frame_count;
    u32 frame_cycles_offset; // into g_frame_cycles
    u64 zero_timeouts;
    // After each present: what keeps requesting frames (deadline scheduled).
    u64 next_pending, next_timeline, next_effect, next_spinner, next_video, next_tooltip, next_surface,
        next_background, next_smooth, next_caret, next_interaction, next_timestamp;
    bool measured;
    bool spin_dumped;
};

extern PhaseStats g_phases[static_cast<u32>(Phase::count)];

extern u64* g_frame_cycles;

struct Probe {
    Phase phase;
    u64 phase_start_qpc;
    u64 qpc_frequency;
    u64 process_create_100ns;
    u64 first_present_100ns;
    u64 ready_100ns;
    u64 history_100ns;
    u64 last_present_cycles;
    u64 present_counter;
    u32 step_counter;
    u32 actions;
    u64 next_action_qpc;
    u64 media_channel;
    u64 text_channel;
    u64 lab_guild;
    bool initialized;
    bool failed;
    const char* failure;
    PerfAccountControlFunction control;
    PerfAccountCountersFunction counters;
    PerfAccountCounters module_counters;
    u32 hooks;
    u32 context_hooks;
    u32 client_width, client_height;
    u32 zero_streak;
    u32 final_messages, final_guilds, final_channels, final_members;
    BackgroundTheme gradient;
};

extern Probe g_probe;

u64 qpc_now() noexcept;

bool due(u64 interval_ms) noexcept;

enum MemoryCategory : u32 {
    cat_app_virtual,
    cat_heap,
    cat_stack,
    cat_synthetic,
    cat_probe,
    cat_write_combined,
    cat_other_private,
    cat_image_exe,
    cat_image_graphics,
    cat_image_system,
    cat_image_synthetic,
    cat_mapped_file,
    cat_mapped_section,
    cat_count
};
const char* const kCategoryNames[] = {"app_virtualalloc", "heaps",         "stacks",       "synthetic_module",
                                      "probe",            "write_combined", "other_private", "image_exe",
                                      "image_graphics",   "image_system",   "image_synthetic", "mapped_files",
                                      "mapped_sections"};

struct CategoryTotals {
    u64 committed, resident, private_resident, regions;
};
struct ModuleTotals {
    u64 base, size, resident, private_resident;
    wchar_t name[48];
};
constexpr u32 kModuleCapacity = 160;
struct SiteTotals {
    u64 frames[kSiteFrames];
    u64 committed, resident, regions;
};
constexpr u32 kSiteCapacity = 2048;
// Per heap (GetProcessHeaps order) and per owning module (perf_owners.hpp).
constexpr u32 kHeapDetailCapacity = 32;
constexpr u32 kProcessHeapCreator = kOwnerNone - 1;
struct HeapTotals {
    u64 handle, committed, resident, busy, blocks, large;
    u32 creator;
};
struct OwnerTotals {
    u64 heap_live, heap_blocks;
    u64 committed[2], resident[2]; // other_private, write_combined regions it reserved
};
// The largest driver-side private regions (other_private, write_combined).
constexpr u32 kRegionDetailCapacity = 16;
struct RegionDetail {
    u64 base, size, resident;
    u32 protect, category, owner;
};
struct MemorySnapshot {
    CategoryTotals categories[cat_count];
    HeapTotals heaps[kHeapDetailCapacity];
    u32 heap_detail_count;
    OwnerTotals owners[kOwnerModuleCapacity];
    RegionDetail regions[kRegionDetailCapacity];
    u32 region_count;
    GpuResourceTotals gpu;
    GpuResourceInfo gpu_largest[20];
    u32 gpu_largest_count;
    u64 owner_entries, owner_dropped, owner_slots;
    ModuleTotals modules[kModuleCapacity];
    u32 module_count;
    SiteTotals sites[kSiteCapacity];
    u32 site_count;
    u64 heap_committed, heap_busy, heap_blocks, heap_count;
    u64 stack_reserved, stack_committed, stack_threads;
    u64 app_regions, app_reserved, app_small_regions;
    u64 gpu_local, gpu_nonlocal;
    bool valid;
};

extern MemorySnapshot* g_snapshots[3];

const char* const kSnapshotNames[] = {"shell", "after_scroll", "end"};

struct Range {
    u64 begin, end;
};

extern Range* g_heap_ranges;

extern u32 g_heap_range_count;

extern u8* g_heap_range_heap;

extern Range* g_stack_ranges;

extern u32 g_stack_range_count;

extern Range g_probe_ranges[12];

extern u32 g_probe_range_count;

u32 range_index(const Range* ranges, u32 count, u64 address) noexcept;

bool in_ranges(const Range* ranges, u32 count, u64 address) noexcept;

void collect_heaps(MemorySnapshot& snapshot) noexcept;

void collect_stacks(MemorySnapshot& snapshot) noexcept;

u32 module_index(MemorySnapshot& snapshot, u64 allocation_base, MemoryCategory* category) noexcept;

bool take_snapshot(u32 index) noexcept;

extern double cycles_per_us;

void calibrate_cycles() noexcept;

void report_phase(Phase phase) noexcept;

void report_snapshot(u32 index) noexcept;

void report_modules(const MemorySnapshot& snapshot) noexcept;

void report_sites(const MemorySnapshot& snapshot) noexcept;

struct ProfileEntry {
    u32 key; // function begin RVA in the probe image, or 0x80000000 | module index
    u32 self;
    u32 inclusive;
    u32 caller_self; // self time of callees in other modules charged here
};
constexpr u32 kProfileEntries = 4096;
struct PhaseProfile {
    ProfileEntry entries[kProfileEntries];
    u32 samples, unattributed, in_syscall;
};

extern PhaseProfile* g_profiles;

extern HANDLE g_sampler;

extern HANDLE g_main_thread;

extern volatile LONG g_sampler_stop;

void profile_refresh_modules() noexcept;

DWORD WINAPI sampler_thread(void*) noexcept;

void report_profile(Phase phase) noexcept;

void report_threads() noexcept;

void report_account_ui_fields_text() noexcept;

void report_account_ui_fields() noexcept;

} // namespace perf_probe_detail

using namespace perf_probe_detail;
} // namespace maicord::perf
