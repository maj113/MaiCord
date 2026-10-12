#pragma once

#include "maicord/media/h264_slice.hpp"
#include "maicord/media/mp4_demux.hpp"

namespace maicord {

// Selected configuration and current access unit for the native GPU decoder.
struct H264GpuPicture {
    H264Sps sps;
    H264Pps pps;
    Mp4AccessUnit unit;
    H264Picture header;
    u8 length_bytes;
};
// Inspect an initialized demuxer through a copy. Preserve the playback cursor.
H264Error h264_gpu_configuration_from_demux(const Mp4Demuxer& demux, H264GpuPicture* output,
                                            Mp4DemuxError* demux_error = nullptr) noexcept;

} // namespace maicord
