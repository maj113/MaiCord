#include "maicord/core/stable_storage.hpp"

namespace maicord {
namespace {

bool valid_alignment(u32 value) noexcept {
    return value != 0 && (value & (value - 1u)) == 0;
}

inline bool storage_bytes(u32 count, u32 element_bytes, usize* bytes) noexcept {
    // Both factors are u32; their full product always fits the x64 usize.
    if (element_bytes == 0) {
        return false;
    }
    *bytes = static_cast<usize>(count) * element_bytes;
    return true;
}

} // namespace

bool stable_storage_initialize(StableStorage* storage, u32 reserved_capacity,
                               u32 initial_capacity, u32 element_bytes,
                               u32 alignment, bool zero_new) noexcept {
    usize reserve_bytes = 0;
    if (!storage || storage->arena.base || reserved_capacity == 0 ||
        initial_capacity > reserved_capacity || !valid_alignment(alignment) ||
        (element_bytes & (alignment - 1u)) != 0 ||
        !storage_bytes(reserved_capacity, element_bytes, &reserve_bytes)) {
        return false;
    }
    *storage = StableStorage{};
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
        required_capacity > stable_storage_reserved_capacity(*storage)) {
        return false;
    }
    if (required_capacity <= storage->capacity) {
        return true;
    }
    usize bytes = 0;
    if (!storage_bytes(required_capacity - storage->capacity,
                       storage->element_bytes, &bytes)) {
        return false;
    }
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
    const u32 reserved_capacity = stable_storage_reserved_capacity(*storage);
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

void stable_storage_release(StableStorage* storage) noexcept {
    if (!storage) {
        return;
    }
    arena_release(&storage->arena);
    *storage = StableStorage{};
}

} // namespace maicord
