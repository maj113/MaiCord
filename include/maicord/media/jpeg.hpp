#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kJpegEncodedMaximumBytes = 2u * 1'024u * 1'024u;
inline constexpr u16 kJpegDimensionMaximum = 512;
inline constexpr u32 kJpegComponentMaximum = 4;
inline constexpr u32 kJpegTableMaximum = 4;
inline constexpr u32 kJpegProgressiveScanMaximum = 64;
inline constexpr u32 kJpegIccChunkMaximum = 255;
inline constexpr u32 kJpegIccProfileMaximumBytes = 1u * 1'024u * 1'024u;
inline constexpr u32 kJpegIccCurveEntryMaximum = 4'096;
inline constexpr u32 kJpegIccCurveChannelCount = 3;
inline constexpr u32 kJpegIccInputChannelMaximum = 4;
inline constexpr u32 kJpegIccLutGridMaximum = 16;
inline constexpr u32 kJpegIccLutClutEntryMaximum = kJpegIccLutGridMaximum * kJpegIccLutGridMaximum *
                                                   kJpegIccLutGridMaximum * kJpegIccLutGridMaximum *
                                                   kJpegIccCurveChannelCount;
inline constexpr u32 kJpegIccEntryMaximum =
    (kJpegIccInputChannelMaximum + kJpegIccCurveChannelCount) * kJpegIccCurveEntryMaximum +
    kJpegIccLutClutEntryMaximum;
inline constexpr u32 kJpegCoefficientBlockMaximum =
    kJpegComponentMaximum * (kJpegDimensionMaximum / 8u) * (kJpegDimensionMaximum / 8u);

enum class JpegColorTransform : u8 {
    grayscale,
    ycbcr,
    rgb,
    // Chromium-compatible inverted CMYK. This also applies when APP14 is
    // absent, as in the official web client.
    cmyk,
    ycck,
};

// CIPA Exif Orientation tag values. The names describe which stored edge
// occupies the visual top/left position.
enum class JpegOrientation : u8 {
    top_left = 1,
    top_right = 2,
    bottom_right = 3,
    bottom_left = 4,
    left_top = 5,
    right_top = 6,
    right_bottom = 7,
    left_bottom = 8,
};

enum class JpegIccDisposition : u8 {
    none,
    applied,
    incomplete_ignored,
    malformed_ignored,
    unsupported_ignored,
};

enum class JpegIccCurveKind : u8 {
    identity,
    parametric,
    table,
};

enum class JpegIccTransformKind : u8 {
    none,
    matrix_trc,
    gray_trc,
    lut,
};

enum class JpegIccPcs : u8 {
    xyz,
    lab,
};

enum class JpegIccDataColorSpace : u8 {
    gray,
    rgb,
    cmyk,
};

struct JpegIccCurve {
    // ICC parametric curves are normalized to the skcms-compatible form:
    // x < d ? c*x+f : (a*x+b)^g+e. Table offsets are u16 indices.
    float g;
    float a;
    float b;
    float c;
    float d;
    float e;
    float f;
    u32 table_offset;
    u16 table_count;
    JpegIccCurveKind kind;
    u8 reserved;
};

struct JpegIccLut {
    JpegIccCurve input_curves[kJpegIccInputChannelMaximum];
    JpegIccCurve output_curves[kJpegIccCurveChannelCount];
    u32 clut_offset;
    u32 clut_entry_count;
    u8 grid_points[kJpegIccInputChannelMaximum];
    u8 input_channels;
    u8 output_channels;
    // One for mft1 and two for mft2. This also selects the specified
    // modern versus legacy normalized PCSLAB encoding.
    u8 precision_bytes;
    u8 reserved;
};

