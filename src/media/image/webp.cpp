#include "maicord/media/image/webp.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/simd.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {
namespace {

inline constexpr u32 kRiff = byte_fourcc_le('R', 'I', 'F', 'F');
inline constexpr u32 kWebp = byte_fourcc_le('W', 'E', 'B', 'P');
inline constexpr u32 kVp8 = byte_fourcc_le('V', 'P', '8', ' ');
inline constexpr u32 kVp8l = byte_fourcc_le('V', 'P', '8', 'L');
inline constexpr u32 kVp8x = byte_fourcc_le('V', 'P', '8', 'X');
inline constexpr u32 kAlph = byte_fourcc_le('A', 'L', 'P', 'H');
inline constexpr u32 kAnim = byte_fourcc_le('A', 'N', 'I', 'M');
inline constexpr u32 kAnmf = byte_fourcc_le('A', 'N', 'M', 'F');
inline constexpr u32 kIccp = byte_fourcc_le('I', 'C', 'C', 'P');
inline constexpr u32 kExif = byte_fourcc_le('E', 'X', 'I', 'F');
inline constexpr u32 kXmp = byte_fourcc_le('X', 'M', 'P', ' ');

WebpParseResult parse_failure(WebpParseResult value, WebpError error) noexcept {
    value.error = error;
    return value;
}

struct BitReader {
    ConstSlice<u8> bytes;
    u64 bit_position;
    bool failed;
};

__forceinline u64 total_bits(const BitReader& reader) noexcept {
    return static_cast<u64>(reader.bytes.count) * 8u;
}

// The last bytes of the stream, LSB first; zero past the end. Out of line:
// only the final eight bytes take this path.
__declspec(noinline) u64 peek_tail(const u8* data, u64 byte, u64 count) noexcept {
    u64 value = 0;
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (u64 index = count; index > byte; --index) {
        value = value << 8 | data[index - 1u];
    }
    return value;
}

// The bits from bit_position on, LSB first: at least 57 while eight bytes
// remain, otherwise every remaining bit; zero past the end.
__forceinline u64 peek_bits(const BitReader& reader) noexcept {
    const u64 byte = reader.bit_position >> 3;
    const u32 shift = static_cast<u32>(reader.bit_position & 7u);
    const u64 value =
        byte + 8u <= reader.bytes.count
            ? static_cast<u64>(
                  _mm_cvtsi128_si64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(reader.bytes.data + byte))))
            : peek_tail(reader.bytes.data, byte, reader.bytes.count);
    return value >> shift;
}

// Reading past the end fails without consuming, like a bit-serial reader.
u32 read_bits(BitReader* reader, u32 count) noexcept {
    if (count > 24 || reader->bit_position + count > total_bits(*reader)) {
        reader->failed = true;
        return 0;
    }
    const u32 value = static_cast<u32>(peek_bits(*reader)) & ((1u << count) - 1u);
    reader->bit_position += count;
    return value;
}

// A decode needing bits past the end: a bit-serial reader consumed every
// remaining bit before its read failed.
__forceinline bool fail_at_end(BitReader* reader) noexcept {
    reader->bit_position = total_bits(*reader);
    reader->failed = true;
    return false;
}

// The code-length code: 19 symbols, codewords of at most 7 bits.
struct SmallHuffmanCode {
    u16 counts[16];
    u16 symbols[19];
    u16 symbol_count;
    u16 single_symbol;
    u8 table_bits;
    // By the next table_bits stream bits: symbol | length << 5.
    u8 table[1u << 7];
};

// Counts the code lengths; only a single-symbol code or a complete prefix code is admitted.
bool count_code(const u8* lengths, u32 alphabet_size, u16* counts, u16* symbol_count,
                u16* single_symbol) noexcept {
    for (u32 symbol = 0; symbol < alphabet_size; ++symbol) {
        const u8 length = lengths[symbol];
        if (length > 15) {
            return false;
        }
        if (length != 0) {
            ++counts[length];
            ++*symbol_count;
            *single_symbol = static_cast<u16>(symbol);
        }
    }
    if (*symbol_count == 0) {
        return false;
    }
    if (*symbol_count != 1) {
        i32 open = 1;
#pragma clang loop unroll(disable)
        for (u32 length = 1; length <= 15; ++length) {
            open = open * 2 - counts[length];
            if (open < 0) {
                return false;
            }
        }
        if (open != 0) {
            return false;
        }
    }
    return true;
}

// Symbols with a code, by length and then value: the canonical decode order,
// as a counting sort over the counts of each length.
void sort_symbols(const u8* lengths, u32 alphabet_size, const u16* counts, u16* output) noexcept {
    u16 next[16];
    u32 offset = 0;
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (u32 length = 1; length <= 15; ++length) {
        next[length] = static_cast<u16>(offset);
        offset += counts[length];
    }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (u32 symbol = 0; symbol < alphabet_size; ++symbol) {
        const u32 length = lengths[symbol];
        if (length != 0) {
            output[next[length]++] = static_cast<u16>(symbol);
        }
    }
}

// The next codeword of the same length in stream order: the canonical code plus
// one, with the codeword's bits reversed (the stream is read LSB first).
__forceinline u32 next_reversed_code(u32 code, u32 length) noexcept {
    u32 bit = 1u << (length - 1u);
    while ((code & bit) != 0) {
        code ^= bit;
        bit >>= 1;
    }
    return code | bit;
}

bool build_small_code(const u8* lengths, u32 alphabet_size, SmallHuffmanCode* code) noexcept {
    reset_value<SmallHuffmanCode>(code);
    if (!count_code(lengths, alphabet_size, code->counts, &code->symbol_count, &code->single_symbol)) {
        return false;
    }
    sort_symbols(lengths, alphabet_size, code->counts, code->symbols);
    if (code->symbol_count < 2) {
        return true;
    }
    // Lengths are 3-bit values: every codeword fits the 7-bit table.
    u32 longest = 7;
    while (code->counts[longest] == 0) {
        --longest;
    }
    const u32 size = 1u << longest;
    u32 reversed = 0;
    u32 sorted = 0;
#pragma clang loop unroll(disable)
    for (u32 length = 1; length <= longest; ++length) {
        for (u32 left = code->counts[length]; left != 0; --left) {
            const u8 entry = static_cast<u8>(code->symbols[sorted++] | length << 5);
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
            for (u32 index = reversed; index < size; index += 1u << length) {
                code->table[index] = entry;
            }
            reversed = next_reversed_code(reversed, length);
        }
    }
    code->table_bits = static_cast<u8>(longest);
    return true;
}

// The next code-length symbol: the table decode of decode_canonical's codeword.
bool decode_length_symbol(BitReader* reader, const SmallHuffmanCode& code, u16* symbol) noexcept {
    if (code.symbol_count == 1) {
        *symbol = code.single_symbol;
        return true;
    }
    // Past the end peek_bits reads zeros: a codeword longer than the remaining
    // bits is where a bit-serial decode runs out.
    const u32 entry = code.table[static_cast<u32>(peek_bits(*reader)) & ((1u << code.table_bits) - 1u)];
    const u32 length = entry >> 5;
    if (length > total_bits(*reader) - reader->bit_position) {
        return fail_at_end(reader);
    }
    reader->bit_position += length;
    *symbol = static_cast<u16>(entry & 31u);
    return true;
}

// Canonical decoding of the next codeword, bit-serial semantics: the first
// read bit is the codeword's most significant bit. Codes whose tables did not
// fit the lookup capacity decode here.
bool decode_canonical(BitReader* reader, const u16* counts, u32 symbol_count, u16 single_symbol, const u16* symbols,
                      u16* symbol) noexcept {
    if (symbol_count == 1) {
        *symbol = single_symbol;
        return true;
    }
    const u64 available = total_bits(*reader) - reader->bit_position;
    const u64 bits = peek_bits(*reader);
    u32 canonical = 0;
    u32 first = 0;
    u32 base = 0;
#pragma clang loop unroll(disable)
    for (u32 length = 1; length <= 15; ++length) {
        if (length > available) {
            return fail_at_end(reader);
        }
        canonical = canonical << 1 | static_cast<u32>(bits >> (length - 1u) & 1u);
        const u32 count = counts[length];
        if (canonical >= first && canonical - first < count) {
            const u32 index = base + canonical - first;
            reader->bit_position += length;
            *symbol = symbols[index];
            return true;
        }
        base += count;
        first = (first + count) << 1;
    }
    reader->bit_position += 15;
    return false;
}

