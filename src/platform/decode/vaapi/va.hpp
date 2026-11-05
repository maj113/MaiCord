#pragma once

#include "maicord/gpu/video_planes.hpp"
#include "maicord/platform/decode/gpu_stream_decoder.hpp"
#include "maicord/platform/decode/vaapi/abi.hpp"

namespace maicord::vaapi {

// The hardware tier's access to VA-API: libva.so.2 and libva-drm.so.2 through dlopen (MAICORD_VAAPI=off disables the
// tier), a display on the DRM render node of the GPU the renderer uses, and one session per live stream.

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
    // slice mode, and a surface that exports as dma-bufs the renderer's EGL imports (probed with a real surface).
    u32 codecs;
    // The codecs (AV1 only) whose profile also decodes 10-bit 4:2:0 into P010 surfaces that export and import the same
    // way.
    u32 ten_bit;
    // The VA profiles found per codec, best first (H.264: High, Main, Constrained Baseline; HEVC: Main; AV1: Profile0);
    // 0 where none.
    i32 profiles[3][kProfileCandidates];
    u32 max_width[3], max_height[3];
    // vaSyncSurface2 works on this driver (a timeout instead of an unbounded wait); others get vaSyncSurface.
    bool timed_sync;
    // A surface's exports are one dma-buf each time (see exports_are_stable): the EGL images of an export are kept.
    bool stable_exports;
};

// Null when libva, a render node, a display or any decoder is missing, or when `gpu` cannot import dma-bufs.
const Driver* driver(GpuDevice* gpu) noexcept;

// ---- Pure helpers (tests) --------------------------------------------------------------------------------------

// The VA profile to decode an H.264 stream of this profile_idc with: the lowest profile of the driver that contains
// it, 0 when the driver has none.
i32 h264_profile_for(const Driver& driver, u8 profile_idc) noexcept;

// Whether the driver named by this vaQueryVendorString hands out the same dma-buf for a surface every time and writes
// the next picture into it, so that the EGL images made from an export keep showing the surface: the Mesa Gallium
// drivers (radeonsi, nouveau) and Intel's iHD and i965. The others (nvidia-vaapi-driver copies the picture into a
// buffer at export time) are imported again for every picture.
bool exports_are_stable(const char* vendor) noexcept;

// The planes of an exported NV12 or P010 surface: separate layers, one dma-buf plane each (plus the compression
// metadata plane of a modifier that has one), R8 or R16 for luma and GR88 or GR1616 at half the extent for chroma.
// False for any other layout.
struct ExportLayout {
    GpuDmabufPlane plane[2];
    u32 objects; // file descriptors in the descriptor that must be closed
};
bool export_layout(const DrmPrimeDescriptor& descriptor, ExportLayout* layout) noexcept;

// Which buffers and layout an export names, independent of the descriptors it came in: the dma-buf of every plane
// (the inode of its descriptor: one per dma-buf, the same for every descriptor of it) and the layout. Two exports with
// the same identity are the same picture memory. No padding: same_export compares bytes.
struct ExportIdentity {
    struct Part {
        u64 inode, aux_inode, modifier;
        u32 fourcc, width, height, offset, pitch, aux_offset, aux_pitch, reserved;
    } plane[2];
};
static_assert(sizeof(ExportIdentity) == 112);
// False when a descriptor cannot be told apart (statx fails).
bool export_identity(const ExportLayout& layout, ExportIdentity* identity) noexcept;
bool same_export(const ExportIdentity& a, const ExportIdentity& b) noexcept;

// ---- One live stream's decoder ------------------------------------------------------------------------------------

// A decoded surface as two textures and views, each handle carrying one reference.
struct ExportedPicture {
    GpuTexture* luma_texture;
    GpuSrv* luma_view;
    GpuTexture* chroma_texture;
    GpuSrv* chroma_view;
};

// What a session remembers of a surface's last export while the driver's exports are stable.
struct KeptExport {
    ExportIdentity identity;
    ExportedPicture planes; // the session's own references
    bool valid;
};

// vaSyncSurface2's wait for one picture: past it the GPU is hung, and the picture fails.
inline constexpr u64 kSyncTimeoutNanoseconds = 1'000'000'000;

inline constexpr u32 kSurfaceLimit = 32;
struct Session {
    const Driver* driver;
    ConfigId config;
    ContextId context;
    SurfaceId surface[kSurfaceLimit];
    KeptExport kept[kSurfaceLimit];
    u32 count;
    u32 width, height; // allocated extent of every surface
    i32 profile;
    bool ten_bit; // P010 surfaces
    // Buffers of the picture being decoded, destroyed at picture_end.
    BufferId created[8 + 2 * 512];
    u32 created_count;
    BufferId pending[2 * 64]; // slice parameter and data buffers waiting for vaRenderPicture
    u32 pending_count;
    SurfaceId target;
    bool open; // between picture_begin and picture_end
    bool failed;
    bool unsynced; // `target` of the last picture ended has not been waited for
};

// A config, `count` surfaces of width x height (the driver rounds them up as it needs) and a context. NV12 surfaces,
// P010 for `ten_bit`. Null on failure.
Session* session_create(const Driver* driver, i32 profile, u32 width, u32 height, u32 count,
                        bool ten_bit = false) noexcept;
void session_destroy(Session* session) noexcept;

// One picture: begin, the parameter buffers, each slice (parameters, then data, copied at once; AV1 gives its tiles'
// parameters and one data buffer they share), end. picture_end returns once the picture is submitted: the decoder may
// still be working on it, and the wait is session_sync's (export_picture makes it, so a picture that is never shown
// is never waited for). Any failure makes the rest no-ops and picture_end false.
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

// Waits until the decoder is done with the surface: vaSyncSurface2 with kSyncTimeoutNanoseconds on drivers that have
// it, vaSyncSurface otherwise. False when the wait fails or times out.
bool session_sync(Session* session, u32 surface) noexcept;

// Waits for the surface, exports it and imports its planes as the two textures and views of a picture; each handle
// carries one reference. On a driver with stable exports the images of the previous export of the surface are the ones
// returned while the export names the same buffers and layout. False when the wait, the export or an import fails.
bool export_picture(GpuDevice* gpu, Session* session, u32 surface, ExportedPicture* picture) noexcept;

} // namespace maicord::vaapi
