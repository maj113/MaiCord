#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// The largest window a frame may declare (windowLog 23). Discord's Gateway
// zstd-stream frames declare 64 KiB.
inline constexpr u32 kZstdMaximumWindowBytes = 8u << 20;
inline constexpr u32 kZstdBlockMaximumBytes = 128u << 10;
// Storage a message needs past its output: one block, a guard and the
// block's decoded literals.
inline constexpr u32 kZstdLiteralGuardBytes = 64;
inline constexpr u32 kZstdMessageSlackBytes = 2 * kZstdBlockMaximumBytes + kZstdLiteralGuardBytes;

enum class ZstdError : u8 {
    none,
    invalid_input,
    poisoned,
    truncated,
    bad_magic,
    unsupported_dictionary,
    window_too_large,
    invalid_frame_header,
    invalid_block,
    invalid_literals,
    invalid_huffman,
    invalid_sequences,
    content_size,
    checksum,
    output_capacity,
};

struct ZstdDecodeResult {
    u32 output_bytes;
    u32 blocks;
    ZstdError error;

    constexpr bool ok() const noexcept {
        return error == ZstdError::none;
    }
};

struct ZstdFseEntry {
    u16 baseline;
    u8 symbol;
    u8 bits;
};
struct ZstdHuffmanEntry {
    u8 symbol;
    u8 bits;
};

// Grows the storage to at least required bytes at the same base, keeping its contents.
using ZstdGrowStorage = bool (*)(void* context, u32 required_bytes, Slice<u8>* storage) noexcept;

// One RFC 8878 stream whose frame spans many messages (Discord's zstd-stream):
// every message holds whole blocks and decodes into one contiguous output.
// The storage keeps the frame's window in front of each message's output, so
// matches read it in place; when a message would not fit, the window slides
// to the front first. Entropy tables and repeat offsets carry across
// messages. Any error poisons the stream; drop the connection.
struct ZstdStreamDecoder {
    Slice<u8> storage;
    ZstdGrowStorage grow;
    void* grow_context;
    u32 maximum_storage;
    u32 position;      // end of the retained output
    u32 frame_start;   // first storage byte a match may reach in the current frame
    u32 window_bytes;
    u32 block_maximum;
    u64 frame_output;
    u64 content_size;
    u32 repeat[3];
    const ZstdFseEntry* literal_lengths;
    const ZstdFseEntry* offsets;
    const ZstdFseEntry* match_lengths;
    u8 literal_lengths_log, offsets_log, match_lengths_log, huffman_bits;
    bool in_frame, has_checksum, has_content_size, failed, initialized;
    u64 hash_state[4];
    u64 hash_length;
    u8 hash_buffer[32];
    u32 hash_buffered;
    ZstdHuffmanEntry huffman[1u << 11];
    ZstdFseEntry literal_length_table[1u << 9];
    ZstdFseEntry offset_table[1u << 8];
    ZstdFseEntry match_length_table[1u << 9];
    ZstdFseEntry default_literal_lengths[1u << 6];
    ZstdFseEntry default_offsets[1u << 5];
    ZstdFseEntry default_match_lengths[1u << 6];
};

// storage must hold at least kZstdMessageSlackBytes; grow (optional) extends
// it up to maximum_storage bytes.
bool zstd_stream_initialize(ZstdStreamDecoder* decoder, Slice<u8> storage, u32 maximum_storage,
                            ZstdGrowStorage grow, void* context) noexcept;
// Erases the retained output and every table; the decoder must be initialized again.
void zstd_stream_clear(ZstdStreamDecoder* decoder) noexcept;
// Decodes one message. output views the storage until the next call; at most
// maximum_output bytes are accepted.
ZstdDecodeResult zstd_stream_decompress(ZstdStreamDecoder* decoder, ConstSlice<u8> input, u32 maximum_output,
                                        ConstSlice<u8>* output) noexcept;
// Moves the window to the front of storage and returns the bytes still in
// use; storage past them may then be decommitted (pass the smaller view).
u32 zstd_stream_compact(ZstdStreamDecoder* decoder) noexcept;
bool zstd_stream_set_storage(ZstdStreamDecoder* decoder, Slice<u8> storage) noexcept;

} // namespace maicord
