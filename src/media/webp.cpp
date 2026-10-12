#include "maicord/media/webp.hpp"

#include <immintrin.h>
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
    if (!reader || count > 24 || reader->bit_position + count > total_bits(*reader)) {
        if (reader) {
            reader->failed = true;
        }
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

struct SmallHuffmanCode {
    u16 counts[16];
    u16 symbols[19];
    u16 symbol_count;
    u16 single_symbol;
};

bool build_small_code(const u8* lengths, u32 alphabet_size, SmallHuffmanCode* code) noexcept {
    if (!lengths || !code || alphabet_size == 0 || alphabet_size > 19) {
        return false;
    }
    *code = SmallHuffmanCode{};
    for (u32 symbol = 0; symbol < alphabet_size; ++symbol) {
        const u8 length = lengths[symbol];
        if (length > 15) {
            return false;
        }
        if (length != 0) {
            ++code->counts[length];
            ++code->symbol_count;
            code->single_symbol = static_cast<u16>(symbol);
        }
    }
    if (code->symbol_count == 0) {
        return false;
    }
    if (code->symbol_count != 1) {
        i32 open = 1;
        for (u32 length = 1; length <= 15; ++length) {
            open = open * 2 - code->counts[length];
            if (open < 0) {
                return false;
            }
        }
        if (open != 0) {
            return false;
        }
    }
    u32 output = 0;
    for (u32 length = 1; length <= 15; ++length) {
        for (u32 symbol = 0; symbol < alphabet_size; ++symbol) {
            if (lengths[symbol] == length) {
                code->symbols[output++] = static_cast<u16>(symbol);
            }
        }
    }
    return output == code->symbol_count;
}

// Canonical decoding of the next codeword, bit-serial semantics: the first
// read bit is the codeword's most significant bit.
template <typename Code, typename Symbol>
bool decode_huffman(BitReader* reader, const Code& code, const Symbol* symbols, u16* symbol) noexcept {
    if (!reader || !symbol || code.symbol_count == 0) {
        return false;
    }
    if (code.symbol_count == 1) {
        *symbol = code.single_symbol;
        return true;
    }
    const u64 available = total_bits(*reader) - reader->bit_position;
    const u64 bits = peek_bits(*reader);
    u32 canonical = 0;
    u32 first = 0;
    u32 base = 0;
    for (u32 length = 1; length <= 15; ++length) {
        if (length > available) {
            return fail_at_end(reader);
        }
        canonical = canonical << 1 | static_cast<u32>(bits >> (length - 1u) & 1u);
        const u32 count = code.counts[length];
        if (canonical >= first && canonical - first < count) {
            const u32 index = base + canonical - first;
            reader->bit_position += length;
            if (index >= code.symbol_count) {
                return false;
            }
            *symbol = symbols[index];
            return true;
        }
        base += count;
        first = (first + count) << 1;
    }
    reader->bit_position += 15;
    return false;
}

// Root lookup for codewords of up to kVp8lLookupBits bits: entry = symbol |
// length << 12, zero where a longer codeword continues (canonical decoding).
void build_lookup(Vp8lWorkspace* workspace, Vp8lHuffmanCode* code) noexcept {
    code->table_bits = 0;
    if (code->symbol_count < 2) {
        return;
    }
    u32 longest = 15;
    while (code->counts[longest] == 0) {
        --longest;
    }
    const u32 bits = minimum(longest, kVp8lLookupBits);
    const u32 size = 1u << bits;
    if (size > kVp8lLookupCapacity - workspace->lookup_count) {
        return;
    }
    u16* table = workspace->lookup + workspace->lookup_count;
    for (u32 index = 0; index < size; ++index) {
        table[index] = 0;
    }
    const u16* symbols = workspace->symbols + code->symbol_offset;
    u32 canonical = 0;
    u32 sorted = 0;
    for (u32 length = 1; length <= bits; ++length, canonical <<= 1) {
        for (u32 left = code->counts[length]; left != 0; --left, ++canonical, ++sorted) {
            u32 reversed = 0;
            for (u32 bit = 0; bit < length; ++bit) {
                reversed |= ((canonical >> bit) & 1u) << (length - 1u - bit);
            }
            const u16 entry = static_cast<u16>(symbols[sorted] | length << 12);
            for (u32 index = reversed; index < size; index += 1u << length) {
                table[index] = entry;
            }
        }
    }
    code->table_offset = workspace->lookup_count;
    code->table_bits = static_cast<u8>(bits);
    workspace->lookup_count += size;
}

bool build_code(Vp8lWorkspace* workspace, u32 alphabet_size, Vp8lHuffmanCode* code) noexcept {
    if (!workspace || !code || alphabet_size == 0 || alphabet_size > kVp8lMaximumAlphabetSize) {
        return false;
    }
    *code = Vp8lHuffmanCode{};
    for (u32 symbol = 0; symbol < alphabet_size; ++symbol) {
        const u8 length = workspace->code_lengths[symbol];
        if (length > 15) {
            return false;
        }
        if (length != 0) {
            ++code->counts[length];
            ++code->symbol_count;
            code->single_symbol = static_cast<u16>(symbol);
        }
    }
    if (code->symbol_count == 0 ||
        workspace->symbol_count + code->symbol_count > kVp8lHuffmanSymbolCapacity) {
        return false;
    }
    if (code->symbol_count != 1) {
        i32 open = 1;
        for (u32 length = 1; length <= 15; ++length) {
            open = open * 2 - code->counts[length];
            if (open < 0) {
                return false;
            }
        }
        if (open != 0) {
            return false;
        }
    }
    code->symbol_offset = workspace->symbol_count;
    for (u32 length = 1; length <= 15; ++length) {
        for (u32 symbol = 0; symbol < alphabet_size; ++symbol) {
            if (workspace->code_lengths[symbol] == length) {
                workspace->symbols[workspace->symbol_count++] = static_cast<u16>(symbol);
            }
        }
    }
    if (workspace->symbol_count != code->symbol_offset + code->symbol_count) {
        return false;
    }
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

// The next symbol through the window; codewords past the lookup decode canonically.
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
        const u32 entry =
            workspace->lookup[code.table_offset + (static_cast<u32>(window->bits) & ((1u << code.table_bits) - 1u))];
        const u32 length = entry >> 12;
        if (length != 0) {
            // Below 15 bits the window holds exactly the rest of the stream.
            if (length > window->available) {
                return fail_at_end(reader);
            }
            window_consume(reader, window, length);
            *symbol = static_cast<u16>(entry & 0xFFFu);
            return true;
        }
    }
    const bool decoded = decode_huffman(reader, code, workspace->symbols + code.symbol_offset, symbol);
    window_fill(*reader, window);
    return decoded;
}

