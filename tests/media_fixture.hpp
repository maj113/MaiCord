#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct PngFixtureResult {
    u32 encoded_bytes;
    u32 zlib_bytes;
    u32 filtered_bytes;
};

struct ApngFixtureResult {
    u32 encoded_bytes;
    u32 frame_count;
    u32 zlib_bytes;
};

struct GifFixtureResult {
    u32 encoded_bytes;
    u32 frame_count;
    u32 lzw_bytes;
};

struct AnimatedWebpFixtureResult {
    u32 encoded_bytes;
    u32 frame_count;
};

struct JpegFixtureResult {
    u32 encoded_bytes;
    u16 width;
    u16 height;
    u16 restart_interval;
};

enum class JpegIccFixtureCurve : u8 {
    parametric_srgb,
    sampled_srgb,
};

enum class JpegIccLutFixtureSpace : u8 {
    rgb,
    cmyk,
};

enum class JpegIccLutFixturePrecision : u8 {
    mft1,
    mft2,
};

struct JpegIccFixtureResult {
    u32 encoded_bytes;
    u32 profile_bytes;
    u32 chunk_data_offsets[2];
    u16 chunk_data_bytes[2];
};

using PngFixturePixel = u32 (*)(u32 x, u32 y, u32 width, u32 height) noexcept;
bool build_rgba8_png_fixture(u16 width, u16 height, Slice<u8> encoded, Slice<u8> zlib_scratch,
                             PngFixtureResult* result, PngFixturePixel pixel = nullptr) noexcept;

bool build_png_format_fixture(u8 color_type, u8 bit_depth, u16 width, u16 height, Slice<u8> encoded,
                              Slice<u8> zlib_scratch, PngFixtureResult* result) noexcept;
bool build_rgba8_apng_fixture(Slice<u8> encoded, Slice<u8> zlib_scratch,
                              ApngFixtureResult* result) noexcept;
bool build_rgba8_apng_default_fixture(Slice<u8> encoded, Slice<u8> zlib_scratch,
                                      ApngFixtureResult* result) noexcept;
bool build_indexed_gif_fixture(Slice<u8> encoded, Slice<u8> lzw_scratch, GifFixtureResult* result) noexcept;
bool build_deferred_clear_gif_fixture(Slice<u8> encoded, Slice<u8> lzw_scratch,
                                      GifFixtureResult* result, u16 width = 128, u16 height = 64) noexcept;
bool build_lossless_animated_webp_fixture(Slice<u8> encoded, AnimatedWebpFixtureResult* result) noexcept;
bool build_mixed_animated_webp_fixture(Slice<u8> encoded, AnimatedWebpFixtureResult* result) noexcept;
bool build_opaque_lossy_animated_webp_fixture(Slice<u8> encoded,
                                              AnimatedWebpFixtureResult* result) noexcept;
bool build_baseline_jpeg_fixture(Slice<u8> encoded, bool restart_markers,
                                 JpegFixtureResult* result) noexcept;
bool build_grayscale_jpeg_fixture(Slice<u8> encoded, JpegFixtureResult* result) noexcept;
bool build_jpeg_exif_orientation_fixture(ConstSlice<u8> source, Slice<u8> encoded, u8 orientation,
                                         bool big_endian, u32* encoded_bytes) noexcept;
bool build_jpeg_icc_matrix_trc_fixture(ConstSlice<u8> source, Slice<u8> encoded, JpegIccFixtureCurve curve,
                                       bool version_four, bool reverse_chunks,
                                       JpegIccFixtureResult* result) noexcept;
bool build_jpeg_icc_lut_fixture(ConstSlice<u8> source, Slice<u8> encoded,
                                JpegIccLutFixtureSpace color_space, JpegIccLutFixturePrecision precision,
                                bool reverse_chunks, JpegIccFixtureResult* result) noexcept;
bool build_jpeg_icc_gray_trc_fixture(ConstSlice<u8> source, Slice<u8> encoded, bool pcs_lab,
                                     bool reverse_chunks, JpegIccFixtureResult* result) noexcept;
bool jpeg_icc_fixture_write_profile_byte(Slice<u8> encoded, const JpegIccFixtureResult& fixture,
                                         u32 profile_offset, u8 value) noexcept;

u32 png_fixture_rgba(u32 x, u32 y, u32 width, u32 height) noexcept;
u32 png_format_fixture_rgba(u8 color_type, u8 bit_depth, u32 x, u32 y) noexcept;
u32 apng_fixture_source_rgba(u32 frame, u32 x, u32 y) noexcept;
u32 apng_fixture_canvas_rgba(u32 presented_frame, u32 x, u32 y) noexcept;
u8 gif_fixture_source_index(u32 frame, u32 x, u32 y) noexcept;
u32 gif_fixture_palette_rgba(u32 frame, u32 index) noexcept;
u32 gif_fixture_canvas_rgba(u32 presented_frame, u32 x, u32 y) noexcept;
u32 animated_webp_fixture_source_rgba(u32 frame) noexcept;
u32 animated_webp_fixture_canvas_rgba(u32 presented_frame, u32 x, u32 y) noexcept;
u32 mixed_animated_webp_fixture_canvas_rgba(u32 presented_frame, u32 x, u32 y) noexcept;
u32 jpeg_fixture_rgba() noexcept;
u32 png_fixture_crc32(ConstSlice<u8> bytes) noexcept;
u32 png_fixture_adler32(ConstSlice<u8> bytes) noexcept;
void png_fixture_write_be32(u8* output, u32 value) noexcept;

} // namespace maicord
