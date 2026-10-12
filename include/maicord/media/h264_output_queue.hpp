#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct H264Output { u64 duration_us; i32 poc; u8 surface; i64 presentation_time_us; };
struct H264OutputQueue {
    H264Output pictures[17]; // 16 DPB frames plus the current decode.
    u8 count;
    u8 leased{255};
};
u32 h264_pending_surfaces(const H264OutputQueue& queue) noexcept;
u8 h264_free_surface(const H264OutputQueue& queue, u32 references, u8 capacity) noexcept;
bool h264_output_ready(const H264OutputQueue& queue, u32 references,
    u8 max_reorder, u8 max_buffering) noexcept;
bool h264_output_push(H264OutputQueue* queue, H264Output picture) noexcept;
// Pop retains a lease; caller releases it only after enqueueing presentation
// on the shared context, before the next decode may overwrite that surface.
bool h264_output_pop(H264OutputQueue* queue, H264Output* picture) noexcept;

static_assert(sizeof(H264Output) == 24);
static_assert(sizeof(H264OutputQueue) == 416);

} // namespace maicord
