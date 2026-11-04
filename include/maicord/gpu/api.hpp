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
enum class GpuFormat : u8 { rgba8, bgra8, r8, rg8, rgba16f, rgba32f, r32_uint };

enum class GpuUsage : u8 {
    default_,     // GPU read/write; content set at creation or by the GPU
    dynamic,      // CPU write through gpu_map (buffers only)
    immutable,    // content given at creation, never changed
    staging_read, // CPU readback target (textures; the backend may not create a real one)
};

enum GpuBind : u8 {
    gpu_bind_vertex = 1,
    gpu_bind_constant = 2,
    gpu_bind_srv = 4,
    gpu_bind_rtv = 8,
    gpu_bind_uav = 16,
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
};

enum class GpuFilter : u8 { point, linear };
enum class GpuAddress : u8 { clamp, wrap };
// Mip level 0 only, one filter for minification and magnification.
struct GpuSamplerDesc {
    GpuFilter filter;
    GpuAddress address;
};

enum class GpuBlendFactor : u8 { zero, one, src_color, inv_src_color, src_alpha, inv_src_alpha, blend_factor, src1_color };
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

// Creates the process's device and immediate context (one per process). The GPU is picked by the driver stack.
GpuStatus gpu_device_create(GpuDevice** device, GpuContext** context);
// Releases every resource the device still tracks, then the device. The caller released its own handles first.
void gpu_device_destroy(GpuDevice** device, GpuContext** context);
const GpuCaps& gpu_caps(const GpuDevice* device);
// Names the driver stack for logs: vendor, renderer and API version in one line.
const char* gpu_device_description(const GpuDevice* device);
// True after the driver reported a lost or reset device; every later call may fail.
bool gpu_device_lost(GpuDevice* device);

// ---- Resources -------------------------------------------------------------------------------------------

GpuStatus gpu_buffer_create(GpuDevice* device, const GpuBufferDesc& desc, const void* initial, GpuBuffer** buffer);
GpuStatus gpu_texture_create(GpuDevice* device, const GpuTextureDesc& desc, const GpuInitialData* initial,
                             GpuTexture** texture);
GpuStatus gpu_srv_create(GpuDevice* device, GpuTexture* texture, const GpuViewDesc& desc, GpuSrv** view);
GpuStatus gpu_srv_create(GpuDevice* device, GpuBuffer* buffer, const GpuViewDesc& desc, GpuSrv** view);
GpuStatus gpu_uav_create(GpuDevice* device, GpuTexture* texture, GpuUav** view);
GpuStatus gpu_uav_create(GpuDevice* device, GpuBuffer* buffer, GpuUav** view);
GpuStatus gpu_rtv_create(GpuDevice* device, GpuTexture* texture, GpuRtv** view);
GpuStatus gpu_sampler_create(GpuDevice* device, const GpuSamplerDesc& desc, GpuSampler** sampler);
GpuStatus gpu_blend_create(GpuDevice* device, const GpuBlendDesc& desc, GpuBlend** blend);
// Solid fill, no culling, scissor test on.
GpuStatus gpu_raster_create(GpuDevice* device, GpuRaster** raster);

// The built-in shaders by ShaderId. A vertex shader creates its input layout from `elements` (empty: none, for
// programs that read only the vertex id); *layout_failed tells which of the two creations failed.
GpuStatus gpu_vertex_shader_create(GpuDevice* device, ShaderId id, ConstSlice<GpuInputElement> elements,
                                   GpuVertexShader** shader, GpuInputLayout** layout, bool* layout_failed);
GpuStatus gpu_pixel_shader_create(GpuDevice* device, ShaderId id, GpuPixelShader** shader);
GpuStatus gpu_compute_shader_create(GpuDevice* device, ShaderId id, GpuComputeShader** shader);

// ---- Commands (each is one immediate call on a context) -----------------------------------------------------

// A null target unbinds.
void gpu_set_render_target(GpuContext* context, GpuRtv* target);
void gpu_set_viewport(GpuContext* context, float x, float y, float width, float height);
void gpu_set_scissor(GpuContext* context, const GpuRect& rectangle);
void gpu_set_raster(GpuContext* context, GpuRaster* raster);
void gpu_set_blend(GpuContext* context, GpuBlend* blend, const float factor[4]);
void gpu_set_input_layout(GpuContext* context, GpuInputLayout* layout);
void gpu_set_vertex_buffer(GpuContext* context, GpuBuffer* buffer, u32 stride, u32 offset);
void gpu_set_shader(GpuContext* context, GpuVertexShader* shader);
void gpu_set_shader(GpuContext* context, GpuPixelShader* shader);
void gpu_set_shader(GpuContext* context, GpuComputeShader* shader);
// Slots are the HLSL register numbers of the stage (b#, t#, s#, u#); a null entry unbinds the slot.
void gpu_set_constants(GpuContext* context, GpuStage stage, u32 first, u32 count, GpuBuffer* const* buffers);
void gpu_set_srvs(GpuContext* context, GpuStage stage, u32 first, u32 count, GpuSrv* const* views);
void gpu_set_samplers(GpuContext* context, GpuStage stage, u32 first, u32 count, GpuSampler* const* samplers);
void gpu_set_uavs(GpuContext* context, u32 first, u32 count, GpuUav* const* views); // compute only
// Triangle strips only.
void gpu_draw(GpuContext* context, u32 vertices, u32 first_vertex);
void gpu_draw_instanced(GpuContext* context, u32 vertices, u32 instances, u32 first_vertex, u32 first_instance);
void gpu_dispatch(GpuContext* context, u32 x, u32 y, u32 z);
void gpu_clear_rtv(GpuContext* context, GpuRtv* target, const float rgba[4]);
// Clears only `rectangle` (D3D11 ClearView), ignoring the scissor.
void gpu_clear_rtv_rect(GpuContext* context, GpuRtv* target, const float rgba[4], const GpuRect& rectangle);
// Replaces the whole contents of a default-usage constant buffer from `bytes` bytes.
void gpu_update_buffer(GpuContext* context, GpuBuffer* buffer, const void* data, u32 bytes);
// Write-only mapping of a dynamic buffer: the whole buffer for discard, which keeps no old contents, or for
// no_overwrite, which promises the GPU does not read the bytes written. Null on failure.
void* gpu_map(GpuContext* context, GpuBuffer* buffer, GpuMap mode, GpuStatus* status);
void gpu_unmap(GpuContext* context, GpuBuffer* buffer);
// Copies `box` of `source` to (x, y) of `destination`; both have the same format.
void gpu_copy_region(GpuContext* context, GpuTexture* destination, u32 x, u32 y, GpuTexture* source, const GpuBox& box);
// Waits for the GPU and writes the rows of `box` as the texture's format, tightly packed by `pitch` bytes per row.
GpuStatus gpu_readback(GpuDevice* device, GpuContext* context, GpuTexture* source, const GpuBox& box, void* destination,
                       u32 pitch);
void gpu_flush(GpuContext* context);
// Unbinds every slot and target (the recovery path after a device loss).
void gpu_clear_state(GpuContext* context);

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
