#include "maicord/media/deflate.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "png_common.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {

// Two-level decode tables. A root entry either decodes a whole codeword or
// points at a subtable indexed by the codeword's remaining bits.
constexpr u32 kLitlenTableBits = 11;
constexpr u32 kDistanceTableBits = 8;
constexpr u32 kPrecodeTableBits = 7;
// Largest table, including subtables, any complete code of 288/32/19 symbols
// needs with these root widths.
constexpr u32 kLitlenTableEntries = 2342;
constexpr u32 kDistanceTableEntries = 402;
constexpr u32 kPrecodeTableEntries = 128;
// A fast iteration writes at most one 258-byte match plus a 15-byte chunk tail.
constexpr u32 kFastOutputMargin = 258 + 16;

// Entry: value << 16 | kind << 8 | extra bits << 4 | codeword bits.
enum EntryKind : u32 {
    kEntryLiteral,
    kEntryLength, // length or distance base
    kEntryEnd,
    kEntrySubtable, // extra = subtable index bits, value = subtable offset
    kEntryInvalid, // unused codeword of an incomplete code
    kEntryBadSymbol, // literal/length 286-287, distance 30-31
    kEntryEmpty, // distance code without codewords
};
enum class TableKind : u8 { litlen, distance, precode };

struct DecodeTables {
    u32 litlen[kLitlenTableEntries];
    u32 distance[kDistanceTableEntries];
    u32 precode[kPrecodeTableEntries];
    u8 litlen_maximum;
    u8 distance_maximum;
    u8 precode_maximum;
};

// Whole bytes in bit_buffer are not consumed yet. Outside the fast loop the
// bits above bit_count are zero, so a short final lookup sees zero padding.
struct BitReader {
    const DeflateInputSpan* spans;
    const u8* span_data;
    const u8* next;
    const u8* end;
    u64 bit_buffer;
    u32 span_count;
    u32 span_index;
    u32 span_base;
    u32 input_bytes;
    u32 bit_count;
};

struct InflateState {
    BitReader reader;
    Slice<u8> output;
    DeflateWorkspace* workspace;
    DecodeTables* tables;
    ZlibStreamInflater* stream;
    Slice<u8>* output_view;
    ZlibStreamGrowOutput grow_output;
    void* grow_context;
    u32 maximum_output_bytes;
    u32 output_bytes;
    u32 blocks;
    u32 literals;
    u32 copies;
    u32 window_size;
    ZlibInflateError error;
    // Windowed (sink) mode: output[0, emitted) was delivered already.
    ZlibInflateSink sink;
    void* sink_context;
    u32 emitted;
    u32 adler;
    u64 delivered;
};

