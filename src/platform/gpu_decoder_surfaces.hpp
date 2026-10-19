#pragma once

#include "maicord/core/base.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {

inline constexpr u32 kGpuDecoderSurfaceCapacity = 32;

// Shader-readable NV12 decoder output array for the H.264 and HEVC attachment
// decoders: every slice has R8/R8G8 single-slice plane views, so the renderer
// converts a decoded picture in place.
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

} // namespace maicord
