#include "media_fixture_internal.hpp"
#include "../fixtures/webp_fixture.hpp"

namespace maicord {

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
    u32 cursor = 0;
    if (!begin_apng(encoded, &cursor, 4, 2) ||
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
    u32 cursor = 0;
    u32 default_bytes = 0;
    if (!begin_apng(encoded, &cursor, 1, 1) ||
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
    write_stored_zlib_header(zlib_scratch.data, filtered_bytes);
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

    u32 output_cursor = 0;
    if (!begin_png(encoded, &output_cursor, width, height, bit_depth, color_type)) {
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

    write_stored_zlib_header(zlib_scratch.data, filtered_bytes);
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

    u32 cursor = 0;
    const u32 split = minimum<u32>(11, zlib_bytes);
    if (!begin_png(encoded, &cursor, width, height, 8, 6) ||
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

    u32 cursor = 0;
    const u8 anim[6] = {48, 32, 16, 128, 2, 0};
    if (!begin_animated_webp(encoded, &cursor, anim)) {
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

    u32 cursor = 0;
    u8 anim[6]{};
    anim[4] = 1;
    if (!begin_animated_webp(encoded, &cursor, anim)) {
        return false;
    }

    for (u32 frame = 0; frame < 3; ++frame) {
        u8 anmf[160]{};
        write_le24(anmf + 6, 3);
        write_le24(anmf + 9, 3);
        write_le24(anmf + 12, 30 + frame * 10);
        anmf[15] = frame == 1 ? 3 : frame == 0 ? 2 : 0;
        u32 frame_cursor = 16;
        const Slice<u8> frame_payload{anmf, sizeof(anmf)};
        const ConstSlice<u8> lossy{kWebpLossy4x4 + lossy_payload_offset, lossy_payload_bytes};
        const bool built =
            frame == 0 ? append_riff_chunk(frame_payload, &frame_cursor, 0x4C385056u,
                                           ConstSlice<u8>{lossless_red, sizeof(lossless_red)})
                       : (frame != 2 || append_riff_chunk(frame_payload, &frame_cursor, 0x48504C41u,
                                                          ConstSlice<u8>{alpha, sizeof(alpha)})) &&
                             append_riff_chunk(frame_payload, &frame_cursor, 0x20385056u, lossy);
        if (!built ||
            !append_riff_chunk(encoded, &cursor, 0x464D4E41u, ConstSlice<u8>{anmf, frame_cursor})) {
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
    // Original opaque VP8 frames with the redundant alpha hint seen in public
    // animated preview containers. No provider payload is embedded in tests.
    u32 cursor = 0;
    const u8 anim[6] = {0, 0, 0, 255, 2, 0};
    if (!begin_animated_webp(encoded, &cursor, anim)) {
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
    u8 frame[] = {
        8, 0, 8, 0, 8, 3, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0,
    };
    if (restart_markers) {
        frame[3] = 0;
        frame[4] = 16;
    }
    u8 huffman[37]{};
    fill_unit_jpeg_tables(quantization, huffman);
    constexpr u8 restart_interval[] = {0, 1};
    constexpr u8 scan[] = {
        3, 1, 0, 2, 0, 3, 0, 0, 63, 0,
    };
    constexpr u8 mcu[] = {0x27, 0xFA, 0x80, 0x3F};
    constexpr u8 restart[] = {0xFF, 0xD0};
    constexpr u8 eoi[] = {0xFF, 0xD9};

    u32 cursor = 0;
    if (!append_bytes(encoded, &cursor, ConstSlice<u8>{soi, sizeof(soi)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xE0, ConstSlice<u8>{jfif, sizeof(jfif)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xDB, ConstSlice<u8>{quantization, sizeof(quantization)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xC0, ConstSlice<u8>{frame, sizeof(frame)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xC4, ConstSlice<u8>{huffman, sizeof(huffman)}) ||
        (restart_markers &&
         !append_jpeg_segment(encoded, &cursor, 0xDD,
                              ConstSlice<u8>{restart_interval, sizeof(restart_interval)})) ||
        !append_jpeg_segment(encoded, &cursor, 0xDA, ConstSlice<u8>{scan, sizeof(scan)}) ||
        !append_bytes(encoded, &cursor, ConstSlice<u8>{mcu, sizeof(mcu)}) ||
        (restart_markers &&
         (!append_bytes(encoded, &cursor, ConstSlice<u8>{restart, sizeof(restart)}) ||
          !append_bytes(encoded, &cursor, ConstSlice<u8>{mcu, sizeof(mcu)}))) ||
        !append_bytes(encoded, &cursor, ConstSlice<u8>{eoi, sizeof(eoi)})) {
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
    u8 huffman[37]{};
    fill_unit_jpeg_tables(quantization, huffman);
    constexpr u8 frame[] = {8, 0, 8, 0, 8, 1, 1, 0x11, 0};
    constexpr u8 scan[] = {1, 1, 0, 0, 63, 0};
    constexpr u8 mcu[] = {0x3F};
    constexpr u8 eoi[] = {0xFF, 0xD9};
    u32 cursor = 0;
    if (!append_bytes(encoded, &cursor, ConstSlice<u8>{soi, sizeof(soi)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xE0, ConstSlice<u8>{jfif, sizeof(jfif)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xDB, ConstSlice<u8>{quantization, sizeof(quantization)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xC0, ConstSlice<u8>{frame, sizeof(frame)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xC4, ConstSlice<u8>{huffman, sizeof(huffman)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xDA, ConstSlice<u8>{scan, sizeof(scan)}) ||
        !append_bytes(encoded, &cursor, ConstSlice<u8>{mcu, sizeof(mcu)}) ||
        !append_bytes(encoded, &cursor, ConstSlice<u8>{eoi, sizeof(eoi)})) {
        return false;
    }
    *result = JpegFixtureResult{cursor, 8, 8, 0};
    return true;
}

bool build_marked_jpeg_fixture(Slice<u8> encoded, u16 width, u16 height, JpegFixtureResult* result) noexcept {
    if (!encoded.data || !result || !width || !height || (width | height) % 8u) {
        return false;
    }
    constexpr u8 soi[] = {0xFF, 0xD8};
    u8 quantization[65]{};
    u8 huffman[37]{};
    fill_unit_jpeg_tables(quantization, huffman);
    const u8 frame[] = {8, static_cast<u8>(height >> 8), static_cast<u8>(height), static_cast<u8>(width >> 8),
                        static_cast<u8>(width), 3, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0};
    constexpr u8 scan[] = {3, 1, 0, 2, 0, 3, 0, 0, 63, 0};
    constexpr u8 eoi[] = {0xFF, 0xD9};
    u32 cursor = 0;
    if (!append_bytes(encoded, &cursor, ConstSlice<u8>{soi, sizeof(soi)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xDB, ConstSlice<u8>{quantization, sizeof(quantization)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xC0, ConstSlice<u8>{frame, sizeof(frame)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xC4, ConstSlice<u8>{huffman, sizeof(huffman)}) ||
        !append_jpeg_segment(encoded, &cursor, 0xDA, ConstSlice<u8>{scan, sizeof(scan)})) {
        return false;
    }
    // The first MCU's luma DC rises by 256 (+32 levels) and the second's falls
    // back: one bright 8x8 block at the top-left of a mid-gray image.
    u32 bits = 0, pending = 0;
    bool fits = true;
    const auto put = [&](u32 value, u32 count) noexcept {
        for (u32 bit = count; bit-- > 0;) {
            pending = (pending << 1) | ((value >> bit) & 1u);
            if (++bits == 8) {
                fits = fits && cursor < encoded.count;
                if (fits)
                    encoded[cursor++] = static_cast<u8>(pending);
                bits = pending = 0;
            }
        }
    };
    const u32 mcus = (width / 8u) * (height / 8u);
    for (u32 mcu = 0; mcu < mcus; ++mcu) {
        if (mcu == 0)
            put(0b10'100000000, 11);
        else if (mcu == 1)
            put(0b10'011111111, 11);
        else
            put(0, 1);
        put(0, 5); // Luma EOB, then chroma DC unchanged and EOB twice.
    }
    if (bits)
        put(0xFF, 8 - bits);
    if (!fits || !append_bytes(encoded, &cursor, ConstSlice<u8>{eoi, sizeof(eoi)})) {
        return false;
    }
    *result = JpegFixtureResult{cursor, width, height, 0};
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
    if (!append_bytes(encoded, &cursor, ConstSlice<u8>{source.data, 2}) ||
        !append_jpeg_segment(encoded, &cursor, 0xE1, ConstSlice<u8>{exif, sizeof(exif)}) ||
        !append_bytes(encoded, &cursor, ConstSlice<u8>{source.data + 2, source.count - 2})) {
        return false;
    }
    *encoded_bytes = cursor;
    return true;
}

bool build_jpeg_icc_matrix_trc_fixture(ConstSlice<u8> source, Slice<u8> encoded, JpegIccFixtureCurve curve,
                                       bool version_four, bool reverse_chunks,
                                       JpegIccFixtureResult* result) noexcept {
    u32 insertion = 0;
    if (static_cast<u8>(curve) > static_cast<u8>(JpegIccFixtureCurve::sampled_srgb) ||
        !jpeg_icc_insertion_point(source, encoded, result, &insertion)) {
        return false;
    }

    u8 profile[768]{};
    write_icc_header(profile, version_four, 0x6D6E7472u, 0x52474220u, 0x58595A20u, 6);
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

    return insert_jpeg_icc_profile(source, encoded, insertion, profile, profile_cursor,
                                   reverse_chunks, result);
}

bool build_jpeg_icc_lut_fixture(ConstSlice<u8> source, Slice<u8> encoded,
                                JpegIccLutFixtureSpace color_space, JpegIccLutFixturePrecision precision,
                                bool reverse_chunks, JpegIccFixtureResult* result) noexcept {
    u32 insertion = 0;
    if (static_cast<u8>(color_space) > static_cast<u8>(JpegIccLutFixtureSpace::cmyk) ||
        static_cast<u8>(precision) > static_cast<u8>(JpegIccLutFixturePrecision::mft2) ||
        !jpeg_icc_insertion_point(source, encoded, result, &insertion)) {
        return false;
    }

    const bool cmyk = color_space == JpegIccLutFixtureSpace::cmyk;
    const bool mft2 = precision == JpegIccLutFixturePrecision::mft2;
    const u32 input_channels = cmyk ? 4u : 3u;
    const u32 table_entries = mft2 ? 2u : 256u;
    const u32 precision_bytes = mft2 ? 2u : 1u;
    const u32 clut_entries = (1u << input_channels) * 3u;

    u8 profile[2'048]{};
    write_icc_header(profile, mft2, cmyk ? 0x70727472u : 0x6D6E7472u, cmyk ? 0x434D594Bu : 0x52474220u,
                     0x4C616220u, 1);
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
    const auto put_sample = [&](u32 value) noexcept {
        if (mft2) {
            write_be16(profile + data, static_cast<u16>(value));
            data += 2u;
        } else {
            profile[data++] = static_cast<u8>(value);
        }
    };
    const auto put_identity_curves = [&](u32 count) noexcept {
        for (u32 channel = 0; channel < count; ++channel) {
            for (u32 entry = 0; entry < table_entries; ++entry) {
                put_sample(table_entries == 2 ? entry * 65'535u : entry);
            }
        }
    };
    put_identity_curves(input_channels);
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
            put_sample(static_cast<u32>(lab[output] * encoded_maximum + 0.5f));
        }
    }
    put_identity_curves(3);
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

    return insert_jpeg_icc_profile(source, encoded, insertion, profile, profile_bytes,
                                   reverse_chunks, result);
}

bool build_jpeg_icc_gray_trc_fixture(ConstSlice<u8> source, Slice<u8> encoded, bool pcs_lab,
                                     bool reverse_chunks, JpegIccFixtureResult* result) noexcept {
    u32 insertion = 0;
    if (!jpeg_icc_insertion_point(source, encoded, result, &insertion)) {
        return false;
    }
    u8 profile[160]{};
    write_icc_header(profile, true, 0x6D6E7472u, 0x47524159u, pcs_lab ? 0x4C616220u : 0x58595A20u, 1);
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

    return insert_jpeg_icc_profile(source, encoded, insertion, profile, sizeof(profile),
                                   reverse_chunks, result);
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
