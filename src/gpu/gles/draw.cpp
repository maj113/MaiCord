#include "maicord/core/log_event.hpp"
#include "gpu/gles/internal.hpp"

namespace maicord {
namespace {

using namespace gles;

constexpr u32 kUnknown = 0xFFFFFFFFu;
constexpr u32 kVertexStage = 0, kPixelStage = 1, kComputeStage = 2;

constexpr u32 stage_index(GpuStage stage) noexcept {
    static_assert(static_cast<u32>(GpuStage::vertex) == kVertexStage && static_cast<u32>(GpuStage::pixel) == kPixelStage &&
                  static_cast<u32>(GpuStage::compute) == kComputeStage);
    return static_cast<u32>(stage);
}

void bind_unit(GpuContext& context, u32 unit, u32 texture, u32 sampler) noexcept {
    if (unit >= kGlesUnits)
        return;
    gl::Api& gl = api();
    if (context.applied_unit_texture[unit] != texture) {
        gl.glActiveTexture(gl::kTexture0 + unit);
        gl.glBindTexture(gl::kTexture2d, texture);
        context.applied_unit_texture[unit] = texture;
    }
    if (context.applied_unit_sampler[unit] != sampler) {
        gl.glBindSampler(unit, sampler);
        context.applied_unit_sampler[unit] = sampler;
    }
}

void bind_storage(GpuContext& context, u32 binding, u32 buffer) noexcept {
    if (binding >= kGlesStorageBindings * 2)
        return;
    if (context.applied_storage_buffer[binding] != buffer) {
        api().glBindBufferBase(gl::kShaderStorageBuffer, binding, buffer);
        context.applied_storage_buffer[binding] = buffer;
    }
}

// Binds what the shader's manifest names from the slots the caller filled for this stage.
void bind_resources(GpuContext& context, const GlesShader& shader, u32 stage) noexcept {
    gl::Api& gl = api();
    for (u32 index = 0; index < shader.row_count; ++index) {
        const GlesBinding& row = shader.rows[index];
        switch (static_cast<GlesBindingKind>(row.kind)) {
        case GlesBindingKind::uniform_buffer: {
            const GpuBuffer* buffer = row.slot < kGlesConstantSlots ? context.constants[stage][row.slot] : nullptr;
            const u32 name = buffer ? buffer->name : 0;
            if (row.binding >= kGlesUniformBindings * 2)
                break;
            if (context.applied_uniform_buffer[row.binding] != name) {
                gl.glBindBufferBase(gl::kUniformBuffer, row.binding, name);
                context.applied_uniform_buffer[row.binding] = name;
            }
            break;
        }
        case GlesBindingKind::texture: {
            const GpuSrv* view = row.slot < kGlesResourceSlots ? context.srvs[stage][row.slot] : nullptr;
            const GpuSampler* sampler = row.sampler != kGlesNoSampler && row.sampler < kGlesResourceSlots
                                            ? context.samplers[stage][row.sampler]
                                            : nullptr;
            const GpuTexture* texture = view && view->kind == GpuViewKind::texture ? view->texture : nullptr;
            u32 sampler_name = sampler ? sampler->name : 0;
            // Float32 texels are not filtered on a device without float_linear: the sampler's point twin.
            if (sampler && sampler->nearest_name && texture && texture->format == GpuFormat::rgba32f)
                sampler_name = sampler->nearest_name;
            bind_unit(context, row.binding, texture ? texture->name : 0, sampler_name);
            break;
        }
        case GlesBindingKind::storage_srv: {
            const GpuSrv* view = row.slot < kGlesResourceSlots ? context.srvs[stage][row.slot] : nullptr;
            bind_storage(context, row.binding, view && view->kind != GpuViewKind::texture ? view->buffer->name : 0);
            break;
        }
        case GlesBindingKind::storage_uav: {
            const GpuUav* view = row.slot < kGlesUavSlots ? context.uavs[row.slot] : nullptr;
            bind_storage(context, row.binding, view && view->buffer ? view->buffer->name : 0);
            break;
        }
        case GlesBindingKind::image: {
            const GpuUav* view = row.slot < kGlesUavSlots ? context.uavs[row.slot] : nullptr;
            if (view && view->texture && row.binding < kGlesImageBindings)
                gl.glBindImageTexture(row.binding, view->texture->name, 0, gl::kFalse, 0, gl::kReadWrite,
                                      view->texture->internal);
            break;
        }
        }
    }
}

bool apply_graphics(GpuContext& context, u32 first_instance) noexcept {
    GpuDevice& d = gles::device();
    gl::Api& gl = d.gl;
    if (!context.vertex_shader || !context.pixel_shader || !context.target) {
        if (!context.logged_incomplete_draw) {
            context.logged_incomplete_draw = true;
            log_write(LogLevel::error, "gpu", "a draw without a vertex shader, a pixel shader or a render target");
        }
        return false;
    }
    const u32 program = graphics_program(&context);
    if (!program)
        return false;
    const u32 framebuffer = texture_framebuffer(context.target->texture);
    if (!framebuffer)
        return false;
    flush_barriers();
    if (context.applied_draw_framebuffer != framebuffer) {
        gl.glBindFramebuffer(gl::kFramebuffer, framebuffer);
        context.applied_draw_framebuffer = framebuffer;
    }
    if (!context.applied_viewport_valid || !bytes_equal(context.viewport, context.applied_viewport, sizeof(float) * 4)) {
        gl.glViewport(static_cast<gl::Int>(context.viewport[0] + 0.5f), static_cast<gl::Int>(context.viewport[1] + 0.5f),
                      static_cast<gl::Sizei>(context.viewport[2] + 0.5f),
                      static_cast<gl::Sizei>(context.viewport[3] + 0.5f));
        copy_bytes(context.applied_viewport, context.viewport, sizeof(float) * 4);
        context.applied_viewport_valid = true;
    }
    if (!context.applied_scissor_valid || !bytes_equal(&context.scissor, &context.applied_scissor, sizeof(GpuRect))) {
        const gl::Sizei width = context.scissor.right > context.scissor.left ? context.scissor.right - context.scissor.left : 0;
        const gl::Sizei height = context.scissor.bottom > context.scissor.top ? context.scissor.bottom - context.scissor.top : 0;
        gl.glScissor(context.scissor.left, context.scissor.top, width, height);
        context.applied_scissor = context.scissor;
        context.applied_scissor_valid = true;
    }
    if (!context.applied_blend_valid || context.applied_blend != context.blend ||
        (context.blend && context.blend->desc.enable &&
         !bytes_equal(context.blend_factor, context.applied_blend_factor, sizeof(float) * 4))) {
        if (context.blend && context.blend->desc.enable) {
            gl::Enum source, destination, source_alpha, destination_alpha;
            blend_factors(context.blend->desc, &source, &destination, &source_alpha, &destination_alpha);
            gl.glEnable(gl::kBlend);
            gl.glBlendEquationSeparate(gl::kFuncAdd, gl::kFuncAdd);
            gl.glBlendFuncSeparate(source, destination, source_alpha, destination_alpha);
            gl.glBlendColor(context.blend_factor[0], context.blend_factor[1], context.blend_factor[2],
                            context.blend_factor[3]);
            copy_bytes(context.applied_blend_factor, context.blend_factor, sizeof(float) * 4);
        } else {
            gl.glDisable(gl::kBlend);
        }
        context.applied_blend = context.blend;
        context.applied_blend_valid = true;
    }
    if (context.applied_program != program) {
        gl.glUseProgram(program);
        context.applied_program = program;
    }
    const u32 vertex_array = context.layout ? context.layout->vertex_array : d.empty_vertex_array;
    if (context.applied_vertex_array != vertex_array) {
        gl.glBindVertexArray(vertex_array);
        context.applied_vertex_array = vertex_array;
        context.applied_vertex_buffer = kUnknown;
    }
    if (context.layout && context.vertex_buffer) {
        const i64 offset = static_cast<i64>(context.vertex_offset) + static_cast<i64>(first_instance) * context.vertex_stride;
        if (context.applied_vertex_buffer != context.vertex_buffer->name || context.applied_vertex_buffer_offset != offset ||
            context.applied_vertex_buffer_stride != context.vertex_stride) {
            gl.glBindVertexBuffer(0, context.vertex_buffer->name, offset, static_cast<gl::Sizei>(context.vertex_stride));
            context.applied_vertex_buffer = context.vertex_buffer->name;
            context.applied_vertex_buffer_offset = offset;
            context.applied_vertex_buffer_stride = context.vertex_stride;
        }
    }
    bind_resources(context, *context.vertex_shader, kVertexStage);
    bind_resources(context, *context.pixel_shader, kPixelStage);
    return true;
}
} // namespace

namespace gles {

u32 graphics_program(GpuContext* context) noexcept {
    GpuDevice& d = device();
    gl::Api& gl = d.gl;
    const GpuVertexShader* vertex = context->vertex_shader;
    const GpuPixelShader* pixel = context->pixel_shader;
    for (u32 index = 0; index < d.program_count; ++index)
        if (d.programs[index].vertex == vertex && d.programs[index].pixel == pixel)
            return d.programs[index].name;
    GlesProgram entry{vertex, pixel, 0};
    const u32 program = gl.glCreateProgram();
    gl.glAttachShader(program, vertex->name);
    gl.glAttachShader(program, pixel->name);
    gl.glLinkProgram(program);
    d.trim_pending = true;
    gl::Int linked = 0;
    gl.glGetProgramiv(program, gl::kLinkStatus, &linked);
    if (linked) {
        entry.name = program;
    } else {
        char log[512];
        gl::Sizei length = 0;
        gl.glGetProgramInfoLog(program, sizeof(log) - 1, &length, log);
        log[length < 0 ? 0 : length] = 0;
        LogEvent("shader_pair_link_failed")
            .field("vertex_slot", static_cast<u64>(vertex - d.vertex_shaders.items))
            .field("pixel_slot", static_cast<u64>(pixel - d.pixel_shaders.items))
            .quoted("log", log)
            .write(LogLevel::error, "gpu");
        gl.glDeleteProgram(program);
    }
    // A failed pair is remembered with name 0 so it is not relinked on every draw.
    if (d.program_count == kGlesProgramCacheSize) {
        gl.glDeleteProgram(d.programs[0].name);
        for (u32 index = 1; index < kGlesProgramCacheSize; ++index)
            d.programs[index - 1] = d.programs[index];
        --d.program_count;
        context->applied_program = 0xFFFFFFFFu;
    }
    d.programs[d.program_count++] = entry;
    return entry.name;
}

} // namespace gles

void gpu_set_render_target(GpuContext* context, GpuRtv* target) {
    context->target = target;
}

void gpu_set_viewport(GpuContext* context, float x, float y, float width, float height) {
    context->viewport[0] = x;
    context->viewport[1] = y;
    context->viewport[2] = width;
    context->viewport[3] = height;
}

void gpu_set_scissor(GpuContext* context, const GpuRect& rectangle) {
    context->scissor = rectangle;
}

// Solid fill, no culling and the scissor test are the only rasterizer state; the device sets them once.
void gpu_set_raster(GpuContext*, GpuRaster*) {}

void gpu_set_blend(GpuContext* context, GpuBlend* blend, const float factor[4]) {
    context->blend = blend;
    for (u32 index = 0; index < 4; ++index)
        context->blend_factor[index] = factor ? factor[index] : 0.0f;
}

void gpu_set_input_layout(GpuContext* context, GpuInputLayout* layout) {
    context->layout = layout;
}

void gpu_set_vertex_buffer(GpuContext* context, GpuBuffer* buffer, u32 stride, u32 offset) {
    context->vertex_buffer = buffer;
    context->vertex_stride = stride;
    context->vertex_offset = offset;
}

void gpu_set_shader(GpuContext* context, GpuVertexShader* shader) {
    context->vertex_shader = shader;
}

void gpu_set_shader(GpuContext* context, GpuPixelShader* shader) {
    context->pixel_shader = shader;
}

void gpu_set_shader(GpuContext* context, GpuComputeShader* shader) {
    context->compute_shader = shader;
}

void gpu_set_constants(GpuContext* context, GpuStage stage, u32 first, u32 count, GpuBuffer* const* buffers) {
    for (u32 index = 0; index < count && first + index < kGlesConstantSlots; ++index) {
        context->constants[stage_index(stage)][first + index] = buffers[index];
    }
}

void gpu_set_strip_topology(GpuContext*) {}

void gpu_set_srvs(GpuContext* context, GpuStage stage, u32 first, u32 count, GpuSrv* const* views) {
    for (u32 index = 0; index < count && first + index < kGlesResourceSlots; ++index)
        context->srvs[stage_index(stage)][first + index] = views[index];
}

void gpu_set_samplers(GpuContext* context, GpuStage stage, u32 first, u32 count, GpuSampler* const* samplers) {
    for (u32 index = 0; index < count && first + index < kGlesResourceSlots; ++index)
        context->samplers[stage_index(stage)][first + index] = samplers[index];
}

void gpu_set_uavs(GpuContext* context, u32 first, u32 count, GpuUav* const* views) {
    for (u32 index = 0; index < count && first + index < kGlesUavSlots; ++index)
        context->uavs[first + index] = views[index];
}

void gpu_draw(GpuContext* context, u32 vertices, u32 first_vertex) {
    if (!apply_graphics(*context, 0))
        return;
    api().glDrawArrays(gl::kTriangleStrip, static_cast<gl::Int>(first_vertex), static_cast<gl::Sizei>(vertices));
}

void gpu_draw_instanced(GpuContext* context, u32 vertices, u32 instances, u32 first_vertex, u32 first_instance) {
    // ES 3.1 has no base instance: apply_graphics moves the vertex buffer binding by first_instance records, which
    // leaves SV_InstanceID counting from zero in every API.
    if (!apply_graphics(*context, first_instance))
        return;
    api().glDrawArraysInstanced(gl::kTriangleStrip, static_cast<gl::Int>(first_vertex),
                                static_cast<gl::Sizei>(vertices), static_cast<gl::Sizei>(instances));
}

void gpu_dispatch(GpuContext* context, u32 x, u32 y, u32 z) {
    if (!context->compute_shader)
        return;
    gl::Api& gl = api();
    flush_barriers();
    const u32 program = context->compute_shader->name;
    if (context->applied_program != program) {
        gl.glUseProgram(program);
        context->applied_program = program;
    }
    bind_resources(*context, *context->compute_shader, kComputeStage);
    gl.glDispatchCompute(x, y, z);
    // D3D11 makes a dispatch's writes visible by itself; here the next draw, dispatch, copy or readback pays.
    context->pending_barriers = gl::kAllBarrierBits;
}

} // namespace maicord
