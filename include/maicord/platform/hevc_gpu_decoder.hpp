#pragma once
#include "maicord/platform/h264_gpu_decoder.hpp"
namespace maicord {
// Shared small opaque handle/error fields; state belongs exclusively to HEVC.
bool hevc_gpu_decoder_initialize(H264GpuDecoder*, ConstSlice<u8>, ID3D11Device*, ID3D11DeviceContext*,
                                 bool full_stream = true, Mp4SourceAvailability availability = {}) noexcept;
Mp4DecodedFrame hevc_gpu_decoder_read(H264GpuDecoder*, bool loop_at_end = false,
                                      u32 max_submissions = 0) noexcept;
void hevc_gpu_decoder_release(H264GpuDecoder*) noexcept;
bool hevc_gpu_decoder_acknowledge_frame(H264GpuDecoder*) noexcept;
Mp4DemuxError hevc_gpu_decoder_seek(H264GpuDecoder*, i64, Mp4SeekPoint*) noexcept;
bool hevc_gpu_decoder_seek_start(H264GpuDecoder*) noexcept;
H264GpuMovie hevc_gpu_decoder_movie(const H264GpuDecoder&) noexcept;
ConstSlice<u8> hevc_gpu_decoder_source(const H264GpuDecoder&) noexcept;
bool hevc_gpu_decoder_wait(H264GpuDecoder*, u32 timeout_ms) noexcept;
} // namespace maicord
