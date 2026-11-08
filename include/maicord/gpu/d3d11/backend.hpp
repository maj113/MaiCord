#pragma once

// The Direct3D 11 GPU backend's vocabulary: every Gpu* handle is the COM interface it stands for, so a struct
// holding handles keeps its size and layout and a call through one is the D3D11 call itself. Only the interface
// names are declared here; code that calls a method includes maicord/sys/d3d11.hpp.

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

struct ID3D11BlendState;
struct ID3D11Buffer;
struct ID3D11ComputeShader;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct ID3D11DeviceContext4;
struct ID3D11Fence;
struct ID3D11InputLayout;
struct ID3D11PixelShader;
struct ID3D11Query;
struct ID3D11RasterizerState;
struct ID3D11RenderTargetView;
struct ID3D11SamplerState;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11UnorderedAccessView;
struct ID3D11VertexShader;
struct IDCompositionDevice;
struct IDCompositionTarget;
struct IDCompositionVisual;
struct IDXGISwapChain1;

namespace maicord {

// An HRESULT: the same width and S_OK/E_* values, so result structs and log values keep their layout.
using GpuStatus = HRESULT;

using GpuDevice = ID3D11Device;
using GpuContext = ID3D11DeviceContext;
// The context interfaces that add ClearView (rectangle clears) and fence signalling. Other backends have one
// context type, so these name GpuContext there.
using GpuContext1 = ID3D11DeviceContext1;
using GpuContext4 = ID3D11DeviceContext4;

using GpuBuffer = ID3D11Buffer;
using GpuTexture = ID3D11Texture2D;
using GpuSrv = ID3D11ShaderResourceView;
using GpuUav = ID3D11UnorderedAccessView;
using GpuRtv = ID3D11RenderTargetView;

using GpuVertexShader = ID3D11VertexShader;
using GpuPixelShader = ID3D11PixelShader;
using GpuComputeShader = ID3D11ComputeShader;
using GpuInputLayout = ID3D11InputLayout;
using GpuSampler = ID3D11SamplerState;
using GpuBlend = ID3D11BlendState;
using GpuRaster = ID3D11RasterizerState;
using GpuQuery = ID3D11Query;
using GpuFence = ID3D11Fence;

// A swap chain shown through one DirectComposition visual. The renderer's owner holds it by value (the caller owns
// the storage; gpu_surface_create fills it, gpu_surface_release zeroes it) and reads it only through gpu_surface_*.
struct GpuSurface {
    IDXGISwapChain1* swap_chain;
    IDCompositionDevice* composition_device;
    IDCompositionTarget* composition_target;
    IDCompositionVisual* composition_visual;
    GpuRtv* target; // the draw target: the back buffer's view, or what gpu_surface_set_target pointed it at
    HANDLE frame_latency_waitable;
    u32 buffer_width;
    u32 buffer_height;
    u32 resize_buffer_reallocations;
};

// Frame timing: a disjoint query with two timestamps. One frame is bracketed by gpu_timer_begin and gpu_timer_end.
struct GpuTimer {
    GpuQuery* disjoint;
    GpuQuery* begin;
    GpuQuery* end;
};

inline constexpr u32 kGpuPagedTextureMaximumPages = 8;
// Rows a paged texture backs at a time: one row of 64 KiB tiles (128 x 128 texels of 4 bytes, 256 x 256 of 1 byte).
constexpr u16 gpu_paged_commit_rows(u32 texel_bytes) noexcept {
    return texel_bytes == 1 ? 256 : 128;
}
// Every page samples through the one tiled texture: draws never split per page.
inline constexpr bool kGpuPagedBindings = false;

// A tiled texture (D3D11_RESOURCE_MISC_TILED) with its views and the tile pool backing it: each tile band of the
// texture maps one band of pool tiles (band_slot holds that band + 1, 0 for none) or nothing. The pool grows by
// ResizeTilePool, goes when nothing is backed, and otherwise shrinks to its last band in use once the GPU has passed
// the event `unmapped`, ended after an unmap (`shrinking` while it is pending, `unmapped_again` when another unmap
// came since); a band takes the lowest free one.
struct GpuPagedTexture {
    GpuTexture* texture;
    GpuSrv* view;
    GpuUav* uav;
    GpuBuffer* pool;
    GpuQuery* unmapped;
    u32 page_rows;
    u32 band_rows;
    u32 tiles_across;
    u32 pool_bands;
    u32 pages;
    u16 committed[kGpuPagedTextureMaximumPages];
    u8 band_slot[128];
    bool shrinking;
    bool unmapped_again;
};

// Releases the reference and clears the handle; null is fine.
template <typename T> inline void gpu_release(T*& handle) noexcept {
    com_release(handle);
}

// Takes another reference to a handle that several owners share (a decoded frame's texture and views, the device a
// decoder keeps); each owner releases its own with gpu_release. Other backends make the handles they share
// retainable the same way.
template <typename T> inline void gpu_retain(T* handle) noexcept {
    handle->AddRef();
}

} // namespace maicord
