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
// The caller owns the storage (a zeroed struct), the backend the state behind it.
struct GpuSurface {
    struct GlesSurface* impl;
};

// What a window surface asks of the window service before each swap: a frame callback on the surface, which the window
// answers through gpu_surface_frame_callback (the frame clock). EGL commits the buffer, so the request has to be made
// first. One hook per process; null removes it, and a surface without one is paced by the caller's watchdog alone.
using GpuFrameRequest = void (*)(void* context) noexcept;
void gpu_surface_set_frame_request(GpuFrameRequest request, void* context) noexcept;

// On WSL Mesa's EGL finds no GPU through the render nodes and falls back to llvmpipe; the GPU is its d3d12 driver
// (GALLIUM_DRIVER=d3d12, through /dev/dxg). Call before the first gpu_device_create, on the main thread: when /dev/dxg
// exists and the environment chose no renderer (GALLIUM_DRIVER, MESA_LOADER_DRIVER_OVERRIDE, LIBGL_ALWAYS_SOFTWARE; or
// MAICORD_WSL_GPU=0), the process environment gets GALLIUM_DRIVER=d3d12, which the device creation takes back and retries
// without when d3d12 cannot make a context (or the window's display). Elsewhere it does nothing.
void gpu_device_prefer_wsl_gpu() noexcept;

// Frame timing, held by value, zeroed: a pair of GL timestamp queries (GL_EXT_disjoint_timer_query), so the create
// fails where caps.timer_query is false.
struct GpuTimer {
    GpuQuery* query;
};

// Releases the handle and clears it; null is fine. Defined by the backend (src/gpu/gles/resources.cpp, sync.cpp) for
// the resource handles; the device and context go with gpu_device_destroy, so releasing a context (the signalling
// interface, GpuContext4) only clears the handle.
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
void gpu_release(GpuContext*& handle) noexcept;
void gpu_release(GpuQuery*& handle) noexcept;
void gpu_release(GpuFence*& handle) noexcept;

// Takes another reference to a handle several owners share (a decoded frame's texture and views, the device a
// decoder keeps); each owner releases its own with gpu_release. The last release of a texture or view destroys it;
// the device goes with gpu_device_destroy, so releasing it only drops a reference.
void gpu_retain(GpuTexture* handle) noexcept;
void gpu_retain(GpuSrv* handle) noexcept;
void gpu_retain(GpuDevice* handle) noexcept;
void gpu_release(GpuDevice*& handle) noexcept;

} // namespace maicord
