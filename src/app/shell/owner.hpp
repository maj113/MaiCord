#pragma once
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {

// Storage of one large feature owner: zeroed pages committed on demand, so an owner costs memory only
// while its feature is open. Null when the system refuses.
template <typename T>
[[nodiscard]] inline T* owner_allocate() noexcept {
    return commit_pages<T>();
}

// Wipes an owner (it may hold typed text or tokens), returns its pages and clears the pointer; null
// is left alone.
template <typename T>
inline void owner_release(T*& owner) noexcept {
    if (!owner)
        return;
    secure_clear_bytes(owner, sizeof(T));
    pages_release(owner);
    owner = nullptr;
}

// `owner` once it exists, allocated on first use; null when the system refuses.
template <typename T>
[[nodiscard]] inline T* owner_ensure(T*& owner) noexcept {
    if (!owner)
        owner = owner_allocate<T>();
    return owner;
}

} // namespace maicord::app_detail
