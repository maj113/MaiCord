// Differential checks of the fast CPU decode paths against straightforward
// reference implementations of the same formats (the bit-serial forms the
// fast paths replaced): every result, error code, counter and output byte
// must agree, on valid input and on truncated or corrupted input.
#include "fixtures/image_decode_corpus.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/gif.hpp"
#include "maicord/media/jpeg.hpp"
#include "../src/media/png_common.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

struct Random {
    u32 state;
    u32 next() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    u32 below(u32 limit) noexcept {
        return next() % limit;
    }
};

u8* reserve(usize bytes) noexcept {
    return static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

// ---- PNG scanline filters ---------------------------------------------------------
u8 reference_paeth(u8 left, u8 above, u8 upper_left) noexcept {
    const i32 prediction = static_cast<i32>(left) + above - upper_left;
    const i32 a = prediction >= left ? prediction - left : left - prediction;
    const i32 b = prediction >= above ? prediction - above : above - prediction;
    const i32 c = prediction >= upper_left ? prediction - upper_left : upper_left - prediction;
    return a <= b && a <= c ? left : b <= c ? above : upper_left;
}

bool reference_unfilter(u8 filter, u8* bytes, const u8* previous, u32 count, u32 bpp) noexcept {
    if (filter > 4) {
        return false;
    }
    for (u32 x = 0; x < count; ++x) {
        const u8 left = x >= bpp ? bytes[x - bpp] : 0;
        const u8 above = previous ? previous[x] : 0;
        const u8 upper_left = previous && x >= bpp ? previous[x - bpp] : 0;
        u32 predictor = 0;
        switch (filter) {
        case 1: predictor = left; break;
        case 2: predictor = above; break;
        case 3: predictor = (static_cast<u32>(left) + above) >> 1; break;
        case 4: predictor = reference_paeth(left, above, upper_left); break;
        default: break;
        }
        bytes[x] = static_cast<u8>(bytes[x] + predictor);
    }
    return true;
}

void test_png_filters(Check check) noexcept {
    Random random{0x5EED1234u};
    constexpr u32 kBytesPerPixel[] = {1, 2, 3, 4, 6, 8};
    u8 raw[600], fast[600], expected[600], previous[600];
    bool exact = true;
    u32 cases = 0;
    for (const u32 bpp : kBytesPerPixel) {
        for (u32 pixels = 1; pixels <= 70 && exact; ++pixels) {
            const u32 count = pixels * bpp;
            for (u32 filter = 0; filter <= 5 && exact; ++filter) {
                for (u32 first = 0; first < 2 && exact; ++first) {
                    for (u32 round = 0; round < 3 && exact; ++round) {
                        // Mostly small residuals with some extremes, as real rows have.
                        for (u32 x = 0; x < count; ++x) {
                            const u32 pick = random.below(8);
                            raw[x] = static_cast<u8>(pick == 0 ? 255u : pick == 1 ? 0u : random.below(256));
                            previous[x] = static_cast<u8>(random.below(256));
                        }
                        copy_bytes(fast, raw, count);
                        copy_bytes(expected, raw, count);
                        const u8* above = first ? nullptr : previous;
                        const bool fast_ok = png_unfilter_row(static_cast<u8>(filter), fast, above, count, bpp);
                        const bool expected_ok = reference_unfilter(static_cast<u8>(filter), expected, above, count, bpp);
                        exact = fast_ok == expected_ok && bytes_equal(fast, expected, count);
                        ++cases;
                    }
                }
            }
        }
    }
    check(exact && cases == 6u * 70u * 6u * 2u * 3u, "png_unfilter_matches_reference_rows");
}

// ---- GIF LZW -------------------------------------------------------------------------
struct ReferenceGifReader {
    const u8* bytes;
    u32 cursor;
    u32 end;
    u32 block_remaining;
    u32 bits;
    u32 bit_count;
    bool terminated;
};

bool reference_gif_byte(ReferenceGifReader* reader, u8* value) noexcept {
    if (reader->terminated) {
        return false;
    }
    if (reader->block_remaining == 0) {
        if (reader->cursor >= reader->end) {
            return false;
        }
        reader->block_remaining = reader->bytes[reader->cursor++];
        if (reader->block_remaining == 0) {
            reader->terminated = true;
            return false;
        }
        if (reader->block_remaining > reader->end - reader->cursor) {
            return false;
        }
    }
    *value = reader->bytes[reader->cursor++];
    --reader->block_remaining;
    return true;
}

bool reference_gif_code(ReferenceGifReader* reader, u32 code_size, u32* code) noexcept {
    while (reader->bit_count < code_size) {
        u8 value = 0;
        if (!reference_gif_byte(reader, &value)) {
            return false;
        }
        reader->bits |= static_cast<u32>(value) << reader->bit_count;
        reader->bit_count += 8;
    }
    *code = reader->bits & ((1u << code_size) - 1u);
    reader->bits >>= code_size;
    reader->bit_count -= code_size;
    return true;
}

// The stack decoder (non-interlaced) with its exact error order.
GifError reference_gif_decode(const u8* data, u32 data_end, u32 minimum_code_size, u32 palette_count, u32 pixels,
                              u8* output) noexcept {
    static u16 prefix[kGifLzwDictionaryCapacity];
    static u8 suffix[kGifLzwDictionaryCapacity];
    static u8 stack[kGifLzwDictionaryCapacity];
    ReferenceGifReader reader{data, 0, data_end, 0, 0, 0, false};
    const u32 clear_code = 1u << minimum_code_size;
    const u32 end_code = clear_code + 1u;
    u32 next_code = end_code + 1u, code_size = minimum_code_size + 1u, old_code = 0, first = 0, output_count = 0;
    bool have_old = false, first_code = true, end_seen = false;
    for (;;) {
        u32 code = 0;
        if (!reference_gif_code(&reader, code_size, &code)) {
            break;
        }
        if (first_code && code != clear_code) {
            return GifError::lzw_missing_clear;
        }
        first_code = false;
        if (code == clear_code) {
            next_code = end_code + 1u;
            code_size = minimum_code_size + 1u;
            have_old = false;
            continue;
        }
        if (code == end_code) {
            end_seen = true;
            break;
        }
        if (!have_old) {
            if (code >= clear_code || output_count >= pixels || code >= palette_count) {
                return GifError::lzw_invalid_code;
            }
            output[output_count++] = static_cast<u8>(code);
            old_code = first = code;
            have_old = true;
            continue;
        }
        const u32 incoming = code;
        u32 stack_count = 0;
        if (code == next_code) {
            stack[stack_count++] = static_cast<u8>(first);
            code = old_code;
        } else if (code > next_code) {
            return GifError::lzw_invalid_code;
        }
        u32 chain = 0;
        while (code >= clear_code) {
            if (code < end_code + 1u || code >= next_code || stack_count >= kGifLzwDictionaryCapacity ||
                ++chain > kGifLzwDictionaryCapacity) {
                return GifError::lzw_dictionary;
            }
            stack[stack_count++] = suffix[code];
            code = prefix[code];
        }
        first = code;
        if (stack_count >= kGifLzwDictionaryCapacity) {
            return GifError::lzw_dictionary;
        }
        stack[stack_count++] = static_cast<u8>(first);
        while (stack_count != 0) {
            const u8 value = stack[--stack_count];
            if (output_count >= pixels || value >= palette_count) {
                return GifError::lzw_pixel_count;
            }
            output[output_count++] = value;
        }
        if (next_code < kGifLzwDictionaryCapacity) {
            prefix[next_code] = static_cast<u16>(old_code);
            suffix[next_code] = static_cast<u8>(first);
            ++next_code;
            if (next_code == (1u << code_size) && code_size < 12) {
                ++code_size;
            }
        }
        old_code = incoming;
    }
    if (!end_seen) {
        return GifError::lzw_missing_end;
    }
    return output_count == pixels ? GifError::none : GifError::lzw_pixel_count;
}

struct CodeWriter {
    u8* bytes;
    u32 count;
    u32 capacity;
    u32 bits;
    u32 bit_count;
    void put(u32 code, u32 size) noexcept {
        bits |= code << bit_count;
        bit_count += size;
        while (bit_count >= 8 && count < capacity) {
            bytes[count++] = static_cast<u8>(bits);
            bits >>= 8;
            bit_count -= 8;
        }
    }
    void flush() noexcept {
        if (bit_count && count < capacity) {
            bytes[count++] = static_cast<u8>(bits);
        }
        bits = bit_count = 0;
    }
};

// A real LZW encoder: long strings, KwKwK codes, full dictionaries either
// cleared or kept (deferred clear).
u32 lzw_encode(const u8* pixels, u32 count, u32 minimum_code_size, bool clear_when_full, u8* output,
               u32 capacity) noexcept {
    static u16 keys_code[8192];
    static u32 keys[8192];
    CodeWriter writer{output, 0, capacity, 0, 0};
    const u32 clear_code = 1u << minimum_code_size, end_code = clear_code + 1u;
    u32 next_code = end_code + 1u, code_size = minimum_code_size + 1u;
    const auto reset = [&]() noexcept {
        for (u32& key : keys) {
            key = ~0u;
        }
        next_code = end_code + 1u;
        code_size = minimum_code_size + 1u;
    };
    reset();
    writer.put(clear_code, code_size);
    u32 current = pixels[0];
    for (u32 index = 1; index < count; ++index) {
        const u32 key = current << 8 | pixels[index];
        u32 slot = (key * 2654435761u) >> 19;
        while (keys[slot] != ~0u && keys[slot] != key) {
            slot = (slot + 1) & 8191u;
        }
        if (keys[slot] == key) {
            current = keys_code[slot];
            continue;
        }
        writer.put(current, code_size);
        if (next_code < kGifLzwDictionaryCapacity) {
            keys[slot] = key;
            keys_code[slot] = static_cast<u16>(next_code++);
            // The decoder widens one code later than the encoder assigns.
            if (next_code - 1u == (1u << code_size) && code_size < 12) {
                ++code_size;
            }
        } else if (clear_when_full) {
            writer.put(clear_code, code_size);
            reset();
        }
        current = pixels[index];
    }
    writer.put(current, code_size);
    // The decoder defines one more entry after the last code before it reads the end code.
    if (next_code < kGifLzwDictionaryCapacity && ++next_code - 1u == (1u << code_size) && code_size < 12) {
        ++code_size;
    }
    writer.put(end_code, code_size);
    writer.flush();
    return writer.count;
}

// GIF89a with a global palette and one full-canvas frame of the given LZW bytes.
u32 build_gif(const u8* lzw, u32 lzw_bytes, u32 width, u32 height, u32 palette_bits, u32 minimum_code_size,
              bool interlaced, u8* output) noexcept {
    u32 cursor = 0;
    const auto put = [&](u32 value) noexcept { output[cursor++] = static_cast<u8>(value); };
    for (const char* c = "GIF89a"; *c; ++c) {
        put(static_cast<u8>(*c));
    }
    put(width & 255u), put(width >> 8), put(height & 255u), put(height >> 8);
    put(0x80u | (palette_bits - 1u)), put(0), put(0);
    for (u32 index = 0; index < (1u << palette_bits); ++index) {
        put(index * 3u), put(255u - index), put(index * 7u);
    }
    put(0x2C), put(0), put(0), put(0), put(0);
    put(width & 255u), put(width >> 8), put(height & 255u), put(height >> 8), put(interlaced ? 0x40u : 0u);
    put(minimum_code_size);
    for (u32 offset = 0; offset < lzw_bytes; offset += 255) {
        const u32 chunk = minimum(255u, lzw_bytes - offset);
        put(chunk);
        copy_bytes(output + cursor, lzw + offset, chunk);
        cursor += chunk;
    }
    put(0);
    put(0x3B);
    return cursor;
}

void test_gif_lzw(Check check) noexcept {
    constexpr u32 kWidth = 61, kHeight = 37, kPixels = kWidth * kHeight;
    u8* const pixels = reserve(kPixels * 2u);
    u8* const lzw = reserve(64u * 1024u);
    u8* const gif = reserve(80u * 1024u);
    u8* const expected = reserve(kPixels * 2u);
    u8* const decoded = reserve(kPixels * 2u + 64u);
    GifWorkspace* const workspace = reinterpret_cast<GifWorkspace*>(reserve(sizeof(GifWorkspace)));
    if (!pixels || !lzw || !gif || !expected || !decoded || !workspace) {
        check(false, "gif_lzw_reference_allocation");
        return;
    }
    Random random{0xC0FFEE11u};
    bool exact = true;
    u32 decoded_ok = 0, rejected = 0, unparsed = 0, interlaced_ok = 0;
    for (u32 trial = 0; trial < 600 && exact; ++trial) {
        const u32 palette_bits = 1u + random.below(8);
        const u32 minimum_code_size = palette_bits < 2 ? 2u : palette_bits;
        const u32 colors = 1u << palette_bits;
        // Runs and gradients for long strings; noise for short ones.
        const u32 style = random.below(3);
        for (u32 index = 0; index < kPixels; ++index) {
            const u32 value = style == 0   ? (index / (1u + random.below(40))) % colors
                              : style == 1 ? random.below(colors)
                                           : (index % kWidth + index / kWidth) % colors;
            pixels[index] = static_cast<u8>(value);
        }
        // Some streams carry indices past a smaller palette or more pixels than the frame.
        const u32 encoded_pixels = trial % 11 == 5 ? kPixels + 1u + random.below(50) : kPixels;
        u32 lzw_bytes = lzw_encode(pixels, encoded_pixels, minimum_code_size, (trial & 1u) != 0, lzw, 64u * 1024u);
        const u32 mutation = trial % 7;
        if (mutation == 1 && lzw_bytes > 2) {
            lzw_bytes = 1u + random.below(lzw_bytes - 1u); // truncated stream
        } else if (mutation == 2 || mutation == 3) {
            for (u32 flip = 0; flip < 1u + random.below(3); ++flip) {
                lzw[random.below(lzw_bytes)] ^= static_cast<u8>(1u << random.below(8));
            }
        }
        const u32 frame_palette_bits = trial % 13 == 4 && palette_bits > 1 ? palette_bits - 1u : palette_bits;
        // Interlaced frames carry the same rows in pass order.
        const bool interlaced = trial % 3 == 2;
        const u32 gif_bytes =
            build_gif(lzw, lzw_bytes, kWidth, kHeight, frame_palette_bits, minimum_code_size, interlaced, gif);
        GifImage image{};
        if (!gif_parse({gif, gif_bytes}, &image, workspace).ok() || image.frame_count != 1) {
            ++unparsed;
            continue;
        }
        const GifFrame& frame = image.frames[0];
        const GifError reference = reference_gif_decode(gif + frame.data_offset, frame.data_end - frame.data_offset,
                                                        frame.minimum_code_size, frame.palette_count, kPixels,
                                                        expected);
        clear_bytes(decoded, kPixels);
        const GifDecodeResult result = gif_decode_parsed_frame({gif, gif_bytes}, image, 0, workspace,
                                                               {decoded, kPixels * 2u + 64u});
        bool rows_equal = true;
        if (reference == GifError::none) {
            u32 stream = 0;
            constexpr u32 starts[4] = {0, 4, 2, 1};
            constexpr u32 steps[4] = {8, 8, 4, 2};
            for (u32 pass = 0; pass < (interlaced ? 4u : 1u); ++pass) {
                for (u32 row = interlaced ? starts[pass] : 0u; row < kHeight; row += interlaced ? steps[pass] : 1u) {
                    rows_equal = rows_equal && bytes_equal(decoded + row * kWidth, expected + stream * kWidth, kWidth);
                    ++stream;
                }
            }
        }
        exact = result.error == reference && rows_equal;
        interlaced_ok += interlaced && reference == GifError::none ? 1u : 0u;
        reference == GifError::none ? ++decoded_ok : ++rejected;
    }
    check(exact && decoded_ok > 100 && rejected > 100 && interlaced_ok > 30 && unparsed < 60,
          "gif_lzw_matches_reference_decoder");
    VirtualFree(pixels, 0, MEM_RELEASE);
    VirtualFree(lzw, 0, MEM_RELEASE);
    VirtualFree(gif, 0, MEM_RELEASE);
    VirtualFree(expected, 0, MEM_RELEASE);
    VirtualFree(decoded, 0, MEM_RELEASE);
    VirtualFree(workspace, 0, MEM_RELEASE);
}

// ---- JPEG entropy decoding -------------------------------------------------------------
inline constexpr u8 kZigzagToRaster[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// Reference: the bit-serial reader and block decoder the lookahead decoder replaced.
struct ReferenceReader {
    ConstSlice<u8> bytes;
    u32 offset;
    u8 value;
    u8 bits;
    JpegError error;
};

bool entropy_load_byte(ReferenceReader* reader) noexcept {
    if (!reader || reader->offset >= reader->bytes.count) {
        if (reader) {
            reader->error = JpegError::truncated;
        }
        return false;
    }
    u8 value = reader->bytes[reader->offset++];
    if (value == 0xFF) {
        while (reader->offset < reader->bytes.count && reader->bytes[reader->offset] == 0xFF) {
            ++reader->offset;
        }
        if (reader->offset >= reader->bytes.count || reader->bytes[reader->offset] != 0) {
            reader->error = JpegError::restart;
            return false;
        }
        ++reader->offset;
        value = 0xFF;
    }
    reader->value = value;
    reader->bits = 8;
    return true;
}

u32 entropy_read_bit(ReferenceReader* reader) noexcept {
    if (!reader || reader->error != JpegError::none) {
        return 0;
    }
    if (reader->bits == 0 && !entropy_load_byte(reader)) {
        return 0;
    }
    --reader->bits;
    return reader->value >> reader->bits & 1u;
}

u32 entropy_read_bits(ReferenceReader* reader, u32 count) noexcept {
    if (count > 16) {
        reader->error = JpegError::entropy;
        return 0;
    }
    u32 value = 0;
    for (u32 index = 0; index < count; ++index) {
        value = value << 1 | entropy_read_bit(reader);
    }
    return value;
}

bool entropy_align(ReferenceReader* reader) noexcept {
    if (!reader || reader->error != JpegError::none) {
        return false;
    }
    if (reader->bits != 0) {
        const u32 mask = (1u << reader->bits) - 1u;
        if ((reader->value & mask) != mask) {
            reader->error = JpegError::entropy;
            return false;
        }
        reader->bits = 0;
    }
    return true;
}

bool entropy_consume_restart(ReferenceReader* reader, u8 expected) noexcept {
    if (!entropy_align(reader) || reader->offset >= reader->bytes.count ||
        reader->bytes[reader->offset] != 0xFF) {
        if (reader && reader->error == JpegError::none) {
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
    return true;
}

bool decode_huffman_symbol(ReferenceReader* reader, const JpegHuffmanTable& table, u8* symbol) noexcept {
    if (!reader || !symbol || !table.present) {
        if (reader) {
            reader->error = JpegError::huffman;
        }
        return false;
    }
    i32 code = 0;
    for (u32 length = 0; length < 16; ++length) {
        code = code << 1 | static_cast<i32>(entropy_read_bit(reader));
        if (reader->error != JpegError::none) {
            return false;
        }
        if (table.maximum_code[length] >= 0 && code >= table.minimum_code[length] &&
            code <= table.maximum_code[length]) {
            const i32 index = table.value_offset[length] + code;
            if (index < 0 || static_cast<u32>(index) >= table.symbol_count) {
                reader->error = JpegError::huffman;
                return false;
            }
            *symbol = table.symbols[index];
            return true;
        }
    }
    reader->error = JpegError::huffman;
    return false;
}

i32 receive_extend(ReferenceReader* reader, u32 bits) noexcept {
    if (bits == 0) {
        return 0;
    }
    const u32 value = entropy_read_bits(reader, bits);
    const u32 threshold = 1u << (bits - 1u);
    return value < threshold ? static_cast<i32>(value) - static_cast<i32>((1u << bits) - 1u)
                             : static_cast<i32>(value);
}

bool decode_block(ReferenceReader* reader, const JpegHuffmanTable& dc_table,
                  const JpegHuffmanTable& ac_table, i32* predictor, JpegCoefficientBlock* block,
                  u32* huffman_symbols) noexcept {
    if (!reader || !predictor || !block || !huffman_symbols) {
        return false;
    }
    for (u32 index = 0; index < 64; ++index) {
        block->values[index] = 0;
    }
    u8 symbol = 0;
    if (!decode_huffman_symbol(reader, dc_table, &symbol) || symbol > 11) {
        reader->error = JpegError::coefficient;
        return false;
    }
    ++*huffman_symbols;
    *predictor += receive_extend(reader, symbol);
    if (reader->error != JpegError::none || *predictor < -32'768 || *predictor > 32'767) {
        reader->error = JpegError::coefficient;
        return false;
    }
    block->values[0] = static_cast<i16>(*predictor);

    u32 coefficient = 1;
    while (coefficient < 64) {
        if (!decode_huffman_symbol(reader, ac_table, &symbol)) {
            return false;
        }
        ++*huffman_symbols;
        const u32 run = symbol >> 4;
        const u32 bits = symbol & 0x0F;
        if (bits == 0) {
            if (run == 0) {
                break;
            }
            if (run != 15 || coefficient + 16 > 64) {
                reader->error = JpegError::coefficient;
                return false;
            }
            coefficient += 16;
            continue;
        }
        if (bits > 10 || coefficient + run >= 64) {
            reader->error = JpegError::coefficient;
            return false;
        }
        coefficient += run;
        const i32 value = receive_extend(reader, bits);
        if (reader->error != JpegError::none || value < -1'023 || value > 1'023) {
            reader->error = JpegError::coefficient;
            return false;
        }
        block->values[kZigzagToRaster[coefficient]] = static_cast<i16>(value);
        ++coefficient;
    }
    return true;
}



// The baseline scan loop of the bit-serial decoder.
JpegDecodeResult reference_baseline(ConstSlice<u8> encoded, const JpegImage& image,
                                    JpegCoefficientWorkspace* coefficients) noexcept {
    JpegDecodeResult result{};
    ReferenceReader reader{ConstSlice<u8>{encoded.data + image.entropy_offset, image.entropy_bytes}, 0, 0, 0,
                           JpegError::none};
    i32 predictor[kJpegComponentMaximum]{};
    const u32 total_mcus = static_cast<u32>(image.mcu_columns) * image.mcu_rows;
    u8 expected_restart = 0;
    for (u32 mcu = 0; mcu < total_mcus; ++mcu) {
        if (image.restart_interval != 0 && mcu != 0 && mcu % image.restart_interval == 0) {
            if (!entropy_consume_restart(&reader, expected_restart)) {
                result.error = reader.error;
                return result;
            }
            expected_restart = static_cast<u8>((expected_restart + 1) & 7);
            for (u32 index = 0; index < image.component_count; ++index) {
                predictor[index] = 0;
            }
        }
        const u32 mcu_x = mcu % image.mcu_columns;
        const u32 mcu_y = mcu / image.mcu_columns;
        for (u32 scan = 0; scan < image.component_count; ++scan) {
            const u32 component_index = image.scan_order[scan];
            const JpegComponent& component = image.components[component_index];
            const JpegHuffmanTable& dc = image.tables->dc[component.dc_table];
            const JpegHuffmanTable& ac = image.tables->ac[component.ac_table];
            for (u32 vertical = 0; vertical < component.vertical_sampling; ++vertical) {
                for (u32 horizontal = 0; horizontal < component.horizontal_sampling; ++horizontal) {
                    const u32 block_x = mcu_x * component.horizontal_sampling + horizontal;
                    const u32 block_y = mcu_y * component.vertical_sampling + vertical;
                    const u32 block_index = component.block_offset + block_y * component.block_columns + block_x;
                    if (block_index >= image.coefficient_blocks ||
                        !decode_block(&reader, dc, ac, &predictor[component_index],
                                      &coefficients->blocks[block_index], &result.huffman_symbols)) {
                        result.error = reader.error == JpegError::none ? JpegError::coefficient : reader.error;
                        return result;
                    }
                    ++result.decoded_blocks;
                }
            }
        }
        ++result.decoded_mcus;
    }
    if (!entropy_align(&reader) || reader.offset != reader.bytes.count ||
        result.decoded_blocks != image.coefficient_blocks) {
        result.error = reader.error == JpegError::none ? JpegError::entropy : reader.error;
        return result;
    }
    result.error = JpegError::none;
    result.decoded_scans = 1;
    return result;
}

// Every block is compared, including those written before a failure.
bool same_decode(const JpegDecodeResult& fast, const JpegDecodeResult& reference, const JpegCoefficientWorkspace& a,
                 const JpegCoefficientWorkspace& b, u32 blocks) noexcept {
    return fast.error == reference.error && fast.decoded_blocks == reference.decoded_blocks &&
           fast.decoded_mcus == reference.decoded_mcus && fast.huffman_symbols == reference.huffman_symbols &&
           fast.decoded_scans == reference.decoded_scans &&
           bytes_equal(a.blocks, b.blocks, blocks * static_cast<u32>(sizeof(JpegCoefficientBlock)));
}

void test_jpeg_entropy(Check check) noexcept {
    auto* workspace = reinterpret_cast<JpegWorkspace*>(reserve(sizeof(JpegWorkspace)));
    auto* reference = reinterpret_cast<JpegCoefficientWorkspace*>(reserve(sizeof(JpegCoefficientWorkspace)));
    u8* const mutated = reserve(1u << 20);
    if (!workspace || !reference || !mutated) {
        check(false, "jpeg_entropy_reference_allocation");
        return;
    }
    Random random{0x1BADB002u};
    bool exact = true;
    u32 images = 0, cases = 0, failures = 0;
    for (const auto& entry : kImageCorpus) {
        if (entry.kind != ImageCorpusKind::jpeg || entry.bytes > (1u << 20)) {
            continue;
        }
        JpegImage image{};
        if (!jpeg_parse({entry.data, entry.bytes}, &image, workspace).ok() || image.progressive) {
            continue;
        }
        ++images;
        const u32 blocks = image.coefficient_blocks;
        const auto compare = [&](ConstSlice<u8> encoded, const JpegImage& variant) noexcept {
            clear_bytes(workspace->coefficients.blocks, blocks * static_cast<u32>(sizeof(JpegCoefficientBlock)));
            clear_bytes(reference->blocks, blocks * static_cast<u32>(sizeof(JpegCoefficientBlock)));
            const JpegDecodeResult fast = jpeg_decode_parsed_coefficients(encoded, variant, &workspace->coefficients);
            const JpegDecodeResult expected = reference_baseline(encoded, variant, reference);
            ++cases;
            failures += expected.error == JpegError::none ? 0u : 1u;
            return same_decode(fast, expected, workspace->coefficients, *reference, blocks);
        };
        exact = exact && compare({entry.data, entry.bytes}, image);
        // Truncated entropy segments at every length near the start and end, sampled between.
        for (u32 length = 0; length < image.entropy_bytes && exact; ++length) {
            if (length > 64 && length + 64 < image.entropy_bytes && random.below(16) != 0) {
                continue;
            }
            JpegImage truncated = image;
            truncated.entropy_bytes = length;
            exact = compare({entry.data, entry.bytes}, truncated);
        }
        // Byte damage: bit flips, stuffed and unstuffed 0xFF, fill bytes and restart markers.
        for (u32 round = 0; round < 160 && exact; ++round) {
            copy_bytes(mutated, entry.data, entry.bytes);
            u8* entropy = mutated + image.entropy_offset;
            const u32 position = random.below(image.entropy_bytes);
            switch (round % 5) {
            case 0: entropy[position] ^= static_cast<u8>(1u << random.below(8)); break;
            case 1: entropy[position] = 0xFF; break;
            case 2:
                if (position + 1 < image.entropy_bytes) {
                    entropy[position] = 0xFF;
                    entropy[position + 1] = static_cast<u8>(0xD0u + random.below(8));
                }
                break;
            case 3:
                if (position + 2 < image.entropy_bytes) {
                    entropy[position] = 0xFF;
                    entropy[position + 1] = 0xFF;
                    entropy[position + 2] = 0x00;
                }
                break;
            default: entropy[position] = static_cast<u8>(random.below(256)); break;
            }
            exact = compare({mutated, entry.bytes}, image);
        }
    }
    check(exact && images >= 6 && failures > cases / 2, "jpeg_entropy_matches_bit_serial_reference");
    VirtualFree(workspace, 0, MEM_RELEASE);
    VirtualFree(reference, 0, MEM_RELEASE);
    VirtualFree(mutated, 0, MEM_RELEASE);
}

} // namespace

void test_media_decode_reference(Check check) noexcept {
    test_png_filters(check);
    test_gif_lzw(check);
    test_jpeg_entropy(check);
}

} // namespace maicord

namespace maicord {
namespace {
u32 g_failures;
void report(bool passed, const char* message) noexcept {
    if (passed) {
        return;
    }
    ++g_failures;
    u32 length = 0;
    while (message[length]) {
        ++length;
    }
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, length, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_media_decode_reference_tests_entry() noexcept {
    maicord::test_media_decode_reference(maicord::report);
    ExitProcess(maicord::g_failures ? 1 : 0);
}
