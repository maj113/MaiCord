#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/intrin.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {

// One 64-byte chunk as bit masks (bit i = byte i).
struct ChunkMasks {
    u64 quotes;
    u64 backslashes;
    u64 structurals;
    u64 controls;
};

__forceinline u64 lane_mask(__m128i lane, u32 shift) noexcept {
    return static_cast<u64>(static_cast<u32>(_mm_movemask_epi8(lane))) << shift;
}

// {}[]:, by nibble lookup: a byte is structural when the classes selected by
// its low and high nibbles intersect (',' 2C, ':' 3A, '[' 5B, ']' 5D, '{' 7B,
// '}' 7D). Bytes >= 0x80 select an empty high-nibble class.
__forceinline ChunkMasks classify_chunk(const char* input) noexcept {
    const __m128i low_table = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 4, 1, 4, 0, 0);
    const __m128i high_table = _mm_setr_epi8(0, 0, 1, 2, 0, 4, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0);
    const __m128i nibble = _mm_set1_epi8(0x0F);
    const __m128i quote = _mm_set1_epi8('"');
    const __m128i backslash = _mm_set1_epi8('\\');
    const __m128i control_bits = _mm_set1_epi8(-32);
    const __m128i zero = _mm_setzero_si128();
    ChunkMasks masks{};
    for (u32 lane = 0; lane < 4; ++lane) {
        const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + lane * 16));
        const __m128i low = _mm_shuffle_epi8(low_table, _mm_and_si128(bytes, nibble));
        const __m128i high = _mm_shuffle_epi8(high_table, _mm_and_si128(_mm_srli_epi16(bytes, 4), nibble));
        const __m128i structural = _mm_cmpeq_epi8(_mm_and_si128(low, high), zero);
        masks.quotes |= lane_mask(_mm_cmpeq_epi8(bytes, quote), lane * 16);
        masks.backslashes |= lane_mask(_mm_cmpeq_epi8(bytes, backslash), lane * 16);
        masks.structurals |= lane_mask(structural, lane * 16);
        masks.controls |= lane_mask(_mm_cmpeq_epi8(_mm_and_si128(bytes, control_bits), zero), lane * 16);
    }
    masks.structurals = ~masks.structurals;
    return masks;
}

// Bits of characters preceded by an odd run of backslashes. A run carried in
// from the previous chunk escapes this chunk's first byte.
__forceinline u64 escaped_bits(u64 backslashes, u64* carried) noexcept {
    constexpr u64 odd = 0xAAAAAAAAAAAAAAAAull;
    const u64 potential = backslashes & ~*carried;
    const u64 maybe_escaped = potential << 1;
    const u64 series = ((maybe_escaped | odd) - potential) ^ odd;
    const u64 escaped = series ^ (backslashes | *carried);
    *carried = (series & backslashes) >> 63;
    return escaped;
}

// Bit i: an odd number of quotes at or before i (inclusive prefix XOR).
__forceinline u64 prefix_xor(u64 bits) noexcept {
    const __m128i product =
        _mm_clmulepi64_si128(_mm_cvtsi64_si128(static_cast<long long>(bits)), _mm_set1_epi8(-1), 0);
    return static_cast<u64>(_mm_cvtsi128_si64(product));
}

JsonScanResult scan_scalar(Utf8View input, Slice<u32> output) noexcept {
    u32 output_count = 0;
    u32 preceding_backslashes = 0;
    bool inside_string = false;
    for (u32 offset = 0; offset < input.bytes; ++offset) {
        const u8 value = static_cast<u8>(input.data[offset]);
        const bool quote = value == '"' && (preceding_backslashes & 1u) == 0;
        if (quote) {
            if (output_count >= output.count) {
                return JsonScanResult{output_count, offset, JsonScanError::output_overflow};
            }
            output[output_count++] = offset;
            inside_string = !inside_string;
        } else if (inside_string && value < 0x20) {
            return JsonScanResult{output_count, offset, JsonScanError::control_in_string};
        } else if (!inside_string &&
                   (value == '{' || value == '}' || value == '[' || value == ']' || value == ':' ||
                    value == ',')) {
            if (output_count >= output.count) {
                return JsonScanResult{output_count, offset, JsonScanError::output_overflow};
            }
            output[output_count++] = offset;
        }
        preceding_backslashes = value == '\\' ? preceding_backslashes + 1 : 0;
    }
    return inside_string ? JsonScanResult{output_count, input.bytes, JsonScanError::unterminated_string}
                         : JsonScanResult{output_count, input.bytes, JsonScanError::none};
}