// Index width of the second-level table that opens at codeword length
// `length`: the length at which the codes still to place (`left`) fill the
// table's share of the code space, a complete code filling it exactly.
u32 second_level_bits(const u16* left, u32 length, u32 root_bits) noexcept {
    i32 space = 1 << (length - root_bits);
    while (length < 15) {
        space -= left[length];
        if (space <= 0) {
            break;
        }
        ++length;
        space <<= 1;
    }
    return length - root_bits;
}

// Lookup tables of one code, the two-level layout of libwebp's
// src/utils/huffman_utils.c: a root table indexed by the next root_bits stream
// bits and, per root entry whose codewords are longer, a second-level table
// indexed by the bits after them. Entry = value | length << 12. A root entry
// with a length past root_bits links to its second-level table: value is the
// table's offset from the root, length - root_bits its index width. Second-level
// entries hold the codeword length - root_bits. A null table measures: the
// return is the entry count either way.
u32 build_tables(const u16* counts, const u16* symbols, u32 longest, u32 root_bits, u16* table) noexcept {
    const u32 root_size = 1u << root_bits;
    u32 reversed = 0;
    u32 sorted = 0;
    if (table) {
#pragma clang loop unroll(disable)
        for (u32 length = 1; length <= root_bits; ++length) {
            for (u32 left = counts[length]; left != 0; --left) {
                const u16 entry = static_cast<u16>(symbols[sorted++] | length << 12);
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
                for (u32 index = reversed; index < root_size; index += 1u << length) {
                    table[index] = entry;
                }
                reversed = next_reversed_code(reversed, length);
            }
        }
    }
    u16 left[16];
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (u32 length = 0; length < 16; ++length) {
        left[length] = counts[length];
    }
    u32 size = root_size;
    u32 current = 0;
    u32 current_bits = 0;
    u32 open = 0; // codewords of the current length the open table has room for
#pragma clang loop unroll(disable)
    for (u32 length = root_bits + 1; length <= longest; ++length) {
        open <<= 1;
        const u32 sub_length = length - root_bits;
        for (; left[length] != 0; --left[length]) {
            if (open == 0) {
                current_bits = second_level_bits(left, length, root_bits);
                current = size;
                size += 1u << current_bits;
                open = 1u << sub_length;
                if (table) {
                    table[reversed & (root_size - 1u)] = static_cast<u16>(current | (root_bits + current_bits) << 12);
                }
            }
            --open;
            if (table) {
                const u16 entry = static_cast<u16>(symbols[sorted++] | sub_length << 12);
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
                for (u32 index = reversed >> root_bits; index < (1u << current_bits); index += 1u << sub_length) {
                    table[current + index] = entry;
                }
                reversed = next_reversed_code(reversed, length);
            }
        }
    }
    return size;
}

void build_lookup(Vp8lWorkspace* workspace, Vp8lHuffmanCode* code) noexcept {
    code->table_bits = 0;
    if (code->symbol_count < 2) {
        return;
    }
    u32 longest = 15;
    while (code->counts[longest] == 0) {
        --longest;
    }
    const u32 root_bits = minimum(longest, kVp8lLookupBits);
    const u32 size = longest > root_bits ? build_tables(code->counts, nullptr, longest, root_bits, nullptr)
                                         : 1u << root_bits;
    // Link offsets are 12-bit entry values.
    if (size > kVp8lLookupCapacity - workspace->lookup_count || size > 0x1000u) {
        return;
    }
    build_tables(code->counts, workspace->symbols + code->symbol_offset, longest, root_bits,
                 workspace->lookup + workspace->lookup_count);
    code->table_offset = workspace->lookup_count;
    code->table_bits = static_cast<u8>(root_bits);
    workspace->lookup_count += size;
}

bool build_code(Vp8lWorkspace* workspace, u32 alphabet_size, Vp8lHuffmanCode* code) noexcept {
    reset_value<Vp8lHuffmanCode>(code);
    if (!count_code(workspace->code_lengths, alphabet_size, code->counts, &code->symbol_count,
                    &code->single_symbol) ||
        workspace->symbol_count + code->symbol_count > kVp8lHuffmanSymbolCapacity) {
        return false;
    }
    code->symbol_offset = workspace->symbol_count;
    sort_symbols(workspace->code_lengths, alphabet_size, code->counts, workspace->symbols + workspace->symbol_count);
    workspace->symbol_count += code->symbol_count;
    build_lookup(workspace, code);
    return true;
}

// A simple code: one symbol, or two of codeword length 1. The state build_code
// gives the same code lengths, without a pass over the alphabet.
bool build_simple_code(Vp8lWorkspace* workspace, u32 first, u32 second, Vp8lHuffmanCode* code) noexcept {
    reset_value<Vp8lHuffmanCode>(code);
    const u32 count = first == second ? 1u : 2u;
    if (workspace->symbol_count + count > kVp8lHuffmanSymbolCapacity) {
        return false;
    }
    code->counts[1] = static_cast<u16>(count);
    code->symbol_count = static_cast<u16>(count);
    code->single_symbol = static_cast<u16>(maximum(first, second));
    code->symbol_offset = workspace->symbol_count;
    workspace->symbols[workspace->symbol_count] = static_cast<u16>(minimum(first, second));
    workspace->symbols[workspace->symbol_count + count - 1u] = static_cast<u16>(maximum(first, second));
    workspace->symbol_count += count;
    build_lookup(workspace, code);
    return true;
}

// The pixel loop's view of the reader: the next `available` bits of the
// stream at bit_position, refilled from memory only when it runs low, so a
// decode does not wait on a load. available is exact near the end.
struct BitWindow {
    u64 bits;
    u32 available;
};

__forceinline void window_fill(const BitReader& reader, BitWindow* window) noexcept {
    window->bits = peek_bits(reader);
    const u64 remaining = total_bits(reader) - reader.bit_position;
    window->available = remaining < 57 ? static_cast<u32>(remaining) : 57u;
}

__forceinline void window_consume(BitReader* reader, BitWindow* window, u32 count) noexcept {
    reader->bit_position += count;
    window->bits >>= count;
    window->available -= count;
}

// read_bits through the window: same failure rule, no consumption on failure.
__forceinline u32 window_read_bits(BitReader* reader, BitWindow* window, u32 count) noexcept {
    if (window->available < count) {
        window_fill(*reader, window);
        if (window->available < count) {
            reader->failed = true;
            return 0;
        }
    }
    const u32 value = static_cast<u32>(window->bits) & ((1u << count) - 1u);
    window_consume(reader, window, count);
    return value;
}

// The next symbol through the window; codes without tables decode canonically.
__forceinline bool window_decode(BitReader* reader, BitWindow* window, const Vp8lWorkspace* workspace,
                                 const Vp8lHuffmanCode& code, u16* symbol) noexcept {
    if (code.symbol_count == 1) {
        *symbol = code.single_symbol;
        return true;
    }
    if (code.table_bits != 0) {
        if (window->available < 15) {
            window_fill(*reader, window);
        }
        const u16* table = workspace->lookup + code.table_offset;
        const u32 root_bits = code.table_bits;
        const u32 bits = static_cast<u32>(window->bits);
        u32 entry = table[bits & ((1u << root_bits) - 1u)];
        u32 length = entry >> 12;
        if (length > root_bits) {
            entry = table[(entry & 0xFFFu) + (bits >> root_bits & ((1u << (length - root_bits)) - 1u))];
            length = root_bits + (entry >> 12);
        }
        // Below 15 bits the window holds exactly the rest of the stream, zeros
        // after it: a codeword longer than that is where a bit-serial decode runs out.
        if (length > window->available) {
            return fail_at_end(reader);
        }
        window_consume(reader, window, length);
        *symbol = static_cast<u16>(entry & 0xFFFu);
        return true;
    }
    const bool decoded = decode_canonical(reader, code.counts, code.symbol_count, code.single_symbol,
                                          workspace->symbols + code.symbol_offset, symbol);
    window_fill(*reader, window);
    return decoded;
}

