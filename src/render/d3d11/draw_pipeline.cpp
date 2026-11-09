#include "maicord/render/d3d11/draw_pipeline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/theme/background_presentation.hpp"

#include "maicord/gpu/api.hpp"

namespace maicord {
namespace {

struct FrameTransform {
    float scale_x;
    float scale_y;
    float translation_x;
    float translation_y;
    float group_opacity;
    float backdrop_gradient_axis[3];
};

static_assert(sizeof(FrameTransform) == kD3D11UiFrameConstantBytes);

constexpr D3D11DrawPipelineResult pipeline_success() noexcept {
    return D3D11DrawPipelineResult{S_OK, D3D11DrawPipelineError::none};
}

constexpr D3D11DrawPipelineResult pipeline_failure(D3D11DrawPipelineError error,
                                                   HRESULT native_error) noexcept {
    return D3D11DrawPipelineResult{native_error, error};
}

bool valid_buffer_dimensions(u32 stride, u32 capacity) noexcept {
    return stride != 0 && capacity != 0 && capacity <= ~0u / stride;
}

GpuStatus create_instance_buffer(GpuDevice* device, u32 stride, u32 capacity, GpuBuffer** buffer) noexcept {
    return gpu_buffer_create(device, {stride * capacity, GpuUsage::dynamic, gpu_bind_vertex, false}, nullptr, buffer);
}

D3D11DrawPipelineError create_pipeline_resources(D3D11DrawPipelineOwner* owner, GpuDevice* device,
                                                 const D3D11DrawPipelineConfig& config,
                                                 GpuStatus* native_error) noexcept {
    D3D11DrawPipelineError failure = D3D11DrawPipelineError::ui_vertex_shader;
    GpuStatus result = kGpuOk;
    bool layout_failed = false;
    do {
        const GpuInputElement ui_elements[] = {
            {"RECT", 0, 0},
            {"COLOR", 0, 16},
            {"COLOR", 1, 32},
            {"TEXCOORD", 0, 48},
        };
        result = gpu_vertex_shader_create(device, ShaderId::ui_vertex, {ui_elements, 4}, &owner->ui_vertex_shader,
                                          &owner->ui_input_layout, &layout_failed);
        if (FAILED(result)) {
            if (layout_failed)
                failure = D3D11DrawPipelineError::ui_input_layout;
            break;
        }
        failure = D3D11DrawPipelineError::ui_pixel_shader;
        result = gpu_pixel_shader_create(device, ShaderId::ui_pixel, &owner->ui_pixel_shader);
        if (FAILED(result)) {
            break;
        }
        failure = D3D11DrawPipelineError::ui_instance_buffer;
        result = create_instance_buffer(device, config.ui_instance_stride, config.ui_instance_capacity,
                                        &owner->ui_instance_buffer);
        if (FAILED(result)) {
            break;
        }

        owner->ui_instance_capacity = config.ui_instance_capacity;

        failure = D3D11DrawPipelineError::ui_frame_buffer;
        result = gpu_buffer_create(device, {sizeof(FrameTransform), GpuUsage::default_, gpu_bind_constant, false},
                                   nullptr, &owner->ui_frame_buffer);
        if (FAILED(result)) {
            break;
        }

        failure = D3D11DrawPipelineError::rasterizer_state;
        result = gpu_raster_create(device, &owner->ui_rasterizer_state);
        if (FAILED(result)) {
            break;
        }

        failure = D3D11DrawPipelineError::blend_state;
        result = gpu_blend_create(device,
                                  {true, GpuBlendFactor::one, GpuBlendFactor::inv_src_alpha, GpuBlendFactor::one,
                                   GpuBlendFactor::inv_src_alpha},
                                  &owner->ui_blend_state);
        if (FAILED(result)) {
            break;
        }

        const GpuInputElement glyph_elements[] = {
            {"RECT", 0, 0},
            {"TEXCOORD", 0, 16},
            {"COLOR", 0, 32},
        };
        failure = D3D11DrawPipelineError::glyph_vertex_shader;
        result = gpu_vertex_shader_create(device, ShaderId::glyph_vertex, {glyph_elements, 3},
                                          &owner->glyph_vertex_shader, &owner->glyph_input_layout, &layout_failed);
        if (FAILED(result)) {
            if (layout_failed)
                failure = D3D11DrawPipelineError::glyph_input_layout;
            break;
        }
        failure = D3D11DrawPipelineError::glyph_pixel_shader;
        result = gpu_pixel_shader_create(device, ShaderId::glyph_pixel, &owner->glyph_pixel_shader);
        if (FAILED(result)) {
            break;
        }
        failure = D3D11DrawPipelineError::media_pixel_shader;
        result = gpu_pixel_shader_create(device, ShaderId::media_pixel, &owner->media_pixel_shader);
        if (FAILED(result)) {
            break;
        }

        const GpuInputElement media_elements[] = {
            {"RECT", 0, 0}, {"TEXCOORD", 0, 16}, {"TEXCOORD", 1, 32}, {"TEXCOORD", 2, 48}};
        result = gpu_vertex_shader_create(device, ShaderId::media_vertex, {media_elements, 4},
                                          &owner->media_vertex_shader, &owner->media_input_layout, &layout_failed);
        if (FAILED(result))
            break;
        failure = D3D11DrawPipelineError::glyph_instance_buffer;
        result = create_instance_buffer(device, config.glyph_instance_stride,
                                        config.glyph_instance_capacity, &owner->glyph_instance_buffer);
        if (FAILED(result)) {
            break;
        }
        owner->glyph_instance_capacity = config.glyph_instance_capacity;
        failure = D3D11DrawPipelineError::media_instance_buffer;
        result = create_instance_buffer(device, config.media_instance_stride,
                                        config.media_instance_capacity, &owner->media_instance_buffer);
        if (FAILED(result)) {
            break;
        }

        failure = D3D11DrawPipelineError::sampler_state;
        result = gpu_sampler_create(device, {GpuFilter::linear, GpuAddress::clamp}, &owner->glyph_sampler);
        if (FAILED(result)) {
            break;
        }
        return D3D11DrawPipelineError::none;
    } while (false);
    *native_error = result;
    return failure;
}

} // namespace

D3D11DrawPipelineResult d3d11_draw_pipeline_create(D3D11DrawPipelineOwner* staged, GpuDevice* device,
                                                   const D3D11DrawPipelineConfig& config) noexcept {
    if (!staged || !device || config.ui_instance_stride < 64 || config.glyph_instance_stride < 48 ||
        config.media_instance_stride < 48 ||
        !valid_buffer_dimensions(config.ui_instance_stride, config.ui_instance_capacity) ||
        config.ui_instance_capacity > kD3D11MaximumUiInstanceCapacity ||
        !valid_buffer_dimensions(config.glyph_instance_stride, config.glyph_instance_capacity) ||
        !valid_buffer_dimensions(config.media_instance_stride, config.media_instance_capacity)) {
        return pipeline_failure(D3D11DrawPipelineError::invalid_input, E_INVALIDARG);
    }
    reset_value<D3D11DrawPipelineOwner>(staged);
    GpuStatus native_error = kGpuOk;
    const D3D11DrawPipelineError error = create_pipeline_resources(staged, device, config, &native_error);
    if (error != D3D11DrawPipelineError::none) {
        d3d11_draw_pipeline_release(staged);
        return pipeline_failure(error, native_error);
    }
    return pipeline_success();
}

void d3d11_draw_pipeline_warm(D3D11DrawPipelineOwner* pipeline, GpuDevice* device, GpuContext* context,
                              const D3D11DrawPipelineConfig& config) noexcept {
    // The scratch target has the format real frames have (the pipelines are made for it: a driver that compiles on
    // the first draw would otherwise compile again on the first frame); a backend names it without a surface.
    GpuTexture* texture = nullptr;
    GpuRtv* target = nullptr;
    if (!gpu_ok(gpu_texture_create(device, {4, 4, gpu_surface_format(nullptr), GpuUsage::default_, gpu_bind_rtv}, nullptr,
                                   &texture)) ||
        !gpu_ok(gpu_rtv_create(device, texture, &target))) {
        gpu_release(texture);
        return;
    }
    const auto zero_instance = [&](GpuBuffer* buffer, u32 bytes) noexcept {
        void* mapped = gpu_map(context, buffer, GpuMap::discard, nullptr);
        if (!mapped)
            return false;
        clear_bytes(mapped, bytes);
        gpu_unmap(context, buffer);
        return true;
    };
    const GpuRect scissor{0, 0, 4, 4};
    const float blend_factor[4]{};
    gpu_set_viewport(context, 0, 0, 4, 4);
    gpu_set_render_target(context, target);
    gpu_set_raster(context, pipeline->ui_rasterizer_state);
    gpu_set_scissor(context, scissor);
    gpu_set_blend(context, pipeline->ui_blend_state, blend_factor);
    gpu_set_constants(context, GpuStage::vertex, 0, 1, &pipeline->ui_frame_buffer);
    gpu_set_constants(context, GpuStage::pixel, 0, 1, &pipeline->ui_frame_buffer);
    gpu_set_strip_topology(context);
    if (zero_instance(pipeline->ui_instance_buffer, config.ui_instance_stride)) {
        gpu_set_input_layout(context, pipeline->ui_input_layout);
        gpu_set_vertex_buffer(context, pipeline->ui_instance_buffer, config.ui_instance_stride, 0);
        gpu_set_shader(context, pipeline->ui_vertex_shader);
        gpu_set_shader(context, pipeline->ui_pixel_shader);
        gpu_draw_instanced(context, 4, 1, 0, 0);
    }
    // The glyph ring restarts with a discard on the first real upload.
    if (zero_instance(pipeline->glyph_instance_buffer, config.glyph_instance_stride)) {
        pipeline->glyph_instance_end = 0;
        gpu_set_input_layout(context, pipeline->glyph_input_layout);
        gpu_set_vertex_buffer(context, pipeline->glyph_instance_buffer, config.glyph_instance_stride, 0);
        gpu_set_shader(context, pipeline->glyph_vertex_shader);
        gpu_set_shader(context, pipeline->glyph_pixel_shader);
        gpu_set_samplers(context, GpuStage::pixel, 0, 1, &pipeline->glyph_sampler);
        gpu_draw_instanced(context, 4, 1, 0, 0);
    }
    if (zero_instance(pipeline->media_instance_buffer, config.media_instance_stride)) {
        gpu_set_input_layout(context, pipeline->media_input_layout);
        gpu_set_vertex_buffer(context, pipeline->media_instance_buffer, config.media_instance_stride, 0);
        gpu_set_shader(context, pipeline->media_vertex_shader);
        gpu_set_shader(context, pipeline->media_pixel_shader);
        gpu_draw_instanced(context, 4, 1, 0, 0);
    }
    GpuSampler* null_sampler = nullptr;
    gpu_set_samplers(context, GpuStage::pixel, 0, 1, &null_sampler);
    gpu_set_render_target(context, nullptr);
    gpu_flush(context);
    gpu_release(target);
    gpu_release(texture);
}

void d3d11_draw_pipeline_publish(D3D11DrawPipelineOwner* owner, D3D11DrawPipelineOwner* staged) noexcept {
    d3d11_draw_pipeline_release(owner);
    copy_value(owner, *staged);
    reset_value<D3D11DrawPipelineOwner>(staged);
}

D3D11DrawPipelineResult d3d11_draw_pipeline_reserve_ui_instances(D3D11DrawPipelineOwner* owner,
                                                                 GpuDevice* device, u32 stride,
                                                                 u32 required_count) noexcept {
    const u32 capacity = d3d11_ui_instance_capacity_for_count(required_count);
    if (!owner || !device || stride < 64 || !valid_buffer_dimensions(stride, capacity)) {
        return pipeline_failure(D3D11DrawPipelineError::invalid_input, E_INVALIDARG);
    }
    if (owner->ui_instance_buffer && required_count <= owner->ui_instance_capacity)
        return pipeline_success();

    GpuBuffer* replacement = nullptr;
    const GpuStatus result = create_instance_buffer(device, stride, capacity, &replacement);
    if (FAILED(result) || !replacement) {
        gpu_release(replacement);
        return pipeline_failure(D3D11DrawPipelineError::ui_instance_buffer,
                                FAILED(result) ? result : E_FAIL);
    }
    gpu_release(owner->ui_instance_buffer);
    owner->ui_instance_buffer = replacement;
    owner->ui_instance_capacity = capacity;
    return pipeline_success();
}

D3D11DrawPipelineResult d3d11_draw_pipeline_reserve_glyph_instances(D3D11DrawPipelineOwner* owner,
                                                                    GpuDevice* device, u32 stride,
                                                                    u32 capacity) noexcept {
    if (!owner || !device || stride < 48 || !valid_buffer_dimensions(stride, capacity)) {
        return pipeline_failure(D3D11DrawPipelineError::invalid_input, E_INVALIDARG);
    }
    if (owner->glyph_instance_buffer && capacity <= owner->glyph_instance_capacity) {
        return pipeline_success();
    }

    GpuBuffer* replacement = nullptr;
    const GpuStatus result = create_instance_buffer(device, stride, capacity, &replacement);
    if (FAILED(result)) {
        return pipeline_failure(D3D11DrawPipelineError::glyph_instance_buffer, result);
    }
    gpu_release(owner->glyph_instance_buffer);
    owner->glyph_instance_buffer = replacement;
    owner->glyph_instance_capacity = capacity;
    owner->glyph_instance_end = 0;
    return pipeline_success();
}

bool d3d11_draw_pipeline_update_transform(const D3D11DrawPipelineOwner& owner, GpuContext* context,
                                          u32 width, u32 height, u32 dpi, u32 origin_y) noexcept {
    if (!d3d11_draw_pipeline_update_layer(owner, context, width, height, dpi, 1, 1, 0, 0)) {
        return false;
    }
    gpu_set_viewport(context, 0.0f, static_cast<float>(origin_y), static_cast<float>(width),
                     static_cast<float>(height));
    return true;
}

namespace {
bool write_frame(const D3D11DrawPipelineOwner& owner, GpuContext* context, u32 width, u32 height,
                 u32 dpi, float opacity, float scale, float origin_x, float origin_y, float shift_x, float shift_y,
                 float surface_top, float surface_height) noexcept {
    if (!owner.ui_frame_buffer || !context || width == 0 || height == 0 || dpi == 0) {
        return false;
    }
    if (!(scale > 0.0f && scale <= 2.0f) || !(origin_x == origin_x) || !(origin_y == origin_y))
        scale = 1.0f, origin_x = origin_y = 0.0f;
    if (!(shift_x == shift_x) || !(shift_y == shift_y))
        shift_x = shift_y = 0.0f;

    // position' = origin + (position - origin) * scale + shift, then the DIP-to-clip map.
    const float dpi_scale = static_cast<float>(dpi) / 96.0f;
    const float x_scale = 2.0f * dpi_scale / static_cast<float>(width);
    const float y_scale = -2.0f * dpi_scale / static_cast<float>(height);
    // The backdrop gradient spans the surface (window caption and content, of
    // surface_height DIPs from surface_top, as wide as the target): its position
    // is 0.5 at the centre and runs along the palette direction to 0 and 1 at the
    // corners. The shader gets it as saturate(dot(point, xy) + z).
    const float half_x = static_cast<float>(width) / dpi_scale * 0.5f, half_y = surface_height * 0.5f;
    const float x = owner.background_constants.controls[0], y = owner.background_constants.controls[1];
    const float projection =
        maximum((half_x * (x < 0 ? -x : x) + half_y * (y < 0 ? -y : y)) * 2.0f, 0.0001f);
    const float axis_x = x / projection, axis_y = y / projection;
    const FrameTransform transform{x_scale * scale,
                                   y_scale * scale,
                                   -1.0f + (origin_x * (1.0f - scale) + shift_x) * x_scale,
                                   1.0f + (origin_y * (1.0f - scale) + shift_y) * y_scale,
                                   opacity >= 0.0f && opacity <= 1.0f ? opacity : 1.0f,
                                   {axis_x, axis_y, 0.5f - (half_x * axis_x + (surface_top + half_y) * axis_y)}};
    gpu_update_buffer(context, owner.ui_frame_buffer, &transform, sizeof(transform));
    return true;
}
} // namespace

bool d3d11_draw_pipeline_update_layer(const D3D11DrawPipelineOwner& owner, GpuContext* context,
                                      u32 width, u32 height, u32 dpi, float opacity, float scale, float origin_x,
                                      float origin_y, float shift_x, float shift_y) noexcept {
    const float dpi_scale = static_cast<float>(dpi ? dpi : 96) / 96.0f;
    const float caption = static_cast<float>(owner.backdrop_caption) / dpi_scale;
    return write_frame(owner, context, width, height, dpi, opacity, scale, origin_x, origin_y, shift_x, shift_y,
                       -caption, static_cast<float>(height) / dpi_scale + caption);
}

bool d3d11_draw_pipeline_update_caption(const D3D11DrawPipelineOwner& owner, GpuContext* context,
                                        u32 width, u32 height, u32 content_height, u32 dpi,
                                        bool content_space) noexcept {
    const float dpi_scale = static_cast<float>(dpi ? dpi : 96) / 96.0f;
    const float caption = static_cast<float>(height) / dpi_scale;
    return write_frame(owner, context, width, height, dpi, 1, 1, 0, 0, 0, content_space ? caption : 0,
                       content_space ? -caption : 0,
                       static_cast<float>(height + content_height) / dpi_scale);
}

void d3d11_draw_pipeline_release(D3D11DrawPipelineOwner* owner) noexcept {
    if (!owner) {
        return;
    }
    gpu_release(owner->background_buffer);
    owner->background_valid = false;
    owner->background_active = false;
    reset_value(&owner->background_constants);
    reset_value(&owner->background_source);
    owner->background_light = owner->background_source_valid = false;
    owner->background_updates = 0;
    gpu_release(owner->glyph_sampler);
    gpu_release(owner->media_instance_buffer);
    gpu_release(owner->glyph_instance_buffer);
    gpu_release(owner->glyph_input_layout);
    gpu_release(owner->media_vertex_shader);
    gpu_release(owner->media_input_layout);
    gpu_release(owner->media_pixel_shader);
    gpu_release(owner->glyph_pixel_shader);
    gpu_release(owner->glyph_vertex_shader);
    gpu_release(owner->ui_blend_state);
    gpu_release(owner->ui_rasterizer_state);
    gpu_release(owner->ui_frame_buffer);
    gpu_release(owner->ui_instance_buffer);
    gpu_release(owner->ui_input_layout);
    gpu_release(owner->ui_pixel_shader);
    gpu_release(owner->ui_vertex_shader);
    owner->glyph_instance_capacity = 0;
    owner->glyph_instance_end = 0;
    owner->ui_instance_capacity = 0;
}

bool d3d11_draw_pipeline_update_background(D3D11DrawPipelineOwner* owner, GpuDevice* device,
                                           GpuContext* context, const Theme& theme) noexcept {
    if (!owner || !context || !device)
        return false;
    const auto& background = theme.background;
    owner->background_active = ui_background_active(theme) || background_effects_active(background);
    if (!owner->background_active)
        return true;
    const bool light = theme.appearance == ThemeAppearance::light;
    D3D11BackgroundConstants next = owner->background_constants;
    if (!owner->background_source_valid || owner->background_light != light ||
        !background_theme_equal(owner->background_source, background)) {
        Color colors[5]{};
        if (ui_background_active(theme) && !ui_background_palette(theme, colors))
            return false;
        reset_value(&next);
        // The shader's single-colour test needs the raw 0 and 1; more than five
        // stops never fit the palette.
        const u32 stops = minimum<u32>(background.color_count, 5);
        for (u32 i = 0; i < stops; ++i) {
            next.colors[i][0] = colors[i].red;
            next.colors[i][1] = colors[i].green;
            next.colors[i][2] = colors[i].blue;
        }
        next.colors[1][3] = light ? 1.f : 0.f;
        ui_background_direction(background.angle_degrees, &next.controls[0], &next.controls[1]);
        next.controls[2] = stops;
    }
    if (!owner->background_buffer) {
        if (FAILED(gpu_buffer_create(device, {sizeof(next), GpuUsage::default_, gpu_bind_constant, false}, nullptr,
                                     &owner->background_buffer)))
            return false;
    }
    if (!owner->background_valid || !bytes_equal(&owner->background_constants, &next, sizeof(next))) {
        gpu_update_buffer(context, owner->background_buffer, &next, sizeof(next));
        owner->background_constants = next;
        owner->background_valid = true;
        ++owner->background_updates;
    }
    owner->background_source = background;
    owner->background_light = light;
    owner->background_source_valid = true;
    return true;
}

} // namespace maicord
