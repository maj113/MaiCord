#pragma once

#include "maicord/media/image/jpeg.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/intrin.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
namespace jpeg_detail {

inline constexpr u8 kZigzagToRaster[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// Payload of one APP2 segment, already bounded by segment_bounds().
struct JpegIccChunk {
    u32 encoded_offset;
    u32 bytes;
};

struct JpegScan {
    u32 entropy_offset;
    u32 entropy_bytes;
    u32 data_units;
    u16 restart_interval;
    u16 restart_markers;
    u8 component_indices[kJpegComponentMaximum];
    u8 dc_tables[kJpegComponentMaximum];
    u8 ac_tables[kJpegComponentMaximum];
    u8 component_count;
    u8 spectral_start;
    u8 spectral_end;
    u8 approximation_high;
    u8 approximation_low;
};

struct JpegEntropyReader {
    ConstSlice<u8> bytes;
    u64 buffer;      // next bit at bit 63; past `bits`: zero, or the leading bits of the byte at offset
    u32 offset;      // next input byte not yet in the buffer
    u32 bits;        // valid bits in buffer
    JpegError stop;  // truncated (segment end) or restart (marker) once refills stopped
    JpegError error;
};

JpegEntropyReader entropy_reader(ConstSlice<u8> bytes) noexcept;

// Fills the buffer to at least 57 bits unless the segment ends or a marker follows.
__forceinline void entropy_refill(JpegEntropyReader* reader) noexcept {
    const u8* const data = reader->bytes.data;
    const u32 count = reader->bytes.count;
    if (reader->stop == JpegError::none && reader->offset + 8u <= count) [[likely]] {
        // Eight bytes without 0xFF are plain entropy data: append as many as
        // fit at once. Bits of the next byte may land below `bits`; they are
        // that byte's own bits, ORed in again when it is appended.
        const u64 raw = static_cast<u64>(
            _mm_cvtsi128_si64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(data + reader->offset))));
        if (((~raw - 0x0101010101010101ull) & raw & 0x8080808080808080ull) == 0) [[likely]] {
            const u32 take = (64u - reader->bits) >> 3;
            reader->buffer |= _byteswap_uint64(raw) >> reader->bits;
            reader->offset += take;
            reader->bits += take * 8u;
            return;
        }
    }
    // The end of the segment and the bytes around an 0xFF: byte at a time.
    // Counted and not unrolled: the unrolled copy at every bit read cost 20 KB
    // (the `bits <= 56` loop form ignored the unroll pragma). An out-of-line
    // call was slower than this loop. Each byte adds eight bits, so
    // (64 - bits) / 8 bytes fit.
    const u32 room = (64u - reader->bits) >> 3;
#pragma clang loop unroll(disable)
    for (u32 taken = 0; taken < room && reader->stop == JpegError::none; ++taken) {
        if (reader->offset >= count) {
            reader->stop = JpegError::truncated;
            break;
        }
        const u8 value = data[reader->offset];
        if (value == 0xFF) {
            u32 next = reader->offset + 1;
            while (next < count && data[next] == 0xFF) {
                ++next;
            }
            if (next >= count || data[next] != 0) {
                reader->stop = JpegError::restart;
                break;
            }
            reader->offset = next + 1;
        } else {
            ++reader->offset;
        }
        reader->buffer |= static_cast<u64>(value) << (56u - reader->bits);
        reader->bits += 8;
    }
}

__forceinline void entropy_consume(JpegEntropyReader* reader, u32 count) noexcept {
    reader->buffer <<= count;
    reader->bits -= count;
}

__forceinline u32 entropy_read_bit(JpegEntropyReader* reader) noexcept {
    if (reader->error != JpegError::none) [[unlikely]] {
        return 0;
    }
    if (reader->bits == 0) [[unlikely]] {
        entropy_refill(reader);
        if (reader->bits == 0) [[unlikely]] {
            reader->error = reader->stop;
            return 0;
        }
    }
    const u32 bit = static_cast<u32>(reader->buffer >> 63);
    entropy_consume(reader, 1);
    return bit;
}

// count is a nibble of a Huffman symbol (at most 15).
__forceinline u32 entropy_read_bits(JpegEntropyReader* reader, u32 count) noexcept {
    if (count == 0 || reader->error != JpegError::none) {
        return 0;
    }
    if (reader->bits < count) [[unlikely]] {
        entropy_refill(reader);
        if (reader->bits < count) [[unlikely]] {
            reader->error = reader->stop;
            return 0;
        }
    }
    const u32 value = static_cast<u32>(reader->buffer >> (64u - count));
    entropy_consume(reader, count);
    return value;
}

