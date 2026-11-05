#pragma once

// Decoded video pictures on OpenGL ES (Linux): the planes the decoders hand to the renderer.
//
// A picture is two planes, luma (one channel) and chroma (two interleaved channels, half the extent), that the video
// conversion samples. The decoders run on their own threads and must not touch GL (all GL calls belong to the
// renderer's thread), yet the frame they publish reaches the app as GpuTexture and GpuSrv handles, retained and
// released from any thread. So a plane is made without GL, from a dma-buf the hardware decoder exported or from CPU
// memory the software decoder wrote, and its GL texture is created, filled and deleted by the renderer's thread
// (video_frame_gpu_present realizes the planes it converts; gpu_video_planes_reap deletes the retired ones).
// gpu_retain and gpu_release on these handles are atomic and touch no GL state.

#include "maicord/gpu/gpu.hpp"

namespace maicord {

// One plane of a dma-buf: fd stays with the caller (the image holds its own reference), fourcc is the DRM format of
// the plane (R8 and GR88 for NV12, R16 and GR1616 for P010). A modifier with compression metadata (Intel's media
// compression) puts the metadata in a second plane of the same image: aux_pitch is non-zero then and aux_fd,
// aux_offset and aux_pitch place it.
inline constexpr u64 kDrmFormatModifierLinear = 0;
inline constexpr u64 kDrmFormatModifierInvalid = 0x00ffffffffffffffull;
inline constexpr u32 kDrmFormatR8 = 0x20203852;     // 'R8  '
inline constexpr u32 kDrmFormatGr88 = 0x38385247;   // 'GR88'
inline constexpr u32 kDrmFormatR16 = 0x20363152;    // 'R16 '
inline constexpr u32 kDrmFormatGr1616 = 0x32335247; // 'GR32'
struct GpuDmabufPlane {
    i32 fd;
    u32 fourcc;
    u32 width, height;
    u32 offset, pitch;
    u64 modifier; // kDrmFormatModifierInvalid when the exporter gave none
    i32 aux_fd;
    u32 aux_offset, aux_pitch;
};

// Samples of a CPU plane: one or two channels of 8 bits.
enum class GpuVideoPlaneFormat : u8 { r8, rg8 };

// EGL attribute list (terminated by EGL_NONE) that imports `plane` as an EGLImage; `modifiers` is whether the display
// has EGL_EXT_image_dma_buf_import_modifiers. Returns the entry count including the terminator; the list needs 28.
u32 gpu_dmabuf_attributes(const GpuDmabufPlane& plane, bool modifiers, i32* attributes) noexcept;

// Whether the device can import `plane` and sample it as a 2D texture: a linear or unspecified modifier always (the
// driver decides the layout of an unspecified one), any other only when EGL_EXT_image_dma_buf_import_modifiers lists
// it for the plane's format as usable without an external sampler. Refusing here, before the import, is what keeps a
// tiled or compressed picture from being read as a linear one. No GL; any thread.
bool gpu_dmabuf_plane_supported(GpuDevice* device, const GpuDmabufPlane& plane);

// The pool of planes holds this many at once (a decoder keeps two per surface).
inline constexpr u32 kGpuVideoPlaneCapacity = 1024;

// Any thread, no GL. Each output handle carries one reference. kGpuUnsupported without dma-buf import on the device.
GpuStatus gpu_video_plane_import(GpuDevice* device, const GpuDmabufPlane& plane, GpuTexture** texture, GpuSrv** view);
// A plane over memory the decoder writes: `pixels` is width * height texels of the format (rows `pitch` bytes apart,
// zeroed), valid while a reference exists. gpu_video_plane_publish makes what was written the plane's content.
GpuStatus gpu_video_plane_create(GpuDevice* device, GpuVideoPlaneFormat format, u32 width, u32 height,
                                 GpuTexture** texture, GpuSrv** view, u8** pixels, u32* pitch);
// The texture of a plane made by gpu_video_plane_create: its pixels changed; the renderer uploads them before the
// plane is next read. The decoder never writes a picture a reader may be converting.
void gpu_video_plane_publish(GpuTexture* texture);
// True for handles made by this module.
bool gpu_video_plane_owns(const GpuTexture* texture);

// Renderer thread: deletes the GL objects of planes nobody references any more.
void gpu_video_planes_reap(GpuDevice* device);

// The DRM render node the device renders on (EGL_EXT_device_query with EGL_EXT_device_drm_render_node), or false
// when the driver does not say (software rendering, a driver without the extensions). `path` takes at least 64 bytes.
bool gpu_dmabuf_render_node(GpuDevice* device, char* path, u32 capacity);

} // namespace maicord
