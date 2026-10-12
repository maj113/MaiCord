#include "media_fixture.hpp"

#include "webp_fixture.hpp"

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

bool append_jpeg_bytes(Slice<u8> output, u32* cursor, ConstSlice<u8> bytes) noexcept {
    if (!cursor || (!output.data && output.count != 0) || (!bytes.data && bytes.count != 0) ||
        static_cast<u64>(*cursor) + bytes.count > output.count) {
        return false;
    }
    for (u32 index = 0; index < bytes.count; ++index) {
        output[*cursor + index] = bytes[index];
    }
    *cursor += bytes.count;
    return true;
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

u32 sample_maximum(u8 bit_depth) noexcept {
    return bit_depth == 16 ? 65'535u : (1u << bit_depth) - 1u;
}

u32 format_sample(u8 bit_depth, u32 channel, u32 x, u32 y) noexcept {
    const u32 seed = x * (13u + channel * 7u) + y * (17u + channel * 11u) + (channel + 1u) * 37u;
    return bit_depth == 16 ? (seed * 257u + x * 31u + channel * 13u) & 65'535u
                           : seed & sample_maximum(bit_depth);
}

u8 expand_sample(u32 sample, u8 bit_depth) noexcept {
    const u32 maximum_sample = sample_maximum(bit_depth);
    return static_cast<u8>((sample * 255u + maximum_sample / 2u) / maximum_sample);
}

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
    output[0] = 0x78;
    output[1] = 0x01;
    output[2] = 0x01;
    output[3] = static_cast<u8>(filtered_bytes);
    output[4] = static_cast<u8>(filtered_bytes >> 8);
    const u16 inverse = static_cast<u16>(filtered_bytes ^ 0xFFFFu);
    output[5] = static_cast<u8>(inverse);
    output[6] = static_cast<u8>(inverse >> 8);
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

} // namespace

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

bool build_deferred_clear_gif_fixture(Slice<u8> encoded, Slice<u8> lzw_scratch,
                                      GifFixtureResult* result, u16 width, u16 height) noexcept {
    const u32 pixels = static_cast<u32>(width) * height;
    if (!encoded.data || !lzw_scratch.data || !result || encoded.count < 13'000 ||
        lzw_scratch.count < 12'500) {
        return false;
    }
    u32 lzw_bytes = 0;
    u32 bits = 0;
    u32 bit_count = 0;
    u32 code_size = 3;
    u32 next_code = 6;
    if (!gif_pack_code(lzw_scratch, &lzw_bytes, &bits, &bit_count, 4, code_size)) {
        return false;
    }
    for (u32 pixel = 0; pixel < pixels; ++pixel) {
        if (!gif_pack_code(lzw_scratch, &lzw_bytes, &bits, &bit_count, pixel & 1u, code_size)) {
            return false;
        }
        if (pixel != 0 && next_code < 4096) {
            ++next_code;
            if (next_code == (1u << code_size) && code_size < 12) {
                ++code_size;
            }
        }
    }
    if (!gif_pack_code(lzw_scratch, &lzw_bytes, &bits, &bit_count, 5, code_size)) {
        return false;
    }
    if (bit_count != 0) {
        if (lzw_bytes >= lzw_scratch.count) {
            return false;
        }
        lzw_scratch[lzw_bytes++] = static_cast<u8>(bits);
    }

    const u8 header[13] = {
        'G',
        'I',
        'F',
        '8',
        '7',
        'a',
        static_cast<u8>(width),
        static_cast<u8>(width >> 8),
        static_cast<u8>(height),
        static_cast<u8>(height >> 8),
        0x80,
        0,
        0,
    };
    constexpr u8 palette[6] = {0, 0, 0, 255, 255, 255};
    u32 cursor = 0;
    if (!append_bytes(encoded, &cursor, ConstSlice<u8>{header, sizeof(header)}) ||
        !append_bytes(encoded, &cursor, ConstSlice<u8>{palette, sizeof(palette)}) ||
        !append_gif_descriptor(encoded, &cursor, 0, 0, width, height, 0) ||
        !append_deferred_lzw(encoded, &cursor, ConstSlice<u8>{lzw_scratch.data, lzw_bytes})) {
        return false;
    }
    const u8 trailer = 0x3B;
    if (!append_bytes(encoded, &cursor, ConstSlice<u8>{&trailer, 1})) {
        return false;
    }
    *result = GifFixtureResult{cursor, 1, lzw_bytes};
    return true;
}