struct JpegIccProfile {
    const u16* entries;
    // Row-major transform to linear sRGB. Matrix/TRC profiles precombine
    // source RGB-to-XYZD50; gray/LUT profiles use XYZD50-to-sRGB.
    float to_linear_srgb[9];
    JpegIccCurve curves[kJpegIccCurveChannelCount];
    JpegIccLut lut;
    u32 profile_bytes;
    u32 entry_count;
    u16 chunk_count;
    JpegIccDisposition disposition;
    u8 major_version;
    JpegIccTransformKind transform_kind;
    JpegIccPcs pcs;
    JpegIccDataColorSpace data_color_space;
    u8 reserved;
};

struct JpegIccWorkspace {
    JpegIccProfile profile;
    u16 entries[kJpegIccEntryMaximum];
};

constexpr bool jpeg_orientation_valid(JpegOrientation orientation) noexcept {
    return static_cast<u8>(orientation) >= static_cast<u8>(JpegOrientation::top_left) &&
           static_cast<u8>(orientation) <= static_cast<u8>(JpegOrientation::left_bottom);
}

constexpr bool jpeg_orientation_swaps_dimensions(JpegOrientation orientation) noexcept {
    return static_cast<u8>(orientation) >= static_cast<u8>(JpegOrientation::left_top);
}

enum class JpegError : u8 {
    none,
    invalid_input,
    encoded_limit,
    truncated,
    signature,
    marker,
    segment,
    order,
    duplicate,
    unsupported_process,
    unsupported_precision,
    unsupported_components,
    unsupported_sampling,
    unsupported_color,
    dimensions,
    quantization,
    huffman,
    scan,
    restart,
    entropy,
    coefficient,
    trailing_data,
    sink,
};

struct JpegQuantizationTable {
    u16 values[64];
    bool present;
    u8 reserved;
};

struct JpegHuffmanTable {
    i32 minimum_code[16];
    i32 maximum_code[16];
    i32 value_offset[16];
    u8 symbols[256];
    u16 symbol_count;
    bool present;
    u8 reserved;
};

struct JpegTables {
    JpegQuantizationTable quantization[kJpegTableMaximum];
    JpegHuffmanTable dc[kJpegTableMaximum];
    JpegHuffmanTable ac[kJpegTableMaximum];
    JpegIccWorkspace* icc;
};

struct JpegComponent {
    u32 block_offset;
    u32 block_count;
    u16 block_columns;
    u16 block_rows;
    u8 identifier;
    u8 horizontal_sampling;
    u8 vertical_sampling;
    u8 quantization_table;
    u8 dc_table;
    u8 ac_table;
    u8 reserved[2];
};

struct JpegImage {
    JpegTables* tables;
    JpegComponent components[kJpegComponentMaximum];
    u32 entropy_offset;
    u32 entropy_bytes;
    u32 coefficient_blocks;
    u16 width;
    u16 height;
    u16 mcu_columns;
    u16 mcu_rows;
    u32 restart_markers;
    u16 restart_interval;
    u16 scan_count;
    u8 component_count;
    u8 maximum_horizontal_sampling;
    u8 maximum_vertical_sampling;
    JpegColorTransform color_transform;
    bool has_jfif;
    bool has_adobe;
    bool progressive;
    u8 scan_order[kJpegComponentMaximum];
    JpegOrientation orientation;
};

constexpr u16 jpeg_display_width(const JpegImage& image) noexcept {
    return jpeg_orientation_swaps_dimensions(image.orientation) ? image.height : image.width;
}

constexpr u16 jpeg_display_height(const JpegImage& image) noexcept {
    return jpeg_orientation_swaps_dimensions(image.orientation) ? image.width : image.height;
}

struct JpegCoefficientBlock {
    // Quantized coefficients in raster order. Dequantization and IDCT are
    // deferred to the hardware presentation stage.
    i16 values[64];
};

struct JpegCoefficientWorkspace {
    JpegCoefficientBlock blocks[kJpegCoefficientBlockMaximum];
};

struct JpegWorkspace {
    JpegTables tables;
    JpegCoefficientWorkspace coefficients;
    JpegIccWorkspace icc;
};

