#pragma once

#include "maicord/core/interlocked.hpp"
#include "maicord/media/compression/deflate.hpp"

namespace maicord {

// Constant data that ships as one zlib stream and is inflated, on first use, once per process (a few hundred
// microseconds for the tables that use it). The block is zero-initialised static storage; the stream is named
// only where the first use can happen.
//
// Into static storage, for tables that hot code reads at constant addresses (the pages of a zero-initialised
// array cost nothing until the inflate writes them):
//
//     alignas(8) u8 g_bytes[kRawBytes];
//     InflatedBlock g_block;
//     bool ready() noexcept {
//         return g_block.state == 2 || inflated_block_into(g_block, kPacked, sizeof(kPacked), g_bytes, kRawBytes);
//     }
//
// Into private pages kept for the process lifetime (`data` points at them):
//
//     const u8* table() noexcept {
//         return g_block.state == 2 ? g_block.data : inflated_block_load(g_block, kPacked, sizeof(kPacked), kRawBytes);
//     }
//
// Any thread may ask: the first inflates while the others wait. False or null means memory or the stream
// failed, and every later call fails the same way. `publish`, when given, runs once on the inflated bytes
// before any thread sees them (to point the owner's tables at them).
struct InflatedBlock {
    const u8* data;
    volatile Atomic32 state; // 0 packed, 1 inflating, 2 ready, 3 unavailable
};

// /volatile:ms makes a read of `state` == 2 an acquire of the inflated bytes.
bool inflated_block_into(InflatedBlock& block, const u8* packed, u32 packed_bytes, u8* destination,
                         u32 bytes) noexcept;
const u8* inflated_block_load(InflatedBlock& block, const u8* packed, u32 packed_bytes, u32 bytes,
                              void (*publish)(const u8* data) noexcept = nullptr) noexcept;

} // namespace maicord