inline constexpr u8 kCodeLengthOrder[19] = {
    17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

bool read_huffman_code(BitReader* reader, u32 alphabet_size, Vp8lWorkspace* workspace,
                       Vp8lHuffmanCode* code) noexcept {
    if (read_bits(reader, 1) != 0) {
        const u32 symbols = read_bits(reader, 1) + 1;
        const u32 first_bits = read_bits(reader, 1) != 0 ? 8u : 1u;
        const u32 first = read_bits(reader, first_bits);
        if (reader->failed || first >= alphabet_size) {
            return false;
        }
        u32 second = first;
        if (symbols == 2) {
            second = read_bits(reader, 8);
            if (reader->failed || second >= alphabet_size) {
                return false;
            }
        }
        return build_simple_code(workspace, first, second, code);
    }

    for (u32 index = 0; index < alphabet_size; ++index) {
        workspace->code_lengths[index] = 0;
    }
    u8 length_code_lengths[19]{};
    const u32 length_code_count = read_bits(reader, 4) + 4;
    for (u32 index = 0; index < length_code_count; ++index) {
        length_code_lengths[kCodeLengthOrder[index]] = static_cast<u8>(read_bits(reader, 3));
    }
    SmallHuffmanCode length_code;
    if (reader->failed || !build_small_code(length_code_lengths, 19, &length_code)) {
        return false;
    }
    u32 maximum_codes = alphabet_size;
    if (read_bits(reader, 1) != 0) {
        const u32 bits = 2 + 2 * read_bits(reader, 3);
        maximum_codes = 2 + read_bits(reader, bits);
        if (reader->failed || maximum_codes > alphabet_size) {
            return false;
        }
    }
    u32 output = 0;
    u8 previous = 8;
    while (output < alphabet_size && maximum_codes != 0) {
        --maximum_codes;
        u16 value = 0;
        if (!decode_length_symbol(reader, length_code, &value)) {
            return false;
        }
        if (value < 16) {
            workspace->code_lengths[output++] = static_cast<u8>(value);
            if (value != 0) {
                previous = static_cast<u8>(value);
            }
            continue;
        }
        const u32 extra_bits = value == 16 ? 2u : value == 17 ? 3u : 7u;
        const u32 offset = value == 18 ? 11u : 3u;
        const u32 repeat = offset + read_bits(reader, extra_bits);
        if (reader->failed || value > 18 || repeat > alphabet_size - output) {
            return false;
        }
        const u8 repeated = value == 16 ? previous : 0;
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
        for (u32 index = 0; index < repeat; ++index) {
            workspace->code_lengths[output++] = repeated;
        }
    }
    return build_code(workspace, alphabet_size, code);
}

} // namespace

// The chunk walk runs once per image, before the decode: compiled for size.
__attribute__((minsize))
WebpParseResult webp_parse(ConstSlice<u8> encoded, WebpImage* image) noexcept {
    WebpParseResult result{};
    if (!image || (!encoded.data && encoded.count != 0)) {
        return parse_failure(result, WebpError::invalid_input);
    }
    reset_value<WebpImage>(image);
    if (encoded.count > kWebpEncodedMaximumBytes) {
        return parse_failure(result, WebpError::encoded_limit);
    }
    if (encoded.count < 20) {
        return parse_failure(result, WebpError::truncated);
    }
    if (byte_load_le32(encoded.data) != kRiff || byte_load_le32(encoded.data + 8) != kWebp ||
        byte_load_le32(encoded.data + 4) != encoded.count - 8 || (encoded.count & 1u) != 0) {
        return parse_failure(result, WebpError::riff);
    }
    bool vp8x_seen = false;
    bool image_seen = false;
    bool alph_seen = false;
    bool iccp_seen = false;
    bool exif_seen = false;
    bool xmp_seen = false;
    u8 feature_flags = 0;
    u32 canvas_width = 0;
    u32 canvas_height = 0;
    u32 cursor = 12;
    while (cursor < encoded.count) {
        if (encoded.count - cursor < 8) {
            return parse_failure(result, WebpError::truncated);
        }
        const u32 type = byte_load_le32(encoded.data + cursor);
        const u32 size = byte_load_le32(encoded.data + cursor + 4);
        const u64 payload_end = static_cast<u64>(cursor) + 8u + size;
        const u64 chunk_end = payload_end + (size & 1u);
        if (payload_end > encoded.count || chunk_end > encoded.count ||
            (size & 1u) != 0 && encoded[static_cast<u32>(payload_end)] != 0) {
            return parse_failure(result, WebpError::chunk);
        }
        const u8* payload = encoded.data + cursor + 8;
        ++result.chunks;
        if (type == kVp8x) {
            if (vp8x_seen || image_seen || size != 10) {
                return parse_failure(result, vp8x_seen ? WebpError::duplicate_chunk : WebpError::chunk);
            }
            if ((payload[0] & 0xC1u) != 0 || payload[1] != 0 || payload[2] != 0 || payload[3] != 0) {
                return parse_failure(result, WebpError::unsupported_feature);
            }
            if ((payload[0] & 0x02u) != 0) {
                return parse_failure(result, WebpError::unsupported_animation);
            }
            canvas_width = byte_load_le24(payload + 4) + 1;
            canvas_height = byte_load_le24(payload + 7) + 1;
            feature_flags = payload[0];
            image->has_alpha = (payload[0] & 0x10u) != 0;
            image->has_color_profile = (payload[0] & 0x20u) != 0;
            vp8x_seen = true;
        } else if (type == kVp8l) {
            if (image_seen || size < 5 || payload[0] != 0x2Fu) {
                return parse_failure(result, image_seen ? WebpError::duplicate_chunk : WebpError::chunk);
            }
            const u32 header = byte_load_le32(payload + 1);
            const u32 width = (header & 0x3FFFu) + 1;
            const u32 height = (header >> 14 & 0x3FFFu) + 1;
            if ((header >> 29) != 0 || width > kWebpLosslessDimensionMaximum ||
                height > kWebpLosslessDimensionMaximum) {
                return parse_failure(result,
                                     width > kWebpLosslessDimensionMaximum || height > kWebpLosslessDimensionMaximum
                                         ? WebpError::dimensions
                                         : WebpError::bitstream);
            }
            if (vp8x_seen && (canvas_width != width || canvas_height != height)) {
                return parse_failure(result, WebpError::dimensions);
            }
            const bool bitstream_alpha = (header & (1u << 28)) != 0;
            if (alph_seen || (vp8x_seen && image->has_alpha != bitstream_alpha)) {
                return parse_failure(result, WebpError::chunk);
            }
            image->bitstream = ConstSlice<u8>{payload + 5, size - 5};
            image->width = width;
            image->height = height;
            image->has_alpha = bitstream_alpha;
            image->codec = WebpCodec::vp8l_lossless;
            image_seen = true;
        } else if (type == kAlph) {
            if (!vp8x_seen || image_seen || alph_seen || size < 1 || !image->has_alpha) {
                return parse_failure(result, alph_seen ? WebpError::duplicate_chunk : WebpError::chunk);
            }
            image->alpha_bitstream = ConstSlice<u8>{payload, size};
            alph_seen = true;
        } else if (type == kVp8) {
            if (image_seen || size < 10) {
                return parse_failure(result, image_seen ? WebpError::duplicate_chunk : WebpError::chunk);
            }
            const u32 tag = byte_load_le24(payload);
            const u32 first_partition_bytes = tag >> 5;
            if ((tag & 1u) != 0 || ((tag >> 1) & 7u) > 3u || (tag & (1u << 4)) == 0 ||
                payload[3] != 0x9Du || payload[4] != 0x01u || payload[5] != 0x2Au ||
                first_partition_bytes == 0 || static_cast<u64>(10u) + first_partition_bytes > size) {
                return parse_failure(result, WebpError::bitstream);
            }
            const u32 width = (static_cast<u32>(payload[6]) | static_cast<u32>(payload[7]) << 8) & 0x3FFFu;
            const u32 height = (static_cast<u32>(payload[8]) | static_cast<u32>(payload[9]) << 8) & 0x3FFFu;
            if (width == 0 || height == 0 || width > kWebpDimensionMaximum ||
                height > kWebpDimensionMaximum || static_cast<u64>(width) * height > kWebpPixelMaximum) {
                return parse_failure(result, WebpError::dimensions);
            }
            if ((vp8x_seen && (canvas_width != width || canvas_height != height)) ||
                (!vp8x_seen && alph_seen) || (vp8x_seen && image->has_alpha != alph_seen)) {
                return parse_failure(result, WebpError::dimensions);
            }
            image->bitstream = ConstSlice<u8>{payload, size};
            image->width = width;
            image->height = height;
            image->codec = WebpCodec::vp8_lossy;
            image_seen = true;
        } else if (type == kAnim || type == kAnmf) {
            return parse_failure(result, WebpError::unsupported_animation);
        } else if (type == kIccp) {
            if (!vp8x_seen || image_seen || alph_seen || iccp_seen || (feature_flags & 0x20u) == 0) {
                return parse_failure(result, iccp_seen ? WebpError::duplicate_chunk : WebpError::chunk);
            }
            iccp_seen = true;
            image->has_color_profile = true;
        } else if (type == kExif) {
            if (!vp8x_seen || !image_seen || exif_seen || xmp_seen || (feature_flags & 0x08u) == 0) {
                return parse_failure(result, exif_seen ? WebpError::duplicate_chunk : WebpError::chunk);
            }
            exif_seen = true;
        } else if (type == kXmp) {
            if (!vp8x_seen || !image_seen || xmp_seen || (feature_flags & 0x04u) == 0) {
                return parse_failure(result, xmp_seen ? WebpError::duplicate_chunk : WebpError::chunk);
            }
            xmp_seen = true;
        } else {
            return parse_failure(result, WebpError::unsupported_feature);
        }
        cursor = static_cast<u32>(chunk_end);
    }
    if (!image_seen || cursor != encoded.count ||
        (vp8x_seen &&
         (((feature_flags & 0x20u) != 0) != iccp_seen || ((feature_flags & 0x08u) != 0) != exif_seen ||
          ((feature_flags & 0x04u) != 0) != xmp_seen))) {
        return parse_failure(result, WebpError::chunk);
    }
    image->encoded_bytes = encoded.count;
    result.error = WebpError::none;
    return result;
}