struct ScanState {
    u64 escape_carry;
    u64 inside;  // all ones while a string continues into the next chunk
    u32 count;
};

enum class ChunkResult : u8 { ok, overflow, control };

__forceinline u32 lowest_bit(u64 bits) noexcept {
    unsigned long index = 0;
    _BitScanForward64(&index, bits);
    return static_cast<u32>(index);
}

// valid: bytes of this chunk that belong to the input (all for whole chunks).
__forceinline ChunkResult scan_chunk(const char* chunk, u64 valid, u32 base, ScanState* state, Slice<u32> output,
                                     u32* error_offset) noexcept {
    const ChunkMasks masks = classify_chunk(chunk);
    const u64 quotes = masks.quotes & ~escaped_bits(masks.backslashes, &state->escape_carry) & valid;
    const u64 inside = prefix_xor(quotes) ^ state->inside;
    if (masks.controls & inside & valid) return ChunkResult::control;
    state->inside = static_cast<u64>(static_cast<i64>(inside) >> 63);
    u64 emitted = quotes | (masks.structurals & ~inside & valid);
    const u32 pending = static_cast<u32>(__popcnt64(emitted));
    u32* out = output.data + state->count;
    if (pending <= output.count - state->count) {
        state->count += pending;
        while (emitted) {
            *out++ = base + lowest_bit(emitted);
            emitted &= emitted - 1;
        }
        return ChunkResult::ok;
    }
    for (u32 room = output.count - state->count; room; --room) {
        *out++ = base + lowest_bit(emitted);
        emitted &= emitted - 1;
    }
    state->count = output.count;
    *error_offset = base + lowest_bit(emitted);
    return ChunkResult::overflow;
}

} // namespace

// 64-byte chunks: quote, backslash, structural and control masks from SSSE3
// lookups, odd backslash runs resolved with carries, string interiors from a
// carry-less prefix XOR. Malformed input (a control character inside a string
// or an unterminated string) is rescanned by the scalar reference so results
// and error offsets match it exactly.
JsonScanResult json_scan_structurals_avx1(Utf8View input, Slice<u32> output) noexcept {
    if ((!input.data && input.bytes != 0) || (!output.data && output.count != 0)) {
        return JsonScanResult{0, 0, JsonScanError::invalid_input};
    }

    ScanState state{};
    u32 error_offset = 0;
    alignas(16) char tail[64];
    // One chunk site: the final partial chunk is padded into `tail` and runs
    // the same inlined body with only its valid bytes enabled.
    for (u32 offset = 0; offset < input.bytes; offset += 64) {
        const u32 remaining = input.bytes - offset;
        const char* chunk = input.data + offset;
        u64 valid = ~0ull;
        if (remaining < 64) {
            clear_bytes(tail, sizeof(tail));
            copy_bytes(tail, chunk, remaining);
            chunk = tail;
            valid = (1ull << remaining) - 1;
        }
        const ChunkResult result = scan_chunk(chunk, valid, offset, &state, output, &error_offset);
        if (result == ChunkResult::control) return scan_scalar(input, output);
        if (result == ChunkResult::overflow)
            return JsonScanResult{state.count, error_offset, JsonScanError::output_overflow};
    }

    if (state.inside) {
        return scan_scalar(input, output);
    }
    return JsonScanResult{state.count, input.bytes, JsonScanError::none};
}

} // namespace maicord
