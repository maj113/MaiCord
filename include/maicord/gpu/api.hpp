#pragma once

// The backend-neutral GPU interface: a thin, D3D11-shaped immediate-mode API of free functions over the pointer
// handles of maicord/gpu/gpu.hpp. Every function is implemented by the build's backend (src/gpu/<backend>); on
// Direct3D 11 each is one inline call and no object changes size, on OpenGL ES each is a few GL calls over a state
// tracker. Nothing here is virtual: the backend is chosen at compile time.
//
// Conventions shared by every backend:
//  * Coordinates have a top-left origin and a texture's row 0 is the top row (D3D11's convention): render targets,
//    uploads, readbacks, scissors and viewports agree, so no caller flips anything.
//  * Calls come from one thread (the renderer's) unless a function says otherwise.
//  * A failed create call returns a negative status, writes null to its output and leaves nothing allocated.

#include "maicord/core/slice.hpp"
#include "maicord/gpu/gpu.hpp"
#include "maicord/gpu/shader_id.hpp"
#include "maicord/os/wait.hpp"

// Direct3D 11 defines the functions marked MAICORD_GPU_INLINE inline in maicord/gpu/d3d11/api.inl (included at the end
// of this header), so each compiles to the D3D11 call it wraps; other backends define them out of line.
#if defined(_WIN32)
#define MAICORD_GPU_INLINE __forceinline
#else
#define MAICORD_GPU_INLINE
#endif