namespace {

enum class TransformType : u8 {
    predictor,
    color,
    subtract_green,
    color_indexing,
};

struct Transform {
    u32 width;
    u32 height;
    u16 palette_count;
    u8 size_bits;
    u8 width_bits;
    TransformType type;
};

struct DecodeState {
    BitReader reader;
    Vp8lWorkspace* workspace;
    WebpError error;
};

void set_error(DecodeState* state, WebpError error) noexcept {
    if (state->error == WebpError::none) {
        state->error = error;
    }
}

__forceinline u32 window_prefix_value(BitReader* reader, BitWindow* window, u32 prefix) noexcept {
    if (prefix < 4) {
        return prefix + 1;
    }
    const u32 extra_bits = (prefix - 2) >> 1;
    const u32 offset = (2 + (prefix & 1u)) << extra_bits;
    return offset + window_read_bits(reader, window, extra_bits) + 1;
}

inline constexpr signed char kDistanceOffsets[120][2] = {
    {0, 1},  {1, 0},  {1, 1},  {-1, 1}, {0, 2},  {2, 0},  {1, 2},  {-1, 2}, {2, 1},  {-2, 1}, {2, 2},
    {-2, 2}, {0, 3},  {3, 0},  {1, 3},  {-1, 3}, {3, 1},  {-3, 1}, {2, 3},  {-2, 3}, {3, 2},  {-3, 2},
    {0, 4},  {4, 0},  {1, 4},  {-1, 4}, {4, 1},  {-4, 1}, {3, 3},  {-3, 3}, {2, 4},  {-2, 4}, {4, 2},
    {-4, 2}, {0, 5},  {3, 4},  {-3, 4}, {4, 3},  {-4, 3}, {5, 0},  {1, 5},  {-1, 5}, {5, 1},  {-5, 1},
    {2, 5},  {-2, 5}, {5, 2},  {-5, 2}, {4, 4},  {-4, 4}, {3, 5},  {-3, 5}, {5, 3},  {-5, 3}, {0, 6},
    {6, 0},  {1, 6},  {-1, 6}, {6, 1},  {-6, 1}, {2, 6},  {-2, 6}, {6, 2},  {-6, 2}, {4, 5},  {-4, 5},
    {5, 4},  {-5, 4}, {3, 6},  {-3, 6}, {6, 3},  {-6, 3}, {0, 7},  {7, 0},  {1, 7},  {-1, 7}, {5, 5},
    {-5, 5}, {7, 1},  {-7, 1}, {4, 6},  {-4, 6}, {6, 4},  {-6, 4}, {2, 7},  {-2, 7}, {7, 2},  {-7, 2},
    {3, 7},  {-3, 7}, {7, 3},  {-7, 3}, {5, 6},  {-5, 6}, {6, 5},  {-6, 5}, {8, 0},  {4, 7},  {-4, 7},
    {7, 4},  {-7, 4}, {8, 1},  {8, 2},  {6, 6},  {-6, 6}, {8, 3},  {5, 7},  {-5, 7}, {7, 5},  {-7, 5},
    {8, 4},  {6, 7},  {-6, 7}, {7, 6},  {-7, 6}, {8, 5},  {7, 7},  {-7, 7}, {8, 6},  {8, 7},
};

__forceinline u32 distance_value(u32 code, u32 width) noexcept {
    if (code == 0) {
        return 0;
    }
    if (code > 120) {
        return code - 120;
    }
    const i32 value = static_cast<i32>(kDistanceOffsets[code - 1][0]) +
                      static_cast<i32>(kDistanceOffsets[code - 1][1]) * static_cast<i32>(width);
    return value < 1 ? 1u : static_cast<u32>(value);
}

// A backward copy within the output, distance >= 1. From distance 4 on, four
// pixels per step read only pixels already written; distances 1 and 2 repeat
// a pattern (the copy's own output).
__forceinline void copy_backward(u32* target, u32 distance, u32 length) noexcept {
    const u32* source = target - distance;
    u32 index = 0;
    if (distance >= 4) {
        for (; index + 4 <= length; index += 4) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(target + index),
                             _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + index)));
        }
    } else if (distance != 3) {
        const __m128i pattern =
            distance == 1 ? _mm_set1_epi32(static_cast<int>(source[0]))
                          : _mm_shuffle_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(source)),
                                              _MM_SHUFFLE(1, 0, 1, 0));
        for (; index + 4 <= length; index += 4) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(target + index), pattern);
        }
    }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (; index < length; ++index) {
        target[index] = source[index];
    }
}

