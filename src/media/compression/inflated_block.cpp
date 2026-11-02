#include "maicord/media/compression/inflated_block.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

// The first caller inflates (into `destination`, or into new private pages when that is null) and publishes
// the result; the others wait. Returns the inflated bytes, or null once the block is unavailable.
const u8* inflate_once(InflatedBlock& block, const u8* packed, u32 packed_bytes, u8* destination, u32 bytes,
                       void (*publish)(const u8* data) noexcept) noexcept {
    for (;;) {
        const long state = block.state;
        if (state == 2)
            return block.data;
        if (state == 3)
            return nullptr;
        if (state == 0 && InterlockedCompareExchange(&block.state, 1, 0) == 0) {
            u8* data = destination ? destination
                                   : static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                                                   PAGE_READWRITE));
            DeflateWorkspace* workspace = data ? deflate_workspace_allocate() : nullptr;
            const DeflateInputSpan input{packed, packed_bytes};
            const bool ok = workspace && zlib_inflate({&input, 1}, {data, bytes}, bytes, workspace).ok();
            deflate_workspace_free(workspace);
            if (ok) {
                if (publish)
                    publish(data);
                block.data = data;
            } else if (data && !destination) {
                VirtualFree(data, 0, MEM_RELEASE);
            }
            block.state = ok ? 2 : 3;
            return block.data;
        }
        YieldProcessor();
    }
}

} // namespace

bool inflated_block_into(InflatedBlock& block, const u8* packed, u32 packed_bytes, u8* destination,
                         u32 bytes) noexcept {
    return inflate_once(block, packed, packed_bytes, destination, bytes, nullptr) != nullptr;
}

const u8* inflated_block_load(InflatedBlock& block, const u8* packed, u32 packed_bytes, u32 bytes,
                              void (*publish)(const u8* data) noexcept) noexcept {
    return inflate_once(block, packed, packed_bytes, nullptr, bytes, publish);
}

} // namespace maicord
