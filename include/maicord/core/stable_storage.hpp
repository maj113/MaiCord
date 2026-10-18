#pragma once

#include "maicord/core/arena.hpp"

namespace maicord {

// Fixed virtual address, page-committed array storage. Capacity growth never
// reallocates or copies existing elements; callers choose whether new slots
// must be zeroed.
struct StableStorage {
    Arena arena;
    u32 capacity;
    u32 reserved_capacity;
    u32 element_bytes;
    u32 alignment;
    bool zero_new;
};

bool stable_storage_initialize(StableStorage* storage, u32 reserved_capacity,
                               u32 initial_capacity, u32 element_bytes,
                               u32 alignment, bool zero_new = false) noexcept;
bool stable_storage_reserve(StableStorage* storage,
                            u32 required_capacity) noexcept;
bool stable_storage_grow(StableStorage* storage, u32 required_capacity) noexcept;
void stable_storage_release(StableStorage* storage) noexcept;

template <typename T>
T* stable_storage_data(StableStorage& storage) noexcept {
    return reinterpret_cast<T*>(storage.arena.base);
}

template <typename T>
const T* stable_storage_data(const StableStorage& storage) noexcept {
    return reinterpret_cast<const T*>(storage.arena.base);
}

} // namespace maicord
