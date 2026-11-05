#pragma once

// The backend-neutral GPU vocabulary: handle types, GpuStatus and gpu_release from the build's backend, plus the
// status test and the capability record every backend fills.

#include "maicord/gpu/backend.hpp"

namespace maicord {

constexpr bool gpu_ok(GpuStatus status) noexcept {
    return status >= 0;
}

// What the device can do, read-only after device creation. Callers branch on this, never on the backend.
struct GpuCaps {
    u32 max_texture_size;
    u32 max_uniform_block_bytes;
    u32 max_compute_invocations; // per group
    u32 max_shared_bytes;        // per group
    u32 max_compute_storage_buffers;
    bool compute;
    bool storage_rgba8;          // RGBA8 unordered-access textures
    bool float_render_targets;   // RGBA16F and RGBA32F render targets
    bool half_render_targets;    // RGBA16F render targets
    bool float_linear;           // linear filtering of RGBA32F
    bool dual_source_blend;
    bool timer_query;
    bool map_no_overwrite_on_srv;
    bool dmabuf_import;
    bool surface_damage;         // presentation with damage rectangles
    bool buffer_age;
};

} // namespace maicord