namespace maicord {

// Statuses with the values of the HRESULTs they stand for, so result structs and logs keep one vocabulary.
inline constexpr GpuStatus kGpuOk = 0;
inline constexpr GpuStatus kGpuFail = static_cast<GpuStatus>(0x80004005u);
inline constexpr GpuStatus kGpuInvalidArgument = static_cast<GpuStatus>(0x80070057u);
inline constexpr GpuStatus kGpuOutOfMemory = static_cast<GpuStatus>(0x8007000Eu);
inline constexpr GpuStatus kGpuUnsupported = static_cast<GpuStatus>(0x887A0004u);
inline constexpr GpuStatus kGpuDeviceLost = static_cast<GpuStatus>(0x887A0005u);

enum class GpuStage : u8 { vertex, pixel, compute };

// Texel formats of textures. rgba8, rg8 and r8 are UNORM. bgra8 is for sampled textures that arrive as BGRA bytes
// (it is not a render-target format on every backend). A backend may store a format wider than it names (r8 as
// RGBA8 where 8-bit single-channel images are not storable) as long as sampling, upload, copy and readback behave as
// the named format. r32_uint names the typed-buffer view format.
// The enumerators of GpuFormat, GpuUsage, GpuBind and GpuBlendFactor carry Direct3D 11's values (DXGI_FORMAT,
// D3D11_USAGE, D3D11_BIND_*, D3D11_BLEND) so that backend passes them through unconverted; other backends switch on
// the names and never depend on the numbers.
enum class GpuFormat : u8 { rgba8 = 28, bgra8 = 87, r8 = 61, rg8 = 49, rgba16f = 10, rgba32f = 2, r32_uint = 42 };

enum class GpuUsage : u8 {
    default_ = 0,     // GPU read/write; content set at creation or by the GPU
    immutable = 1,    // content given at creation, never changed
    dynamic = 2,      // CPU write through gpu_map (buffers only)
    staging_read = 3, // CPU readback target (textures; the backend may not create a real one)
};

enum GpuBind : u8 {
    gpu_bind_vertex = 0x01,
    gpu_bind_constant = 0x04,
    gpu_bind_srv = 0x08,
    gpu_bind_rtv = 0x20,
    gpu_bind_uav = 0x80,
};

enum class GpuMap : u8 { discard, no_overwrite };

struct GpuRect {
    i32 left, top, right, bottom;
};
struct GpuBox {
    u32 left, top, right, bottom;
};

struct GpuBufferDesc {
    u32 bytes;
    GpuUsage usage;
    u8 bind;  // GpuBind bits
    bool raw; // byte-address storage for shaders (SRV/UAV views of kind buffer_raw)
};

struct GpuTextureDesc {
    u32 width, height;
    GpuFormat format;
    GpuUsage usage;
    u8 bind; // GpuBind bits
};

// Initial texel rows of an immutable or default texture; pitch is the byte distance between rows.
struct GpuInitialData {
    const void* data;
    u32 pitch;
};

enum class GpuViewKind : u8 {
    texture,      // the whole texture
    buffer_raw,   // byte-address buffer
    buffer_typed, // one u32 per element (format r32_uint)
};

struct GpuViewDesc {
    GpuViewKind kind;
    GpuFormat format; // the view's format (a texture view may not change the texture's)
    u32 elements;     // buffer views: the u32 elements covered (the buffer's bytes / 4); texture views: 0
};

enum class GpuFilter : u8 { point = 0, linear = 0x14 };
enum class GpuAddress : u8 { clamp = 3, wrap = 1 };
// Mip level 0 only, one filter for minification and magnification.
struct GpuSamplerDesc {
    GpuFilter filter;
    GpuAddress address;
};

enum class GpuBlendFactor : u8 {
    zero = 1, one = 2, src_color = 3, inv_src_color = 4, src_alpha = 5, inv_src_alpha = 6, blend_factor = 14, src1_color = 16
};
// ADD for colour and alpha, all channels written. enable false is blending off.
struct GpuBlendDesc {
    bool enable;
    GpuBlendFactor source, destination, source_alpha, destination_alpha;
};

// One vertex attribute: four 32-bit floats read per instance (step rate 1) at `offset` of the vertex buffer record.
// `semantic` and `index` name it for Direct3D (RECT, COLOR0, TEXCOORD0); other backends use the element order.
struct GpuInputElement {
    const char* semantic;
    u32 index;
    u32 offset;
};

// ---- Device ----------------------------------------------------------------------------------------------

// Creates the process's device and immediate context (one per process). The GPU is picked by the driver stack. When
// gpu_device_prestart ran, this adopts that device once the worker has made it (and creates one itself when the
// worker failed).
GpuStatus gpu_device_create(GpuDevice** device, GpuContext** context);
// Releases every resource the device still tracks, then the device. The caller released its own handles first.
void gpu_device_destroy(GpuDevice** device, GpuContext** context);
const GpuCaps& gpu_caps(const GpuDevice* device);
// Names the driver stack for logs: vendor, renderer and API version in one line.
const char* gpu_device_description(const GpuDevice* device);
// A stable identity of the GPU (vendor, device, revision and adapter) that per-GPU records are keyed by.
u64 gpu_adapter_key(const GpuDevice* device);
// True after the driver reported a lost or reset device; every later call may fail.
bool gpu_device_lost(GpuDevice* device);
// Call before gpu_device_create: the window system connection surfaces will be created on (wl_display* on Wayland,
// ignored on Windows). EGL makes the display from it, so a context made without it cannot present to a window.
void gpu_device_set_native_display(void* native_display);

// Startup overlap: a worker thread makes the device so driver initialisation runs beside the rest of startup, then
// runs `continuation` on that thread with the device and its context. The worker owns the context until
// gpu_device_prestart_finish returns: no other thread may use it before then (creating objects on the device is
// free-threaded). Main thread only; a second call is ignored.
using GpuDevicePrestartContinuation = void (*)(GpuDevice* device, GpuContext* context) noexcept;
void gpu_device_prestart(GpuDevicePrestartContinuation continuation);
// Waits for the worker and its continuation; releases a device nobody adopted.
void gpu_device_prestart_finish();
// True when the device came from the prestart worker.
bool gpu_device_prestarted(const GpuDevice* device);
// The context's extended interface (ClearView, offset constant buffers; also fence signalling where `GpuContext4` is
// distinct): the same object, not counted; null when the device lacks it. Other backends return the context itself.
GpuContext1* gpu_context_1(GpuContext* context);
// The device a context belongs to, not counted.
MAICORD_GPU_INLINE GpuDevice* gpu_context_device(GpuContext* context);
// Whether textures of `format` support every use in the GpuBind bits `bind` (shader sampling, render target,
// unordered access). kGpuOk when they do, kGpuUnsupported when not, the driver's error otherwise.
MAICORD_GPU_INLINE GpuStatus gpu_format_check(GpuDevice* device, GpuFormat format, u8 bind);

// ---- Resources -------------------------------------------------------------------------------------------

MAICORD_GPU_INLINE GpuStatus gpu_buffer_create(GpuDevice* device, const GpuBufferDesc& desc, const void* initial,
                                               GpuBuffer** buffer);
MAICORD_GPU_INLINE GpuStatus gpu_texture_create(GpuDevice* device, const GpuTextureDesc& desc,
                                                const GpuInitialData* initial, GpuTexture** texture);
// What gpu_texture_info reads of a texture (size, format, usage, bind bits): enough to make another texture like it
// at another height. The backend defines the type.
struct GpuTextureInfo;
#if !defined(_WIN32)
struct GpuTextureInfo {
    u32 width, height;
    GpuFormat format;
    GpuUsage usage;
    u8 bind;
};
#endif
MAICORD_GPU_INLINE void gpu_texture_info(GpuTexture* texture, GpuTextureInfo* info);
MAICORD_GPU_INLINE u32 gpu_texture_info_width(const GpuTextureInfo& info);
MAICORD_GPU_INLINE u32 gpu_texture_info_height(const GpuTextureInfo& info);
// Makes a texture with the info's width, format, usage and bind bits at `height`, no initial data. The info becomes
// that texture's (its height is `height`; with copy_only it has no bind bits: the texture is only a copy source and
// destination, a disposal backup), so the creation builds nothing beyond it.
MAICORD_GPU_INLINE GpuStatus gpu_texture_create_from_info(GpuDevice* device, GpuTextureInfo& info, u32 height,
                                                          bool copy_only, GpuTexture** texture);
MAICORD_GPU_INLINE GpuStatus gpu_srv_create(GpuDevice* device, GpuTexture* texture, const GpuViewDesc& desc,
                                            GpuSrv** view);
MAICORD_GPU_INLINE GpuStatus gpu_srv_create(GpuDevice* device, GpuBuffer* buffer, const GpuViewDesc& desc,
                                            GpuSrv** view);
MAICORD_GPU_INLINE GpuStatus gpu_uav_create(GpuDevice* device, GpuTexture* texture, GpuUav** view);
MAICORD_GPU_INLINE GpuStatus gpu_uav_create(GpuDevice* device, GpuBuffer* buffer, u32 elements, GpuUav** view);
MAICORD_GPU_INLINE GpuStatus gpu_rtv_create(GpuDevice* device, GpuTexture* texture, GpuRtv** view);
MAICORD_GPU_INLINE GpuStatus gpu_sampler_create(GpuDevice* device, const GpuSamplerDesc& desc, GpuSampler** sampler);
MAICORD_GPU_INLINE GpuStatus gpu_blend_create(GpuDevice* device, const GpuBlendDesc& desc, GpuBlend** blend);
// Solid fill, no culling, scissor test on.
MAICORD_GPU_INLINE GpuStatus gpu_raster_create(GpuDevice* device, GpuRaster** raster);

// The built-in shaders by ShaderId. A vertex shader creates its input layout from `elements` (empty: none, for
// programs that read only the vertex id; the layout output may then be null); *layout_failed (optional) tells which
// of the two creations failed.
GpuStatus gpu_vertex_shader_create(GpuDevice* device, ShaderId id, ConstSlice<GpuInputElement> elements,
                                   GpuVertexShader** shader, GpuInputLayout** layout, bool* layout_failed);
GpuStatus gpu_pixel_shader_create(GpuDevice* device, ShaderId id, GpuPixelShader** shader);
GpuStatus gpu_compute_shader_create(GpuDevice* device, ShaderId id, GpuComputeShader** shader);
// Shaders of a package (an effect), as bytes in this backend's shader language: DXBC on Direct3D 11.
GpuStatus gpu_vertex_shader_create_blob(GpuDevice* device, ConstSlice<u8> payload, GpuVertexShader** shader);
GpuStatus gpu_pixel_shader_create_blob(GpuDevice* device, ConstSlice<u8> payload, GpuPixelShader** shader);

// ---- Commands (each is one immediate call on a context) -----------------------------------------------------

// A null target unbinds.
MAICORD_GPU_INLINE void gpu_set_render_target(GpuContext* context, GpuRtv* target);
// Depth range 0 to 1.
MAICORD_GPU_INLINE void gpu_set_viewport(GpuContext* context, float x, float y, float width, float height);
MAICORD_GPU_INLINE void gpu_set_scissor(GpuContext* context, const GpuRect& rectangle);
MAICORD_GPU_INLINE void gpu_set_raster(GpuContext* context, GpuRaster* raster);
// A null blend is blending off (the factor is then ignored and may be null).
MAICORD_GPU_INLINE void gpu_set_blend(GpuContext* context, GpuBlend* blend, const float factor[4]);
MAICORD_GPU_INLINE void gpu_set_input_layout(GpuContext* context, GpuInputLayout* layout);
MAICORD_GPU_INLINE void gpu_set_vertex_buffer(GpuContext* context, GpuBuffer* buffer, u32 stride, u32 offset);
// Triangle strips are the only topology; every draw uses them. Direct3D needs it set once, OpenGL ES ignores it.
MAICORD_GPU_INLINE void gpu_set_strip_topology(GpuContext* context);
MAICORD_GPU_INLINE void gpu_set_shader(GpuContext* context, GpuVertexShader* shader);
MAICORD_GPU_INLINE void gpu_set_shader(GpuContext* context, GpuPixelShader* shader);
MAICORD_GPU_INLINE void gpu_set_shader(GpuContext* context, GpuComputeShader* shader);
// Slots are the HLSL register numbers of the stage (b#, t#, s#, u#); a null entry unbinds the slot.
MAICORD_GPU_INLINE void gpu_set_constants(GpuContext* context, GpuStage stage, u32 first, u32 count,
                                          GpuBuffer* const* buffers);
// One compute-stage constant buffer slot bound to `constant_count` 16-byte constants from `first_constant` of the buffer.
MAICORD_GPU_INLINE void gpu_set_constants_range(GpuContext1* context, u32 slot, GpuBuffer* buffer, u32 first_constant,
                                                u32 constant_count);
MAICORD_GPU_INLINE void gpu_set_srvs(GpuContext* context, GpuStage stage, u32 first, u32 count, GpuSrv* const* views);
MAICORD_GPU_INLINE void gpu_set_samplers(GpuContext* context, GpuStage stage, u32 first, u32 count,
                                         GpuSampler* const* samplers);
MAICORD_GPU_INLINE void gpu_set_uavs(GpuContext* context, u32 first, u32 count, GpuUav* const* views); // compute only
// Triangle strips only.
MAICORD_GPU_INLINE void gpu_draw(GpuContext* context, u32 vertices, u32 first_vertex);
MAICORD_GPU_INLINE void gpu_draw_instanced(GpuContext* context, u32 vertices, u32 instances, u32 first_vertex,
                                           u32 first_instance);
MAICORD_GPU_INLINE void gpu_dispatch(GpuContext* context, u32 x, u32 y, u32 z);
MAICORD_GPU_INLINE void gpu_clear_rtv(GpuContext* context, GpuRtv* target, const float rgba[4]);
// Clears only `rectangle` (D3D11 ClearView), ignoring the scissor.
MAICORD_GPU_INLINE void gpu_clear_rtv_rect(GpuContext1* context, GpuRtv* target, const float rgba[4],
                                           const GpuRect& rectangle);
// Replaces the whole contents of a default-usage constant buffer from `bytes` bytes.
MAICORD_GPU_INLINE void gpu_update_buffer(GpuContext* context, GpuBuffer* buffer, const void* data, u32 bytes);
// Write-only mapping of a dynamic buffer: the whole buffer for discard, which keeps no old contents, or for
// no_overwrite, which promises the GPU does not read the bytes written. Null on failure.
MAICORD_GPU_INLINE void* gpu_map(GpuContext* context, GpuBuffer* buffer, GpuMap mode, GpuStatus* status);
MAICORD_GPU_INLINE void gpu_unmap(GpuContext* context, GpuBuffer* buffer);
// Copies `box` of `source` to (x, y) of `destination`; both have the same format.
MAICORD_GPU_INLINE void gpu_copy_region(GpuContext* context, GpuTexture* destination, u32 x, u32 y, GpuTexture* source,
                                        const GpuBox& box);
// Waits for the GPU and writes the rows of `box` as the texture's format, tightly packed by `pitch` bytes per row.
MAICORD_GPU_INLINE GpuStatus gpu_readback(GpuDevice* device, GpuContext* context, GpuTexture* source,
                                          const GpuBox& box, void* destination, u32 pitch);
// The same, in place: a read-only view of the rows of `box` (as the texture's format, `pitch` bytes apart), valid until
// gpu_readback_unmap. `format` is the source's own. Nothing stays mapped when it fails. `backend` is the backend's.
struct GpuReadback {
    const void* data;
    u32 pitch;
    GpuFormat format;
    void* backend;
};
MAICORD_GPU_INLINE GpuStatus gpu_readback_map(GpuDevice* device, GpuContext* context, GpuTexture* source,
                                              GpuFormat format, const GpuBox& box, GpuReadback* readback);
MAICORD_GPU_INLINE void gpu_readback_unmap(GpuContext* context, GpuReadback* readback);
MAICORD_GPU_INLINE void gpu_flush(GpuContext* context);
// Unbinds every slot and target (the recovery path after a device loss).
MAICORD_GPU_INLINE void gpu_clear_state(GpuContext* context);

// ---- Frame timing and fences -------------------------------------------------------------------------------

// A GPU timer measures one frame between gpu_timer_begin and gpu_timer_end (the backend header defines the struct,
// which the caller holds by value, zeroed). A device without timers (caps.timer_query false) fails the create.
MAICORD_GPU_INLINE GpuStatus gpu_timer_create(GpuDevice* device, GpuTimer* timer);
MAICORD_GPU_INLINE bool gpu_timer_valid(const GpuTimer& timer);
MAICORD_GPU_INLINE void gpu_timer_release(GpuTimer* timer);
MAICORD_GPU_INLINE void gpu_timer_begin(GpuContext* context, GpuTimer* timer);
MAICORD_GPU_INLINE void gpu_timer_end(GpuContext* context, GpuTimer* timer);
enum class GpuPoll : u8 {
    pending, // the GPU has not finished the frame
    ready,   // *milliseconds is the frame's GPU time
    invalid, // the measurement is unusable (the clock jumped, the device reset)
};
// Reads a finished frame's time. `flush` also submits the queued work (a synchronous probe); without it a frame still
// in flight stays pending.
MAICORD_GPU_INLINE GpuPoll gpu_timer_poll(GpuContext* context, GpuTimer* timer, bool flush, double* milliseconds);

// A fence is signalled after the work queued before it and wakes an event when it passes a value. `signaller` is the
// context interface that signals (GpuContext4 on Direct3D 11, the context elsewhere); both are made together from the
// device's immediate context and released with gpu_release.
MAICORD_GPU_INLINE GpuStatus gpu_fence_create(GpuDevice* device, GpuContext4** signaller, GpuFence** fence);
MAICORD_GPU_INLINE GpuStatus gpu_fence_signal(GpuContext4* signaller, GpuFence* fence, u64 value);
MAICORD_GPU_INLINE GpuStatus gpu_fence_notify(GpuFence* fence, u64 value, Waitable event);

// Bytes one texel of `format` takes in the bytes gpu_readback and GpuInitialData carry (0 for r32_uint).
constexpr u32 gpu_texel_bytes(GpuFormat format) noexcept {
    switch (format) {
    case GpuFormat::rgba8:
    case GpuFormat::bgra8:
        return 4;
    case GpuFormat::r8:
        return 1;
    case GpuFormat::rg8:
        return 2;
    case GpuFormat::rgba16f:
        return 8;
    case GpuFormat::rgba32f:
        return 16;
    case GpuFormat::r32_uint:
        return 0;
    }
    return 0;
}

} // namespace maicord

#include "maicord/gpu/surface.hpp"
