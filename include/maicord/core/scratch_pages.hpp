#pragma once

#include "maicord/core/win32_kernel.hpp"

namespace maicord {

// Zeroed pages for the large scratch of one cold call. Unlike a static buffer
// it costs no image commit, keeps nothing resident between calls and gives
// each caller its own storage. `data` is null when the commit failed.
template <typename T> struct ScratchPages {
    T* data = static_cast<T*>(VirtualAlloc(nullptr, sizeof(T), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));

    ScratchPages() noexcept = default;
    ScratchPages(const ScratchPages&) = delete;
    ScratchPages& operator=(const ScratchPages&) = delete;
    ~ScratchPages() {
        if (data)
            VirtualFree(data, 0, MEM_RELEASE);
    }
};

} // namespace maicord
