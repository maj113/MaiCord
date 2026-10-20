#pragma once
// Startup attribution for MaiCordPerfProbe (tools/perf): a main-thread
// timeline of named points, every DLL the process maps, timed system calls
// (window, D3D11, DXGI, DirectComposition, COM, module loads) and the D3D11
// objects created before the first presented frame. Shipping builds never
// include this header; MAICORD_PERF_MARK compiles to nothing there.
#include "maicord/core/base.hpp"

namespace maicord::perf {

// First call at process entry, before anything else runs.
void startup_trace_initialize() noexcept;
// A named main-thread point; only the first occurrence of each name before
// the first presented frame is kept.
void startup_mark(const char* name) noexcept;
// The first presented frame ends the attribution window.
void startup_first_present() noexcept;
// Writes the startup lines to standard output (ahead of the probe report).
void startup_trace_report() noexcept;

// GPU resources the client created through the device (buffers, 2D textures)
// and the swap chain, by class; sizes are computed from the descriptions, so
// driver padding and internal allocations are not included.
enum GpuResourceClass : u32 {
    gpu_swap_chain,
    gpu_texture_default,
    gpu_texture_dynamic,
    gpu_texture_staging,
    gpu_texture_immutable,
    gpu_buffer_default,
    gpu_buffer_dynamic,
    gpu_buffer_staging,
    gpu_buffer_immutable,
    gpu_class_count
};
inline constexpr const char* kGpuResourceClassNames[] = {
    "swap_chain",     "texture_default", "texture_dynamic", "texture_staging",  "texture_immutable",
    "buffer_default", "buffer_dynamic",  "buffer_staging",  "buffer_immutable"};
struct GpuResourceTotals {
    u64 live_bytes[gpu_class_count], live_count[gpu_class_count];
    u64 created_bytes[gpu_class_count], created_count[gpu_class_count];
};
struct GpuResourceInfo {
    u64 bytes, caller_rva;
    u32 klass, width, height, format, bind;
};
void startup_gpu_totals(GpuResourceTotals* totals) noexcept;
// Resources created while set are the probe's own and are not recorded.
void startup_gpu_ignore(bool ignore) noexcept;
// Live resources, largest first; returns how many were written.
u32 startup_gpu_largest(GpuResourceInfo* resources, u32 capacity) noexcept;
// Allocation tags of immediate-context calls the probe hooks, and the name of
// any tag (perf_owners.hpp).
inline constexpr u32 kStartupTagMap = 42;
inline constexpr u32 kStartupTagUpdate = 43;
inline constexpr u32 kStartupTagFlush = 44;
const char* startup_tag_name(u32 tag) noexcept;
// Memory this file owns, excluded from the client attribution.
void startup_probe_range(u64* begin, u64* end) noexcept;

} // namespace maicord::perf
