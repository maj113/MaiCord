#pragma once
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"

namespace maicord::app_detail {

// The pages of one large feature owner: zeroed and committed, so an owner costs memory only for the pages its
// feature touches. On Windows the system also tracks which of them are written (owner_wipe_written). Null when the
// system refuses.
__declspec(noinline) inline void* owner_pages_allocate(usize bytes) noexcept {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
#else
    return commit_pages(bytes);
#endif
}

// Wipes every page of [base, base + bytes) that can hold data: the pages the owner wrote (Windows' write watch), or
// the pages present or swapped out (/proc/self/pagemap on Linux). A page never written still reads as zeros, so the
// rest is left alone and closing a large owner faults none of it in. Wipes the whole range when the system cannot
// tell (a range allocated without the write watch, no pagemap).
__declspec(noinline) inline void owner_wipe_written(void* base, usize bytes) noexcept {
    u8* const start = static_cast<u8*>(base);
#if defined(_WIN32)
    for (u8* cursor = start; cursor < start + bytes;) {
        PVOID pages[64];
        ULONG_PTR count = 64;
        DWORD page_bytes = 0;
        const usize left = static_cast<usize>(start + bytes - cursor);
        if (GetWriteWatch(0, cursor, left, pages, &count, &page_bytes) != 0) {
            secure_clear_bytes(cursor, left);
            return;
        }
        for (ULONG_PTR index = 0; index < count; ++index) {
            u8* const page = static_cast<u8*>(pages[index]);
            secure_clear_bytes(page, minimum<usize>(page_bytes, static_cast<usize>(start + bytes - page)));
        }
        if (count < 64)
            return;
        cursor = static_cast<u8*>(pages[63]) + page_bytes;
    }
#else
    constexpr u64 kPagePresent = 1ull << 63, kPageSwapped = 1ull << 62;
    const usize page_bytes = process_startup().page_size;
    const long file = sys_open("/proc/self/pagemap", kOpenReadOnly);
    if (syscall_failed(file)) {
        secure_clear_bytes(start, bytes);
        return;
    }
    for (usize at = 0; at < bytes;) {
        u64 entries[64];
        const usize first = (reinterpret_cast<usize>(start) + at) / page_bytes;
        const usize wanted = minimum<usize>(64, (bytes - at + page_bytes - 1) / page_bytes);
        if (sys_pread(static_cast<int>(file), entries, wanted * sizeof(u64), first * sizeof(u64)) !=
            static_cast<long>(wanted * sizeof(u64))) {
            secure_clear_bytes(start + at, bytes - at);
            break;
        }
        for (usize index = 0; index < wanted; ++index, at += page_bytes)
            if (entries[index] & (kPagePresent | kPageSwapped))
                secure_clear_bytes(start + at, minimum(page_bytes, bytes - at));
    }
    sys_close(static_cast<int>(file));
#endif
}

template <typename T>
[[nodiscard]] inline T* owner_allocate() noexcept {
    return static_cast<T*>(owner_pages_allocate(sizeof(T)));
}

// Wipes an owner (it may hold typed text or tokens), returns its pages and clears the pointer; null
// is left alone.
template <typename T>
inline void owner_release(T*& owner) noexcept {
    if (!owner)
        return;
    owner_wipe_written(owner, sizeof(T));
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
