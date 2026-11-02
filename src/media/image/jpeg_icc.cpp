#include "jpeg_internal.hpp"

// The profile is parsed once per image: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace jpeg_detail {

struct IccScatter {
    ConstSlice<u8> encoded;
    const JpegIccChunk* chunks;
    u32 chunk_count;
    u32 profile_bytes;
};

struct IccCursor {
    const IccScatter* scatter;
    u32 chunk_index;
    u32 chunk_profile_offset;
    u32 offset;
};

struct IccTag {
    u32 signature;
    u32 offset;
    u32 bytes;
};

enum class IccParseOutcome : u8 {
    applied,
    malformed,
    unsupported,
};

inline constexpr u32 kIccTagMaximum = 64;
inline constexpr u32 kIccSignatureAcsp = 0x61637370u;
inline constexpr u32 kIccSignatureInput = 0x73636E72u;
inline constexpr u32 kIccSignatureDisplay = 0x6D6E7472u;
inline constexpr u32 kIccSignatureOutput = 0x70727472u;
inline constexpr u32 kIccSignatureGray = 0x47524159u;
inline constexpr u32 kIccSignatureRgb = 0x52474220u;
inline constexpr u32 kIccSignatureCmyk = 0x434D594Bu;
inline constexpr u32 kIccSignatureXyz = 0x58595A20u;
inline constexpr u32 kIccSignatureLab = 0x4C616220u;
inline constexpr u32 kIccSignatureCurve = 0x63757276u;
inline constexpr u32 kIccSignatureParametric = 0x70617261u;
inline constexpr u32 kIccSignatureMft1 = 0x6D667431u;
inline constexpr u32 kIccSignatureMft2 = 0x6D667432u;
inline constexpr u32 kIccTagRedXyz = 0x7258595Au;
inline constexpr u32 kIccTagGreenXyz = 0x6758595Au;
inline constexpr u32 kIccTagBlueXyz = 0x6258595Au;
inline constexpr u32 kIccTagRedTrc = 0x72545243u;
inline constexpr u32 kIccTagGreenTrc = 0x67545243u;
inline constexpr u32 kIccTagBlueTrc = 0x62545243u;
inline constexpr u32 kIccTagGrayTrc = 0x6B545243u;
inline constexpr u32 kIccTagA2b0 = 0x41324230u;
inline constexpr u32 kIccTagA2b1 = 0x41324231u;

bool icc_cursor_begin(const IccScatter& scatter, u32 offset, IccCursor* cursor) noexcept {
    if (offset > scatter.profile_bytes) {
        return false;
    }
    u32 profile_offset = 0;
    for (u32 chunk_index = 0; chunk_index < scatter.chunk_count; ++chunk_index) {
        const JpegIccChunk& chunk = scatter.chunks[chunk_index];
        if (offset < profile_offset + chunk.bytes ||
            (offset == scatter.profile_bytes && chunk_index + 1u == scatter.chunk_count)) {
            *cursor = IccCursor{&scatter, chunk_index, profile_offset, offset};
            return true;
        }
        profile_offset += chunk.bytes;
    }
    return offset == 0 && scatter.profile_bytes == 0;
}

bool icc_cursor_read_u8(IccCursor* cursor, u8* value) noexcept {
    if (cursor->offset >= cursor->scatter->profile_bytes) {
        return false;
    }
    while (cursor->chunk_index < cursor->scatter->chunk_count) {
        const JpegIccChunk& chunk = cursor->scatter->chunks[cursor->chunk_index];
        const u32 within = cursor->offset - cursor->chunk_profile_offset;
        if (within < chunk.bytes) {
            *value = cursor->scatter->encoded[chunk.encoded_offset + within];
            ++cursor->offset;
            return true;
        }
        cursor->chunk_profile_offset += chunk.bytes;
        ++cursor->chunk_index;
    }
    return false;
}

