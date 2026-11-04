#pragma once

#include "maicord/core/virtual_memory.hpp"

namespace maicord {

// Private zeroed pages, committed: one out-of-line call instead of VirtualAlloc's four arguments at every site. Null
// when the system refuses. Free with pages_release(pointer, bytes) (on Windows VirtualFree(pointer, 0, MEM_RELEASE)).
__declspec(noinline) inline void* commit_pages(usize bytes) noexcept {
    return pages_allocate(bytes);
}
// A T's worth of them by default, or `bytes` for a block of Ts; the T is only the pointer type.
template <typename T> __forceinline T* commit_pages(usize bytes = sizeof(T)) noexcept {
    return static_cast<T*>(commit_pages(bytes));
}

// Zeroed pages for the large scratch of one cold call. Unlike a static buffer
// it costs no image commit, keeps nothing resident between calls and gives
// each caller its own storage. `data` is null when the commit failed.
template <typename T> struct ScratchPages {
    T* data = static_cast<T*>(commit_pages(sizeof(T)));

    ScratchPages() noexcept = default;
    ScratchPages(const ScratchPages&) = delete;
    ScratchPages& operator=(const ScratchPages&) = delete;
    ~ScratchPages() {
        if (data)
            pages_release(data, sizeof(T));
    }
};

} // namespace maicord
