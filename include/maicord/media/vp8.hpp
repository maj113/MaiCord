#pragma once

#include "maicord/media/webp.hpp"

namespace maicord {

enum class Vp8Error : u8 {
    none,
    invalid_input,
    encoded_limit,
    truncated,
    interframe,
    unsupported_version,
    invisible_frame,
    signature,
    dimensions,
    first_partition,
};

enum class Vp8BoolError : u8 {
    none,
    invalid_input,
    truncated,
    literal_width,
};

enum class Vp8ControlError : u8 {
    none,
    invalid_input,
    boolean_stream,
    token_partition_table,
    token_partition_bounds,
};

enum class Vp8ModeError : u8 {
    none,
    invalid_input,
    dimensions,
    boolean_stream,
};

enum class Vp8CoefficientError : u8 {
    none,
    invalid_input,
    dimensions,
    token_initialize,
    boolean_stream,
    coefficient_range,
};

enum class Vp8IntraMode : u8 {
    dc = 0,
    true_motion = 1,
    vertical = 2,
    horizontal = 3,
    subblocks = 10,
};

enum class Vp8SubblockMode : u8 {
    dc = 0,
    true_motion = 1,
    vertical_edge = 2,
    horizontal_edge = 3,
    right_down = 4,
    vertical_right = 5,
    left_down = 6,
    vertical_left = 7,
    horizontal_down = 8,
    horizontal_up = 9,
};

struct Vp8BoolReader {
    ConstSlice<u8> bytes;
    u32 byte_offset;
    u32 value;
    u32 range;
    u8 bit_count;
    Vp8BoolError error;
    u16 reserved;

    constexpr bool ok() const noexcept {
        return error == Vp8BoolError::none;
    }
};

struct Vp8Keyframe {
    ConstSlice<u8> first_partition;
    ConstSlice<u8> remaining_data;
    u32 payload_bytes;
    u32 first_partition_bytes;
    u16 width;
    u16 height;
    u8 version;
    u8 horizontal_scale;
    u8 vertical_scale;
    u8 reserved;
};

struct Vp8SegmentationHeader {
    i32 quantizer[4];
    i32 filter_strength[4];
    u8 tree_probability[3];
    u8 enabled;
    u8 update_map;
    u8 update_data;
    u8 absolute_delta;
    u8 reserved;
};

struct Vp8LoopFilterHeader {
    i32 reference_delta[4];
    i32 mode_delta[4];
    u8 level;
    u8 sharpness;
    u8 simple;
    u8 use_delta;
    u8 update_delta;
    u8 reserved[3];
};

struct Vp8QuantizationHeader {
    i32 y1_dc_delta;
    i32 y2_dc_delta;
    i32 y2_ac_delta;
    i32 uv_dc_delta;
    i32 uv_ac_delta;
    u8 base_quantizer;
    u8 reserved[3];
};

struct Vp8TokenPartition {
    ConstSlice<u8> bytes;
};

inline constexpr u32 kVp8CoefficientProbabilityCount =
    4u * 8u * 3u * 11u;

struct Vp8ProbabilityWorkspace {
    // [block type][coefficient band][previous-coefficient context][tree node]
    u8 coefficient[kVp8CoefficientProbabilityCount];
};

struct Vp8ControlHeader {
    // Positioned at the first macroblock mode after successful parsing.
    Vp8BoolReader macroblock_reader;
    Vp8TokenPartition token_partitions[8];
    Vp8SegmentationHeader segmentation;
    Vp8LoopFilterHeader loop_filter;
    Vp8QuantizationHeader quantization;
    u32 coefficient_probability_updates;
    u8 color_space;
    u8 clamping_type;
    u8 token_partition_count;
    u8 refresh_entropy_probabilities;
    u8 use_skip_probability;
    u8 skip_probability;
    u8 reserved[2];
};

inline constexpr u32 kVp8MacroblockDimensionMaximum =
    (kWebpDimensionMaximum + 15u) / 16u;
inline constexpr u32 kVp8MacroblockMaximum =
    kVp8MacroblockDimensionMaximum * kVp8MacroblockDimensionMaximum;

struct Vp8MacroblockMode {
    Vp8SubblockMode luma_subblock[16];
    Vp8IntraMode luma;
    Vp8IntraMode chroma;
    u8 segment_id;
    u8 skip_coefficients;
};

struct Vp8ModeWorkspace {
    Vp8BoolReader reader;
    Vp8MacroblockMode macroblocks[kVp8MacroblockMaximum];
    u32 macroblock_count;
    u16 columns;
    u16 rows;
};

struct Vp8TokenContext {
    u8 y[4];
    u8 u[2];
    u8 v[2];
    u8 y2;
};

struct Vp8MacroblockCoefficients {
    // 16 luma blocks, then four U and four V blocks. Each block is
    // dequantized and stored in raster coefficient order for GPU IDCT.
    i16 values[24][16];
    u8 has_coefficients;
    u8 reserved;
};

struct Vp8CoefficientWorkspace {
    Vp8BoolReader token_reader[8];
    Vp8MacroblockCoefficients macroblocks[kVp8MacroblockMaximum];
    Vp8TokenContext top_context[kVp8MacroblockDimensionMaximum];
    Vp8TokenContext left_context;
    u32 macroblock_count;
    u16 columns;
    u16 rows;
};

struct Vp8ParseResult {
    Vp8Error error;

