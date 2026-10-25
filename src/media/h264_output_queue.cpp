#include "maicord/media/h264_output_queue.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
u32 h264_pending_surfaces(const H264OutputQueue& queue) noexcept {
    u32 mask = 0;
    for (u32 i = 0; i < queue.count && i < 17; ++i)
        if (queue.pictures[i].surface < 32) mask |= 1u << queue.pictures[i].surface;
    return mask;
}
u8 h264_free_surface(const H264OutputQueue& queue, u32 references, u8 capacity) noexcept {
    if (!capacity || capacity > 32 || queue.count > 17) return 255;
    u32 busy = references | h264_pending_surfaces(queue);
    if (queue.leased < 32) busy |= 1u << queue.leased;
    for (u8 i = 0; i < capacity; ++i) if (!(busy & (1u << i))) return i;
    return 255;
}
bool h264_output_ready(const H264OutputQueue& queue, u32 references,
    u8 max_reorder, u8 max_buffering) noexcept {
    if (!queue.count) return false;
    if (queue.count > max_reorder) return true;
    u32 mask = references | h264_pending_surfaces(queue), count = 0;
    while (mask) { mask &= mask - 1u; ++count; }
    return count > max_buffering;
}
bool h264_output_push(H264OutputQueue* queue, H264Output picture) noexcept {
    if (!queue || queue->count >= 17 || picture.surface >= 32 || picture.surface == queue->leased ||
        (h264_pending_surfaces(*queue) & (1u << picture.surface))) return false;
    queue->pictures[queue->count++] = picture;
    return true;
}
bool h264_output_pop(H264OutputQueue* queue, H264Output* picture) noexcept {
    if (!queue || !picture || !queue->count || queue->count > 17 || queue->leased != 255) return false;
    u32 selected = 0;
    for (u32 i = 1; i < queue->count; ++i)
        if (queue->pictures[i].poc < queue->pictures[selected].poc) selected = i;
    *picture = queue->pictures[selected]; queue->leased = picture->surface;
    for (u32 i = selected + 1; i < queue->count; ++i) queue->pictures[i - 1] = queue->pictures[i];
    reset_value(&queue->pictures[--queue->count]);
    return true;
}
} // namespace maicord
