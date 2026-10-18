#include "maicord/media/jpeg.hpp"

#include "maicord/core/byte_order.hpp"

#include <immintrin.h>
#include <intrin.h>

#pragma intrinsic(_byteswap_uint64)

namespace maicord {
namespace {

inline constexpr u8 kZigzagToRaster[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

u16 read_tiff16(const u8* bytes, bool big_endian) noexcept {
    return big_endian ? byte_load_be16(bytes) : byte_load_le16(bytes);
}

u32 read_tiff32(const u8* bytes, bool big_endian) noexcept {
    return big_endian ? byte_load_be32(bytes) : byte_load_le32(bytes);
}

JpegParseResult parse_failure(JpegParseResult result, JpegError error) noexcept {
    result.error = error;
    return result;
}

bool read_marker(ConstSlice<u8> encoded, u32* cursor, u8* marker) noexcept {
    if (*cursor >= encoded.count || encoded[*cursor] != 0xFF) {
        return false;
    }
    u32 offset = *cursor;
    while (offset < encoded.count && encoded[offset] == 0xFF) {
        ++offset;
    }
    if (offset >= encoded.count || encoded[offset] == 0) {
        return false;
    }
    *marker = encoded[offset++];
    *cursor = offset;
    return true;
}

bool segment_bounds(ConstSlice<u8> encoded, u32* cursor, u32* payload, u32* payload_bytes) noexcept {
    if (*cursor + 2u > encoded.count) {
        return false;
    }
    const u32 length = byte_load_be16(encoded.data + *cursor);
    if (length < 2 || static_cast<u64>(*cursor) + length > encoded.count) {
        return false;
    }
    *payload = *cursor + 2u;
    *payload_bytes = length - 2u;
    *cursor += length;
    return true;
}

bool bytes_equal(const u8* left, const char* right, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index) {
        if (left[index] != static_cast<u8>(right[index])) {
            return false;
        }
    }
    return true;
}

bool parse_exif_orientation(const u8* payload, u32 bytes, JpegOrientation* orientation) noexcept {
    // The sixth identifier byte is a fill byte and is not checked, as in Chromium.
    if (bytes < 14 || !bytes_equal(payload, "Exif\0", 5)) {
        return false;
    }
    const u8* tiff = payload + 6;
    const u32 tiff_bytes = bytes - 6;
    const bool little_endian = tiff[0] == 'I' && tiff[1] == 'I';
    const bool big_endian = tiff[0] == 'M' && tiff[1] == 'M';
    if ((!little_endian && !big_endian) || read_tiff16(tiff + 2, big_endian) != 42) {
        return false;
    }
    const u32 directory_offset = read_tiff32(tiff + 4, big_endian);
    if (directory_offset > tiff_bytes - 2u) {
        return false;
    }
    const u16 entries = read_tiff16(tiff + directory_offset, big_endian);
    u32 cursor = directory_offset + 2u;
    const u32 available_entries = (tiff_bytes - cursor) / 12u;
    const u32 checked_entries = minimum<u32>(entries, available_entries);
    for (u32 index = 0; index < checked_entries; ++index) {
        const u8* entry = tiff + cursor;
        const u16 tag = read_tiff16(entry, big_endian);
        const u16 type = read_tiff16(entry + 2, big_endian);
        const u32 count = read_tiff32(entry + 4, big_endian);
        if (tag == 0x0112 && type == 3 && count == 1) {
            const u16 value = read_tiff16(entry + 8, big_endian);
            if (value >= static_cast<u16>(JpegOrientation::top_left) &&
                value <= static_cast<u16>(JpegOrientation::left_bottom)) {
                *orientation = static_cast<JpegOrientation>(value);
                return true;
            }
            return false;
        }
        cursor += 12u;
    }
    return false;
}

// Payload of one APP2 segment, already bounded by segment_bounds().
struct JpegIccChunk {
    u32 encoded_offset;
    u32 bytes;
};

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

bool icc_parametric_sane(u16 function, const float* parameters) noexcept {
    const float g = parameters[0];
    if (!(g > 0.0f) || g > 16.0f) {
        return false;
    }
    constexpr u8 counts[5] = {1, 3, 4, 5, 7};
    for (u32 index = 0; index < counts[function]; ++index) {
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
    *curve = JpegIccCurve{};
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
    constexpr u8 parameter_counts[5] = {1, 3, 4, 5, 7};
    const u32 parameter_count = parameter_counts[function];
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
    if (function == 0) {
        curve->a = 1.0f;
    } else {
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
    constexpr float from_xyz_to_srgb[9] = {
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
        if (!icc_read_u32(scatter, tag.offset + 12u + index * 4u, &value)) {
            return IccParseOutcome::malformed;
        }
        if (value != expected) {
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
    workspace->profile.lut = JpegIccLut{};
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

    constexpr u32 required_signatures[6] = {
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

bool build_huffman_table(JpegHuffmanTable* table, const u8* counts, const u8* symbols,
                         u32 symbol_count) noexcept {
    if (symbol_count == 0 || symbol_count > 256) {
        return false;
    }
    *table = JpegHuffmanTable{};
    i32 code = 0;
    u32 symbol_offset = 0;
    for (u32 length = 0; length < 16; ++length) {
        const u32 count = counts[length];
        const u32 code_limit = 1u << (length + 1u);
        if (static_cast<u32>(code) + count > code_limit ||
            (count != 0 && static_cast<u32>(code) + count == code_limit)) {
            return false;
        }
        if (count != 0) {
            table->minimum_code[length] = code;
            table->maximum_code[length] = code + static_cast<i32>(count) - 1;
            table->value_offset[length] = static_cast<i32>(symbol_offset) - code;
        } else {
            table->minimum_code[length] = -1;
            table->maximum_code[length] = -1;
            table->value_offset[length] = 0;
        }
        code = (code + static_cast<i32>(count)) << 1;
        symbol_offset += count;
    }
    if (symbol_offset != symbol_count) {
        return false;
    }
    for (u32 index = 0; index < symbol_count; ++index) {
        table->symbols[index] = symbols[index];
    }
    table->symbol_count = static_cast<u16>(symbol_count);
    table->present = true;
    return true;
}

JpegError parse_quantization(ConstSlice<u8> encoded, u32 payload, u32 bytes, u8 locked_tables,
                             JpegTables* tables) noexcept {
    u32 cursor = payload;
    const u32 end = payload + bytes;
    while (cursor < end) {
        const u8 information = encoded[cursor++];
        const u8 precision = information >> 4;
        const u8 identifier = information & 0x0F;
        if (precision != 0 || identifier >= kJpegTableMaximum ||
            (locked_tables & (1u << identifier)) != 0 || end - cursor < 64) {
            return JpegError::quantization;
        }
        JpegQuantizationTable& table = tables->quantization[identifier];
        table = JpegQuantizationTable{};
        for (u32 index = 0; index < 64; ++index) {
            const u8 value = encoded[cursor++];
            if (value == 0) {
                return JpegError::quantization;
            }
            table.values[kZigzagToRaster[index]] = value;
        }
        table.present = true;
    }
    return cursor == end ? JpegError::none : JpegError::quantization;
}

JpegError parse_huffman(ConstSlice<u8> encoded, u32 payload, u32 bytes, JpegTables* tables) noexcept {
    u32 cursor = payload;
    const u32 end = payload + bytes;
    while (cursor < end) {
        if (end - cursor < 17) {
            return JpegError::huffman;
        }
        const u8 information = encoded[cursor++];
        const u8 table_class = information >> 4;
        const u8 identifier = information & 0x0F;
        if (table_class > 1 || identifier >= kJpegTableMaximum) {
            return JpegError::huffman;
        }
        JpegHuffmanTable* table = table_class == 0 ? &tables->dc[identifier] : &tables->ac[identifier];
        const u8* counts = encoded.data + cursor;
        u32 symbol_count = 0;
        for (u32 index = 0; index < 16; ++index) {
            symbol_count += counts[index];
        }
        cursor += 16;
        if (symbol_count == 0 || symbol_count > 256 || end - cursor < symbol_count) {
            return JpegError::huffman;
        }
        for (u32 index = 0; index < symbol_count; ++index) {
            const u8 symbol = encoded[cursor + index];
            const u8 magnitude = symbol & 0x0F;
            if ((table_class == 0 && symbol > 11) || (table_class == 1 && magnitude > 10)) {
                return JpegError::huffman;
            }
        }
        if (!build_huffman_table(table, counts, encoded.data + cursor, symbol_count)) {
            return JpegError::huffman;
        }
        cursor += symbol_count;
    }
    return cursor == end ? JpegError::none : JpegError::huffman;
}

JpegError parse_frame(ConstSlice<u8> encoded, u32 payload, u32 bytes, JpegImage* image,
                      bool thumbnail) noexcept {
    if (bytes < 9 || image->component_count != 0) {
        return image->component_count != 0 ? JpegError::duplicate : JpegError::segment;
    }
    const u8 precision = encoded[payload];
    const u32 height = byte_load_be16(encoded.data + payload + 1);
    const u32 width = byte_load_be16(encoded.data + payload + 3);
    const u8 components = encoded[payload + 5];
    if (precision != 8) {
        return JpegError::unsupported_precision;
    }
    if (components != 1 && components != 3 && components != 4) {
        return JpegError::unsupported_components;
    }
    if (bytes != 6u + static_cast<u32>(components) * 3u) {
        return JpegError::segment;
    }
    const u32 dimension_limit = thumbnail ? kJpegThumbnailDimensionMaximum : kJpegDimensionMaximum;
    if (width == 0 || height == 0 || width > dimension_limit || height > dimension_limit) {
        return JpegError::dimensions;
    }

    u8 maximum_h = 0;
    u8 maximum_v = 0;
    for (u32 index = 0; index < components; ++index) {
        const u32 offset = payload + 6u + index * 3u;
        const u8 identifier = encoded[offset];
        const u8 sampling = encoded[offset + 1];
        const u8 horizontal = sampling >> 4;
        const u8 vertical = sampling & 0x0F;
        const u8 quantization = encoded[offset + 2];
        if (horizontal == 0 || horizontal > 2 || vertical == 0 || vertical > 2 ||
            quantization >= kJpegTableMaximum) {
            return JpegError::unsupported_sampling;
        }
        for (u32 prior = 0; prior < index; ++prior) {
            if (image->components[prior].identifier == identifier) {
                return JpegError::duplicate;
            }
        }
        JpegComponent& component = image->components[index];
        component.identifier = identifier;
        component.horizontal_sampling = horizontal;
        component.vertical_sampling = vertical;
        component.quantization_table = quantization;
        maximum_h = maximum(maximum_h, horizontal);
        maximum_v = maximum(maximum_v, vertical);
    }
    if (components == 1) {
        // A single-component scan is non-interleaved: its MCU is one block
        // whatever factors the frame header declares (T.81 A.2.2). Some
        // encoders write 2x2 for grayscale; treat it as 1x1.
        image->components[0].horizontal_sampling = image->components[0].vertical_sampling = 1;
        maximum_h = maximum_v = 1;
    }

    const u32 mcu_width = 8u * maximum_h;
    const u32 mcu_height = 8u * maximum_v;
    const u32 mcu_columns = (width + mcu_width - 1u) / mcu_width;
    const u32 mcu_rows = (height + mcu_height - 1u) / mcu_height;
    u32 block_offset = 0;
    for (u32 index = 0; index < components; ++index) {
        JpegComponent& component = image->components[index];
        const u32 columns = mcu_columns * component.horizontal_sampling;
        const u32 rows = mcu_rows * component.vertical_sampling;
        const u32 count = columns * rows;
        if (!thumbnail && static_cast<u64>(block_offset) + count > kJpegCoefficientBlockMaximum) {
            return JpegError::dimensions;
        }
        component.block_offset = block_offset;
        component.block_count = count;
        component.block_columns = static_cast<u16>(columns);
        component.block_rows = static_cast<u16>(rows);
        block_offset += count;
    }

    image->width = static_cast<u16>(width);
    image->height = static_cast<u16>(height);
    image->mcu_columns = static_cast<u16>(mcu_columns);
    image->mcu_rows = static_cast<u16>(mcu_rows);
    image->coefficient_blocks = block_offset;
    image->component_count = components;
    image->maximum_horizontal_sampling = maximum_h;
    image->maximum_vertical_sampling = maximum_v;
    return JpegError::none;
}

struct JpegScan {
    u32 entropy_offset;
    u32 entropy_bytes;
    u32 data_units;
    u16 restart_interval;
    u16 restart_markers;
    u8 component_indices[kJpegComponentMaximum];
    u8 dc_tables[kJpegComponentMaximum];
    u8 ac_tables[kJpegComponentMaximum];
    u8 component_count;
    u8 spectral_start;
    u8 spectral_end;
    u8 approximation_high;
    u8 approximation_low;
};

u32 scan_component_columns(const JpegImage& image, const JpegComponent& component) noexcept {
    const u64 numerator = static_cast<u64>(image.width) * component.horizontal_sampling;
    const u32 denominator = 8u * image.maximum_horizontal_sampling;
    return static_cast<u32>((numerator + denominator - 1u) / denominator);
}

u32 scan_component_rows(const JpegImage& image, const JpegComponent& component) noexcept {
    const u64 numerator = static_cast<u64>(image.height) * component.vertical_sampling;
    const u32 denominator = 8u * image.maximum_vertical_sampling;
    return static_cast<u32>((numerator + denominator - 1u) / denominator);
}

JpegError parse_scan_header(ConstSlice<u8> encoded, u32 payload, u32 bytes, const JpegImage& image,
                            const JpegTables& tables, u16 restart_interval, JpegScan* scan) noexcept {
    if (image.component_count == 0 || bytes < 6) {
        return JpegError::order;
    }
    *scan = JpegScan{};
    const u8 scan_components = encoded[payload];
    if (scan_components == 0 || scan_components > image.component_count ||
        bytes != 1u + static_cast<u32>(scan_components) * 2u + 3u ||
        (!image.progressive && scan_components != image.component_count)) {
        return JpegError::unsupported_process;
    }
    u32 sampling_sum = 0;
    for (u32 scan_index = 0; scan_index < scan_components; ++scan_index) {
        const u8 identifier = encoded[payload + 1u + scan_index * 2u];
        const u8 selectors = encoded[payload + 2u + scan_index * 2u];
        const u8 dc = selectors >> 4;
        const u8 ac = selectors & 0x0F;
        if (dc >= kJpegTableMaximum || ac >= kJpegTableMaximum) {
            return JpegError::huffman;
        }
        u32 component_index = image.component_count;
        for (u32 index = 0; index < image.component_count; ++index) {
            if (image.components[index].identifier == identifier) {
                component_index = index;
                break;
            }
        }
        if (component_index == image.component_count) {
            return JpegError::scan;
        }
        for (u32 prior = 0; prior < scan_index; ++prior) {
            if (scan->component_indices[prior] == component_index) {
                return JpegError::scan;
            }
        }
        const JpegComponent& component = image.components[component_index];
        if (!tables.quantization[component.quantization_table].present) {
            return JpegError::order;
        }
        sampling_sum += static_cast<u32>(component.horizontal_sampling) * component.vertical_sampling;
        scan->component_indices[scan_index] = static_cast<u8>(component_index);
        scan->dc_tables[scan_index] = dc;
        scan->ac_tables[scan_index] = ac;
    }
    if (scan_components > 1 && sampling_sum > 10) {
        return JpegError::unsupported_sampling;
    }
    const u32 parameters = payload + 1u + scan_components * 2u;
    const u8 spectral_start = encoded[parameters];
    const u8 spectral_end = encoded[parameters + 1];
    const u8 approximation = encoded[parameters + 2];
    const u8 approximation_high = approximation >> 4;
    const u8 approximation_low = approximation & 0x0F;
    if (image.progressive) {
        if (spectral_start > spectral_end || spectral_end >= 64 ||
            (spectral_start == 0 && spectral_end != 0) || (spectral_start != 0 && scan_components != 1) ||
            approximation_high > 13 || approximation_low > 13 ||
            (approximation_high != 0 && approximation_high != approximation_low + 1u)) {
            return JpegError::scan;
        }
    } else if (spectral_start != 0 || spectral_end != 63 || approximation != 0) {
        return JpegError::unsupported_process;
    }
    for (u32 index = 0; index < scan_components; ++index) {
        if (spectral_start == 0 && approximation_high == 0 && !tables.dc[scan->dc_tables[index]].present) {
            return JpegError::order;
        }
        if (spectral_start != 0 && !tables.ac[scan->ac_tables[index]].present) {
            return JpegError::order;
        }
        if (!image.progressive &&
            (!tables.dc[scan->dc_tables[index]].present || !tables.ac[scan->ac_tables[index]].present)) {
            return JpegError::order;
        }
    }
    scan->component_count = scan_components;
    scan->spectral_start = spectral_start;
    scan->spectral_end = spectral_end;
    scan->approximation_high = approximation_high;
    scan->approximation_low = approximation_low;
    scan->restart_interval = restart_interval;
    if (scan_components > 1) {
        scan->data_units = static_cast<u32>(image.mcu_columns) * image.mcu_rows;
    } else {
        const JpegComponent& component = image.components[scan->component_indices[0]];
        scan->data_units = scan_component_columns(image, component) * scan_component_rows(image, component);
    }
    return scan->data_units != 0 ? JpegError::none : JpegError::scan;
}

JpegError validate_progression(const JpegScan& scan, u8 progression[kJpegComponentMaximum][64]) noexcept {
    for (u32 scan_index = 0; scan_index < scan.component_count; ++scan_index) {
        const u32 component = scan.component_indices[scan_index];
        if (scan.spectral_start != 0 && progression[component][0] == 0xFF) {
            return JpegError::order;
        }
        for (u32 coefficient = scan.spectral_start; coefficient <= scan.spectral_end; ++coefficient) {
            const u8 previous = progression[component][coefficient];
            if ((scan.approximation_high == 0 && previous != 0xFF) ||
                (scan.approximation_high != 0 && previous != scan.approximation_high)) {
                return JpegError::scan;
            }
        }
    }
    for (u32 scan_index = 0; scan_index < scan.component_count; ++scan_index) {
        const u32 component = scan.component_indices[scan_index];
        for (u32 coefficient = scan.spectral_start; coefficient <= scan.spectral_end; ++coefficient) {
            progression[component][coefficient] = scan.approximation_low;
        }
    }
    return JpegError::none;
}

JpegError find_entropy_end(ConstSlice<u8> encoded, u32 start, JpegScan* scan, u32* next_marker) noexcept {
    u32 cursor = start;
    u32 restart_markers = 0;
    const __m128i marker_byte = _mm_set1_epi8(static_cast<char>(0xFF));
    while (cursor < encoded.count) {
        if (encoded[cursor] != 0xFF) {
            // Skip entropy bytes sixteen at a time up to the next 0xFF.
            ++cursor;
            while (cursor + 16u <= encoded.count) {
                const u32 mask = static_cast<u32>(_mm_movemask_epi8(_mm_cmpeq_epi8(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(encoded.data + cursor)), marker_byte)));
                if (mask != 0) {
                    unsigned long first = 0;
                    _BitScanForward(&first, mask);
                    cursor += first;
                    break;
                }
                cursor += 16u;
            }
            continue;
        }
        const u32 marker_start = cursor;
        while (cursor < encoded.count && encoded[cursor] == 0xFF) {
            ++cursor;
        }
        if (cursor >= encoded.count) {
            return JpegError::truncated;
        }
        const u8 marker = encoded[cursor++];
        if (marker == 0) {
            continue;
        }
        if (marker >= 0xD0 && marker <= 0xD7) {
            ++restart_markers;
            continue;
        }
        scan->entropy_offset = start;
        scan->entropy_bytes = marker_start - start;
        if (restart_markers > 65'535u) {
            return JpegError::restart;
        }
        scan->restart_markers = static_cast<u16>(restart_markers);
        *next_marker = marker_start;
        return JpegError::none;
    }
    return JpegError::truncated;
}

// Bit-exact with a bit-serial reader that loads one entropy byte only when it
// needs its first bit: refills run ahead, but a refill that reaches the end of
// the segment or a marker only records why (stop), and the error is raised
// when a decode actually needs a bit past that point.
struct JpegEntropyReader {
    ConstSlice<u8> bytes;
    u64 buffer;      // next bit at bit 63; past `bits`: zero, or the leading bits of the byte at offset
    u32 offset;      // next input byte not yet in the buffer
    u32 bits;        // valid bits in buffer
    JpegError stop;  // truncated (segment end) or restart (marker) once refills stopped
    JpegError error;
};

JpegEntropyReader entropy_reader(ConstSlice<u8> bytes) noexcept {
    return JpegEntropyReader{bytes, 0, 0, 0, JpegError::none, JpegError::none};
}

// Fills the buffer to at least 57 bits unless the segment ends or a marker follows.
__forceinline void entropy_refill(JpegEntropyReader* reader) noexcept {
    const u8* const data = reader->bytes.data;
    const u32 count = reader->bytes.count;
    if (reader->stop == JpegError::none && reader->offset + 8u <= count) {
        // Eight bytes without 0xFF are plain entropy data: append as many as
        // fit at once. Bits of the next byte may land below `bits`; they are
        // that byte's own bits, ORed in again when it is appended.
        const u64 raw = static_cast<u64>(
            _mm_cvtsi128_si64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(data + reader->offset))));
        if (((~raw - 0x0101010101010101ull) & raw & 0x8080808080808080ull) == 0) {
            const u32 take = (64u - reader->bits) >> 3;
            reader->buffer |= _byteswap_uint64(raw) >> reader->bits;
            reader->offset += take;
            reader->bits += take * 8u;
            return;
        }
    }
    while (reader->bits <= 56 && reader->stop == JpegError::none) {
        if (reader->offset >= count) {
            reader->stop = JpegError::truncated;
            break;
        }
        const u8 value = data[reader->offset];
        if (value == 0xFF) {
            u32 next = reader->offset + 1;
            while (next < count && data[next] == 0xFF) {
                ++next;
            }
            if (next >= count || data[next] != 0) {
                reader->stop = JpegError::restart;
                break;
            }
            reader->offset = next + 1;
        } else {
            ++reader->offset;
        }
        reader->buffer |= static_cast<u64>(value) << (56u - reader->bits);
        reader->bits += 8;
    }
}

__forceinline void entropy_consume(JpegEntropyReader* reader, u32 count) noexcept {
    reader->buffer <<= count;
    reader->bits -= count;
}

__forceinline u32 entropy_read_bit(JpegEntropyReader* reader) noexcept {
    if (reader->error != JpegError::none) {
        return 0;
    }
    if (reader->bits == 0) {
        entropy_refill(reader);
        if (reader->bits == 0) {
            reader->error = reader->stop;
            return 0;
        }
    }
    const u32 bit = static_cast<u32>(reader->buffer >> 63);
    entropy_consume(reader, 1);
    return bit;
}

// count is a nibble of a Huffman symbol (at most 15).
__forceinline u32 entropy_read_bits(JpegEntropyReader* reader, u32 count) noexcept {
    if (count == 0 || reader->error != JpegError::none) {
        return 0;
    }
    if (reader->bits < count) {
        entropy_refill(reader);
        if (reader->bits < count) {
            reader->error = reader->stop;
            return 0;
        }
    }
    const u32 value = static_cast<u32>(reader->buffer >> (64u - count));
    entropy_consume(reader, count);
    return value;
}

__forceinline bool entropy_align(JpegEntropyReader* reader) noexcept {
    if (reader->error != JpegError::none) {
        return false;
    }
    // Bits of a partly consumed byte: every refill adds whole bytes.
    const u32 partial = reader->bits & 7u;
    if (partial != 0) {
        const u32 mask = (1u << partial) - 1u;
        if (static_cast<u32>(reader->buffer >> (64u - partial)) != mask) {
            reader->error = JpegError::entropy;
            return false;
        }
        entropy_consume(reader, partial);
    }
    return true;
}

// Aligned and every loaded byte consumed: the input position a bit-serial reader would have.
__forceinline bool entropy_at_end(JpegEntropyReader* reader) noexcept {
    return entropy_align(reader) && reader->bits == 0 && reader->offset == reader->bytes.count;
}

__forceinline bool entropy_consume_restart(JpegEntropyReader* reader, u8 expected) noexcept {
    // Buffered whole bytes are entropy data a bit-serial reader would find
    // where the marker must start.
    if (!entropy_align(reader) || reader->bits != 0 || reader->offset >= reader->bytes.count ||
        reader->bytes[reader->offset] != 0xFF) {
        if (reader->error == JpegError::none) {
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
    reader->buffer = 0;
    reader->stop = JpegError::none;
    return true;
}

// A 9-bit lookahead over one Huffman table. An entry holds the code length
// (bits 8-11) and symbol of the shortest code prefixing the index, filled in
// increasing length order exactly as a bit-serial match finds it; zero means
// no code of nine bits or fewer matches.
inline constexpr u32 kJpegLookaheadBits = 9;
inline constexpr u32 kJpegEndOfBlock = 0x8000u;
struct JpegDecodeTable {
    const JpegHuffmanTable* table;
    u16 lookahead[1u << kJpegLookaheadBits];
    // Baseline AC: a codeword and its magnitude bits within the lookahead.
    // value << 16 | run << 5 | total bits; zero when they do not fit.
    // kJpegEndOfBlock marks the end-of-block codeword (total = its length).
    u32 coefficient[1u << kJpegLookaheadBits];
};

void build_decode_table(const JpegHuffmanTable& table, JpegDecodeTable* decode, bool coefficients = false) noexcept {
    decode->table = &table;
    for (u16& entry : decode->lookahead) {
        entry = 0;
    }
    if (!table.present) {
        if (coefficients) {
            for (u32& entry : decode->coefficient) {
                entry = 0;
            }
        }
        return;
    }
    for (u32 length = 0; length < kJpegLookaheadBits; ++length) {
        if (table.maximum_code[length] < 0) {
            continue;
        }
        const u32 shift = kJpegLookaheadBits - 1u - length;
        for (i32 code = table.minimum_code[length]; code <= table.maximum_code[length]; ++code) {
            const i32 index = table.value_offset[length] + code;
            if (index < 0 || static_cast<u32>(index) >= table.symbol_count) {
                continue; // the canonical path reports it
            }
            const u16 entry = static_cast<u16>((length + 1u) << 8 | table.symbols[index]);
            const u32 first = static_cast<u32>(code) << shift;
            for (u32 fill = 0; fill < (1u << shift); ++fill) {
                if (decode->lookahead[first + fill] == 0) {
                    decode->lookahead[first + fill] = entry;
                }
            }
        }
    }
    if (!coefficients) {
        return;
    }
    for (u32 index = 0; index < (1u << kJpegLookaheadBits); ++index) {
        const u32 entry = decode->lookahead[index];
        const u32 length = entry >> 8;
        const u32 size = entry & 0x0Fu;
        decode->coefficient[index] = 0;
        if (entry == 0) {
            continue;
        }
        if ((entry & 0xFFu) == 0) {
            decode->coefficient[index] = kJpegEndOfBlock | length;
            continue;
        }
        if (size == 0 || length + size > kJpegLookaheadBits) {
            continue;
        }
        const u32 raw = index >> (kJpegLookaheadBits - length - size) & ((1u << size) - 1u);
        const i32 value = raw < (1u << (size - 1u)) ? static_cast<i32>(raw) - static_cast<i32>((1u << size) - 1u)
                                                    : static_cast<i32>(raw);
        decode->coefficient[index] =
            static_cast<u32>(value) << 16 | (entry >> 4 & 0x0Fu) << 5 | (length + size);
    }
}

// Lookahead tables for the Huffman tables a baseline scan's components use.
void build_component_tables(const JpegImage& image, JpegDecodeTable* dc, JpegDecodeTable* ac) noexcept {
    u32 built = 0;
    for (u32 index = 0; index < image.component_count && index < kJpegComponentMaximum; ++index) {
        const JpegComponent& component = image.components[index];
        if (component.dc_table < kJpegTableMaximum && (built & (1u << component.dc_table)) == 0) {
            build_decode_table(image.tables->dc[component.dc_table], &dc[component.dc_table]);
            built |= 1u << component.dc_table;
        }
        if (component.ac_table < kJpegTableMaximum && (built & (16u << component.ac_table)) == 0) {
            build_decode_table(image.tables->ac[component.ac_table], &ac[component.ac_table], true);
            built |= 16u << component.ac_table;
        }
    }
}

// Bit-serial canonical match over a 16-bit window: length << 8 | symbol, with
// bit 16 set when the matching codeword indexes past the symbols; 0 if no
// codeword of up to 16 bits matches.
__declspec(noinline) u32 canonical_entry(const JpegHuffmanTable& table, u32 window) noexcept {
    for (u32 length = 0; length < 16; ++length) {
        const i32 code = static_cast<i32>(window >> (15u - length));
        if (table.maximum_code[length] >= 0 && code >= table.minimum_code[length] &&
            code <= table.maximum_code[length]) {
            const i32 index = table.value_offset[length] + code;
            if (index < 0 || static_cast<u32>(index) >= table.symbol_count) {
                return 0x10000u | (length + 1u) << 8;
            }
            return (length + 1u) << 8 | table.symbols[index];
        }
    }
    return 0;
}

__forceinline bool decode_huffman_symbol(JpegEntropyReader* reader, const JpegDecodeTable& decode,
                                         u8* symbol) noexcept {
    if (!decode.table->present) {
        reader->error = JpegError::huffman;
        return false;
    }
    if (reader->error != JpegError::none) {
        return false;
    }
    if (reader->bits < 16) {
        entropy_refill(reader);
    }
    u32 entry = decode.lookahead[reader->buffer >> (64u - kJpegLookaheadBits)];
    if (entry == 0) {
        entry = canonical_entry(*decode.table, static_cast<u32>(reader->buffer >> 48));
        if (entry == 0) {
            // A bit-serial reader runs out of input before it has read 16 bits.
            reader->error = reader->bits < 16 ? reader->stop : JpegError::huffman;
            return false;
        }
    }
    const u32 length = entry >> 8 & 0x1Fu;
    if (length > reader->bits) {
        reader->error = reader->stop;
        return false;
    }
    if ((entry & 0x10000u) != 0) {
        reader->error = JpegError::huffman;
        return false;
    }
    entropy_consume(reader, length);
    *symbol = static_cast<u8>(entry);
    return true;
}

__forceinline i32 receive_extend(JpegEntropyReader* reader, u32 bits) noexcept {
    if (bits == 0) {
        return 0;
    }
    const u32 value = entropy_read_bits(reader, bits);
    const u32 threshold = 1u << (bits - 1u);
    return value < threshold ? static_cast<i32>(value) - static_cast<i32>((1u << bits) - 1u)
                             : static_cast<i32>(value);
}

__forceinline bool decode_block_local(JpegEntropyReader* reader, const JpegDecodeTable& dc_table,
                                      const JpegDecodeTable& ac_table, i32* predictor, JpegCoefficientBlock* block,
                                      u32* huffman_symbols) noexcept {
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
        if (reader->bits < 16) {
            entropy_refill(reader);
        }
        // Codeword and magnitude bits in one lookup: the same consumption,
        // checks and value as the symbol path below.
        const u32 fast = ac_table.coefficient[reader->buffer >> (64u - kJpegLookaheadBits)];
        if (fast != 0) {
            const u32 total = fast & 0x1Fu;
            const u32 skip = fast >> 5 & 0x0Fu;
            if ((fast & kJpegEndOfBlock) != 0) {
                if (total <= reader->bits) {
                    entropy_consume(reader, total);
                    ++*huffman_symbols;
                    break;
                }
            } else if (total <= reader->bits && coefficient + skip < 64) {
                entropy_consume(reader, total);
                ++*huffman_symbols;
                coefficient += skip;
                block->values[kZigzagToRaster[coefficient]] = static_cast<i16>(static_cast<i32>(fast) >> 16);
                ++coefficient;
                continue;
            }
        }
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

// The reader state lives in a local copy (registers) for the whole block.
bool decode_block(JpegEntropyReader* reader, const JpegDecodeTable& dc_table, const JpegDecodeTable& ac_table,
                  i32* predictor, JpegCoefficientBlock* block, u32* huffman_symbols) noexcept {
    JpegEntropyReader local = *reader;
    u32 symbols = *huffman_symbols;
    const bool ok = decode_block_local(&local, dc_table, ac_table, predictor, block, &symbols);
    *reader = local;
    *huffman_symbols = symbols;
    return ok;
}

__forceinline bool store_coefficient(JpegEntropyReader* reader, i32 value, i16* destination) noexcept {
    if (value < -32'768 || value > 32'767) {
        reader->error = JpegError::coefficient;
        return false;
    }
    *destination = static_cast<i16>(value);
    return true;
}

__forceinline bool decode_progressive_dc_initial(JpegEntropyReader* reader, const JpegDecodeTable& table, i32* predictor,
                                   u8 approximation_low, JpegCoefficientBlock* block,
                                   u32* huffman_symbols) noexcept {
    u8 symbol = 0;
    if (!decode_huffman_symbol(reader, table, &symbol) || symbol > 11) {
        if (reader->error == JpegError::none) {
            reader->error = JpegError::coefficient;
        }
        return false;
    }
    ++*huffman_symbols;
    *predictor += receive_extend(reader, symbol);
    const i64 scaled = static_cast<i64>(*predictor) * (1ull << approximation_low);
    return reader->error == JpegError::none && scaled >= -32'768 && scaled <= 32'767 &&
           store_coefficient(reader, static_cast<i32>(scaled), &block->values[0]);
}

__forceinline bool decode_progressive_dc_refinement(JpegEntropyReader* reader, u8 approximation_low,
                                      JpegCoefficientBlock* block) noexcept {
    const i32 delta = 1u << approximation_low;
    const u32 bit = entropy_read_bit(reader);
    i32 value = block->values[0];
    if (reader->error != JpegError::none) {
        return false;
    }
    if (bit != 0) {
        if ((value & delta) != 0) {
            reader->error = JpegError::coefficient;
            return false;
        }
        value |= delta;
    }
    return store_coefficient(reader, value, &block->values[0]);
}

__forceinline bool decode_progressive_ac_initial(JpegEntropyReader* reader, const JpegDecodeTable& table,
                                   u8 spectral_start, u8 spectral_end, u8 approximation_low,
                                   JpegCoefficientBlock* block, u32* eob_run,
                                   u32* huffman_symbols) noexcept {
    if (*eob_run != 0) {
        --*eob_run;
        return true;
    }
    u32 coefficient = spectral_start;
    while (coefficient <= spectral_end) {
        if (reader->bits < 16) {
            entropy_refill(reader);
        }
        // One lookup for codeword and magnitude bits when every check below passes.
        const u32 fast = table.coefficient[reader->buffer >> (64u - kJpegLookaheadBits)];
        if (fast != 0 && (fast & kJpegEndOfBlock) == 0) {
            const u32 total = fast & 0x1Fu;
            const u32 skip = fast >> 5 & 0x0Fu;
            const i32 scaled = (static_cast<i32>(fast) >> 16) * (1 << approximation_low);
            if (total <= reader->bits && coefficient + skip <= spectral_end && scaled >= -32'768 &&
                scaled <= 32'767 && block->values[kZigzagToRaster[coefficient + skip]] == 0) {
                entropy_consume(reader, total);
                ++*huffman_symbols;
                coefficient += skip;
                block->values[kZigzagToRaster[coefficient]] = static_cast<i16>(scaled);
                ++coefficient;
                continue;
            }
        }
        u8 symbol = 0;
        if (!decode_huffman_symbol(reader, table, &symbol)) {
            return false;
        }
        ++*huffman_symbols;
        const u32 run = symbol >> 4;
        const u32 bits = symbol & 0x0F;
        if (bits == 0) {
            if (run == 15) {
                if (coefficient + 16u > static_cast<u32>(spectral_end) + 1u) {
                    reader->error = JpegError::coefficient;
                    return false;
                }
                coefficient += 16;
                continue;
            }
            *eob_run = (1u << run) + entropy_read_bits(reader, run);
            if (reader->error != JpegError::none || *eob_run == 0) {
                reader->error = JpegError::coefficient;
                return false;
            }
            --*eob_run;
            return true;
        }
        if (bits > 10 || coefficient + run > spectral_end) {
            reader->error = JpegError::coefficient;
            return false;
        }
        coefficient += run;
        const i32 unscaled = receive_extend(reader, bits);
        const i64 scaled = static_cast<i64>(unscaled) * (1ull << approximation_low);
        i16& destination = block->values[kZigzagToRaster[coefficient]];
        if (reader->error != JpegError::none || destination != 0 || scaled < -32'768 || scaled > 32'767 ||
            !store_coefficient(reader, static_cast<i32>(scaled), &destination)) {
            if (reader->error == JpegError::none) {
                reader->error = JpegError::coefficient;
            }
            return false;
        }
        ++coefficient;
    }
    return true;
}

__forceinline bool refine_nonzeroes(JpegEntropyReader* reader, JpegCoefficientBlock* block, u32* coefficient,
                      u32 spectral_end, i32 zeroes_to_skip, i32 delta) noexcept {
    if (delta <= 0) {
        return false;
    }
    while (*coefficient <= spectral_end) {
        i16& stored = block->values[kZigzagToRaster[*coefficient]];
        i32 value = stored;
        if (value == 0) {
            if (zeroes_to_skip == 0) {
                return true;
            }
            if (zeroes_to_skip > 0) {
                --zeroes_to_skip;
            }
            ++*coefficient;
            continue;
        }
        const u32 bit = entropy_read_bit(reader);
        if (reader->error != JpegError::none) {
            return false;
        }
        if (bit != 0) {
            const i32 magnitude = value < 0 ? -value : value;
            if ((magnitude & delta) != 0) {
                reader->error = JpegError::coefficient;
                return false;
            }
            value += value < 0 ? -delta : delta;
            if (!store_coefficient(reader, value, &stored)) {
                return false;
            }
        }
        ++*coefficient;
    }
    if (zeroes_to_skip >= 0) {
        reader->error = JpegError::coefficient;
        return false;
    }
    return true;
}

__forceinline bool decode_progressive_ac_refinement(JpegEntropyReader* reader, const JpegDecodeTable& table,
                                      u8 spectral_start, u8 spectral_end, u8 approximation_low,
                                      JpegCoefficientBlock* block, u32* eob_run,
                                      u32* huffman_symbols) noexcept {
    const i32 delta = 1u << approximation_low;
    u32 coefficient = spectral_start;
    if (*eob_run == 0) {
        while (coefficient <= spectral_end) {
            u8 symbol = 0;
            if (!decode_huffman_symbol(reader, table, &symbol)) {
                return false;
            }
            ++*huffman_symbols;
            const u32 zeroes = symbol >> 4;
            const u32 magnitude = symbol & 0x0F;
            i32 new_value = 0;
            if (magnitude == 0) {
                if (zeroes != 15) {
                    *eob_run = (1u << zeroes) + entropy_read_bits(reader, zeroes);
                    if (reader->error != JpegError::none || *eob_run == 0) {
                        reader->error = JpegError::coefficient;
                        return false;
                    }
                    break;
                }
            } else if (magnitude == 1) {
                new_value = entropy_read_bit(reader) != 0 ? delta : -delta;
                if (reader->error != JpegError::none) {
                    return false;
                }
            } else {
                reader->error = JpegError::coefficient;
                return false;
            }
            if (!refine_nonzeroes(reader, block, &coefficient, spectral_end, static_cast<i32>(zeroes),
                                  delta) ||
                coefficient > spectral_end) {
                if (reader->error == JpegError::none) {
                    reader->error = JpegError::coefficient;
                }
                return false;
            }
            if (new_value != 0) {
                i16& destination = block->values[kZigzagToRaster[coefficient]];
                if (destination != 0 || !store_coefficient(reader, new_value, &destination)) {
                    reader->error = JpegError::coefficient;
                    return false;
                }
            }
            ++coefficient;
        }
    }
    if (*eob_run != 0) {
        --*eob_run;
        if (!refine_nonzeroes(reader, block, &coefficient, spectral_end, -1, delta)) {
            return false;
        }
    }
    return true;
}

__forceinline bool decode_progressive_block(JpegEntropyReader* reader, const JpegDecodeTable* dc, const JpegDecodeTable* ac,
                              const JpegScan& scan, u32 scan_component, i32* predictor, JpegCoefficientBlock* block,
                              u32* eob_run, u32* huffman_symbols) noexcept {
    if (scan.spectral_start == 0) {
        return scan.approximation_high == 0
                   ? decode_progressive_dc_initial(reader, dc[scan.dc_tables[scan_component]],
                                                   predictor, scan.approximation_low, block,
                                                   huffman_symbols)
                   : decode_progressive_dc_refinement(reader, scan.approximation_low, block);
    }
    return scan.approximation_high == 0
               ? decode_progressive_ac_initial(reader, ac[scan.ac_tables[scan_component]],
                                               scan.spectral_start, scan.spectral_end,
                                               scan.approximation_low, block, eob_run, huffman_symbols)
               : decode_progressive_ac_refinement(reader, ac[scan.ac_tables[scan_component]],
                                                  scan.spectral_start, scan.spectral_end,
                                                  scan.approximation_low, block, eob_run, huffman_symbols);
}

bool decode_progressive_scan(ConstSlice<u8> encoded, const JpegImage& image, const JpegTables& tables,
                             const JpegScan& scan, JpegCoefficientBlock* blocks,
                             JpegDecodeResult* result) noexcept {
    if (static_cast<u64>(scan.entropy_offset) + scan.entropy_bytes > encoded.count) {
        return false;
    }
    JpegEntropyReader reader = entropy_reader(ConstSlice<u8>{encoded.data + scan.entropy_offset, scan.entropy_bytes});
    // Lookahead tables for the Huffman tables this scan references only.
    JpegDecodeTable dc[kJpegTableMaximum];
    JpegDecodeTable ac[kJpegTableMaximum];
    u8 built = 0;
    for (u32 index = 0; index < scan.component_count; ++index) {
        if (scan.spectral_start == 0 && scan.approximation_high == 0) {
            const u8 table = scan.dc_tables[index];
            if (table < kJpegTableMaximum && (built & (1u << table)) == 0) {
                build_decode_table(tables.dc[table], &dc[table]);
                built |= static_cast<u8>(1u << table);
            }
        } else if (scan.spectral_start != 0) {
            const u8 table = scan.ac_tables[index];
            if (table < kJpegTableMaximum && (built & (16u << table)) == 0) {
                // First AC scans take codeword and magnitude in one lookup.
                build_decode_table(tables.ac[table], &ac[table], scan.approximation_high == 0);
                built |= static_cast<u8>(16u << table);
            }
        }
    }
    i32 predictor[kJpegComponentMaximum]{};
    u32 eob_run = 0;
    u8 expected_restart = 0;
    // An interleaved scan walks MCUs (each component's sampling factors); a
    // single-component scan walks that component's blocks, one per data unit.
    // One loop serves both so the block decoders are expanded once.
    const bool interleaved = scan.component_count > 1;
    const u32 unit_columns =
        interleaved ? image.mcu_columns : scan_component_columns(image, image.components[scan.component_indices[0]]);
    u32 unit_x = 0;
    u32 unit_y = 0;
    for (u32 unit = 0; unit < scan.data_units; ++unit) {
        if (scan.restart_interval != 0 && unit != 0 && unit % scan.restart_interval == 0) {
            if (!entropy_consume_restart(&reader, expected_restart)) {
                result->error = reader.error;
                return false;
            }
            expected_restart = static_cast<u8>((expected_restart + 1u) & 7u);
            for (u32 index = 0; index < image.component_count; ++index) {
                predictor[index] = 0;
            }
            eob_run = 0;
        }
        for (u32 scan_component = 0; scan_component < scan.component_count; ++scan_component) {
            const u32 component_index = scan.component_indices[scan_component];
            const JpegComponent& component = image.components[component_index];
            const u32 horizontal_blocks = interleaved ? component.horizontal_sampling : 1u;
            const u32 vertical_blocks = interleaved ? component.vertical_sampling : 1u;
            for (u32 vertical = 0; vertical < vertical_blocks; ++vertical) {
                for (u32 horizontal = 0; horizontal < horizontal_blocks; ++horizontal) {
                    const u32 block_x = unit_x * horizontal_blocks + horizontal;
                    const u32 block_y = unit_y * vertical_blocks + vertical;
                    const u32 block_index = component.block_offset + block_y * component.block_columns + block_x;
                    if (block_index >= image.coefficient_blocks ||
                        !decode_progressive_block(&reader, dc, ac, scan, scan_component, &predictor[component_index],
                                                  &blocks[block_index], &eob_run, &result->huffman_symbols)) {
                        result->error = reader.error == JpegError::none ? JpegError::coefficient : reader.error;
                        return false;
                    }
                    ++result->decoded_blocks;
                }
            }
        }
        ++result->decoded_mcus;
        if (++unit_x == unit_columns) {
            unit_x = 0;
            ++unit_y;
        }
    }
    if (eob_run != 0 || !entropy_at_end(&reader)) {
        result->error = reader.error == JpegError::none ? JpegError::entropy : reader.error;
        return false;
    }
    ++result->decoded_scans;
    return true;
}

JpegParseResult parse_jpeg(ConstSlice<u8> encoded, JpegImage* image, JpegWorkspace* workspace,
                           bool thumbnail) noexcept {
    JpegParseResult result{};
    if (!image || !workspace || (!encoded.data && encoded.count != 0)) {
        return parse_failure(result, JpegError::invalid_input);
    }
    *image = JpegImage{};
    image->orientation = JpegOrientation::top_left;
    workspace->tables = JpegTables{};
    workspace->icc.profile = JpegIccProfile{};
    workspace->icc.profile.entries = workspace->icc.entries;
    workspace->tables.icc = &workspace->icc;
    image->tables = &workspace->tables;
    if (encoded.count > (thumbnail ? kJpegThumbnailEncodedMaximumBytes : kJpegEncodedMaximumBytes)) {
        return parse_failure(result, JpegError::encoded_limit);
    }
    if (encoded.count < 4 || encoded[0] != 0xFF || encoded[1] != 0xD8) {
        return parse_failure(result, JpegError::signature);
    }

    bool adobe_transform_valid = false;
    bool orientation_found = false;
    bool jfif_prefix_exif_only = true;
    bool icc_marker_seen = false;
    bool icc_transport_malformed = false;
    u8 adobe_transform = 0;
    u32 icc_declared_chunks = 0;
    u32 icc_seen_chunks = 0;
    u8 quantization_locked = 0;
    u16 restart_interval = 0;
    JpegIccChunk icc_chunks[kJpegIccChunkMaximum];
    u32 icc_chunk_presence[8]{};
    u8 progression[kJpegComponentMaximum][64]{};
    for (u32 component = 0; component < kJpegComponentMaximum; ++component) {
        for (u32 coefficient = 0; coefficient < 64; ++coefficient) {
            progression[component][coefficient] = 0xFF;
        }
    }
    u32 cursor = 2;
    while (cursor < encoded.count) {
        const bool first_segment = result.segments == 0;
        u8 marker = 0;
        if (!read_marker(encoded, &cursor, &marker)) {
            return parse_failure(result, JpegError::marker);
        }
        if (marker == 0xD9) {
            if (result.scans == 0 || cursor != encoded.count) {
                return parse_failure(result,
                                     result.scans == 0 ? JpegError::order : JpegError::trailing_data);
            }
            if (image->progressive) {
                for (u32 component = 0; component < image->component_count; ++component) {
                    if (progression[component][0] == 0xFF) {
                        return parse_failure(result, JpegError::order);
                    }
                }
            }
            if (image->component_count == 1) {
                image->color_transform = JpegColorTransform::grayscale;
            } else if (image->component_count == 3 && adobe_transform_valid) {
                if (adobe_transform > 1) {
                    return parse_failure(result, JpegError::unsupported_color);
                }
                image->color_transform =
                    adobe_transform == 0 ? JpegColorTransform::rgb : JpegColorTransform::ycbcr;
            } else if (image->component_count == 3) {
                image->color_transform = JpegColorTransform::ycbcr;
            } else if (adobe_transform_valid) {
                if (adobe_transform != 0 && adobe_transform != 2) {
                    return parse_failure(result, JpegError::unsupported_color);
                }
                image->color_transform =
                    adobe_transform == 0 ? JpegColorTransform::cmyk : JpegColorTransform::ycck;
            } else {
                image->color_transform = JpegColorTransform::cmyk;
            }
            if (image->has_jfif && image->component_count == 4) {
                return parse_failure(result, JpegError::unsupported_color);
            }
            if (image->has_jfif && image->component_count == 3 &&
                (image->components[0].identifier != 1 || image->components[1].identifier != 2 ||
                 image->components[2].identifier != 3)) {
                return parse_failure(result, JpegError::unsupported_color);
            }
            if (image->color_transform == JpegColorTransform::rgb) {
                for (u32 index = 0; index < 3; ++index) {
                    if (image->components[index].horizontal_sampling != 1 ||
                        image->components[index].vertical_sampling != 1) {
                        return parse_failure(result, JpegError::unsupported_sampling);
                    }
                }
            }
            image->scan_count = static_cast<u16>(result.scans);
            image->restart_markers = result.restart_markers;
            finalize_icc_profile(encoded, icc_chunks, icc_seen_chunks, icc_declared_chunks, icc_marker_seen,
                                 icc_transport_malformed, *image, &workspace->icc);
            result.error = JpegError::none;
            return result;
        }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            return parse_failure(result, JpegError::order);
        }
        u32 payload = 0;
        u32 payload_bytes = 0;
        if (!segment_bounds(encoded, &cursor, &payload, &payload_bytes)) {
            return parse_failure(result, JpegError::truncated);
        }
        ++result.segments;

        const bool exif_segment =
            marker == 0xE1 && payload_bytes >= 5 && bytes_equal(encoded.data + payload, "Exif\0", 5);
        const bool icc_segment = marker == 0xE2 && payload_bytes >= 12 &&
                                 bytes_equal(encoded.data + payload, "ICC_PROFILE\0", 12);
        JpegError error = JpegError::none;
        if (marker == 0xE0 && payload_bytes >= 5 && bytes_equal(encoded.data + payload, "JFIF\0", 5)) {
            if ((!first_segment && !jfif_prefix_exif_only) || image->has_jfif || payload_bytes < 14) {
                return parse_failure(result, JpegError::order);
            }
            const u8 major = encoded[payload + 5];
            const u8 units = encoded[payload + 7];
            const u16 density_x = byte_load_be16(encoded.data + payload + 8);
            const u16 density_y = byte_load_be16(encoded.data + payload + 10);
            const u32 thumbnail_x = encoded[payload + 12];
            const u32 thumbnail_y = encoded[payload + 13];
            const u64 expected = 14ull + 3ull * thumbnail_x * thumbnail_y;
            if (major != 1 || units > 2 || density_x == 0 || density_y == 0 || expected != payload_bytes) {
                return parse_failure(result, JpegError::segment);
            }
            image->has_jfif = true;
        } else if (marker == 0xEE && payload_bytes >= 5 &&
                   bytes_equal(encoded.data + payload, "Adobe", 5)) {
            if (image->has_adobe || payload_bytes < 12) {
                return parse_failure(result, JpegError::duplicate);
            }
            image->has_adobe = true;
            adobe_transform = encoded[payload + 11];
            adobe_transform_valid = true;
        } else if (exif_segment && !orientation_found) {
            JpegOrientation orientation = JpegOrientation::top_left;
            if (parse_exif_orientation(encoded.data + payload, payload_bytes, &orientation)) {
                image->orientation = orientation;
                orientation_found = true;
            }
        } else if (icc_segment) {
            icc_marker_seen = true;
            if (payload_bytes < 14) {
                icc_transport_malformed = true;
            } else {
                const u32 sequence = encoded[payload + 12u];
                const u32 total = encoded[payload + 13u];
                if (sequence == 0 || total == 0 || sequence > total ||
                    (icc_declared_chunks != 0 && icc_declared_chunks != total)) {
                    icc_transport_malformed = true;
                } else {
                    if (icc_declared_chunks == 0) {
                        icc_declared_chunks = total;
                    }
                    const u32 chunk_index = sequence - 1u;
                    const u32 presence_word = chunk_index >> 5u;
                    const u32 presence_mask = 1u << (chunk_index & 31u);
                    if ((icc_chunk_presence[presence_word] & presence_mask) != 0) {
                        icc_transport_malformed = true;
                    } else {
                        icc_chunk_presence[presence_word] |= presence_mask;
                        JpegIccChunk& chunk = icc_chunks[chunk_index];
                        chunk.encoded_offset = payload + 14u;
                        chunk.bytes = payload_bytes - 14u;
                        ++icc_seen_chunks;
                    }
                }
            }
        } else if (marker == 0xDB) {
            error = parse_quantization(encoded, payload, payload_bytes, quantization_locked, image->tables);
        } else if (marker == 0xC4) {
            error = parse_huffman(encoded, payload, payload_bytes, image->tables);
        } else if (marker == 0xC0 || marker == 0xC2) {
            error = parse_frame(encoded, payload, payload_bytes, image, thumbnail);
            if (error == JpegError::none) {
                image->progressive = marker == 0xC2;
            }
        } else if (marker >= 0xC1 && marker <= 0xCF) {
            return parse_failure(result, JpegError::unsupported_process);
        } else if (marker == 0xDD) {
            if (payload_bytes != 2) {
                return parse_failure(result, JpegError::segment);
            }
            restart_interval = byte_load_be16(encoded.data + payload);
        } else if (marker == 0xDA) {
            if (result.scans >= kJpegProgressiveScanMaximum || (!image->progressive && result.scans != 0)) {
                return parse_failure(result, JpegError::unsupported_process);
            }
            JpegScan scan{};
            error = parse_scan_header(encoded, payload, payload_bytes, *image, *image->tables,
                                      restart_interval, &scan);
            if (error != JpegError::none) {
                return parse_failure(result, error);
            }
            if (image->progressive) {
                error = validate_progression(scan, progression);
                if (error != JpegError::none) {
                    return parse_failure(result, error);
                }
            }
            u32 next_marker = 0;
            error = find_entropy_end(encoded, cursor, &scan, &next_marker);
            if (error != JpegError::none) {
                return parse_failure(result, error);
            }
            const u32 expected_restarts =
                scan.restart_interval == 0 ? 0 : (scan.data_units - 1u) / scan.restart_interval;
            if (scan.restart_markers != expected_restarts) {
                return parse_failure(result, JpegError::restart);
            }
            if (result.scans == 0) {
                image->entropy_offset = scan.entropy_offset;
                image->entropy_bytes = scan.entropy_bytes;
            }
            for (u32 index = 0; index < scan.component_count; ++index) {
                const u32 component_index = scan.component_indices[index];
                JpegComponent& component = image->components[component_index];
                component.dc_table = scan.dc_tables[index];
                component.ac_table = scan.ac_tables[index];
                image->scan_order[index] = static_cast<u8>(component_index);
                quantization_locked |= static_cast<u8>(1u << component.quantization_table);
            }
            image->restart_interval = scan.restart_interval;
            result.restart_markers += scan.restart_markers;
            ++result.scans;
            cursor = next_marker;
            continue;
        }
        if (error != JpegError::none) {
            return parse_failure(result, error);
        }
        if (!image->has_jfif && !exif_segment) {
            jfif_prefix_exif_only = false;
        }
    }
    return parse_failure(result, JpegError::truncated);
}

// Re-walks the markers of a progressive file and decodes every scan into blocks, which
// holds image.coefficient_blocks entries.
JpegDecodeResult decode_progressive_scans(ConstSlice<u8> encoded, const JpegImage& image,
                                          JpegCoefficientBlock* blocks) noexcept {
    JpegDecodeResult result{};
    if (image.scan_count == 0 || encoded.count < 4 || encoded[0] != 0xFF || encoded[1] != 0xD8) {
        result.error = JpegError::invalid_input;
        return result;
    }
    for (u32 block = 0; block < image.coefficient_blocks; ++block) {
        for (u32 coefficient = 0; coefficient < 64; ++coefficient) {
            blocks[block].values[coefficient] = 0;
        }
    }
    JpegTables tables{};
    u16 restart_interval = 0;
    u8 quantization_locked = 0;
    u8 progression[kJpegComponentMaximum][64]{};
    for (u32 component = 0; component < kJpegComponentMaximum; ++component) {
        for (u32 coefficient = 0; coefficient < 64; ++coefficient) {
            progression[component][coefficient] = 0xFF;
        }
    }
    u32 cursor = 2;
    u32 scans = 0;
    while (cursor < encoded.count) {
        u8 marker = 0;
        if (!read_marker(encoded, &cursor, &marker)) {
            result.error = JpegError::marker;
            return result;
        }
        if (marker == 0xD9) {
            if (cursor != encoded.count || scans != image.scan_count ||
                result.decoded_scans != image.scan_count) {
                result.error = cursor != encoded.count ? JpegError::trailing_data : JpegError::scan;
                return result;
            }
            result.error = JpegError::none;
            return result;
        }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            result.error = JpegError::order;
            return result;
        }
        u32 payload = 0;
        u32 payload_bytes = 0;
        if (!segment_bounds(encoded, &cursor, &payload, &payload_bytes)) {
            result.error = JpegError::truncated;
            return result;
        }
        JpegError error = JpegError::none;
        if (marker == 0xDB) {
            error = parse_quantization(encoded, payload, payload_bytes, quantization_locked, &tables);
        } else if (marker == 0xC4) {
            error = parse_huffman(encoded, payload, payload_bytes, &tables);
        } else if (marker == 0xDD) {
            if (payload_bytes != 2) {
                result.error = JpegError::segment;
                return result;
            }
            restart_interval = byte_load_be16(encoded.data + payload);
        } else if (marker == 0xDA) {
            JpegScan scan{};
            error = parse_scan_header(encoded, payload, payload_bytes, image, tables, restart_interval,
                                      &scan);
            if (error != JpegError::none) {
                result.error = error;
                return result;
            }
            error = validate_progression(scan, progression);
            if (error != JpegError::none) {
                result.error = error;
                return result;
            }
            u32 next_marker = 0;
            error = find_entropy_end(encoded, cursor, &scan, &next_marker);
            if (error != JpegError::none ||
                !decode_progressive_scan(encoded, image, tables, scan, blocks, &result)) {
                if (result.error == JpegError::none) {
                    result.error = error == JpegError::none ? JpegError::coefficient : error;
                }
                return result;
            }
            for (u32 index = 0; index < scan.component_count; ++index) {
                const JpegComponent& component = image.components[scan.component_indices[index]];
                quantization_locked |= static_cast<u8>(1u << component.quantization_table);
            }
            ++scans;
            cursor = next_marker;
        }
        if (error != JpegError::none) {
            result.error = error;
            return result;
        }
    }
    result.error = JpegError::truncated;
    return result;
}

} // namespace

JpegParseResult jpeg_parse(ConstSlice<u8> encoded, JpegImage* image, JpegWorkspace* workspace) noexcept {
    return parse_jpeg(encoded, image, workspace, false);
}

JpegDecodeResult jpeg_decode_parsed_coefficients(ConstSlice<u8> encoded, const JpegImage& image,
                                                 JpegCoefficientWorkspace* coefficients) noexcept {
    JpegDecodeResult result{};
    if (!coefficients || !image.tables || (!encoded.data && encoded.count != 0) || image.width == 0 ||
        image.height == 0 || image.component_count == 0 || image.component_count > kJpegComponentMaximum ||
        !jpeg_orientation_valid(image.orientation) ||
        image.coefficient_blocks > kJpegCoefficientBlockMaximum ||
        static_cast<u64>(image.entropy_offset) + image.entropy_bytes > encoded.count) {
        result.error = JpegError::invalid_input;
        return result;
    }

    if (image.progressive) {
        return decode_progressive_scans(encoded, image, coefficients->blocks);
    }

    JpegEntropyReader reader = entropy_reader(ConstSlice<u8>{encoded.data + image.entropy_offset, image.entropy_bytes});
    JpegDecodeTable dc_decode[kJpegTableMaximum];
    JpegDecodeTable ac_decode[kJpegTableMaximum];
    build_component_tables(image, dc_decode, ac_decode);
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
            if (component_index >= image.component_count) {
                result.error = JpegError::scan;
                return result;
            }
            const JpegComponent& component = image.components[component_index];
            const JpegDecodeTable& dc = dc_decode[component.dc_table];
            const JpegDecodeTable& ac = ac_decode[component.ac_table];
            for (u32 vertical = 0; vertical < component.vertical_sampling; ++vertical) {
                for (u32 horizontal = 0; horizontal < component.horizontal_sampling; ++horizontal) {
                    const u32 block_x = mcu_x * component.horizontal_sampling + horizontal;
                    const u32 block_y = mcu_y * component.vertical_sampling + vertical;
                    const u32 block_index =
                        component.block_offset + block_y * component.block_columns + block_x;
                    if (block_index >= image.coefficient_blocks ||
                        !decode_block(&reader, dc, ac, &predictor[component_index],
                                      &coefficients->blocks[block_index], &result.huffman_symbols)) {
                        result.error =
                            reader.error == JpegError::none ? JpegError::coefficient : reader.error;
                        return result;
                    }
                    ++result.decoded_blocks;
                }
            }
        }
        ++result.decoded_mcus;
    }
    if (!entropy_at_end(&reader) || result.decoded_blocks != image.coefficient_blocks) {
        result.error = reader.error == JpegError::none ? JpegError::entropy : reader.error;
        return result;
    }
    result.error = JpegError::none;
    result.decoded_scans = 1;
    return result;
}

JpegDecodeResult jpeg_decode_coefficients(ConstSlice<u8> encoded, JpegImage* image,
                                          JpegWorkspace* workspace) noexcept {
    JpegDecodeResult result{};
    result.parse = jpeg_parse(encoded, image, workspace);
    if (!result.parse.ok()) {
        result.error = result.parse.error;
        return result;
    }
    const JpegParseResult parse = result.parse;
    result = jpeg_decode_parsed_coefficients(encoded, *image, &workspace->coefficients);
    result.parse = parse;
    return result;
}

namespace {
inline constexpr float kIdctBasis[8][8] = {
    {0.353553391f, 0.490392640f, 0.461939766f, 0.415734806f, 0.353553391f, 0.277785117f, 0.191341716f, 0.097545161f},
    {0.353553391f, 0.415734806f, 0.191341716f, -0.097545161f, -0.353553391f, -0.490392640f, -0.461939766f, -0.277785117f},
    {0.353553391f, 0.277785117f, -0.191341716f, -0.490392640f, -0.353553391f, 0.097545161f, 0.461939766f, 0.415734806f},
    {0.353553391f, 0.097545161f, -0.461939766f, -0.277785117f, 0.353553391f, 0.415734806f, -0.191341716f, -0.490392640f},
    {0.353553391f, -0.097545161f, -0.461939766f, 0.277785117f, 0.353553391f, -0.415734806f, -0.191341716f, 0.490392640f},
    {0.353553391f, -0.277785117f, -0.191341716f, 0.490392640f, -0.353553391f, -0.097545161f, 0.461939766f, -0.415734806f},
    {0.353553391f, -0.415734806f, 0.191341716f, 0.097545161f, -0.353553391f, 0.490392640f, -0.461939766f, 0.277785117f},
    {0.353553391f, -0.490392640f, 0.461939766f, -0.415734806f, 0.353553391f, -0.277785117f, 0.191341716f, -0.097545161f},
};

struct IdctColumns {
    float values[8][8];
};
constexpr IdctColumns transpose_basis() noexcept {
    IdctColumns result{};
    for (u32 row = 0; row < 8; ++row)
        for (u32 column = 0; column < 8; ++column)
            result.values[column][row] = kIdctBasis[row][column];
    return result;
}
// kIdctBasisColumns.values[u][x] == kIdctBasis[x][u]: one row pass step for eight x at once.
alignas(32) inline constexpr IdctColumns kIdctBasisColumns = transpose_basis();

u8 clamp_sample(float value) noexcept {
    value += .5f;
    return value <= 0.f ? 0 : value >= 255.f ? 255 : static_cast<u8>(value);
}

// Exact 8x8 IDCT, box-averaged to samples x samples (DC alone at one sample).
void reconstruct_block(const JpegCoefficientBlock& block, const JpegQuantizationTable& table, u32 samples,
                       u8* plane, u32 pitch) noexcept {
    if (samples == 1) {
        plane[0] = clamp_sample(static_cast<float>(block.values[0]) * table.values[0] * .125f + 128.f);
        return;
    }
    bool dc_only = true;
    for (u32 i = 1; i < 64; ++i)
        if (block.values[i] != 0) {
            dc_only = false;
            break;
        }
    if (dc_only) {
        // Keep the two basis products and repeated box sum from the full IDCT.
        // DC / 8 can round differently at a sample boundary.
        const float coefficient = static_cast<float>(block.values[0]) * table.values[0];
        const float row = kIdctBasis[0][0] * coefficient;
        const float value = kIdctBasis[0][0] * row;
        const u32 step = 8 / samples;
        const float scale = 1.f / static_cast<float>(step * step);
        float total = 0;
        for (u32 i = 0; i < step * step; ++i)
            total += value;
        const u8 sample = clamp_sample(total * scale + 128.f);
        for (u32 y = 0; y < samples; ++y)
            for (u32 x = 0; x < samples; ++x)
                plane[y * pitch + x] = sample;
        return;
    }
    float coefficients[64];
    for (u32 i = 0; i < 64; ++i)
        coefficients[i] = static_cast<float>(block.values[i]) * table.values[i];
    // Both passes run eight independent outputs per instruction; each output
    // keeps the scalar order (sum from 0, terms in index order), and the box
    // totals below keep the scalar y-then-x order.
    alignas(32) float rows[64];
    for (u32 v = 0; v < 8; ++v) {
        __m256 sum = _mm256_setzero_ps();
        for (u32 u = 0; u < 8; ++u)
            sum = _mm256_add_ps(sum, _mm256_mul_ps(_mm256_load_ps(kIdctBasisColumns.values[u]),
                                                   _mm256_set1_ps(coefficients[v * 8 + u])));
        _mm256_store_ps(rows + v * 8, sum);
    }
    alignas(32) float columns[64];
    for (u32 y = 0; y < 8; ++y) {
        __m256 sum = _mm256_setzero_ps();
        for (u32 v = 0; v < 8; ++v)
            sum = _mm256_add_ps(sum, _mm256_mul_ps(_mm256_set1_ps(kIdctBasis[y][v]), _mm256_load_ps(rows + v * 8)));
        _mm256_store_ps(columns + y * 8, sum);
    }
    const u32 step = 8 / samples;
    const float scale = 1.f / static_cast<float>(step * step);
    for (u32 sy = 0; sy < samples; ++sy)
        for (u32 sx = 0; sx < samples; ++sx) {
            float total = 0;
            for (u32 y = sy * step; y < (sy + 1) * step; ++y)
                for (u32 x = sx * step; x < (sx + 1) * step; ++x)
                    total += columns[y * 8 + x];
            plane[sy * pitch + sx] = clamp_sample(total * scale + 128.f);
        }
}

void reconstruct_component_block(const JpegImage& image, const JpegThumbnailLayout& layout, u8* planes,
                                 u32 component_index, u32 block_x, u32 block_y,
                                 const JpegCoefficientBlock& block) noexcept {
    const JpegComponent& component = image.components[component_index];
    const u32 pitch = layout.plane_pitches[component_index];
    u8* target = planes + layout.plane_offsets[component_index] +
                 static_cast<usize>(block_y) * layout.samples * pitch + static_cast<usize>(block_x) * layout.samples;
    reconstruct_block(block, image.tables->quantization[component.quantization_table], layout.samples, target, pitch);
}

bool convert_planes(const JpegImage& image, const JpegThumbnailLayout& layout, const u8* planes, u8* row,
                    JpegThumbnailRowSink sink, void* context) noexcept {
    const u32 max_h = image.maximum_horizontal_sampling, max_v = image.maximum_vertical_sampling;
    for (u32 y = 0; y < layout.height; ++y) {
        for (u32 x = 0; x < layout.width; ++x) {
            u8 sample[kJpegComponentMaximum]{};
            for (u32 c = 0; c < image.component_count; ++c) {
                const JpegComponent& component = image.components[c];
                const u32 px = x * component.horizontal_sampling / max_h;
                const u32 py = y * component.vertical_sampling / max_v;
                sample[c] = planes[layout.plane_offsets[c] + static_cast<usize>(py) * layout.plane_pitches[c] + px];
            }
            u8* out = row + x * 4u;
            float r = 0, g = 0, b = 0;
            switch (image.color_transform) {
            case JpegColorTransform::grayscale:
                r = g = b = sample[0];
                break;
            case JpegColorTransform::rgb:
                r = sample[0], g = sample[1], b = sample[2];
                break;
            case JpegColorTransform::ycbcr:
            case JpegColorTransform::ycck: {
                const float luma = sample[0], cb = sample[1] - 128.f, cr = sample[2] - 128.f;
                r = luma + 1.402f * cr;
                g = luma - .344136f * cb - .714136f * cr;
                b = luma + 1.772f * cb;
                if (image.color_transform == JpegColorTransform::ycck) {
                    // Inverted Adobe CMYK: the YCC triple is already 255 - CMY.
                    const float k = sample[3] / 255.f;
                    r = (r < 0 ? 0 : r > 255 ? 255 : r) * k;
                    g = (g < 0 ? 0 : g > 255 ? 255 : g) * k;
                    b = (b < 0 ? 0 : b > 255 ? 255 : b) * k;
                }
                break;
            }
            case JpegColorTransform::cmyk: {
                const float k = sample[3] / 255.f;
                r = sample[0] * k, g = sample[1] * k, b = sample[2] * k;
                break;
            }
            }
            out[0] = clamp_sample(r);
            out[1] = clamp_sample(g);
            out[2] = clamp_sample(b);
            out[3] = 255;
        }
        if (!sink(context, y, row)) return false;
    }
    return true;
}
} // namespace

JpegParseResult jpeg_thumbnail_parse(ConstSlice<u8> encoded, u32 target_extent, JpegImage* image,
                                     JpegWorkspace* workspace, JpegThumbnailLayout* layout) noexcept {
    if (!layout || !target_extent)
        return parse_failure({}, JpegError::invalid_input);
    *layout = {};
    JpegParseResult result = parse_jpeg(encoded, image, workspace, true);
    if (!result.ok())
        return result;
    const u32 longest = maximum<u32>(image->width, image->height);
    u32 samples = 1;
    while (samples < 8 && longest * samples / 8u < target_extent)
        samples *= 2;
    layout->samples = static_cast<u8>(samples);
    layout->width = (static_cast<u32>(image->width) * samples + 7u) / 8u;
    layout->height = (static_cast<u32>(image->height) * samples + 7u) / 8u;
    u64 plane = 0;
    for (u32 c = 0; c < image->component_count; ++c) {
        const JpegComponent& component = image->components[c];
        const u64 pitch = static_cast<u64>(component.block_columns) * samples;
        const u64 rows = static_cast<u64>(component.block_rows) * samples;
        if (plane > 0xffffffffull || pitch > 0xffffffffull)
            return parse_failure(result, JpegError::dimensions);
        layout->plane_offsets[c] = static_cast<u32>(plane);
        layout->plane_pitches[c] = static_cast<u32>(pitch);
        plane += pitch * rows;
    }
    layout->plane_bytes = (plane + 15u) & ~15ull;
    layout->coefficient_bytes =
        image->progressive ? static_cast<u64>(image->coefficient_blocks) * sizeof(JpegCoefficientBlock) : 0;
    layout->rgba_bytes = static_cast<u64>(layout->width) * layout->height * 4u;
    if (layout->plane_bytes + layout->coefficient_bytes > kJpegThumbnailScratchMaximumBytes ||
        layout->plane_bytes > 0xffffffffull)
        return parse_failure(result, JpegError::dimensions);
    return result;
}

JpegDecodeResult jpeg_thumbnail_decode_rows(ConstSlice<u8> encoded, const JpegImage& image,
                                            const JpegThumbnailLayout& layout, Slice<u8> scratch,
                                            Slice<u8> row, JpegThumbnailRowSink sink, void* context) noexcept {
    JpegDecodeResult result{};
    const u64 required = layout.coefficient_bytes + layout.plane_bytes;
    if (!sink || !image.tables || !layout.samples || (!encoded.data && encoded.count) || !scratch.data ||
        required > scratch.count || !row.data || u64{layout.width} * 4 > row.count ||
        (reinterpret_cast<usize>(scratch.data) & 15u) || !image.component_count ||
        static_cast<u64>(image.entropy_offset) + image.entropy_bytes > encoded.count) {
        result.error = JpegError::invalid_input;
        return result;
    }
    auto* blocks = reinterpret_cast<JpegCoefficientBlock*>(scratch.data);
    u8* planes = scratch.data + layout.coefficient_bytes;
    if (image.progressive) {
        result = decode_progressive_scans(encoded, image, blocks);
        if (!result.ok()) {
            return result;
        }
        for (u32 c = 0; c < image.component_count; ++c) {
            const JpegComponent& component = image.components[c];
            for (u32 by = 0; by < component.block_rows; ++by)
                for (u32 bx = 0; bx < component.block_columns; ++bx)
                    reconstruct_component_block(image, layout, planes, c, bx, by,
                                                blocks[component.block_offset + by * component.block_columns + bx]);
        }
    } else {
        JpegEntropyReader reader =
            entropy_reader(ConstSlice<u8>{encoded.data + image.entropy_offset, image.entropy_bytes});
        JpegDecodeTable dc_decode[kJpegTableMaximum];
        JpegDecodeTable ac_decode[kJpegTableMaximum];
        build_component_tables(image, dc_decode, ac_decode);
        i32 predictor[kJpegComponentMaximum]{};
        const u32 total_mcus = static_cast<u32>(image.mcu_columns) * image.mcu_rows;
        u8 expected_restart = 0;
        JpegCoefficientBlock block{};
        for (u32 mcu = 0; mcu < total_mcus; ++mcu) {
            if (image.restart_interval != 0 && mcu != 0 && mcu % image.restart_interval == 0) {
                if (!entropy_consume_restart(&reader, expected_restart)) {
                    result.error = reader.error;
                    return result;
                }
                expected_restart = static_cast<u8>((expected_restart + 1) & 7);
                for (auto& value : predictor)
                    value = 0;
            }
            const u32 mcu_x = mcu % image.mcu_columns, mcu_y = mcu / image.mcu_columns;
            for (u32 scan = 0; scan < image.component_count; ++scan) {
                const u32 component_index = image.scan_order[scan];
                if (component_index >= image.component_count) {
                    result.error = JpegError::scan;
                    return result;
                }
                const JpegComponent& component = image.components[component_index];
                for (u32 vertical = 0; vertical < component.vertical_sampling; ++vertical)
                    for (u32 horizontal = 0; horizontal < component.horizontal_sampling; ++horizontal) {
                        if (!decode_block(&reader, dc_decode[component.dc_table],
                                          ac_decode[component.ac_table], &predictor[component_index], &block,
                                          &result.huffman_symbols)) {
                            result.error = reader.error == JpegError::none ? JpegError::coefficient : reader.error;
                            return result;
                        }
                        reconstruct_component_block(image, layout, planes, component_index,
                                                    mcu_x * component.horizontal_sampling + horizontal,
                                                    mcu_y * component.vertical_sampling + vertical, block);
                        ++result.decoded_blocks;
                    }
            }
            ++result.decoded_mcus;
        }
        if (!entropy_at_end(&reader)) {
            result.error = reader.error == JpegError::none ? JpegError::entropy : reader.error;
            return result;
        }
    }
    if (!convert_planes(image, layout, planes, row.data, sink, context)) {
        result.error = JpegError::sink;
        return result;
    }
    result.decoded_scans = image.scan_count;
    result.error = JpegError::none;
    return result;
}
} // namespace maicord
