#include "maicord/render/d3d11/background_fx.hpp"
#include "maicord/render/effects/background_fx.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/d3d11/shader_bytecode.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

bool create_resources(D3D11BackgroundFxOwner* owner, ID3D11Device* device) noexcept {
    if (owner->vertex_shader && owner->builtin_shader && owner->constant_buffer && owner->blend_state)
        return true;
    bool layout_failed = false;
    HRESULT result = owner->vertex_shader
                         ? S_OK
                         : shader_create_vertex(device, ShaderId::fx_vertex, {}, &owner->vertex_shader, nullptr,
                                                &layout_failed);
    if (SUCCEEDED(result) && !owner->builtin_shader)
        result = shader_create_pixel(device, ShaderId::fx_builtin, &owner->builtin_shader);
    if (SUCCEEDED(result) && !owner->constant_buffer) {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = sizeof(BackgroundFxConstants);
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        result = device->CreateBuffer(&description, nullptr, &owner->constant_buffer);
    }
    if (SUCCEEDED(result) && !owner->blend_state) {
        // Premultiplied over: dst = src * opacity + dst * (1 - src.a).
        D3D11_BLEND_DESC description{};
        auto& target = description.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        result = device->CreateBlendState(&description, &owner->blend_state);
    }
    if (FAILED(result)) {
        log_u64(LogLevel::error, "render", "background_fx_resource_error", static_cast<u32>(result));
        return false;
    }
    return true;
}

} // namespace

bool d3d11_background_fx_begin_frame(D3D11BackgroundFxOwner* owner, ID3D11Device* device,
                                     ID3D11DeviceContext* context, const Theme& theme, float phase_seconds,
                                     u32 width, u32 height, u32 dpi, bool suppressed) noexcept {
    if (!owner || !device || !context)
        return false;
    owner->source = BackgroundFxSource::none;
    owner->drawn_count = 0;
    owner->draws_last_frame = 0;
    owner->vertex_constants_bound = false;
    owner->layer_opacity = 1;
    const bool builtin = !suppressed && background_fx_builtin_mask(theme.background.effect) &&
                         background_effects_active(theme.background);
    if (!builtin || owner->failed || !width || !height)
        return true;
    if (!create_resources(owner, device)) {
        // Effects are decoration: the frame draws without them.
        owner->failed = true;
        return true;
    }
    const float scale = static_cast<float>(dpi ? dpi : 96) / 96.f;
    const float phase = phase_seconds >= 0 && phase_seconds <= 3.402823466e38f ? phase_seconds : 0;
    owner->frame_pixel_scale = scale;
    if (background_fx_pack_builtin(&owner->frame, theme, phase, UiRect{0, 0, 1, 1}, scale, width / scale,
                                   height / scale))
        owner->source = BackgroundFxSource::builtin;
    return true;
}

bool d3d11_background_fx_draw(D3D11BackgroundFxOwner* owner, ID3D11DeviceContext* context, UiRect canvas,
                              u32 quad_index) noexcept {
    if (!owner || !context || owner->source == BackgroundFxSource::none)
        return false;
    for (u32 i = 0; i < owner->drawn_count; ++i)
        if (owner->drawn[i] == quad_index)
            return false;
    if (owner->drawn_count == kD3D11BackgroundFxCanvases)
        return false;
    auto& frame = owner->frame;
    const float scale = owner->frame_pixel_scale;
    if (!ui_coordinate_valid(canvas.left) || !ui_coordinate_valid(canvas.top) ||
        !ui_coordinate_valid(canvas.right) || !ui_coordinate_valid(canvas.bottom) ||
        !(canvas.right > canvas.left && canvas.bottom > canvas.top))
        return false;
    frame.canvas[0] = canvas.left;
    frame.canvas[1] = canvas.top;
    frame.canvas[2] = canvas.right;
    frame.canvas[3] = canvas.bottom;
    // One instanced sprite per comet lane and star cell the canvas touches.
    const u32 instances = background_fx_builtin_grid(&frame, canvas);
    if (!instances)
        return false;
    owner->drawn[owner->drawn_count++] = quad_index;
    frame.resolution[0] = (canvas.right - canvas.left) * scale;
    frame.resolution[1] = (canvas.bottom - canvas.top) * scale;
    frame.origin[0] = canvas.left * scale;
    frame.origin[1] = canvas.top * scale;
    context->UpdateSubresource(owner->constant_buffer, 0, nullptr, &frame, 0, 0);
    // The strip topology and the bound input layout stay: the vertex shader reads only SV_VertexID and SV_InstanceID.
    context->VSSetShader(owner->vertex_shader, nullptr, 0);
    // VS b2 is the effect's alone: bound once per frame.
    if (!owner->vertex_constants_bound) {
        context->VSSetConstantBuffers(2, 1, &owner->constant_buffer);
        owner->vertex_constants_bound = true;
    }
    context->PSSetShader(owner->builtin_shader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &owner->constant_buffer);
    const float opacity = owner->layer_opacity;
    const float factor = opacity >= 0 && opacity <= 1 ? opacity : 1;
    const float blend_factor[4]{factor, factor, factor, factor};
    context->OMSetBlendState(owner->blend_state, blend_factor, 0xFFFFFFFFu);
    context->DrawInstanced(4, instances, 0, 0);
    ++owner->draws_last_frame;
    return true;
}

void d3d11_background_fx_release(D3D11BackgroundFxOwner* owner) noexcept {
    if (!owner)
        return;
    com_release(owner->blend_state);
    com_release(owner->constant_buffer);
    com_release(owner->builtin_shader);
    com_release(owner->vertex_shader);
    reset_value(owner);
}

} // namespace maicord
