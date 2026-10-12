#include "maicord/media/gif.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

#include <immintrin.h>

namespace maicord {
namespace {

struct PendingControl {
    u16 delay_cs;
    u8 transparent_index;
    u8 disposal;
    bool present;
    bool transparent;
};

// Where a frame's LZW sub-blocks start and end in the encoded file.
struct GifBitReader {
    const u8* bytes;
    u32 cursor;
    u32 end;
};

bool bytes_equal(const u8* bytes, const char* expected, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index) {
        if (bytes[index] != static_cast<u8>(expected[index])) {
            return false;
        }
    }
    return true;
}

GifParseResult fail(GifParseResult result, GifError error) noexcept {
    result.error = error;
    return result;
}

GifDecodeResult decode_fail(GifDecodeResult result, GifError error) noexcept {
    result.error = error;
    return result;
}

void initialize_image(GifImage* image, GifWorkspace* workspace) noexcept {
    *image = GifImage{};
    image->frames = workspace->frames;
    image->frame_capacity = kGifFrameCapacity;
}

bool palette_bounds(ConstSlice<u8> encoded, u32 offset, u32 count) noexcept {
    return count >= 2 && count <= 256 && offset <= encoded.count && count * 3u <= encoded.count - offset;
}

bool skip_subblocks(ConstSlice<u8> encoded, u32* cursor, GifParseResult* result,
                    u32* payload_bytes) noexcept {
    if (!cursor || !result) {
        return false;
    }
    u32 total = 0;
    for (;;) {
        if (*cursor >= encoded.count) {
            return false;
        }
        const u32 bytes = encoded[(*cursor)++];
        ++result->data_subblocks;
        if (bytes == 0) {
            if (payload_bytes) {
                *payload_bytes = total;
            }
            return true;
        }
        if (bytes > encoded.count - *cursor || bytes > kGifEncodedMaximumBytes - total) {
            return false;
        }
        total += bytes;
        *cursor += bytes;
    }
}

// Non-interlaced LZW. Every dictionary string is old's string plus the first
// byte of the string after it, and those two were written back to back, so an
// entry is just (position, length) of bytes already in the output: a code
// becomes one forward copy, as in LZ77. Every byte of an entry was
// range-checked against the palette when it was first written, so only root
// codes need the check.
struct GifFastReader {
    const u8* bytes;
    u32 cursor;
    u32 end;
    u32 block_remaining;
    u32 bit_count;
    u64 bits;
    bool exhausted;
};

__forceinline void fast_refill(GifFastReader* reader) noexcept {
    while (reader->bit_count <= 56 && !reader->exhausted) {
        if (reader->block_remaining == 0) {
            // A zero-size sub-block terminates the data.
            if (reader->cursor >= reader->end) {
                reader->exhausted = true;
                break;
            }
            const u32 size = reader->bytes[reader->cursor++];
            if (size == 0 || size > reader->end - reader->cursor) {
                reader->exhausted = true;
                break;
            }
            reader->block_remaining = size;
        }
        reader->bits |= static_cast<u64>(reader->bytes[reader->cursor++]) << reader->bit_count;
        reader->bit_count += 8;
        --reader->block_remaining;
    }
}

__forceinline void copy_string(u8* output, u32 position, u32 source, u32 length, u32 capacity) noexcept {
    u8* destination = output + position;
    const u8* from = output + source;
    // The source ends at or before the destination; whole 16-byte chunks may
    // write past the string into output that later codes overwrite.
    if (position - source >= 16 && position + length + 16 <= capacity) {
        u32 copied = 0;
        do {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + copied),
                             _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + copied)));
            copied += 16;
        } while (copied < length);
        return;
    }
    if (length <= 8 && position - source >= 8 && position + 8 <= capacity) {
        _mm_storel_epi64(reinterpret_cast<__m128i*>(destination),
                         _mm_loadl_epi64(reinterpret_cast<const __m128i*>(from)));
        return;
    }
    for (u32 index = 0; index < length; ++index) {
        destination[index] = from[index];
    }
}