__forceinline bool entropy_align(JpegEntropyReader* reader) noexcept {
    if (reader->error != JpegError::none) {
        return false;
    }
    // Bits of a partly consumed byte: every refill adds whole bytes.
    const u32 partial = reader->bits & 7u;
    if (partial != 0) {
        const u32 mask = (1u << partial) - 1u;
        if (static_cast<u32>(reader->buffer >> (64u - partial)) != mask) {
            reader->error = JpegError::entropy;
            return false;
        }
        entropy_consume(reader, partial);
    }
    return true;
}

// Aligned and every loaded byte consumed: the input position a bit-serial reader would have.
__forceinline bool entropy_at_end(JpegEntropyReader* reader) noexcept {
    return entropy_align(reader) && reader->bits == 0 && reader->offset == reader->bytes.count;
}

__forceinline bool entropy_consume_restart(JpegEntropyReader* reader, u8 expected) noexcept {
    // Buffered whole bytes are entropy data a bit-serial reader would find
    // where the marker must start.
    if (!entropy_align(reader) || reader->bits != 0 || reader->offset >= reader->bytes.count ||
        reader->bytes[reader->offset] != 0xFF) {
        if (reader->error == JpegError::none) {
            reader->error = JpegError::restart;
        }
        return false;
    }
    while (reader->offset < reader->bytes.count && reader->bytes[reader->offset] == 0xFF) {
        ++reader->offset;
    }
    if (reader->offset >= reader->bytes.count ||
        reader->bytes[reader->offset++] != static_cast<u8>(0xD0u + expected)) {
        reader->error = JpegError::restart;
        return false;
    }
    reader->buffer = 0;
    reader->stop = JpegError::none;
    return true;
}

// A 9-bit lookahead over one Huffman table. An entry holds the code length
// (bits 8-11) and symbol of the shortest code prefixing the index, filled in
// increasing length order exactly as a bit-serial match finds it; zero means
// no code of nine bits or fewer matches.
inline constexpr u32 kJpegLookaheadBits = 9;
inline constexpr u32 kJpegEndOfBlock = 0x8000u;
struct JpegDecodeTable {
    const JpegHuffmanTable* table;
    u16 lookahead[1u << kJpegLookaheadBits];
};
struct JpegAcDecodeTable : JpegDecodeTable {
    // Baseline AC: a codeword and its magnitude bits within the lookahead.
    // value << 16 | run << 5 | total bits; zero when they do not fit.
    // kJpegEndOfBlock marks the end-of-block codeword (total = its length).
    u32 coefficient[1u << kJpegLookaheadBits];
};

void finalize_icc_profile(ConstSlice<u8> encoded, const JpegIccChunk* chunks, u32 chunk_count,
                          u32 declared_chunk_count, bool marker_seen, bool transport_malformed,
                          const JpegImage& image, JpegIccWorkspace* workspace) noexcept;
bool build_huffman_table(JpegHuffmanTable* table, const u8* counts, const u8* symbols,
                         u32 symbol_count) noexcept;
u32 scan_component_columns(const JpegImage& image, const JpegComponent& component) noexcept;
JpegError validate_progression(const JpegScan& scan, u8 progression[kJpegComponentMaximum][64]) noexcept;
JpegError find_entropy_end(ConstSlice<u8> encoded, u32 start, JpegScan* scan, u32* next_marker) noexcept;
void build_component_tables(const JpegImage& image, JpegDecodeTable* dc, JpegAcDecodeTable* ac) noexcept;
bool decode_block(JpegEntropyReader* reader, const JpegDecodeTable& dc_table, const JpegAcDecodeTable& ac_table,
                  i32* predictor, JpegCoefficientBlock* block, u32* huffman_symbols) noexcept;
bool decode_progressive_scan(ConstSlice<u8> encoded, const JpegImage& image, const JpegTables& tables,
                             const JpegScan& scan, JpegCoefficientBlock* blocks,
                             JpegDecodeResult* result) noexcept;
JpegParseResult parse_failure(JpegParseResult result, JpegError error) noexcept;
JpegParseResult parse_jpeg(ConstSlice<u8> encoded, JpegImage* image, JpegWorkspace* workspace,
                           bool thumbnail) noexcept;
JpegDecodeResult decode_progressive_scans(ConstSlice<u8> encoded, const JpegImage& image,
                                          JpegCoefficientBlock* blocks) noexcept;

} // namespace jpeg_detail
} // namespace maicord