bool icc_cursor_read_u16(IccCursor* cursor, u16* value) noexcept {
    u8 high = 0;
    u8 low = 0;
    if (!icc_cursor_read_u8(cursor, &high) || !icc_cursor_read_u8(cursor, &low)) {
        return false;
    }
    *value = static_cast<u16>(static_cast<u16>(high) << 8u | low);
    return true;
}

IccParseOutcome icc_append_entries(const IccScatter& scatter, u32 offset, u32 count, u8 precision_bytes,
                                   bool require_monotonic, JpegIccWorkspace* workspace,
                                   u32* table_offset) noexcept {
    if (count == 0 || (precision_bytes != 1 && precision_bytes != 2)) {
        return IccParseOutcome::unsupported;
    }
    if (workspace->profile.entry_count > kJpegIccEntryMaximum ||
        count > kJpegIccEntryMaximum - workspace->profile.entry_count) {
        return IccParseOutcome::unsupported;
    }
    const u64 bytes = static_cast<u64>(count) * precision_bytes;
    if (static_cast<u64>(offset) + bytes > scatter.profile_bytes) {
        return IccParseOutcome::malformed;
    }
    IccCursor cursor{};
    if (!icc_cursor_begin(scatter, offset, &cursor)) {
        return IccParseOutcome::malformed;
    }
    *table_offset = workspace->profile.entry_count;
    u16 previous = 0;
    for (u32 index = 0; index < count; ++index) {
        u16 value = 0;
        if (precision_bytes == 1) {
            u8 byte = 0;
            if (!icc_cursor_read_u8(&cursor, &byte)) {
                return IccParseOutcome::malformed;
            }
            value = static_cast<u16>(static_cast<u16>(byte) * 257u);
        } else if (!icc_cursor_read_u16(&cursor, &value)) {
            return IccParseOutcome::malformed;
        }
        if (require_monotonic && index != 0 && value < previous) {
            return IccParseOutcome::malformed;
        }
        workspace->entries[*table_offset + index] = value;
        previous = value;
    }
    workspace->profile.entry_count += count;
    return IccParseOutcome::applied;
}

IccParseOutcome icc_append_table(const IccScatter& scatter, u32 offset, u32 count, u8 precision_bytes,
                                 bool require_monotonic, JpegIccWorkspace* workspace,
                                 JpegIccCurve* curve) noexcept {
    if (count < 2 || count > kJpegIccCurveEntryMaximum) {
        return IccParseOutcome::unsupported;
    }
    u32 table_offset = 0;
    const IccParseOutcome outcome = icc_append_entries(scatter, offset, count, precision_bytes,
                                                       require_monotonic, workspace, &table_offset);
    if (outcome != IccParseOutcome::applied) {
        return outcome;
    }
    curve->table_offset = table_offset;
    curve->table_count = static_cast<u16>(count);
    curve->kind = JpegIccCurveKind::table;
    return IccParseOutcome::applied;
}

bool icc_copy(const IccScatter& scatter, u32 offset, u8* destination, u32 bytes) noexcept {
    if (static_cast<u64>(offset) + bytes > scatter.profile_bytes) {
        return false;
    }
    u32 profile_offset = 0;
    u32 copied = 0;
    for (u32 chunk_index = 0; chunk_index < scatter.chunk_count && copied < bytes; ++chunk_index) {
        const JpegIccChunk& chunk = scatter.chunks[chunk_index];
        const u32 chunk_end = profile_offset + chunk.bytes;
        if (offset < chunk_end) {
            const u32 within = offset > profile_offset ? offset - profile_offset : 0;
            const u32 available = chunk.bytes - within;
            const u32 take = minimum<u32>(bytes - copied, available);
            for (u32 index = 0; index < take; ++index) {
                destination[copied + index] = scatter.encoded[chunk.encoded_offset + within + index];
            }
            copied += take;
            offset += take;
        }
        profile_offset = chunk_end;
    }
    return copied == bytes;
}

