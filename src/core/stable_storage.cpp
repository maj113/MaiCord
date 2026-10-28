#include "maicord/core/stable_storage.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

bool valid_alignment(u32 value) noexcept {
    return value != 0 && (value & (value - 1u)) == 0;
}

} // namespace

bool stable_storage_initialize(StableStorage* storage, u32 reserved_capacity,
                               u32 initial_capacity, u32 element_bytes,
                               u32 alignment, bool zero_new) noexcept {
    if (!storage || storage->arena.base || reserved_capacity == 0 ||
        initial_capacity > reserved_capacity || !valid_alignment(alignment) || element_bytes == 0 ||
        (element_bytes & (alignment - 1u)) != 0) {
        return false;
    }
    // Both factors are u32; their full product always fits the x64 usize.
    const usize reserve_bytes = static_cast<usize>(reserved_capacity) * element_bytes;
    reset_value<StableStorage>(storage);
    if (!arena_initialize(&storage->arena, reserve_bytes, 0, 4 * 1024)) {
        return false;
    }
    storage->reserved_capacity = reserved_capacity;
    storage->element_bytes = element_bytes;
    storage->alignment = alignment;
    storage->zero_new = zero_new;
    if (initial_capacity != 0 &&
        !stable_storage_reserve(storage, initial_capacity)) {
        stable_storage_release(storage);
        return false;
    }
    return true;
}

bool stable_storage_reserve(StableStorage* storage,
                            u32 required_capacity) noexcept {
    if (!storage || !storage->arena.base || required_capacity == 0 ||
        required_capacity > storage->reserved_capacity) {
        return false;
    }
    if (required_capacity <= storage->capacity) {
        return true;
    }
    const usize bytes = static_cast<usize>(required_capacity - storage->capacity) * storage->element_bytes;
    const ArenaMark mark = arena_mark(&storage->arena);
    void* allocation = storage->zero_new
                           ? arena_allocate_zero(&storage->arena, bytes,
                                                 storage->alignment)
                           : arena_allocate(&storage->arena, bytes,
                                            storage->alignment);
    u8* expected = storage->arena.base +
                   static_cast<usize>(storage->capacity) *
                       storage->element_bytes;
    if (allocation != expected) {
        arena_rewind(&storage->arena, mark);
        return false;
    }
    storage->capacity = required_capacity;
    return true;
}

bool stable_storage_grow(StableStorage* storage,
                         u32 required_capacity) noexcept {
    if (!storage) {
        return false;
    }
    const u32 reserved_capacity = storage->reserved_capacity;
    if (required_capacity == 0 || required_capacity > reserved_capacity) {
        return false;
    }
    if (required_capacity <= storage->capacity) {
        return true;
    }
    u32 capacity = storage->capacity;
    if (capacity == 0) {
        capacity = minimum(reserved_capacity, required_capacity);
    }
    while (capacity < required_capacity) {
        const u32 doubled = capacity > reserved_capacity / 2u
                                ? reserved_capacity
                                : capacity * 2u;
        capacity = minimum(reserved_capacity,
                           maximum(doubled, required_capacity));
    }
    return stable_storage_reserve(storage, capacity);
}

bool stable_storage_shrink(StableStorage* storage, u32 capacity) noexcept {
    if (!storage || !storage->arena.base || capacity > storage->capacity) {
        return false;
    }
    if (capacity == storage->capacity) {
        return true;
    }
    if (!arena_trim(&storage->arena, static_cast<usize>(capacity) * storage->element_bytes)) {
        return false;
    }
    storage->capacity = capacity;
    return true;
}

void stable_storage_release(StableStorage* storage) noexcept {
    if (!storage) {
        return;
    }
    arena_release(&storage->arena);
    reset_value<StableStorage>(storage);
}

} // namespace maicord