    constexpr bool ok() const noexcept {
        return error == Vp8Error::none;
    }
};

struct Vp8ControlParseResult {
    Vp8ControlError error;
    Vp8BoolError boolean_error;

    constexpr bool ok() const noexcept {
        return error == Vp8ControlError::none;
    }
};

struct Vp8ModeParseResult {
    Vp8ModeError error;
    Vp8BoolError boolean_error;

    constexpr bool ok() const noexcept {
        return error == Vp8ModeError::none;
    }
};

struct Vp8CoefficientParseResult {
    Vp8CoefficientError error;
    Vp8BoolError boolean_error;

    constexpr bool ok() const noexcept {
        return error == Vp8CoefficientError::none;
    }
};

// Parses the uncompressed ten-byte header of one complete VP8 keyframe
// payload and bounds its first boolean-coded partition. It does not claim
// that the remaining compressed syntax has decoded successfully.
Vp8ParseResult vp8_parse_keyframe(ConstSlice<u8> payload,
                                  Vp8Keyframe* frame) noexcept;
// Parses RFC 6386 sections 9.2 through 9.11 for a still-image keyframe,
// validates and slices all coefficient token partitions, and leaves the
// bounded boolean reader positioned at the first macroblock mode. The
// final coefficient probability values are retained in the caller-owned
// workspace for the later token decoder rather than bloating this header.
Vp8ControlParseResult vp8_parse_keyframe_control(
    const Vp8Keyframe& frame, Vp8ControlHeader* header,
    Vp8ProbabilityWorkspace* probabilities) noexcept;
Vp8ModeParseResult vp8_parse_keyframe_modes(
    const Vp8Keyframe& frame, const Vp8ControlHeader& header,
    Vp8ModeWorkspace* workspace) noexcept;
Vp8CoefficientParseResult vp8_decode_keyframe_coefficients(
    const Vp8Keyframe& frame, const Vp8ControlHeader& header,
    const Vp8ProbabilityWorkspace& probabilities,
    const Vp8ModeWorkspace& modes,
    Vp8CoefficientWorkspace* workspace) noexcept;
bool vp8_bool_reader_initialize(Vp8BoolReader* reader,
                                ConstSlice<u8> bytes) noexcept;

// CPU thumbnails of keyframes beyond the 512 px GPU path: the same header
// checks with VP8's own 16383 px bound and the caller's encoded limit.
inline constexpr u32 kVp8DimensionMaximum = 16383;
Vp8ParseResult vp8_parse_keyframe_bounded(ConstSlice<u8> payload, u32 maximum_bytes,
                                          Vp8Keyframe* frame) noexcept;

// Row-streaming keyframe syntax: one macroblock row of modes and dequantized
// residuals at a time, so frame size never bounds memory. The caller owns
// the per-column contexts (columns entries each) for the parser's lifetime.
struct Vp8RowParser {
    Vp8BoolReader modes;
    Vp8BoolReader tokens[8];
    Vp8TokenContext* top_context;
    Vp8SubblockMode* above_modes; // columns * 4: bottom subblock modes of the row above
    Vp8TokenContext left_context;
    u32 columns;
    u32 rows;
    u32 row;
    u32 partitions;
};
Vp8CoefficientParseResult vp8_row_parser_initialize(Vp8RowParser* parser, const Vp8Keyframe& frame,
                                                    const Vp8ControlHeader& header,
                                                    Slice<Vp8TokenContext> top_context,
                                                    Slice<Vp8SubblockMode> above_modes) noexcept;
// Parses the next row into modes[0, columns) and coefficients[0, columns).
Vp8CoefficientParseResult vp8_row_parser_next(Vp8RowParser* parser, const Vp8ControlHeader& header,
                                              const Vp8ProbabilityWorkspace& probabilities,
                                              Slice<Vp8MacroblockMode> modes,
                                              Slice<Vp8MacroblockCoefficients> coefficients) noexcept;
u32 vp8_bool_read(Vp8BoolReader* reader, u8 probability) noexcept;
u32 vp8_bool_read_literal(Vp8BoolReader* reader,
                          u32 bit_count) noexcept;

static_assert(sizeof(Vp8BoolReader) == 32);
static_assert(sizeof(Vp8Keyframe) == 48);
static_assert(sizeof(Vp8SegmentationHeader) == 40);
static_assert(sizeof(Vp8LoopFilterHeader) == 40);
static_assert(sizeof(Vp8QuantizationHeader) == 24);
static_assert(sizeof(Vp8TokenPartition) == 16);
static_assert(sizeof(Vp8ProbabilityWorkspace) == 1'056);
static_assert(sizeof(Vp8ControlHeader) == 280);
static_assert(sizeof(Vp8MacroblockMode) == 20);
static_assert(sizeof(Vp8ModeWorkspace) == 20'520);
static_assert(sizeof(Vp8TokenContext) == 9);
static_assert(sizeof(Vp8MacroblockCoefficients) == 770);
static_assert(sizeof(Vp8CoefficientWorkspace) == 789'048);

} // namespace maicord
