#pragma once

#include "maicord/platform/mp4_decoder.hpp"

#include <Windows.h>

struct IMFSourceReader;
struct IMFSample;
struct IMFTransform;

namespace maicord {
// Benchmark-only Source Reader baseline. Native GPU NV12, no video processor,
// software RGB conversion, pixel readback or hidden upload fallback.
struct Mp4MfGpuReference {
    IMFSourceReader* reader;
    IMFSample* sample;
    GpuVideoFrame frame;
    HRESULT status;
};
bool mp4_mf_gpu_initialize(Mp4MfGpuReference* reference, ConstSlice<u8> encoded,
                           ID3D11Device* device) noexcept;
Mp4DecodedFrame mp4_mf_gpu_read(Mp4MfGpuReference* reference, bool loop = false) noexcept;
void mp4_mf_gpu_release(Mp4MfGpuReference* reference) noexcept;
// Diagnostic-only; caller releases the returned reference.
IMFTransform* mp4_mf_gpu_transform(Mp4MfGpuReference* reference) noexcept;
} // namespace maicord
