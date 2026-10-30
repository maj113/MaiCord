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

// Eight offsets from the lowest set bits of *bits. An empty mask stores 63 + base: the high bit
// keeps the scan input nonzero, so there is no branch for "no bit left".
__forceinline void store_eight(u32* out, u64* bits, u32 base) noexcept {
    u64 rest = *bits;
    for (u32 k = 0; k < 8; ++k) {
        out[k] = base + lowest_bit(rest | 1ull << 63);
        rest &= rest - 1;
    }
    *bits = rest;
}

// valid: bytes of this chunk that belong to the input (all for whole chunks).
__forceinline ChunkResult scan_chunk(const char* chunk, u64 valid, u32 base, ScanState* state, Slice<u32> output,
                                     u32* error_offset) noexcept {
    const ChunkMasks masks = classify_chunk(chunk);
    // Without a backslash in the chunk or a run carried in, nothing is escaped and the carry stays clear.
    const u64 escaped = masks.backslashes | state->escape_carry ? escaped_bits(masks.backslashes, &state->escape_carry) : 0;
    const u64 quotes = masks.quotes & ~escaped & valid;
    const u64 inside = prefix_xor(quotes) ^ state->inside;
    if (masks.controls & inside & valid) return ChunkResult::control;
    state->inside = static_cast<u64>(static_cast<i64>(inside) >> 63);
    u64 emitted = quotes | (masks.structurals & ~inside & valid);
    const u32 pending = static_cast<u32>(__popcnt64(emitted));
    u32* out = output.data + state->count;
    const u32 room = output.count - state->count;
    if (room >= 64) {
        // Whole blocks of eight stores, past `pending` when it is not a multiple: with 64 slots free they
        // stay inside the output, and the next chunk (or nothing) overwrites them.
        state->count += pending;
        store_eight(out, &emitted, base);
        store_eight(out + 8, &emitted, base);
        for (u32 i = 16; i < pending; i += 8) store_eight(out + i, &emitted, base);
        return ChunkResult::ok;
    }
    if (pending <= room) {
        state->count += pending;
        while (emitted) {
            *out++ = base + lowest_bit(emitted);
            emitted &= emitted - 1;
        }
        return ChunkResult::ok;
    }
    for (u32 left = room; left; --left) {
        *out++ = base + lowest_bit(emitted);
        emitted &= emitted - 1;
    }
    state->count = output.count;
    *error_offset = base + lowest_bit(emitted);
    return ChunkResult::overflow;
}

// The document json_scan_source_set registered: input, its exact structural index, and the thread that owns it.
// Only the owner reads the other fields, so another thread's scans never see them half written.
// Containers shorter than this are cheaper to scan than to look up.
constexpr u32 kServedMinimumBytes = 256;

struct ScanSource {
    const char* input;
    u32 bytes;
    const u32* index;
    u32 count;
    volatile u32 owner;
};
ScanSource g_source;

__forceinline u32 current_thread() noexcept {
    return __readgsdword(0x48);
}

u32 first_at_or_after(const u32* index, u32 count, u32 value) noexcept {
    u32 first = 0, last = count;
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        if (index[middle] < value) first = middle + 1;
        else last = middle;
    }
    return first;
}

// The scan of a container inside the registered document, answered by rebasing its entries; false when the
// scan has to run (another input, a range that is not a whole container, output over the index).
bool serve_from_source(const Utf8View& input, const Slice<u32>& output, JsonScanResult* result) noexcept {
    const auto at = reinterpret_cast<usize>(input.data), base = reinterpret_cast<usize>(g_source.input);
    if (at < base || at - base > g_source.bytes || input.bytes > g_source.bytes - (at - base) ||
        (input.data[0] != '{' && input.data[0] != '[') ||
        (input.data[input.bytes - 1] != '}' && input.data[input.bytes - 1] != ']'))
        return false;
    const auto out = reinterpret_cast<usize>(output.data), index = reinterpret_cast<usize>(g_source.index);
    if (out < index + usize{g_source.count} * 4 && index < out + usize{output.count} * 4) {
        // The scan would overwrite the index: it stops being a source.
        g_source.owner = 0;
        return false;
    }
    const u32 begin = static_cast<u32>(at - base), end = begin + input.bytes;
    const u32 first = first_at_or_after(g_source.index, g_source.count, begin);
    const u32 last = first + first_at_or_after(g_source.index + first, g_source.count - first, end);
    if (first == last || g_source.index[first] != begin || g_source.index[last - 1] != end - 1)
        return false;
    const u32 count = last - first;
    const u32 stored = count < output.count ? count : output.count;
    for (u32 i = 0; i < stored; ++i)
        output.data[i] = g_source.index[first + i] - begin;
    *result = stored < count ? JsonScanResult{stored, g_source.index[first + stored] - begin, JsonScanError::output_overflow}
                             : JsonScanResult{count, input.bytes, JsonScanError::none};
    return true;
}

} // namespace

void json_scan_source_set(const Utf8View& input, const ConstSlice<u32>& index) noexcept {
    g_source.owner = 0;
    if (!input.data || !index.data || !index.count)
        return;
    g_source.input = input.data;
    g_source.bytes = input.bytes;
    g_source.index = index.data;
    g_source.count = index.count;
    g_source.owner = current_thread();
}

void json_scan_source_clear() noexcept {
    if (g_source.owner == current_thread())
        g_source.owner = 0;
}

// 64-byte chunks: quote, backslash, structural and control masks from SSSE3
// lookups, odd backslash runs resolved with carries, string interiors from a
// carry-less prefix XOR. Malformed input (a control character inside a string
// or an unterminated string) is rescanned by the scalar reference so results
// and error offsets match it exactly.
JsonScanResult json_scan_structurals_avx1(const Utf8View& input_view, const Slice<u32>& output_view) noexcept {
    // Value copies: the structural stores below must not force reloads of the caller's views.
    const Utf8View input = input_view;
    const Slice<u32> output = output_view;
    if ((!input.data && input.bytes != 0) || (!output.data && output.count != 0)) {
        return JsonScanResult{0, 0, JsonScanError::invalid_input};
    }
    JsonScanResult served;
    if (g_source.owner && input.bytes >= kServedMinimumBytes && g_source.owner == current_thread() &&
        serve_from_source(input, output, &served))
        return served;

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