bool build_rgba8_apng_fixture(Slice<u8> encoded, Slice<u8> zlib_scratch,
                              ApngFixtureResult* result) noexcept {
    if (!encoded.data || !zlib_scratch.data || !result || encoded.count < 512 || zlib_scratch.count < 512) {
        return false;
    }
    constexpr u8 signature[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    for (u32 index = 0; index < sizeof(signature); ++index) {
        encoded[index] = signature[index];
    }
    u32 cursor = sizeof(signature);
    u8 header[13]{};
    write_be32(header, 4);
    write_be32(header + 4, 4);
    header[8] = 8;
    header[9] = 6;
    u8 animation[8]{};
    write_be32(animation, 4);
    write_be32(animation + 4, 2);
    if (!append_chunk(encoded, &cursor, 0x49484452u, ConstSlice<u8>{header, sizeof(header)}) ||
        !append_chunk(encoded, &cursor, 0x6163544Cu, ConstSlice<u8>{animation, sizeof(animation)}) ||
        !append_fctl(encoded, &cursor, 0, 4, 4, 0, 0, 1, 20, 0, 0)) {
        return false;
    }

    u32 scratch_cursor = 0;
    u32 total_zlib = 0;
    u32 frame_bytes = 0;
    if (!build_apng_zlib(0, 4, 4,
                         Slice<u8>{zlib_scratch.data + scratch_cursor, zlib_scratch.count - scratch_cursor},
                         &frame_bytes)) {
        return false;
    }
    const u32 split0 = frame_bytes / 2;
    if (!append_chunk(encoded, &cursor, 0x49444154u,
                      ConstSlice<u8>{zlib_scratch.data + scratch_cursor, split0}) ||
        !append_chunk(encoded, &cursor, 0x49444154u,
                      ConstSlice<u8>{zlib_scratch.data + scratch_cursor + split0, frame_bytes - split0})) {
        return false;
    }
    scratch_cursor += frame_bytes;
    total_zlib += frame_bytes;

    if (!append_fctl(encoded, &cursor, 1, 2, 2, 1, 1, 0, 0, 2, 1) ||
        !build_apng_zlib(1, 2, 2,
                         Slice<u8>{zlib_scratch.data + scratch_cursor, zlib_scratch.count - scratch_cursor},
                         &frame_bytes)) {
        return false;
    }
    const u32 split1 = frame_bytes / 2;
    if (!append_fdat(encoded, &cursor, 2, ConstSlice<u8>{zlib_scratch.data + scratch_cursor, split1}) ||
        !append_fdat(encoded, &cursor, 3,
                     ConstSlice<u8>{zlib_scratch.data + scratch_cursor + split1, frame_bytes - split1})) {
        return false;
    }
    scratch_cursor += frame_bytes;
    total_zlib += frame_bytes;

    if (!append_fctl(encoded, &cursor, 4, 2, 2, 0, 0, 1, 100, 1, 0) ||
        !build_apng_zlib(2, 2, 2,
                         Slice<u8>{zlib_scratch.data + scratch_cursor, zlib_scratch.count - scratch_cursor},
                         &frame_bytes) ||
        !append_fdat(encoded, &cursor, 5,
                     ConstSlice<u8>{zlib_scratch.data + scratch_cursor, frame_bytes})) {
        return false;
    }
    scratch_cursor += frame_bytes;
    total_zlib += frame_bytes;

    if (!append_fctl(encoded, &cursor, 6, 1, 1, 3, 3, 1, 60, 0, 0) ||
        !build_apng_zlib(3, 1, 1,
                         Slice<u8>{zlib_scratch.data + scratch_cursor, zlib_scratch.count - scratch_cursor},
                         &frame_bytes) ||
        !append_fdat(encoded, &cursor, 7,
                     ConstSlice<u8>{zlib_scratch.data + scratch_cursor, frame_bytes}) ||
        !append_chunk(encoded, &cursor, 0x49454E44u, ConstSlice<u8>{})) {
        return false;
    }
    total_zlib += frame_bytes;
    *result = ApngFixtureResult{cursor, 4, total_zlib};
    return true;
}

bool build_rgba8_apng_default_fixture(Slice<u8> encoded, Slice<u8> zlib_scratch,
                                      ApngFixtureResult* result) noexcept {
    if (!encoded.data || !zlib_scratch.data || !result || encoded.count < 512 || zlib_scratch.count < 512) {
        return false;
    }
    constexpr u8 signature[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    for (u32 index = 0; index < sizeof(signature); ++index) {
        encoded[index] = signature[index];
    }
    u32 cursor = sizeof(signature);
    u8 header[13]{};
    write_be32(header, 4);
    write_be32(header + 4, 4);
    header[8] = 8;
    header[9] = 6;
    u8 animation[8]{};
    write_be32(animation, 1);
    write_be32(animation + 4, 1);
    u32 default_bytes = 0;
    if (!append_chunk(encoded, &cursor, 0x49484452u, ConstSlice<u8>{header, sizeof(header)}) ||
        !append_chunk(encoded, &cursor, 0x6163544Cu, ConstSlice<u8>{animation, sizeof(animation)}) ||
        !build_apng_zlib(0, 4, 4, zlib_scratch, &default_bytes) ||
        !append_chunk(encoded, &cursor, 0x49444154u, ConstSlice<u8>{zlib_scratch.data, default_bytes}) ||
        !append_fctl(encoded, &cursor, 0, 1, 1, 3, 3, 1, 20, 0, 0)) {
        return false;
    }
    u32 frame_bytes = 0;
    if (!build_apng_zlib(3, 1, 1,
                         Slice<u8>{zlib_scratch.data + default_bytes, zlib_scratch.count - default_bytes},
                         &frame_bytes) ||
        !append_fdat(encoded, &cursor, 1, ConstSlice<u8>{zlib_scratch.data + default_bytes, frame_bytes}) ||
        !append_chunk(encoded, &cursor, 0x49454E44u, ConstSlice<u8>{})) {
        return false;
    }
    *result = ApngFixtureResult{cursor, 1, default_bytes + frame_bytes};
    return true;
}

bool build_png_format_fixture(u8 color_type, u8 bit_depth, u16 width, u16 height, Slice<u8> encoded,
                              Slice<u8> zlib_scratch, PngFixtureResult* result) noexcept {
    u32 channels = 0;
    if (!format_supported(color_type, bit_depth, &channels) || width == 0 || height == 0 || !encoded.data ||
        !zlib_scratch.data || !result) {
        return false;
    }
    const u32 row_bytes = (static_cast<u32>(width) * channels * bit_depth + 7u) / 8u;
    const u32 filtered_bytes = (row_bytes + 1u) * height;
    const u32 zlib_bytes = 2u + 5u + filtered_bytes + 4u;
    if (filtered_bytes > 65'535 || zlib_bytes > zlib_scratch.count) {
        return false;
    }
    zlib_scratch[0] = 0x78;
    zlib_scratch[1] = 0x01;
    zlib_scratch[2] = 0x01;
    zlib_scratch[3] = static_cast<u8>(filtered_bytes);
    zlib_scratch[4] = static_cast<u8>(filtered_bytes >> 8);
    const u16 inverse = static_cast<u16>(filtered_bytes ^ 0xFFFFu);
    zlib_scratch[5] = static_cast<u8>(inverse);
    zlib_scratch[6] = static_cast<u8>(inverse >> 8);
    u8* filtered = zlib_scratch.data + 7;
    for (u32 y = 0; y < height; ++y) {
        u8* row = filtered + y * (row_bytes + 1u);
        for (u32 index = 0; index <= row_bytes; ++index) {
            row[index] = 0;
        }
        if (bit_depth < 8) {
            for (u32 x = 0; x < width; ++x) {
                const u32 sample = color_type == 3 ? (x * 3u + y * 5u) % indexed_palette_count(bit_depth)
                                                   : format_sample(bit_depth, 0, x, y);
                const u32 bit_offset = x * bit_depth;
                const u32 shift = 8u - bit_depth - (bit_offset & 7u);
                row[1u + (bit_offset >> 3u)] |= static_cast<u8>(sample << shift);
            }
        } else {
            u32 cursor = 1;
            for (u32 x = 0; x < width; ++x) {
                for (u32 channel = 0; channel < channels; ++channel) {
                    const u32 sample = color_type == 3
                                           ? (x * 3u + y * 5u) % indexed_palette_count(bit_depth)
                                           : format_sample(bit_depth, channel, x, y);
                    if (bit_depth == 16) {
                        row[cursor++] = static_cast<u8>(sample >> 8);
                    }
                    row[cursor++] = static_cast<u8>(sample);
                }
            }
        }
    }
    write_be32(filtered + filtered_bytes, adler32(filtered, filtered_bytes));

    constexpr u8 signature[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    if (encoded.count < sizeof(signature)) {
        return false;
    }
    for (u32 index = 0; index < sizeof(signature); ++index) {
        encoded[index] = signature[index];
    }
    u32 output_cursor = sizeof(signature);
    u8 header[13]{};
    write_be32(header, width);
    write_be32(header + 4, height);
    header[8] = bit_depth;
    header[9] = color_type;
    if (!append_chunk(encoded, &output_cursor, 0x49484452u, ConstSlice<u8>{header, sizeof(header)})) {
        return false;
    }

    u8 palette[768]{};
    u8 transparency[256]{};
    if (color_type == 3) {
        const u32 palette_count = indexed_palette_count(bit_depth);
        for (u32 index = 0; index < palette_count; ++index) {
            const u32 rgba = indexed_palette_rgba(index);
            palette[index * 3] = static_cast<u8>(rgba);
            palette[index * 3 + 1] = static_cast<u8>(rgba >> 8);
            palette[index * 3 + 2] = static_cast<u8>(rgba >> 16);
            transparency[index] = static_cast<u8>(rgba >> 24);
        }
        if (!append_chunk(encoded, &output_cursor, 0x504C5445u,
                          ConstSlice<u8>{palette, palette_count * 3u}) ||
            !append_chunk(encoded, &output_cursor, 0x74524E53u,
                          ConstSlice<u8>{transparency, palette_count})) {
            return false;
        }
    } else if (color_type == 0) {
        const u32 transparent = format_sample(bit_depth, 0, 0, 0);
        transparency[0] = static_cast<u8>(transparent >> 8);
        transparency[1] = static_cast<u8>(transparent);
        if (!append_chunk(encoded, &output_cursor, 0x74524E53u, ConstSlice<u8>{transparency, 2})) {
            return false;
        }
    } else if (color_type == 2) {
        for (u32 channel = 0; channel < 3; ++channel) {
            const u32 transparent = format_sample(bit_depth, channel, 0, 0);
            transparency[channel * 2] = static_cast<u8>(transparent >> 8);
            transparency[channel * 2 + 1] = static_cast<u8>(transparent);
        }
        if (!append_chunk(encoded, &output_cursor, 0x74524E53u, ConstSlice<u8>{transparency, 6})) {
            return false;
        }
    }
    if (!append_chunk(encoded, &output_cursor, 0x49444154u,
                      ConstSlice<u8>{zlib_scratch.data, zlib_bytes}) ||
        !append_chunk(encoded, &output_cursor, 0x49454E44u, ConstSlice<u8>{})) {
        return false;
    }
    *result = PngFixtureResult{output_cursor, zlib_bytes, filtered_bytes};
    return true;
}

bool build_rgba8_png_fixture(u16 width, u16 height, Slice<u8> encoded, Slice<u8> zlib_scratch,
                             PngFixtureResult* result, PngFixturePixel pixel) noexcept {
    if (!encoded.data || !zlib_scratch.data || !result || width == 0 || height == 0) {
        return false;
    }
    const u32 row_bytes = static_cast<u32>(width) * 4;
    const u32 filtered_bytes = (row_bytes + 1) * height;
    const u32 zlib_bytes = 2 + 5 + filtered_bytes + 4;
    if (filtered_bytes > 65'535 || zlib_bytes > zlib_scratch.count) {
        return false;
    }

    zlib_scratch[0] = 0x78;
    zlib_scratch[1] = 0x01;
    zlib_scratch[2] = 0x01;
    zlib_scratch[3] = static_cast<u8>(filtered_bytes);
    zlib_scratch[4] = static_cast<u8>(filtered_bytes >> 8);
    const u16 inverse = static_cast<u16>(filtered_bytes ^ 0xFFFFu);
    zlib_scratch[5] = static_cast<u8>(inverse);
    zlib_scratch[6] = static_cast<u8>(inverse >> 8);
    u8* filtered = zlib_scratch.data + 7;
    for (u32 y = 0; y < height; ++y) {
        u8* row = filtered + y * (row_bytes + 1);
        const u8 filter = static_cast<u8>(y % 5);
        row[0] = filter;
        for (u32 x = 0; x < row_bytes; ++x) {
            const u8 value = channel_byte(x, y, width, height, pixel);
            const u8 left = x >= 4 ? channel_byte(x - 4, y, width, height, pixel) : 0;
            const u8 above = y != 0 ? channel_byte(x, y - 1, width, height, pixel) : 0;
            const u8 upper_left = x >= 4 && y != 0 ? channel_byte(x - 4, y - 1, width, height, pixel) : 0;
            u32 predictor = 0;
            switch (filter) {
            case 1:
                predictor = left;
                break;
            case 2:
                predictor = above;
                break;
            case 3:
                predictor = (static_cast<u32>(left) + above) >> 1;
                break;
            case 4:
                predictor = paeth(left, above, upper_left);
                break;
            }
            row[x + 1] = static_cast<u8>(value - predictor);
        }
    }
    write_be32(filtered + filtered_bytes, adler32(filtered, filtered_bytes));

    constexpr u8 signature[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    if (encoded.count < sizeof(signature)) {
        return false;
    }
    for (u32 index = 0; index < sizeof(signature); ++index) {
        encoded[index] = signature[index];
    }
    u32 cursor = sizeof(signature);
    u8 header[13]{};
    write_be32(header, width);
    write_be32(header + 4, height);
    header[8] = 8;
    header[9] = 6;
    const u32 split = minimum<u32>(11, zlib_bytes);
    if (!append_chunk(encoded, &cursor, 0x49484452u, ConstSlice<u8>{header, sizeof(header)}) ||
        !append_chunk(encoded, &cursor, 0x49444154u, ConstSlice<u8>{zlib_scratch.data, split}) ||
        !append_chunk(encoded, &cursor, 0x49444154u,
                      ConstSlice<u8>{zlib_scratch.data + split, zlib_bytes - split}) ||
        !append_chunk(encoded, &cursor, 0x49454E44u, ConstSlice<u8>{})) {
        return false;
    }
    *result = PngFixtureResult{cursor, zlib_bytes, filtered_bytes};
    return true;
}

bool build_lossless_animated_webp_fixture(Slice<u8> encoded, AnimatedWebpFixtureResult* result) noexcept {
    if (!encoded.data || !result || encoded.count < 256) {
        return false;
    }
    constexpr u8 frame_payloads[4][16] = {
        {0x2F, 0x03, 0xC0, 0x00, 0x00, 0x07, 0x10, 0xFD, 0x8F, 0xFE, 0x07, 0x22, 0xA2, 0xFF, 0x01, 0x00},
        {0x2F, 0x01, 0x40, 0x00, 0x10, 0x07, 0xD0, 0xFF, 0x88, 0x02, 0x06, 0x22, 0xA2, 0xFF, 0x01, 0x00},
        {0x2F, 0x01, 0xC0, 0x00, 0x00, 0x07, 0x10, 0xD1, 0xFF, 0xFE, 0x07, 0x22, 0xA2, 0xFF, 0x01, 0x00},
        {0x2F, 0x01, 0x40, 0x00, 0x10, 0x07, 0xD0, 0xFF, 0xFE, 0x47, 0x01, 0x03, 0x11, 0xD1, 0xFF, 0x00},
    };
    constexpr u8 payload_bytes[4] = {15, 15, 15, 16};
    constexpr u16 x_offsets[4] = {0, 2, 0, 2};
    constexpr u16 y_offsets[4] = {0, 0, 0, 2};
    constexpr u16 widths[4] = {4, 2, 2, 2};
    constexpr u16 heights[4] = {4, 2, 4, 2};
    constexpr u32 durations[4] = {50, 0, 10, 20};
    constexpr u8 flags[4] = {2, 1, 2, 0};

    write_le32(encoded.data, 0x46464952u);
    write_le32(encoded.data + 8, 0x50424557u);
    u32 cursor = 12;
    u8 vp8x[10]{};
    vp8x[0] = 0x12;
    write_le24(vp8x + 4, 3);
    write_le24(vp8x + 7, 3);
    u8 anim[6] = {48, 32, 16, 128, 2, 0};
    if (!append_riff_chunk(encoded, &cursor, 0x58385056u, ConstSlice<u8>{vp8x, sizeof(vp8x)}) ||
        !append_riff_chunk(encoded, &cursor, 0x4D494E41u, ConstSlice<u8>{anim, sizeof(anim)})) {
        return false;
    }
    for (u32 frame = 0; frame < 4; ++frame) {
        u8 anmf[48]{};
        write_le24(anmf, x_offsets[frame] / 2);
        write_le24(anmf + 3, y_offsets[frame] / 2);
        write_le24(anmf + 6, widths[frame] - 1);
        write_le24(anmf + 9, heights[frame] - 1);
        write_le24(anmf + 12, durations[frame]);
        anmf[15] = flags[frame];
        u32 frame_cursor = 16;
        if (!append_riff_chunk(Slice<u8>{anmf, sizeof(anmf)}, &frame_cursor, 0x4C385056u,
                               ConstSlice<u8>{frame_payloads[frame], payload_bytes[frame]}) ||
            !append_riff_chunk(encoded, &cursor, 0x464D4E41u, ConstSlice<u8>{anmf, frame_cursor})) {
            return false;
        }
    }
    constexpr u8 unknown = 0x5A;
    if (!append_riff_chunk(encoded, &cursor, 0x54534554u, ConstSlice<u8>{&unknown, 1})) {
        return false;
    }
    write_le32(encoded.data + 4, cursor - 8);
    *result = AnimatedWebpFixtureResult{cursor, 4};
    return true;
}

bool build_mixed_animated_webp_fixture(Slice<u8> encoded, AnimatedWebpFixtureResult* result) noexcept {
    if (!encoded.data || !result || encoded.count < 512) {
        return false;
    }
    constexpr u8 lossless_red[15] = {
        0x2F, 0x03, 0xC0, 0x00, 0x00, 0x07, 0x10, 0xFD, 0x8F, 0xFE, 0x07, 0x22, 0xA2, 0xFF, 0x01,
    };
    constexpr u32 lossy_payload_offset = 20;
    constexpr u32 lossy_payload_bytes = sizeof(kWebpLossy4x4) - lossy_payload_offset;
    u8 alpha[17]{};
    for (u32 index = 1; index < sizeof(alpha); ++index) {
        alpha[index] = 128;
    }

    write_le32(encoded.data, 0x46464952u);
    write_le32(encoded.data + 8, 0x50424557u);
    u32 cursor = 12;
    u8 vp8x[10]{};
    vp8x[0] = 0x12;
    write_le24(vp8x + 4, 3);
    write_le24(vp8x + 7, 3);
    u8 anim[6]{};
    anim[4] = 1;
    if (!append_riff_chunk(encoded, &cursor, 0x58385056u, ConstSlice<u8>{vp8x, sizeof(vp8x)}) ||
        !append_riff_chunk(encoded, &cursor, 0x4D494E41u, ConstSlice<u8>{anim, sizeof(anim)})) {
        return false;
    }

    for (u32 frame = 0; frame < 3; ++frame) {
        u8 anmf[160]{};
        write_le24(anmf + 6, 3);
        write_le24(anmf + 9, 3);
        write_le24(anmf + 12, 30 + frame * 10);
        anmf[15] = frame == 1 ? 3 : frame == 0 ? 2 : 0;
        u32 frame_cursor = 16;
        if (frame == 0) {
            if (!append_riff_chunk(Slice<u8>{anmf, sizeof(anmf)}, &frame_cursor, 0x4C385056u,
                                   ConstSlice<u8>{lossless_red, sizeof(lossless_red)})) {
                return false;
            }
        } else {
            if (frame == 2 && !append_riff_chunk(Slice<u8>{anmf, sizeof(anmf)}, &frame_cursor, 0x48504C41u,
                                                 ConstSlice<u8>{alpha, sizeof(alpha)})) {
                return false;
            }
            if (!append_riff_chunk(
                    Slice<u8>{anmf, sizeof(anmf)}, &frame_cursor, 0x20385056u,
                    ConstSlice<u8>{kWebpLossy4x4 + lossy_payload_offset, lossy_payload_bytes})) {
                return false;
            }
        }
        if (!append_riff_chunk(encoded, &cursor, 0x464D4E41u, ConstSlice<u8>{anmf, frame_cursor})) {
            return false;
        }
    }
    write_le32(encoded.data + 4, cursor - 8);
    *result = AnimatedWebpFixtureResult{cursor, 3};
    return true;
}

bool build_opaque_lossy_animated_webp_fixture(Slice<u8> encoded,
                                              AnimatedWebpFixtureResult* result) noexcept {
    if (!encoded.data || !result || encoded.count < 256) {
        return false;
    }
    write_le32(encoded.data, 0x46464952u);
    write_le32(encoded.data + 8, 0x50424557u);
    u32 cursor = 12;
    u8 vp8x[10]{};
    // Original opaque VP8 frames with the redundant alpha hint seen in public
    // animated preview containers. No provider payload is embedded in tests.
    vp8x[0] = 0x12;
    write_le24(vp8x + 4, 3);
    write_le24(vp8x + 7, 3);
    u8 anim[6] = {0, 0, 0, 255, 2, 0};
    if (!append_riff_chunk(encoded, &cursor, 0x58385056u, {vp8x, sizeof(vp8x)}) ||
        !append_riff_chunk(encoded, &cursor, 0x4D494E41u, {anim, sizeof(anim)})) {
        return false;
    }
    for (u32 frame = 0; frame < 2; ++frame) {
        u8 anmf[96]{};
        write_le24(anmf + 6, 3);
        write_le24(anmf + 9, 3);
        write_le24(anmf + 12, 20 + frame * 20);
        anmf[15] = frame == 0 ? 2 : 1;
        u32 frame_cursor = 16;
        if (!append_riff_chunk({anmf, sizeof(anmf)}, &frame_cursor, 0x20385056u,
                               {kWebpLossy4x4 + 20, sizeof(kWebpLossy4x4) - 20}) ||
            !append_riff_chunk(encoded, &cursor, 0x464D4E41u, {anmf, frame_cursor})) {
            return false;
        }
    }
    write_le32(encoded.data + 4, cursor - 8);
    *result = AnimatedWebpFixtureResult{cursor, 2};
    return true;
}

bool build_baseline_jpeg_fixture(Slice<u8> encoded, bool restart_markers,
                                 JpegFixtureResult* result) noexcept {
    if (!encoded.data || !result || encoded.count < 192) {
        return false;
    }
    constexpr u8 soi[] = {0xFF, 0xD8};
    constexpr u8 jfif[] = {
        'J', 'F', 'I', 'F', 0, 1, 2, 0, 0, 1, 0, 1, 0, 0,
    };
    u8 quantization[65]{};
    for (u32 index = 1; index < sizeof(quantization); ++index) {
        quantization[index] = 1;
    }
    u8 frame[] = {
        8, 0, 8, 0, 8, 3, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0,
    };
    if (restart_markers) {
        frame[3] = 0;
        frame[4] = 16;
    }
    u8 huffman[37]{};
    u32 huffman_cursor = 0;
    huffman[huffman_cursor++] = 0x00;
    huffman[huffman_cursor++] = 1;
    huffman[huffman_cursor++] = 1;
    huffman_cursor += 14;
    huffman[huffman_cursor++] = 0;
    huffman[huffman_cursor++] = 9;
    huffman[huffman_cursor++] = 0x10;
    huffman[huffman_cursor++] = 1;
    huffman_cursor += 15;
    huffman[huffman_cursor++] = 0;
    constexpr u8 restart_interval[] = {0, 1};
    constexpr u8 scan[] = {
        3, 1, 0, 2, 0, 3, 0, 0, 63, 0,
    };
    constexpr u8 mcu[] = {0x27, 0xFA, 0x80, 0x3F};
    constexpr u8 restart[] = {0xFF, 0xD0};
    constexpr u8 eoi[] = {0xFF, 0xD9};

    u32 cursor = 0;
    if (!append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{soi, sizeof(soi)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xE0, ConstSlice<u8>{jfif, sizeof(jfif)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xDB, ConstSlice<u8>{quantization, sizeof(quantization)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xC0, ConstSlice<u8>{frame, sizeof(frame)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xC4, ConstSlice<u8>{huffman, sizeof(huffman)}) ||
        (restart_markers &&
         !append_jpeg_segment(encoded, &cursor, 0xDD,
                              ConstSlice<u8>{restart_interval, sizeof(restart_interval)})) ||
        !append_jpeg_segment(encoded, &cursor, 0xDA, ConstSlice<u8>{scan, sizeof(scan)}) ||
        !append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{mcu, sizeof(mcu)}) ||
        (restart_markers &&
         (!append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{restart, sizeof(restart)}) ||
          !append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{mcu, sizeof(mcu)}))) ||
        !append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{eoi, sizeof(eoi)})) {
        return false;
    }
    *result = JpegFixtureResult{cursor, static_cast<u16>(restart_markers ? 16 : 8), 8,
                                static_cast<u16>(restart_markers ? 1 : 0)};
    return true;
}

bool build_grayscale_jpeg_fixture(Slice<u8> encoded, JpegFixtureResult* result) noexcept {
    if (!encoded.data || !result || encoded.count < 160) {
        return false;
    }
    constexpr u8 soi[] = {0xFF, 0xD8};
    constexpr u8 jfif[] = {
        'J', 'F', 'I', 'F', 0, 1, 2, 0, 0, 1, 0, 1, 0, 0,
    };
    u8 quantization[65]{};
    for (u32 index = 1; index < sizeof(quantization); ++index) {
        quantization[index] = 1;
    }
    constexpr u8 frame[] = {8, 0, 8, 0, 8, 1, 1, 0x11, 0};
    u8 huffman[37]{};
    u32 huffman_cursor = 0;
    huffman[huffman_cursor++] = 0x00;
    huffman[huffman_cursor++] = 1;
    huffman[huffman_cursor++] = 1;
    huffman_cursor += 14;
    huffman[huffman_cursor++] = 0;
    huffman[huffman_cursor++] = 9;
    huffman[huffman_cursor++] = 0x10;
    huffman[huffman_cursor++] = 1;
    huffman_cursor += 15;
    huffman[huffman_cursor++] = 0;
    constexpr u8 scan[] = {1, 1, 0, 0, 63, 0};
    constexpr u8 mcu[] = {0x3F};
    constexpr u8 eoi[] = {0xFF, 0xD9};
    u32 cursor = 0;
    if (!append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{soi, sizeof(soi)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xE0, ConstSlice<u8>{jfif, sizeof(jfif)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xDB, ConstSlice<u8>{quantization, sizeof(quantization)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xC0, ConstSlice<u8>{frame, sizeof(frame)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xC4, ConstSlice<u8>{huffman, sizeof(huffman)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xDA, ConstSlice<u8>{scan, sizeof(scan)}) ||
        !append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{mcu, sizeof(mcu)}) ||
        !append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{eoi, sizeof(eoi)})) {
        return false;
    }
    *result = JpegFixtureResult{cursor, 8, 8, 0};
    return true;
}

bool build_jpeg_exif_orientation_fixture(ConstSlice<u8> source, Slice<u8> encoded, u8 orientation,
                                         bool big_endian, u32* encoded_bytes) noexcept {
    if (!source.data || source.count < 2 || !encoded.data || encoded.data == source.data ||
        !encoded_bytes || source[0] != 0xFF || source[1] != 0xD8 || orientation < 1 || orientation > 8 ||
        static_cast<u64>(source.count) + 36u > encoded.count) {
        return false;
    }
    u8 exif[32]{};
    exif[0] = 'E';
    exif[1] = 'x';
    exif[2] = 'i';
    exif[3] = 'f';
    if (big_endian) {
        exif[6] = 'M';
        exif[7] = 'M';
        exif[9] = 42;
        exif[13] = 8;
        exif[15] = 1;
        exif[16] = 0x01;
        exif[17] = 0x12;
        exif[19] = 3;
        exif[23] = 1;
        exif[24] = 0;
        exif[25] = orientation;
    } else {
        exif[6] = 'I';
        exif[7] = 'I';
        exif[8] = 42;
        exif[10] = 8;
        exif[14] = 1;
        exif[16] = 0x12;
        exif[17] = 0x01;
        exif[18] = 3;
        exif[20] = 1;
        exif[24] = orientation;
    }
    u32 cursor = 0;
    if (!append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{source.data, 2}) ||
        !append_jpeg_segment(encoded, &cursor, 0xE1, ConstSlice<u8>{exif, sizeof(exif)}) ||
        !append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{source.data + 2, source.count - 2})) {
        return false;
    }
    *encoded_bytes = cursor;
    return true;
}

bool build_jpeg_icc_matrix_trc_fixture(ConstSlice<u8> source, Slice<u8> encoded, JpegIccFixtureCurve curve,
                                       bool version_four, bool reverse_chunks,
                                       JpegIccFixtureResult* result) noexcept {
    if (!source.data || source.count < 6 || !encoded.data || encoded.data == source.data || !result ||
        source[0] != 0xFF || source[1] != 0xD8 ||
        static_cast<u8>(curve) > static_cast<u8>(JpegIccFixtureCurve::sampled_srgb)) {
        return false;
    }
    u32 insertion = 2;
    if (source[2] == 0xFF && source[3] == 0xE0) {
        const u32 length = static_cast<u32>(source[4]) << 8u | source[5];
        if (length < 2 || 4u + length > source.count) {
            return false;
        }
        insertion = 4u + length;
    }

    u8 profile[768]{};
    write_be32(profile, 0);
    profile[8] = version_four ? 4 : 2;
    profile[9] = version_four ? 0x30 : 0x10;
    write_be32(profile + 12, 0x6D6E7472u); // mntr
    write_be32(profile + 16, 0x52474220u); // RGB
    write_be32(profile + 20, 0x58595A20u); // XYZ
    write_be32(profile + 36, 0x61637370u); // acsp
    write_be32(profile + 64, 1);
    write_be32(profile + 68, 0x0000F6D6u);
    write_be32(profile + 72, 0x00010000u);
    write_be32(profile + 76, 0x0000D32Du);
    write_be32(profile + 128, 6);
    constexpr u32 tag_signatures[6] = {
        0x7258595Au, 0x6758595Au, 0x6258595Au, 0x72545243u, 0x67545243u, 0x62545243u,
    };
    for (u32 index = 0; index < 6; ++index) {
        write_be32(profile + 132u + index * 12u, tag_signatures[index]);
    }

    // The source primaries are canonical sRGB with red and blue columns
    // exchanged. An sRGB TRC therefore gives a simple independent R/B-swap
    // oracle after conversion back to sRGB.
    constexpr u32 swapped_columns[3][3] = {
        {9'376u, 3'972u, 46'799u},
        {25'241u, 46'981u, 6'362u},
        {28'578u, 14'581u, 912u},
    };
    u32 profile_cursor = 204;
    for (u32 channel = 0; channel < 3; ++channel) {
        const u32 tag = 132u + channel * 12u;
        write_be32(profile + tag + 4u, profile_cursor);
        write_be32(profile + tag + 8u, 20);
        write_be32(profile + profile_cursor, 0x58595A20u);
        for (u32 component = 0; component < 3; ++component) {
            write_be32(profile + profile_cursor + 8u + component * 4u, swapped_columns[channel][component]);
        }
        profile_cursor += 20;
    }
    constexpr u32 srgb_parameters[7] = {
        157'286u, 62'119u, 3'417u, 5'072u, 2'651u, 0, 0,
    };
    constexpr u16 srgb_table[65] = {
        0,      79,     159,    240,    338,    456,    595,    756,    940,    1'148,  1'381,
        1'639,  1'923,  2'234,  2'572,  2'939,  3'334,  3'759,  4'214,  4'699,  5'215,  5'763,
        6'344,  6'957,  7'603,  8'283,  8'997,  9'746,  10'530, 11'350, 12'206, 13'098, 14'027,
        14'994, 15'998, 17'041, 18'122, 19'242, 20'401, 21'600, 22'840, 24'119, 25'440, 26'802,
        28'206, 29'651, 31'139, 32'670, 34'243, 35'860, 37'521, 39'226, 40'975, 42'768, 44'607,
        46'491, 48'420, 50'396, 52'417, 54'486, 56'601, 58'763, 60'972, 63'230, 65'535,
    };
    for (u32 channel = 0; channel < 3; ++channel) {
        profile_cursor = (profile_cursor + 3u) & ~3u;
        const u32 tag = 132u + (3u + channel) * 12u;
        const u32 tag_bytes = curve == JpegIccFixtureCurve::parametric_srgb ? 40u : 142u;
        write_be32(profile + tag + 4u, profile_cursor);
        write_be32(profile + tag + 8u, tag_bytes);
        if (curve == JpegIccFixtureCurve::parametric_srgb) {
            write_be32(profile + profile_cursor, 0x70617261u); // para
            write_be16(profile + profile_cursor + 8u, 4);
            for (u32 parameter = 0; parameter < 7; ++parameter) {
                write_be32(profile + profile_cursor + 12u + parameter * 4u, srgb_parameters[parameter]);
            }
        } else {
            write_be32(profile + profile_cursor, 0x63757276u); // curv
            write_be32(profile + profile_cursor + 8u, 65);
            for (u32 entry = 0; entry < 65; ++entry) {
                write_be16(profile + profile_cursor + 12u + entry * 2u, srgb_table[entry]);
            }
        }
        profile_cursor += tag_bytes;
    }
    profile_cursor = (profile_cursor + 3u) & ~3u;
    write_be32(profile, profile_cursor);
    if (static_cast<u64>(source.count) + profile_cursor + 36u > encoded.count) {
        return false;
    }

    JpegIccFixtureResult output{};
    output.profile_bytes = profile_cursor;
    const u32 first_bytes = profile_cursor / 2u - 3u;
    output.chunk_data_bytes[0] = static_cast<u16>(first_bytes);
    output.chunk_data_bytes[1] = static_cast<u16>(profile_cursor - first_bytes);
    u32 cursor = 0;
    if (!append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{source.data, insertion})) {
        return false;
    }
    for (u32 placement = 0; placement < 2; ++placement) {
        const u32 sequence = reverse_chunks ? 2u - placement : placement + 1u;
        const u32 profile_offset = sequence == 1 ? 0 : first_bytes;
        const u32 profile_part_bytes = output.chunk_data_bytes[sequence - 1u];
        u8 payload[14 + sizeof(profile)]{};
        constexpr u8 signature[12] = {
            'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0,
        };
        for (u32 index = 0; index < sizeof(signature); ++index) {
            payload[index] = signature[index];
        }
        payload[12] = static_cast<u8>(sequence);
        payload[13] = 2;
        for (u32 index = 0; index < profile_part_bytes; ++index) {
            payload[14u + index] = profile[profile_offset + index];
        }
        output.chunk_data_offsets[sequence - 1u] = cursor + 18u;
        if (!append_jpeg_segment(encoded, &cursor, 0xE2,
                                 ConstSlice<u8>{payload, 14u + profile_part_bytes})) {
            return false;
        }
    }
    if (!append_jpeg_bytes(encoded, &cursor,
                           ConstSlice<u8>{source.data + insertion, source.count - insertion})) {
        return false;
    }
    output.encoded_bytes = cursor;
    *result = output;
    return true;
}

bool build_jpeg_icc_lut_fixture(ConstSlice<u8> source, Slice<u8> encoded,
                                JpegIccLutFixtureSpace color_space, JpegIccLutFixturePrecision precision,
                                bool reverse_chunks, JpegIccFixtureResult* result) noexcept {
    if (!source.data || source.count < 6 || !encoded.data || encoded.data == source.data || !result ||
        source[0] != 0xFF || source[1] != 0xD8 ||
        static_cast<u8>(color_space) > static_cast<u8>(JpegIccLutFixtureSpace::cmyk) ||
        static_cast<u8>(precision) > static_cast<u8>(JpegIccLutFixturePrecision::mft2)) {
        return false;
    }
    u32 insertion = 2;
    if (source[2] == 0xFF && source[3] == 0xE0) {
        const u32 length = static_cast<u32>(source[4]) << 8u | source[5];
        if (length < 2 || 4u + length > source.count) {
            return false;
        }
        insertion = 4u + length;
    }

    const bool cmyk = color_space == JpegIccLutFixtureSpace::cmyk;
    const bool mft2 = precision == JpegIccLutFixturePrecision::mft2;
    const u32 input_channels = cmyk ? 4u : 3u;
    const u32 table_entries = mft2 ? 2u : 256u;
    const u32 precision_bytes = mft2 ? 2u : 1u;
    const u32 clut_entries = (1u << input_channels) * 3u;

    u8 profile[2'048]{};
    profile[8] = mft2 ? 4 : 2;
    profile[9] = mft2 ? 0x30 : 0x10;
    write_be32(profile + 12, cmyk ? 0x70727472u : 0x6D6E7472u); // prtr / mntr
    write_be32(profile + 16, cmyk ? 0x434D594Bu : 0x52474220u); // CMYK / RGB
    write_be32(profile + 20, 0x4C616220u);                      // Lab
    write_be32(profile + 36, 0x61637370u);                      // acsp
    write_be32(profile + 64, 1);
    write_be32(profile + 68, 0x0000F6D6u);
    write_be32(profile + 72, 0x00010000u);
    write_be32(profile + 76, 0x0000D32Du);
    write_be32(profile + 128, 1);
    write_be32(profile + 132, 0x41324230u); // A2B0
    write_be32(profile + 136, 144);

    u32 cursor = 144;
    write_be32(profile + cursor, mft2 ? 0x6D667432u : 0x6D667431u); // mft2 / mft1
    profile[cursor + 8u] = static_cast<u8>(input_channels);
    profile[cursor + 9u] = 3;
    profile[cursor + 10u] = 2;
    for (u32 diagonal = 0; diagonal < 3; ++diagonal) {
        write_be32(profile + cursor + 12u + (diagonal * 3u + diagonal) * 4u, 0x00010000u);
    }
    u32 data = cursor + 48u;
    if (mft2) {
        write_be16(profile + data, 2);
        write_be16(profile + data + 2u, 2);
        data += 4u;
    }
    for (u32 channel = 0; channel < input_channels; ++channel) {
        for (u32 entry = 0; entry < table_entries; ++entry) {
            const u32 value = table_entries == 2 ? entry * 65'535u : entry;
            if (mft2) {
                write_be16(profile + data, static_cast<u16>(value));
                data += 2u;
            } else {
                profile[data++] = static_cast<u8>(value);
            }
        }
    }
    const float encoded_maximum = mft2 ? 65'280.0f : 255.0f;
    const u32 node_count = 1u << input_channels;
    for (u32 node = 0; node < node_count; ++node) {
        const float first_node = static_cast<float>((node >> (input_channels - 1u)) & 1u);
        const float second_node = static_cast<float>((node >> (input_channels - 2u)) & 1u);
        const float third_node = static_cast<float>((node >> (input_channels - 3u)) & 1u);
        const float fourth_node = cmyk ? static_cast<float>(node & 1u) : 0.0f;
        const float lab[3] = {
            cmyk ? 0.20f + 0.60f * (1.0f - first_node) * (1.0f - fourth_node) : 0.25f + 0.50f * first_node,
            0.375f + 0.25f * (cmyk ? 1.0f - second_node : second_node),
            0.375f + 0.25f * (cmyk ? 1.0f - third_node : third_node),
        };
        for (u32 output = 0; output < 3; ++output) {
            const u32 value = static_cast<u32>(lab[output] * encoded_maximum + 0.5f);
            if (mft2) {
                write_be16(profile + data, static_cast<u16>(value));
                data += 2u;
            } else {
                profile[data++] = static_cast<u8>(value);
            }
        }
    }
    for (u32 channel = 0; channel < 3; ++channel) {
        for (u32 entry = 0; entry < table_entries; ++entry) {
            const u32 value = table_entries == 2 ? entry * 65'535u : entry;
            if (mft2) {
                write_be16(profile + data, static_cast<u16>(value));
                data += 2u;
            } else {
                profile[data++] = static_cast<u8>(value);
            }
        }
    }
    const u32 tag_bytes = data - cursor;
    write_be32(profile + 140, tag_bytes);
    const u32 profile_bytes = (data + 3u) & ~3u;
    write_be32(profile, profile_bytes);
    if (tag_bytes !=
            48u + (input_channels * table_entries + clut_entries + 3u * table_entries) * precision_bytes +
                (mft2 ? 4u : 0u) ||
        static_cast<u64>(source.count) + profile_bytes + 36u > encoded.count) {
        return false;
    }

    JpegIccFixtureResult output{};
    output.profile_bytes = profile_bytes;
    const u32 first_bytes = profile_bytes / 2u - 3u;
    output.chunk_data_bytes[0] = static_cast<u16>(first_bytes);
    output.chunk_data_bytes[1] = static_cast<u16>(profile_bytes - first_bytes);
    cursor = 0;
    if (!append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{source.data, insertion})) {
        return false;
    }
    for (u32 placement = 0; placement < 2; ++placement) {
        const u32 sequence = reverse_chunks ? 2u - placement : placement + 1u;
        const u32 profile_offset = sequence == 1 ? 0 : first_bytes;
        const u32 part_bytes = output.chunk_data_bytes[sequence - 1u];
        u8 payload[14 + sizeof(profile)]{};
        constexpr u8 signature[12] = {
            'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0,
        };
        for (u32 index = 0; index < sizeof(signature); ++index) {
            payload[index] = signature[index];
        }
        payload[12] = static_cast<u8>(sequence);
        payload[13] = 2;
        for (u32 index = 0; index < part_bytes; ++index) {
            payload[14u + index] = profile[profile_offset + index];
        }
        output.chunk_data_offsets[sequence - 1u] = cursor + 18u;
        if (!append_jpeg_segment(encoded, &cursor, 0xE2, ConstSlice<u8>{payload, 14u + part_bytes})) {
            return false;
        }
    }
    if (!append_jpeg_bytes(encoded, &cursor,
                           ConstSlice<u8>{source.data + insertion, source.count - insertion})) {
        return false;
    }
    output.encoded_bytes = cursor;
    *result = output;
    return true;
}

bool build_jpeg_icc_gray_trc_fixture(ConstSlice<u8> source, Slice<u8> encoded, bool pcs_lab,
                                     bool reverse_chunks, JpegIccFixtureResult* result) noexcept {
    if (!source.data || source.count < 6 || !encoded.data || encoded.data == source.data || !result ||
        source[0] != 0xFF || source[1] != 0xD8) {
        return false;
    }
    u32 insertion = 2;
    if (source[2] == 0xFF && source[3] == 0xE0) {
        const u32 length = static_cast<u32>(source[4]) << 8u | source[5];
        if (length < 2 || 4u + length > source.count) {
            return false;
        }
        insertion = 4u + length;
    }
    u8 profile[160]{};
    profile[8] = 4;
    profile[9] = 0x30;
    write_be32(profile + 12, 0x6D6E7472u); // mntr
    write_be32(profile + 16, 0x47524159u); // GRAY
    write_be32(profile + 20, pcs_lab ? 0x4C616220u : 0x58595A20u);
    write_be32(profile + 36, 0x61637370u); // acsp
    write_be32(profile + 64, 1);
    write_be32(profile + 68, 0x0000F6D6u);
    write_be32(profile + 72, 0x00010000u);
    write_be32(profile + 76, 0x0000D32Du);
    write_be32(profile + 128, 1);
    write_be32(profile + 132, 0x6B545243u); // kTRC
    write_be32(profile + 136, 144);
    write_be32(profile + 140, 14);
    write_be32(profile + 144, 0x63757276u); // curv
    write_be32(profile + 152, 1);
    write_be16(profile + 156, 0x0100u); // gamma 1.0
    write_be32(profile, sizeof(profile));
    if (static_cast<u64>(source.count) + sizeof(profile) + 36u > encoded.count) {
        return false;
    }

    JpegIccFixtureResult output{};
    output.profile_bytes = sizeof(profile);
    const u32 first_bytes = sizeof(profile) / 2u - 3u;
    output.chunk_data_bytes[0] = static_cast<u16>(first_bytes);
    output.chunk_data_bytes[1] = static_cast<u16>(sizeof(profile) - first_bytes);
    u32 cursor = 0;
    if (!append_jpeg_bytes(encoded, &cursor, ConstSlice<u8>{source.data, insertion})) {
        return false;
    }
    for (u32 placement = 0; placement < 2; ++placement) {
        const u32 sequence = reverse_chunks ? 2u - placement : placement + 1u;
        const u32 profile_offset = sequence == 1 ? 0 : first_bytes;
        const u32 part_bytes = output.chunk_data_bytes[sequence - 1u];
        u8 payload[14 + sizeof(profile)]{};
        constexpr u8 signature[12] = {
            'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0,
        };
        for (u32 index = 0; index < sizeof(signature); ++index) {
            payload[index] = signature[index];
        }
        payload[12] = static_cast<u8>(sequence);
        payload[13] = 2;
        for (u32 index = 0; index < part_bytes; ++index) {
            payload[14u + index] = profile[profile_offset + index];
        }
        output.chunk_data_offsets[sequence - 1u] = cursor + 18u;
        if (!append_jpeg_segment(encoded, &cursor, 0xE2, ConstSlice<u8>{payload, 14u + part_bytes})) {
            return false;
        }
    }
    if (!append_jpeg_bytes(encoded, &cursor,
                           ConstSlice<u8>{source.data + insertion, source.count - insertion})) {
        return false;
    }
    output.encoded_bytes = cursor;
    *result = output;
    return true;
}

bool jpeg_icc_fixture_write_profile_byte(Slice<u8> encoded, const JpegIccFixtureResult& fixture,
                                         u32 profile_offset, u8 value) noexcept {
    if (!encoded.data || profile_offset >= fixture.profile_bytes) {
        return false;
    }
    u32 offset = profile_offset;
    for (u32 chunk = 0; chunk < 2; ++chunk) {
        if (offset < fixture.chunk_data_bytes[chunk]) {
            const u32 encoded_offset = fixture.chunk_data_offsets[chunk] + offset;
            if (encoded_offset >= encoded.count) {
                return false;
            }
            encoded[encoded_offset] = value;
            return true;
        }
        offset -= fixture.chunk_data_bytes[chunk];
    }
    return false;
}

u32 jpeg_fixture_rgba() noexcept {
    return 0xFF4774ADu;
}

u32 animated_webp_fixture_source_rgba(u32 frame) noexcept {
    constexpr u32 colors[4] = {
        0xFF0000FFu,
        0x8000FF00u,
        0xFFFF0000u,
        0x8000FFFFu,
    };
    return frame < 4 ? colors[frame] : 0;
}

u32 animated_webp_fixture_canvas_rgba(u32 presented_frame, u32 x, u32 y) noexcept {
    if (presented_frame > 3 || x >= 4 || y >= 4) {
        return 0;
    }
    const u32 red = 0xFF0000FFu;
    const u32 green = 0x8000FF00u;
    const u32 blue = 0xFFFF0000u;
    const u32 yellow = 0x8000FFFFu;
    const u32 background = premultiply_rgba(0x80302010u);
    u32 canvas = red;
    if (presented_frame >= 1 && x >= 2 && y < 2) {
        canvas = over_rgba(green, canvas);
    }
    if (presented_frame >= 2) {
        if (x >= 2 && y < 2) {
            canvas = background;
        }
        if (x < 2) {
            canvas = blue;
        }
    }
    if (presented_frame >= 3 && x >= 2 && y >= 2) {
        canvas = over_rgba(yellow, canvas);
    }
    return canvas;
}

u32 mixed_animated_webp_fixture_canvas_rgba(u32 presented_frame, u32 x, u32 y) noexcept {
    if (presented_frame > 2 || x >= 4 || y >= 4) {
        return 0;
    }
    if (presented_frame == 0) {
        return 0xFF0000FFu;
    }
    constexpr u32 lossy[16] = {
        0xFF000204u, 0xFF281E3Bu, 0xFF532C82u, 0xFF7C48B9u, 0xFF223B20u, 0xFF4B5657u,
        0xFF75649Du, 0xFF9E7FD4u, 0xFF397E2Au, 0xFF629A61u, 0xFF8CA8A6u, 0xFFB5C4DDu,
        0xFF59B645u, 0xFF82D17Cu, 0xFFADE1C2u, 0xFFD7FCF8u,
    };
    const u32 pixel = lossy[y * 4 + x];
    if (presented_frame == 1) {
        return pixel;
    }
    const u32 red = (pixel & 255u) * 128u + 127u;
    const u32 green = ((pixel >> 8) & 255u) * 128u + 127u;
    const u32 blue = ((pixel >> 16) & 255u) * 128u + 127u;
    return red / 255u | green / 255u << 8 | blue / 255u << 16 | 128u << 24;
}

} // namespace maicord
