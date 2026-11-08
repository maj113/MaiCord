#include "perf_probe_internal.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/process_info.hpp"
#include "maicord/core/win32_exception.hpp"

namespace maicord::perf {
namespace perf_probe_detail {

bool take_snapshot(u32 index) noexcept {
    MemorySnapshot* snapshot = g_snapshots[index];
    if (!snapshot)
        return false;
    *snapshot = {};
    owners_scan();
    startup_gpu_totals(&snapshot->gpu);
    snapshot->gpu_largest_count = startup_gpu_largest(snapshot->gpu_largest, 20);
    owners_counters(&snapshot->owner_entries, &snapshot->owner_dropped, &snapshot->owner_slots);
    for (u32 m = 0; m < owners_module_count() && m < kOwnerModuleCapacity; ++m) {
        const OwnerModule* owner = owners_module(m);
        snapshot->owners[m].heap_live = static_cast<u64>(owner->heap_live_bytes);
        snapshot->owners[m].heap_blocks = static_cast<u64>(owner->heap_live_blocks);
    }
    collect_heaps(*snapshot);
    collect_stacks(*snapshot);
    // GPU memory as DXGI accounts it for this process.
    if (g_app.renderer.graphics.device) {
        IDXGIDevice* dxgi = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(g_app.renderer.graphics.device->QueryInterface(__uuidof(IDXGIDevice),
                                                                     reinterpret_cast<void**>(&dxgi))) &&
            SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
                snapshot->gpu_local = local.CurrentUsage;
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal)))
                snapshot->gpu_nonlocal = nonlocal.CurrentUsage;
        }
        com_release(adapter3);
        com_release(adapter);
        com_release(dxgi);
    }
    PerfAccountCounters module{};
    if (g_probe.counters)
        g_probe.counters(&module);
    AcquireSRWLockShared(&g_tracked_lock);
    snapshot->app_regions = g_tracked_count;
    for (u32 i = 0; i < g_tracked_count; ++i) {
        snapshot->app_reserved += g_tracked[i].bytes;
        if (g_tracked[i].bytes <= 16 * 1024)
            ++snapshot->app_small_regions;
    }
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const u64 limit = reinterpret_cast<u64>(info.lpMaximumApplicationAddress);
    PSAPI_WORKING_SET_EX_INFORMATION pages[512];
    u64 at = 0;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region)) != sizeof(region))
            break;
        const u64 begin = reinterpret_cast<u64>(region.BaseAddress);
        const u64 end = begin + region.RegionSize;
        if (end <= at)
            break;
        at = end;
        if (region.State != MEM_COMMIT)
            continue;
        MemoryCategory category = cat_other_private;
        u32 heap_range = ~0u;
        u32 module_slot = ~0u;
        i32 site = -1;
        if (region.Type == MEM_IMAGE) {
            module_slot = module_index(*snapshot, reinterpret_cast<u64>(region.AllocationBase), &category);
        } else if (region.Type == MEM_MAPPED) {
            wchar_t path[8];
            category = K32GetMappedFileNameW(GetCurrentProcess(), region.BaseAddress, path, 8) ? cat_mapped_file
                                                                                                 : cat_mapped_section;
        } else {
            const u64 allocation = reinterpret_cast<u64>(region.AllocationBase);
            if (module.module_base && begin >= module.module_base &&
                begin < module.module_base + module.module_reserved_bytes)
                category = cat_synthetic;
            else if (in_ranges(g_probe_ranges, g_probe_range_count, begin))
                category = cat_probe;
            else if (in_ranges(g_stack_ranges, g_stack_range_count, begin))
                category = cat_stack;
            else {
                const u32 tracked = tracked_lower_bound(allocation);
                if (tracked < g_tracked_count && g_tracked[tracked].base == allocation) {
                    category = cat_app_virtual;
                    // Aggregate by allocation call stack.
                    const TrackedRegion& owner = g_tracked[tracked];
                    for (u32 s = 0; s < snapshot->site_count; ++s)
                        if (bytes_equal(snapshot->sites[s].frames, owner.frames, sizeof(owner.frames))) {
                            site = static_cast<i32>(s);
                            break;
                        }
                    if (site < 0 && snapshot->site_count < kSiteCapacity) {
                        site = static_cast<i32>(snapshot->site_count++);
                        copy_bytes(snapshot->sites[site].frames, owner.frames, sizeof(owner.frames));
                    }
                    if (site >= 0 && begin == allocation)
                        ++snapshot->sites[site].regions;
                } else if ((heap_range = range_index(g_heap_ranges, g_heap_range_count, begin)) != ~0u)
                    category = cat_heap;
                else if (region.Protect & PAGE_WRITECOMBINE)
                    category = cat_write_combined;
            }
        }
        u64 resident = 0, private_resident = 0;
        for (u64 page = begin; page < end;) {
            u32 count = 0;
            while (count < 512 && page < end) {
                pages[count++].VirtualAddress = reinterpret_cast<void*>(page);
                page += 4096;
            }
            if (!K32QueryWorkingSetEx(GetCurrentProcess(), pages, static_cast<DWORD>(count * sizeof(pages[0]))))
                break;
            for (u32 i = 0; i < count; ++i)
                if (pages[i].VirtualAttributes.Valid) {
                    resident += 4096;
                    if (!pages[i].VirtualAttributes.Shared)
                        private_resident += 4096;
                }
        }
        CategoryTotals& totals = snapshot->categories[category];
        totals.committed += region.RegionSize;
        totals.resident += resident;
        totals.private_resident += private_resident;
        ++totals.regions;
        if (category == cat_stack)
            snapshot->stack_committed += region.RegionSize;
        if (module_slot != ~0u) {
            snapshot->modules[module_slot].size += region.RegionSize;
            snapshot->modules[module_slot].resident += resident;
            snapshot->modules[module_slot].private_resident += private_resident;
        }
        if (site >= 0) {
            snapshot->sites[site].committed += region.RegionSize;
            snapshot->sites[site].resident += private_resident;
        }
        if (heap_range != ~0u)
            snapshot->heaps[g_heap_range_heap[heap_range]].resident += private_resident;
        if (category == cat_other_private || category == cat_write_combined) {
            const u32 kind = category == cat_write_combined ? 1 : 0;
            const u32 owner = owners_region_owner(reinterpret_cast<u64>(region.AllocationBase));
            if (owner < kOwnerModuleCapacity) {
                snapshot->owners[owner].committed[kind] += region.RegionSize;
                snapshot->owners[owner].resident[kind] += private_resident;
            }
            // Keep the largest resident regions (insertion into a short list).
            u32 slot = snapshot->region_count;
            while (slot > 0 && snapshot->regions[slot - 1].resident < private_resident)
                --slot;
            if (slot < kRegionDetailCapacity) {
                const u32 last = minimum<u32>(snapshot->region_count, kRegionDetailCapacity - 1);
                for (u32 k = last; k > slot; --k)
                    snapshot->regions[k] = snapshot->regions[k - 1];
                snapshot->regions[slot] = {begin, region.RegionSize, private_resident, region.Protect, kind, owner};
                if (snapshot->region_count < kRegionDetailCapacity)
                    ++snapshot->region_count;
            }
        }
    }
    ReleaseSRWLockShared(&g_tracked_lock);
    snapshot->valid = true;
    return true;
}

double cycles_per_us = 0;

