#pragma once

#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {

// A request body too large for the stack: demand-committed pages the REST queue copies from before the
// owner returns. The destructor erases the first `used` bytes (set it to what the writer produced; the
// body holds the account's text and images) and returns the pages. `data` is null when the system
// refused the pages, or after REST took ownership (rest_client_submit_large_owned nulls it).
struct RequestBodyBank {
    u8* data;
    u32 capacity;
    u32 used = 0;

    explicit RequestBodyBank(u32 bytes) noexcept
        : data(static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))),
          capacity(bytes) {}
    RequestBodyBank(const RequestBodyBank&) = delete;
    RequestBodyBank& operator=(const RequestBodyBank&) = delete;
    ~RequestBodyBank() noexcept {
        if (!data)
            return;
        secure_clear_bytes(data, used);
        VirtualFree(data, 0, MEM_RELEASE);
    }
    Slice<u8> all() const noexcept {
        return {data, capacity};
    }
};

} // namespace maicord