inline constexpr u16 kLengthBases[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23,  27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
inline constexpr u8 kLengthExtraBits[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
inline constexpr u16 kDistanceBases[30] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
inline constexpr u8 kDistanceExtraBits[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};
inline constexpr u8 kCodeLengthOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

__forceinline constexpr u32 make_entry(u32 value, u32 kind, u32 extra, u32 bits) noexcept {
    return value << 16 | kind << 8 | extra << 4 | bits;
}
__forceinline constexpr u32 entry_kind(u32 entry) noexcept { return (entry >> 8) & 0xFFu; }
__forceinline constexpr u32 entry_extra(u32 entry) noexcept { return (entry >> 4) & 15u; }
__forceinline constexpr u32 entry_bits(u32 entry) noexcept { return entry & 15u; }
__forceinline constexpr u32 entry_value(u32 entry) noexcept { return entry >> 16; }
__forceinline constexpr u64 low_bits(u64 value, u32 count) noexcept {
    return value & ((u64{1} << count) - 1u);
}

u32 symbol_entry(TableKind kind, u32 symbol) noexcept {
    if (kind == TableKind::precode)
        return make_entry(symbol, kEntryLiteral, 0, 0);
    if (kind == TableKind::distance)
        return symbol < 30 ? make_entry(kDistanceBases[symbol], kEntryLength, kDistanceExtraBits[symbol], 0)
                           : make_entry(0, kEntryBadSymbol, 0, 0);
    if (symbol < 256)
        return make_entry(symbol, kEntryLiteral, 0, 0);
    if (symbol == 256)
        return make_entry(0, kEntryEnd, 0, 0);
    if (symbol <= 285)
        return make_entry(kLengthBases[symbol - 257], kEntryLength, kLengthExtraBits[symbol - 257], 0);
    return make_entry(0, kEntryBadSymbol, 0, 0);
}

// A complete canonical code, codewords sorted by (length, symbol). Each root
// codeword is stored once at its bit-reversed index, and the filled prefix is
// doubled whenever the length grows, so a root entry costs one store or one
// block copy instead of a strided fill. Subtables match the strided layout.
bool build_complete_table(const u16* counts, const u16* sorted, u32 maximum_length, TableKind kind,
                          u32 table_bits, u32 capacity, u32* table) noexcept {
    u32 length = 1;
    while (!counts[length])
        ++length;
    u32 left = counts[length];
    u32 filled = 1u << minimum(length, table_bits);
    u32 code = 0; // bit-reversed codeword
    u32 next = 0;
    if (length <= table_bits) {
        for (;;) {
            table[code] = symbol_entry(kind, sorted[next++]) | length;
            const u32 last = (1u << length) - 1;
            if (code == last) {
                // The all-ones codeword completes a code that fits the root.
                for (; filled < (1u << table_bits); filled <<= 1)
                    copy_bytes(table + filled, table, filled * static_cast<u32>(sizeof(u32)));
                return true;
            }
            unsigned long high = 0;
            _BitScanReverse(&high, code ^ last);
            code = (code & ((1u << high) - 1)) | (1u << high);
            if (--left)
                continue;
            do {
                if (++length <= table_bits) {
                    copy_bytes(table + filled, table, filled * static_cast<u32>(sizeof(u32)));
                    filled <<= 1;
                }
            } while (!(left = counts[length]));
            if (length > table_bits)
                break;
        }
    }
    const u32 root_mask = (1u << table_bits) - 1;
    u32 end = 1u << table_bits;
    u32 prefix = ~0u, subtable = 0;
    for (;;) {
        if ((code & root_mask) != prefix) {
            prefix = code & root_mask;
            u32 subtable_bits = length - table_bits;
            i32 room = static_cast<i32>(1u << subtable_bits) - static_cast<i32>(left);
            for (u32 at = length + 1; room > 0 && at <= maximum_length; ++at) {
                ++subtable_bits;
                room = (room << 1) - counts[at];
            }
            if (end + (1u << subtable_bits) > capacity)
                return false;
            subtable = end;
            end += 1u << subtable_bits;
            table[prefix] = make_entry(subtable, kEntrySubtable, subtable_bits, table_bits);
        }
        const u32 entry = symbol_entry(kind, sorted[next++]) | (length - table_bits);
        for (u32 index = subtable + (code >> table_bits); index < end; index += 1u << (length - table_bits))
            table[index] = entry;
        const u32 last = (1u << length) - 1;
        if (code == last)
            return true;
        unsigned long high = 0;
        _BitScanReverse(&high, code ^ last);
        code = (code & ((1u << high) - 1)) | (1u << high);
        if (!--left)
            while (!(left = counts[++length])) {
            }
    }
}

// Accepts exactly the codes the RFC 1951 decoder admits: complete codes, an
// empty code where allowed, or a single one-bit codeword where allowed.
bool build_table(const u8* lengths, u32 symbol_count, TableKind kind, u32 table_bits, u32 capacity,
                 u32* table, u8* maximum_bits, bool allow_empty, bool allow_single) noexcept {
    u16 counts[16]{};
    u32 used = 0;
    u32 maximum_length = 0;
    for (u32 symbol = 0; symbol < symbol_count; ++symbol) {
        const u32 length = lengths[symbol];
        if (length > 15)
            return false;
        if (length != 0) {
            ++counts[length];
            ++used;
            maximum_length = maximum(maximum_length, length);
        }
    }
    *maximum_bits = static_cast<u8>(maximum_length);
    const u32 root_entries = 1u << table_bits;
    if (used == 0) {
        if (!allow_empty)
            return false;
        for (u32 index = 0; index < root_entries; ++index)
            table[index] = make_entry(0, kEntryEmpty, 0, 0);
        return true;
    }
    i32 remaining = 1;
    for (u32 bits = 1; bits <= 15; ++bits) {
        remaining = (remaining << 1) - counts[bits];
        if (remaining < 0)
            return false;
    }
    const bool incomplete = remaining != 0;
    if (incomplete && !(allow_single && used == 1 && maximum_length == 1))
        return false;

    u16 offsets[16];
    offsets[1] = 0;
    for (u32 bits = 1; bits < 15; ++bits)
        offsets[bits + 1] = static_cast<u16>(offsets[bits] + counts[bits]);
    u16 sorted[kDeflateLiteralSymbolCapacity];
    for (u32 symbol = 0; symbol < symbol_count; ++symbol)
        if (lengths[symbol] != 0)
            sorted[offsets[lengths[symbol]]++] = static_cast<u16>(symbol);
    if (!incomplete)
        return build_complete_table(counts, sorted, maximum_length, kind, table_bits, capacity, table);
    for (u32 index = 0; index < root_entries; ++index)
        table[index] = make_entry(0, kEntryInvalid, 0, 0);

    const u32 root_mask = root_entries - 1;
    u32 code = 0, sorted_index = 0, next_subtable = root_entries;
    u32 prefix = ~0u, subtable = 0, subtable_bits = 0;
    for (u32 length = 1; length <= maximum_length; ++length, code <<= 1) {
        for (u32 left_at_length = counts[length]; left_at_length != 0; --left_at_length, ++code) {
            u32 reversed = 0;
            for (u32 bit = 0; bit < length; ++bit)
                reversed |= ((code >> bit) & 1u) << (length - 1 - bit);
            const u32 entry = symbol_entry(kind, sorted[sorted_index++]);
            if (length <= table_bits) {
                for (u32 index = reversed; index < root_entries; index += 1u << length)
                    table[index] = entry | length;
                continue;
            }
            if ((reversed & root_mask) != prefix) {
                // Size the subtable for the remaining codewords sharing this prefix.
                prefix = reversed & root_mask;
                subtable_bits = length - table_bits;
                i32 left = 1 << subtable_bits;
                for (u32 at = length; at < maximum_length; ++at) {
                    left -= at == length ? static_cast<i32>(left_at_length) : counts[at];
                    if (left <= 0)
                        break;
                    ++subtable_bits;
                    left <<= 1;
                }
                if (next_subtable + (1u << subtable_bits) > capacity)
                    return false;
                subtable = next_subtable;
                next_subtable += 1u << subtable_bits;
                table[prefix] = make_entry(subtable, kEntrySubtable, subtable_bits, table_bits);
            }
            for (u32 index = reversed >> table_bits; index < (1u << subtable_bits);
                 index += 1u << (length - table_bits))
                table[subtable + index] = entry | (length - table_bits);
        }
    }
    return true;
}

void build_fixed_tables(DecodeTables* tables) noexcept {
    u8 lengths[kDeflateLiteralSymbolCapacity + kDeflateDistanceSymbolCapacity];
    for (u32 symbol = 0; symbol < kDeflateLiteralSymbolCapacity; ++symbol)
        lengths[symbol] = symbol <= 143 ? 8 : symbol <= 255 ? 9 : symbol <= 279 ? 7 : 8;
    for (u32 symbol = 0; symbol < kDeflateDistanceSymbolCapacity; ++symbol)
        lengths[kDeflateLiteralSymbolCapacity + symbol] = 5;
    build_table(lengths, kDeflateLiteralSymbolCapacity, TableKind::litlen, kLitlenTableBits,
                kLitlenTableEntries, tables->litlen, &tables->litlen_maximum, false, false);
    build_table(lengths + kDeflateLiteralSymbolCapacity, kDeflateDistanceSymbolCapacity, TableKind::distance,
                kDistanceTableBits, kDistanceTableEntries, tables->distance, &tables->distance_maximum, false,
                false);
}

DecodeTables g_fixed_tables;
volatile long g_fixed_state;

// Built once on first use; a thread racing the builder uses its own tables.
const DecodeTables* fixed_tables(DecodeTables* fallback) noexcept {
    if (g_fixed_state == 2) {
        _ReadWriteBarrier();
        return &g_fixed_tables;
    }
    if (_InterlockedCompareExchange(&g_fixed_state, 1, 0) == 0) {
        build_fixed_tables(&g_fixed_tables);
        _InterlockedExchange(&g_fixed_state, 2);
        return &g_fixed_tables;
    }
    build_fixed_tables(fallback);
    return fallback;
}

ZlibInflateResult failure(const InflateState& state, ZlibInflateError error) noexcept {
    return ZlibInflateResult{state.reader.input_bytes, state.output_bytes, state.blocks,
                             state.literals,           state.copies,       error};
}

bool advance_span(BitReader* reader) noexcept {
    while (reader->next == reader->end && reader->span_index < reader->span_count) {
        reader->span_base += reader->spans[reader->span_index].bytes;
        if (++reader->span_index == reader->span_count) {
            reader->span_data = reader->next = reader->end = nullptr;
            return false;
        }
        const DeflateInputSpan& span = reader->spans[reader->span_index];
        reader->span_data = reader->next = span.data;
        reader->end = span.data + span.bytes;
    }
    return reader->next != reader->end;
}

bool reader_initialize(BitReader* reader, ConstSlice<DeflateInputSpan> spans) noexcept {
    if (!spans.data || spans.count == 0) {
        return false;
    }
    *reader = BitReader{};
    reader->spans = spans.data;
    reader->span_count = spans.count;
    u64 total = 0;
    for (u32 index = 0; index < spans.count; ++index) {
        if (!spans[index].data && spans[index].bytes != 0) {
            return false;
        }
        total += spans[index].bytes;
        if (total > ~0u) {
            return false;
        }
    }
    reader->input_bytes = static_cast<u32>(total);
    reader->span_data = reader->next = spans[0].data;
    reader->end = spans[0].data + spans[0].bytes;
    return true;
}

__forceinline u32 reader_consumed(const BitReader& reader) noexcept {
    return reader.span_base + static_cast<u32>(reader.next - reader.span_data) - reader.bit_count / 8;
}

void refill_slow(BitReader* reader) noexcept {
    while (reader->bit_count <= 56) {
        if (reader->next == reader->end && !advance_span(reader))
            return;
        reader->bit_buffer |= static_cast<u64>(*reader->next++) << reader->bit_count;
        reader->bit_count += 8;
    }
}

bool read_bits(BitReader* reader, u32 count, u32* value) noexcept {
    if (reader->bit_count < count)
        refill_slow(reader);
    if (reader->bit_count < count)
        return false;
    *value = static_cast<u32>(low_bits(reader->bit_buffer, count));
    reader->bit_buffer >>= count;
    reader->bit_count -= count;
    return true;
}

void align_reader(BitReader* reader) noexcept {
    const u32 partial = reader->bit_count & 7u;
    reader->bit_buffer >>= partial;
    reader->bit_count -= partial;
}

// Requires a byte-aligned reader.
bool read_aligned_bytes(BitReader* reader, u8* destination, u32 count) noexcept {
    for (; count != 0 && reader->bit_count != 0; --count) {
        *destination++ = static_cast<u8>(reader->bit_buffer);
        reader->bit_buffer >>= 8;
        reader->bit_count -= 8;
    }
    while (count != 0) {
        if (reader->next == reader->end && !advance_span(reader))
            return false;
        const u32 chunk = minimum(count, static_cast<u32>(reader->end - reader->next));
        copy_bytes(destination, reader->next, chunk);
        destination += chunk;
        reader->next += chunk;
        count -= chunk;
    }
    return true;
}

// Exactly mirrors bit-serial canonical decoding: a codeword that does not fit
// the remaining input is truncation; an unused codeword is invalid only after
// maximum_bits bits are available. An empty code consumes nothing.
bool decode_slow(BitReader* reader, const u32* table, u32 table_bits, u32 maximum_bits, u32* decoded,
                 ZlibInflateError* error) noexcept {
    refill_slow(reader);
    u32 entry = table[low_bits(reader->bit_buffer, table_bits)];
    u32 bits = entry_bits(entry);
    if (entry_kind(entry) == kEntrySubtable) {
        entry = table[entry_value(entry) + low_bits(reader->bit_buffer >> table_bits, entry_extra(entry))];
        bits = table_bits + entry_bits(entry);
    }
    if (entry_kind(entry) == kEntryInvalid) {
        *error = reader->bit_count >= maximum_bits ? ZlibInflateError::invalid_huffman
                                                   : ZlibInflateError::truncated;
        return false;
    }
    if (entry_kind(entry) != kEntryEmpty) {
        if (bits > reader->bit_count) {
            *error = ZlibInflateError::truncated;
            return false;
        }
        reader->bit_buffer >>= bits;
        reader->bit_count -= bits;
    }
    *decoded = entry;
    return true;
}

u32 adler32_update(u32 adler, ConstSlice<u8> bytes) noexcept;

// Delivers pending output, then keeps only the dictionary at the window start.
bool slide_window(InflateState* state) noexcept {
    const u32 produced = state->output_bytes;
    if (produced > state->emitted) {
        const ConstSlice<u8> fresh{state->output.data + state->emitted, produced - state->emitted};
        state->adler = adler32_update(state->adler, fresh);
        state->delivered += fresh.count;
        if (!state->sink(state->sink_context, fresh)) {
            state->error = ZlibInflateError::stream_failed;
            return false;
        }
    }
    const u32 keep = minimum(produced, kZlibStreamWindowBytes);
    // A slide only happens past half the window, so the ranges never overlap.
    if (keep && produced - keep >= keep)
        copy_bytes(state->output.data, state->output.data + produced - keep, keep);
    else if (keep && produced != keep)
        for (u32 i = 0; i < keep; ++i)
            state->output.data[i] = state->output.data[produced - keep + i];
    state->output_bytes = state->emitted = keep;
    return true;
}

bool stream_output_disjoint(const InflateState& state, Slice<u8> output) noexcept {
    const ConstSlice<u8> destination{output.data, output.count};
    if (!output.data || !byte_range_valid(destination) ||
        byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(state.stream), sizeof(*state.stream)}) ||
        byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(state.output_view), sizeof(*state.output_view)}) ||
        byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(state.reader.spans),
            static_cast<u32>(state.reader.span_count * sizeof(DeflateInputSpan))})) return false;
    for (u32 i = 0; i < state.reader.span_count; ++i)
        if (byte_ranges_overlap(destination, {state.reader.spans[i].data, state.reader.spans[i].bytes})) return false;
    return true;
}
template <bool Streaming> bool ensure_output(InflateState* state, u32 bytes) noexcept {
    if (bytes <= state->output.count - state->output_bytes) return true;
    if constexpr (!Streaming) {
        if (state->sink) {
            if (!slide_window(state)) return false;
            if (bytes <= state->output.count - state->output_bytes) return true;
        }
    }
    if constexpr (Streaming) {
        if (state->grow_output && bytes <= state->maximum_output_bytes - state->output_bytes) {
            const u32 required = state->output_bytes + bytes;
            Slice<u8> backed{};
            if (state->grow_output(state->grow_context, required, &backed) &&
                backed.data == state->output.data && backed.count > state->output.count &&
                backed.count >= required && backed.count <= state->maximum_output_bytes &&
                stream_output_disjoint(*state, backed)) {
                state->output = backed;
                *state->output_view = backed;
                return true;
            }
        }
    }
    state->error = ZlibInflateError::output_capacity;
    return false;
}