bool icc_read_u16(const IccScatter& scatter, u32 offset, u16* value) noexcept {
    u8 bytes[2]{};
    if (!icc_copy(scatter, offset, bytes, sizeof(bytes))) {
        return false;
    }
    *value = static_cast<u16>(static_cast<u16>(bytes[0]) << 8u | bytes[1]);
    return true;
}

bool icc_read_u32(const IccScatter& scatter, u32 offset, u32* value) noexcept {
    u8 bytes[4]{};
    if (!icc_copy(scatter, offset, bytes, sizeof(bytes))) {
        return false;
    }
    *value = static_cast<u32>(bytes[0]) << 24u | static_cast<u32>(bytes[1]) << 16u |
             static_cast<u32>(bytes[2]) << 8u | bytes[3];
    return true;
}

float icc_fixed(u32 bits) noexcept {
    const i64 signed_value =
        (bits & 0x80000000u) != 0 ? static_cast<i64>(bits) - 0x1'0000'0000ll : static_cast<i64>(bits);
    return static_cast<float>(signed_value) / 65'536.0f;
}

float absolute(float value) noexcept {
    return value < 0.0f ? -value : value;
}

bool icc_read_fixed(const IccScatter& scatter, u32 offset, float* value) noexcept {
    u32 bits = 0;
    if (!icc_read_u32(scatter, offset, &bits)) {
        return false;
    }
    *value = icc_fixed(bits);
    return true;
}

bool icc_tag_range_valid(const IccTag& tag, u32 tag_data_begin, u32 profile_bytes) noexcept {
    return (tag.offset & 3u) == 0 && tag.bytes >= 8 && tag.offset >= tag_data_begin &&
           tag.offset <= profile_bytes && tag.bytes <= profile_bytes - tag.offset;
}

IccParseOutcome parse_icc_xyz(const IccScatter& scatter, const IccTag& tag, float values[3]) noexcept {
    u32 type = 0;
    u32 reserved = 0;
    if (tag.bytes < 20 || !icc_read_u32(scatter, tag.offset, &type) ||
        !icc_read_u32(scatter, tag.offset + 4u, &reserved)) {
        return IccParseOutcome::malformed;
    }
    if (type != kIccSignatureXyz) {
        return IccParseOutcome::unsupported;
    }
    if (reserved != 0 || !icc_read_fixed(scatter, tag.offset + 8u, values) ||
        !icc_read_fixed(scatter, tag.offset + 12u, values + 1) ||
        !icc_read_fixed(scatter, tag.offset + 16u, values + 2)) {
        return IccParseOutcome::malformed;
    }
    for (u32 index = 0; index < 3; ++index) {
        if (absolute(values[index]) > 16.0f) {
            return IccParseOutcome::unsupported;
        }
    }
    return IccParseOutcome::applied;
}

// Parameters of ICC parametric curve functions 0-4.
inline constexpr u8 kIccParametricParameterCounts[5] = {1, 3, 4, 5, 7};

bool icc_parametric_sane(u16 function, const float* parameters) noexcept {
    const float g = parameters[0];
    if (!(g > 0.0f) || g > 16.0f) {
        return false;
    }
    for (u32 index = 0; index < kIccParametricParameterCounts[function]; ++index) {
        if (absolute(parameters[index]) > 16.0f) {
            return false;
        }
    }
    if (function == 0) {
        return true;
    }
    const float a = parameters[1];
    const float b = parameters[2];
    if (!(a > 0.0f)) {
        return false;
    }
    if (function <= 2) {
        const float threshold = -b / a;
        const float branch = threshold > 0.0f ? threshold : 0.0f;
        return threshold > 1.0f || a * branch + b >= -0.0001f;
    }
    const float c = parameters[3];
    const float d = parameters[4];
    return c >= 0.0f && d >= 0.0f && d <= 1.0f && a * d + b >= -0.0001f;
}