inline constexpr u8 kCodeLengthOrder[19] = {
    17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

bool read_huffman_code(BitReader* reader, u32 alphabet_size, Vp8lWorkspace* workspace,
                       Vp8lHuffmanCode* code) noexcept {
    if (!reader || !workspace || !code || alphabet_size == 0 || alphabet_size > kVp8lMaximumAlphabetSize) {
        return false;
    }
    for (u32 index = 0; index < alphabet_size; ++index) {
        workspace->code_lengths[index] = 0;
    }
    if (read_bits(reader, 1) != 0) {
        const u32 symbols = read_bits(reader, 1) + 1;
        const u32 first_bits = read_bits(reader, 1) != 0 ? 8u : 1u;
        const u32 first = read_bits(reader, first_bits);
        if (reader->failed || first >= alphabet_size) {
            return false;
        }
        workspace->code_lengths[first] = 1;
        if (symbols == 2) {
            const u32 second = read_bits(reader, 8);
            if (reader->failed || second >= alphabet_size) {
                return false;
            }
            workspace->code_lengths[second] = 1;
        }
        return build_code(workspace, alphabet_size, code);
    }

    u8 length_code_lengths[19]{};
    const u32 length_code_count = read_bits(reader, 4) + 4;
    for (u32 index = 0; index < length_code_count; ++index) {
        length_code_lengths[kCodeLengthOrder[index]] = static_cast<u8>(read_bits(reader, 3));
    }
    SmallHuffmanCode length_code{};
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
        if (!decode_huffman(reader, length_code, length_code.symbols, &value)) {
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
        for (u32 index = 0; index < repeat; ++index) {
            workspace->code_lengths[output++] = repeated;
        }
    }
    return build_code(workspace, alphabet_size, code);
}

} // namespace