__forceinline u64 load_u64(const u8* source) noexcept {
    return static_cast<u64>(_mm_cvtsi128_si64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(source))));
}
__forceinline void copy16(u8* destination, const u8* source) noexcept {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination),
                     _mm_loadu_si128(reinterpret_cast<const __m128i*>(source)));
}

// Byte order matters when distance < length: later bytes repeat earlier output.
__forceinline void copy_match_exact(u8* destination, u32 distance, u32 length) noexcept {
    const u8* source = destination - distance;
    if (distance >= length) {
        copy_bytes(destination, source, length);
        return;
    }
    for (u32 index = 0; index < length; ++index)
        destination[index] = source[index];
}

// May write up to 15 bytes past the match; the caller guarantees that room.
__forceinline void copy_match_fast(u8* destination, u32 distance, u32 length) noexcept {
    u8* const end = destination + length;
    const u8* source = destination - distance;
    if (distance >= 16) {
        do {
            copy16(destination, source);
            destination += 16;
            source += 16;
        } while (destination < end);
        return;
    }
    if (distance == 1) {
        const __m128i fill = _mm_set1_epi8(static_cast<char>(*source));
        do {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination), fill);
            destination += 16;
        } while (destination < end);
        return;
    }
    // Extend the period to a multiple of distance >= 16, then copy in chunks.
    const u32 period = distance * ((15 + distance) / distance);
    const u32 head = minimum(period - distance, length);
    for (u32 index = 0; index < head; ++index)
        destination[index] = source[index];
    destination += head;
    source = destination - period;
    while (destination < end) {
        copy16(destination, source);
        destination += 16;
        source += 16;
    }
}

