// Frozen copy of the original bit-serial canonical-code decoder. It is used
// only as the oracle for differential tests of src/media/deflate.cpp.
#include "deflate_reference.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/sys/crt.hpp"

namespace maicord::deflate_reference {
namespace {

struct DeflatePrefixTable {
    u16 counts[16];
    u16 first_codes[16];
    u16 first_symbols[16];
    u8 maximum_bits;
    bool empty;
};

struct DeflateWorkspace {
    DeflatePrefixTable literal_table;
    DeflatePrefixTable distance_table;
    DeflatePrefixTable code_table;
    u16 literal_symbols[kDeflateLiteralSymbolCapacity];
    u16 distance_symbols[kDeflateDistanceSymbolCapacity];
    u16 code_symbols[kDeflateCodeSymbolCapacity];
    u8 lengths[kDeflateDynamicLengthCapacity];
};

struct BitReader {
    const DeflateInputSpan* spans;
    u32 span_count;
    u32 span_index;
    u32 span_offset;
    u32 input_bytes;
    u32 consumed_bytes;
    u32 bit_buffer;
    u8 bit_count;
};

struct InflateState {
    BitReader reader;
    Slice<u8> output;
    DeflateWorkspace* workspace;
    u32 output_bytes;
    u32 blocks;
    u32 literals;
    u32 copies;
    u32 window_size;
    ZlibInflateError error;
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

struct FixedDeflateTables {
    DeflatePrefixTable literal_table;
    DeflatePrefixTable distance_table;
    u16 literal_symbols[kDeflateLiteralSymbolCapacity];
    u16 distance_symbols[kDeflateDistanceSymbolCapacity];
};

constexpr void assign_canonical_symbols(const u8* lengths, u32 symbol_count, DeflatePrefixTable* table,
                                        u16* symbols) noexcept {
    u32 code = 0;
    u32 first_symbol = 0;
    for (u32 bits = 1; bits <= 15; ++bits) {
        code = (code + table->counts[bits - 1]) << 1;
        table->first_codes[bits] = static_cast<u16>(code);
        table->first_symbols[bits] = static_cast<u16>(first_symbol);
        first_symbol += table->counts[bits];
    }
    u16 offsets[16]{};
    for (u32 bits = 1; bits <= 15; ++bits) {
        offsets[bits] = table->first_symbols[bits];
    }
    for (u32 symbol = 0; symbol < symbol_count; ++symbol) {
        const u8 length = lengths[symbol];
        if (length != 0) {
            symbols[offsets[length]++] = static_cast<u16>(symbol);
        }
    }
}

consteval void make_constant_table(const u8* lengths, u32 symbol_count, DeflatePrefixTable* table,
                                   u16* symbols) noexcept {
    for (u32 symbol = 0; symbol < symbol_count; ++symbol) {
        const u8 length = lengths[symbol];
        if (length != 0) {
            ++table->counts[length];
            if (length > table->maximum_bits) {
                table->maximum_bits = length;
            }
        }
    }
    assign_canonical_symbols(lengths, symbol_count, table, symbols);
}

consteval FixedDeflateTables make_fixed_tables() noexcept {
    FixedDeflateTables tables{};
    u8 literal_lengths[kDeflateLiteralSymbolCapacity]{};
    for (u32 symbol = 0; symbol < kDeflateLiteralSymbolCapacity; ++symbol) {
        literal_lengths[symbol] = symbol <= 143 ? 8 : symbol <= 255 ? 9 : symbol <= 279 ? 7 : 8;
    }
    make_constant_table(literal_lengths, kDeflateLiteralSymbolCapacity, &tables.literal_table,
                        tables.literal_symbols);
    u8 distance_lengths[kDeflateDistanceSymbolCapacity]{};
    for (u32 symbol = 0; symbol < kDeflateDistanceSymbolCapacity; ++symbol) {
        distance_lengths[symbol] = 5;
    }
    make_constant_table(distance_lengths, kDeflateDistanceSymbolCapacity, &tables.distance_table,
                        tables.distance_symbols);
    return tables;
}

inline constexpr FixedDeflateTables kFixedTables = make_fixed_tables();

ZlibInflateResult failure(const InflateState& state, ZlibInflateError error) noexcept {
    return ZlibInflateResult{state.reader.input_bytes, state.output_bytes, state.blocks,
                             state.literals,           state.copies,       error};
}

bool reader_initialize(BitReader* reader, ConstSlice<DeflateInputSpan> spans) noexcept {
    if (!reader || !spans.data || spans.count == 0) {
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
    return true;
}

bool read_raw_byte(BitReader* reader, u8* value) noexcept {
    if (!reader || !value) {
        return false;
    }
    while (reader->span_index < reader->span_count &&
           reader->span_offset >= reader->spans[reader->span_index].bytes) {
        ++reader->span_index;
        reader->span_offset = 0;
    }
    if (reader->span_index >= reader->span_count) {
        return false;
    }
    *value = reader->spans[reader->span_index].data[reader->span_offset++];
    ++reader->consumed_bytes;
    return true;
}

bool read_byte(BitReader* reader, u8* value) noexcept {
    return reader && reader->bit_count == 0 && read_raw_byte(reader, value);
}

bool fill_bits(BitReader* reader, u32 count) noexcept {
    while (reader->bit_count < count) {
        u8 byte = 0;
        if (!read_raw_byte(reader, &byte)) {
            return false;
        }
        reader->bit_buffer |= static_cast<u32>(byte) << reader->bit_count;
        reader->bit_count = static_cast<u8>(reader->bit_count + 8);
    }
    return true;
}

bool read_bits(BitReader* reader, u32 count, u32* value) noexcept {
    if (!reader || !value || count > 16 || !fill_bits(reader, count)) {
        return false;
    }
    const u32 mask = count == 0 ? 0 : (1u << count) - 1u;
    *value = reader->bit_buffer & mask;
    reader->bit_buffer >>= count;
    reader->bit_count = static_cast<u8>(reader->bit_count - count);
    return true;
}

void align_reader(BitReader* reader) noexcept {
    if (reader) {
        reader->bit_buffer = 0;
        reader->bit_count = 0;
    }
}

bool build_table(const u8* lengths, u32 symbol_count, DeflatePrefixTable* table, u16* symbols,
                 bool allow_empty, bool allow_single) noexcept {
    if (!lengths || !table || !symbols || symbol_count == 0) {
        return false;
    }
    *table = DeflatePrefixTable{};
    u32 used = 0;
    for (u32 symbol = 0; symbol < symbol_count; ++symbol) {
        const u8 length = lengths[symbol];
        if (length > 15) {
            return false;
        }
        if (length != 0) {
            ++table->counts[length];
            table->maximum_bits = maximum(table->maximum_bits, length);
            ++used;
        }
    }
    if (used == 0) {
        table->empty = true;
        return allow_empty;
    }

    i32 remaining = 1;
    for (u32 bits = 1; bits <= 15; ++bits) {
        remaining = (remaining << 1) - table->counts[bits];
        if (remaining < 0) {
            return false;
        }
    }
    if (remaining != 0 && !(allow_single && used == 1 && table->maximum_bits == 1)) {
        return false;
    }

    assign_canonical_symbols(lengths, symbol_count, table, symbols);
    return true;
}

bool decode_symbol(InflateState* state, const DeflatePrefixTable& table, const u16* symbols,
                   u32* symbol) noexcept {
    if (!state || !symbols || !symbol || table.empty) {
        return false;
    }
    u32 code = 0;
    for (u32 bits = 1; bits <= table.maximum_bits; ++bits) {
        u32 bit = 0;
        if (!read_bits(&state->reader, 1, &bit)) {
            state->error = ZlibInflateError::truncated;
            return false;
        }
        code = (code << 1) | bit;
        const u32 first = table.first_codes[bits];
        const u32 count = table.counts[bits];
        if (code >= first && code - first < count) {
            *symbol = symbols[table.first_symbols[bits] + code - first];
            return true;
        }
    }
    state->error = ZlibInflateError::invalid_huffman;
    return false;
}

bool ensure_output(InflateState* state, u32 bytes) noexcept {
    if (bytes <= state->output.count - state->output_bytes) return true;
    state->error = ZlibInflateError::output_capacity;
    return false;
}
bool emit_byte(InflateState* state, u8 value) noexcept {
    if (!ensure_output(state, 1)) {
        state->error = ZlibInflateError::output_capacity;
        return false;
    }
    state->output[state->output_bytes++] = value;
    return true;
}

bool build_dynamic_tables(InflateState* state) noexcept {
    u32 hlit_bits = 0;
    u32 hdist_bits = 0;
    u32 hclen_bits = 0;
    if (!read_bits(&state->reader, 5, &hlit_bits) || !read_bits(&state->reader, 5, &hdist_bits) ||
        !read_bits(&state->reader, 4, &hclen_bits)) {
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
        state->workspace->lengths[index] = 0;
    }
    for (u32 index = 0; index < code_count; ++index) {
        u32 length = 0;
        if (!read_bits(&state->reader, 3, &length)) {
            state->error = ZlibInflateError::truncated;
            return false;
        }
        state->workspace->lengths[kCodeLengthOrder[index]] = static_cast<u8>(length);
    }
    if (!build_table(state->workspace->lengths, 19, &state->workspace->code_table,
                     state->workspace->code_symbols, false, false)) {
        state->error = ZlibInflateError::invalid_huffman;
        return false;
    }

    const u32 total = literal_count + distance_count;
    u32 cursor = 0;
    while (cursor < total) {
        u32 symbol = 0;
        if (!decode_symbol(state, state->workspace->code_table, state->workspace->code_symbols, &symbol)) {
            return false;
        }
        if (symbol <= 15) {
            state->workspace->lengths[cursor++] = static_cast<u8>(symbol);
            continue;
        }
        u32 extra = 0;
        u32 repeat = 0;
        u8 value = 0;
        if (symbol == 16) {
            if (cursor == 0 || !read_bits(&state->reader, 2, &extra)) {
                state->error =
                    cursor == 0 ? ZlibInflateError::invalid_huffman : ZlibInflateError::truncated;
                return false;
            }
            repeat = extra + 3;
            value = state->workspace->lengths[cursor - 1];
        } else if (symbol == 17) {
            if (!read_bits(&state->reader, 3, &extra)) {
                state->error = ZlibInflateError::truncated;
                return false;
            }
            repeat = extra + 3;
        } else if (symbol == 18) {
            if (!read_bits(&state->reader, 7, &extra)) {
                state->error = ZlibInflateError::truncated;
                return false;
            }
            repeat = extra + 11;
        } else {
            state->error = ZlibInflateError::invalid_symbol;
            return false;
        }
        if (repeat > total - cursor) {
            state->error = ZlibInflateError::invalid_huffman;
            return false;
        }
        for (u32 index = 0; index < repeat; ++index) {
            state->workspace->lengths[cursor++] = value;
        }
    }
    if (state->workspace->lengths[256] == 0 ||
        !build_table(state->workspace->lengths, literal_count, &state->workspace->literal_table,
                     state->workspace->literal_symbols, false, true) ||
        !build_table(state->workspace->lengths + literal_count, distance_count,
                     &state->workspace->distance_table, state->workspace->distance_symbols, true, true)) {
        state->error = ZlibInflateError::invalid_huffman;
        return false;
    }
    return true;
}

bool decode_compressed_block(InflateState* state, const DeflatePrefixTable& literal_table,
                             const u16* literal_symbols, const DeflatePrefixTable& distance_table,
                             const u16* distance_symbols) noexcept {
    for (;;) {
        u32 symbol = 0;
        if (!decode_symbol(state, literal_table, literal_symbols, &symbol)) {
            return false;
        }
        if (symbol < 256) {
            if (!emit_byte(state, static_cast<u8>(symbol))) {
                return false;
            }
            ++state->literals;
            continue;
        }
        if (symbol == 256) {
            return true;
        }
        if (symbol < 257 || symbol > 285) {
            state->error = ZlibInflateError::invalid_symbol;
            return false;
        }
        const u32 length_index = symbol - 257;
        u32 extra = 0;
        if (!read_bits(&state->reader, kLengthExtraBits[length_index], &extra)) {
            state->error = ZlibInflateError::truncated;
            return false;
        }
        const u32 length = kLengthBases[length_index] + extra;

        u32 distance_symbol = 0;
        if (!decode_symbol(state, distance_table, distance_symbols, &distance_symbol)) {
            if (state->error == ZlibInflateError::none) {
                state->error = ZlibInflateError::invalid_distance;
            }
            return false;
        }
        if (distance_symbol >= 30) {
            state->error = ZlibInflateError::invalid_distance;
            return false;
        }
        if (!read_bits(&state->reader, kDistanceExtraBits[distance_symbol], &extra)) {
            state->error = ZlibInflateError::truncated;
            return false;
        }
        const u32 distance = kDistanceBases[distance_symbol] + extra;
        const bool unavailable_distance = distance > state->output_bytes;
        if (distance == 0 || unavailable_distance || distance > state->window_size) {
            state->error = unavailable_distance || distance > state->window_size
                               ? ZlibInflateError::invalid_distance
                               : ZlibInflateError::output_capacity;
            return false;
        }
        if (!ensure_output(state, length)) return false;
        u8* destination = state->output.data + state->output_bytes;
        if (distance == 1) {
            memset(destination, destination[-1], length);
        } else {
            // Seed from initialized history, then double the initialized
            // prefix. Each copy is non-overlapping. A single memmove of an
            // overlapping LZ match would read bytes not produced yet.
            u32 copied = minimum(distance, length);
            copy_bytes(destination, destination - distance, copied);
            while (copied < length) {
                const u32 count = minimum(copied, length - copied);
                copy_bytes(destination + copied, destination, count);
                copied += count;
            }
        }
        state->output_bytes += length;
        ++state->copies;
    }
}

bool decode_stored_block(InflateState* state) noexcept {
    align_reader(&state->reader);
    u8 len_low = 0;
    u8 len_high = 0;
    u8 nlen_low = 0;
    u8 nlen_high = 0;
    if (!read_byte(&state->reader, &len_low) || !read_byte(&state->reader, &len_high) ||
        !read_byte(&state->reader, &nlen_low) || !read_byte(&state->reader, &nlen_high)) {
        state->error = ZlibInflateError::truncated;
        return false;
    }
    const u16 length = static_cast<u16>(len_low | len_high << 8);
    const u16 inverse = static_cast<u16>(nlen_low | nlen_high << 8);
    if (static_cast<u16>(length ^ 0xFFFFu) != inverse) {
        state->error = ZlibInflateError::invalid_block;
        return false;
    }
    if (!ensure_output(state, length)) {
        state->error = ZlibInflateError::output_capacity;
        return false;
    }
    for (u32 index = 0; index < length; ++index) {
        u8 value = 0;
        if (!read_byte(&state->reader, &value)) {
            state->error = ZlibInflateError::truncated;
            return false;
        }
        state->output[state->output_bytes++] = value;
    }
    state->literals += length;
    return true;
}

u32 adler32(ConstSlice<u8> bytes) noexcept {
    u64 first = 1;
    u64 second = 0;
    u32 cursor = 0;
    while (cursor < bytes.count) {
        const u32 batch = minimum<u32>(5'552, bytes.count - cursor);
        for (u32 index = 0; index < batch; ++index) {
            first += bytes[cursor++];
            second += first;
        }
        first %= 65'521;
        second %= 65'521;
    }
    return static_cast<u32>((second << 16) | first);
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
        bool decoded = false;
        switch (type) {
        case 0:
            decoded = decode_stored_block(state);
            break;
        case 1:
            decoded = decode_compressed_block(
                state, kFixedTables.literal_table, kFixedTables.literal_symbols,
                kFixedTables.distance_table, kFixedTables.distance_symbols);
            break;
        case 2:
            if (build_dynamic_tables(state))
                decoded = decode_compressed_block(
                    state, state->workspace->literal_table, state->workspace->literal_symbols,
                    state->workspace->distance_table, state->workspace->distance_symbols);
            break;
        default:
            state->error = ZlibInflateError::invalid_block;
            break;
        }
        if (!decoded) {
            if (state->error == ZlibInflateError::none)
                state->error = ZlibInflateError::invalid_block;
            return false;
        }
    }
    align_reader(&state->reader);
    return true;
}

ZlibInflateResult zlib_inflate_impl(ConstSlice<DeflateInputSpan> input, Slice<u8> output,
                               u32 expected_output_bytes, DeflateWorkspace* workspace) noexcept {
    InflateState state{};
    state.output = output;
    state.workspace = workspace;
    if (!workspace || (!output.data && output.count != 0) || !reader_initialize(&state.reader, input)) {
        return failure(state, ZlibInflateError::invalid_input);
    }
    if (expected_output_bytes != ~0u && expected_output_bytes > output.count) {
        return failure(state, ZlibInflateError::output_capacity);
    }
    *workspace = DeflateWorkspace{};

    u8 cmf = 0;
    u8 flags = 0;
    if (!read_byte(&state.reader, &cmf) || !read_byte(&state.reader, &flags)) {
        return failure(state, ZlibInflateError::truncated);
    }
    if ((cmf & 0x0Fu) != 8 || (cmf >> 4) > 7) {
        return failure(state, ZlibInflateError::unsupported_method);
    }
    if ((static_cast<u32>(cmf) * 256u + flags) % 31u != 0) {
        return failure(state, ZlibInflateError::invalid_header);
    }
    if ((flags & 0x20u) != 0) {
        return failure(state, ZlibInflateError::unsupported_dictionary);
    }
    state.window_size = 1u << ((cmf >> 4) + 8u);

    if (!decode_final_blocks(&state))
        return failure(state, state.error);

    align_reader(&state.reader);
    u8 checksum_bytes[4]{};
    for (u32 index = 0; index < 4; ++index) {
        if (!read_byte(&state.reader, &checksum_bytes[index])) {
            return failure(state, ZlibInflateError::truncated);
        }
    }
    if (state.reader.consumed_bytes != state.reader.input_bytes) {
        return failure(state, ZlibInflateError::trailing_data);
    }
    if (expected_output_bytes != ~0u && state.output_bytes != expected_output_bytes) {
        return failure(state, ZlibInflateError::output_size);
    }
    const u32 expected_checksum = static_cast<u32>(checksum_bytes[0]) << 24 |
                                  static_cast<u32>(checksum_bytes[1]) << 16 |
                                  static_cast<u32>(checksum_bytes[2]) << 8 | checksum_bytes[3];
    if (adler32(ConstSlice<u8>{output.data, state.output_bytes}) != expected_checksum) {
        return failure(state, ZlibInflateError::checksum);
    }
    return failure(state, ZlibInflateError::none);
}

DeflateWorkspace g_workspace;

} // namespace

ZlibInflateResult zlib_inflate(ConstSlice<DeflateInputSpan> input, Slice<u8> output,
                               u32 expected_output_bytes) noexcept {
    return zlib_inflate_impl(input, output, expected_output_bytes, &g_workspace);
}

} // namespace maicord::deflate_reference
