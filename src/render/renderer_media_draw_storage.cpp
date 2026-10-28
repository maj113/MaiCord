#include "maicord/render/renderer_media_draw_storage.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/media_atlas.hpp"

namespace maicord {
namespace {

// Callers grow only past the current capacity, so one doubling step suffices.
u32 grown_capacity(u32 current, u32 required, u32 maximum_capacity) noexcept {
    return required > maximum_capacity ? 0 : minimum(maximum_capacity, maximum(current * 2u, required));
}

} // namespace

bool renderer_media_draw_storage_initialize(
    RendererMediaDrawStorage* storage) noexcept {
    if (!storage) {
        return false;
    }
    reset_value<RendererMediaDrawStorage>(storage);
    if (!stable_storage_initialize(
            &storage->quads, kRendererMediaQuadCapacity,
            kRendererMediaInitialQuadCapacity, sizeof(MediaQuad),
            alignof(MediaQuad)) ||
        !stable_storage_initialize(
            &storage->handles, kRendererMediaQuadCapacity,
            kRendererMediaInitialQuadCapacity, sizeof(MediaAtlasHandle),
            alignof(MediaAtlasHandle)) ||
        !stable_storage_initialize(
            &storage->domains, kRendererMediaQuadCapacity,
            kRendererMediaInitialQuadCapacity, sizeof(u8), alignof(u8)) ||
        !stable_storage_initialize(
            &storage->batches, kRendererMediaBatchCapacity,
            kRendererMediaInitialBatchCapacity, sizeof(MediaDrawBatch),
            alignof(MediaDrawBatch))) {
        renderer_media_draw_storage_release(storage);
        return false;
    }
    storage->quad_data = stable_storage_data<MediaQuad>(storage->quads);
    storage->handle_data =
        stable_storage_data<MediaAtlasHandle>(storage->handles);
    storage->domain_data = stable_storage_data<u8>(storage->domains);
    storage->batch_data =
        stable_storage_data<MediaDrawBatch>(storage->batches);
    storage->quad_capacity = storage->quads.capacity;
    storage->batch_capacity = storage->batches.capacity;
    return true;
}

bool renderer_media_draw_storage_reserve_quads(
    RendererMediaDrawStorage* storage, u32 required_capacity) noexcept {
    if (!storage || !storage->quad_data || required_capacity == 0) {
        return false;
    }
    if (required_capacity <= storage->quad_capacity) {
        return true;
    }
    const u32 capacity = grown_capacity(
        storage->quad_capacity, required_capacity,
        kRendererMediaQuadCapacity);
    if (capacity == 0) {
        return false;
    }
    const u32 previous = storage->quad_capacity;
    const ArenaMark quad_mark = arena_mark(&storage->quads.arena);
    const ArenaMark handle_mark = arena_mark(&storage->handles.arena);
    const ArenaMark domain_mark = arena_mark(&storage->domains.arena);
    if (!stable_storage_reserve(&storage->quads, capacity) ||
        !stable_storage_reserve(&storage->handles, capacity) ||
        !stable_storage_reserve(&storage->domains, capacity)) {
        arena_rewind(&storage->quads.arena, quad_mark);
        arena_rewind(&storage->handles.arena, handle_mark);
        arena_rewind(&storage->domains.arena, domain_mark);
        storage->quads.capacity = previous;
        storage->handles.capacity = previous;
        storage->domains.capacity = previous;
        return false;
    }
    storage->quad_capacity = capacity;
    return true;
}

bool renderer_media_draw_storage_reserve_batches(
    RendererMediaDrawStorage* storage, u32 required_capacity) noexcept {
    if (!storage || !storage->batch_data || required_capacity == 0) {
        return false;
    }
    if (required_capacity <= storage->batch_capacity) {
        return true;
    }
    const u32 capacity = grown_capacity(
        storage->batch_capacity, required_capacity,
        kRendererMediaBatchCapacity);
    if (capacity == 0) {
        return false;
    }
    if (!stable_storage_reserve(&storage->batches, capacity)) {
        return false;
    }
    storage->batch_capacity = capacity;
    return true;
}

void renderer_media_draw_storage_release(
    RendererMediaDrawStorage* storage) noexcept {
    if (!storage) {
        return;
    }
    stable_storage_release(&storage->quads);
    stable_storage_release(&storage->handles);
    stable_storage_release(&storage->domains);
    stable_storage_release(&storage->batches);
    reset_value<RendererMediaDrawStorage>(storage);
}

} // namespace maicord
