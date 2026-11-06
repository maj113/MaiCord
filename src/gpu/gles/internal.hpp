#pragma once

// The OpenGL ES 3.1 GPU backend: one EGL context made current on the calling thread, a table of GL entry points
// loaded at run time, fixed slabs of handle bodies and a state tracker that applies the D3D11-style state of
// maicord/gpu/api.hpp when something is drawn. No allocation per resource, no virtual calls. The single device and
// context are process globals (static storage: images run no constructors, so every global here is zero-initialized
// data and is set up by gpu_device_create).

#include "maicord/core/memory.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/gpu/glsl_shader.hpp"
#include "maicord/sys/linux/egl.hpp"
#include "maicord/sys/linux/gles.hpp"

namespace maicord {

// One row of a shader's binding manifest: how an HLSL resource slot maps to a GL binding point
// (tools/build_gles_shader_pack.py writes them; the layout is the pack format, see shader_pack.cpp).
enum class GlesBindingKind : u8 {
    uniform_buffer, // b#   -> glBindBufferBase(UNIFORM_BUFFER, binding)
    texture,        // t#+s# -> texture unit `binding` with the sampler of slot `sampler` (0xff: none)
    storage_srv,    // t# raw or structured buffer -> SHADER_STORAGE binding
    storage_uav,    // u# raw buffer -> SHADER_STORAGE binding
    image,          // u# texture -> image unit `binding`
};
inline constexpr u8 kGlesNoSampler = 0xff;

struct GlesBinding {
    u8 kind; // GlesBindingKind
    u8 slot;
    u8 binding;
    u8 sampler;
};
static_assert(sizeof(GlesBinding) == 4);

struct GpuBuffer {
    u32 refs; // owners: the handle, every view, every retain (a texture's the same)
    u32 name;
    u32 bytes;
    GpuUsage usage;
    u8 bind;
    bool raw;
    bool mapped;
};

struct GpuTexture {
    u32 refs; // owners: the handle, every view, every retain
    u32 name;
    u32 framebuffer; // created on first use as a render or read target
    u32 width, height;
    gl::Enum internal; // the GL storage format
    GpuFormat format;  // the logical format callers see
    u8 bind;
    GpuUsage usage;
    bool wide; // r8 stored as RGBA8 because it is a storage image
};

struct GpuSrv {
    u32 refs; // gpu_retain: owners beyond the first
    GpuViewKind kind;
    GpuTexture* texture;
    GpuBuffer* buffer;
};
struct GpuUav {
    GpuTexture* texture;
    GpuBuffer* buffer;
};
struct GpuRtv {
    GpuTexture* texture;
};
// One texture per backed page, with its views. `view` (never counted, no texture) only stands for the surface: draws
// bind a page's own view.
struct GpuPagedTexture {
    GpuSrv view;
    GpuTexture* textures[kGpuPagedTextureMaximumPages];
    GpuSrv* views[kGpuPagedTextureMaximumPages];
    GpuUav* uavs[kGpuPagedTextureMaximumPages];
    GpuRtv* targets[kGpuPagedTextureMaximumPages];
    u32 width;
    u32 page_rows;
    u32 pages;
    GpuFormat format;
    u8 bind;
};
struct GpuSampler {
    u32 name;
    u32 nearest_name; // a linear sampler's point twin: float32 textures are not filtered without float_linear
};
struct GpuBlend {
    GpuBlendDesc desc;
};
struct GpuRaster {
    u8 reserved;
};

struct GlesShader {
    u32 name;                   // GL shader object (graphics) or program (compute)
    const GlesBinding* rows;    // inside the pack, which outlives every shader, or owned_rows
    u32 row_count;
    ShaderId id;
    GlesBinding owned_rows[kGlslBindingMaximum]; // a package shader's manifest
};
struct GpuVertexShader : GlesShader {};
struct GpuPixelShader : GlesShader {};
struct GpuComputeShader : GlesShader {};

struct GpuInputLayout {
    u32 vertex_array;
    u32 count;
};

template <typename T, u32 Capacity>
struct GlesSlab {
    T items[Capacity];
    u16 free_list[Capacity];
    u32 free_count;
    bool ready;