// Copies the part of a match that precedes this flush from the dictionary.
void copy_from_window(const ZlibStreamInflater& stream, u8* output, u32 position, u32 distance,
                      u32 length) noexcept {
    const u32 back = distance - position;
    const u32 from_window = minimum(back, length);
    const u32 index = (stream.window_cursor - back) & (kZlibStreamWindowBytes - 1);
    u8* destination = output + position;
    // The dictionary is a separate ring: at most two block copies.
    const u32 first = minimum(from_window, kZlibStreamWindowBytes - index);
    copy_bytes(destination, stream.window + index, first);
    copy_bytes(destination + first, stream.window, from_window - first);
    if (length > from_window)
        copy_match_exact(destination + from_window, distance, length - from_window);
}

template <bool Streaming>
__forceinline bool distance_available(const InflateState& state, u32 distance, u32 position) noexcept {
    if constexpr (Streaming) {
        return distance <= state.window_size && distance <= state.stream->window_bytes + position;
    } else {
        return distance <= state.window_size && distance <= position;
    }
}

template <bool Streaming>
bool decode_huffman_block(InflateState* state, const u32* litlen, const u32* distances, u32 litlen_maximum,
                          u32 distance_maximum) noexcept {
    constexpr u32 kLitlenMask = (1u << kLitlenTableBits) - 1;
    constexpr u32 kDistanceMask = (1u << kDistanceTableBits) - 1;
    BitReader& reader = state->reader;
    for (;;) {
        if (reader.end - reader.next >= 8 && state->output.count - state->output_bytes >= kFastOutputMargin) {
            // A refill leaves 56..63 bits; one iteration uses at most 48
            // (length 15+5, distance 15+13) or two literals.
            u64 bits = reader.bit_buffer;
            u32 count = reader.bit_count;
            const u8* in = reader.next;
            const u8* const in_limit = reader.end - 8;
            u8* const out = state->output.data;
            u32 position = state->output_bytes;
            const u32 out_limit = state->output.count - kFastOutputMargin;
            u32 literals = 0, copies = 0;
            ZlibInflateError error = ZlibInflateError::none;
            bool finished = false;
            do {
                bits |= load_u64(in) << count;
                in += (63 - count) >> 3;
                count |= 56;
                u32 entry = litlen[bits & kLitlenMask];
                if (entry_kind(entry) == kEntrySubtable) {
                    bits >>= kLitlenTableBits;
                    count -= kLitlenTableBits;
                    entry = litlen[entry_value(entry) + low_bits(bits, entry_extra(entry))];
                }
                bits >>= entry_bits(entry);
                count -= entry_bits(entry);
                if (entry_kind(entry) == kEntryLiteral) {
                    out[position++] = static_cast<u8>(entry_value(entry));
                    ++literals;
                    entry = litlen[bits & kLitlenMask];
                    if (entry_kind(entry) == kEntryLiteral) {
                        bits >>= entry_bits(entry);
                        count -= entry_bits(entry);
                        out[position++] = static_cast<u8>(entry_value(entry));
                        ++literals;
                    }
                    continue;
                }
                if (entry_kind(entry) != kEntryLength) {
                    if (entry_kind(entry) == kEntryEnd)
                        finished = true;
                    else
                        error = entry_kind(entry) == kEntryBadSymbol ? ZlibInflateError::invalid_symbol
                                                                      : ZlibInflateError::invalid_huffman;
                    break;
                }
                const u32 length = entry_value(entry) + static_cast<u32>(low_bits(bits, entry_extra(entry)));
                bits >>= entry_extra(entry);
                count -= entry_extra(entry);
                entry = distances[bits & kDistanceMask];
                if (entry_kind(entry) == kEntrySubtable) {
                    bits >>= kDistanceTableBits;
                    count -= kDistanceTableBits;
                    entry = distances[entry_value(entry) + low_bits(bits, entry_extra(entry))];
                }
                if (entry_kind(entry) != kEntryLength) {
                    error = entry_kind(entry) == kEntryInvalid ? ZlibInflateError::invalid_huffman
                                                               : ZlibInflateError::invalid_distance;
                    break;
                }
                bits >>= entry_bits(entry);
                count -= entry_bits(entry);
                const u32 distance = entry_value(entry) + static_cast<u32>(low_bits(bits, entry_extra(entry)));
                bits >>= entry_extra(entry);
                count -= entry_extra(entry);
                if (!distance_available<Streaming>(*state, distance, position)) {
                    error = ZlibInflateError::invalid_distance;
                    break;
                }
                if constexpr (Streaming) {
                    if (distance > position)
                        copy_from_window(*state->stream, out, position, distance, length);
                    else
                        copy_match_fast(out + position, distance, length);
                } else {
                    copy_match_fast(out + position, distance, length);
                }
                position += length;
                ++copies;
            } while (in <= in_limit && position <= out_limit);
            reader.bit_buffer = low_bits(bits, count);
            reader.bit_count = count;
            reader.next = in;
            state->output_bytes = position;
            state->literals += literals;
            state->copies += copies;
            if (finished)
                return true;
            if (error != ZlibInflateError::none) {
                state->error = error;
                return false;
            }
        }

        // One symbol near the end of input/output, with exact error semantics.
        u32 entry = 0;
        ZlibInflateError error = ZlibInflateError::none;
        if (!decode_slow(&reader, litlen, kLitlenTableBits, litlen_maximum, &entry, &error)) {
            state->error = error;
            return false;
        }
        if (entry_kind(entry) == kEntryLiteral) {
            if (!ensure_output<Streaming>(state, 1))
                return false;
            state->output.data[state->output_bytes++] = static_cast<u8>(entry_value(entry));
            ++state->literals;
            continue;
        }
        if (entry_kind(entry) == kEntryEnd)
            return true;
        if (entry_kind(entry) != kEntryLength) {
            state->error = ZlibInflateError::invalid_symbol;
            return false;
        }
        u32 extra = 0;
        if (!read_bits(&reader, entry_extra(entry), &extra)) {
            state->error = ZlibInflateError::truncated;
            return false;
        }
        const u32 length = entry_value(entry) + extra;
        if (!decode_slow(&reader, distances, kDistanceTableBits, distance_maximum, &entry, &error)) {
            state->error = error;
            return false;
        }
        if (entry_kind(entry) != kEntryLength) {
            state->error = ZlibInflateError::invalid_distance;
            return false;
        }
        if (!read_bits(&reader, entry_extra(entry), &extra)) {
            state->error = ZlibInflateError::truncated;
            return false;
        }
        const u32 distance = entry_value(entry) + extra;
        if (!distance_available<Streaming>(*state, distance, state->output_bytes)) {
            state->error = ZlibInflateError::invalid_distance;
            return false;
        }
        if (!ensure_output<Streaming>(state, length))
            return false;
        if constexpr (Streaming) {
            if (distance > state->output_bytes)
                copy_from_window(*state->stream, state->output.data, state->output_bytes, distance, length);
            else
                copy_match_exact(state->output.data + state->output_bytes, distance, length);
        } else {
            copy_match_exact(state->output.data + state->output_bytes, distance, length);
        }
        state->output_bytes += length;
        ++state->copies;
    }
}

