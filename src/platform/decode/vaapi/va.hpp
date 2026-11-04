#pragma once

#include "maicord/gpu/video_planes.hpp"
#include "maicord/platform/decode/gpu_stream_decoder.hpp"
#include "maicord/platform/decode/vaapi/abi.hpp"

namespace maicord::vaapi {

// The hardware tier's access to VA-API: libva.so.2 and libva-drm.so.2 through dlopen (MAICORD_VAAPI_LIBRARY and
// MAICORD_VAAPI_DRM_LIBRARY replace the sonames; MAICORD_VAAPI=off disables the tier), a display on the DRM render node
// of the GPU the renderer uses, and one session per live stream.

// What the process's VA display decodes. Opened and probed once: the render node is MAICORD_VAAPI_DEVICE when set,
// else the one EGL reports for the renderer's device (EGL_EXT_device_query, EGL_DRM_RENDER_NODE_FILE_EXT), else the
// first /dev/dri/renderD* a VA display initializes on.
inline constexpr u32 kProfileCandidates = 4;
struct Driver {
    Api api;
    Display display;
    i32 descriptor; // the render node
    char node[64];
    // gpu_stream_codec_bit set the driver decodes: a VLD entry point of a profile, 8-bit 4:2:0 surfaces, the normal
    // slice mode, and a surface that exports as one dma-buf per plane (probed with a real surface).
    u32 codecs;
    // The VA profiles found per codec, best first (H.264: High, Main, Constrained Baseline; HEVC: Main; AV1: Profile0);
    // 0 where none.
    i32 profiles[3][kProfileCandidates];
    u32 max_width[3], max_height[3];
};

// Null when libva, a render node, a display or any decoder is missing, or when `gpu` cannot import dma-bufs.
const Driver* driver(GpuDevice* gpu) noexcept;

// ---- Pure helpers (tests) --------------------------------------------------------------------------------------

// The VA profile to decode an H.264 stream of this profile_idc with: the lowest profile of the driver that contains
// it, 0 when the driver has none.
i32 h264_profile_for(const Driver& driver, u8 profile_idc) noexcept;

// The two dma-buf planes of an exported NV12 or P010 surface: separate layers, one plane each, R8 or R16 for luma
// and GR88 or GR1616 at half the extent for chroma. False for any other layout.
struct ExportLayout {
    GpuDmabufPlane plane[2];
    u32 objects; // file descriptors in the descriptor that must be closed
};
bool export_layout(const DrmPrimeDescriptor& descriptor, ExportLayout* layout) noexcept;

// ---- One live stream's decoder ------------------------------------------------------------------------------------

inline constexpr u32 kSurfaceLimit = 32;
struct Session {
    const Driver* driver;
    ConfigId config;
    ContextId context;
    SurfaceId surface[kSurfaceLimit];
    u32 count;
    u32 width, height; // allocated extent of every surface
    i32 profile;
    // Buffers of the picture being decoded, destroyed at picture_end.
    BufferId created[8 + 2 * 512];
    u32 created_count;
    BufferId pending[2 * 64]; // slice parameter and data buffers waiting for vaRenderPicture
    u32 pending_count;
    SurfaceId target;
    bool open; // between picture_begin and picture_end
    bool failed;
};

// A config, `count` surfaces of width x height (the driver rounds them up as it needs) and a context. Null on failure.
Session* session_create(const Driver* driver, i32 profile, u32 width, u32 height, u32 count) noexcept;
void session_destroy(Session* session) noexcept;

// One picture: begin, the parameter buffers, each slice (parameters, then data, copied at once; AV1 gives its tiles'
// parameters and one data buffer they share), end. picture_end
// returns once the decoder finished the picture (vaSyncSurface). Any failure makes the rest no-ops and picture_end
// false.
bool picture_begin(Session* session, u32 surface) noexcept;
bool picture_buffer(Session* session, i32 type, const void* data, u32 bytes) noexcept;
bool picture_slice_parameters(Session* session, const void* parameters, u32 bytes) noexcept;
bool picture_slice_data(Session* session, const void* data, u32 bytes) noexcept;
// A slice's parameters followed by its data.
inline bool picture_slice(Session* session, const void* parameters, u32 parameter_bytes, const void* data,
                          u32 data_bytes) noexcept {
    return picture_slice_parameters(session, parameters, parameter_bytes) && picture_slice_data(session, data, data_bytes);
}
bool picture_end(Session* session) noexcept;

// Exports a decoded surface and imports its planes as the two textures and views of a picture; each handle carries one
// reference. False when the export or an import fails.
struct ExportedPicture {
    GpuTexture* luma_texture;
    GpuSrv* luma_view;
    GpuTexture* chroma_texture;
    GpuSrv* chroma_view;
};
bool export_picture(GpuDevice* gpu, Session* session, u32 surface, ExportedPicture* picture) noexcept;

} // namespace maicord::vaapi
