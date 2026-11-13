#pragma once

namespace maicord::gles {

// The hooks of gpu_retain and gpu_release (resources.cpp, sync.cpp) for the handles of the decoded-picture planes
// (video_planes.cpp), which live in their own pool, count atomically and touch no GL state. Each returns true when
// `object` (a GpuTexture* or GpuSrv*) is one of them and has been handled.
bool video_plane_retain(const void* object) noexcept;
bool video_plane_release(const void* object) noexcept;

// Renderer thread: creates a plane's GL texture and uploads the pixels the decoder published since the last call.
// False when the plane cannot be made current (no dma-buf support, a GL error).
bool video_plane_realize(const void* object) noexcept;

// gpu_device_destroy: frees every plane (the EGLImages need the display) and forgets what was resolved.
void video_planes_shutdown() noexcept;

} // namespace maicord::gles
