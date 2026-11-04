#pragma once

// The OpenGL ES GPU backend's vocabulary. Handles are opaque types the backend defines (slab entries holding a GL
// name and the metadata the D3D11 object carried); shared code only passes pointers to them.

#include "maicord/core/base.hpp"

namespace maicord {

// Zero or positive: success. Negative: failure, with the D3D11 error values where one maps.
using GpuStatus = i32;

struct GpuDevice;
struct GpuContext;
// ClearView and fence signalling live on GpuContext here.
using GpuContext1 = GpuContext;
using GpuContext4 = GpuContext;

struct GpuBuffer;
struct GpuTexture;
struct GpuSrv;
struct GpuUav;
struct GpuRtv;

struct GpuVertexShader;
struct GpuPixelShader;
struct GpuComputeShader;
struct GpuInputLayout;
struct GpuSampler;
struct GpuBlend;
struct GpuRaster;

struct GpuQuery;
struct GpuFence;

// Releases the handle and clears it; null is fine. Defined by the backend (src/gpu/gles/resources.cpp) for the
// resource handles; the device and context go with gpu_device_destroy, queries and fences come with their slice.
void gpu_release(GpuBuffer*& handle) noexcept;
void gpu_release(GpuTexture*& handle) noexcept;
void gpu_release(GpuSrv*& handle) noexcept;
void gpu_release(GpuUav*& handle) noexcept;
void gpu_release(GpuRtv*& handle) noexcept;
void gpu_release(GpuVertexShader*& handle) noexcept;
void gpu_release(GpuPixelShader*& handle) noexcept;
void gpu_release(GpuComputeShader*& handle) noexcept;
void gpu_release(GpuInputLayout*& handle) noexcept;
void gpu_release(GpuSampler*& handle) noexcept;
void gpu_release(GpuBlend*& handle) noexcept;
void gpu_release(GpuRaster*& handle) noexcept;

} // namespace maicord