GifError decode_lzw_linear(GifBitReader start, const GifFrame& frame, u32 palette_count, Slice<u8> indices,
                           u32 pixels) noexcept {
    GifFastReader reader{start.bytes, start.cursor, start.end, 0, 0, 0, false};
    u32 positions[kGifLzwDictionaryCapacity];
    u16 lengths[kGifLzwDictionaryCapacity];
    const u32 clear_code = 1u << frame.minimum_code_size;
    const u32 end_code = clear_code + 1u;
    u32 next_code = end_code + 1u;
    u32 code_size = frame.minimum_code_size + 1u;
    u32 old_position = 0;
    u32 old_length = 0;
    u32 output_count = 0;
    bool have_old = false;
    bool first_code = true;
    u8* const output = indices.data;
    const u32 capacity = indices.count;
    for (;;) {
        if (reader.bit_count < code_size) {
            fast_refill(&reader);
            if (reader.bit_count < code_size) {
                return GifError::lzw_missing_end;
            }
        }
        u32 code = static_cast<u32>(reader.bits) & ((1u << code_size) - 1u);
        reader.bits >>= code_size;
        reader.bit_count -= code_size;
        if (first_code && code != clear_code) {
            return GifError::lzw_missing_clear;
        }
        first_code = false;
        if (code == clear_code) {
            next_code = end_code + 1u;
            code_size = frame.minimum_code_size + 1u;
            have_old = false;
            continue;
        }
        if (code == end_code) {
            return output_count == pixels ? GifError::none : GifError::lzw_pixel_count;
        }
        if (!have_old) {
            if (code >= clear_code || output_count >= pixels || code >= palette_count) {
                return GifError::lzw_invalid_code;
            }
            output[output_count] = static_cast<u8>(code);
                old_position = output_count++;
            old_length = 1;
            have_old = true;
            continue;
        }
        u32 length = 1;
        if (code < clear_code) {
            if (output_count >= pixels || code >= palette_count) {
                return GifError::lzw_pixel_count;
            }
            output[output_count] = static_cast<u8>(code);
        } else if (code < next_code) {
            length = lengths[code];
            if (length > pixels - output_count) {
                return GifError::lzw_pixel_count;
            }
            copy_string(output, output_count, positions[code], length, capacity);
        } else if (code == next_code) {
            // The entry being defined: old's string plus its own first byte.
            length = old_length + 1u;
            if (length > pixels - output_count) {
                return GifError::lzw_pixel_count;
            }
            copy_string(output, output_count, old_position, old_length, capacity);
            output[output_count + old_length] = output[old_position];
        } else {
            return GifError::lzw_invalid_code;
        }
        if (next_code < kGifLzwDictionaryCapacity) {
            positions[next_code] = old_position;
            lengths[next_code] = static_cast<u16>(old_length + 1u);
            ++next_code;
            if (next_code == (1u << code_size) && code_size < 12) {
                ++code_size;
            }
        }
        old_position = output_count;
        old_length = length;
        output_count += length;
    }
}

// Moves stream rows (pass order) to their image rows in place, following each
// permutation cycle with one row of scratch.
void deinterlace_rows(u8* indices, u32 width, u32 height) noexcept {
    u16 source_of[kGifDimensionMaximum];
    u32 stream = 0;
    constexpr u32 starts[4] = {0, 4, 2, 1};
    constexpr u32 steps[4] = {8, 8, 4, 2};
    for (u32 pass = 0; pass < 4; ++pass) {
        for (u32 row = starts[pass]; row < height; row += steps[pass]) {
            source_of[row] = static_cast<u16>(stream++);
        }
    }
    u8 carried[kGifDimensionMaximum];
    u8 placed[kGifDimensionMaximum / 8]{};
    for (u32 first = 0; first < height; ++first) {
        if ((placed[first >> 3] >> (first & 7u) & 1u) != 0 || source_of[first] == first) {
            continue;
        }
        copy_bytes(carried, indices + u64{first} * width, width);
        u32 row = first;
        for (;;) {
            placed[row >> 3] = static_cast<u8>(placed[row >> 3] | 1u << (row & 7u));
            const u32 source = source_of[row];
            if (source == first) {
                copy_bytes(indices + u64{row} * width, carried, width);
                break;
            }
            copy_bytes(indices + u64{row} * width, indices + u64{source} * width, width);
            row = source;
        }
    }
}

bool read_palette(ConstSlice<u8> encoded, u32 offset, u32 count, GifPalette* palette) noexcept {
    if (!palette || !palette_bounds(encoded, offset, count)) {
        return false;
    }
    *palette = GifPalette{};
    palette->count = static_cast<u16>(count);
    for (u32 index = 0; index < count; ++index) {
        const u8* color = encoded.data + offset + index * 3u;
        palette->rgba[index] = static_cast<u32>(color[0]) | static_cast<u32>(color[1]) << 8 |
                               static_cast<u32>(color[2]) << 16 | 0xFF000000u;
    }
    return true;
}

} // namespace

