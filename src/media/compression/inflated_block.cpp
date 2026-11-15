#include "maicord/media/compression/inflated_block.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/compression/deflate.hpp"
#include "maicord/media/compression/zstd.hpp"

namespace maicord {
namespace {

// Both return the unpacked bytes (`destination`, or new private pages when that is null), or null.
u8* unpack_zlib(const u8* packed, u32 packed_bytes, u8* destination, u32 bytes) noexcept {
    u8* data = destination ? destination : static_cast<u8*>(commit_pages(bytes));
    DeflateWorkspace* workspace = data ? deflate_workspace_allocate() : nullptr;
    const DeflateInputSpan input{packed, packed_bytes};
    const bool ok = workspace && zlib_inflate({&input, 1}, {data, bytes}, bytes, workspace).ok();
    deflate_workspace_free(workspace);
    if (ok)
        return data;
    if (data && !destination)
        pages_release(data, bytes);
    return nullptr;
}

// The zstd decoder writes the literals of each block after the output of the block, so the output needs
// room that a destination does not have. The frame decodes into new pages that also hold the decoder. Without
// a destination the first pages become the result, and the pages after the data go back to the system.
u8* unpack_zstd(const u8* packed, u32 packed_bytes, u8* destination, u32 bytes) noexcept {
    const u32 storage_bytes = (bytes + kZstdMessageSlackBytes + 63) & ~63u;
    const usize scratch_bytes = storage_bytes + sizeof(ZstdStreamDecoder);
    u8* storage = static_cast<u8*>(commit_pages(scratch_bytes));
    if (!storage)
        return nullptr;
    auto* decoder = reinterpret_cast<ZstdStreamDecoder*>(storage + storage_bytes);
    ConstSlice<u8> output{};
    const bool ok = zstd_stream_initialize(decoder, {storage, storage_bytes}, storage_bytes, nullptr, nullptr) &&
                    zstd_stream_decompress(decoder, {packed, packed_bytes}, bytes, &output).ok() &&
                    output.count == bytes;
    if (ok && !destination) {
        const usize kept = (usize{bytes} + 4095) & ~usize{4095};
        pages_decommit(storage + kept, scratch_bytes - kept);
        return storage;
    }
    if (ok)
        copy_bytes(destination, storage, bytes);
    pages_release(storage, scratch_bytes);
    return ok ? destination : nullptr;
}

// The first caller unpacks (into `destination`, or into new private pages when that is null) and publishes
// the result; the others wait. Returns the unpacked bytes, or null once the block is unavailable. A zstd frame
// starts with the byte 0x28 of its magic number. The zlib streams of the packers start with 0x78.
const u8* inflate_once(InflatedBlock& block, const u8* packed, u32 packed_bytes, u8* destination, u32 bytes,
                       void (*publish)(const u8* data) noexcept) noexcept {
    for (;;) {
        const Atomic32 state = block.state;
        if (state == 2)
            return block.data;
        if (state == 3)
            return nullptr;
        if (state == 0 && InterlockedCompareExchange(&block.state, 1, 0) == 0) {
            u8* data = (packed[0] == 0x28 ? unpack_zstd : unpack_zlib)(packed, packed_bytes, destination, bytes);
            if (data) {
                if (publish)
                    publish(data);
                block.data = data;
            }
            block.state = data ? 2 : 3;
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