    void reset() noexcept {
        clear_bytes(items, static_cast<u32>(sizeof(items)));
        for (u32 index = 0; index < Capacity; ++index)
            free_list[index] = static_cast<u16>(Capacity - 1 - index);
        free_count = Capacity;
        ready = true;
    }
    T* allocate() noexcept {
        if (free_count == 0)
            return nullptr;
        T* item = &items[free_list[--free_count]];
        clear_bytes(item, static_cast<u32>(sizeof(T)));
        return item;
    }
    void release(T* item) noexcept {
        free_list[free_count++] = static_cast<u16>(item - items);
    }
};

inline constexpr u32 kGlesConstantSlots = 8; // HLSL b0..b7 of a stage
inline constexpr u32 kGlesResourceSlots = 16; // t0..t15, s0..s15
inline constexpr u32 kGlesUavSlots = 8;
inline constexpr u32 kGlesUnits = 64;
inline constexpr u32 kGlesUniformBindings = 16; // vertex 0..7, pixel 8..15
inline constexpr u32 kGlesStorageBindings = 8;
inline constexpr u32 kGlesImageBindings = 8;
inline constexpr u32 kGlesProgramCacheSize = 64;
inline constexpr u32 kGlesPackCapacity = 32; // entries of the shader pack (shader_pack.cpp asserts it)

struct GlesProgram {
    const GpuVertexShader* vertex;
    const GpuPixelShader* pixel;
    u32 name;
};

// A compute shader's program, shared by every GpuComputeShader made of it (refs of them): GL programs are not state.
struct GlesComputeProgram {
    u32 name;
    u32 refs;
};

// A timer's two timestamp queries (GL_EXT_disjoint_timer_query).
struct GpuQuery {
    u32 begin, end;
    bool issued; // end was queued and not yet read
};

// A fence: the sync objects of its signals in order, and the events armed for values not yet reached. Completion is
// noticed where the render thread calls into the backend (gles::poll_fences): GL sync objects belong to the context.
inline constexpr u32 kGlesFenceSignals = 16, kGlesFenceWaits = 16;
struct GpuFence {
    gl::Sync syncs[kGlesFenceSignals];
    u64 values[kGlesFenceSignals];
    u32 signal_count;
    u64 completed;
    u64 wait_values[kGlesFenceWaits];
    Waitable wait_events[kGlesFenceWaits];
    u32 wait_count;
    bool in_use;
};

struct GpuContext {
    // Requested state (what the caller set, in D3D11's slot vocabulary).
    GpuVertexShader* vertex_shader;
    GpuPixelShader* pixel_shader;
    GpuComputeShader* compute_shader;
    GpuInputLayout* layout;
    GpuBuffer* vertex_buffer;
    u32 vertex_stride, vertex_offset;
    GpuBuffer* constants[3][kGlesConstantSlots];
    u32 compute_range_first[kGlesConstantSlots]; // gpu_set_constants_range: 16-byte constants; count 0 = whole buffer
    u32 compute_range_count[kGlesConstantSlots];
    GpuSrv* srvs[3][kGlesResourceSlots];
    GpuSampler* samplers[3][kGlesResourceSlots];
    GpuUav* uavs[kGlesUavSlots];
    GpuRtv* target;
    GpuBlend* blend;
    float blend_factor[4];
    GpuRaster* raster;
    float viewport[4];
    GpuRect scissor;

    // What the driver has, so unchanged state costs nothing.
    u32 applied_program;
    u32 applied_vertex_array;
    u32 applied_draw_framebuffer;
    u32 applied_vertex_buffer, applied_vertex_buffer_stride;
    i64 applied_vertex_buffer_offset;
    float applied_viewport[4];
    GpuRect applied_scissor;
    bool applied_viewport_valid, applied_scissor_valid;
    const GpuBlend* applied_blend;
    bool applied_blend_valid;
    float applied_blend_factor[4];
    u32 applied_unit_texture[kGlesUnits];
    u32 applied_unit_sampler[kGlesUnits];
    u32 applied_uniform_buffer[kGlesUniformBindings * 2];
    u32 applied_storage_buffer[kGlesStorageBindings * 2];
    u32 pending_barriers;
    bool logged_incomplete_draw;
};

struct GpuDevice {
    egl::Api egl;
    gl::Api gl;
    void* egl_library;
    void* gles_library;
    egl::Display display;
    egl::Context context;
    egl::Surface surface; // the 1x1 pbuffer when the context could not be made current without a surface
    egl::Config config;   // the config of the context; window surfaces use the same one
    bool present_opaque;  // the window config has alpha and the surfaces ask the compositor to ignore it
    void* native_display; // the window system connection (wl_display*) this display was made from, or null
    GpuCaps caps;
    char description[160];
    u32 empty_vertex_array;
    u32 scratch_framebuffer;
    bool lost;
    bool debug_checks; // MAICORD_GLES_CHECK=1: a GL error is reported after the draw or clear that raised it
    bool created;
    u32 refs; // gpu_retain: owners beyond the first

