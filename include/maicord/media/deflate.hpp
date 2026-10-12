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

// Code lengths of the current dynamic block. Decode tables live on the
// inflating thread's stack (about 11 KiB per call).
struct DeflateWorkspace {
    u8 lengths[kDeflateDynamicLengthCapacity];
};

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
    invalid_flush,
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

// One RFC 1950 stream split at Z_SYNC_FLUSH boundaries. The dictionary is
// connection-owned and a decode error poisons the stream; callers must drop
// it rather than publish partial output or attempt recovery.
struct ZlibStreamInflater {
    DeflateWorkspace workspace;
    u8 window[kZlibStreamWindowBytes];
    u32 window_bytes;
    u32 window_cursor;
    u32 window_size;
    bool initialized;
    bool failed;
    u8 reserved[2];
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
bool zlib_stream_initialize(ZlibStreamInflater* stream) noexcept;
void zlib_stream_clear(ZlibStreamInflater* stream) noexcept;
ZlibInflateResult zlib_stream_inflate_flush(ZlibStreamInflater* stream, ConstSlice<DeflateInputSpan> input,
                                            Slice<u8> output) noexcept;
// Synchronous growth at the next output-write boundary, without rewinding the
// reader or dictionary. The adapter must preserve the base and written prefix.
// Failure poisons the stream just like a fixed output-capacity failure.
using ZlibStreamGrowOutput = bool (*)(void*, u32 required_bytes, Slice<u8>* backed) noexcept;
ZlibInflateResult zlib_stream_inflate_flush_growing(ZlibStreamInflater* stream,
    ConstSlice<DeflateInputSpan> input, Slice<u8>* output, u32 maximum_output_bytes,
    ZlibStreamGrowOutput grow, void* context) noexcept;

static_assert(sizeof(DeflateInputSpan) == 16);
static_assert(sizeof(DeflateWorkspace) == 320);
static_assert(sizeof(ZlibStreamInflater) == 33'104);

} // namespace maicord