WebpParseResult webp_parse(ConstSlice<u8> encoded, WebpImage* image) noexcept {
    WebpParseResult result{};
    if (!image || (!encoded.data && encoded.count != 0)) {
        return parse_failure(result, WebpError::invalid_input);
    }
    *image = WebpImage{};
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
            image->extended = true;
            vp8x_seen = true;
        } else if (type == kVp8l) {
            if (image_seen || size < 5 || payload[0] != 0x2Fu) {
                return parse_failure(result, image_seen ? WebpError::duplicate_chunk : WebpError::chunk);
            }
            const u32 header = byte_load_le32(payload + 1);
            const u32 width = (header & 0x3FFFu) + 1;
            const u32 height = (header >> 14 & 0x3FFFu) + 1;
            if ((header >> 29) != 0 || width > kWebpDimensionMaximum || height > kWebpDimensionMaximum) {
                return parse_failure(result, width > kWebpDimensionMaximum || height > kWebpDimensionMaximum
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
    u32 peak_groups;
    u32 peak_symbols;
    WebpError error;
};

void set_error(DecodeState* state, WebpError error) noexcept {
    if (state && state->error == WebpError::none) {
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

bool decode_entropy_image(DecodeState* state, u32 width, u32 height, bool allow_meta, u32* output,
                          u32 output_capacity) noexcept {
    if (!state || !state->workspace || !output || width == 0 || height == 0 ||
        static_cast<u64>(width) * height > output_capacity) {
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
            workspace->entropy_codes[index] = static_cast<u16>(group);
            maximum_group = maximum(maximum_group, group);
        }
        group_count = maximum_group + 1;
    }
    if (state->reader.failed || group_count == 0 || group_count > kVp8lHuffmanGroupCapacity) {
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
    }
    state->peak_groups = maximum(state->peak_groups, group_count);
    state->peak_symbols = maximum(state->peak_symbols, workspace->symbol_count);

    const u32 pixel_count = width * height;
    // x/y track position; the group changes only at prefix-block columns,
    // on a new row and after a backward copy.
    const u32 block_mask = prefix_bits != 0 ? (1u << prefix_bits) - 1u : ~0u;
    const u32* const cache = workspace->color_cache;
    u32* const color_cache = workspace->color_cache;
    const u32 cache_shift = 32u - cache_bits;
    u32 position = 0;
    u32 x = 0;
    u32 y = 0;
    const Vp8lHuffmanGroup* codes = &workspace->groups[0];
    BitWindow window{};
    window_fill(state->reader, &window);
    while (position < pixel_count) {
        if ((x & block_mask) == 0 && prefix_bits != 0) {
            codes = &workspace->groups[workspace->entropy_codes[(y >> prefix_bits) * prefix_width + (x >> prefix_bits)]];
        }
        u16 green = 0;
        if (!window_decode(&state->reader, &window, workspace, codes->codes[0], &green)) {
            set_error(state, WebpError::huffman);
            return false;
        }
        if (green < 256) {
            u16 red = 0;
            u16 blue = 0;
            u16 alpha = 0;
            if (!window_decode(&state->reader, &window, workspace, codes->codes[1], &red) ||
                !window_decode(&state->reader, &window, workspace, codes->codes[2], &blue) ||
                !window_decode(&state->reader, &window, workspace, codes->codes[3], &alpha)) {
                set_error(state, WebpError::huffman);
                return false;
            }
            const u32 pixel = static_cast<u32>(alpha) << 24 | static_cast<u32>(red) << 16 |
                              static_cast<u32>(green) << 8 | blue;
            output[position++] = pixel;
            if (cache_bits != 0) {
                color_cache[static_cast<u32>(0x1E35A7BDull * pixel) >> cache_shift] = pixel;
            }
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
            if (state->reader.failed || length == 0 || length > pixel_count - position || distance == 0 ||
                distance > position) {
                set_error(state, WebpError::distance);
                return false;
            }
            u32* target = output + position;
            const u32* source = target - distance;
            if (cache_bits != 0) {
                for (u32 index = 0; index < length; ++index) {
                    const u32 pixel = source[index];
                    target[index] = pixel;
                    color_cache[static_cast<u32>(0x1E35A7BDull * pixel) >> cache_shift] = pixel;
                }
            } else {
                for (u32 index = 0; index < length; ++index) {
                    target[index] = source[index];
                }
            }
            position += length;
            x += length;
            while (x >= width) {
                x -= width;
                ++y;
            }
            if (prefix_bits != 0 && position < pixel_count) {
                codes = &workspace->groups[workspace->entropy_codes[(y >> prefix_bits) * prefix_width + (x >> prefix_bits)]];
            }
        } else {
            const u32 cache_index = green - 280;
            if (cache_index >= cache_size) {
                set_error(state, WebpError::bitstream);
                return false;
            }
            const u32 pixel = cache[cache_index];
            output[position++] = pixel;
            color_cache[static_cast<u32>(0x1E35A7BDull * pixel) >> cache_shift] = pixel;
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

__forceinline u32 pack_channels(u8 alpha, u8 red, u8 green, u8 blue) noexcept {
    return static_cast<u32>(alpha) << 24 | static_cast<u32>(red) << 16 | static_cast<u32>(green) << 8 |
           blue;
}

// Per-channel sums and floor averages without carries between channels.
__forceinline u32 add_pixels(u32 first, u32 second) noexcept {
    const u32 alpha_green = (first & 0xFF00FF00u) + (second & 0xFF00FF00u);
    const u32 red_blue = (first & 0x00FF00FFu) + (second & 0x00FF00FFu);
    return (alpha_green & 0xFF00FF00u) | (red_blue & 0x00FF00FFu);
}

__forceinline u32 average_pixels(u32 first, u32 second) noexcept {
    return (((first ^ second) & 0xFEFEFEFEu) >> 1) + (first & second);
}

__forceinline u32 select_predictor(u32 left, u32 top, u32 top_left) noexcept {
    // Sums over the four channels of |estimate - l| = |t - tl| and
    // |estimate - t| = |l - tl|, as byte sums of absolute differences.
    const __m128i corner = _mm_cvtsi32_si128(static_cast<int>(top_left));
    const i32 left_distance =
        _mm_cvtsi128_si32(_mm_sad_epu8(_mm_cvtsi32_si128(static_cast<int>(top)), corner));
    const i32 top_distance =
        _mm_cvtsi128_si32(_mm_sad_epu8(_mm_cvtsi32_si128(static_cast<int>(left)), corner));
    return left_distance < top_distance ? left : top;
}

__forceinline u32 clamp_add_subtract(u32 first, u32 second, u32 third) noexcept {
    const __m128i zero = _mm_setzero_si128();
    const __m128i a = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(first)), zero);
    const __m128i b = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(second)), zero);
    const __m128i c = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(third)), zero);
    const __m128i sum = _mm_sub_epi16(_mm_add_epi16(a, b), c);
    return static_cast<u32>(_mm_cvtsi128_si32(_mm_packus_epi16(sum, sum)));
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

// One run of pixels sharing a predictor mode; top_right of the last pixel in a
// row is the first pixel of the current row, which is pixels[p - width + 1].
template <u32 Mode>
void predict_run(u32* pixels, u32 begin, u32 end, u32 width) noexcept {
    for (u32 p = begin; p < end; ++p) {
        const u32 left = pixels[p - 1];
        const u32 top = pixels[p - width];
        u32 predicted = 0;
        if constexpr (Mode == 0) {
            predicted = 0xFF000000u;
        } else if constexpr (Mode == 1) {
            predicted = left;
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
        } else if constexpr (Mode == 11) {
            predicted = select_predictor(left, top, pixels[p - width - 1]);
        } else if constexpr (Mode == 12) {
            predicted = clamp_add_subtract(left, top, pixels[p - width - 1]);
        } else {
            predicted = clamp_add_half(average_pixels(left, top), pixels[p - width - 1]);
        }
        pixels[p] = add_pixels(pixels[p], predicted);
    }
}

void predict_mode_run(u8 mode, u32* pixels, u32 begin, u32 end, u32 width) noexcept {
    switch (mode) {
    case 0: predict_run<0>(pixels, begin, end, width); break;
    case 1: predict_run<1>(pixels, begin, end, width); break;
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

bool inverse_predictor(const Transform& transform, const Vp8lWorkspace* workspace, u32* pixels) noexcept {
    const u32 width = transform.width;
    const u32 block = 1u << transform.size_bits;
    const u32 blocks_wide = (width + block - 1) >> transform.size_bits;
    // read_transforms admitted only modes 0..13 and a map within capacity.
    pixels[0] = add_pixels(pixels[0], 0xFF000000u);
    for (u32 x = 1; x < width; ++x) {
        pixels[x] = add_pixels(pixels[x], pixels[x - 1]);
    }
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
    return true;
}

__forceinline i32 color_delta(signed char transform, signed char color) noexcept {
    return static_cast<i32>(transform) * color >> 5;
}

bool inverse_color(const Transform& transform, const Vp8lWorkspace* workspace, u32* pixels) noexcept {
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
            // Four pixels per step in 32-bit lanes: the same signed products,
            // arithmetic shifts and byte wrap as the scalar loop below.
            const __m128i to_red = _mm_set1_epi32(green_to_red);
            const __m128i to_blue = _mm_set1_epi32(green_to_blue);
            const __m128i red_to_blue_lanes = _mm_set1_epi32(red_to_blue);
            const __m128i low_byte = _mm_set1_epi32(0xFF);
            for (; x + 4 <= end; x += 4) {
                __m128i* slot = reinterpret_cast<__m128i*>(row + x);
                const __m128i pixel = _mm_loadu_si128(slot);
                const __m128i green = _mm_srai_epi32(_mm_slli_epi32(pixel, 16), 24);
                const __m128i red = _mm_and_si128(
                    _mm_add_epi32(_mm_srli_epi32(pixel, 16), _mm_srai_epi32(_mm_mullo_epi32(to_red, green), 5)),
                    low_byte);
                const __m128i signed_red = _mm_srai_epi32(_mm_slli_epi32(red, 24), 24);
                const __m128i blue = _mm_and_si128(
                    _mm_add_epi32(_mm_add_epi32(pixel, _mm_srai_epi32(_mm_mullo_epi32(to_blue, green), 5)),
                                  _mm_srai_epi32(_mm_mullo_epi32(red_to_blue_lanes, signed_red), 5)),
                    low_byte);
                const __m128i kept = _mm_and_si128(pixel, _mm_set1_epi32(static_cast<int>(0xFF00FF00u)));
                _mm_storeu_si128(slot, _mm_or_si128(_mm_or_si128(kept, _mm_slli_epi32(red, 16)), blue));
            }
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
    return true;
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
    for (; index < count; ++index) {
        const u32 pixel = pixels[index];
        const u32 green = pixel >> 8 & 0xFFu;
        pixels[index] = add_pixels(pixel, green << 16 | green);
    }
}

bool inverse_color_indexing(const Transform& transform, const u32* palette, u32 current_width,
                            u32* pixels) noexcept {
    // Indices past palette_count decode as transparent black.
    u32 table[256];
    for (u32 index = 0; index < 256; ++index) {
        table[index] = index < transform.palette_count ? palette[index] : 0;
    }
    const u32 bits = transform.width_bits;
    const u32 sample_bits = 8u >> bits;
    const u32 mask = bits == 0 ? 0xFFu : (1u << sample_bits) - 1u;
    for (u32 reverse_y = transform.height; reverse_y != 0; --reverse_y) {
        const u32 y = reverse_y - 1;
        const u32* packed_row = pixels + y * current_width;
        u32* row = pixels + y * transform.width;
        for (u32 reverse_x = transform.width; reverse_x != 0; --reverse_x) {
            const u32 x = reverse_x - 1;
            const u32 packed = packed_row[x >> bits];
            const u32 shift = (x & ((1u << bits) - 1u)) * sample_bits;
            row[x] = table[(packed >> 8 & 0xFFu) >> shift & mask];
        }
    }
    return true;
}

bool read_transforms(DecodeState* state, u32 original_width, u32 height, Transform* transforms,
                     u32* transform_count, u32* decoded_width, u32* palette) noexcept {
    bool seen[4]{};
    u32 width = original_width;
    while (read_bits(&state->reader, 1) != 0) {
        if (*transform_count >= 4) {
            set_error(state, WebpError::transform);
            return false;
        }
        const u32 type_value = read_bits(&state->reader, 2);
        if (type_value > 3 || seen[type_value]) {
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
    if (!reader || reader->failed) {
        return false;
    }
    const u64 total_bits = static_cast<u64>(reader->bytes.count) * 8u;
    if (reader->bit_position > total_bits) {
        return false;
    }
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
    if (!workspace || !rgba_output.data || !image || image->codec != WebpCodec::vp8l_lossless ||
        image->width == 0 || image->height == 0 || image->width > kWebpDimensionMaximum ||
        image->height > kWebpDimensionMaximum || !image->bitstream.data || image->bitstream.count == 0) {
        result.error = !workspace || !rgba_output.data || !image ? WebpError::invalid_input
                       : image->codec == WebpCodec::vp8_lossy    ? WebpError::unsupported_lossy
                                                                 : WebpError::bitstream;
        return result;
    }
    const u64 required = static_cast<u64>(image->width) * image->height * 4u;
    if (required > rgba_output.count || (reinterpret_cast<usize>(rgba_output.data) & 3u) != 0) {
        result.error = WebpError::output_capacity;
        return result;
    }
    DecodeState state{BitReader{image->bitstream, 0, false}, workspace, 0, 0, WebpError::none};
    Transform transforms[4]{};
    u32 transform_count = 0;
    u32 decoded_width = image->width;
    u32 palette[256]{};
    if (!read_transforms(&state, image->width, image->height, transforms, &transform_count, &decoded_width,
                         palette)) {
        result.error = state.error;
        return result;
    }
    auto* pixels = reinterpret_cast<u32*>(rgba_output.data);
    if (!decode_entropy_image(&state, decoded_width, image->height, true, pixels, kWebpPixelMaximum)) {
        result.error = state.error;
        return result;
    }
    u32 current_width = decoded_width;
    for (u32 reverse = transform_count; reverse != 0; --reverse) {
        const Transform& transform = transforms[reverse - 1];
        bool ok = true;
        switch (transform.type) {
        case TransformType::predictor:
            ok = inverse_predictor(transform, workspace, pixels);
            break;
        case TransformType::color:
            ok = inverse_color(transform, workspace, pixels);
            break;
        case TransformType::subtract_green:
            inverse_subtract_green(transform, pixels);
            break;
        case TransformType::color_indexing:
            ok = inverse_color_indexing(transform, palette, current_width, pixels);
            current_width = transform.width;
            break;
        }
        if (!ok) {
            result.error = WebpError::transform;
            return result;
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
        u32 index = 0;
        for (; index + 4 <= pixel_count; index += 4) {
            __m128i* slot = reinterpret_cast<__m128i*>(pixels + index);
            _mm_storeu_si128(slot, _mm_shuffle_epi8(_mm_loadu_si128(slot), order));
        }
        for (; index < pixel_count; ++index) {
            const u32 pixel = pixels[index];
            rgba_output[index * 4] = channel(pixel, 16);
            rgba_output[index * 4 + 1] = channel(pixel, 8);
            rgba_output[index * 4 + 2] = channel(pixel, 0);
            rgba_output[index * 4 + 3] = channel(pixel, 24);
        }
    }
    result.rgba_bytes = pixel_count * 4;
    result.huffman_groups = state.peak_groups;
    result.huffman_symbols = state.peak_symbols;
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
    *image = WebpImage{};
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
    *image = WebpImage{};
    if (width > kWebpDimensionMaximum || height > kWebpDimensionMaximum ||
        static_cast<u64>(width) * height > kWebpPixelMaximum) {
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
