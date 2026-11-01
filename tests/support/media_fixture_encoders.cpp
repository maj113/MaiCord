#include "media_fixture_internal.hpp"

namespace maicord {
namespace {

u32 crc32(const u8* bytes, u32 count) noexcept {
    constexpr u32 table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    u32 crc = 0xFFFFFFFFu;
    for (u32 index = 0; index < count; ++index) {
        crc = crc >> 4 ^ table[(crc ^ bytes[index]) & 0xFu];
        crc = crc >> 4 ^ table[(crc ^ (bytes[index] >> 4)) & 0xFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace

namespace media_fixture_detail {

u32 adler32(const u8* bytes, u32 count) noexcept {
    u64 first = 1;
    u64 second = 0;
    for (u32 index = 0; index < count; ++index) {
        first += bytes[index];
        second += first;
    }
    first %= 65'521;
    second %= 65'521;
    return static_cast<u32>(second << 16 | first);
}

void write_be32(u8* output, u32 value) noexcept {
    output[0] = static_cast<u8>(value >> 24);
    output[1] = static_cast<u8>(value >> 16);
    output[2] = static_cast<u8>(value >> 8);
    output[3] = static_cast<u8>(value);
}

void write_be16(u8* output, u16 value) noexcept {
    output[0] = static_cast<u8>(value >> 8);
    output[1] = static_cast<u8>(value);
}

void write_le24(u8* output, u32 value) noexcept {
    output[0] = static_cast<u8>(value);
    output[1] = static_cast<u8>(value >> 8);
    output[2] = static_cast<u8>(value >> 16);
}

void write_le32(u8* output, u32 value) noexcept {
    write_le24(output, value);
    output[3] = static_cast<u8>(value >> 24);
}

bool append_riff_chunk(Slice<u8> output, u32* cursor, u32 type, ConstSlice<u8> payload) noexcept {
    if (!cursor || (!output.data && output.count != 0) || (!payload.data && payload.count != 0)) {
        return false;
    }
    const u32 padding = payload.count & 1u;
    const u64 required = static_cast<u64>(*cursor) + 8u + payload.count + padding;
    if (required > output.count) {
        return false;
    }
    write_le32(output.data + *cursor, type);
    write_le32(output.data + *cursor + 4, payload.count);
    for (u32 index = 0; index < payload.count; ++index) {
        output[*cursor + 8 + index] = payload[index];
    }
    if (padding != 0) {
        output[*cursor + 8 + payload.count] = 0;
    }
    *cursor = static_cast<u32>(required);
    return true;
}

bool begin_animated_webp(Slice<u8> encoded, u32* cursor, const u8 (&anim)[6]) noexcept {
    write_le32(encoded.data, 0x46464952u);
    write_le32(encoded.data + 8, 0x50424557u);
    *cursor = 12;
    u8 vp8x[10]{};
    vp8x[0] = 0x12;
    write_le24(vp8x + 4, 3);
    write_le24(vp8x + 7, 3);
    return append_riff_chunk(encoded, cursor, 0x58385056u, ConstSlice<u8>{vp8x, sizeof(vp8x)}) &&
           append_riff_chunk(encoded, cursor, 0x4D494E41u, ConstSlice<u8>{anim, sizeof(anim)});
}

void fill_unit_jpeg_tables(u8 (&quantization)[65], u8 (&huffman)[37]) noexcept {
    for (u32 index = 1; index < sizeof(quantization); ++index) {
        quantization[index] = 1;
    }
    // DC: '0' no change, '10' a nine-bit difference; AC: '0' end of block.
    huffman[1] = 1;
    huffman[2] = 1;
    huffman[18] = 9;
    huffman[19] = 0x10;
    huffman[20] = 1;
}

bool append_jpeg_segment(Slice<u8> output, u32* cursor, u8 marker, ConstSlice<u8> payload) noexcept {
    if (!cursor || payload.count > 65'533u ||
        static_cast<u64>(*cursor) + payload.count + 4u > output.count) {
        return false;
    }
    output[*cursor] = 0xFF;
    output[*cursor + 1] = marker;
    const u16 length = static_cast<u16>(payload.count + 2u);
    output[*cursor + 2] = static_cast<u8>(length >> 8);
    output[*cursor + 3] = static_cast<u8>(length);
    for (u32 index = 0; index < payload.count; ++index) {
        output[*cursor + 4 + index] = payload[index];
    }
    *cursor += payload.count + 4u;
    return true;
}

bool append_chunk(Slice<u8> output, u32* cursor, u32 type, ConstSlice<u8> data) noexcept {
    if (!cursor) {
        return false;
    }
    const u64 required = static_cast<u64>(*cursor) + 12 + data.count;
    if (required > output.count) {
        return false;
    }
    u8* destination = output.data + *cursor;
    write_be32(destination, data.count);
    write_be32(destination + 4, type);
    for (u32 index = 0; index < data.count; ++index) {
        destination[8 + index] = data[index];
    }
    write_be32(destination + 8 + data.count, crc32(destination + 4, data.count + 4));
    *cursor += data.count + 12;
    return true;
}

bool begin_png(Slice<u8> encoded, u32* cursor, u32 width, u32 height, u8 bit_depth,
               u8 color_type) noexcept {
    constexpr u8 signature[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    if (encoded.count < sizeof(signature)) {
        return false;
    }
    for (u32 index = 0; index < sizeof(signature); ++index) {
        encoded[index] = signature[index];
    }
    *cursor = sizeof(signature);
    u8 header[13]{};
    write_be32(header, width);
    write_be32(header + 4, height);
    header[8] = bit_depth;
    header[9] = color_type;
    return append_chunk(encoded, cursor, 0x49484452u, ConstSlice<u8>{header, sizeof(header)});
}

bool begin_apng(Slice<u8> encoded, u32* cursor, u32 frames, u32 plays) noexcept {
    u8 animation[8]{};
    write_be32(animation, frames);
    write_be32(animation + 4, plays);
    return begin_png(encoded, cursor, 4, 4, 8, 6) &&
           append_chunk(encoded, cursor, 0x6163544Cu, ConstSlice<u8>{animation, sizeof(animation)});
}

void write_stored_zlib_header(u8* output, u32 filtered_bytes) noexcept {
    const u16 inverse = static_cast<u16>(filtered_bytes ^ 0xFFFFu);
    const u8 header[7] = {
        0x78,
        0x01,
        0x01,
        static_cast<u8>(filtered_bytes),
        static_cast<u8>(filtered_bytes >> 8),
        static_cast<u8>(inverse),
        static_cast<u8>(inverse >> 8),
    };
    for (u32 index = 0; index < sizeof(header); ++index) {
        output[index] = header[index];
    }
}

u8 channel_byte(u32 x_byte, u32 y, u32 width, u32 height, PngFixturePixel pixel) noexcept {
    const u32 pixel_x = x_byte >> 2;
    const u32 component = x_byte & 3u;
    const u32 rgba = pixel ? pixel(pixel_x, y, width, height) : png_fixture_rgba(pixel_x, y, width, height);
    return static_cast<u8>(rgba >> (component * 8));
}

u8 paeth(u8 left, u8 above, u8 upper_left) noexcept {
    const i32 a = left;
    const i32 b = above;
    const i32 c = upper_left;
    const i32 prediction = a + b - c;
    const i32 distance_a = prediction >= a ? prediction - a : a - prediction;
    const i32 distance_b = prediction >= b ? prediction - b : b - prediction;
    const i32 distance_c = prediction >= c ? prediction - c : c - prediction;
    return distance_a <= distance_b && distance_a <= distance_c ? left
           : distance_b <= distance_c                           ? above
                                                                : upper_left;
}

} // namespace media_fixture_detail

namespace {

u32 sample_maximum(u8 bit_depth) noexcept {
    return bit_depth == 16 ? 65'535u : (1u << bit_depth) - 1u;
}

} // namespace

namespace media_fixture_detail {

u32 format_sample(u8 bit_depth, u32 channel, u32 x, u32 y) noexcept {
    const u32 seed = x * (13u + channel * 7u) + y * (17u + channel * 11u) + (channel + 1u) * 37u;
    return bit_depth == 16 ? (seed * 257u + x * 31u + channel * 13u) & 65'535u
                           : seed & sample_maximum(bit_depth);
}

} // namespace media_fixture_detail

namespace {

u8 expand_sample(u32 sample, u8 bit_depth) noexcept {
    const u32 maximum_sample = sample_maximum(bit_depth);
    return static_cast<u8>((sample * 255u + maximum_sample / 2u) / maximum_sample);
}

} // namespace

namespace media_fixture_detail {

u32 indexed_palette_count(u8 bit_depth) noexcept {
    return bit_depth == 8 ? 17u : 1u << bit_depth;
}

u32 indexed_palette_rgba(u32 index) noexcept {
    const u8 red = static_cast<u8>(17u + index * 11u);
    const u8 green = static_cast<u8>(231u - index * 7u);
    const u8 blue = static_cast<u8>(43u + index * 13u);
    const u8 alpha = index % 3u == 0 ? 0u : index % 3u == 1 ? 127u : 255u;
    return static_cast<u32>(red) | static_cast<u32>(green) << 8 | static_cast<u32>(blue) << 16 |
           static_cast<u32>(alpha) << 24;
}

bool format_supported(u8 color_type, u8 bit_depth, u32* channels) noexcept {
    if (!channels) {
        return false;
    }
    switch (color_type) {
    case 0:
        *channels = 1;
        return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8 || bit_depth == 16;
    case 2:
        *channels = 3;
        return bit_depth == 8 || bit_depth == 16;
    case 3:
        *channels = 1;
        return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8;
    case 4:
        *channels = 2;
        return bit_depth == 8 || bit_depth == 16;
    case 6:
        *channels = 4;
        return bit_depth == 8 || bit_depth == 16;
    default:
        return false;
    }
}

u32 premultiply_rgba(u32 rgba) noexcept {
    const u32 alpha = rgba >> 24;
    const u32 red = ((rgba & 255u) * alpha + 127u) / 255u;
    const u32 green = (((rgba >> 8) & 255u) * alpha + 127u) / 255u;
    const u32 blue = (((rgba >> 16) & 255u) * alpha + 127u) / 255u;
    return red | green << 8 | blue << 16 | alpha << 24;
}

u32 over_rgba(u32 source, u32 destination) noexcept {
    source = premultiply_rgba(source);
    const u32 source_alpha = source >> 24;
    const u32 inverse = 255u - source_alpha;
    u32 output = 0;
    for (u32 channel = 0; channel < 4; ++channel) {
        const u32 source_value = source >> (channel * 8) & 255u;
        const u32 destination_value = destination >> (channel * 8) & 255u;
        const u32 value = minimum<u32>(255, source_value + (destination_value * inverse + 127u) / 255u);
        output |= value << (channel * 8);
    }
    return output;
}

bool build_apng_zlib(u32 frame, u16 width, u16 height, Slice<u8> output, u32* bytes) noexcept {
    if (!output.data || !bytes) {
        return false;
    }
    const u32 filtered_bytes = (static_cast<u32>(width) * 4u + 1u) * height;
    const u32 required = 2u + 5u + filtered_bytes + 4u;
    if (filtered_bytes > 65'535 || required > output.count) {
        return false;
    }
    write_stored_zlib_header(output.data, filtered_bytes);
    u8* filtered = output.data + 7;
    for (u32 y = 0; y < height; ++y) {
        u8* row = filtered + y * (static_cast<u32>(width) * 4u + 1u);
        row[0] = 0;
        for (u32 x = 0; x < width; ++x) {
            const u32 rgba = apng_fixture_source_rgba(frame, x, y);
            row[1 + x * 4] = static_cast<u8>(rgba);
            row[2 + x * 4] = static_cast<u8>(rgba >> 8);
            row[3 + x * 4] = static_cast<u8>(rgba >> 16);
            row[4 + x * 4] = static_cast<u8>(rgba >> 24);
        }
    }
    write_be32(filtered + filtered_bytes, adler32(filtered, filtered_bytes));
    *bytes = required;
    return true;
}

bool append_fctl(Slice<u8> output, u32* cursor, u32 sequence, u32 width, u32 height, u32 x, u32 y,
                 u16 delay_num, u16 delay_den, u8 dispose, u8 blend) noexcept {
    u8 control[26]{};
    write_be32(control, sequence);
    write_be32(control + 4, width);
    write_be32(control + 8, height);
    write_be32(control + 12, x);
    write_be32(control + 16, y);
    control[20] = static_cast<u8>(delay_num >> 8);
    control[21] = static_cast<u8>(delay_num);
    control[22] = static_cast<u8>(delay_den >> 8);
    control[23] = static_cast<u8>(delay_den);
    control[24] = dispose;
    control[25] = blend;
    return append_chunk(output, cursor, 0x6663544Cu, ConstSlice<u8>{control, sizeof(control)});
}

bool append_fdat(Slice<u8> output, u32* cursor, u32 sequence, ConstSlice<u8> zlib) noexcept {
    u8 data[256]{};
    if (zlib.count + 4u > sizeof(data)) {
        return false;
    }
    write_be32(data, sequence);
    for (u32 index = 0; index < zlib.count; ++index) {
        data[4 + index] = zlib[index];
    }
    return append_chunk(output, cursor, 0x66644154u, ConstSlice<u8>{data, zlib.count + 4u});
}

bool append_bytes(Slice<u8> output, u32* cursor, ConstSlice<u8> bytes) noexcept {
    if (!cursor || *cursor > output.count || bytes.count > output.count - *cursor) {
        return false;
    }
    for (u32 index = 0; index < bytes.count; ++index) {
        output[*cursor + index] = bytes[index];
    }
    *cursor += bytes.count;
    return true;
}

bool gif_pack_code(Slice<u8> output, u32* byte_count, u32* bits, u32* bit_count, u32 code,
                   u32 code_size) noexcept {
    if (!byte_count || !bits || !bit_count || code_size > 12 || code >= (1u << code_size)) {
        return false;
    }
    *bits |= code << *bit_count;
    *bit_count += code_size;
    while (*bit_count >= 8) {
        if (*byte_count >= output.count) {
            return false;
        }
        output[(*byte_count)++] = static_cast<u8>(*bits);
        *bits >>= 8;
        *bit_count -= 8;
    }
    return true;
}

} // namespace media_fixture_detail

namespace {

u32 gif_interlaced_row(u32 stream_row, u32 height) noexcept {
    constexpr u32 starts[4] = {0, 4, 2, 1};
    constexpr u32 steps[4] = {8, 8, 4, 2};
    for (u32 pass = 0; pass < 4; ++pass) {
        if (starts[pass] >= height) {
            continue;
        }
        const u32 rows = (height - 1u - starts[pass]) / steps[pass] + 1u;
        if (stream_row < rows) {
            return starts[pass] + stream_row * steps[pass];
        }
        stream_row -= rows;
    }
    return height;
}

bool build_gif_frame_lzw(u32 frame, u32 width, u32 height, bool interlaced, Slice<u8> output,
                         u32* bytes) noexcept {
    if (!bytes) {
        return false;
    }
    u32 output_bytes = 0;
    u32 bits = 0;
    u32 bit_count = 0;
    constexpr u32 clear_code = 4;
    constexpr u32 end_code = 5;
    for (u32 stream_y = 0; stream_y < height; ++stream_y) {
        const u32 y = interlaced ? gif_interlaced_row(stream_y, height) : stream_y;
        for (u32 x = 0; x < width; ++x) {
            if (!gif_pack_code(output, &output_bytes, &bits, &bit_count, clear_code, 3) ||
                !gif_pack_code(output, &output_bytes, &bits, &bit_count,
                               gif_fixture_source_index(frame, x, y), 3)) {
                return false;
            }
        }
    }
    if (!gif_pack_code(output, &output_bytes, &bits, &bit_count, end_code, 3)) {
        return false;
    }
    if (bit_count != 0) {
        if (output_bytes >= output.count) {
            return false;
        }
        output[output_bytes++] = static_cast<u8>(bits);
    }
    *bytes = output_bytes;
    return true;
}

bool append_gif_image_data(Slice<u8> output, u32* cursor, ConstSlice<u8> lzw, bool split) noexcept {
    if (!cursor || lzw.count == 0 || lzw.count > 510) {
        return false;
    }
    const u8 minimum_code_size = 2;
    if (!append_bytes(output, cursor, ConstSlice<u8>{&minimum_code_size, 1})) {
        return false;
    }
    const u32 first = split ? lzw.count / 2u : lzw.count;
    const u8 first_size = static_cast<u8>(first);
    if (!append_bytes(output, cursor, ConstSlice<u8>{&first_size, 1}) ||
        !append_bytes(output, cursor, ConstSlice<u8>{lzw.data, first})) {
        return false;
    }
    if (first != lzw.count) {
        const u8 second_size = static_cast<u8>(lzw.count - first);
        if (!append_bytes(output, cursor, ConstSlice<u8>{&second_size, 1}) ||
            !append_bytes(output, cursor, ConstSlice<u8>{lzw.data + first, lzw.count - first})) {
            return false;
        }
    }
    const u8 terminator = 0;
    return append_bytes(output, cursor, ConstSlice<u8>{&terminator, 1});
}

bool append_gif_control(Slice<u8> output, u32* cursor, u8 disposal, u16 delay_cs, bool transparent,
                        u8 transparent_index) noexcept {
    u8 control[8] = {
        0x21,
        0xF9,
        0x04,
        static_cast<u8>(disposal << 2 | static_cast<u8>(transparent)),
        static_cast<u8>(delay_cs),
        static_cast<u8>(delay_cs >> 8),
        transparent_index,
        0,
    };
    return append_bytes(output, cursor, ConstSlice<u8>{control, sizeof(control)});
}

} // namespace

namespace media_fixture_detail {

bool append_gif_descriptor(Slice<u8> output, u32* cursor, u16 x, u16 y, u16 width, u16 height,
                           u8 packed) noexcept {
    u8 descriptor[10] = {
        0x2C,
        static_cast<u8>(x),
        static_cast<u8>(x >> 8),
        static_cast<u8>(y),
        static_cast<u8>(y >> 8),
        static_cast<u8>(width),
        static_cast<u8>(width >> 8),
        static_cast<u8>(height),
        static_cast<u8>(height >> 8),
        packed,
    };
    return append_bytes(output, cursor, ConstSlice<u8>{descriptor, sizeof(descriptor)});
}

} // namespace media_fixture_detail

namespace {

bool append_gif_palette(Slice<u8> output, u32* cursor, u32 frame) noexcept {
    u8 palette[12]{};
    for (u32 index = 0; index < 4; ++index) {
        const u32 rgba = gif_fixture_palette_rgba(frame, index);
        palette[index * 3] = static_cast<u8>(rgba);
        palette[index * 3 + 1] = static_cast<u8>(rgba >> 8);
        palette[index * 3 + 2] = static_cast<u8>(rgba >> 16);
    }
    return append_bytes(output, cursor, ConstSlice<u8>{palette, sizeof(palette)});
}

} // namespace

namespace media_fixture_detail {

bool append_deferred_lzw(Slice<u8> output, u32* cursor, ConstSlice<u8> lzw) noexcept {
    if (!cursor) {
        return false;
    }
    const u8 minimum_code_size = 2;
    if (!append_bytes(output, cursor, ConstSlice<u8>{&minimum_code_size, 1})) {
        return false;
    }
    u32 consumed = 0;
    while (consumed < lzw.count) {
        const u32 bytes = minimum<u32>(255, lzw.count - consumed);
        const u8 block_size = static_cast<u8>(bytes);
        if (!append_bytes(output, cursor, ConstSlice<u8>{&block_size, 1}) ||
            !append_bytes(output, cursor, ConstSlice<u8>{lzw.data + consumed, bytes})) {
            return false;
        }
        consumed += bytes;
    }
    const u8 terminator = 0;
    return append_bytes(output, cursor, ConstSlice<u8>{&terminator, 1});
}

bool jpeg_icc_insertion_point(ConstSlice<u8> source, Slice<u8> encoded, const JpegIccFixtureResult* result,
                              u32* insertion) noexcept {
    if (!source.data || source.count < 6 || !encoded.data || encoded.data == source.data || !result ||
        source[0] != 0xFF || source[1] != 0xD8) {
        return false;
    }
    *insertion = 2;
    if (source[2] == 0xFF && source[3] == 0xE0) {
        const u32 length = static_cast<u32>(source[4]) << 8u | source[5];
        if (length < 2 || 4u + length > source.count) {
            return false;
        }
        *insertion = 4u + length;
    }
    return true;
}

void write_icc_header(u8* profile, bool version_four, u32 device_class, u32 color_space, u32 pcs,
                      u32 tag_count) noexcept {
    profile[8] = version_four ? 4 : 2;
    profile[9] = version_four ? 0x30 : 0x10;
    write_be32(profile + 12, device_class);
    write_be32(profile + 16, color_space);
    write_be32(profile + 20, pcs);
    write_be32(profile + 36, 0x61637370u); // acsp
    write_be32(profile + 64, 1);
    write_be32(profile + 68, 0x0000F6D6u);
    write_be32(profile + 72, 0x00010000u);
    write_be32(profile + 76, 0x0000D32Du);
    write_be32(profile + 128, tag_count);
}

} // namespace media_fixture_detail

u32 png_fixture_crc32(ConstSlice<u8> bytes) noexcept {
    return bytes.data || bytes.count == 0 ? crc32(bytes.data, bytes.count) : 0;
}

u32 png_fixture_adler32(ConstSlice<u8> bytes) noexcept {
    return bytes.data || bytes.count == 0 ? adler32(bytes.data, bytes.count) : 0;
}

void png_fixture_write_be32(u8* output, u32 value) noexcept {
    if (output) {
        write_be32(output, value);
    }
}

u32 png_fixture_rgba(u32 x, u32 y, u32 width, u32 height) noexcept {
    const i32 dx = static_cast<i32>(x * 2 + 1) - static_cast<i32>(width);
    const i32 dy = static_cast<i32>(y * 2 + 1) - static_cast<i32>(height);
    const i32 radius = static_cast<i32>(minimum(width, height)) - 4;
    const u8 alpha = dx * dx + dy * dy <= radius * radius ? 255 : 0;
    const u8 red = static_cast<u8>(32 + x * 3);
    const u8 green = static_cast<u8>(48 + y * 3);
    const u8 blue = static_cast<u8>(224 - x * 2);
    return static_cast<u32>(red) | static_cast<u32>(green) << 8 | static_cast<u32>(blue) << 16 |
           static_cast<u32>(alpha) << 24;
}

u32 png_format_fixture_rgba(u8 color_type, u8 bit_depth, u32 x, u32 y) noexcept {
    u8 red = 0;
    u8 green = 0;
    u8 blue = 0;
    u8 alpha = 255;
    if (color_type == 0) {
        const u32 gray = format_sample(bit_depth, 0, x, y);
        red = green = blue = expand_sample(gray, bit_depth);
        alpha = gray == format_sample(bit_depth, 0, 0, 0) ? 0 : 255;
    } else if (color_type == 2) {
        const u32 samples[3] = {
            format_sample(bit_depth, 0, x, y),
            format_sample(bit_depth, 1, x, y),
            format_sample(bit_depth, 2, x, y),
        };
        red = expand_sample(samples[0], bit_depth);
        green = expand_sample(samples[1], bit_depth);
        blue = expand_sample(samples[2], bit_depth);
        alpha = samples[0] == format_sample(bit_depth, 0, 0, 0) &&
                        samples[1] == format_sample(bit_depth, 1, 0, 0) &&
                        samples[2] == format_sample(bit_depth, 2, 0, 0)
                    ? 0
                    : 255;
    } else if (color_type == 3) {
        const u32 count = indexed_palette_count(bit_depth);
        return indexed_palette_rgba((x * 3u + y * 5u) % count);
    } else if (color_type == 4) {
        const u8 gray = expand_sample(format_sample(bit_depth, 0, x, y), bit_depth);
        red = green = blue = gray;
        alpha = expand_sample(format_sample(bit_depth, 1, x, y), bit_depth);
    } else if (color_type == 6) {
        red = expand_sample(format_sample(bit_depth, 0, x, y), bit_depth);
        green = expand_sample(format_sample(bit_depth, 1, x, y), bit_depth);
        blue = expand_sample(format_sample(bit_depth, 2, x, y), bit_depth);
        alpha = expand_sample(format_sample(bit_depth, 3, x, y), bit_depth);
    }
    return static_cast<u32>(red) | static_cast<u32>(green) << 8 | static_cast<u32>(blue) << 16 |
           static_cast<u32>(alpha) << 24;
}

u32 apng_fixture_source_rgba(u32 frame, u32 x, u32 y) noexcept {
    if (frame == 0) {
        return static_cast<u32>(20u + x * 10u) | static_cast<u32>(30u + y * 10u) << 8 | 200u << 16 |
               255u << 24;
    }
    if (frame == 1) {
        return 20u | 220u << 8 | 40u << 16 | 128u << 24;
    }
    if (frame == 2) {
        return 240u | 30u << 8 | 20u << 16 | 255u << 24;
    }
    return 0xFFFFFFFFu;
}

u32 apng_fixture_canvas_rgba(u32 presented_frame, u32 x, u32 y) noexcept {
    const u32 base = premultiply_rgba(apng_fixture_source_rgba(0, x, y));
    if (presented_frame == 0) {
        return base;
    }
    if (presented_frame == 1) {
        return x >= 1 && x < 3 && y >= 1 && y < 3
                   ? over_rgba(apng_fixture_source_rgba(1, x - 1, y - 1), base)
                   : base;
    }
    if (presented_frame == 2) {
        return x < 2 && y < 2 ? premultiply_rgba(apng_fixture_source_rgba(2, x, y)) : base;
    }
    if (x < 2 && y < 2) {
        return 0;
    }
    return x == 3 && y == 3 ? 0xFFFFFFFFu : base;
}

u8 gif_fixture_source_index(u32 frame, u32 x, u32 y) noexcept {
    if (frame == 0) {
        return static_cast<u8>(1u + (x + y) % 3u);
    }
    if (frame == 1) {
        constexpr u8 indices[4] = {0, 2, 3, 2};
        return x < 2 && y < 2 ? indices[y * 2 + x] : 0;
    }
    if (frame == 2) {
        return static_cast<u8>(1u + (x + y * 2u) % 3u);
    }
    return 3;
}

u32 gif_fixture_palette_rgba(u32 frame, u32 index) noexcept {
    constexpr u32 global[4] = {
        0xFF100804u,
        0xFFFF3020u,
        0xFF30FF40u,
        0xFF4020FFu,
    };
    constexpr u32 local[4] = {
        0xFF080808u,
        0xFF20FFFFu,
        0xFFFFFF20u,
        0xFFFF20FFu,
    };
    return index < 4 ? (frame == 1 ? local[index] : global[index]) : 0;
}

u32 gif_fixture_canvas_rgba(u32 presented_frame, u32 x, u32 y) noexcept {
    const u32 base = gif_fixture_palette_rgba(0, gif_fixture_source_index(0, x, y));
    if (presented_frame == 0) {
        return base;
    }
    if (presented_frame == 1) {
        if (x >= 1 && x < 3 && y >= 1 && y < 3) {
            const u8 index = gif_fixture_source_index(1, x - 1, y - 1);
            return index == 0 ? base : gif_fixture_palette_rgba(1, index);
        }
        return base;
    }
    if (presented_frame == 2) {
        return x < 2 ? gif_fixture_palette_rgba(2, gif_fixture_source_index(2, x, y)) : base;
    }
    if (x < 2) {
        return 0;
    }
    return x == 3 && y == 3 ? gif_fixture_palette_rgba(3, 3) : base;
}

bool build_indexed_gif_fixture(Slice<u8> encoded, Slice<u8> lzw_scratch,
                               GifFixtureResult* result) noexcept {
    if (!encoded.data || !lzw_scratch.data || !result || encoded.count < 512 || lzw_scratch.count < 128) {
        return false;
    }
    constexpr u8 header[13] = {
        'G', 'I', 'F', '8', '9', 'a', 4, 0, 4, 0, 0x91, 0, 0,
    };
    constexpr u8 loop[19] = {
        0x21, 0xFF, 0x0B, 'N', 'E',  'T',  'S',  'C',  'A',  'P',
        'E',  '2',  '.',  '0', 0x03, 0x01, 0x02, 0x00, 0x00,
    };
    constexpr u8 comment[9] = {
        0x21, 0xFE, 0x02, 'M', '4', 0x02, '4', '4', 0x00,
    };
    u32 cursor = 0;
    if (!append_bytes(encoded, &cursor, ConstSlice<u8>{header, sizeof(header)}) ||
        !append_gif_palette(encoded, &cursor, 0) ||
        !append_bytes(encoded, &cursor, ConstSlice<u8>{loop, sizeof(loop)}) ||
        !append_bytes(encoded, &cursor, ConstSlice<u8>{comment, sizeof(comment)})) {
        return false;
    }
    const u16 widths[4] = {4, 2, 2, 1};
    const u16 heights[4] = {4, 2, 4, 1};
    const u16 xs[4] = {0, 1, 0, 3};
    const u16 ys[4] = {0, 1, 0, 3};
    const u16 delays[4] = {5, 0, 1, 2};
    const u8 disposals[4] = {0, 3, 2, 1};
    u32 total_lzw = 0;
    for (u32 frame = 0; frame < 4; ++frame) {
        const bool transparent = frame == 1;
        const bool interlaced = frame == 2;
        u32 lzw_bytes = 0;
        if (!build_gif_frame_lzw(frame, widths[frame], heights[frame], interlaced, lzw_scratch,
                                 &lzw_bytes) ||
            !append_gif_control(encoded, &cursor, disposals[frame], delays[frame], transparent, 0) ||
            !append_gif_descriptor(
                encoded, &cursor, xs[frame], ys[frame], widths[frame], heights[frame],
                static_cast<u8>((frame == 1 ? 0x81u : 0u) | (interlaced ? 0x40u : 0u))) ||
            (frame == 1 && !append_gif_palette(encoded, &cursor, 1)) ||
            !append_gif_image_data(encoded, &cursor, ConstSlice<u8>{lzw_scratch.data, lzw_bytes},
                                   frame == 0)) {
            return false;
        }
        total_lzw += lzw_bytes;
    }
    const u8 trailer = 0x3B;
    if (!append_bytes(encoded, &cursor, ConstSlice<u8>{&trailer, 1})) {
        return false;
    }
    *result = GifFixtureResult{cursor, 4, total_lzw};
    return true;
}

} // namespace maicord
