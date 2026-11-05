// platform/decode/mp4_decoder.hpp on Linux: no MP4 file decoder yet. The Windows one drives DXVA through
// h264_gpu_decoder.cpp; the Linux video stack (VA-API and the software tier, platform/decode/linux) decodes live
// streams only. Every initialization fails, so the application treats an attachment video the way it treats one the
// system cannot decode (a failed media request, never a crash), and the rest of the interface answers for a decoder
// that never became ready.
#include "maicord/platform/decode/mp4_decoder.hpp"

namespace maicord {

bool mp4_decoder_initialize(Mp4Decoder*, ConstSlice<u8>, u16, u16, GpuDevice*, Mp4SourceAvailability) noexcept {
    return false;
}

bool mp4_decoder_initialize_fragmented(Mp4Decoder*, Mp4FragmentSource*, GpuDevice*) noexcept {
    return false;
}

ConstSlice<u8> mp4_decoder_source(const Mp4Decoder&) noexcept {
    return {};
}

H264GpuMovie mp4_decoder_movie(const Mp4Decoder&) noexcept {
    return {};
}

bool mp4_decoder_busy(const Mp4Decoder&) noexcept {
    return false;
}

bool mp4_decoder_wait(Mp4Decoder*, u32) noexcept {
    return true;
}

bool mp4_decoder_acknowledge_frame(Mp4Decoder*) noexcept {
    return false;
}

Mp4DemuxError mp4_decoder_seek(Mp4Decoder*, i64, Mp4SeekPoint*) noexcept {
    return Mp4DemuxError::invalid_input;
}

bool mp4_decoder_seek_start(Mp4Decoder*) noexcept {
    return false;
}

Mp4DecodedFrame mp4_decoder_read_selected(Mp4Decoder*) noexcept {
    Mp4DecodedFrame frame{};
    frame.error = Mp4DecodeError::runtime;
    return frame;
}

Mp4DecodedFrame mp4_decoder_read(Mp4Decoder*, bool) noexcept {
    Mp4DecodedFrame frame{};
    frame.error = Mp4DecodeError::runtime;
    return frame;
}

void mp4_decoder_release(Mp4Decoder* decoder) noexcept {
    if (decoder)
        *decoder = {};
}

} // namespace maicord
