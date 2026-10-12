#include "maicord/media/h264_parameters.hpp"
#include "h264_bits.hpp"

namespace maicord {

H264Error h264_select_parameters(ConstSlice<u8> avcc, u8 pps_id,
    H264Sps* sps, H264Pps* pps) noexcept {
    if (!sps || !pps || !avcc.data || avcc.count < 7) return H264Error::invalid_input;
    if (avcc.data[0] != 1 || (avcc.data[4] & 0xFCu) != 0xFCu ||
        (avcc.data[4] & 3u) == 2 || (avcc.data[5] & 0xE0u) != 0xE0u) return H264Error::malformed;
    const u32 sps_count = avcc.data[5] & 31u;
    H264NalCursor cursor{avcc, 6, 2};
    for (u32 i = 0; i < sps_count; ++i) {
        const auto nal = h264_next_nal(&cursor);
        if (nal.error != H264Error::none) return nal.error;
        if (nal.type != 7) return H264Error::malformed;
    }
    if (cursor.offset == avcc.count) return H264Error::truncated;
    const u32 pps_count = avcc.data[cursor.offset++];
    ConstSlice<u8> selected_pps{}, selected_sps{};
    u32 sps_id = 0;
    for (u32 i = 0; i < pps_count; ++i) {
        const auto nal = h264_next_nal(&cursor);
        if (nal.error != H264Error::none) return nal.error;
        if (nal.type != 8) return H264Error::malformed;
        H264Bits bits{{nal.bytes.data + 1, nal.bytes.count - 1}};
        const u32 id = bits.ue(255), sequence_id = bits.ue(31);
        if (bits.error != H264Error::none) return bits.error;
        if (id == pps_id) { selected_pps = nal.bytes; sps_id = sequence_id; }
    }
    if (!selected_pps.data) return H264Error::malformed;
    cursor.offset = 6;
    for (u32 i = 0; i < sps_count; ++i) {
        const auto nal = h264_next_nal(&cursor);
        if (nal.error != H264Error::none) return nal.error;
        H264Bits bits{{nal.bytes.data + 1, nal.bytes.count - 1}};
        bits.read(24); const u32 id = bits.ue(31);
        if (bits.error != H264Error::none) return bits.error;
        if (id == sps_id) selected_sps = nal.bytes;
    }
    if (!selected_sps.data) return H264Error::malformed;
    H264Sps a{}; H264Pps b{};
    H264Error error = h264_parse_sps(selected_sps, &a);
    if (error == H264Error::none) error = h264_parse_pps(selected_pps, a, &b);
    if (error == H264Error::none) { *sps = a; *pps = b; }
    return error;
}

} // namespace maicord
