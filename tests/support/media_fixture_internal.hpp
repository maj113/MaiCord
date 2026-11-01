#pragma once

#include "media_fixture.hpp"

namespace maicord {
namespace media_fixture_detail {

u32 adler32(const u8* bytes, u32 count) noexcept;

void write_be32(u8* output, u32 value) noexcept;

void write_be16(u8* output, u16 value) noexcept;

void write_le24(u8* output, u32 value) noexcept;

void write_le32(u8* output, u32 value) noexcept;

bool append_riff_chunk(Slice<u8> output, u32* cursor, u32 type, ConstSlice<u8> payload) noexcept;

bool begin_animated_webp(Slice<u8> encoded, u32* cursor, const u8 (&anim)[6]) noexcept;

void fill_unit_jpeg_tables(u8 (&quantization)[65], u8 (&huffman)[37]) noexcept;

bool append_jpeg_segment(Slice<u8> output, u32* cursor, u8 marker, ConstSlice<u8> payload) noexcept;

bool append_chunk(Slice<u8> output, u32* cursor, u32 type, ConstSlice<u8> data) noexcept;

bool begin_png(Slice<u8> encoded, u32* cursor, u32 width, u32 height, u8 bit_depth,
               u8 color_type) noexcept;

bool begin_apng(Slice<u8> encoded, u32* cursor, u32 frames, u32 plays) noexcept;

void write_stored_zlib_header(u8* output, u32 filtered_bytes) noexcept;

u8 channel_byte(u32 x_byte, u32 y, u32 width, u32 height, PngFixturePixel pixel) noexcept;

u8 paeth(u8 left, u8 above, u8 upper_left) noexcept;

u32 format_sample(u8 bit_depth, u32 channel, u32 x, u32 y) noexcept;

u32 indexed_palette_count(u8 bit_depth) noexcept;

u32 indexed_palette_rgba(u32 index) noexcept;

bool format_supported(u8 color_type, u8 bit_depth, u32* channels) noexcept;

u32 premultiply_rgba(u32 rgba) noexcept;

u32 over_rgba(u32 source, u32 destination) noexcept;

bool build_apng_zlib(u32 frame, u16 width, u16 height, Slice<u8> output, u32* bytes) noexcept;

bool append_fctl(Slice<u8> output, u32* cursor, u32 sequence, u32 width, u32 height, u32 x, u32 y,
                 u16 delay_num, u16 delay_den, u8 dispose, u8 blend) noexcept;

bool append_fdat(Slice<u8> output, u32* cursor, u32 sequence, ConstSlice<u8> zlib) noexcept;

bool append_bytes(Slice<u8> output, u32* cursor, ConstSlice<u8> bytes) noexcept;

bool gif_pack_code(Slice<u8> output, u32* byte_count, u32* bits, u32* bit_count, u32 code,
                   u32 code_size) noexcept;

bool append_gif_descriptor(Slice<u8> output, u32* cursor, u16 x, u16 y, u16 width, u16 height,
                           u8 packed) noexcept;

bool append_deferred_lzw(Slice<u8> output, u32* cursor, ConstSlice<u8> lzw) noexcept;

bool jpeg_icc_insertion_point(ConstSlice<u8> source, Slice<u8> encoded, const JpegIccFixtureResult* result,
                              u32* insertion) noexcept;

void write_icc_header(u8* profile, bool version_four, u32 device_class, u32 color_space, u32 pcs,
                      u32 tag_count) noexcept;

template <usize Capacity>
bool insert_jpeg_icc_profile(ConstSlice<u8> source, Slice<u8> encoded, u32 insertion,
                             const u8 (&profile)[Capacity], u32 profile_bytes, bool reverse_chunks,
                             JpegIccFixtureResult* result) noexcept {
    JpegIccFixtureResult output{};
    output.profile_bytes = profile_bytes;
    const u32 first_bytes = profile_bytes / 2u - 3u;
    output.chunk_data_bytes[0] = static_cast<u16>(first_bytes);
    output.chunk_data_bytes[1] = static_cast<u16>(profile_bytes - first_bytes);
    u32 cursor = 0;
    if (!append_bytes(encoded, &cursor, ConstSlice<u8>{source.data, insertion})) {
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
        if (!append_jpeg_segment(encoded, &cursor, 0xE2,
                                 ConstSlice<u8>{payload, 14u + part_bytes})) {
            return false;
        }
    }
    if (!append_bytes(encoded, &cursor,
                      ConstSlice<u8>{source.data + insertion, source.count - insertion})) {
        return false;
    }
    output.encoded_bytes = cursor;
    *result = output;
    return true;
}

} // namespace media_fixture_detail

using namespace media_fixture_detail;
} // namespace maicord