bool decode_entropy_image(DecodeState* state, u32 width, u32 height, bool allow_meta, u32* output,
                          u32 output_capacity) noexcept {
    if (width == 0 || height == 0 || static_cast<u64>(width) * height > output_capacity) {
        set_error(state, WebpError::resource_limit);
        return false;
    }
    Vp8lWorkspace* workspace = state->workspace;
    u32 cache_bits = 0;
    if (read_bits(&state->reader, 1) != 0) {
        cache_bits = read_bits(&state->reader, 4);
        if (cache_bits < 1 || cache_bits > 11) {
            set_error(state, WebpError::bitstream);
            return false;
        }
    }
    const u32 cache_size = cache_bits == 0 ? 0u : 1u << cache_bits;
    for (u32 index = 0; index < cache_size; ++index) {
        workspace->color_cache[index] = 0;
    }

    u32 prefix_bits = 0;
    u32 prefix_width = 0;
    u32 prefix_height = 0;
    u32 group_count = 1;
    if (allow_meta && read_bits(&state->reader, 1) != 0) {
        prefix_bits = read_bits(&state->reader, 3) + 2;
        prefix_width = (width + (1u << prefix_bits) - 1) >> prefix_bits;
        prefix_height = (height + (1u << prefix_bits) - 1) >> prefix_bits;
        const u32 prefix_pixels = prefix_width * prefix_height;
        if (prefix_pixels > kVp8lTransformPixelCapacity ||
            !decode_entropy_image(state, prefix_width, prefix_height, false, workspace->subimage_pixels,
                                  kVp8lTransformPixelCapacity)) {
            if (state->error == WebpError::none) {
                set_error(state, WebpError::resource_limit);
            }
            return false;
        }
        u32 maximum_group = 0;
        for (u32 index = 0; index < prefix_pixels; ++index) {
            const u32 group = workspace->subimage_pixels[index] >> 8 & 0xFFFFu;
            if (group >= kVp8lHuffmanGroupCapacity) {
                set_error(state, WebpError::resource_limit);
                return false;
            }
            maximum_group = maximum(maximum_group, group);
        }
        group_count = maximum_group + 1;
    }
    if (state->reader.failed) {
        set_error(state, WebpError::bitstream);
        return false;
    }

    workspace->symbol_count = 0;
    workspace->lookup_count = 0;
    const u32 alphabets[5] = {
        256u + 24u + cache_size, 256u, 256u, 256u, 40u,
    };
    for (u32 group = 0; group < group_count; ++group) {
        for (u32 tree = 0; tree < 5; ++tree) {
            if (!read_huffman_code(&state->reader, alphabets[tree], workspace,
                                   &workspace->groups[group].codes[tree])) {
                set_error(state, workspace->symbol_count >= kVp8lHuffmanSymbolCapacity
                                     ? WebpError::resource_limit
                                     : WebpError::huffman);
                return false;
            }
        }
        Vp8lHuffmanGroup& entry = workspace->groups[group];
        entry.trivial_literal =
            entry.codes[1].symbol_count == 1 && entry.codes[2].symbol_count == 1 && entry.codes[3].symbol_count == 1;
        entry.literal_arb = static_cast<u32>(entry.codes[3].single_symbol) << 24 |
                            static_cast<u32>(entry.codes[1].single_symbol) << 16 | entry.codes[2].single_symbol;
    }

    const u32 pixel_count = width * height;
    // x/y track position; the group changes only at prefix-block columns,
    // on a new row and after a backward copy.
    const u32 block_mask = prefix_bits != 0 ? (1u << prefix_bits) - 1u : ~0u;
    u32* const color_cache = workspace->color_cache;
    const u32 cache_shift = 32u - cache_bits;
    // The color cache takes pixels in output order, lazily: output[0, cached)
    // are in it, the rest go in before a cache symbol reads it (libwebp's
    // src/dec/vp8l_dec.c does the same), so literals and copies skip the hash.
    u32 cached = 0;
    u32 position = 0;
    u32 x = 0;
    u32 y = 0;
    const Vp8lHuffmanGroup* codes = &workspace->groups[0];
    BitWindow window{};
    window_fill(state->reader, &window);
    while (position < pixel_count) {
        if ((x & block_mask) == 0 && prefix_bits != 0) {
            codes = &workspace->groups[workspace->subimage_pixels[(y >> prefix_bits) * prefix_width + (x >> prefix_bits)] >> 8 & 0xFFFFu];
        }
        u16 green = 0;
        if (!window_decode(&state->reader, &window, workspace, codes->codes[0], &green)) {
            set_error(state, WebpError::huffman);
            return false;
        }
        if (green < 256) {
            u32 pixel = codes->literal_arb | static_cast<u32>(green) << 8;
            if (!codes->trivial_literal) {
                u16 red = 0;
                u16 blue = 0;
                u16 alpha = 0;
                if (!window_decode(&state->reader, &window, workspace, codes->codes[1], &red) ||
                    !window_decode(&state->reader, &window, workspace, codes->codes[2], &blue) ||
                    !window_decode(&state->reader, &window, workspace, codes->codes[3], &alpha)) {
                    set_error(state, WebpError::huffman);
                    return false;
                }
                pixel = static_cast<u32>(alpha) << 24 | static_cast<u32>(red) << 16 | static_cast<u32>(green) << 8 |
                        blue;
            }
            output[position++] = pixel;
            if (++x == width) {
                x = 0;
                ++y;
            }
        } else if (green < 280) {
            const u32 length = window_prefix_value(&state->reader, &window, green - 256);
            u16 distance_prefix = 0;
            if (!window_decode(&state->reader, &window, workspace, codes->codes[4], &distance_prefix)) {
                set_error(state, WebpError::huffman);
                return false;
            }
            const u32 distance_code = window_prefix_value(&state->reader, &window, distance_prefix);
            const u32 distance = distance_value(distance_code, width);
            if (state->reader.failed || length > pixel_count - position || distance > position) {
                set_error(state, WebpError::distance);
                return false;
            }
            copy_backward(output + position, distance, length);
            position += length;
            x += length;
            if (x >= width) {
                y += x / width;
                x %= width;
            }
            if (prefix_bits != 0 && position < pixel_count) {
                codes = &workspace->groups[workspace->subimage_pixels[(y >> prefix_bits) * prefix_width + (x >> prefix_bits)] >> 8 & 0xFFFFu];
            }
        } else {
            const u32 cache_index = green - 280;
            if (cache_index >= cache_size) {
                set_error(state, WebpError::bitstream);
                return false;
            }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
            for (; cached < position; ++cached) {
                const u32 pixel = output[cached];
                color_cache[static_cast<u32>(0x1E35A7BDull * pixel) >> cache_shift] = pixel;
            }
            output[position++] = color_cache[cache_index];
            if (++x == width) {
                x = 0;
                ++y;
            }
        }
    }
    if (state->reader.failed) {
        set_error(state, WebpError::truncated);
        return false;
    }
    return true;
}

__forceinline u8 channel(u32 pixel, u32 shift) noexcept {
    return static_cast<u8>(pixel >> shift);
}

// Per-channel sums and floor averages without carries between channels. The
// sum adds the low seven bits of each byte and sets bit 7 by xor; its masked
// alpha/green plus red/blue form was turned by clang's SLP vectorizer into a
// two-lane vector sum, slow on the serial left-pixel chain.
__forceinline u32 add_pixels(u32 first, u32 second) noexcept {
    return ((first & 0x7F7F7F7Fu) + (second & 0x7F7F7F7Fu)) ^ ((first ^ second) & 0x80808080u);
}

__forceinline u32 average_pixels(u32 first, u32 second) noexcept {
    return (((first ^ second) & 0xFEFEFEFEu) >> 1) + (first & second);
}

__forceinline u32 clamp_add_half(u32 average, u32 top_left) noexcept {
    // Per channel clamp(a + (a - b) / 2), the division truncating toward zero.
    const __m128i zero = _mm_setzero_si128();
    const __m128i a = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(average)), zero);
    const __m128i b = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(top_left)), zero);
    const __m128i difference = _mm_sub_epi16(a, b);
    const __m128i half = _mm_srai_epi16(_mm_add_epi16(difference, _mm_srli_epi16(difference, 15)), 1);
    const __m128i sum = _mm_add_epi16(a, half);
    return static_cast<u32>(_mm_cvtsi128_si32(_mm_packus_epi16(sum, sum)));
}

__forceinline __m128i load_pixels(const u32* pixels) noexcept {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels));
}

// average_pixels on four pixels: pavgb rounds up, so drop the carried half.
__forceinline __m128i average_pixels4(__m128i first, __m128i second) noexcept {
    return _mm_sub_epi8(_mm_avg_epu8(first, second), _mm_and_si128(_mm_xor_si128(first, second), _mm_set1_epi8(1)));
}

// Mode 1 (left) over [begin, end): a running per-channel sum, four pixels per
// step as an in-register prefix sum (libwebp's PredictorAdd1_SSE2 technique).
void predict_left_run(u32* pixels, u32 begin, u32 end) noexcept {
    u32 p = begin;
    __m128i left = _mm_set1_epi32(static_cast<int>(pixels[begin - 1]));
    for (; p + 4 <= end; p += 4) {
        __m128i sum = load_pixels(pixels + p);
        sum = _mm_add_epi8(sum, _mm_slli_si128(sum, 4));
        sum = _mm_add_epi8(sum, _mm_slli_si128(sum, 8));
        sum = _mm_add_epi8(sum, left);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(pixels + p), sum);
        left = _mm_shuffle_epi32(sum, _MM_SHUFFLE(3, 3, 3, 3));
    }
    u32 last = static_cast<u32>(_mm_cvtsi128_si32(left));
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (; p < end; ++p) {
        last = add_pixels(pixels[p], last);
        pixels[p] = last;
    }
}