IccParseOutcome parse_icc_curve(const IccScatter& scatter, const IccTag& tag, JpegIccWorkspace* workspace,
                                JpegIccCurve* curve) noexcept {
    u32 type = 0;
    u32 reserved = 0;
    if (tag.bytes < 12 || !icc_read_u32(scatter, tag.offset, &type) ||
        !icc_read_u32(scatter, tag.offset + 4u, &reserved)) {
        return IccParseOutcome::malformed;
    }
    if (reserved != 0) {
        return IccParseOutcome::malformed;
    }
    reset_value<JpegIccCurve>(curve);
    curve->a = 1.0f;
    if (type == kIccSignatureCurve) {
        u32 count = 0;
        if (!icc_read_u32(scatter, tag.offset + 8u, &count)) {
            return IccParseOutcome::malformed;
        }
        const u64 required = 12ull + static_cast<u64>(count) * 2ull;
        if (required > tag.bytes) {
            return IccParseOutcome::malformed;
        }
        if (count == 0) {
            curve->kind = JpegIccCurveKind::identity;
            return IccParseOutcome::applied;
        }
        if (count == 1) {
            u16 gamma = 0;
            if (!icc_read_u16(scatter, tag.offset + 12u, &gamma) || gamma == 0) {
                return IccParseOutcome::malformed;
            }
            curve->g = static_cast<float>(gamma) / 256.0f;
            curve->kind = JpegIccCurveKind::parametric;
            return curve->g <= 16.0f ? IccParseOutcome::applied : IccParseOutcome::unsupported;
        }
        return icc_append_table(scatter, tag.offset + 12u, count, 2, true, workspace, curve);
    }
    if (type != kIccSignatureParametric) {
        return IccParseOutcome::unsupported;
    }
    u16 function = 0;
    u16 function_reserved = 0;
    if (!icc_read_u16(scatter, tag.offset + 8u, &function) ||
        !icc_read_u16(scatter, tag.offset + 10u, &function_reserved)) {
        return IccParseOutcome::malformed;
    }
    if (function > 4) {
        return IccParseOutcome::unsupported;
    }
    if (function_reserved != 0) {
        return IccParseOutcome::malformed;
    }
    const u32 parameter_count = kIccParametricParameterCounts[function];
    if (12u + parameter_count * 4u > tag.bytes) {
        return IccParseOutcome::malformed;
    }
    float parameters[7]{};
    for (u32 index = 0; index < parameter_count; ++index) {
        if (!icc_read_fixed(scatter, tag.offset + 12u + index * 4u, parameters + index)) {
            return IccParseOutcome::malformed;
        }
    }
    if (!icc_parametric_sane(function, parameters)) {
        return IccParseOutcome::unsupported;
    }
    curve->g = parameters[0];
    if (function != 0) {
        curve->a = parameters[1];
        curve->b = parameters[2];
    }
    if (function == 1) {
        curve->d = -curve->b / curve->a;
    } else if (function == 2) {
        curve->d = -curve->b / curve->a;
        curve->e = parameters[3];
        curve->f = parameters[3];
    } else if (function >= 3) {
        curve->c = parameters[3];
        curve->d = parameters[4];
        if (function == 4) {
            curve->e = parameters[5];
            curve->f = parameters[6];
        }
    }
    curve->kind = JpegIccCurveKind::parametric;
    return IccParseOutcome::applied;
}

void set_xyz_to_linear_srgb(float matrix[9]) noexcept {
    static constexpr float from_xyz_to_srgb[9] = {
        3.13411215f,   -1.61739242f,  -0.490633398f, -0.978787303f, 1.91627955f,
        0.0334547125f, 0.0719830394f, -0.228985846f, 1.40538514f,
    };
    for (u32 index = 0; index < 9; ++index) {
        matrix[index] = from_xyz_to_srgb[index];
    }
}

const IccTag* find_icc_tag(const IccTag* tags, u32 tag_count, u32 signature) noexcept {
    for (u32 index = 0; index < tag_count; ++index) {
        if (tags[index].signature == signature) {
            return tags + index;
        }
    }
    return nullptr;
}

