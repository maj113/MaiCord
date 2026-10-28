#pragma once

#include "maicord/core/base.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {

struct GpuVideoFrame;
struct H264OutputQueue;
struct Mp4DecodedFrame;
struct VideoDisplayExtent;

inline constexpr u32 kGpuDecoderSurfaceCapacity = 32;

// Shader-readable NV12 decoder output array for the attachment and live
// stream decoders: every slice has R8/R8G8 single-slice plane views, so the
// renderer converts a decoded picture in place.
struct GpuDecoderSurfaces {
    ID3D11Texture2D* texture;
    ID3D11VideoDecoderOutputView* views[kGpuDecoderSurfaceCapacity];
    ID3D11ShaderResourceView* luma[kGpuDecoderSurfaceCapacity];
    ID3D11ShaderResourceView* chroma[kGpuDecoderSurfaceCapacity];
    u8 count;
};

// Hardware decode requires shader-readable NV12 decoder output. Without it the
// decoders fail with DXGI_ERROR_UNSUPPORTED, as on a GPU without the codec.
bool gpu_decoder_surfaces_supported(ID3D11Device* device) noexcept;
// output_views: decoder output views to create (all slices for a stream, one
// for a single picture); plane views cover every slice. A refused array is
// DXGI_ERROR_UNSUPPORTED.
HRESULT gpu_decoder_surfaces_create(GpuDecoderSurfaces* surfaces, ID3D11Device* device,
                                    ID3D11VideoDevice* video, const GUID& profile, u32 width, u32 height,
                                    u32 count, u32 output_views) noexcept;
void gpu_decoder_surfaces_release(GpuDecoderSurfaces* surfaces) noexcept;
// First decoder configuration for description that uses bitstream_raw (H.264 short slices 2, HEVC and AV1 1)
// without encryption; DXGI_ERROR_UNSUPPORTED when the driver offers none.
HRESULT gpu_decoder_select_config(ID3D11VideoDevice* video, const D3D11_VIDEO_DECODER_DESC& description,
                                  u8 bitstream_raw, D3D11_VIDEO_DECODER_CONFIG* selected) noexcept;
// Copies bytes into the driver's mapped decoder buffer of the given type and describes it for
// SubmitDecoderBuffers; E_OUTOFMEMORY when the driver's buffer is smaller.
HRESULT gpu_decoder_upload_buffer(ID3D11VideoContext* context, ID3D11VideoDecoder* decoder,
                                  D3D11_VIDEO_DECODER_BUFFER_TYPE type, const void* data, u32 bytes,
                                  D3D11_VIDEO_DECODER_BUFFER_DESC* description) noexcept;
// The next decoded picture in display order, leased until the caller's next read: points *frame
// (the decoder's GpuVideoFrame) at its surface and reports the sizes and times; Mp4DecodeError::sample
// when the queue is empty.
Mp4DecodedFrame gpu_decoder_output_frame(H264OutputQueue* output, GpuVideoFrame* frame,
                                         const GpuDecoderSurfaces& surfaces,
                                         const VideoDisplayExtent& display) noexcept;

} // namespace maicord
