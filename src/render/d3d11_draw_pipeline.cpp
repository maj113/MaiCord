#include "maicord/render/d3d11_draw_pipeline.hpp"
#include "maicord/ui/background_presentation.hpp"

#include "maicord/render/shader_bytecode.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

struct FrameTransform {
    float scale_x;
    float scale_y;
    float translation_x;
    float translation_y;
    float group_opacity;
    float reserved[3];
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

HRESULT create_instance_buffer(ID3D11Device* device, u32 stride, u32 capacity,
                               ID3D11Buffer** buffer) noexcept {
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = stride * capacity;
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return device->CreateBuffer(&description, nullptr, buffer);
}

D3D11DrawPipelineError create_pipeline_resources(D3D11DrawPipelineOwner* owner, ID3D11Device* device,
                                                 const D3D11DrawPipelineConfig& config,
                                                 HRESULT* native_error) noexcept {
    D3D11DrawPipelineError failure = D3D11DrawPipelineError::ui_vertex_shader;
    HRESULT result = S_OK;
    bool layout_failed = false;
    do {
        const D3D11_INPUT_ELEMENT_DESC ui_elements[] = {
            {"RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        };
        result = shader_create_vertex(device, ShaderId::ui_vertex, {ui_elements, 4}, &owner->ui_vertex_shader,
                                      &owner->ui_input_layout, &layout_failed);
        if (FAILED(result)) {
            if (layout_failed)
                failure = D3D11DrawPipelineError::ui_input_layout;
            break;
        }
        failure = D3D11DrawPipelineError::ui_pixel_shader;
        result = shader_create_pixel(device, ShaderId::ui_pixel, &owner->ui_pixel_shader);
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

        D3D11_BUFFER_DESC frame_description{};
        frame_description.ByteWidth = sizeof(FrameTransform);
        frame_description.Usage = D3D11_USAGE_DEFAULT;
        frame_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        failure = D3D11DrawPipelineError::ui_frame_buffer;
        result = device->CreateBuffer(&frame_description, nullptr, &owner->ui_frame_buffer);
        if (FAILED(result)) {
            break;
        }

        D3D11_RASTERIZER_DESC rasterizer_description{};
        rasterizer_description.FillMode = D3D11_FILL_SOLID;
        rasterizer_description.CullMode = D3D11_CULL_NONE;
        rasterizer_description.DepthClipEnable = TRUE;
        rasterizer_description.ScissorEnable = TRUE;
        failure = D3D11DrawPipelineError::rasterizer_state;
        result = device->CreateRasterizerState(&rasterizer_description, &owner->ui_rasterizer_state);
        if (FAILED(result)) {
            break;
        }

        D3D11_BLEND_DESC blend_description{};
        D3D11_RENDER_TARGET_BLEND_DESC& target_blend = blend_description.RenderTarget[0];
        target_blend.BlendEnable = TRUE;
        target_blend.SrcBlend = D3D11_BLEND_ONE;
        target_blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target_blend.BlendOp = D3D11_BLEND_OP_ADD;
        target_blend.SrcBlendAlpha = D3D11_BLEND_ONE;
        target_blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target_blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target_blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        failure = D3D11DrawPipelineError::blend_state;
        result = device->CreateBlendState(&blend_description, &owner->ui_blend_state);
        if (FAILED(result)) {
            break;
        }

        const D3D11_INPUT_ELEMENT_DESC glyph_elements[] = {
            {"RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        };
        failure = D3D11DrawPipelineError::glyph_vertex_shader;
        result = shader_create_vertex(device, ShaderId::glyph_vertex, {glyph_elements, 3},
                                      &owner->glyph_vertex_shader, &owner->glyph_input_layout, &layout_failed);
        if (FAILED(result)) {
            if (layout_failed)
                failure = D3D11DrawPipelineError::glyph_input_layout;
            break;
        }
        failure = D3D11DrawPipelineError::glyph_pixel_shader;
        result = shader_create_pixel(device, ShaderId::glyph_pixel, &owner->glyph_pixel_shader);
        if (FAILED(result)) {
            break;
        }
        failure = D3D11DrawPipelineError::media_pixel_shader;
        result = shader_create_pixel(device, ShaderId::media_pixel, &owner->media_pixel_shader);
        if (FAILED(result)) {
            break;
        }

        const D3D11_INPUT_ELEMENT_DESC media_elements[] = {
            {"RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
        result = shader_create_vertex(device, ShaderId::media_vertex, {media_elements, 4},
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

        D3D11_SAMPLER_DESC sampler_description{};
        sampler_description.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.MaxLOD = 0.0f;
        failure = D3D11DrawPipelineError::sampler_state;
        result = device->CreateSamplerState(&sampler_description, &owner->glyph_sampler);
        if (FAILED(result)) {
            break;
        }
        return D3D11DrawPipelineError::none;
    } while (false);
    *native_error = result;
    return failure;
}

} // namespace

D3D11DrawPipelineResult d3d11_draw_pipeline_create(D3D11DrawPipelineOwner* staged, ID3D11Device* device,
                                                   const D3D11DrawPipelineConfig& config) noexcept {
    if (!staged || !device || config.ui_instance_stride < 64 || config.glyph_instance_stride < 48 ||
        config.media_instance_stride < 48 ||
        !valid_buffer_dimensions(config.ui_instance_stride, config.ui_instance_capacity) ||
        config.ui_instance_capacity > kD3D11MaximumUiInstanceCapacity ||
        !valid_buffer_dimensions(config.glyph_instance_stride, config.glyph_instance_capacity) ||
        !valid_buffer_dimensions(config.media_instance_stride, config.media_instance_capacity)) {
        return pipeline_failure(D3D11DrawPipelineError::invalid_input, E_INVALIDARG);
    }
    *staged = D3D11DrawPipelineOwner{};
    HRESULT native_error = S_OK;
    const D3D11DrawPipelineError error = create_pipeline_resources(staged, device, config, &native_error);
    if (error != D3D11DrawPipelineError::none) {
        d3d11_draw_pipeline_release(staged);
        return pipeline_failure(error, native_error);
    }
    return pipeline_success();
}

D3D11DrawPipelineResult d3d11_draw_pipeline_publish(D3D11DrawPipelineOwner* owner,
                                                    D3D11DrawPipelineOwner* staged,
                                                    ID3D11DeviceContext* context, u32 width, u32 height,
                                                    u32 dpi) noexcept {
    if (!owner || !staged) {
        return pipeline_failure(D3D11DrawPipelineError::invalid_input, E_INVALIDARG);
    }
    if (!d3d11_draw_pipeline_update_transform(*staged, context, width, height, dpi)) {
        d3d11_draw_pipeline_release(staged);
        return pipeline_failure(D3D11DrawPipelineError::invalid_input, E_INVALIDARG);
    }
    d3d11_draw_pipeline_release(owner);
    *owner = *staged;
    *staged = D3D11DrawPipelineOwner{};
    return pipeline_success();
}

D3D11DrawPipelineResult d3d11_draw_pipeline_initialize(D3D11DrawPipelineOwner* owner, ID3D11Device* device,
                                                       ID3D11DeviceContext* context, u32 width, u32 height,
                                                       u32 dpi,
                                                       const D3D11DrawPipelineConfig& config) noexcept {
    if (!owner || !context || width == 0 || height == 0 || dpi == 0) {
        return pipeline_failure(D3D11DrawPipelineError::invalid_input, E_INVALIDARG);
    }
    D3D11DrawPipelineOwner staged{};
    const D3D11DrawPipelineResult created = d3d11_draw_pipeline_create(&staged, device, config);
    return created.ok() ? d3d11_draw_pipeline_publish(owner, &staged, context, width, height, dpi) : created;
}

D3D11DrawPipelineResult d3d11_draw_pipeline_reserve_ui_instances(D3D11DrawPipelineOwner* owner,
                                                                 ID3D11Device* device, u32 stride,
                                                                 u32 required_count) noexcept {
    const u32 capacity = d3d11_ui_instance_capacity_for_count(required_count);
    if (!owner || !device || stride < 64 || !valid_buffer_dimensions(stride, capacity) ||
        owner->ui_instance_capacity > kD3D11MaximumUiInstanceCapacity) {
        return pipeline_failure(D3D11DrawPipelineError::invalid_input, E_INVALIDARG);
    }
    if (owner->ui_instance_buffer && required_count <= owner->ui_instance_capacity)
        return pipeline_success();

    ID3D11Buffer* replacement = nullptr;
    const HRESULT result = create_instance_buffer(device, stride, capacity, &replacement);
    if (FAILED(result) || !replacement) {
        com_release(replacement);
        return pipeline_failure(D3D11DrawPipelineError::ui_instance_buffer,
                                FAILED(result) ? result : E_FAIL);
    }
    com_release(owner->ui_instance_buffer);
    owner->ui_instance_buffer = replacement;
    owner->ui_instance_capacity = capacity;
    return pipeline_success();
}

D3D11DrawPipelineResult d3d11_draw_pipeline_reserve_glyph_instances(D3D11DrawPipelineOwner* owner,
                                                                    ID3D11Device* device, u32 stride,
                                                                    u32 capacity) noexcept {
    if (!owner || !device || stride < 48 || !valid_buffer_dimensions(stride, capacity)) {
        return pipeline_failure(D3D11DrawPipelineError::invalid_input, E_INVALIDARG);
    }
    if (owner->glyph_instance_buffer && capacity <= owner->glyph_instance_capacity) {
        return pipeline_success();
    }

    ID3D11Buffer* replacement = nullptr;
    const HRESULT result = create_instance_buffer(device, stride, capacity, &replacement);
    if (FAILED(result)) {
        return pipeline_failure(D3D11DrawPipelineError::glyph_instance_buffer, result);
    }
    com_release(owner->glyph_instance_buffer);
    owner->glyph_instance_buffer = replacement;
    owner->glyph_instance_capacity = capacity;
    owner->glyph_instance_end = 0;
    return pipeline_success();
}

bool d3d11_draw_pipeline_update_transform(const D3D11DrawPipelineOwner& owner, ID3D11DeviceContext* context,
                                          u32 width, u32 height, u32 dpi, u32 origin_y) noexcept {
    if (!d3d11_draw_pipeline_update_opacity(owner, context, width, height, dpi, 1.0f)) {
        return false;
    }
    const D3D11_VIEWPORT viewport{0.0f, static_cast<float>(origin_y), static_cast<float>(width),
                                  static_cast<float>(height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    return true;
}

bool d3d11_draw_pipeline_update_opacity(const D3D11DrawPipelineOwner& owner, ID3D11DeviceContext* context,
                                        u32 width, u32 height, u32 dpi, float opacity) noexcept {
    return d3d11_draw_pipeline_update_layer(owner, context, width, height, dpi, opacity, 1, 0, 0);
}

bool d3d11_draw_pipeline_update_layer(const D3D11DrawPipelineOwner& owner, ID3D11DeviceContext* context,
                                      u32 width, u32 height, u32 dpi, float opacity, float scale, float origin_x,
                                      float origin_y) noexcept {
    if (!owner.ui_frame_buffer || !context || width == 0 || height == 0 || dpi == 0) {
        return false;
    }
    if (!(scale > 0.0f && scale <= 2.0f) || !(origin_x == origin_x) || !(origin_y == origin_y))
        scale = 1.0f, origin_x = origin_y = 0.0f;

    // position' = origin + (position - origin) * scale, then the DIP-to-clip map.
    const float dpi_scale = static_cast<float>(dpi) / 96.0f;
    const float x_scale = 2.0f * dpi_scale / static_cast<float>(width);
    const float y_scale = -2.0f * dpi_scale / static_cast<float>(height);
    const FrameTransform transform{x_scale * scale,
                                   y_scale * scale,
                                   -1.0f + origin_x * (1.0f - scale) * x_scale,
                                   1.0f + origin_y * (1.0f - scale) * y_scale,
                                   opacity >= 0.0f && opacity <= 1.0f ? opacity : 1.0f,
                                   {}};
    context->UpdateSubresource(owner.ui_frame_buffer, 0, nullptr, &transform, 0, 0);
    return true;
}

void d3d11_draw_pipeline_release(D3D11DrawPipelineOwner* owner) noexcept {
    if (!owner) {
        return;
    }
    com_release(owner->background_buffer);
    owner->background_valid = false;
    owner->background_active = false;
    owner->background_constants = {};
    owner->background_source = {};
    owner->background_light = owner->background_source_valid = false;
    owner->background_updates = 0;
    com_release(owner->glyph_sampler);
    com_release(owner->media_instance_buffer);
    com_release(owner->glyph_instance_buffer);
    com_release(owner->glyph_input_layout);
    com_release(owner->media_vertex_shader);
    com_release(owner->media_input_layout);
    com_release(owner->media_pixel_shader);
    com_release(owner->glyph_pixel_shader);
    com_release(owner->glyph_vertex_shader);
    com_release(owner->ui_blend_state);
    com_release(owner->ui_rasterizer_state);
    com_release(owner->ui_frame_buffer);
    com_release(owner->ui_instance_buffer);
    com_release(owner->ui_input_layout);
    com_release(owner->ui_pixel_shader);
    com_release(owner->ui_vertex_shader);
    owner->glyph_instance_capacity = 0;
    owner->glyph_instance_end = 0;
    owner->ui_instance_capacity = 0;
}

bool d3d11_draw_pipeline_update_background(D3D11DrawPipelineOwner* owner, ID3D11Device* device,
                                           ID3D11DeviceContext* context, const Theme& theme,
                                           float phase_seconds) noexcept {
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
        next = {};
        for (u32 i = 0; i < background.color_count; ++i) {
            next.colors[i][0] = colors[i].red;
            next.colors[i][1] = colors[i].green;
            next.colors[i][2] = colors[i].blue;
        }
        next.colors[0][3] = background.density / 100.f;
        next.colors[1][3] = light ? 1.f : 0.f;
        ui_background_direction(background.angle_degrees, &next.controls[0], &next.controls[1]);
        next.controls[2] = background.color_count;
        next.controls[3] = background.intensity / 100.f;
        next.motion[1] = background.speed / 100.f;
        next.motion[2] = static_cast<u8>(background.effect);
        next.motion[3] = background.strength / 100.f;
    }
    next.motion[0] = background_effects_active(background) && background.speed && phase_seconds >= 0 &&
                             phase_seconds <= 3.402823466e38f
                         ? phase_seconds
                         : 0;
    if (!owner->background_buffer) {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = sizeof(next);
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&description, nullptr, &owner->background_buffer)))
            return false;
    }
    if (!owner->background_valid || !bytes_equal(&owner->background_constants, &next, sizeof(next))) {
        context->UpdateSubresource(owner->background_buffer, 0, nullptr, &next, 0, 0);
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