void calibrate_cycles() noexcept {
    const u64 qpc0 = qpc_now();
    const u64 tsc0 = __rdtsc();
    Sleep(30);
    const u64 qpc1 = qpc_now();
    const u64 tsc1 = __rdtsc();
    const double us = static_cast<double>(qpc1 - qpc0) * 1e6 / static_cast<double>(g_probe.qpc_frequency);
    cycles_per_us = us > 0 ? static_cast<double>(tsc1 - tsc0) / us : 1.0;
}

} // namespace perf_probe_detail

namespace {

double cycles_to_us(u64 cycles) noexcept {
    return cycles_per_us > 0 ? static_cast<double>(cycles) / cycles_per_us : 0;
}

void sort_u64(u64* values, u32 count) noexcept {
    // Heap sort: no CRT qsort.
    auto sift = [&](u32 start, u32 end) noexcept {
        u32 root = start;
        while (root * 2 + 1 < end) {
            u32 child = root * 2 + 1;
            if (child + 1 < end && values[child] < values[child + 1])
                ++child;
            if (values[root] >= values[child])
                return;
            const u64 t = values[root];
            values[root] = values[child];
            values[child] = t;
            root = child;
        }
    };
    if (count < 2)
        return;
    for (u32 start = count / 2; start-- > 0;)
        sift(start, count);
    for (u32 end = count - 1; end > 0; --end) {
        const u64 t = values[0];
        values[0] = values[end];
        values[end] = t;
        sift(0, end);
    }
}
u64 percentile(const u64* sorted, u32 count, u32 permille) noexcept {
    if (!count)
        return 0;
    const u32 index = minimum<u32>(count - 1, static_cast<u32>((static_cast<u64>(count) * permille + 999) / 1000) - 1);
    return sorted[index];
}

} // namespace