GifParseResult gif_parse(ConstSlice<u8> encoded, GifImage* image, GifWorkspace* workspace) noexcept {
    GifParseResult result{};
    if (!image || !workspace || (!encoded.data && encoded.count != 0)) {
        return fail(result, GifError::invalid_input);
    }
    initialize_image(image, workspace);
    if (encoded.count > kGifEncodedMaximumBytes) {
        return fail(result, GifError::encoded_limit);
    }
    if (encoded.count < 13) {
        return fail(result, GifError::truncated);
    }
    if (!bytes_equal(encoded.data, "GIF", 3)) {
        return fail(result, GifError::signature);
    }
    const bool version_87a = bytes_equal(encoded.data + 3, "87a", 3);
    const bool version_89a = bytes_equal(encoded.data + 3, "89a", 3);
    if (!version_87a && !version_89a) {
        return fail(result, GifError::version);
    }

    image->canvas_width = byte_load_le16(encoded.data + 6);
    image->canvas_height = byte_load_le16(encoded.data + 8);
    if (image->canvas_width == 0 || image->canvas_height == 0 ||
        image->canvas_width > kGifDimensionMaximum || image->canvas_height > kGifDimensionMaximum ||
        image->canvas_width * image->canvas_height > kGifPixelMaximum) {
        return fail(result, GifError::dimensions);
    }
    const u8 screen_packed = encoded[10];
    image->background_index = encoded[11];
    u32 cursor = 13;
    if ((screen_packed & 0x80u) != 0) {
        const u32 count = 2u << (screen_packed & 7u);
        if (!palette_bounds(encoded, cursor, count)) {
            return fail(result, GifError::palette);
        }
        image->has_global_palette = true;
        image->global_palette_offset = cursor;
        image->global_palette_count = static_cast<u16>(count);
        if (image->background_index >= count) {
            return fail(result, GifError::palette);
        }
        const u8* background = encoded.data + cursor + image->background_index * 3u;
        image->background_rgba = static_cast<u32>(background[0]) | static_cast<u32>(background[1]) << 8 |
                                 static_cast<u32>(background[2]) << 16 | 0xFF000000u;
        cursor += count * 3u;
    }

    PendingControl pending{};
    bool trailer_seen = false;
    bool loop_seen = false;
    while (cursor < encoded.count) {
        const u8 block = encoded[cursor++];
        ++result.blocks;
        if (block == 0x3Bu) {
            if (pending.present || image->frame_count == 0 || cursor != encoded.count) {
                return fail(result, GifError::block_order);
            }
            trailer_seen = true;
            break;
        }
        if (block == 0x21u) {
            if (version_87a || cursor >= encoded.count) {
                return fail(result, version_87a ? GifError::version : GifError::truncated);
            }
            ++result.extensions;
            const u8 label = encoded[cursor++];
            if (label == 0xF9u) {
                if (pending.present || encoded.count - cursor < 6 || encoded[cursor] != 4) {
                    return fail(result, GifError::graphic_control);
                }
                const u8 packed = encoded[cursor + 1];
                const u8 disposal = static_cast<u8>((packed >> 2) & 7u);
                if ((packed & 0xE0u) != 0 || disposal > 4 || encoded[cursor + 5] != 0) {
                    return fail(result, (packed & 0xE0u) != 0 ? GifError::reserved_bits
                                                              : GifError::graphic_control);
                }
                pending.delay_cs = byte_load_le16(encoded.data + cursor + 2);
                pending.transparent_index = encoded[cursor + 4];
                pending.disposal = disposal == 4 ? 3 : disposal;
                pending.transparent = (packed & 1u) != 0;
                pending.present = true;
                cursor += 6;
                ++result.data_subblocks;
                continue;
            }
            if (label == 0xFFu) {
                if (cursor >= encoded.count) {
                    return fail(result, GifError::truncated);
                }
                const u32 header_bytes = encoded[cursor++];
                if (header_bytes != 11 || header_bytes > encoded.count - cursor) {
                    return fail(result, GifError::application_extension);
                }
                const bool netscape = bytes_equal(encoded.data + cursor, "NETSCAPE2.0", 11) ||
                                      bytes_equal(encoded.data + cursor, "ANIMEXTS1.0", 11);
                cursor += header_bytes;
                if (netscape) {
                    for (;;) {
                        if (cursor >= encoded.count) {
                            return fail(result, GifError::truncated);
                        }
                        const u32 bytes = encoded[cursor++];
                        ++result.data_subblocks;
                        if (bytes == 0) {
                            break;
                        }
                        if (bytes > encoded.count - cursor) {
                            return fail(result, GifError::truncated);
                        }
                        const u8 identifier = encoded[cursor] & 7u;
                        if (identifier == 1) {
                            if (bytes < 3 || loop_seen) {
                                return fail(result, loop_seen ? GifError::duplicate_loop_extension
                                                              : GifError::application_extension);
                            }
                            const u32 extra_plays = byte_load_le16(encoded.data + cursor + 1);
                            image->play_count = extra_plays == 0 ? 0 : extra_plays + 1u;
                            loop_seen = true;
                        } else if (identifier != 2) {
                            return fail(result, GifError::application_extension);
                        }
                        cursor += bytes;
                    }
                } else if (!skip_subblocks(encoded, &cursor, &result, nullptr)) {
                    return fail(result, GifError::truncated);
                }
                continue;
            }
            if (label == 0xFEu) {
                if (!skip_subblocks(encoded, &cursor, &result, nullptr)) {
                    return fail(result, GifError::truncated);
                }
                continue;
            }
            if (label == 0x01u) {
                if (cursor >= encoded.count || encoded[cursor++] != 12 || encoded.count - cursor < 12) {
                    return fail(result, GifError::block);
                }
                cursor += 12;
                if (!skip_subblocks(encoded, &cursor, &result, nullptr)) {
                    return fail(result, GifError::truncated);
                }
                pending = PendingControl{};
                continue;
            }
            return fail(result, GifError::block);
        }
        if (block != 0x2Cu) {
            return fail(result, GifError::block);
        }
        if (image->frame_count >= image->frame_capacity || encoded.count - cursor < 9) {
            return fail(result, image->frame_count >= image->frame_capacity ? GifError::frame_limit
                                                                            : GifError::truncated);
        }
        GifFrame frame{};
        frame.x_offset = byte_load_le16(encoded.data + cursor);
        frame.y_offset = byte_load_le16(encoded.data + cursor + 2);
        frame.width = byte_load_le16(encoded.data + cursor + 4);
        frame.height = byte_load_le16(encoded.data + cursor + 6);
        const u8 packed = encoded[cursor + 8];
        cursor += 9;
        if ((packed & 0x18u) != 0) {
            return fail(result, GifError::reserved_bits);
        }
        if (frame.width == 0 || frame.height == 0 || frame.x_offset > image->canvas_width ||
            frame.y_offset > image->canvas_height || frame.width > image->canvas_width - frame.x_offset ||
            frame.height > image->canvas_height - frame.y_offset ||
            frame.width * frame.height > kGifPixelMaximum) {
            return fail(result, GifError::frame_rectangle);
        }
        if ((packed & 0x40u) != 0) {
            frame.flags |= gif_frame_interlaced;
        }
        if ((packed & 0x80u) != 0) {
            const u32 count = 2u << (packed & 7u);
            if (!palette_bounds(encoded, cursor, count)) {
                return fail(result, GifError::palette);
            }
            frame.flags |= gif_frame_local_palette;
            frame.palette_offset = cursor;
            frame.palette_count = static_cast<u16>(count);
            cursor += count * 3u;
        } else {
            if (!image->has_global_palette) {
                return fail(result, GifError::palette);
            }
            frame.palette_offset = image->global_palette_offset;
            frame.palette_count = image->global_palette_count;
        }
        if (pending.transparent) {
            if (pending.transparent_index >= frame.palette_count) {
                return fail(result, GifError::graphic_control);
            }
            frame.flags |= gif_frame_transparent;
            frame.transparent_index = pending.transparent_index;
        }
        frame.delay_cs = pending.delay_cs;
        frame.dispose = pending.disposal == 2   ? GifDispose::background
                        : pending.disposal == 3 ? GifDispose::previous
                                                : GifDispose::none;
        if (image->frame_count == 0 && frame.dispose == GifDispose::previous) {
            frame.dispose = GifDispose::background;
        }
        pending = PendingControl{};
        if (cursor >= encoded.count) {
            return fail(result, GifError::truncated);
        }
        frame.minimum_code_size = encoded[cursor++];
        if (frame.minimum_code_size < 2 || frame.minimum_code_size > 8) {
            return fail(result, GifError::lzw_code_size);
        }
        frame.data_offset = cursor;
        if (!skip_subblocks(encoded, &cursor, &result, &frame.compressed_bytes)) {
            return fail(result, GifError::truncated);
        }
        if (frame.compressed_bytes == 0) {
            return fail(result, GifError::image_data);
        }
        frame.data_end = cursor;
        image->frames[image->frame_count++] = frame;
    }
    if (!trailer_seen) {
        return fail(result, GifError::truncated);
    }
    if (!loop_seen) {
        image->play_count = 1;
    }
    if (image->has_global_palette && image->frame_count != 0 &&
        (image->frames[0].flags & gif_frame_transparent) != 0 &&
        image->frames[0].transparent_index == image->background_index) {
        image->background_rgba = 0;
    }
    return result;
}

