#include "h264_gpu_configuration.hpp"

namespace maicord {
H264Error h264_gpu_configuration_from_demux(const Mp4Demuxer& input, H264GpuPicture* out,
                                            Mp4DemuxError* demux_error) noexcept {
    if (demux_error)
        *demux_error = Mp4DemuxError::none;
    if (!out)
        return H264Error::invalid_input;
    Mp4Demuxer demux = input;
    H264GpuPicture picture{};
    picture.unit = mp4_demux_next_ready(&demux);
    if (!picture.unit.ok()) {
        if (demux_error)
            *demux_error = picture.unit.error;
        return H264Error::malformed;
    }
    picture.length_bytes = demux.nal_length_bytes;
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
            error = h264_select_parameters(mp4_demux_avcc(demux), id, &picture.sps, &picture.pps);
        if (error == H264Error::none)
            error = h264_parse_picture(picture.unit.bytes, picture.length_bytes, picture.sps, picture.pps,
                                       &picture.header, mp4_demux_avcc(demux));
        if (error != H264Error::none)
            return error;
        break;
    }
    if (!picture.header.first.idr || picture.sps.width != demux.width || picture.sps.height != demux.height)
        return H264Error::unsupported;
    *out = picture;
    return H264Error::none;
}
} // namespace maicord