namespace perf_probe_detail {

void report_phase(Phase phase) noexcept {
    PhaseStats& stats = g_phases[static_cast<u32>(phase)];
    if (!stats.measured)
        return;
    Out& o = g_out;
    const double freq = static_cast<double>(g_probe.qpc_frequency);
    o.s("phase name=");
    o.s(kPhaseNames[static_cast<u32>(phase)]);
    o.kv("ms", static_cast<u64>((stats.end.qpc - stats.begin.qpc) * 1000.0 / freq));
    o.kv("frames", stats.frame_count);
    o.kv("main_cycles", stats.end.main_cycles - stats.begin.main_cycles);
    o.kv("process_cycles", stats.end.process_cycles - stats.begin.process_cycles);
    o.kvf("main_cpu_ms", cycles_to_us(stats.end.main_cycles - stats.begin.main_cycles) / 1000.0);
    o.kvf("proc_cpu_ms", cycles_to_us(stats.end.process_cycles - stats.begin.process_cycles) / 1000.0);
    o.kvf("proc_time_ms", static_cast<double>((stats.end.user_100ns + stats.end.kernel_100ns) -
                                              (stats.begin.user_100ns + stats.begin.kernel_100ns)) / 10'000.0);
    u64* frames = g_frame_cycles + stats.frame_cycles_offset;
    sort_u64(frames, stats.frame_count);
    o.kvf("frame_p50_us", cycles_to_us(percentile(frames, stats.frame_count, 500)));
    o.kvf("frame_p95_us", cycles_to_us(percentile(frames, stats.frame_count, 950)));
    o.kvf("frame_p99_us", cycles_to_us(percentile(frames, stats.frame_count, 990)));
    o.kvf("frame_max_us", cycles_to_us(stats.frame_count ? frames[stats.frame_count - 1] : 0));
    o.kv("wakes", stats.wakes);
    o.kv("wake_timeouts", stats.wake_timeouts);
    o.kv("wake_frames", stats.wake_frames);
    o.kv("wake_messages", stats.wake_messages);
    o.kv("wake_handles", stats.wake_handles);
    o.kv("zero_timeouts", stats.zero_timeouts);
    o.kv("next_pending", stats.next_pending);
    o.kv("next_timeline", stats.next_timeline);
    o.kv("next_effect", stats.next_effect);
    o.kv("next_spinner", stats.next_spinner);
    o.kv("next_video", stats.next_video);
    o.kv("next_tooltip", stats.next_tooltip);
    o.kv("next_surface", stats.next_surface);
    o.kv("next_background", stats.next_background);
    o.kv("next_smooth", stats.next_smooth);
    o.kv("next_caret", stats.next_caret);
    o.kv("next_interaction", stats.next_interaction);
    o.kv("next_timestamp", stats.next_timestamp);
    const auto delta = [](u64 end, u64 begin) noexcept { return end >= begin ? end - begin : end; };
    o.kv("md_parses", delta(stats.text_end.markdown_parses, stats.text_begin.markdown_parses));
    o.kv("md_hits", delta(stats.text_end.markdown_hits, stats.text_begin.markdown_hits));
    o.kv("text_builds", delta(stats.text_end.text_builds, stats.text_begin.text_builds));
    o.kv("shell_reuses", delta(stats.text_end.shell_reuses, stats.text_begin.shell_reuses));
    o.kv("paint_builds", delta(stats.text_end.paint_builds, stats.text_begin.paint_builds));
    o.kv("up_paint_b", delta(stats.text_end.paint_upload_bytes, stats.text_begin.paint_upload_bytes));
    o.kv("up_text_b", delta(stats.text_end.text_upload_bytes, stats.text_begin.text_upload_bytes));
    o.kv("up_media_b", delta(stats.text_end.media_upload_bytes, stats.text_begin.media_upload_bytes));
    o.kv("member_resolves", delta(stats.text_end.member_media_resolves, stats.text_begin.member_media_resolves));
    o.kv("shell_replays", delta(stats.text_end.shell_replays, stats.text_begin.shell_replays));
    o.kv("anim_entries", stats.anim_entries);
    o.kv("anim_visible", stats.anim_visible);
    o.kv("anim_suspended", stats.anim_suspended);
    o.kv("anim_advances", stats.anim_advances);
    o.kv("anim_visible_peak", stats.anim_visible_peak);
    o.kv("ctxsw", stats.end.context_switches);
    o.kv("ctxsw_main", stats.end.main_context_switches - stats.begin.main_context_switches);
    o.kv("draws", stats.draw_calls);
    {
        const GpuApiCounters& a = stats.gpu_end.api;
        const GpuApiCounters& b = stats.gpu_begin.api;
        const auto d = [](LONG64 end, LONG64 begin) noexcept { return static_cast<u64>(end - begin); };
        o.kv("g_shader", d(a.shader_binds, b.shader_binds));
        o.kv("g_srv", d(a.resource_binds, b.resource_binds));
        o.kv("g_sampler", d(a.sampler_binds, b.sampler_binds));
        o.kv("g_cb", d(a.constant_binds, b.constant_binds));
        o.kv("g_ia", d(a.input_binds, b.input_binds));
        o.kv("g_om", d(a.output_binds, b.output_binds));
        o.kv("g_rs", d(a.raster_binds, b.raster_binds));
        o.kv("g_scissor", d(a.scissors, b.scissors));
        o.kv("g_scissor_same", d(a.repeated_scissors, b.repeated_scissors));
        o.kv("g_maps", d(a.maps, b.maps));
        o.kv("g_discard", d(a.map_discards, b.map_discards));
        o.kv("g_nooverwrite", d(a.map_no_overwrites, b.map_no_overwrites));
        o.kv("g_copies", d(a.copies, b.copies));
        o.kv("g_copy_px", d(a.copy_pixels, b.copy_pixels));
        o.kv("g_dispatch", d(a.dispatches, b.dispatches));
        o.kv("g_groups", d(a.dispatch_groups, b.dispatch_groups));
        o.kv("g_cs", d(a.compute_binds, b.compute_binds));
        o.kv("g_clears", d(a.clears, b.clears));
        o.kv("g_flushes", d(a.flushes, b.flushes));
        const GpuWork& e = stats.gpu_end;
        const GpuWork& w = stats.gpu_begin;
        o.kv("replay_full_n", e.full_replays - w.full_replays);
        o.kv("replay_partial_n", e.partial_replays - w.partial_replays);
        o.kv("replay_partial_px", e.replay_pixels - w.replay_pixels);
        o.kv("replay_fail", e.replay_failures - w.replay_failures);
        // Main-thread CPU of the replayed draw streams: submission cost alone.
        o.kv("replays", e.replays - w.replays);
        o.kvf("replay_cpu_us", cycles_to_us(e.replay_cycles - w.replay_cycles));
        o.kv("painted", e.painted - w.painted);
        o.kv("partial_px", e.partial_pixels - w.partial_pixels);
        o.kv("media_up_b", e.media_upload_bytes - w.media_upload_bytes);
        o.kv("anim_copy_px", e.animation_copy_pixels - w.animation_copy_pixels);
        o.kv("media_frames", e.media_frames - w.media_frames);
        o.kv("anim_restores", e.animation_restores - w.animation_restores);
        o.kv("video_frames", e.video_frames - w.video_frames);
        o.kv("glyph_px", e.glyph_pixels - w.glyph_pixels);
        o.kv("inst_uploads", e.instance_uploads - w.instance_uploads);
    }
    o.kv("va_calls", static_cast<u64>(stats.alloc_end.virtual_alloc_calls - stats.alloc_begin.virtual_alloc_calls));
    o.kv("va_commit_kb",
         static_cast<u64>(stats.alloc_end.virtual_commit_bytes - stats.alloc_begin.virtual_commit_bytes) / 1024);
    o.kv("vf_calls", static_cast<u64>(stats.alloc_end.virtual_free_calls - stats.alloc_begin.virtual_free_calls));
    o.kv("heap_calls", static_cast<u64>((stats.alloc_end.heap_alloc_calls - stats.alloc_begin.heap_alloc_calls) +
                                        (stats.alloc_end.heap_realloc_calls - stats.alloc_begin.heap_realloc_calls)));
    o.kv("heap_kb", static_cast<u64>(stats.alloc_end.heap_alloc_bytes - stats.alloc_begin.heap_alloc_bytes) / 1024);
    o.kv("faults", stats.end.page_faults - stats.begin.page_faults);
    o.kv("pws_kb", stats.end.private_working_set / 1024);
    o.kv("private_kb", stats.end.private_bytes / 1024);
    o.kv("threads", stats.end.threads);
    o.kv("handles", stats.end.handles);
    o.s("\n");
}

void report_snapshot(u32 index) noexcept {
    const MemorySnapshot* snapshot = g_snapshots[index];
    if (!snapshot || !snapshot->valid)
        return;
    Out& o = g_out;
    for (u32 c = 0; c < cat_count; ++c) {
        const CategoryTotals& totals = snapshot->categories[c];
        o.s("memory snapshot=");
        o.s(kSnapshotNames[index]);
        o.s(" category=");
        o.s(kCategoryNames[c]);
        o.kv("commit_kb", totals.committed / 1024);
        o.kv("resident_kb", totals.resident / 1024);
        o.kv("pws_kb", totals.private_resident / 1024);
        o.kv("regions", totals.regions);
        o.s("\n");
    }
    o.s("memory_detail snapshot=");
    o.s(kSnapshotNames[index]);
    o.kv("heap_count", snapshot->heap_count);
    o.kv("heap_committed_kb", snapshot->heap_committed / 1024);
    o.kv("heap_busy_kb", snapshot->heap_busy / 1024);
    o.kv("heap_blocks", snapshot->heap_blocks);
    o.kv("stack_threads", snapshot->stack_threads);
    o.kv("stack_reserved_kb", snapshot->stack_reserved / 1024);
    o.kv("stack_committed_kb", snapshot->stack_committed / 1024);
    o.kv("app_regions", snapshot->app_regions);
    o.kv("app_small_regions", snapshot->app_small_regions);
    o.kv("app_reserved_kb", snapshot->app_reserved / 1024);
    o.kv("gpu_local_kb", snapshot->gpu_local / 1024);
    o.kv("gpu_nonlocal_kb", snapshot->gpu_nonlocal / 1024);
    o.s("\n");
    for (u32 c = 0; c < gpu_class_count; ++c) {
        if (!snapshot->gpu.created_count[c])
            continue;
        o.s("gpu snapshot=");
        o.s(kSnapshotNames[index]);
        o.s(" class=");
        o.s(kGpuResourceClassNames[c]);
        o.kv("live_kb", snapshot->gpu.live_bytes[c] / 1024);
        o.kv("live", snapshot->gpu.live_count[c]);
        o.kv("created_kb", snapshot->gpu.created_bytes[c] / 1024);
        o.kv("created", snapshot->gpu.created_count[c]);
        o.s("\n");
    }
    const auto owner_name = [&](u32 owner) noexcept {
        const OwnerModule* module = owner < kOwnerModuleCapacity ? owners_module(owner) : nullptr;
        if (owner == kProcessHeapCreator)
            o.s("process_heap");
        else if (module)
            o.wide(module->name);
        else
            o.s("unknown");
    };
    for (u32 h = 0; h < snapshot->heap_detail_count; ++h) {
        const HeapTotals& heap = snapshot->heaps[h];
        o.s("heap snapshot=");
        o.s(kSnapshotNames[index]);
        o.kv("index", h);
        o.s(" creator=");
        owner_name(heap.creator);
        o.kv("commit_kb", heap.committed / 1024);
        o.kv("pws_kb", heap.resident / 1024);
        o.kv("busy_kb", heap.busy / 1024);
        o.kv("large_kb", heap.large / 1024);
        o.kv("blocks", heap.blocks);
        o.s("\n");
    }
    for (u32 m = 0; m < owners_module_count() && m < kOwnerModuleCapacity; ++m) {
        const OwnerTotals& owner = snapshot->owners[m];
        if (!owner.heap_live && !owner.committed[0] && !owner.committed[1])
            continue;
        o.s("owner snapshot=");
        o.s(kSnapshotNames[index]);
        o.s(" module=");
        owner_name(m);
        o.kv("heap_live_kb", owner.heap_live / 1024);
        o.kv("heap_blocks", owner.heap_blocks);
        o.kv("private_commit_kb", owner.committed[0] / 1024);
        o.kv("private_pws_kb", owner.resident[0] / 1024);
        o.kv("wc_commit_kb", owner.committed[1] / 1024);
        o.kv("wc_pws_kb", owner.resident[1] / 1024);
        o.s("\n");
    }
    for (u32 m = 0; m < owners_module_count() && m < kOwnerModuleCapacity; ++m) {
        if (snapshot->owners[m].heap_live < 256 * 1024 || index != 2)
            continue;
        u64 tags[kOwnerTagCount]{}, sizes[kOwnerSizeBuckets]{};
        owners_block_totals(m, tags, sizes);
        for (u32 t = 0; t < kOwnerTagCount; ++t) {
            if (tags[t] < 16 * 1024)
                continue;
            o.s("owner_tag snapshot=");
            o.s(kSnapshotNames[index]);
            o.s(" module=");
            owner_name(m);
            o.s(" tag=");
            o.s(startup_tag_name(t));
            o.kv("kb", tags[t] / 1024);
            o.s("\n");
        }
        o.s("owner_sizes snapshot=");
        o.s(kSnapshotNames[index]);
        o.s(" module=");
        owner_name(m);
        for (u32 b = 0; b < kOwnerSizeBuckets; ++b) {
            static const char* const kBuckets[] = {"lt1k", "lt4k", "lt16k", "lt64k", "lt256k", "lt1m", "ge1m"};
            o.kv(kBuckets[b], sizes[b] / 1024);
        }
        o.s("\n");
    }
    o.s("owner_table snapshot=");
    o.s(kSnapshotNames[index]);
    o.kv("live_blocks", snapshot->owner_entries);
    o.kv("dropped", snapshot->owner_dropped);
    o.kv("patched_slots", snapshot->owner_slots);
    o.s("\n");
    for (u32 g = 0; g < snapshot->gpu_largest_count; ++g) {
        const GpuResourceInfo& resource = snapshot->gpu_largest[g];
        o.s("gpu_resource snapshot=");
        o.s(kSnapshotNames[index]);
        o.s(" class=");
        o.s(kGpuResourceClassNames[resource.klass]);
        o.kv("kb", resource.bytes / 1024);
        o.kv("width", resource.width);
        o.kv("height", resource.height);
        o.kv("format", resource.format);
        o.s(" bind=");
        o.hex(resource.bind);
        o.s(" caller=");
        o.hex(resource.caller_rva);
        o.s("\n");
    }
    for (u32 r = 0; r < snapshot->region_count; ++r) {
        const RegionDetail& region = snapshot->regions[r];
        o.s("region snapshot=");
        o.s(kSnapshotNames[index]);
        o.s(region.category ? " kind=write_combined" : " kind=other_private");
        o.s(" owner=");
        owner_name(region.owner);
        o.kv("size_kb", region.size / 1024);
        o.kv("pws_kb", region.resident / 1024);
        o.s(" protect=");
        o.hex(region.protect);
        o.s("\n");
    }
}

void report_modules(const MemorySnapshot& snapshot) noexcept {
    // Largest resident modules first (selection by repeated maximum).
    bool used[kModuleCapacity]{};
    for (u32 rank = 0; rank < 24 && rank < snapshot.module_count; ++rank) {
        u32 best = ~0u;
        for (u32 i = 0; i < snapshot.module_count; ++i)
            if (!used[i] && (best == ~0u || snapshot.modules[i].resident > snapshot.modules[best].resident))
                best = i;
        if (best == ~0u)
            break;
        used[best] = true;
        const ModuleTotals& module = snapshot.modules[best];
        g_out.s("module name=");
        g_out.wide(module.name);
        g_out.kv("commit_kb", module.size / 1024);
        g_out.kv("resident_kb", module.resident / 1024);
        g_out.kv("pws_kb", module.private_resident / 1024);
        g_out.s("\n");
    }
    g_out.kv("modules_loaded", snapshot.module_count);
    g_out.s("\n");
}

void report_sites(const MemorySnapshot& snapshot) noexcept {
    bool used[kSiteCapacity]{};
    for (u32 rank = 0; rank < 40 && rank < snapshot.site_count; ++rank) {
        u32 best = ~0u;
        for (u32 i = 0; i < snapshot.site_count; ++i)
            if (!used[i] && (best == ~0u || snapshot.sites[i].committed > snapshot.sites[best].committed))
                best = i;
        if (best == ~0u)
            break;
        used[best] = true;
        const SiteTotals& site = snapshot.sites[best];
        g_out.s("site frames=");
        for (u32 f = 0; f < kSiteFrames; ++f) {
            if (f)
                g_out.s("/");
            const u64 frame = site.frames[f];
            g_out.hex(frame >= g_image_base && frame < g_image_base + 0x10000000ull ? frame - g_image_base : 0);
        }
        g_out.kv("commit_kb", site.committed / 1024);
        g_out.kv("pws_kb", site.resident / 1024);
        g_out.kv("regions", site.regions);
        g_out.s("\n");
    }
}

} // namespace perf_probe_detail

namespace {

// Sampling profiler, opt-in (MAICORD_PERF_PROFILE=1): a 1 kHz sampler suspends
// the main thread, unwinds it with .pdata (no symbols, no locks: a module table
// built while the main thread runs) and counts function-start RVAs per phase.
// Self time in another module is charged to that module and to the nearest
// probe-image caller; run_perf.ps1 resolves RVAs through the link map.
struct ProfileModule {
    u64 base, end;
    const RUNTIME_FUNCTION* functions;
    u32 function_count;
    u32 stable_id; // index into g_profile_names; module order changes as DLLs load
};
struct ProfileName {
    u64 base;
    wchar_t name[32];
};
constexpr u32 kProfileNameCapacity = 256;
ProfileName g_profile_names[kProfileNameCapacity];
u32 g_profile_name_count = 0;
constexpr u32 kProfileModuleCapacity = 96;
ProfileModule g_profile_modules[kProfileModuleCapacity];
u32 g_profile_module_count = 0;

} // namespace

namespace perf_probe_detail {

PhaseProfile* g_profiles = nullptr; // one per phase
HANDLE g_sampler = nullptr;
HANDLE g_main_thread = nullptr;
volatile LONG g_sampler_stop = 0;

void profile_refresh_modules() noexcept {
    HMODULE modules[kProfileModuleCapacity];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return;
    u32 count = minimum<u32>(bytes / sizeof(HMODULE), kProfileModuleCapacity);
    ProfileModule fresh[kProfileModuleCapacity];
    u32 used = 0;
    for (u32 i = 0; i < count; ++i) {
        auto* base = reinterpret_cast<const u8*>(modules[i]);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        ProfileModule& m = fresh[used++];
        m = {};
        m.base = reinterpret_cast<u64>(base);
        m.end = m.base + nt->OptionalHeader.SizeOfImage;
        m.functions = reinterpret_cast<const RUNTIME_FUNCTION*>(base + pdata.VirtualAddress);
        m.function_count = pdata.Size / sizeof(RUNTIME_FUNCTION);
        u32 id = 0;
        while (id < g_profile_name_count && g_profile_names[id].base != m.base)
            ++id;
        if (id == g_profile_name_count && id < kProfileNameCapacity) {
            ProfileName& name = g_profile_names[g_profile_name_count++];
            name = {};
            name.base = m.base;
            wchar_t path[MAX_PATH]{};
            K32GetModuleBaseNameW(GetCurrentProcess(), modules[i], path, MAX_PATH);
            for (u32 c = 0; c < 31 && path[c]; ++c)
                name.name[c] = path[c];
        }
        m.stable_id = id;
    }
    // Sort by base (insertion sort; under a hundred modules).
    for (u32 i = 1; i < used; ++i)
        for (u32 j = i; j > 0 && fresh[j - 1].base > fresh[j].base; --j) {
            const ProfileModule t = fresh[j];
            fresh[j] = fresh[j - 1];
            fresh[j - 1] = t;
        }
    copy_bytes(g_profile_modules, fresh, used * static_cast<u32>(sizeof(ProfileModule)));
    g_profile_module_count = used;
}

} // namespace perf_probe_detail

namespace {

i32 profile_module_of(u64 address) noexcept {
    u32 low = 0, high = g_profile_module_count;
    while (low < high) {
        const u32 mid = (low + high) / 2;
        if (g_profile_modules[mid].end <= address)
            low = mid + 1;
        else
            high = mid;
    }
    return low < g_profile_module_count && address >= g_profile_modules[low].base ? static_cast<i32>(low) : -1;
}

const RUNTIME_FUNCTION* profile_function_of(const ProfileModule& module, u64 address) noexcept {
    const u32 rva = static_cast<u32>(address - module.base);
    u32 low = 0, high = module.function_count;
    while (low < high) {
        const u32 mid = (low + high) / 2;
        if (module.functions[mid].EndAddress <= rva)
            low = mid + 1;
        else
            high = mid;
    }
    if (low < module.function_count && rva >= module.functions[low].BeginAddress)
        return &module.functions[low];
    return nullptr;
}

// The primary entry of a chained (split) function, for attribution.
u32 profile_primary_begin(const ProfileModule& module, const RUNTIME_FUNCTION* function) noexcept {
    for (u32 depth = 0; depth < 8 && function; ++depth) {
        if (function->UnwindData & 1)
            return function->BeginAddress;
        const u8* info = reinterpret_cast<const u8*>(module.base + function->UnwindData);
        if (!((info[0] >> 3) & 0x4)) // UNW_FLAG_CHAININFO
            return function->BeginAddress;
        const u32 codes = info[2];
        const u32 offset = 4 + ((codes + 1) & ~1u) * 2;
        function = reinterpret_cast<const RUNTIME_FUNCTION*>(info + offset);
    }
    return function ? function->BeginAddress : 0;
}

void profile_count(PhaseProfile& profile, u32 key, u32 self, u32 inclusive, u32 caller_self) noexcept {
    u32 slot = (key * 2654435761u) & (kProfileEntries - 1);
    for (u32 probe = 0; probe < kProfileEntries; ++probe, slot = (slot + 1) & (kProfileEntries - 1)) {
        ProfileEntry& entry = profile.entries[slot];
        if (entry.key == key || (!entry.key && !entry.self && !entry.inclusive)) {
            entry.key = key;
            entry.self += self;
            entry.inclusive += inclusive;
            entry.caller_self += caller_self;
            return;
        }
    }
}

u64 g_profile_last_cycles = 0;

void profile_sample() noexcept {
    PhaseProfile& profile = g_profiles[static_cast<u32>(g_probe.phase)];
    // CPU-weighted: a thread that barely ran since the last tick was waiting.
    ULONG64 cycles = 0;
    QueryThreadCycleTime(g_main_thread, &cycles);
    const u64 ran = cycles - g_profile_last_cycles;
    g_profile_last_cycles = cycles;
    // Startup is sampled by wall clock (waits included) to show where the
    // main thread blocks; every other phase is CPU weighted.
    const bool wall = g_probe.phase == Phase::startup;
    if (ran < 200'000 && !wall)
        return;
    if (SuspendThread(g_main_thread) == static_cast<DWORD>(-1))
        return;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    u32 keys[24];
    u32 key_count = 0;
    bool top_external = false;
    bool charged_caller = false;
    if (GetThreadContext(g_main_thread, &context)) {
        ++profile.samples;
        for (u32 depth = 0; depth < 24 && context.Rip; ++depth) {
            const i32 module_index = profile_module_of(context.Rip);
            if (module_index < 0)
                break;
            const ProfileModule& module = g_profile_modules[module_index];
            const RUNTIME_FUNCTION* function = profile_function_of(module, context.Rip);
            u32 key;
            if (module.base == g_image_base)
                key = function ? profile_primary_begin(module, function) : static_cast<u32>(context.Rip - module.base);
            else
                key = 0x80000000u | module.stable_id;
            if (depth == 0) {
                // Parked right after a syscall: blocked in a wait or in the kernel.
                const u8* code = reinterpret_cast<const u8*>(context.Rip);
                if (code[-2] == 0x0F && code[-1] == 0x05) {
                    ++profile.in_syscall;
                    if (!wall)
                        break;
                }
                profile_count(profile, key, 1, 0, 0);
                top_external = module.base != g_image_base;
            } else if (top_external && !charged_caller && module.base == g_image_base) {
                profile_count(profile, key, 0, 0, 1);
                charged_caller = true;
            }
            bool seen = false;
            for (u32 k = 0; k < key_count; ++k)
                seen = seen || keys[k] == key;
            if (!seen && key_count < 24)
                keys[key_count++] = key;
            if (!function) {
                // Leaf: the return address is at the stack pointer.
                context.Rip = *reinterpret_cast<const u64*>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void* handler_data = nullptr;
            u64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, module.base, context.Rip, const_cast<RUNTIME_FUNCTION*>(function),
                             &context, &handler_data, &establisher, nullptr);
        }
    } else {
        ++profile.unattributed;
    }
    ResumeThread(g_main_thread);
    for (u32 k = 0; k < key_count; ++k)
        profile_count(profile, keys[k], 0, 1, 0);
}

} // namespace

namespace perf_probe_detail {

DWORD WINAPI sampler_thread(void*) noexcept {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        return 1;
    LARGE_INTEGER due{};
    due.QuadPart = -10'000;
    SetWaitableTimer(timer, &due, 1, nullptr, nullptr, FALSE);
    u32 ticks = 0;
    while (!g_sampler_stop) {
        WaitForSingleObject(timer, INFINITE);
        if ((ticks++ & 255) == 0)
            profile_refresh_modules();
        if (g_probe.phase < Phase::report)
            profile_sample();
    }
    CloseHandle(timer);
    return 0;
}

} // namespace perf_probe_detail

namespace {

u32 profile_weight(const ProfileEntry& e, bool self) noexcept {
    return self ? e.self + e.caller_self : e.inclusive;
}

} // namespace

namespace perf_probe_detail {

void report_profile(Phase phase) noexcept {
    if (!g_profiles)
        return;
    const PhaseProfile& profile = g_profiles[static_cast<u32>(phase)];
    if (!profile.samples)
        return;
    Out& o = g_out;
    o.s("profile phase=");
    o.s(kPhaseNames[static_cast<u32>(phase)]);
    o.kv("samples", profile.samples);
    o.kv("in_syscall", profile.in_syscall);
    o.s("\n");
    for (u32 pass = 0; pass < 2; ++pass) {
        static bool used[kProfileEntries];
        clear_bytes(used, sizeof(used));
        for (u32 rank = 0; rank < 30; ++rank) {
            u32 best = ~0u;
            for (u32 i = 0; i < kProfileEntries; ++i) {
                const ProfileEntry& e = profile.entries[i];
                if (used[i] || !e.key || !profile_weight(e, pass == 0))
                    continue;
                if (best == ~0u || profile_weight(e, pass == 0) > profile_weight(profile.entries[best], pass == 0))
                    best = i;
            }
            if (best == ~0u)
                break;
            used[best] = true;
            const ProfileEntry& e = profile.entries[best];
            o.s("hot phase=");
            o.s(kPhaseNames[static_cast<u32>(phase)]);
            o.s(pass == 0 ? " kind=self" : " kind=inclusive");
            o.kv("weight", profile_weight(e, pass == 0));
            o.kv("own", e.self);
            o.kv("callees", e.caller_self);
            o.kv("incl", e.inclusive);
            if (e.key & 0x80000000u) {
                o.s(" module=");
                const u32 index = e.key & 0x7FFFFFFFu;
                o.wide(index < g_profile_name_count ? g_profile_names[index].name : L"?");
            } else {
                o.s(" rva=");
                o.hex(e.key);
            }
            o.s("\n");
        }
    }
}

void report_threads() noexcept {
    for (u32 i = 0; i < g_thread_count; ++i) {
        const ThreadRow& row = g_threads[i];
        g_out.s("thread start=");
        if (row.module[0]) {
            g_out.wide(row.module);
            g_out.s("+");
            g_out.hex(row.start - row.module_base);
        } else {
            g_out.hex(row.start);
        }
        g_out.kvf("cpu_ms", cycles_to_us(row.cycles) / 1000.0);
        g_out.kv("ctxsw", row.context_switches);
        g_out.kv("idle_ctxsw", row.idle_switches);
        g_out.kv("alive_at_idle", row.seen_at_idle);
        g_out.kv("stack_reserve_kb", row.stack_reserved / 1024);
        g_out.kv("stack_commit_kb", row.stack_committed / 1024);
        g_out.s("\n");
    }
}

} // namespace perf_probe_detail

namespace {

// AccountUiRuntime members by residency (private working set per member).
Out g_fields;

} // namespace

namespace perf_probe_detail {

void report_account_ui_fields_text() noexcept {
    g_out.s(g_fields.text);
}

} // namespace perf_probe_detail

namespace {

struct FieldSpan {
    const char* name;
    u64 offset, bytes;
};
const FieldSpan kAccountUiFields[] = {
    {"author_hydration", offsetof(app_detail::AccountUiRuntime, author_hydration), sizeof(app_detail::AccountUiRuntime::author_hydration)},
    {"private_conversation", offsetof(app_detail::AccountUiRuntime, private_conversation), sizeof(app_detail::AccountUiRuntime::private_conversation)},
    {"chat_state", offsetof(app_detail::AccountUiRuntime, chat_state), sizeof(app_detail::AccountUiRuntime::chat_state)},
    {"read_state_store", offsetof(app_detail::AccountUiRuntime, read_state_store), sizeof(app_detail::AccountUiRuntime::read_state_store)},
    {"read_state_bank", offsetof(app_detail::AccountUiRuntime, read_state_bank), sizeof(app_detail::AccountUiRuntime::read_state_bank)},
    {"role_storage", offsetof(app_detail::AccountUiRuntime, role_storage), sizeof(app_detail::AccountUiRuntime::role_storage)},
    {"overwrite_storage", offsetof(app_detail::AccountUiRuntime, overwrite_storage), sizeof(app_detail::AccountUiRuntime::overwrite_storage)},
    {"private_channel_store", offsetof(app_detail::AccountUiRuntime, private_channel_store), sizeof(app_detail::AccountUiRuntime::private_channel_store)},
    {"user_store", offsetof(app_detail::AccountUiRuntime, user_store), sizeof(app_detail::AccountUiRuntime::user_store)},
    {"guild_member_store", offsetof(app_detail::AccountUiRuntime, guild_member_store), sizeof(app_detail::AccountUiRuntime::guild_member_store)},
    {"thread_member_store", offsetof(app_detail::AccountUiRuntime, thread_member_store), sizeof(app_detail::AccountUiRuntime::thread_member_store)},
    {"thread_member_banks", offsetof(app_detail::AccountUiRuntime, thread_member_banks), sizeof(app_detail::AccountUiRuntime::thread_member_banks)},
    {"typing_store", offsetof(app_detail::AccountUiRuntime, typing_store), sizeof(app_detail::AccountUiRuntime::typing_store)},
    {"voice_states", offsetof(app_detail::AccountUiRuntime, voice_states), sizeof(app_detail::AccountUiRuntime::voice_states)},
    {"voice_session", offsetof(app_detail::AccountUiRuntime, voice_session), sizeof(app_detail::AccountUiRuntime::voice_session)},
    {"member_list", offsetof(app_detail::AccountUiRuntime, member_list), sizeof(app_detail::AccountUiRuntime::member_list)},
    {"navigation_view", offsetof(app_detail::AccountUiRuntime, navigation_view), sizeof(app_detail::AccountUiRuntime::navigation_view)},
    {"message_text", offsetof(app_detail::AccountUiRuntime, message_text), sizeof(app_detail::AccountUiRuntime::message_text)},
    {"text_measure_cache", offsetof(app_detail::AccountUiRuntime, text_measure_cache), sizeof(app_detail::AccountUiRuntime::text_measure_cache)},
    {"renderer_text", offsetof(app_detail::AccountUiRuntime, renderer_text), sizeof(app_detail::AccountUiRuntime::renderer_text)},
    {"message_rich_store", offsetof(app_detail::AccountUiRuntime, message_rich_store), sizeof(app_detail::AccountUiRuntime::message_rich_store)},
    {"message_poll_store", offsetof(app_detail::AccountUiRuntime, message_poll_store), sizeof(app_detail::AccountUiRuntime::message_poll_store)},
    {"message_poll_binding", offsetof(app_detail::AccountUiRuntime, message_poll_binding), sizeof(app_detail::AccountUiRuntime::message_poll_binding)},
    {"poll_gateway_serial", offsetof(app_detail::AccountUiRuntime, poll_gateway_serial), sizeof(app_detail::AccountUiRuntime::poll_gateway_serial)},
    {"message_history_poll_serial", offsetof(app_detail::AccountUiRuntime, message_history_poll_serial), sizeof(app_detail::AccountUiRuntime::message_history_poll_serial)},
    {"poll_sequence", offsetof(app_detail::AccountUiRuntime, poll_sequence), sizeof(app_detail::AccountUiRuntime::poll_sequence)},
    {"poll_sequence_known", offsetof(app_detail::AccountUiRuntime, poll_sequence_known), sizeof(app_detail::AccountUiRuntime::poll_sequence_known)},
    {"media_atlas", offsetof(app_detail::AccountUiRuntime, media_atlas), sizeof(app_detail::AccountUiRuntime::media_atlas)},
    {"preview_media_atlas", offsetof(app_detail::AccountUiRuntime, preview_media_atlas), sizeof(app_detail::AccountUiRuntime::preview_media_atlas)},
    {"message_drafts", offsetof(app_detail::AccountUiRuntime, message_drafts), sizeof(app_detail::AccountUiRuntime::message_drafts)},
    {"reaction_preferences", offsetof(app_detail::AccountUiRuntime, reaction_preferences), sizeof(app_detail::AccountUiRuntime::reaction_preferences)},
    {"message_composer_controller", offsetof(app_detail::AccountUiRuntime, message_composer_controller), sizeof(app_detail::AccountUiRuntime::message_composer_controller)},
    {"guild_record_storage", offsetof(app_detail::AccountUiRuntime, guild_record_storage), sizeof(app_detail::AccountUiRuntime::guild_record_storage)},
    {"channel_record_storage", offsetof(app_detail::AccountUiRuntime, channel_record_storage), sizeof(app_detail::AccountUiRuntime::channel_record_storage)},
    {"emoji_record_storage", offsetof(app_detail::AccountUiRuntime, emoji_record_storage), sizeof(app_detail::AccountUiRuntime::emoji_record_storage)},
    {"emoji_role_storage", offsetof(app_detail::AccountUiRuntime, emoji_role_storage), sizeof(app_detail::AccountUiRuntime::emoji_role_storage)},
    {"user_record_storage", offsetof(app_detail::AccountUiRuntime, user_record_storage), sizeof(app_detail::AccountUiRuntime::user_record_storage)},
    {"guild_member_record_storage", offsetof(app_detail::AccountUiRuntime, guild_member_record_storage), sizeof(app_detail::AccountUiRuntime::guild_member_record_storage)},
    {"typing_record_storage", offsetof(app_detail::AccountUiRuntime, typing_record_storage), sizeof(app_detail::AccountUiRuntime::typing_record_storage)},
    {"guild_order_storage", offsetof(app_detail::AccountUiRuntime, guild_order_storage), sizeof(app_detail::AccountUiRuntime::guild_order_storage)},
    {"channel_order_storage", offsetof(app_detail::AccountUiRuntime, channel_order_storage), sizeof(app_detail::AccountUiRuntime::channel_order_storage)},
    {"user_order_storage", offsetof(app_detail::AccountUiRuntime, user_order_storage), sizeof(app_detail::AccountUiRuntime::user_order_storage)},
    {"guild_member_order_storage", offsetof(app_detail::AccountUiRuntime, guild_member_order_storage), sizeof(app_detail::AccountUiRuntime::guild_member_order_storage)},
    {"member_list_rows", offsetof(app_detail::AccountUiRuntime, member_list_rows), sizeof(app_detail::AccountUiRuntime::member_list_rows)},
    {"navigation_guild_rows", offsetof(app_detail::AccountUiRuntime, navigation_guild_rows), sizeof(app_detail::AccountUiRuntime::navigation_guild_rows)},
    {"navigation_channel_rows", offsetof(app_detail::AccountUiRuntime, navigation_channel_rows), sizeof(app_detail::AccountUiRuntime::navigation_channel_rows)},
    {"renderer_text_quads", offsetof(app_detail::AccountUiRuntime, renderer_text_quads), sizeof(app_detail::AccountUiRuntime::renderer_text_quads)},
    {"renderer_text_batches", offsetof(app_detail::AccountUiRuntime, renderer_text_batches), sizeof(app_detail::AccountUiRuntime::renderer_text_batches)},
    {"message_text_commands", offsetof(app_detail::AccountUiRuntime, message_text_commands), sizeof(app_detail::AccountUiRuntime::message_text_commands)},
    {"message_text_utf8", offsetof(app_detail::AccountUiRuntime, message_text_utf8), sizeof(app_detail::AccountUiRuntime::message_text_utf8)},
    {"message_text_styles", offsetof(app_detail::AccountUiRuntime, message_text_styles), sizeof(app_detail::AccountUiRuntime::message_text_styles)},
    {"message_text_semantics", offsetof(app_detail::AccountUiRuntime, message_text_semantics), sizeof(app_detail::AccountUiRuntime::message_text_semantics)},
    {"message_text_measurements", offsetof(app_detail::AccountUiRuntime, message_text_measurements), sizeof(app_detail::AccountUiRuntime::message_text_measurements)},
    {"message_text_measure_cache_records", offsetof(app_detail::AccountUiRuntime, message_text_measure_cache_records), sizeof(app_detail::AccountUiRuntime::message_text_measure_cache_records)},
    {"message_text_spoiler_reveals", offsetof(app_detail::AccountUiRuntime, message_text_spoiler_reveals), sizeof(app_detail::AccountUiRuntime::message_text_spoiler_reveals)},
    {"renderer_content_semantics", offsetof(app_detail::AccountUiRuntime, renderer_content_semantics), sizeof(app_detail::AccountUiRuntime::renderer_content_semantics)},
    {"renderer_content_hit_regions", offsetof(app_detail::AccountUiRuntime, renderer_content_hit_regions), sizeof(app_detail::AccountUiRuntime::renderer_content_hit_regions)},
    {"chat_state_name_storage", offsetof(app_detail::AccountUiRuntime, chat_state_name_storage), sizeof(app_detail::AccountUiRuntime::chat_state_name_storage)},
    {"user_name_storage", offsetof(app_detail::AccountUiRuntime, user_name_storage), sizeof(app_detail::AccountUiRuntime::user_name_storage)},
    {"event_payload_scratch", offsetof(app_detail::AccountUiRuntime, event_payload_scratch), sizeof(app_detail::AccountUiRuntime::event_payload_scratch)},
    {"guild_id_scratch", offsetof(app_detail::AccountUiRuntime, guild_id_scratch), sizeof(app_detail::AccountUiRuntime::guild_id_scratch)},
    {"message_bulk_ids", offsetof(app_detail::AccountUiRuntime, message_bulk_ids), sizeof(app_detail::AccountUiRuntime::message_bulk_ids)},
    {"response_shape_scratch", offsetof(app_detail::AccountUiRuntime, response_shape_scratch), sizeof(app_detail::AccountUiRuntime::response_shape_scratch)},
    {"navigation_sort_keys", offsetof(app_detail::AccountUiRuntime, navigation_sort_keys), sizeof(app_detail::AccountUiRuntime::navigation_sort_keys)},
    {"message_history_page_structurals", offsetof(app_detail::AccountUiRuntime, message_history_page_structurals), sizeof(app_detail::AccountUiRuntime::message_history_page_structurals)},
    {"profile_decode", offsetof(app_detail::AccountUiRuntime, profile_decode), sizeof(app_detail::AccountUiRuntime::profile_decode)},
    {"projection_scratch", offsetof(app_detail::AccountUiRuntime, projection_scratch), sizeof(app_detail::AccountUiRuntime::projection_scratch)},
    {"message_history_object_structurals", offsetof(app_detail::AccountUiRuntime, message_history_object_structurals), sizeof(app_detail::AccountUiRuntime::message_history_object_structurals)},
    {"history_decode_storage", offsetof(app_detail::AccountUiRuntime, history_decode_storage), sizeof(app_detail::AccountUiRuntime::history_decode_storage)},
    {"message_rich_record_storage", offsetof(app_detail::AccountUiRuntime, message_rich_record_storage), sizeof(app_detail::AccountUiRuntime::message_rich_record_storage)},
    {"message_rich_order", offsetof(app_detail::AccountUiRuntime, message_rich_order), sizeof(app_detail::AccountUiRuntime::message_rich_order)},
    {"message_rich_text", offsetof(app_detail::AccountUiRuntime, message_rich_text), sizeof(app_detail::AccountUiRuntime::message_rich_text)},
    {"media_atlas_entries", offsetof(app_detail::AccountUiRuntime, media_atlas_entries), sizeof(app_detail::AccountUiRuntime::media_atlas_entries)},
    {"media_atlas_buckets", offsetof(app_detail::AccountUiRuntime, media_atlas_buckets), sizeof(app_detail::AccountUiRuntime::media_atlas_buckets)},
    {"media_atlas_allocations", offsetof(app_detail::AccountUiRuntime, media_atlas_allocations), sizeof(app_detail::AccountUiRuntime::media_atlas_allocations)},
    {"media_atlas_rows", offsetof(app_detail::AccountUiRuntime, media_atlas_rows), sizeof(app_detail::AccountUiRuntime::media_atlas_rows)},
    {"preview_media_atlas_entries", offsetof(app_detail::AccountUiRuntime, preview_media_atlas_entries), sizeof(app_detail::AccountUiRuntime::preview_media_atlas_entries)},
    {"preview_media_atlas_buckets", offsetof(app_detail::AccountUiRuntime, preview_media_atlas_buckets), sizeof(app_detail::AccountUiRuntime::preview_media_atlas_buckets)},
    {"preview_media_atlas_allocations", offsetof(app_detail::AccountUiRuntime, preview_media_atlas_allocations), sizeof(app_detail::AccountUiRuntime::preview_media_atlas_allocations)},
    {"preview_media_atlas_rows", offsetof(app_detail::AccountUiRuntime, preview_media_atlas_rows), sizeof(app_detail::AccountUiRuntime::preview_media_atlas_rows)},
    {"message_draft_records", offsetof(app_detail::AccountUiRuntime, message_draft_records), sizeof(app_detail::AccountUiRuntime::message_draft_records)},
    {"message_draft_text", offsetof(app_detail::AccountUiRuntime, message_draft_text), sizeof(app_detail::AccountUiRuntime::message_draft_text)},
    {"message_request_text", offsetof(app_detail::AccountUiRuntime, message_request_text), sizeof(app_detail::AccountUiRuntime::message_request_text)},
    {"message_draft_session", offsetof(app_detail::AccountUiRuntime, message_draft_session), sizeof(app_detail::AccountUiRuntime::message_draft_session)},
    {"active_channel_id", offsetof(app_detail::AccountUiRuntime, active_channel_id), sizeof(app_detail::AccountUiRuntime::active_channel_id)},
    {"message_history_request_id", offsetof(app_detail::AccountUiRuntime, message_history_request_id), sizeof(app_detail::AccountUiRuntime::message_history_request_id)},
    {"observed_selection_revision", offsetof(app_detail::AccountUiRuntime, observed_selection_revision), sizeof(app_detail::AccountUiRuntime::observed_selection_revision)},
    {"rendered_navigation_revision", offsetof(app_detail::AccountUiRuntime, rendered_navigation_revision), sizeof(app_detail::AccountUiRuntime::rendered_navigation_revision)},
    {"rendered_message_created", offsetof(app_detail::AccountUiRuntime, rendered_message_created), sizeof(app_detail::AccountUiRuntime::rendered_message_created)},
    {"rendered_message_updated", offsetof(app_detail::AccountUiRuntime, rendered_message_updated), sizeof(app_detail::AccountUiRuntime::rendered_message_updated)},
    {"rendered_message_deleted", offsetof(app_detail::AccountUiRuntime, rendered_message_deleted), sizeof(app_detail::AccountUiRuntime::rendered_message_deleted)},
    {"rendered_user_revision", offsetof(app_detail::AccountUiRuntime, rendered_user_revision), sizeof(app_detail::AccountUiRuntime::rendered_user_revision)},
    {"rendered_member_revision", offsetof(app_detail::AccountUiRuntime, rendered_member_revision), sizeof(app_detail::AccountUiRuntime::rendered_member_revision)},
    {"rendered_private_revision", offsetof(app_detail::AccountUiRuntime, rendered_private_revision), sizeof(app_detail::AccountUiRuntime::rendered_private_revision)},
    {"rendered_emoji_mutations", offsetof(app_detail::AccountUiRuntime, rendered_emoji_mutations), sizeof(app_detail::AccountUiRuntime::rendered_emoji_mutations)},
    {"rendered_typing_revision", offsetof(app_detail::AccountUiRuntime, rendered_typing_revision), sizeof(app_detail::AccountUiRuntime::rendered_typing_revision)},
    {"rendered_permission_mutations", offsetof(app_detail::AccountUiRuntime, rendered_permission_mutations), sizeof(app_detail::AccountUiRuntime::rendered_permission_mutations)},
    {"rendered_composer_revision", offsetof(app_detail::AccountUiRuntime, rendered_composer_revision), sizeof(app_detail::AccountUiRuntime::rendered_composer_revision)},
    {"rendered_channel_id", offsetof(app_detail::AccountUiRuntime, rendered_channel_id), sizeof(app_detail::AccountUiRuntime::rendered_channel_id)},
    {"text_rebuild_retry_at_ms", offsetof(app_detail::AccountUiRuntime, text_rebuild_retry_at_ms), sizeof(app_detail::AccountUiRuntime::text_rebuild_retry_at_ms)},
    {"text_rebuild_failures", offsetof(app_detail::AccountUiRuntime, text_rebuild_failures), sizeof(app_detail::AccountUiRuntime::text_rebuild_failures)},
    {"rendered_poll_revision", offsetof(app_detail::AccountUiRuntime, rendered_poll_revision), sizeof(app_detail::AccountUiRuntime::rendered_poll_revision)},
};

} // namespace

namespace perf_probe_detail {

void report_account_ui_fields() noexcept {
    if (!g_app.account_ui)
        return;
    const u64 base = reinterpret_cast<u64>(g_app.account_ui);
    const u64 total = sizeof(app_detail::AccountUiRuntime);
    // Covers the demand tail too (tens of MiB reserved); pages past the table read as absent.
    constexpr u32 kPageCapacity = 16'384;
    static PSAPI_WORKING_SET_EX_INFORMATION info[kPageCapacity];
    const u32 pages = minimum<u32>(kPageCapacity, static_cast<u32>((total + 4095) / 4096 + 1));
    const u64 first = base & ~4095ull;
    for (u32 i = 0; i < pages; ++i)
        info[i].VirtualAddress = reinterpret_cast<void*>(first + u64{i} * 4096);
    if (!K32QueryWorkingSetEx(GetCurrentProcess(), info, pages * sizeof(info[0])))
        return;
    for (const FieldSpan& field : kAccountUiFields) {
        if (field.bytes < 16 * 1024)
            continue;
        u64 resident = 0;
        const u64 begin = base + field.offset, end = begin + field.bytes;
        for (u64 page = begin & ~4095ull; page < end; page += 4096) {
            const u32 index = static_cast<u32>((page - first) / 4096);
            if (index < pages && info[index].VirtualAttributes.Valid) {
                const u64 lo = page > begin ? page : begin;
                const u64 hi = page + 4096 < end ? page + 4096 : end;
                resident += hi - lo;
            }
        }
        g_fields.s("ui_field name=");
        g_fields.s(field.name);
        g_fields.kv("kb", field.bytes / 1024);
        g_fields.kv("resident_kb", resident / 1024);
        g_fields.s("\n");
    }
    const MessageRichStore& rich = g_app.account_ui->message_rich_store;
    u32 live = 0, children = 0;
    u64 reactions = 0, attachments = 0, embeds = 0, fields = 0, stickers = 0, components = 0;
    u32 reaction_max = 0, attachment_max = 0, embed_max = 0, field_max = 0, component_max = 0;
    for (u32 slot = 0; rich.records && slot < rich.record_extent; ++slot) {
        const MessageRichRecord& record = rich.records[slot];
        if (!record.occupied)
            continue;
        ++live;
        children += record.forwarded_snapshot_child;
        reactions += record.reaction_count;
        attachments += record.attachment_count;
        embeds += record.embed_count;
        fields += record.embed_field_count;
        stickers += record.sticker_count;
        components += record.component_count;
        reaction_max = maximum<u32>(reaction_max, record.reaction_count);
        attachment_max = maximum<u32>(attachment_max, record.attachment_count);
        embed_max = maximum<u32>(embed_max, record.embed_count);
        field_max = maximum<u32>(field_max, record.embed_field_count);
        component_max = maximum<u32>(component_max, record.component_count);
    }
    g_fields.s("rich_store");
    g_fields.kv("live", live);
    g_fields.kv("children", children);
    g_fields.kv("extent", rich.record_extent);
    g_fields.kv("record_bytes", sizeof(MessageRichRecord));
    g_fields.kv("reactions", reactions);
    g_fields.kv("reaction_max", reaction_max);
    g_fields.kv("attachments", attachments);
    g_fields.kv("attachment_max", attachment_max);
    g_fields.kv("embeds", embeds);
    g_fields.kv("embed_max", embed_max);
    g_fields.kv("embed_fields", fields);
    g_fields.kv("field_max", field_max);
    g_fields.kv("stickers", stickers);
    g_fields.kv("components", components);
    g_fields.kv("component_max", component_max);
    g_fields.s("\n");
}

} // namespace perf_probe_detail
} // namespace maicord::perf