struct JpegParseResult {
    u32 segments;
    u32 restart_markers;
    u32 scans;
    JpegError error;

    constexpr bool ok() const noexcept {
        return error == JpegError::none;
    }
};

struct JpegDecodeResult {
    JpegParseResult parse;
    u32 decoded_mcus;
    u32 decoded_blocks;
    u32 huffman_symbols;
    u32 decoded_scans;
    JpegError error;

    constexpr bool ok() const noexcept {
        return error == JpegError::none;
    }
};

JpegParseResult jpeg_parse(ConstSlice<u8> encoded, JpegImage* image, JpegWorkspace* workspace) noexcept;
JpegDecodeResult jpeg_decode_parsed_coefficients(ConstSlice<u8> encoded, const JpegImage& image,
                                                 JpegCoefficientWorkspace* coefficients) noexcept;
JpegDecodeResult jpeg_decode_coefficients(ConstSlice<u8> encoded, JpegImage* image,
                                          JpegWorkspace* workspace) noexcept;

// Local-file thumbnails (composer upload previews). The parse accepts photos
// up to 16384 px per side; decoding reconstructs each 8x8 block at 1, 2, 4
// or 8 samples per axis (box average of the exact IDCT, DC only at 1) into
// per-component planes, then converts to straight RGBA8. Baseline scans
// stream without coefficient storage; progressive scans retain coefficients.
// EXIF orientation is reported, not applied.
inline constexpr u32 kJpegThumbnailEncodedMaximumBytes = 48u * 1'024u * 1'024u;
inline constexpr u32 kJpegThumbnailDimensionMaximum = 16'384;
inline constexpr u64 kJpegThumbnailScratchMaximumBytes = 160ull * 1'024u * 1'024u;
struct JpegThumbnailLayout {
    u32 width;
    u32 height;
    u64 coefficient_bytes;
    u64 plane_bytes;
    u64 rgba_bytes;
    u32 plane_offsets[kJpegComponentMaximum];
    u32 plane_pitches[kJpegComponentMaximum];
    u8 samples;
};
// samples is the smallest per-block reconstruction reaching target_extent.
JpegParseResult jpeg_thumbnail_parse(ConstSlice<u8> encoded, u32 target_extent, JpegImage* image,
                                     JpegWorkspace* workspace, JpegThumbnailLayout* layout) noexcept;
// scratch holds coefficient_bytes + plane_bytes (16-byte aligned); rgba holds rgba_bytes.
JpegDecodeResult jpeg_thumbnail_decode(ConstSlice<u8> encoded, const JpegImage& image,
                                       const JpegThumbnailLayout& layout, Slice<u8> scratch,
                                       Slice<u8> rgba) noexcept;
using JpegThumbnailRowSink = bool (*)(void* context, u32 row_index, const u8* rgba) noexcept;
// The row holds width*4 bytes, with no alignment requirement. The synchronous
// sink consumes each row before the next row overwrites it. Rows start only
// after entropy decode succeeds. A false callback stops output with sink error.
JpegDecodeResult jpeg_thumbnail_decode_rows(ConstSlice<u8> encoded, const JpegImage& image,
                                            const JpegThumbnailLayout& layout, Slice<u8> scratch,
                                            Slice<u8> row, JpegThumbnailRowSink sink, void* context) noexcept;

static_assert(sizeof(JpegQuantizationTable) == 130);
static_assert(sizeof(JpegHuffmanTable) == 452);
static_assert(sizeof(JpegComponent) == 20);
static_assert(sizeof(JpegImage) == 128);
static_assert(sizeof(JpegIccCurve) == 36);
static_assert(sizeof(JpegIccLut) == 268);
static_assert(sizeof(JpegIccProfile) == 440);
static_assert(sizeof(JpegIccWorkspace) == 451'000);
static_assert(sizeof(JpegCoefficientBlock) == 128);
static_assert(sizeof(JpegCoefficientWorkspace) == 2'097'152);

} // namespace maicord
