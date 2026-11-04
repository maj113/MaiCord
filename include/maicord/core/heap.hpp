#pragma once

#include "maicord/core/base.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process.hpp"
#else
#include "maicord/sys/linux/libc.hpp"
#endif

// Blocks from the process heap, for objects that are allocated and freed one at a time and must not be a page
// each (use core/scratch_pages.hpp or virtual_memory.hpp for page-granular storage). On Windows these are
// HeapAlloc and HeapFree on the process heap, so a call site compiles to the same calls; on Linux malloc, calloc
// and free of the C library the image links.

namespace maicord {

// `bytes` of storage, zeroed when asked; null when memory is exhausted.
__forceinline void* heap_allocate(usize bytes, bool zeroed = false) noexcept {
#if defined(_WIN32)
    return HeapAlloc(process_heap(), zeroed ? HEAP_ZERO_MEMORY : 0, bytes);
#else
    return zeroed ? calloc(1, bytes) : malloc(bytes);
#endif
}

// Frees a block heap_allocate returned; null is accepted on Linux only, so test it first.
__forceinline void heap_free(void* block) noexcept {
#if defined(_WIN32)
    HeapFree(process_heap(), 0, block);
#else
    free(block);
#endif
}

} // namespace maicord
