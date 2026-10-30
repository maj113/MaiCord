#include "maicord/media/h264_slice.hpp"
#include "maicord/core/memory.hpp"
#include "h264_bits.hpp"
// Header parsing, once per picture and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {
H264Error repeated_parameter(const H264Nal& nal, ConstSlice<u8> avcc, const H264Sps& sps,
                             const H264Pps& pps) noexcept {
    // Parse before comparison: malformed/truncated repeats are not silently
    // treated as an unsupported configuration change.
    if (nal.type == 7) {
        H264Sps candidate{};
        const auto error = h264_parse_sps(nal.bytes, &candidate);
        if (error != H264Error::none)
            return error;
        if (candidate.id != sps.id)
            return H264Error::unsupported;
    } else {
        H264Pps candidate{};
        const auto error = h264_parse_pps(nal.bytes, sps, &candidate);
        if (error != H264Error::none)
            return error;
        if (candidate.id != pps.id)
            return H264Error::unsupported;
    }
    if (!avcc.data || avcc.count < 7)
        return H264Error::unsupported;
    H264NalCursor cursor{avcc, 6, 2};
    ConstSlice<u8> active{};
    const u32 sequences = avcc[5] & 31u;
    for (u32 group = 0; group < 2; ++group) {
        if (group && cursor.offset >= avcc.count)
            return H264Error::truncated;
        const u32 count = group ? avcc[cursor.offset++] : sequences;
        for (u32 i = 0; i < count; ++i) {
            const auto declared = h264_next_nal(&cursor);
            if (declared.error != H264Error::none)
                return declared.error;
            if (declared.type != (group ? 8 : 7))
                return H264Error::malformed;
            if (declared.type == nal.type) {
                H264Bits bits{{declared.bytes.data + 1, declared.bytes.count - 1}};
                if (nal.type == 7)
                    bits.read(24);
                const u32 id = bits.ue(nal.type == 7 ? 31 : 255);
                if (bits.error != H264Error::none)
                    return bits.error;
                if (id == (nal.type == 7 ? sps.id : pps.id))
                    active = declared.bytes;
            }
        }
    }
    return active.count == nal.bytes.count && active.data &&
                   bytes_equal(active.data, nal.bytes.data, nal.bytes.count)
               ? H264Error::none
               : H264Error::unsupported;
}
} // namespace
H264Error h264_parse_picture(ConstSlice<u8> bytes, u8 length_bytes, const H264Sps& sps, const H264Pps& pps,
                             H264Picture* output, ConstSlice<u8> avcc) noexcept {
    if (!output)
        return H264Error::invalid_input;
    H264Picture picture{};
    picture.all_intra = true;
    H264NalCursor cursor{bytes, 0, length_bytes};
    u32 last_mb = 0;
    bool sequence_ended = false, stream_ended = false;
    for (;;) {
        const auto nal = h264_next_nal(&cursor);
        if (nal.error == H264Error::end)
            break;
        if (nal.error != H264Error::none)
            return nal.error;
        if (nal.type == 10 || nal.type == 11) {
            // These RBSPs are empty (H.264 7.3.2.5/7.3.2.6): the NAL
            // consists solely of its non-reference header. They terminate
            // this picture, not the MP4 sample table or its following IDR.
            if (!picture.slice_count || nal.reference || nal.bytes.count != 1 || stream_ended ||
                (nal.type == 10 && sequence_ended))
                return H264Error::malformed;
            sequence_ended = true;
            stream_ended = nal.type == 11;
            continue;
        }
        if (sequence_ended)
            return H264Error::malformed;
        if (nal.type == 6 || nal.type == 9 || nal.type == 12)
            continue;
        if (nal.type == 7 || nal.type == 8) {
            const auto error = repeated_parameter(nal, avcc, sps, pps);
            if (error != H264Error::none)
                return error;
            continue;
        }
        H264Slice slice{};
        const auto error = h264_parse_slice(nal.bytes, sps, pps, &slice);
        if (error != H264Error::none)
            return error;
        if (!picture.slice_count) {
            if (slice.first_mb)
                return H264Error::unsupported;
            picture.first = slice;
        } else if (slice.first_mb <= last_mb || !h264_same_picture(picture.first, slice))
            return H264Error::unsupported;
        if (nal.bytes.count > ~0u - 3u - picture.bitstream_bytes)
            return H264Error::malformed;
        picture.bitstream_bytes += nal.bytes.count + 3u;
        picture.all_intra &= slice.type == H264SliceType::i;
        ++picture.slice_count;
        last_mb = slice.first_mb;
    }
    if (!picture.slice_count)
        return H264Error::malformed;
    *output = picture;
    return H264Error::none;
}
} // namespace maicord
#pragma clang attribute pop
