#include "h264_gpu_configuration.hpp"

namespace maicord {
H264Error h264_gpu_configuration_from_demux(const Mp4Demuxer& input, H264GpuPicture* out,
                                            Mp4DemuxError* demux_error) noexcept {
    if (demux_error)
        *demux_error = Mp4DemuxError::none;
    if (!out)
        return H264Error::invalid_input;
    Mp4Demuxer demux = input;
    const auto unit = mp4_demux_next_ready(&demux);
    if (!unit.ok()) {
        if (demux_error)
            *demux_error = unit.error;
        return H264Error::malformed;
    }
    return h264_gpu_configuration_from_unit(unit, mp4_demux_avcc(demux), demux.nal_length_bytes,
                                            demux.width, demux.height, out);
}
H264Error h264_gpu_configuration_from_unit(const Mp4AccessUnit& unit, ConstSlice<u8> avcc,
    u8 length_bytes, u32 width, u32 height, H264GpuPicture* out) noexcept {
    if (!out || !unit.ok() || !unit.bytes.data || !unit.bytes.count || !avcc.data || !avcc.count)
        return H264Error::invalid_input;
    H264GpuPicture picture{};
    picture.unit = unit;
    picture.length_bytes = length_bytes;
    H264NalCursor cursor{picture.unit.bytes, 0, picture.length_bytes};
    for (;;) {
        const auto nal = h264_next_nal(&cursor);
        if (nal.error != H264Error::none)
            return nal.error;
        if (nal.type == 6 || nal.type == 9 || nal.type == 12 || nal.type == 7 || nal.type == 8)
            continue;
        u8 id = 0;
        auto error = h264_slice_parameter_id(nal.bytes, &id);
        if (error == H264Error::none)
            error = h264_select_parameters(avcc, id, &picture.sps, &picture.pps);
        if (error == H264Error::none)
            error = h264_parse_picture(picture.unit.bytes, picture.length_bytes, picture.sps, picture.pps,
                                       &picture.header, avcc);
        if (error != H264Error::none)
            return error;
        break;
    }
    if (!picture.header.first.idr || picture.sps.width != width || picture.sps.height != height)
        return H264Error::unsupported;
    *out = picture;
    return H264Error::none;
}
} // namespace maicord
