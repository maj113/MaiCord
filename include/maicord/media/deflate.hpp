#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kDeflateLiteralSymbolCapacity = 288;
inline constexpr u32 kDeflateDistanceSymbolCapacity = 32;
inline constexpr u32 kDeflateCodeSymbolCapacity = 19;
inline constexpr u32 kDeflateDynamicLengthCapacity = 320;
inline constexpr u32 kZlibStreamWindowBytes = 32 * 1024;

struct DeflateInputSpan {
    const u8* data;
    u32 bytes;
};

// Two-level decode tables with root widths 11 (literal/length), 8 (distance)
// and 7 (code lengths): the largest complete codes of 288/32/19 symbols need
// these entry counts, subtables included.
inline constexpr u32 kDeflateLitlenTableEntries = 2342;
inline constexpr u32 kDeflateDistanceTableEntries = 402;
inline constexpr u32 kDeflatePrecodeTableEntries = 128;
struct DeflateTables {
    u32 litlen[kDeflateLitlenTableEntries];
    u32 distance[kDeflateDistanceTableEntries];
    u32 precode[kDeflatePrecodeTableEntries];
    u8 litlen_maximum;
    u8 distance_maximum;
    u8 precode_maximum;
};

// The current dynamic block's decode tables and code lengths (about
// 11.5 KiB). Keep it in storage the inflating owner already holds, or take
// one from deflate_workspace_allocate; never on the stack. Needs no
// initialization.
struct DeflateWorkspace {
    DeflateTables tables;
    u8 lengths[kDeflateDynamicLengthCapacity];
};
// Process-heap workspace for one-shot inflates; null when the heap is out of
// memory. deflate_workspace_free accepts null.
DeflateWorkspace* deflate_workspace_allocate() noexcept;
void deflate_workspace_free(DeflateWorkspace* workspace) noexcept;

enum class ZlibInflateError : u8 {
    none,
    invalid_input,
    truncated,
    unsupported_method,
    unsupported_dictionary,
    invalid_header,
    invalid_block,
    invalid_huffman,
    invalid_symbol,
    invalid_distance,
    output_capacity,
    output_size,
    checksum,
    trailing_data,
    stream_failed,
};

struct ZlibInflateResult {
    u32 input_bytes;
    u32 output_bytes;
    u32 blocks;
    u32 literals;
    u32 copies;
    ZlibInflateError error;

    constexpr bool ok() const noexcept {
        return error == ZlibInflateError::none;
    }
};

ZlibInflateResult zlib_inflate(ConstSlice<DeflateInputSpan> input, Slice<u8> output,
                               u32 expected_output_bytes, DeflateWorkspace* workspace) noexcept;
// One complete RFC 1950 stream delivered in order through `sink` while only
// `window` stays resident: the 32 KiB dictionary plus room for the largest
// stored block. Output of any size streams through a fixed buffer; the sink
// sees every byte exactly once, in chunks, before the Adler-32 check (so a
// sink must tolerate a later checksum failure). A false sink return stops
// decoding with stream_failed. output_bytes reports the delivered total.
inline constexpr u32 kZlibWindowedMinimumBytes = 128u * 1024u;
using ZlibInflateSink = bool (*)(void* context, ConstSlice<u8> bytes) noexcept;
ZlibInflateResult zlib_inflate_windowed(ConstSlice<DeflateInputSpan> input, Slice<u8> window,
                                        u32 expected_output_bytes, ZlibInflateSink sink, void* context,
                                        DeflateWorkspace* workspace) noexcept;
// RFC 1952, including optional headers and concatenated members. Checks every
// member's CRC/size, bounds output, and never allocates. For zlib_inflate,
// expected_output_bytes == ~0u selects bounded, unknown-length HTTP decoding.
ZlibInflateResult gzip_inflate(ConstSlice<u8> input, Slice<u8> output,
                               DeflateWorkspace* workspace) noexcept;

static_assert(sizeof(DeflateInputSpan) == 16);
static_assert(sizeof(DeflateWorkspace) == 11812);

} // namespace maicord
