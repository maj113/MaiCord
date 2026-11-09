#include "access_unit.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/media/video/av1.hpp"

namespace maicord {
namespace {

// One NAL unit of an Annex B stream at *offset (its bytes without the start code and trailing zeros); advances past it.
bool next_nal(ConstSlice<u8> data, u32* offset, ConstSlice<u8>* nal) noexcept {
    u32 i = *offset;
    // The start code: two or more zeros and a one.
    while (i + 2 < data.count && !(data.data[i] == 0 && data.data[i + 1] == 0 && data.data[i + 2] == 1))
        ++i;
    if (i + 2 >= data.count)
        return false;
    const u32 begin = i + 3;
    u32 end = begin;
    while (end + 2 < data.count && !(data.data[end] == 0 && data.data[end + 1] == 0 && data.data[end + 2] <= 1))
        ++end;
    if (end + 2 >= data.count)
        end = data.count;
    *offset = end;
    while (end > begin && data.data[end - 1] == 0)
        --end;
    *nal = {data.data + begin, end - begin};
    return true;
}

u8 nal_type(GpuStreamCodec codec, ConstSlice<u8> nal) noexcept {
    return codec == GpuStreamCodec::h264 ? nal.data[0] & 0x1Fu : (nal.data[0] >> 1) & 0x3Fu;
}
bool is_delimiter(GpuStreamCodec codec, u8 type) noexcept {
    return type == (codec == GpuStreamCodec::h264 ? 9 : 35);
}
bool is_sequence_parameters(GpuStreamCodec codec, u8 type) noexcept {
    return type == (codec == GpuStreamCodec::h264 ? 7 : 33);
}

bool append(EncodedUnitBuffer* output, const void* data, u32 bytes) noexcept {
    if (output->count + u64{bytes} > 0xFFFFFFFFu || !encoded_unit_reserve(output, output->count + bytes))
        return false;
    copy_bytes(output->data + output->count, data, bytes);
    output->count += bytes;
    return true;
}

// Every NAL unit of `input` but delimiters, with 4-byte start codes.
bool append_nals(GpuStreamCodec codec, ConstSlice<u8> input, EncodedUnitBuffer* output) noexcept {
    constexpr u8 kStartCode[4]{0, 0, 0, 1};
    u32 offset = 0;
    ConstSlice<u8> nal{};
    while (next_nal(input, &offset, &nal))
        if (nal.count && !is_delimiter(codec, nal_type(codec, nal)) &&
            !(append(output, kStartCode, 4) && append(output, nal.data, nal.count)))
            return false;
    return true;
}

bool has_sequence_parameters(GpuStreamCodec codec, ConstSlice<u8> input) noexcept {
    u32 offset = 0;
    ConstSlice<u8> nal{};
    while (next_nal(input, &offset, &nal))
        if (nal.count && is_sequence_parameters(codec, nal_type(codec, nal)))
            return true;
    return false;
}

constexpr u8 kAv1ObuPadding = 15;

bool append_obu(const Av1Obu& obu, EncodedUnitBuffer* output) noexcept {
    u8 header[2 + 8];
    u32 bytes = 0;
    header[bytes++] = static_cast<u8>(obu.type << 3 | (obu.extension ? 0x04 : 0) | 0x02);
    if (obu.extension)
        header[bytes++] = static_cast<u8>(obu.temporal_id << 5 | obu.spatial_id << 3);
    u32 size = obu.payload.count;
    do {
        header[bytes++] = static_cast<u8>((size & 0x7F) | (size > 0x7F ? 0x80 : 0));
        size >>= 7;
    } while (size);
    return append(output, header, bytes) && append(output, obu.payload.data, obu.payload.count);
}

} // namespace

bool encoded_unit_reserve(EncodedUnitBuffer* buffer, u32 bytes) noexcept {
    if (buffer->capacity >= bytes)
        return true;
    const u64 capacity = (u64{bytes} + 0xFFFFu) & ~u64{0xFFFFu};
    if (capacity > 0xFFFFFFFFu)
        return false;
    auto* data = commit_pages<u8>(capacity);
    if (!data)
        return false;
    if (buffer->data) {
        copy_bytes(data, buffer->data, buffer->count);
        pages_release(buffer->data, buffer->capacity);
    }
    buffer->data = data;
    buffer->capacity = static_cast<u32>(capacity);
    return true;
}

void encoded_unit_release(EncodedUnitBuffer* buffer) noexcept {
    if (buffer->data)
        pages_release(buffer->data, buffer->capacity);
    reset_value(buffer);
}

bool encoded_annexb_unit(GpuStreamCodec codec, ConstSlice<u8> input, ConstSlice<u8> parameter_sets, bool keyframe,
                         EncodedUnitBuffer* output) noexcept {
    output->count = 0;
    if (keyframe && !has_sequence_parameters(codec, input) && !append_nals(codec, parameter_sets, output))
        return false;
    return append_nals(codec, input, output) && output->count;
}

bool encoded_av1_unit(ConstSlice<u8> input, ConstSlice<u8> sequence_header, bool keyframe, EncodedUnitBuffer* output,
                      ConstSlice<u8>* found) noexcept {
    output->count = 0;
    *found = {};
    bool has_sequence_header = false;
    u32 offset = 0;
    Av1Obu obu{};
    while (offset < input.count) {
        if (av1_next_obu(input, &offset, &obu) != Av1Status::ok)
            return false;
        has_sequence_header = has_sequence_header || obu.type == kAv1ObuSequenceHeader;
    }
    // Where the output's sequence header lies; resolved at the end, after the buffer stopped growing.
    u32 found_start = 0, found_end = 0;
    if (keyframe && !has_sequence_header && sequence_header.count) {
        u32 at = 0;
        if (av1_next_obu(sequence_header, &at, &obu) != Av1Status::ok || obu.type != kAv1ObuSequenceHeader ||
            !append_obu(obu, output))
            return false;
        found_end = output->count;
    }
    for (offset = 0; offset < input.count;) {
        av1_next_obu(input, &offset, &obu);
        if (obu.type == kAv1ObuTemporalDelimiter || obu.type == kAv1ObuPadding)
            continue;
        const u32 start = output->count;
        if (!append_obu(obu, output))
            return false;
        if (obu.type == kAv1ObuSequenceHeader) {
            found_start = start;
            found_end = output->count;
        }
    }
    if (found_end)
        *found = {output->data + found_start, found_end - found_start};
    return output->count != 0;
}
} // namespace maicord