// One run of pixels sharing a predictor mode; top_right of the last pixel in a
// row is the first pixel of the current row, which is pixels[p - width + 1].
// The left pixel stays in a register from one pixel to the next.
template <u32 Mode>
void predict_run(u32* pixels, u32 begin, u32 end, u32 width) noexcept {
    u32 p = begin;
    if constexpr (Mode == 0 || Mode == 2 || Mode == 3 || Mode == 4 || Mode == 8 || Mode == 9) {
        // These read the row above and, for a row's last pixel, the row's first
        // pixel (already final), never a pixel of their own step: four at a time.
        for (; p + 4 <= end; p += 4) {
            const u32* above = pixels + p - width;
            __m128i predicted;
            if constexpr (Mode == 0) {
                predicted = _mm_set1_epi32(static_cast<int>(0xFF000000u));
            } else if constexpr (Mode == 2) {
                predicted = load_pixels(above);
            } else if constexpr (Mode == 3) {
                predicted = load_pixels(above + 1);
            } else if constexpr (Mode == 4) {
                predicted = load_pixels(above - 1);
            } else if constexpr (Mode == 8) {
                predicted = average_pixels4(load_pixels(above - 1), load_pixels(above));
            } else {
                predicted = average_pixels4(load_pixels(above), load_pixels(above + 1));
            }
            _mm_storeu_si128(reinterpret_cast<__m128i*>(pixels + p),
                             _mm_add_epi8(load_pixels(pixels + p), predicted));
        }
    }
    if constexpr (Mode == 11 || Mode == 12) {
        // The left pixel stays in a vector register: its serial chain makes no
        // round trip through a general register. Bytes past the pixel stay zero.
        __m128i left = _mm_cvtsi32_si128(static_cast<int>(pixels[p - 1]));
        const __m128i zero = _mm_setzero_si128();
        for (; p < end; ++p) {
            const __m128i top = _mm_cvtsi32_si128(static_cast<int>(pixels[p - width]));
            const __m128i top_left = _mm_cvtsi32_si128(static_cast<int>(pixels[p - width - 1]));
            __m128i predicted;
            if constexpr (Mode == 11) {
                // Select: the channel sums |estimate - l| = |t - tl| and |estimate - t| =
                // |l - tl| as sums of absolute byte differences; left where the first is smaller.
                const __m128i left_wins =
                    _mm_cmpgt_epi16(_mm_sad_epu8(left, top_left), _mm_sad_epu8(top, top_left));
                predicted = _mm_blendv_epi8(top, left, _mm_shufflelo_epi16(left_wins, 0));
            } else {
                const __m128i sum = _mm_sub_epi16(
                    _mm_add_epi16(_mm_unpacklo_epi8(left, zero), _mm_unpacklo_epi8(top, zero)),
                    _mm_unpacklo_epi8(top_left, zero));
                predicted = _mm_packus_epi16(sum, zero);
            }
            left = _mm_add_epi8(_mm_cvtsi32_si128(static_cast<int>(pixels[p])), predicted);
            pixels[p] = static_cast<u32>(_mm_cvtsi128_si32(left));
        }
    } else {
        u32 left = pixels[p - 1];
        for (; p < end; ++p) {
            const u32 top = pixels[p - width];
            u32 predicted = 0;
            if constexpr (Mode == 0) {
                predicted = 0xFF000000u;
            } else if constexpr (Mode == 2) {
                predicted = top;
            } else if constexpr (Mode == 3) {
                predicted = pixels[p - width + 1];
            } else if constexpr (Mode == 4) {
                predicted = pixels[p - width - 1];
            } else if constexpr (Mode == 5) {
                predicted = average_pixels(average_pixels(left, pixels[p - width + 1]), top);
            } else if constexpr (Mode == 6) {
                predicted = average_pixels(left, pixels[p - width - 1]);
            } else if constexpr (Mode == 7) {
                predicted = average_pixels(left, top);
            } else if constexpr (Mode == 8) {
                predicted = average_pixels(pixels[p - width - 1], top);
            } else if constexpr (Mode == 9) {
                predicted = average_pixels(top, pixels[p - width + 1]);
            } else if constexpr (Mode == 10) {
                predicted = average_pixels(average_pixels(left, pixels[p - width - 1]),
                                           average_pixels(top, pixels[p - width + 1]));
            } else {
                static_assert(Mode == 13, "mode 1 runs in predict_left_run, 11 and 12 above");
                predicted = clamp_add_half(average_pixels(left, top), pixels[p - width - 1]);
            }
            left = add_pixels(pixels[p], predicted);
            pixels[p] = left;
        }
    }
}

void predict_mode_run(u8 mode, u32* pixels, u32 begin, u32 end, u32 width) noexcept {
    switch (mode) {
    case 0: predict_run<0>(pixels, begin, end, width); break;
    case 1: predict_left_run(pixels, begin, end); break;
    case 2: predict_run<2>(pixels, begin, end, width); break;
    case 3: predict_run<3>(pixels, begin, end, width); break;
    case 4: predict_run<4>(pixels, begin, end, width); break;
    case 5: predict_run<5>(pixels, begin, end, width); break;
    case 6: predict_run<6>(pixels, begin, end, width); break;
    case 7: predict_run<7>(pixels, begin, end, width); break;
    case 8: predict_run<8>(pixels, begin, end, width); break;
    case 9: predict_run<9>(pixels, begin, end, width); break;
    case 10: predict_run<10>(pixels, begin, end, width); break;
    case 11: predict_run<11>(pixels, begin, end, width); break;
    case 12: predict_run<12>(pixels, begin, end, width); break;
    default: predict_run<13>(pixels, begin, end, width); break;
    }
}

void inverse_predictor(const Transform& transform, const Vp8lWorkspace* workspace, u32* pixels) noexcept {
    const u32 width = transform.width;
    const u32 block = 1u << transform.size_bits;
    const u32 blocks_wide = (width + block - 1) >> transform.size_bits;
    // read_transforms admitted only modes 0..13 and a map within capacity.
    pixels[0] = add_pixels(pixels[0], 0xFF000000u);
    predict_left_run(pixels, 1, width);
    for (u32 y = 1; y < transform.height; ++y) {
        const u32 row = y * width;
        pixels[row] = add_pixels(pixels[row], pixels[row - width]);
        const u8* modes = workspace->predictor_modes + (y >> transform.size_bits) * blocks_wide;
        for (u32 x = 1, column = 0; x < width; ++column) {
            const u32 end = minimum((column + 1u) << transform.size_bits, width);
            predict_mode_run(modes[column], pixels, row + x, row + end, width);
            x = end;
        }
    }
}

__forceinline i32 color_delta(signed char transform, signed char color) noexcept {
    return static_cast<i32>(transform) * color >> 5;
}

void inverse_color(const Transform& transform, const Vp8lWorkspace* workspace, u32* pixels) noexcept {
    const u32 width = transform.width;
    const u32 blocks_wide = (width + (1u << transform.size_bits) - 1) >> transform.size_bits;
    for (u32 y = 0; y < transform.height; ++y) {
        const u32* elements = workspace->color_elements + (y >> transform.size_bits) * blocks_wide;
        u32* row = pixels + y * width;
        for (u32 x = 0, column = 0; x < width; ++column) {
            const u32 element = elements[column];
            const signed char green_to_red = static_cast<signed char>(element);
            const signed char green_to_blue = static_cast<signed char>(element >> 8);
            const signed char red_to_blue = static_cast<signed char>(element >> 16);
            const u32 end = minimum((column + 1u) << transform.size_bits, width);
            // Four pixels per step in 16-bit lanes (libwebp's
            // TransformColorInverse_SSE2 technique): green, or the new red,
            // in a lane's high byte times the coefficient * 8, high half:
            // (color * coefficient) >> 5 with the same sign and floor; only
            // the low byte is added.
            const __m128i green_factors =
                _mm_set1_epi32(static_cast<i32>(static_cast<u32>(green_to_red * 8) << 16 |
                                                static_cast<u16>(green_to_blue * 8)));
            const __m128i red_factor = _mm_set1_epi32(static_cast<u16>(red_to_blue * 8));
            const __m128i green_high = _mm_setr_epi8(-1, 1, -1, 1, -1, 5, -1, 5, -1, 9, -1, 9, -1, 13, -1, 13);
            const __m128i red_high = _mm_setr_epi8(-1, 2, -1, -1, -1, 6, -1, -1, -1, 10, -1, -1, -1, 14, -1, -1);
            const __m128i red_blue_bytes = _mm_set1_epi32(0x00FF00FF);
            const __m128i blue_byte = _mm_set1_epi32(0xFF);
            for (; x + 4 <= end; x += 4) {
                __m128i* slot = reinterpret_cast<__m128i*>(row + x);
                const __m128i pixel = _mm_loadu_si128(slot);
                const __m128i deltas = _mm_mulhi_epi16(_mm_shuffle_epi8(pixel, green_high), green_factors);
                const __m128i red_done = _mm_add_epi8(pixel, _mm_and_si128(deltas, red_blue_bytes));
                const __m128i red_delta = _mm_mulhi_epi16(_mm_shuffle_epi8(red_done, red_high), red_factor);
                _mm_storeu_si128(slot, _mm_add_epi8(red_done, _mm_and_si128(red_delta, blue_byte)));
            }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
            for (; x < end; ++x) {
                const u32 pixel = row[x];
                const signed char green = static_cast<signed char>(pixel >> 8);
                const u8 red = static_cast<u8>(channel(pixel, 16) + color_delta(green_to_red, green));
                const u8 blue = static_cast<u8>(channel(pixel, 0) + color_delta(green_to_blue, green) +
                                                color_delta(red_to_blue, static_cast<signed char>(red)));
                row[x] = (pixel & 0xFF00FF00u) | static_cast<u32>(red) << 16 | blue;
            }
        }
    }
}

