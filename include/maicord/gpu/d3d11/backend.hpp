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

// Releases the reference and clears the handle; null is fine.
template <typename T> inline void gpu_release(T*& handle) noexcept {
    com_release(handle);
}

} // namespace maicord