bool build_dynamic_tables(InflateState* state) noexcept {
    BitReader* reader = &state->reader;
    DecodeTables* tables = state->tables;
    u8* lengths = state->workspace->lengths;
    u32 hlit_bits = 0;
    u32 hdist_bits = 0;
    u32 hclen_bits = 0;
    if (!read_bits(reader, 5, &hlit_bits) || !read_bits(reader, 5, &hdist_bits) ||
        !read_bits(reader, 4, &hclen_bits)) {
        state->error = ZlibInflateError::truncated;
        return false;
    }
    const u32 literal_count = hlit_bits + 257;
    const u32 distance_count = hdist_bits + 1;
    const u32 code_count = hclen_bits + 4;
    if (literal_count > 286 || distance_count > 32 || code_count > 19) {
        state->error = ZlibInflateError::invalid_block;
        return false;
    }

    for (u32 index = 0; index < 19; ++index) {
        lengths[index] = 0;
    }
    for (u32 index = 0; index < code_count; ++index) {
        u32 length = 0;
        if (!read_bits(reader, 3, &length)) {
            state->error = ZlibInflateError::truncated;
            return false;
        }
        lengths[kCodeLengthOrder[index]] = static_cast<u8>(length);
    }
    if (!build_table(lengths, 19, TableKind::precode, kPrecodeTableBits, kPrecodeTableEntries, tables->precode,
                     &tables->precode_maximum, false, false)) {
        state->error = ZlibInflateError::invalid_huffman;
        return false;
    }

    const u32 total = literal_count + distance_count;
    u32 cursor = 0;
    while (cursor < total) {
        u32 entry = 0;
        if (!decode_slow(reader, tables->precode, kPrecodeTableBits, tables->precode_maximum, &entry,
                         &state->error)) {
            return false;
        }
        const u32 symbol = entry_value(entry);
        if (symbol <= 15) {
            lengths[cursor++] = static_cast<u8>(symbol);
            continue;
        }
        u32 extra = 0;
        u32 repeat = 0;
        u8 value = 0;
        if (symbol == 16) {
            if (cursor == 0 || !read_bits(reader, 2, &extra)) {
                state->error =
                    cursor == 0 ? ZlibInflateError::invalid_huffman : ZlibInflateError::truncated;
                return false;
            }
            repeat = extra + 3;
            value = lengths[cursor - 1];
        } else if (symbol == 17) {
            if (!read_bits(reader, 3, &extra)) {
                state->error = ZlibInflateError::truncated;
                return false;
            }
            repeat = extra + 3;
        } else {
            if (!read_bits(reader, 7, &extra)) {
                state->error = ZlibInflateError::truncated;
                return false;
            }
            repeat = extra + 11;
        }
        if (repeat > total - cursor) {
            state->error = ZlibInflateError::invalid_huffman;
            return false;
        }
        for (u32 index = 0; index < repeat; ++index) {
            lengths[cursor++] = value;
        }
    }
    if (lengths[256] == 0 ||
        !build_table(lengths, literal_count, TableKind::litlen, kLitlenTableBits, kLitlenTableEntries,
                     tables->litlen, &tables->litlen_maximum, false, true) ||
        !build_table(lengths + literal_count, distance_count, TableKind::distance, kDistanceTableBits,
                     kDistanceTableEntries, tables->distance, &tables->distance_maximum, true, true)) {
        state->error = ZlibInflateError::invalid_huffman;
        return false;
    }
    return true;
}

