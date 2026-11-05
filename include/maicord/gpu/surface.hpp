#pragma once

// The presentation half of the GPU interface (included by api.hpp): a surface is what the renderer draws a frame
// into and what puts it on screen. On Direct3D 11 it is the swap chain with its composition visual; on OpenGL ES an
// EGL window surface fed by an offscreen frame texture, so the renderer's picture keeps D3D11's orientation and its
// damage and previous-buffer logic stay the same on both (the present flips the picture when it blits).

#include "maicord/gpu/api.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/window/event.hpp"

namespace maicord {

struct GpuSurfaceDesc {
    NativeWindow window; // the window's native handle: an HWND, or the wl_surface* of the Wayland backend
    u32 width, height;   // the buffer size in pixels
    u32 dpi;
};

// One surface per device. On OpenGL ES the device must have been made with gpu_device_set_native_display. The caller
// owns the storage: `surface` is a zeroed GpuSurface that this fills and gpu_surface_release releases and zeroes.
GpuStatus gpu_surface_create(GpuDevice* device, const GpuSurfaceDesc& desc, GpuSurface* surface);
// A new buffer size in pixels (the window resized or its scale changed). The frame target is recreated, so the
// next frame must draw everything.
GpuStatus gpu_surface_resize(GpuSurface* surface, u32 width, u32 height);
// What the renderer draws into, valid until the next resize: the frame target unless gpu_surface_set_target moved it.
MAICORD_GPU_INLINE GpuRtv* gpu_surface_target(const GpuSurface* surface);
// The texture behind gpu_surface_target (for reading the frame back), not counted.
MAICORD_GPU_INLINE GpuTexture* gpu_surface_target_texture(const GpuSurface* surface);
// Points the draw target at an offscreen target for a frame drawn off to the side; the caller restores the frame
// target by passing back the gpu_surface_target it saved. Presentation is unaffected: a present shows the surface's own frame.
MAICORD_GPU_INLINE void gpu_surface_set_target(GpuSurface* surface, GpuRtv* target);
// The buffer size in pixels as last created or resized (the renderer may hold a larger buffer than the window while
// the window is dragged), and whether the surface was created (a released one is not).
MAICORD_GPU_INLINE u32 gpu_surface_width(const GpuSurface* surface);
MAICORD_GPU_INLINE u32 gpu_surface_height(const GpuSurface* surface);
// The texel format of the surface's frame target (offscreen targets drawn in its place use the same). It is the
// backend's, the same for every surface: a null surface asks it before any surface exists (the pipeline warm-up).
MAICORD_GPU_INLINE GpuFormat gpu_surface_format(const GpuSurface* surface);
MAICORD_GPU_INLINE bool gpu_surface_valid(const GpuSurface* surface);
// Presents the frame. `dirty` lists the rectangles (top-left origin, pixels) that changed since the previous
// present; none (null or zero) means the whole frame. sync_interval is the swap interval on Direct3D; OpenGL ES
// never blocks in the swap (the frame clock paces).
GpuStatus gpu_surface_present(GpuSurface* surface, u32 sync_interval, const GpuRect* dirty, u32 dirty_count);
// Moves the whole picture by (x, y) physical pixels without a new frame (the app's shake). *needs_present is true
// where the move only shows after a present (OpenGL ES); Direct3D moves its visual at once.
GpuStatus gpu_surface_set_offset(GpuSurface* surface, float x, float y, bool* needs_present);
void gpu_surface_release(GpuSurface* surface);

// Frame clock: the surface says when the next frame may start. The waitable is auto-reset and signaled while a frame
// permit is available (one at creation). Direct3D: the swap chain's frame-latency waitable. OpenGL ES: the
// compositor's frame callback, delivered through gpu_surface_frame_callback.
MAICORD_GPU_INLINE Waitable gpu_frame_clock_waitable(const GpuSurface* surface);
// Takes the permit without blocking.
bool gpu_frame_try_acquire(GpuSurface* surface);
// Waits up to timeout_ms for the permit and takes it.
bool gpu_frame_wait(GpuSurface* surface, u32 timeout_ms);
// The window service forwards the compositor's frame callback here (window_set_frame_listener): any thread. A
// no-op on Direct3D, whose swap chain signals its own waitable.
void gpu_surface_frame_callback(GpuSurface* surface);

} // namespace maicord

#if defined(_WIN32)
#include "maicord/gpu/d3d11/api.inl"
#endif
