#include "maicord/core/heap.hpp"
#include "maicord/core/log_event.hpp"
#include "gpu/gles/internal.hpp"
#include "gpu/gles/video_planes_internal.hpp"

namespace maicord {
namespace {

using namespace gles;

constexpr u32 kUnknown = 0xFFFFFFFFu;

template <typename T, u32 N>
void forget_pointer(T* (&array)[N], const T* released) noexcept {
    for (u32 index = 0; index < N; ++index)
        if (array[index] == released)
            array[index] = nullptr;
}

template <typename T, u32 N>
void forget_name(T (&array)[N], T released) noexcept {
    for (u32 index = 0; index < N; ++index)
        if (array[index] == released)
            array[index] = static_cast<T>(kUnknown);
}

void bind_framebuffer(GpuContext& context, u32 name) noexcept {
    if (context.applied_draw_framebuffer != name) {
        api().glBindFramebuffer(gl::kFramebuffer, name);
        context.applied_draw_framebuffer = name;
    }
}

// Texture unit 0 is the scratch unit of creation and uploads; draws rebind what they use.
void bind_scratch_texture(GpuContext& context, u32 name) noexcept {
    api().glActiveTexture(gl::kTexture0);
    api().glBindTexture(gl::kTexture2d, name);
    context.applied_unit_texture[0] = kUnknown;
}

gl::Enum blend_color_factor(GpuBlendFactor factor) noexcept {
    switch (factor) {
    case GpuBlendFactor::zero:
        return gl::kZero;
    case GpuBlendFactor::one:
        return gl::kOne;
    case GpuBlendFactor::src_color:
        return gl::kSrcColor;
    case GpuBlendFactor::inv_src_color:
        return gl::kOneMinusSrcColor;
    case GpuBlendFactor::src_alpha:
        return gl::kSrcAlpha;
    case GpuBlendFactor::inv_src_alpha:
        return gl::kOneMinusSrcAlpha;
    case GpuBlendFactor::blend_factor:
        return gl::kConstantColor;
    case GpuBlendFactor::src1_color:
        return gl::kSrc1Color;
    }
    return gl::kOne;
}

} // namespace

namespace gles {

// The GL enums of a blend description, for the draw path.
void blend_factors(const GpuBlendDesc& desc, gl::Enum* out_source, gl::Enum* out_destination,
                   gl::Enum* out_source_alpha, gl::Enum* out_destination_alpha) noexcept {
    *out_source = blend_color_factor(desc.source);
    *out_destination = blend_color_factor(desc.destination);
    // Alpha factors name the constant's alpha, not its colour, as D3D11's alpha blend does.
    auto alpha = [](GpuBlendFactor factor) noexcept {
        return factor == GpuBlendFactor::blend_factor ? gl::kConstantAlpha : blend_color_factor(factor);
    };
    *out_source_alpha = alpha(desc.source_alpha);
    *out_destination_alpha = alpha(desc.destination_alpha);
}

} // namespace gles

GpuStatus gpu_buffer_create(GpuDevice* device, const GpuBufferDesc& desc, const void* initial, GpuBuffer** buffer) {
    *buffer = nullptr;
    if (desc.bytes == 0 || ((desc.bind & gpu_bind_constant) && (desc.bind & ~gpu_bind_constant) != 0))
        return kGpuInvalidArgument;
    if ((desc.bind & gpu_bind_constant) && desc.bytes > device->caps.max_uniform_block_bytes) {
        LogEvent("constant_buffer_over_uniform_block_limit")
            .field("bytes", desc.bytes)
            .field("limit", device->caps.max_uniform_block_bytes)
            .write(LogLevel::error, "gpu");
        return kGpuUnsupported;
    }
    if ((desc.raw || (desc.bind & (gpu_bind_srv | gpu_bind_uav))) && (desc.bytes & 3u) != 0)
        return kGpuInvalidArgument;
    if (desc.usage == GpuUsage::immutable && !initial)
        return kGpuInvalidArgument;
    GpuBuffer* result = device->buffers.allocate();
    if (!result)
        return kGpuOutOfMemory;
    gl::Api& gl = device->gl;
    gl.glGenBuffers(1, &result->name);
    gl.glBindBuffer(gl::kCopyWriteBuffer, result->name);
    gl.glBufferData(gl::kCopyWriteBuffer, desc.bytes, initial,
                    desc.usage == GpuUsage::dynamic ? gl::kDynamicDraw : gl::kStaticDraw);
    const GpuStatus status = check_error("buffer creation");
    if (!gpu_ok(status)) {
        gl.glDeleteBuffers(1, &result->name);
        device->buffers.release(result);
        return status;
    }
    result->bytes = desc.bytes;
    result->usage = desc.usage;
    result->bind = desc.bind;
    result->raw = desc.raw;
    result->refs = 1;
    *buffer = result;
    return kGpuOk;
}

GpuStatus gpu_texture_create(GpuDevice* device, const GpuTextureDesc& desc, const GpuInitialData* initial,
                             GpuTexture** texture) {
    *texture = nullptr;
    if (desc.width == 0 || desc.height == 0 || desc.width > device->caps.max_texture_size ||
        desc.height > device->caps.max_texture_size || desc.usage == GpuUsage::dynamic ||
        (desc.usage == GpuUsage::immutable && !initial) || (initial && !initial->data))
        return kGpuInvalidArgument;
    if (desc.format == GpuFormat::bgra8 && (desc.bind & (gpu_bind_rtv | gpu_bind_uav)))
        return kGpuUnsupported; // BGRA is a sampled-only format here: a swizzle cannot apply on write
    FormatInfo info{};
    bool wide = false;
    if (!texture_format(*device, desc.format, desc.bind, &info, &wide))
        return kGpuUnsupported;
    GpuTexture* result = device->textures.allocate();
    if (!result)
        return kGpuOutOfMemory;
    gl::Api& gl = device->gl;
    GpuContext& context = gles::context();
    gl.glGenTextures(1, &result->name);
    bind_scratch_texture(context, result->name);
    gl.glTexStorage2D(gl::kTexture2d, 1, info.internal, static_cast<gl::Sizei>(desc.width),
                      static_cast<gl::Sizei>(desc.height));
    gl.glTexParameteri(gl::kTexture2d, gl::kTextureMinFilter, gl::kNearest);
    gl.glTexParameteri(gl::kTexture2d, gl::kTextureMagFilter, gl::kNearest);
    gl.glTexParameteri(gl::kTexture2d, gl::kTextureWrapS, gl::kClampToEdge);
    gl.glTexParameteri(gl::kTexture2d, gl::kTextureWrapT, gl::kClampToEdge);
    gl.glTexParameteri(gl::kTexture2d, gl::kTextureBaseLevel, 0);
    gl.glTexParameteri(gl::kTexture2d, gl::kTextureMaxLevel, 0);
    if (desc.format == GpuFormat::bgra8) {
        gl.glTexParameteri(gl::kTexture2d, gl::kTextureSwizzleR, static_cast<gl::Int>(gl::kBlue));
        gl.glTexParameteri(gl::kTexture2d, gl::kTextureSwizzleB, static_cast<gl::Int>(gl::kRed));
    }
    GpuStatus status = kGpuOk;
    if (initial) {
        const u8* rows = static_cast<const u8*>(initial->data);
        u8* expanded = nullptr;
        u32 pitch = initial->pitch;
        if (wide) {
            expanded = static_cast<u8*>(heap_allocate(usize{desc.width} * desc.height * 4));
            if (!expanded) {
                status = kGpuOutOfMemory;
            } else {
                for (u32 y = 0; y < desc.height; ++y) {
                    u8* out = expanded + usize{y} * desc.width * 4;
                    const u8* in = rows + usize{y} * pitch;
                    for (u32 x = 0; x < desc.width; ++x) {
                        out[x * 4 + 0] = out[x * 4 + 1] = out[x * 4 + 2] = out[x * 4 + 3] = in[x];
                    }
                }
                rows = expanded;
                pitch = desc.width * 4;
            }
        }
        if (gpu_ok(status)) {
            const u32 stored = info.stored_texel_bytes;
            if (pitch % stored == 0) {
                gl.glPixelStorei(gl::kUnpackRowLength, static_cast<gl::Int>(pitch / stored));
                gl.glTexSubImage2D(gl::kTexture2d, 0, 0, 0, static_cast<gl::Sizei>(desc.width),
                                   static_cast<gl::Sizei>(desc.height), info.format, info.type, rows);
                gl.glPixelStorei(gl::kUnpackRowLength, 0);
            } else {
                for (u32 y = 0; y < desc.height; ++y)
                    gl.glTexSubImage2D(gl::kTexture2d, 0, 0, static_cast<gl::Int>(y),
                                       static_cast<gl::Sizei>(desc.width), 1, info.format, info.type,
                                       rows + usize{y} * pitch);
            }
        }
        if (expanded)
            heap_free(expanded);
    }
    if (gpu_ok(status))
        status = check_error("texture creation");
    if (!gpu_ok(status)) {
        gl.glDeleteTextures(1, &result->name);
        device->textures.release(result);
        return status;
    }
    result->width = desc.width;
    result->height = desc.height;
    result->internal = info.internal;
    result->format = desc.format;
    result->bind = desc.bind;
    result->refs = 1;
    *texture = result;
    return kGpuOk;
}

GpuStatus gpu_format_check(GpuDevice* device, GpuFormat format, u8 bind) {
    if (format == GpuFormat::bgra8 && (bind & (gpu_bind_rtv | gpu_bind_uav)))
        return kGpuUnsupported;
    FormatInfo info{};
    bool wide = false;
    return texture_format(*device, format, bind, &info, &wide) ? kGpuOk : kGpuUnsupported;
}

GpuStatus gpu_srv_create(GpuDevice* device, GpuTexture* texture, const GpuViewDesc& desc, GpuSrv** view) {
    *view = nullptr;
    if (!texture || desc.kind != GpuViewKind::texture || !(texture->bind & gpu_bind_srv))
        return kGpuInvalidArgument;
    GpuSrv* result = device->srvs.allocate();
    if (!result)
        return kGpuOutOfMemory;
    result->kind = GpuViewKind::texture;
    result->texture = texture;
    ++texture->refs;
    *view = result;
    return kGpuOk;
}

GpuStatus gpu_srv_create(GpuDevice* device, GpuBuffer* buffer, const GpuViewDesc& desc, GpuSrv** view) {
    *view = nullptr;
    if (!buffer || !(buffer->bind & gpu_bind_srv) || desc.kind == GpuViewKind::texture ||
        (desc.kind == GpuViewKind::buffer_raw && !buffer->raw) ||
        (desc.kind == GpuViewKind::buffer_typed && desc.format != GpuFormat::r32_uint))
        return kGpuInvalidArgument;
    GpuSrv* result = device->srvs.allocate();
    if (!result)
        return kGpuOutOfMemory;
    result->kind = desc.kind;
    result->buffer = buffer;
    ++buffer->refs;
    *view = result;
    return kGpuOk;
}

GpuStatus gpu_uav_create(GpuDevice* device, GpuTexture* texture, GpuUav** view) {
    *view = nullptr;
    if (!texture || !(texture->bind & gpu_bind_uav))
        return kGpuInvalidArgument;
    GpuUav* result = device->uavs.allocate();
    if (!result)
        return kGpuOutOfMemory;
    result->texture = texture;
    ++texture->refs;
    *view = result;
    return kGpuOk;
}

GpuStatus gpu_uav_create(GpuDevice* device, GpuBuffer* buffer, u32, GpuUav** view) {
    *view = nullptr;
    if (!buffer || !(buffer->bind & gpu_bind_uav) || !buffer->raw)
        return kGpuInvalidArgument;
    GpuUav* result = device->uavs.allocate();
    if (!result)
        return kGpuOutOfMemory;
    result->buffer = buffer;
    ++buffer->refs;
    *view = result;
    return kGpuOk;
}

GpuStatus gpu_rtv_create(GpuDevice* device, GpuTexture* texture, GpuRtv** view) {
    *view = nullptr;
    if (!texture || !(texture->bind & gpu_bind_rtv))
        return kGpuInvalidArgument;
    if (!texture_framebuffer(texture))
        return kGpuUnsupported;
    GpuRtv* result = device->rtvs.allocate();
    if (!result)
        return kGpuOutOfMemory;
    result->texture = texture;
    ++texture->refs;
    *view = result;
    return kGpuOk;
}

GpuStatus gpu_sampler_create(GpuDevice* device, const GpuSamplerDesc& desc, GpuSampler** sampler) {
    *sampler = nullptr;
    GpuSampler* result = device->samplers.allocate();
    if (!result)
        return kGpuOutOfMemory;
    gl::Api& gl = device->gl;
    gl.glGenSamplers(1, &result->name);
    const gl::Int filter = static_cast<gl::Int>(desc.filter == GpuFilter::linear ? gl::kLinear : gl::kNearest);
    const gl::Int address = static_cast<gl::Int>(desc.address == GpuAddress::wrap ? gl::kRepeat : gl::kClampToEdge);
    gl.glSamplerParameteri(result->name, gl::kTextureMinFilter, filter);
    gl.glSamplerParameteri(result->name, gl::kTextureMagFilter, filter);
    gl.glSamplerParameteri(result->name, gl::kTextureWrapS, address);
    gl.glSamplerParameteri(result->name, gl::kTextureWrapT, address);
    if (desc.filter == GpuFilter::linear && !device->caps.float_linear) {
        gl.glGenSamplers(1, &result->nearest_name);
        gl.glSamplerParameteri(result->nearest_name, gl::kTextureMinFilter, static_cast<gl::Int>(gl::kNearest));
        gl.glSamplerParameteri(result->nearest_name, gl::kTextureMagFilter, static_cast<gl::Int>(gl::kNearest));
        gl.glSamplerParameteri(result->nearest_name, gl::kTextureWrapS, address);
        gl.glSamplerParameteri(result->nearest_name, gl::kTextureWrapT, address);
    }
    const GpuStatus status = check_error("sampler creation");
    if (!gpu_ok(status)) {
        gl.glDeleteSamplers(1, &result->name);
        if (result->nearest_name)
            gl.glDeleteSamplers(1, &result->nearest_name);
        device->samplers.release(result);
        return status;
    }
    *sampler = result;
    return kGpuOk;
}

GpuStatus gpu_blend_create(GpuDevice* device, const GpuBlendDesc& desc, GpuBlend** blend) {
    *blend = nullptr;
    GpuBlend* result = device->blends.allocate();
    if (!result)
        return kGpuOutOfMemory;
    result->desc = desc;
    *blend = result;
    return kGpuOk;
}

GpuStatus gpu_raster_create(GpuDevice* device, GpuRaster** raster) {
    *raster = nullptr;
    GpuRaster* result = device->rasters.allocate();
    if (!result)
        return kGpuOutOfMemory;
    *raster = result;
    return kGpuOk;
}

// ---- Release ---------------------------------------------------------------------------------------------

namespace {

// A buffer or texture is owned by its handle, by every view made of it and by every gpu_retain; the last owner to
// let go destroys it (D3D11's views hold their resource the same way: the executors release a texture right after
// making its views).
void drop_buffer(GpuBuffer* buffer) noexcept {
    if (!buffer || --buffer->refs)
        return;
    GpuDevice& d = gles::device();
    GpuContext& c = gles::context();
    d.gl.glDeleteBuffers(1, &buffer->name);
    forget_pointer(c.constants[0], buffer);
    forget_pointer(c.constants[1], buffer);
    forget_pointer(c.constants[2], buffer);
    if (c.vertex_buffer == buffer)
        c.vertex_buffer = nullptr;
    forget_name(c.applied_uniform_buffer, buffer->name);
    forget_name(c.applied_storage_buffer, buffer->name);
    if (c.applied_vertex_buffer == buffer->name)
        c.applied_vertex_buffer = kUnknown;
    d.buffers.release(buffer);
}

void drop_texture(GpuTexture* texture) noexcept {
    if (!texture || gles::video_plane_release(texture)) // a decoded picture's plane counts atomically, in its own pool
        return;
    if (--texture->refs)
        return;
    GpuDevice& d = gles::device();
    GpuContext& c = gles::context();
    if (texture->framebuffer) {
        d.gl.glDeleteFramebuffers(1, &texture->framebuffer);
        if (c.applied_draw_framebuffer == texture->framebuffer)
            c.applied_draw_framebuffer = kUnknown;
    }
    d.gl.glDeleteTextures(1, &texture->name);
    forget_name(c.applied_unit_texture, texture->name);
    d.textures.release(texture);
}

} // namespace

void gpu_release(GpuBuffer*& handle) noexcept {
    GpuBuffer* buffer = handle;
    handle = nullptr;
    drop_buffer(buffer);
}

void gpu_release(GpuTexture*& handle) noexcept {
    GpuTexture* texture = handle;
    handle = nullptr;
    drop_texture(texture);
}

void gpu_release(GpuSrv*& handle) noexcept {
    GpuSrv* view = handle;
    handle = nullptr;
    if (!view || gles::video_plane_release(view))
        return;
    if (view->refs) {
        --view->refs;
        return;
    }
    GpuContext& c = gles::context();
    forget_pointer(c.srvs[0], view);
    forget_pointer(c.srvs[1], view);
    forget_pointer(c.srvs[2], view);
    GpuTexture* texture = view->kind == GpuViewKind::texture ? view->texture : nullptr;
    GpuBuffer* buffer = view->kind == GpuViewKind::texture ? nullptr : view->buffer;
    gles::device().srvs.release(view);
    drop_texture(texture);
    drop_buffer(buffer);
}

void gpu_release(GpuUav*& handle) noexcept {
    GpuUav* view = handle;
    handle = nullptr;
    if (!view)
        return;
    forget_pointer(gles::context().uavs, view);
    GpuTexture* texture = view->texture;
    GpuBuffer* buffer = view->buffer;
    gles::device().uavs.release(view);
    drop_texture(texture);
    drop_buffer(buffer);
}

void gpu_release(GpuRtv*& handle) noexcept {
    GpuRtv* view = handle;
    handle = nullptr;
    if (!view)
        return;
    GpuContext& c = gles::context();
    if (c.target == view)
        c.target = nullptr;
    GpuTexture* texture = view->texture;
    gles::device().rtvs.release(view);
    drop_texture(texture);
}

// Paged textures: a texture per page, made at the page's full height when it is first backed and released when it
// is returned, never resized. Creation and commits run per atlas page: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
namespace {

void release_page(GpuPagedTexture* paged, u32 page) noexcept {
    gpu_release(paged->uavs[page]);
    gpu_release(paged->views[page]);
    gpu_release(paged->textures[page]);
}

} // namespace

GpuStatus gpu_paged_texture_create(GpuDevice* device, const GpuPagedTextureDesc& desc, GpuPagedTexture** texture) {
    *texture = nullptr;
    if (!device || desc.width == 0 || desc.width > device->caps.max_texture_size || desc.page_rows == 0 ||
        desc.page_rows > device->caps.max_texture_size || desc.pages == 0 ||
        desc.pages > kGpuPagedTextureMaximumPages || (desc.bind & gpu_bind_rtv))
        return kGpuInvalidArgument;
    const GpuStatus supported = gpu_format_check(device, desc.format, desc.bind);
    if (!gpu_ok(supported))
        return supported;
    auto* paged = static_cast<GpuPagedTexture*>(heap_allocate(sizeof(GpuPagedTexture), true));
    if (!paged)
        return kGpuOutOfMemory;
    paged->view.kind = GpuViewKind::texture;
    paged->width = desc.width;
    paged->page_rows = desc.page_rows;
    paged->pages = desc.pages;
    paged->format = desc.format;
    paged->bind = desc.bind;
    *texture = paged;
    return kGpuOk;
}

void gpu_paged_texture_release(GpuPagedTexture** texture) {
    GpuPagedTexture* paged = *texture;
    if (!paged)
        return;
    GpuContext& c = gles::context();
    forget_pointer(c.srvs[0], &paged->view);
    forget_pointer(c.srvs[1], &paged->view);
    forget_pointer(c.srvs[2], &paged->view);
    for (u32 page = 0; page < paged->pages; ++page)
        release_page(paged, page);
    heap_free(paged);
    *texture = nullptr;
}

GpuStatus gpu_paged_texture_commit(GpuContext* context, GpuPagedTexture* paged, u32 page, u32 rows) {
    if (!context || !paged || page >= paged->pages || (rows != 0 && rows != paged->page_rows))
        return kGpuInvalidArgument;
    if (!rows) {
        release_page(paged, page);
        return kGpuOk;
    }
    if (paged->textures[page])
        return kGpuOk;
    GpuDevice* device = &gles::device();
    GpuStatus status = gpu_texture_create(
        device, {paged->width, paged->page_rows, paged->format, GpuUsage::default_, paged->bind}, nullptr,
        &paged->textures[page]);
    if (gpu_ok(status) && (paged->bind & gpu_bind_srv))
        status = gpu_srv_create(device, paged->textures[page], {GpuViewKind::texture, paged->format}, &paged->views[page]);
    if (gpu_ok(status) && (paged->bind & gpu_bind_uav))
        status = gpu_uav_create(device, paged->textures[page], &paged->uavs[page]);
    if (!gpu_ok(status))
        release_page(paged, page);
    return status;
}

u64 gpu_paged_texture_bytes(const GpuPagedTexture* texture) {
    u64 bytes = 0;
    for (u32 page = 0; texture && page < texture->pages; ++page)
        bytes += texture->textures[page] ? u64{texture->width} * texture->page_rows * gpu_texel_bytes(texture->format) : 0;
    return bytes;
}

GpuPage gpu_paged_texture_page(const GpuPagedTexture* texture, u32 row) {
    const u32 page = minimum(row / texture->page_rows, texture->pages - 1u);
    return GpuPage{texture->textures[page], texture->uavs[page], texture->views[page], page * texture->page_rows,
                   texture->page_rows, page};
}

bool gpu_paged_texture_backed(const GpuPagedTexture* texture, u32 row, u32 rows) {
    const u32 page = row / texture->page_rows;
    return page < texture->pages && texture->textures[page] && row + rows <= (page + 1u) * texture->page_rows;
}

GpuSrv* gpu_paged_texture_binding_view(const GpuPagedTexture* texture, u32 binding) {
    return binding < texture->pages ? texture->views[binding] : nullptr;
}

GpuSrv* gpu_paged_texture_view(const GpuPagedTexture* texture) {
    return const_cast<GpuSrv*>(&texture->view);
}
#pragma clang attribute pop

void gpu_release(GpuSampler*& handle) noexcept {
    GpuSampler* sampler = handle;
    handle = nullptr;
    if (!sampler)
        return;
    GpuDevice& d = gles::device();
    GpuContext& c = gles::context();
    d.gl.glDeleteSamplers(1, &sampler->name);
    if (sampler->nearest_name) {
        d.gl.glDeleteSamplers(1, &sampler->nearest_name);
        forget_name(c.applied_unit_sampler, sampler->nearest_name);
    }
    forget_pointer(c.samplers[0], sampler);
    forget_pointer(c.samplers[1], sampler);
    forget_pointer(c.samplers[2], sampler);
    forget_name(c.applied_unit_sampler, sampler->name);
    d.samplers.release(sampler);
}

void gpu_release(GpuBlend*& handle) noexcept {
    GpuBlend* blend = handle;
    handle = nullptr;
    if (!blend)
        return;
    GpuContext& c = gles::context();
    if (c.blend == blend)
        c.blend = nullptr;
    if (c.applied_blend == blend)
        c.applied_blend_valid = false;
    gles::device().blends.release(blend);
}

void gpu_release(GpuRaster*& handle) noexcept {
    GpuRaster* raster = handle;
    handle = nullptr;
    if (raster)
        gles::device().rasters.release(raster);
}

void gpu_release(GpuInputLayout*& handle) noexcept {
    GpuInputLayout* layout = handle;
    handle = nullptr;
    if (!layout)
        return;
    GpuDevice& d = gles::device();
    GpuContext& c = gles::context();
    d.gl.glDeleteVertexArrays(1, &layout->vertex_array);
    if (c.layout == layout)
        c.layout = nullptr;
    if (c.applied_vertex_array == layout->vertex_array)
        c.applied_vertex_array = kUnknown;
    d.layouts.release(layout);
}

// ---- Commands --------------------------------------------------------------------------------------------

void gpu_clear_rtv(GpuContext* context, GpuRtv* target, const float rgba[4]) {
    gl::Api& gl = api();
    flush_barriers();
    const u32 framebuffer = texture_framebuffer(target->texture);
    if (!framebuffer)
        return;
    bind_framebuffer(*context, framebuffer);
    // ClearRenderTargetView ignores the scissor.
    gl.glDisable(gl::kScissorTest);
    gl.glClearBufferfv(gl::kColor, 0, rgba);
    gl.glEnable(gl::kScissorTest);
}

namespace {

// A rectangle of `texture` cleared through its framebuffer, ignoring the scissor.
void clear_rect(GpuContext* context, GpuTexture* texture, const float rgba[4], const GpuRect& rectangle) noexcept {
    gl::Api& gl = api();
    flush_barriers();
    const u32 framebuffer = texture_framebuffer(texture);
    if (!framebuffer || rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top)
        return;
    bind_framebuffer(*context, framebuffer);
    gl.glScissor(rectangle.left, rectangle.top, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top);
    gl.glClearBufferfv(gl::kColor, 0, rgba);
    context->applied_scissor_valid = false;
}

} // namespace

void gpu_clear_rtv_rect(GpuContext* context, GpuRtv* target, const float rgba[4], const GpuRect& rectangle) {
    clear_rect(context, target->texture, rgba, rectangle);
}

void gpu_clear_uav_rect(GpuContext* context, GpuUav* target, const float rgba[4], const GpuRect& rectangle) {
    clear_rect(context, target->texture, rgba, rectangle);
}

void gpu_update_buffer(GpuContext* context, GpuBuffer* buffer, const void* data, u32 bytes) {
    (void)context;
    if (bytes > buffer->bytes)
        return;
    gl::Api& gl = api();
    gl.glBindBuffer(gl::kCopyWriteBuffer, buffer->name);
    gl.glBufferSubData(gl::kCopyWriteBuffer, 0, bytes, data);
}

void* gpu_map(GpuContext* context, GpuBuffer* buffer, GpuMap mode, GpuStatus* status) {
    (void)context;
    if (buffer->usage != GpuUsage::dynamic || buffer->mapped) {
        if (status)
            *status = kGpuInvalidArgument;
        return nullptr;
    }
    gl::Api& gl = api();
    gl.glBindBuffer(gl::kCopyWriteBuffer, buffer->name);
    const gl::Bitfield access = gl::kMapWriteBit | (mode == GpuMap::discard ? gl::kMapInvalidateBufferBit
                                                                            : gl::kMapUnsynchronizedBit);
    void* mapped = gl.glMapBufferRange(gl::kCopyWriteBuffer, 0, buffer->bytes, access);
    const GpuStatus result = check_error("buffer map");
    if (status)
        *status = mapped ? kGpuOk : (gpu_ok(result) ? kGpuFail : result);
    buffer->mapped = mapped != nullptr;
    return mapped;
}

void gpu_unmap(GpuContext* context, GpuBuffer* buffer) {
    (void)context;
    if (!buffer->mapped)
        return;
    gl::Api& gl = api();
    gl.glBindBuffer(gl::kCopyWriteBuffer, buffer->name);
    gl.glUnmapBuffer(gl::kCopyWriteBuffer);
    buffer->mapped = false;
}

void gpu_copy_region(GpuContext*, GpuTexture* destination, u32 x, u32 y, GpuTexture* source, const GpuBox& box) {
    const u32 width = box.right - box.left, height = box.bottom - box.top;
    if (width == 0 || height == 0)
        return;
    flush_barriers();
    api().glCopyImageSubData(source->name, gl::kTexture2d, 0, static_cast<gl::Int>(box.left),
                             static_cast<gl::Int>(box.top), 0, destination->name, gl::kTexture2d, 0,
                             static_cast<gl::Int>(x), static_cast<gl::Int>(y), 0, static_cast<gl::Sizei>(width),
                             static_cast<gl::Sizei>(height), 1);
    check_error("texture copy");
}

GpuStatus gpu_readback(GpuDevice* device, GpuContext* context, GpuTexture* source, const GpuBox& box,
                       void* destination, u32 pitch) {
    const u32 width = box.right - box.left, height = box.bottom - box.top;
    if (width == 0 || height == 0 || box.right > source->width || box.bottom > source->height)
        return kGpuInvalidArgument;
    FormatInfo info{};
    bool wide = false;
    if (!texture_format(*device, source->format, source->bind, &info, &wide))
        return kGpuUnsupported;
    const u32 texel = gpu_texel_bytes(source->format);
    if (pitch < width * texel)
        return kGpuInvalidArgument;
    gl::Api& gl = device->gl;
    flush_barriers();
    const u32 framebuffer = texture_framebuffer(source);
    if (!framebuffer)
        return kGpuUnsupported;
    bind_framebuffer(*context, framebuffer);
    gl.glReadBuffer(gl::kColorAttachment0);
    u8* out = static_cast<u8*>(destination);
    GpuStatus status;
    if (wide || (pitch % info.stored_texel_bytes) != 0) {
        // Read tightly into scratch, then lay the rows out (and take the red channel of a wide r8).
        const u32 stored_row = width * info.stored_texel_bytes;
        u8* scratch = static_cast<u8*>(heap_allocate(usize{stored_row} * height));
        if (!scratch)
            return kGpuOutOfMemory;
        gl.glReadPixels(static_cast<gl::Int>(box.left), static_cast<gl::Int>(box.top), static_cast<gl::Sizei>(width),
                        static_cast<gl::Sizei>(height), info.format, info.type, scratch);
        status = check_error("texture readback");
        if (gpu_ok(status)) {
            for (u32 row = 0; row < height; ++row) {
                const u8* in = scratch + usize{row} * stored_row;
                u8* line = out + usize{row} * pitch;
                if (wide) {
                    for (u32 column = 0; column < width; ++column)
                        line[column] = in[column * 4];
                } else {
                    copy_bytes(line, in, width * texel);
                }
            }
        }
        heap_free(scratch);
    } else {
        gl.glPixelStorei(gl::kPackRowLength, static_cast<gl::Int>(pitch / info.stored_texel_bytes));
        gl.glReadPixels(static_cast<gl::Int>(box.left), static_cast<gl::Int>(box.top), static_cast<gl::Sizei>(width),
                        static_cast<gl::Sizei>(height), info.format, info.type, destination);
        gl.glPixelStorei(gl::kPackRowLength, 0);
        status = check_error("texture readback");
    }
    return status;
}

GpuStatus gpu_readback_map(GpuDevice* device, GpuContext* context, GpuTexture* source, GpuFormat, const GpuBox& box,
                           GpuReadback* readback) {
    const u32 width = box.right - box.left, height = box.bottom - box.top;
    if (width == 0 || height == 0 || box.right > source->width || box.bottom > source->height)
        return kGpuInvalidArgument;
    const u32 pitch = width * gpu_texel_bytes(source->format);
    u8* rows = static_cast<u8*>(heap_allocate(usize{pitch} * height));
    if (!rows)
        return kGpuOutOfMemory;
    const GpuStatus status = gpu_readback(device, context, source, box, rows, pitch);
    if (!gpu_ok(status)) {
        heap_free(rows);
        return status;
    }
    readback->data = rows;
    readback->pitch = pitch;
    readback->format = source->format;
    readback->backend = rows;
    return kGpuOk;
}

void gpu_readback_unmap(GpuContext*, GpuReadback* readback) {
    heap_free(readback->backend);
    readback->backend = nullptr;
    readback->data = nullptr;
}

void gpu_flush(GpuContext* context) {
    (void)context;
    api().glFlush();
    poll_fences();
}

void gpu_clear_state(GpuContext* context) {
    gl::Api& gl = api();
    clear_bytes(context->constants, static_cast<u32>(sizeof(context->constants)));
    clear_bytes(context->srvs, static_cast<u32>(sizeof(context->srvs)));
    clear_bytes(context->samplers, static_cast<u32>(sizeof(context->samplers)));
    clear_bytes(context->uavs, static_cast<u32>(sizeof(context->uavs)));
    context->vertex_shader = nullptr;
    context->pixel_shader = nullptr;
    context->compute_shader = nullptr;
    context->layout = nullptr;
    context->vertex_buffer = nullptr;
    context->target = nullptr;
    context->blend = nullptr;
    gl.glBindFramebuffer(gl::kFramebuffer, 0);
    gl.glUseProgram(0);
    gl.glBindVertexArray(0);
    context->applied_draw_framebuffer = 0;
    context->applied_program = 0;
    context->applied_vertex_array = 0;
    context->applied_vertex_buffer = kUnknown;
    context->applied_viewport_valid = false;
    context->applied_scissor_valid = false;
    context->applied_blend_valid = false;
    for (u32 unit = 0; unit < kGlesUnits; ++unit) {
        context->applied_unit_texture[unit] = kUnknown;
        context->applied_unit_sampler[unit] = kUnknown;
    }
    for (u32 index = 0; index < kGlesUniformBindings * 2; ++index)
        context->applied_uniform_buffer[index] = kUnknown;
    for (u32 index = 0; index < kGlesStorageBindings * 2; ++index)
        context->applied_storage_buffer[index] = kUnknown;
    gl.glFlush();
}

} // namespace maicord