template <bool Streaming>
bool decode_stored_block(InflateState* state, u16* decoded_length) noexcept {
    align_reader(&state->reader);
    u8 header[4]{};
    if (!read_aligned_bytes(&state->reader, header, 4)) {
        state->error = ZlibInflateError::truncated;
        return false;
    }
    const u16 length = static_cast<u16>(header[0] | header[1] << 8);
    const u16 inverse = static_cast<u16>(header[2] | header[3] << 8);
    if (static_cast<u16>(length ^ 0xFFFFu) != inverse) {
        state->error = ZlibInflateError::invalid_block;
        return false;
    }
    if (!ensure_output<Streaming>(state, length)) {
        state->error = ZlibInflateError::output_capacity;
        return false;
    }
    if (!read_aligned_bytes(&state->reader, state->output.data + state->output_bytes, length)) {
        state->error = ZlibInflateError::truncated;
        return false;
    }
    state->output_bytes += length;
    state->literals += length;
    if (decoded_length) {
        *decoded_length = length;
    }
    return true;
}

template <bool Streaming> bool decode_block(InflateState* state, u32 type, u16* stored_length) noexcept {
    switch (type) {
    case 0:
        return decode_stored_block<Streaming>(state, stored_length);
    case 1: {
        const DecodeTables* fixed = fixed_tables(state->tables);
        return decode_huffman_block<Streaming>(state, fixed->litlen, fixed->distance, fixed->litlen_maximum,
                                               fixed->distance_maximum);
    }
    case 2:
        return build_dynamic_tables(state) &&
               decode_huffman_block<Streaming>(state, state->tables->litlen, state->tables->distance,
                                               state->tables->litlen_maximum, state->tables->distance_maximum);
    default:
        state->error = ZlibInflateError::invalid_block;
        return false;
    }
}

// SSSE3 Adler-32 over 32-byte blocks, reduced every NMAX (5552) bytes.
u32 adler32_update(u32 adler, ConstSlice<u8> bytes) noexcept {
    u32 first = adler & 0xFFFFu;
    u32 second = adler >> 16;
    const u8* data = bytes.data;
    u32 remaining = bytes.count;
    constexpr u32 kBlock = 32;
    const __m128i tap_high = _mm_setr_epi8(32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17);
    const __m128i tap_low = _mm_setr_epi8(16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1);
    const __m128i zero = _mm_setzero_si128();
    const __m128i ones = _mm_set1_epi16(1);
    while (remaining >= kBlock) {
        u32 blocks = minimum<u32>(remaining / kBlock, 5552 / kBlock);
        remaining -= blocks * kBlock;
        __m128i prefix = _mm_cvtsi32_si128(static_cast<int>(first * blocks));
        __m128i sum_second = _mm_cvtsi32_si128(static_cast<int>(second));
        __m128i sum_first = zero;
        do {
            const __m128i low = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
            const __m128i high = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16));
            prefix = _mm_add_epi32(prefix, sum_first);
            sum_first = _mm_add_epi32(sum_first, _mm_sad_epu8(low, zero));
            sum_second = _mm_add_epi32(sum_second, _mm_madd_epi16(_mm_maddubs_epi16(low, tap_high), ones));
            sum_first = _mm_add_epi32(sum_first, _mm_sad_epu8(high, zero));
            sum_second = _mm_add_epi32(sum_second, _mm_madd_epi16(_mm_maddubs_epi16(high, tap_low), ones));
            data += kBlock;
        } while (--blocks);
        sum_second = _mm_add_epi32(sum_second, _mm_slli_epi32(prefix, 5));
        sum_first = _mm_add_epi32(sum_first, _mm_shuffle_epi32(sum_first, _MM_SHUFFLE(2, 3, 0, 1)));
        sum_first = _mm_add_epi32(sum_first, _mm_shuffle_epi32(sum_first, _MM_SHUFFLE(1, 0, 3, 2)));
        sum_second = _mm_add_epi32(sum_second, _mm_shuffle_epi32(sum_second, _MM_SHUFFLE(2, 3, 0, 1)));
        sum_second = _mm_add_epi32(sum_second, _mm_shuffle_epi32(sum_second, _MM_SHUFFLE(1, 0, 3, 2)));
        first = (first + static_cast<u32>(_mm_cvtsi128_si32(sum_first))) % 65'521;
        second = static_cast<u32>(_mm_cvtsi128_si32(sum_second)) % 65'521;
    }
    for (; remaining != 0; --remaining) {
        first += *data++;
        second += first;
    }
    return ((second % 65'521) << 16) | (first % 65'521);
}

bool input_byte_from_end(ConstSlice<DeflateInputSpan> input, u32 reverse_index, u8* value) noexcept {
    for (u32 span_index = input.count; span_index != 0; --span_index) {
        const DeflateInputSpan& span = input[span_index - 1];
        if (reverse_index < span.bytes) {
            *value = span.data[span.bytes - reverse_index - 1];
            return true;
        }
        reverse_index -= span.bytes;
    }
    return false;
}

bool input_ends_sync_flush(ConstSlice<DeflateInputSpan> input) noexcept {
    constexpr u8 suffix[4] = {0x00, 0x00, 0xFF, 0xFF};
    for (u32 index = 0; index < 4; ++index) {
        u8 value = 0;
        if (!input_byte_from_end(input, 3 - index, &value) || value != suffix[index]) {
            return false;
        }
    }
    return true;
}

ZlibInflateResult poison_stream(ZlibStreamInflater* stream, const InflateState& state,
                                ZlibInflateError error) noexcept {
    stream->failed = true;
    return failure(state, error);
}

bool decode_final_blocks(InflateState* state) noexcept {
    bool final_block = false;
    while (!final_block) {
        u32 final = 0, type = 0;
        if (!read_bits(&state->reader, 1, &final) || !read_bits(&state->reader, 2, &type)) {
            state->error = ZlibInflateError::truncated;
            return false;
        }
        final_block = final != 0;
        ++state->blocks;
        if (!decode_block<false>(state, type, nullptr)) {
            if (state->error == ZlibInflateError::none)
                state->error = ZlibInflateError::invalid_block;
            return false;
        }
    }
    align_reader(&state->reader);
    return true;
}

bool read_zlib_header(BitReader* reader, u32* window_size, ZlibInflateError* error) noexcept {
    u8 header[2]{};
    if (!read_aligned_bytes(reader, header, 2)) {
        *error = ZlibInflateError::truncated;
        return false;
    }
    const u8 cmf = header[0], flags = header[1];
    if ((cmf & 0x0Fu) != 8 || (cmf >> 4) > 7) {
        *error = ZlibInflateError::unsupported_method;
        return false;
    }
    if ((static_cast<u32>(cmf) * 256u + flags) % 31u != 0) {
        *error = ZlibInflateError::invalid_header;
        return false;
    }
    if ((flags & 0x20u) != 0) {
        *error = ZlibInflateError::unsupported_dictionary;
        return false;
    }
    *window_size = 1u << ((cmf >> 4) + 8u);
    return true;
}