void inverse_subtract_green(const Transform& transform, u32* pixels) noexcept {
    const u32 count = transform.width * transform.height;
    // Green (byte 1 of each pixel) added to red (byte 2) and blue (byte 0).
    const __m128i spread = _mm_setr_epi8(1, -1, 1, -1, 5, -1, 5, -1, 9, -1, 9, -1, 13, -1, 13, -1);
    u32 index = 0;
    for (; index + 4 <= count; index += 4) {
        __m128i* slot = reinterpret_cast<__m128i*>(pixels + index);
        const __m128i value = _mm_loadu_si128(slot);
        _mm_storeu_si128(slot, _mm_add_epi8(value, _mm_shuffle_epi8(value, spread)));
    }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (; index < count; ++index) {
        const u32 pixel = pixels[index];
        const u32 green = pixel >> 8 & 0xFFu;
        pixels[index] = add_pixels(pixel, green << 16 | green);
    }
}

// Expands one row's packed indices in place, right to left: a packed pixel's
// outputs lie at or after it, so no output overwrites a packed pixel still to
// read. Indices past palette_count decode as transparent black: `palette` has
// 256 entries, zero past the palette.
template <u32 Bits>
__forceinline void expand_indexed_row(const u32* packed, u32* row, u32 width, const u32* palette) noexcept {
    constexpr u32 kPerPixel = 1u << Bits;
    constexpr u32 kSampleBits = 8u >> Bits;
    constexpr u32 kMask = (1u << kSampleBits) - 1u;
    u32 full = width >> Bits;
    const u32 tail = width & (kPerPixel - 1u);
    if (tail != 0) {
        u32 indices = packed[full] >> 8;
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
        for (u32 index = 0; index < tail; ++index, indices >>= kSampleBits) {
            row[full * kPerPixel + index] = palette[indices & kMask];
        }
    }
    if constexpr (Bits == 3) {
        // Eight 1-bit indices: each lane takes the second color where its bit is set.
        const __m128i first = _mm_set1_epi32(static_cast<int>(palette[0]));
        const __m128i second = _mm_set1_epi32(static_cast<int>(palette[1]));
        const __m128i low_bits = _mm_setr_epi8(1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 8, 8, 8, 8);
        const __m128i high_bits =
            _mm_setr_epi8(16, 16, 16, 16, 32, 32, 32, 32, 64, 64, 64, 64, -128, -128, -128, -128);
        while (full != 0) {
            --full;
            const __m128i indices = _mm_set1_epi8(static_cast<char>(packed[full] >> 8));
            const __m128i low = _mm_cmpeq_epi8(_mm_and_si128(indices, low_bits), low_bits);
            const __m128i high = _mm_cmpeq_epi8(_mm_and_si128(indices, high_bits), high_bits);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(row + full * 8u), _mm_blendv_epi8(first, second, low));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(row + full * 8u + 4u), _mm_blendv_epi8(first, second, high));
        }
    } else {
        while (full != 0) {
            --full;
            u32 indices = packed[full] >> 8;
            u32* out = row + full * kPerPixel;
            for (u32 index = 0; index < kPerPixel; ++index, indices >>= kSampleBits) {
                out[index] = palette[indices & kMask];
            }
        }
    }
}

void inverse_color_indexing(const Transform& transform, const u32* palette, u32 current_width,
                            u32* pixels) noexcept {
    const u32 width = transform.width;
    if (transform.width_bits == 0) {
        const u32 count = width * transform.height;
#pragma clang loop vectorize(disable) interleave(disable)
        for (u32 index = 0; index < count; ++index) {
            pixels[index] = palette[pixels[index] >> 8 & 0xFFu];
        }
        return;
    }
    // Rows bottom up: a row's outputs lie past every packed row above it.
    for (u32 y = transform.height; y-- != 0;) {
        const u32* packed = pixels + y * current_width;
        u32* row = pixels + y * width;
        switch (transform.width_bits) {
        case 1: expand_indexed_row<1>(packed, row, width, palette); break;
        case 2: expand_indexed_row<2>(packed, row, width, palette); break;
        default: expand_indexed_row<3>(packed, row, width, palette); break;
        }
    }
}

bool read_transforms(DecodeState* state, u32 original_width, u32 height, Transform* transforms,
                     u32* transform_count, u32* decoded_width, u32* palette) noexcept {
    bool seen[4]{};
    u32 width = original_width;
    while (read_bits(&state->reader, 1) != 0) {
        const u32 type_value = read_bits(&state->reader, 2);
        if (seen[type_value]) {
            set_error(state, WebpError::transform);
            return false;
        }
        seen[type_value] = true;
        Transform transform{};
        transform.type = static_cast<TransformType>(type_value);
        transform.width = width;
        transform.height = height;
        if (transform.type == TransformType::predictor || transform.type == TransformType::color) {
            transform.size_bits = static_cast<u8>(read_bits(&state->reader, 3) + 2);
            const u32 map_width = (width + (1u << transform.size_bits) - 1) >> transform.size_bits;
            const u32 map_height = (height + (1u << transform.size_bits) - 1) >> transform.size_bits;
            const u32 map_pixels = map_width * map_height;
            if (map_pixels > kVp8lTransformPixelCapacity ||
                !decode_entropy_image(state, map_width, map_height, false,
                                      state->workspace->subimage_pixels, kVp8lTransformPixelCapacity)) {
                if (state->error == WebpError::none) {
                    set_error(state, WebpError::resource_limit);
                }
                return false;
            }
            if (transform.type == TransformType::predictor) {
                for (u32 index = 0; index < map_pixels; ++index) {
                    const u8 mode = channel(state->workspace->subimage_pixels[index], 8);
                    if (mode > 13) {
                        set_error(state, WebpError::transform);
                        return false;
                    }
                    state->workspace->predictor_modes[index] = mode;
                }
            } else {
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
                for (u32 index = 0; index < map_pixels; ++index) {
                    state->workspace->color_elements[index] = state->workspace->subimage_pixels[index];
                }
            }
        } else if (transform.type == TransformType::color_indexing) {
            transform.palette_count = static_cast<u16>(read_bits(&state->reader, 8) + 1);
            if (!decode_entropy_image(state, transform.palette_count, 1, false,
                                      state->workspace->subimage_pixels, kVp8lTransformPixelCapacity)) {
                return false;
            }
            u32 previous = 0;
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
            for (u32 index = 0; index < transform.palette_count; ++index) {
                previous = add_pixels(previous, state->workspace->subimage_pixels[index]);
                palette[index] = previous;
            }
            transform.width_bits = transform.palette_count <= 2    ? 3
                                   : transform.palette_count <= 4  ? 2
                                   : transform.palette_count <= 16 ? 1
                                                                   : 0;
            width = (width + (1u << transform.width_bits) - 1) >> transform.width_bits;
        }
        transforms[(*transform_count)++] = transform;
    }
    if (state->reader.failed) {
        set_error(state, WebpError::truncated);
        return false;
    }
    *decoded_width = width;
    return true;
}