IccParseOutcome parse_icc_mft(const IccScatter& scatter, const IccTag& tag, u8 expected_input_channels,
                              JpegIccPcs pcs, JpegIccWorkspace* workspace) noexcept {
    u32 type = 0;
    u32 reserved = 0;
    u8 channels[4]{};
    if (tag.bytes < 48 || !icc_read_u32(scatter, tag.offset, &type) ||
        !icc_read_u32(scatter, tag.offset + 4u, &reserved) ||
        !icc_copy(scatter, tag.offset + 8u, channels, sizeof(channels))) {
        return IccParseOutcome::malformed;
    }
    if (type != kIccSignatureMft1 && type != kIccSignatureMft2) {
        return IccParseOutcome::unsupported;
    }
    const u8 input_channels = channels[0];
    const u8 output_channels = channels[1];
    const u8 grid_points = channels[2];
    if (reserved != 0 || channels[3] != 0) {
        return IccParseOutcome::malformed;
    }
    if (input_channels != expected_input_channels || input_channels == 0 ||
        input_channels > kJpegIccInputChannelMaximum || output_channels != kJpegIccCurveChannelCount ||
        grid_points < 2 || grid_points > kJpegIccLutGridMaximum) {
        return IccParseOutcome::unsupported;
    }
    if (type == kIccSignatureMft1 && pcs == JpegIccPcs::xyz) {
        return IccParseOutcome::unsupported;
    }
    for (u32 index = 0; index < 9; ++index) {
        u32 value = 0;
        const u32 expected = index == 0 || index == 4 || index == 8 ? 0x00010000u : 0;
        if (!icc_read_u32(scatter, tag.offset + 12u + index * 4u, &value) || value != expected) {
            return IccParseOutcome::malformed;
        }
    }

    u8 precision_bytes = 1;
    u32 input_entry_count = 256;
    u32 output_entry_count = 256;
    u32 data_offset = 48;
    if (type == kIccSignatureMft2) {
        u16 input_count = 0;
        u16 output_count = 0;
        if (tag.bytes < 52 || !icc_read_u16(scatter, tag.offset + 48u, &input_count) ||
            !icc_read_u16(scatter, tag.offset + 50u, &output_count)) {
            return IccParseOutcome::malformed;
        }
        if (input_count < 2 || output_count < 2 || input_count > kJpegIccCurveEntryMaximum ||
            output_count > kJpegIccCurveEntryMaximum) {
            return IccParseOutcome::unsupported;
        }
        precision_bytes = 2;
        input_entry_count = input_count;
        output_entry_count = output_count;
        data_offset = 52;
    }

    u64 grid_product = 1;
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (u32 channel = 0; channel < input_channels; ++channel) {
        grid_product *= grid_points;
    }
    const u64 clut_entry_count64 = grid_product * output_channels;
    if (clut_entry_count64 > kJpegIccLutClutEntryMaximum) {
        return IccParseOutcome::unsupported;
    }
    const u64 input_bytes = static_cast<u64>(input_channels) * input_entry_count * precision_bytes;
    const u64 clut_bytes = clut_entry_count64 * precision_bytes;
    const u64 output_bytes = static_cast<u64>(output_channels) * output_entry_count * precision_bytes;
    const u64 required_bytes = static_cast<u64>(data_offset) + input_bytes + clut_bytes + output_bytes;
    if (required_bytes > tag.bytes) {
        return IccParseOutcome::malformed;
    }

    workspace->profile.entry_count = 0;
    reset_value<JpegIccLut>(&workspace->profile.lut);
    JpegIccLut& lut = workspace->profile.lut;
    u32 cursor = tag.offset + data_offset;
    for (u32 channel = 0; channel < input_channels; ++channel) {
        JpegIccCurve& curve = lut.input_curves[channel];
        curve.a = 1.0f;
        const IccParseOutcome outcome =
            icc_append_table(scatter, cursor, input_entry_count, precision_bytes, false, workspace, &curve);
        if (outcome != IccParseOutcome::applied) {
            return outcome;
        }
        cursor += input_entry_count * precision_bytes;
    }
    const u32 clut_entry_count = static_cast<u32>(clut_entry_count64);
    const IccParseOutcome clut = icc_append_entries(scatter, cursor, clut_entry_count, precision_bytes,
                                                    false, workspace, &lut.clut_offset);
    if (clut != IccParseOutcome::applied) {
        return clut;
    }
    lut.clut_entry_count = clut_entry_count;
    cursor += clut_entry_count * precision_bytes;
    for (u32 channel = 0; channel < output_channels; ++channel) {
        JpegIccCurve& curve = lut.output_curves[channel];
        curve.a = 1.0f;
        const IccParseOutcome outcome = icc_append_table(scatter, cursor, output_entry_count,
                                                         precision_bytes, false, workspace, &curve);
        if (outcome != IccParseOutcome::applied) {
            return outcome;
        }
        cursor += output_entry_count * precision_bytes;
    }
    for (u32 channel = 0; channel < input_channels; ++channel) {
        lut.grid_points[channel] = grid_points;
    }
    lut.input_channels = input_channels;
    lut.output_channels = output_channels;
    lut.precision_bytes = precision_bytes;
    workspace->profile.transform_kind = JpegIccTransformKind::lut;
    set_xyz_to_linear_srgb(workspace->profile.to_linear_srgb);
    return IccParseOutcome::applied;
}