// The dictionary keeps the last 32 KiB of output; matches read it only for
// the bytes that precede this flush.
void commit_window(ZlibStreamInflater* stream, const u8* output, u32 bytes) noexcept {
    constexpr u32 kMask = kZlibStreamWindowBytes - 1;
    const u32 kept = minimum(bytes, kZlibStreamWindowBytes);
    const u8* source = output + bytes - kept;
    const u32 first = minimum(kept, kZlibStreamWindowBytes - stream->window_cursor);
    copy_bytes(stream->window + stream->window_cursor, source, first);
    copy_bytes(stream->window, source + first, kept - first);
    stream->window_cursor = (stream->window_cursor + kept) & kMask;
    stream->window_bytes = static_cast<u32>(
        minimum<u64>(static_cast<u64>(stream->window_bytes) + bytes, stream->window_size));
}

} // namespace

ZlibInflateResult zlib_inflate(ConstSlice<DeflateInputSpan> input, Slice<u8> output,
                               u32 expected_output_bytes, DeflateWorkspace* workspace) noexcept {
    InflateState state{};
    DecodeTables tables;
    state.output = output;
    state.workspace = workspace;
    state.tables = &tables;
    if (!workspace || (!output.data && output.count != 0) || !reader_initialize(&state.reader, input)) {
        return failure(state, ZlibInflateError::invalid_input);
    }
    if (expected_output_bytes != ~0u && expected_output_bytes > output.count) {
        return failure(state, ZlibInflateError::output_capacity);
    }
    ZlibInflateError header_error = ZlibInflateError::none;
    if (!read_zlib_header(&state.reader, &state.window_size, &header_error)) {
        return failure(state, header_error);
    }

    if (!decode_final_blocks(&state))
        return failure(state, state.error);

    u8 checksum_bytes[4]{};
    if (!read_aligned_bytes(&state.reader, checksum_bytes, 4)) {
        return failure(state, ZlibInflateError::truncated);
    }
    if (reader_consumed(state.reader) != state.reader.input_bytes) {
        return failure(state, ZlibInflateError::trailing_data);
    }
    if (expected_output_bytes != ~0u && state.output_bytes != expected_output_bytes) {
        return failure(state, ZlibInflateError::output_size);
    }
    const u32 expected_checksum = static_cast<u32>(checksum_bytes[0]) << 24 |
                                  static_cast<u32>(checksum_bytes[1]) << 16 |
                                  static_cast<u32>(checksum_bytes[2]) << 8 | checksum_bytes[3];
    if (adler32_update(1u, ConstSlice<u8>{output.data, state.output_bytes}) != expected_checksum) {
        return failure(state, ZlibInflateError::checksum);
    }
    return failure(state, ZlibInflateError::none);
}

ZlibInflateResult zlib_inflate_windowed(ConstSlice<DeflateInputSpan> input, Slice<u8> window,
                                        u32 expected_output_bytes, ZlibInflateSink sink, void* context,
                                        DeflateWorkspace* workspace) noexcept {
    InflateState state{};
    DecodeTables tables;
    state.output = window;
    state.workspace = workspace;
    state.tables = &tables;
    state.sink = sink;
    state.sink_context = context;
    state.adler = 1;
    if (!workspace || !sink || !window.data || window.count < kZlibWindowedMinimumBytes ||
        !reader_initialize(&state.reader, input))
        return failure(state, ZlibInflateError::invalid_input);
    ZlibInflateError header_error = ZlibInflateError::none;
    if (!read_zlib_header(&state.reader, &state.window_size, &header_error))
        return failure(state, header_error);
    const auto finish = [&](ZlibInflateError error) noexcept {
        auto value = failure(state, error);
        value.output_bytes = static_cast<u32>(minimum<u64>(state.delivered, ~0u));
        return value;
    };
    if (!decode_final_blocks(&state) || !slide_window(&state))
        return finish(state.error);
    u8 checksum_bytes[4]{};
    if (!read_aligned_bytes(&state.reader, checksum_bytes, 4))
        return finish(ZlibInflateError::truncated);
    if (reader_consumed(state.reader) != state.reader.input_bytes)
        return finish(ZlibInflateError::trailing_data);
    if (expected_output_bytes != ~0u && state.delivered != expected_output_bytes)
        return finish(ZlibInflateError::output_size);
    const u32 expected_checksum = static_cast<u32>(checksum_bytes[0]) << 24 |
                                  static_cast<u32>(checksum_bytes[1]) << 16 |
                                  static_cast<u32>(checksum_bytes[2]) << 8 | checksum_bytes[3];
    return finish(state.adler != expected_checksum ? ZlibInflateError::checksum : ZlibInflateError::none);
}

ZlibInflateResult gzip_inflate(ConstSlice<u8> input, Slice<u8> output,
                               DeflateWorkspace* workspace) noexcept {
    ZlibInflateResult result{};
    if (!input.data || !workspace || (!output.data && output.count) || !input.count) {
        result.error = ZlibInflateError::invalid_input;
        return result;
    }
    u32 cursor = 0;
    const auto fail = [&](ZlibInflateError error) noexcept {
        result.error = error;
        return result;
    };
    const auto le32 = [&](u32 at) noexcept {
        return static_cast<u32>(input[at]) | static_cast<u32>(input[at + 1]) << 8 |
               static_cast<u32>(input[at + 2]) << 16 | static_cast<u32>(input[at + 3]) << 24;
    };
    DecodeTables tables;
    while (cursor < input.count) {
        const u32 header = cursor;
        if (input.count - cursor < 18)
            return fail(ZlibInflateError::truncated);
        if (input[cursor] != 0x1f || input[cursor + 1] != 0x8b || input[cursor + 2] != 8 ||
            (input[cursor + 3] & 0xe0))
            return fail(ZlibInflateError::invalid_header);
        const u8 flags = input[cursor + 3];
        cursor += 10;
        if (flags & 4) {
            if (input.count - cursor < 2)
                return fail(ZlibInflateError::truncated);
            const u32 length = input[cursor] | static_cast<u32>(input[cursor + 1]) << 8;
            cursor += 2;
            if (length > input.count - cursor)
                return fail(ZlibInflateError::truncated);
            cursor += length;
        }
        for (u8 flag = 8; flag <= 16; flag <<= 1) {
            if (!(flags & flag))
                continue;
            while (cursor < input.count && input[cursor])
                ++cursor;
            if (cursor == input.count)
                return fail(ZlibInflateError::truncated);
            ++cursor;
        }
        if (flags & 2) {
            if (input.count - cursor < 2)
                return fail(ZlibInflateError::truncated);
            const u32 crc = input[cursor] | static_cast<u32>(input[cursor + 1]) << 8;
            if ((png_crc32(input.data + header, cursor - header) & 0xffffu) != crc)
                return fail(ZlibInflateError::checksum);
            cursor += 2;
        }
        if (input.count - cursor < 8)
            return fail(ZlibInflateError::truncated);
        const DeflateInputSpan span{input.data + cursor, input.count - cursor};
        InflateState state{};
        state.output = {output.data ? output.data + result.output_bytes : nullptr,
                        output.count - result.output_bytes};
        state.workspace = workspace;
        state.tables = &tables;
        state.window_size = 32768;
        if (!reader_initialize(&state.reader, {&span, 1}) || !decode_final_blocks(&state))
            return fail(state.error == ZlibInflateError::none ? ZlibInflateError::truncated : state.error);
        cursor += reader_consumed(state.reader);
        if (input.count - cursor < 8)
            return fail(ZlibInflateError::truncated);
        if (le32(cursor + 4) != state.output_bytes)
            return fail(ZlibInflateError::output_size);
        if (png_crc32(state.output.data, state.output_bytes) != le32(cursor))
            return fail(ZlibInflateError::checksum);
        cursor += 8;
        result.input_bytes = cursor;
        result.output_bytes += state.output_bytes;
        result.blocks += state.blocks;
        result.literals += state.literals;
        result.copies += state.copies;
    }
    return result;
}