bool remaining_padding_is_zero(BitReader* reader) noexcept {
    if (reader->failed) {
        return false;
    }
    const u64 total_bits = static_cast<u64>(reader->bytes.count) * 8u;
    const u64 remaining = total_bits - reader->bit_position;
    return remaining <= 7 && (remaining == 0 || read_bits(reader, static_cast<u32>(remaining)) == 0);
}

} // namespace

namespace {

WebpDecodeResult decode_validated_lossless_pixels(WebpImage* image, WebpParseResult parse,
                                                  Vp8lWorkspace* workspace, Slice<u8> rgba_output,
                                                  bool rgba_order) noexcept {
    WebpDecodeResult result{};
    result.parse = parse;
    if (!workspace || !rgba_output.data || image->codec != WebpCodec::vp8l_lossless ||
        !image->bitstream.data || image->bitstream.count == 0) {
        result.error = !workspace || !rgba_output.data ? WebpError::invalid_input
                       : image->codec == WebpCodec::vp8_lossy ? WebpError::unsupported_lossy
                                                              : WebpError::bitstream;
        return result;
    }
    const u64 required = static_cast<u64>(image->width) * image->height * 4u;
    if (required > rgba_output.count || (reinterpret_cast<usize>(rgba_output.data) & 3u) != 0) {
        result.error = WebpError::output_capacity;
        return result;
    }
    DecodeState state{BitReader{image->bitstream, 0, false}, workspace, WebpError::none};
    Transform transforms[4]{};
    u32 transform_count = 0;
    u32 decoded_width = image->width;
    u32 palette[256]{}; // zero past the palette: out-of-range indices decode as transparent black
    if (!read_transforms(&state, image->width, image->height, transforms, &transform_count, &decoded_width,
                         palette)) {
        result.error = state.error;
        return result;
    }
    auto* pixels = reinterpret_cast<u32*>(rgba_output.data);
    if (!decode_entropy_image(&state, decoded_width, image->height, true, pixels, kWebpLosslessPixelMaximum)) {
        result.error = state.error;
        return result;
    }
    // A subtract-green read first is undone last: in the RGBA pass, sparing a
    // pass over the image.
    const bool green_in_order_pass =
        rgba_order && transform_count != 0 && transforms[0].type == TransformType::subtract_green;
    u32 current_width = decoded_width;
    for (u32 reverse = transform_count; reverse != (green_in_order_pass ? 1u : 0u); --reverse) {
        const Transform& transform = transforms[reverse - 1];
        switch (transform.type) {
        case TransformType::predictor:
            inverse_predictor(transform, workspace, pixels);
            break;
        case TransformType::color:
            inverse_color(transform, workspace, pixels);
            break;
        case TransformType::subtract_green:
            inverse_subtract_green(transform, pixels);
            break;
        case TransformType::color_indexing:
            inverse_color_indexing(transform, palette, current_width, pixels);
            current_width = transform.width;
            break;
        }
    }
    if (current_width != image->width || !remaining_padding_is_zero(&state.reader)) {
        result.error = WebpError::bitstream;
        return result;
    }
    const u32 pixel_count = image->width * image->height;
    if (rgba_order) {
        // Packed 0xAARRGGBB to R, G, B, A bytes.
        const __m128i order = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
        // Green (byte 1 of each pixel) onto red (byte 2) and blue (byte 0), or nothing.
        const __m128i spread = green_in_order_pass
                                   ? _mm_setr_epi8(1, -1, 1, -1, 5, -1, 5, -1, 9, -1, 9, -1, 13, -1, 13, -1)
                                   : _mm_set1_epi8(-1);
        u32 index = 0;
        for (; index + 4 <= pixel_count; index += 4) {
            __m128i* slot = reinterpret_cast<__m128i*>(pixels + index);
            const __m128i value = _mm_loadu_si128(slot);
            _mm_storeu_si128(slot, _mm_shuffle_epi8(_mm_add_epi8(value, _mm_shuffle_epi8(value, spread)), order));
        }
        for (; index < pixel_count; ++index) {
            u32 pixel = pixels[index];
            if (green_in_order_pass) {
                const u32 green = pixel >> 8 & 0xFFu;
                pixel = add_pixels(pixel, green << 16 | green);
            }
            rgba_output[index * 4] = channel(pixel, 16);
            rgba_output[index * 4 + 1] = channel(pixel, 8);
            rgba_output[index * 4 + 2] = channel(pixel, 0);
            rgba_output[index * 4 + 3] = channel(pixel, 24);
        }
    }
    result.rgba_bytes = pixel_count * 4;
    result.error = WebpError::none;
    return result;
}

} // namespace

WebpDecodeResult webp_lossless_decode_rgba8(ConstSlice<u8> encoded, WebpImage* image,
                                            Vp8lWorkspace* workspace, Slice<u8> rgba_output) noexcept {
    WebpDecodeResult result{};
    const WebpParseResult parse = webp_parse(encoded, image);
    if (!parse.ok()) {
        result.parse = parse;
        result.error = parse.error;
        return result;
    }
    return decode_validated_lossless_pixels(image, parse, workspace, rgba_output, true);
}

WebpDecodeResult webp_lossless_decode_vp8l_frame_rgba8(ConstSlice<u8> vp8l_payload, u32 expected_width,
                                                       u32 expected_height, WebpImage* image,
                                                       Vp8lWorkspace* workspace,
                                                       Slice<u8> rgba_output) noexcept {
    WebpDecodeResult result{};
    if (!image || (!vp8l_payload.data && vp8l_payload.count != 0) || expected_width == 0 ||
        expected_height == 0) {
        result.error = WebpError::invalid_input;
        return result;
    }
    reset_value<WebpImage>(image);
    if (vp8l_payload.count < 5) {
        result.parse.error = WebpError::truncated;
        result.error = WebpError::truncated;
        return result;
    }
    if (vp8l_payload[0] != 0x2Fu) {
        result.parse.error = WebpError::chunk;
        result.error = WebpError::chunk;
        return result;
    }
    const u32 header = byte_load_le32(vp8l_payload.data + 1);
    const u32 width = (header & 0x3FFFu) + 1;
    const u32 height = (header >> 14 & 0x3FFFu) + 1;
    if (width != expected_width || height != expected_height || width > kWebpDimensionMaximum ||
        height > kWebpDimensionMaximum) {
        result.parse.error = WebpError::dimensions;
        result.error = WebpError::dimensions;
        return result;
    }
    if ((header >> 29) != 0) {
        result.parse.error = WebpError::bitstream;
        result.error = WebpError::bitstream;
        return result;
    }
    image->bitstream = ConstSlice<u8>{vp8l_payload.data + 5, vp8l_payload.count - 5};
    image->width = width;
    image->height = height;
    image->encoded_bytes = vp8l_payload.count;
    image->codec = WebpCodec::vp8l_lossless;
    image->has_alpha = (header & (1u << 28)) != 0;
    result.parse.chunks = 1;
    result.parse.error = WebpError::none;
    return decode_validated_lossless_pixels(image, result.parse, workspace, rgba_output, true);
}

WebpDecodeResult webp_lossless_decode_image_stream_argb32(ConstSlice<u8> bitstream, u32 width, u32 height,
                                                          WebpImage* image, Vp8lWorkspace* workspace,
                                                          Slice<u8> pixel_output) noexcept {
    WebpDecodeResult result{};
    if (!image || (!bitstream.data && bitstream.count != 0) || width == 0 || height == 0) {
        result.error = WebpError::invalid_input;
        return result;
    }
    reset_value<WebpImage>(image);
    if (width > kWebpLosslessDimensionMaximum || height > kWebpLosslessDimensionMaximum ||
        static_cast<u64>(width) * height > kWebpLosslessPixelMaximum) {
        result.parse.error = WebpError::dimensions;
        result.error = WebpError::dimensions;
        return result;
    }
    if (!bitstream.data || bitstream.count == 0) {
        result.parse.error = WebpError::truncated;
        result.error = WebpError::truncated;
        return result;
    }
    image->bitstream = bitstream;
    image->width = width;
    image->height = height;
    image->encoded_bytes = bitstream.count;
    image->codec = WebpCodec::vp8l_lossless;
    result.parse.chunks = 1;
    result.parse.error = WebpError::none;
    return decode_validated_lossless_pixels(image, result.parse, workspace, pixel_output, false);
}

} // namespace maicord