IccParseOutcome parse_icc_profile(const IccScatter& scatter, const JpegImage& image,
                                  JpegIccWorkspace* workspace) noexcept {
    if (scatter.profile_bytes < 132u) {
        return IccParseOutcome::malformed;
    }
    u32 declared_bytes = 0;
    u32 profile_class = 0;
    u32 color_space = 0;
    u32 pcs_signature = 0;
    u32 signature = 0;
    u32 illuminant_x = 0;
    u32 illuminant_y = 0;
    u32 illuminant_z = 0;
    u32 rendering_intent = 0;
    u8 version = 0;
    if (!icc_read_u32(scatter, 0, &declared_bytes) || !icc_copy(scatter, 8, &version, 1) ||
        !icc_read_u32(scatter, 12, &profile_class) || !icc_read_u32(scatter, 16, &color_space) ||
        !icc_read_u32(scatter, 20, &pcs_signature) || !icc_read_u32(scatter, 36, &signature) ||
        !icc_read_u32(scatter, 64, &rendering_intent) || !icc_read_u32(scatter, 68, &illuminant_x) ||
        !icc_read_u32(scatter, 72, &illuminant_y) || !icc_read_u32(scatter, 76, &illuminant_z)) {
        return IccParseOutcome::malformed;
    }
    if (declared_bytes != scatter.profile_bytes || signature != kIccSignatureAcsp ||
        rendering_intent > 3u || illuminant_x != 0x0000F6D6u || illuminant_y != 0x00010000u ||
        illuminant_z != 0x0000D32Du) {
        return IccParseOutcome::malformed;
    }
    u8 reserved[28]{};
    if (!icc_copy(scatter, 100, reserved, sizeof(reserved))) {
        return IccParseOutcome::malformed;
    }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (u32 index = 0; index < sizeof(reserved); ++index) {
        if (reserved[index] != 0) {
            return IccParseOutcome::malformed;
        }
    }
    const u8 major = version;
    if ((major != 2 && major != 4) ||
        (profile_class != kIccSignatureInput && profile_class != kIccSignatureDisplay &&
         profile_class != kIccSignatureOutput)) {
        return IccParseOutcome::unsupported;
    }

    u8 input_channels = 0;
    if (color_space == kIccSignatureGray) {
        input_channels = 1;
        workspace->profile.data_color_space = JpegIccDataColorSpace::gray;
        if (image.component_count != 1 || image.color_transform != JpegColorTransform::grayscale) {
            return IccParseOutcome::unsupported;
        }
    } else if (color_space == kIccSignatureRgb) {
        input_channels = 3;
        workspace->profile.data_color_space = JpegIccDataColorSpace::rgb;
        if (image.component_count != 3 || (image.color_transform != JpegColorTransform::rgb &&
                                           image.color_transform != JpegColorTransform::ycbcr)) {
            return IccParseOutcome::unsupported;
        }
    } else if (color_space == kIccSignatureCmyk) {
        input_channels = 4;
        workspace->profile.data_color_space = JpegIccDataColorSpace::cmyk;
        if (image.component_count != 4 || (image.color_transform != JpegColorTransform::cmyk &&
                                           image.color_transform != JpegColorTransform::ycck)) {
            return IccParseOutcome::unsupported;
        }
    } else {
        return IccParseOutcome::unsupported;
    }
    if (pcs_signature == kIccSignatureXyz) {
        workspace->profile.pcs = JpegIccPcs::xyz;
    } else if (pcs_signature == kIccSignatureLab) {
        workspace->profile.pcs = JpegIccPcs::lab;
    } else {
        return IccParseOutcome::unsupported;
    }

    u32 tag_count = 0;
    if (!icc_read_u32(scatter, 128, &tag_count)) {
        return IccParseOutcome::malformed;
    }
    if (tag_count == 0 || tag_count > kIccTagMaximum) {
        return IccParseOutcome::unsupported;
    }
    const u32 tag_data_begin = 132u + tag_count * 12u;
    if (tag_data_begin > scatter.profile_bytes) {
        return IccParseOutcome::malformed;
    }
    IccTag tags[kIccTagMaximum]{};
    for (u32 index = 0; index < tag_count; ++index) {
        const u32 offset = 132u + index * 12u;
        if (!icc_read_u32(scatter, offset, &tags[index].signature) ||
            !icc_read_u32(scatter, offset + 4u, &tags[index].offset) ||
            !icc_read_u32(scatter, offset + 8u, &tags[index].bytes) ||
            !icc_tag_range_valid(tags[index], tag_data_begin, scatter.profile_bytes)) {
            return IccParseOutcome::malformed;
        }
        for (u32 previous = 0; previous < index; ++previous) {
            if (tags[previous].signature == tags[index].signature) {
                return IccParseOutcome::malformed;
            }
            const bool exact_share =
                tags[previous].offset == tags[index].offset && tags[previous].bytes == tags[index].bytes;
            const u32 first_end = tags[previous].offset + tags[previous].bytes;
            const u32 second_end = tags[index].offset + tags[index].bytes;
            if (!exact_share && tags[previous].offset < second_end && tags[index].offset < first_end) {
                return IccParseOutcome::malformed;
            }
        }
    }

    workspace->profile.major_version = major;
    const IccTag* a2b = find_icc_tag(tags, tag_count, kIccTagA2b0);
    if (!a2b) {
        a2b = find_icc_tag(tags, tag_count, kIccTagA2b1);
    }
    if (a2b) {
        return parse_icc_mft(scatter, *a2b, input_channels, workspace->profile.pcs, workspace);
    }

    workspace->profile.entry_count = 0;
    if (color_space == kIccSignatureGray) {
        const IccTag* gray_trc = find_icc_tag(tags, tag_count, kIccTagGrayTrc);
        if (!gray_trc) {
            return IccParseOutcome::unsupported;
        }
        const IccParseOutcome trc =
            parse_icc_curve(scatter, *gray_trc, workspace, workspace->profile.curves);
        if (trc != IccParseOutcome::applied) {
            return trc;
        }
        set_xyz_to_linear_srgb(workspace->profile.to_linear_srgb);
        workspace->profile.transform_kind = JpegIccTransformKind::gray_trc;
        return IccParseOutcome::applied;
    }
    if (color_space != kIccSignatureRgb || workspace->profile.pcs != JpegIccPcs::xyz) {
        return IccParseOutcome::unsupported;
    }

    static constexpr u32 required_signatures[6] = {
        kIccTagRedXyz, kIccTagGreenXyz, kIccTagBlueXyz, kIccTagRedTrc, kIccTagGreenTrc, kIccTagBlueTrc,
    };
    const IccTag* required[6]{};
    for (u32 index = 0; index < 6; ++index) {
        required[index] = find_icc_tag(tags, tag_count, required_signatures[index]);
        if (!required[index]) {
            return IccParseOutcome::unsupported;
        }
    }
    float columns[3][3]{};
    for (u32 channel = 0; channel < 3; ++channel) {
        const IccParseOutcome xyz = parse_icc_xyz(scatter, *required[channel], columns[channel]);
        if (xyz != IccParseOutcome::applied) {
            return xyz;
        }
    }
    float source_to_xyz[9]{};
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 3; ++column) {
            source_to_xyz[row * 3u + column] = columns[column][row];
        }
    }
    const float determinant =
        source_to_xyz[0] * (source_to_xyz[4] * source_to_xyz[8] - source_to_xyz[5] * source_to_xyz[7]) -
        source_to_xyz[1] * (source_to_xyz[3] * source_to_xyz[8] - source_to_xyz[5] * source_to_xyz[6]) +
        source_to_xyz[2] * (source_to_xyz[3] * source_to_xyz[7] - source_to_xyz[4] * source_to_xyz[6]);
    if (absolute(determinant) < 0.000001f) {
        return IccParseOutcome::unsupported;
    }
    for (u32 channel = 0; channel < 3; ++channel) {
        const IccParseOutcome trc = parse_icc_curve(scatter, *required[3u + channel], workspace,
                                                    workspace->profile.curves + channel);
        if (trc != IccParseOutcome::applied) {
            return trc;
        }
    }
    float xyz_to_srgb[9]{};
    set_xyz_to_linear_srgb(xyz_to_srgb);
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 3; ++column) {
            float value = 0.0f;
            for (u32 index = 0; index < 3; ++index) {
                value += xyz_to_srgb[row * 3u + index] * source_to_xyz[index * 3u + column];
            }
            workspace->profile.to_linear_srgb[row * 3u + column] = value;
        }
    }
    workspace->profile.transform_kind = JpegIccTransformKind::matrix_trc;
    return IccParseOutcome::applied;
}