u64 gif_frame_duration_us(const GifFrame& frame) noexcept {
    const u64 source = static_cast<u64>(frame.delay_cs) * 10'000ull;
    return source <= kGifShortFrameThresholdUs ? kGifShortFrameReplacementUs : source;
}

GifDecodeResult gif_decode_parsed_frame(ConstSlice<u8> encoded, const GifImage& image, u32 frame_index,
                                        GifWorkspace* workspace, Slice<u8> indices) noexcept {
    GifDecodeResult result{};
    result.frame_index = frame_index;
    if (!workspace || image.frames != workspace->frames || (!encoded.data && encoded.count != 0) ||
        (!indices.data && indices.count != 0) || encoded.count > kGifEncodedMaximumBytes ||
        frame_index >= image.frame_count || image.frame_count > image.frame_capacity ||
        image.frame_capacity > kGifFrameCapacity) {
        return decode_fail(result, GifError::invalid_input);
    }
    const GifFrame frame = image.frames[frame_index];
    result.frame = frame;
    result.duration_us = gif_frame_duration_us(frame);
    const u32 pixels = frame.width * frame.height;
    result.pixels = pixels;
    if (frame.width == 0 || frame.height == 0 || frame.width > kGifDimensionMaximum ||
        frame.height > kGifDimensionMaximum || pixels > kGifPixelMaximum || indices.count < pixels) {
        return decode_fail(result,
                           indices.count < pixels ? GifError::output_capacity : GifError::frame_rectangle);
    }
    if (!read_palette(encoded, frame.palette_offset, frame.palette_count, &result.palette)) {
        return decode_fail(result, GifError::palette);
    }
    result.palette.background_rgba = image.background_rgba;
    if ((frame.flags & gif_frame_transparent) != 0) {
        if (frame.transparent_index >= result.palette.count) {
            return decode_fail(result, GifError::palette);
        }
        result.palette.has_transparency = true;
        result.palette.transparent_index = frame.transparent_index;
        result.palette.rgba[frame.transparent_index] &= 0x00FFFFFFu;
    }
    if (frame.minimum_code_size < 2 || frame.minimum_code_size > 8 || frame.data_offset >= frame.data_end ||
        frame.data_end > encoded.count) {
        return decode_fail(result, GifError::image_data);
    }

    const GifBitReader reader{encoded.data, frame.data_offset, frame.data_end};
    const GifError error = decode_lzw_linear(reader, frame, result.palette.count, indices, pixels);
    if (error != GifError::none) {
        return decode_fail(result, error);
    }
    // Interlaced rows arrive in pass order (every 8th row from 0, every 8th
    // from 4, every 4th from 2, every 2nd from 1); put them in place last.
    if ((frame.flags & gif_frame_interlaced) != 0) {
        deinterlace_rows(indices.data, frame.width, frame.height);
    }
    return result;
}

GifDecodeResult gif_decode_frame(ConstSlice<u8> encoded, u32 frame_index, GifImage* image,
                                 GifWorkspace* workspace, Slice<u8> indices) noexcept {
    GifDecodeResult result{};
    const GifParseResult parsed = gif_parse(encoded, image, workspace);
    result.parse = parsed;
    if (!parsed.ok()) {
        result.error = parsed.error;
        return result;
    }
    result = gif_decode_parsed_frame(encoded, *image, frame_index, workspace, indices);
    result.parse = parsed;
    return result;
}

} // namespace maicord