    GlesSlab<GpuBuffer, 256> buffers;
    GlesSlab<GpuTexture, 128> textures;
    GlesSlab<GpuSrv, 256> srvs;
    GlesSlab<GpuUav, 64> uavs;
    GlesSlab<GpuRtv, 128> rtvs;
    GlesSlab<GpuSampler, 32> samplers;
    GlesSlab<GpuBlend, 32> blends;
    GlesSlab<GpuRaster, 4> rasters;
    GlesSlab<GpuVertexShader, 32> vertex_shaders;
    GlesSlab<GpuPixelShader, 32> pixel_shaders;
    GlesSlab<GpuComputeShader, 24> compute_shaders;
    GlesSlab<GpuInputLayout, 8> layouts;
    GlesSlab<GpuQuery, 16> queries;
    GlesSlab<GpuFence, 8> fences;
    GlesProgram programs[kGlesProgramCacheSize];
    u32 program_count;
    GlesComputeProgram compute_programs[kGlesPackCapacity]; // by shader pack entry
    // A shader or program was built since the last present: the driver's compiler (llvmpipe's LLVM, Mesa's NIR) left
    // freed memory in the C library's heap, which the present hands back (heap_trim).
    bool trim_pending;
};

// What one present changed: the whole frame, or the bounding box of its dirty rectangles.
struct GlesDamage {
    bool full;
    GpuRect box;
};
inline constexpr u32 kGlesDamageHistory = 3; // buffer ages up to history + 1 blit only the damage they missed

// The window surface (behind the caller's GpuSurface::impl): an EGL surface on the window's wl_egl_window and the
// offscreen frame the renderer draws into.
struct GlesSurface {
    void* window;       // wl_egl_window*
    egl::Surface surface;
    GpuTexture* frame;
    GpuRtv* own_target; // the frame's render target, what a present blits
    GpuRtv* target;     // what gpu_surface_target answers: own_target unless gpu_surface_set_target moved it
    u32 width, height;
    i32 offset_x, offset_y;
    GlesDamage history[kGlesDamageHistory]; // newest first
    u32 history_count;
    Waitable clock;      // the frame permit
    i32 swap_interval;   // applied, -1 unknown
    bool needs_full;     // the next present blits the whole frame
    bool resized;        // the window surface must be revalidated at its new size before the next blit
    bool created;
};

namespace gles {

GpuDevice& device() noexcept;
GpuContext& context() noexcept;
inline gl::Api& api() noexcept {
    return device().gl;
}

// The OpenGL ES entry points and EGL: opens libEGL.so.1 and libGLESv2.so.2 and resolves the tables.
GpuStatus load_libraries(GpuDevice* device) noexcept;
void unload_libraries(GpuDevice* device) noexcept;

// True when the driver lists the extension (GL_ or EGL_ prefixed, as the driver spells it).
bool has_extension(const gl::Api& api, const char* name) noexcept;
bool string_has_word(const char* list, const char* word) noexcept;

// Logs an unexpected GL error from `where` and returns the status for it; kGpuOk when there was none.
GpuStatus check_error(const char* where) noexcept;

// Applies the barriers a dispatch left pending.
void flush_barriers() noexcept;

// Formats.
struct FormatInfo {
    gl::Enum internal;
    gl::Enum format;
    gl::Enum type;
    u32 stored_texel_bytes; // bytes per texel in storage and in uploads to it
};
bool texture_format(const GpuDevice& device, GpuFormat format, u8 bind, FormatInfo* info, bool* wide) noexcept;

// The framebuffer object of a texture, created on first use; 0 when it cannot be complete.
u32 texture_framebuffer(GpuTexture* texture) noexcept;

// Shader pack (shader_pack.cpp).
GpuStatus create_shader(GpuDevice* device, ShaderId id, gl::Enum stage, GlesShader* shader) noexcept;
GpuStatus create_compute_program(GpuDevice* device, ShaderId id, GlesShader* shader) noexcept;
u32 graphics_program(GpuContext* context) noexcept;
// Waits for the prestart worker, if one still runs, and makes the context current on the calling thread.
void claim_context() noexcept;
// Looks for the fences whose signals the GPU has passed and wakes the events armed for them.
void poll_fences() noexcept;

} // namespace gles
} // namespace maicord
