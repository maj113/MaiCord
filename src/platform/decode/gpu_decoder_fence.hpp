#pragma once

#include "maicord/core/base.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {

// The fence the H.264 and HEVC GPU decoders wait on for submitted decode
// work. Every member is created on the first wait and freed by release.
struct GpuDecoderFence {
    ID3D11DeviceContext4* context;
    ID3D11Fence* fence;
    HANDLE wake;
    u64 signal;
};

// Signals the fence on the decoder's immediate context (entered through
// guard) and waits up to timeout_ms for it to complete.
HRESULT gpu_decoder_fence_wait(GpuDecoderFence* fence, ID3D11VideoContext* context, ID3D11Multithread* guard,
                               u32 timeout_ms) noexcept;
void gpu_decoder_fence_release(GpuDecoderFence* fence) noexcept;

} // namespace maicord