bool zlib_stream_initialize(ZlibStreamInflater* stream) noexcept {
    if (!stream) {
        return false;
    }
    SecureZeroMemory(stream, sizeof(*stream));
    return true;
}

void zlib_stream_clear(ZlibStreamInflater* stream) noexcept {
    if (stream) {
        SecureZeroMemory(stream, sizeof(*stream));
    }
}

static ZlibInflateResult stream_inflate_flush(ZlibStreamInflater* stream, ConstSlice<DeflateInputSpan> input,
    Slice<u8> output, Slice<u8>* output_view = nullptr, u32 maximum_output_bytes = 0,
    ZlibStreamGrowOutput grow = nullptr, void* context = nullptr) noexcept {
    InflateState state{};
    DecodeTables tables;
    state.output = output;
    if (!stream || (!output.data && output.count != 0)) {
        return failure(state, ZlibInflateError::invalid_input);
    }
    if (stream->failed) {
        return failure(state, ZlibInflateError::stream_failed);
    }
    state.workspace = &stream->workspace;
    state.tables = &tables;
    state.stream = stream;
    state.output_view = output_view;
    state.maximum_output_bytes = maximum_output_bytes;
    state.grow_output = grow;
    state.grow_context = context;
    if (!reader_initialize(&state.reader, input)) {
        return failure(state, ZlibInflateError::invalid_input);
    }
    if (!input_ends_sync_flush(input)) {
        return poison_stream(stream, state, ZlibInflateError::invalid_flush);
    }

    if (!stream->initialized) {
        ZlibInflateError header_error = ZlibInflateError::none;
        if (!read_zlib_header(&state.reader, &stream->window_size, &header_error)) {
            return poison_stream(stream, state, header_error);
        }
        stream->initialized = true;
    }
    state.window_size = stream->window_size;

    while (reader_consumed(state.reader) < state.reader.input_bytes) {
        u32 final = 0;
        u32 type = 0;
        if (!read_bits(&state.reader, 1, &final) || !read_bits(&state.reader, 2, &type)) {
            return poison_stream(stream, state, ZlibInflateError::truncated);
        }
        if (final != 0) {
            return poison_stream(stream, state, ZlibInflateError::invalid_flush);
        }
        ++state.blocks;
        u16 stored_length = 0;
        if (!decode_block<true>(&state, type, &stored_length)) {
            return poison_stream(stream, state,
                                 state.error == ZlibInflateError::none ? ZlibInflateError::invalid_block
                                                                       : state.error);
        }
        if (type == 0 && stored_length == 0 && reader_consumed(state.reader) == state.reader.input_bytes) {
            commit_window(stream, state.output.data, state.output_bytes);
            return failure(state, ZlibInflateError::none);
        }
    }
    return poison_stream(stream, state, ZlibInflateError::invalid_flush);
}

ZlibInflateResult zlib_stream_inflate_flush(ZlibStreamInflater* stream, ConstSlice<DeflateInputSpan> input,
    Slice<u8> output) noexcept {
    return stream_inflate_flush(stream, input, output);
}
ZlibInflateResult zlib_stream_inflate_flush_growing(ZlibStreamInflater* stream,
    ConstSlice<DeflateInputSpan> input, Slice<u8>* output, u32 maximum_output_bytes,
    ZlibStreamGrowOutput grow, void* context) noexcept {
    InflateState state{};
    if (!stream || !output || !grow || !maximum_output_bytes ||
        !input.data || !input.count || input.count > ~0u / sizeof(DeflateInputSpan))
        return failure(state, ZlibInflateError::invalid_input);
    const ConstSlice<u8> owner{reinterpret_cast<const u8*>(stream), sizeof(*stream)};
    const ConstSlice<u8> view{reinterpret_cast<const u8*>(output), sizeof(*output)};
    const ConstSlice<u8> spans{reinterpret_cast<const u8*>(input.data),
        static_cast<u32>(input.count * sizeof(DeflateInputSpan))};
    if (!byte_range_valid(owner) || !byte_range_valid(view) || !byte_range_valid(spans) ||
        byte_ranges_overlap(view, owner) || byte_ranges_overlap(view, spans) ||
        byte_ranges_overlap(owner, spans)) return failure(state, ZlibInflateError::invalid_input);
    for (u32 i = 0; i < input.count; ++i) {
        const ConstSlice<u8> span{input[i].data, input[i].bytes};
        if (!byte_range_valid(span) || byte_ranges_overlap(span, view) || byte_ranges_overlap(span, owner))
            return failure(state, ZlibInflateError::invalid_input);
    }
    state.output = *output;
    state.output_view = output;
    state.stream = stream;
    state.reader.spans = input.data;
    state.reader.span_count = input.count;
    if (!output->count || output->count > maximum_output_bytes || !stream_output_disjoint(state, *output))
        return failure(state, ZlibInflateError::invalid_input);
    return stream_inflate_flush(stream, input, *output, output, maximum_output_bytes, grow, context);
}

} // namespace maicord