void finalize_icc_profile(ConstSlice<u8> encoded, const JpegIccChunk* chunks, u32 chunk_count,
                          u32 declared_chunk_count, bool marker_seen, bool transport_malformed,
                          const JpegImage& image, JpegIccWorkspace* workspace) noexcept {
    JpegIccProfile& profile = workspace->profile;
    profile.entries = workspace->entries;
    if (!marker_seen) {
        return;
    }
    profile.chunk_count = static_cast<u16>(declared_chunk_count);
    if (transport_malformed) {
        profile.disposition = JpegIccDisposition::malformed_ignored;
        return;
    }
    if (declared_chunk_count == 0 || chunk_count != declared_chunk_count) {
        profile.disposition = JpegIccDisposition::incomplete_ignored;
        return;
    }
    u64 total = 0;
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (u32 index = 0; index < declared_chunk_count; ++index) {
        total += chunks[index].bytes;
    }
    if (total == 0 || total > kJpegIccProfileMaximumBytes) {
        profile.disposition =
            total == 0 ? JpegIccDisposition::malformed_ignored : JpegIccDisposition::unsupported_ignored;
        return;
    }
    profile.profile_bytes = static_cast<u32>(total);
    const IccScatter scatter{encoded, chunks, declared_chunk_count, static_cast<u32>(total)};
    const IccParseOutcome outcome = parse_icc_profile(scatter, image, workspace);
    profile.disposition = outcome == IccParseOutcome::applied     ? JpegIccDisposition::applied
                          : outcome == IccParseOutcome::malformed ? JpegIccDisposition::malformed_ignored
                                                                  : JpegIccDisposition::unsupported_ignored;
}

} // namespace jpeg_detail
} // namespace maicord
#pragma clang attribute pop
