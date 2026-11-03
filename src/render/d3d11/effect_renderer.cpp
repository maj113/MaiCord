#include "maicord/render/d3d11/effect_renderer.hpp"
#include "maicord/render/effects/background_fx.hpp"

#include "effect_inputs.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/d3d11/shader_bytecode.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

void release_timing(D3D11EffectTiming& timing) noexcept {
    com_release(timing.disjoint);
    com_release(timing.begin);
    com_release(timing.end);
    reset_value(&timing);
}

bool create_timing(D3D11EffectTiming& timing, ID3D11Device* device) noexcept {
    if (timing.disjoint)
        return true;
    const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    const D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
    const bool ok = SUCCEEDED(device->CreateQuery(&disjoint, &timing.disjoint)) &&
                    SUCCEEDED(device->CreateQuery(&stamp, &timing.begin)) &&
                    SUCCEEDED(device->CreateQuery(&stamp, &timing.end));
    if (!ok)
        release_timing(timing);
    return ok;
}

void release_target(D3D11EffectOwner* owner) noexcept {
    com_release(owner->target_srv);
    com_release(owner->target_view);
    com_release(owner->target);
    owner->target_width = owner->target_height = 0;
    owner->content_valid = false;
}

// Everything but the selection: the next select (or begin_frame, after a failure) builds it again.
void release_objects(D3D11EffectOwner* owner) noexcept {
    release_target(owner);
    for (auto& timing : owner->timing)
        release_timing(timing);
    for (auto*& view : owner->inputs)
        com_release(view);
    for (auto*& state : owner->blends)
        com_release(state);
    for (auto*& state : owner->samplers)
        com_release(state);
    com_release(owner->rasterizer);
    com_release(owner->composite_buffer);
    com_release(owner->frame_buffer);
    com_release(owner->shader);
    com_release(owner->composite_pixel);
    com_release(owner->composite_vertex);
    com_release(owner->target_vertex);
    owner->composite_bound = false;
    owner->dirty = true;
}

void mark_idle(D3D11EffectOwner* owner) noexcept {
    if (owner->marker)
        *owner->marker = 0;
}

bool create_blend_states(D3D11EffectOwner* owner, ID3D11Device* device) noexcept {
    for (u32 mode = 0; mode < 3; ++mode) {
        // The target holds premultiplied colour, so every mode adds the source and weighs what is behind it:
        // normal by what the source leaves open, screen by one minus the source colour, add by one.
        D3D11_BLEND_DESC description{};
        auto& target = description.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
        target.DestBlend = mode == 0 ? D3D11_BLEND_INV_SRC_ALPHA
                           : mode == 1 ? D3D11_BLEND_INV_SRC_COLOR
                                       : D3D11_BLEND_ONE;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        // Normal composes alpha like colour; the other modes leave the destination alpha alone.
        target.SrcBlendAlpha = mode == 0 ? D3D11_BLEND_BLEND_FACTOR : D3D11_BLEND_ZERO;
        target.DestBlendAlpha = mode == 0 ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_ONE;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&description, &owner->blends[mode])))
            return false;
    }
    return true;
}

bool create_sampler_states(D3D11EffectOwner* owner, ID3D11Device* device) noexcept {
    // mc_point_wrap, mc_linear_wrap, mc_linear_clamp.
    const D3D11_FILTER filters[3]{D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,
                                  D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT};
    const D3D11_TEXTURE_ADDRESS_MODE modes[3]{D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP,
                                              D3D11_TEXTURE_ADDRESS_CLAMP};
    for (u32 i = 0; i < 3; ++i) {
        D3D11_SAMPLER_DESC description{};
        description.Filter = filters[i];
        description.AddressU = description.AddressV = description.AddressW = modes[i];
        description.MaxLOD = 0;
        if (FAILED(device->CreateSamplerState(&description, &owner->samplers[i])))
            return false;
    }
    return true;
}

bool create_buffer(ID3D11Device* device, u32 bytes, ID3D11Buffer** buffer) noexcept {
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = bytes;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device->CreateBuffer(&description, nullptr, buffer));
}

// Shaders, states and constants: independent of the package.
bool create_pass_objects(D3D11EffectOwner* owner, ID3D11Device* device) noexcept {
    if (owner->target_vertex && owner->composite_vertex && owner->composite_pixel && owner->frame_buffer &&
        owner->composite_buffer && owner->rasterizer && owner->blends[2] && owner->samplers[2])
        return true;
    bool layout_failed = false;
    HRESULT result = owner->target_vertex ? S_OK
                                          : shader_create_vertex(device, ShaderId::fx_target_vertex, {},
                                                                 &owner->target_vertex, nullptr, &layout_failed);
    if (SUCCEEDED(result) && !owner->composite_vertex)
        result = shader_create_vertex(device, ShaderId::fx_composite_vertex, {}, &owner->composite_vertex, nullptr,
                                      &layout_failed);
    if (SUCCEEDED(result) && !owner->composite_pixel)
        result = shader_create_pixel(device, ShaderId::fx_composite_pixel, &owner->composite_pixel);
    bool ok = SUCCEEDED(result) && (owner->frame_buffer || create_buffer(device, sizeof(EffectFrameConstants), &owner->frame_buffer)) &&
              (owner->composite_buffer ||
               create_buffer(device, sizeof(EffectCompositeConstants), &owner->composite_buffer));
    if (ok && !owner->rasterizer) {
        D3D11_RASTERIZER_DESC description{};
        description.FillMode = D3D11_FILL_SOLID;
        description.CullMode = D3D11_CULL_NONE;
        description.DepthClipEnable = TRUE;
        // The strips are scissored.
        description.ScissorEnable = TRUE;
        ok = SUCCEEDED(device->CreateRasterizerState(&description, &owner->rasterizer));
    }
    ok = ok && (owner->blends[2] || create_blend_states(owner, device)) &&
         (owner->samplers[2] || create_sampler_states(owner, device));
    if (!ok)
        log_u64(LogLevel::error, "render", "effect_pass_resource_error", static_cast<u32>(result));
    return ok;
}

bool ensure_target(D3D11EffectOwner* owner, ID3D11Device* device, ID3D11DeviceContext* context, u32 width,
                   u32 height) noexcept {
    const u32 bucket = kD3D11EffectTargetBucket;
    const u32 allocated_width = (width + bucket - 1) / bucket * bucket;
    const u32 allocated_height = (height + bucket - 1) / bucket * bucket;
    if (owner->target && owner->target_width == allocated_width && owner->target_height == allocated_height)
        return true;
    release_target(owner);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = allocated_width;
    description.Height = allocated_height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const bool ok = SUCCEEDED(device->CreateTexture2D(&description, nullptr, &owner->target)) &&
                    SUCCEEDED(device->CreateRenderTargetView(owner->target, nullptr, &owner->target_view)) &&
                    SUCCEEDED(device->CreateShaderResourceView(owner->target, nullptr, &owner->target_srv));
    if (!ok) {
        release_target(owner);
        return false;
    }
    // Unused texels beyond the viewport stay transparent.
    const float clear[4]{};
    context->ClearRenderTargetView(owner->target_view, clear);
    owner->target_width = allocated_width;
    owner->target_height = allocated_height;
    return true;
}

// Binds the pass's shaders, inputs and samplers (everything but the viewport, target and constants' content).
void bind_pass(D3D11EffectOwner* owner, ID3D11DeviceContext* context) noexcept {
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->IASetInputLayout(nullptr);
    context->RSSetState(owner->rasterizer);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    context->VSSetShader(owner->target_vertex, nullptr, 0);
    context->PSSetShader(owner->shader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &owner->frame_buffer);
    context->PSSetSamplers(0, 3, owner->samplers);
    context->PSSetShaderResources(0, kD3D11EffectInputs, owner->inputs);
}

// The pass's draws: the whole target in kD3D11EffectStrips horizontal strips, so no single draw runs long.
void draw_strips(ID3D11DeviceContext* context, u32 width, u32 height) noexcept {
    const u32 strip = (height + kD3D11EffectStrips - 1) / kD3D11EffectStrips;
    for (u32 top = 0; top < height; top += strip) {
        const D3D11_RECT scissor{0, static_cast<LONG>(top), static_cast<LONG>(width),
                                 static_cast<LONG>(minimum(top + strip, height))};
        context->RSSetScissorRects(1, &scissor);
        context->Draw(4, 0);
    }
}

void unbind_pass(ID3D11DeviceContext* context) noexcept {
    ID3D11ShaderResourceView* views[kD3D11EffectInputs]{};
    context->PSSetShaderResources(0, kD3D11EffectInputs, views);
    ID3D11RenderTargetView* none = nullptr;
    context->OMSetRenderTargets(1, &none, nullptr);
}

float budget_ms(const EffectSettings& settings) noexcept {
    return kEffectBudgetMs[static_cast<u32>(settings.budget)];
}

// Reads finished frames without flushing; a frame still in flight stays for a later call. Feeds the budget
// controller; an overload turns the effect off.
void collect(D3D11EffectOwner* owner, ID3D11DeviceContext* context) noexcept {
    bool pending = false;
    for (auto& timing : owner->timing) {
        if (!timing.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        const HRESULT state =
            context->GetData(timing.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        u64 begin = 0, end = 0;
        HRESULT a = S_OK, b = S_OK;
        if (state != S_FALSE && SUCCEEDED(state)) {
            a = context->GetData(timing.begin, &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            b = context->GetData(timing.end, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        }
        if (state == S_FALSE || a == S_FALSE || b == S_FALSE) {
            pending = true;
            continue;
        }
        timing.pending = false;
        if (FAILED(state) || FAILED(a) || FAILED(b) || disjoint.Disjoint || !disjoint.Frequency || end < begin ||
            !owner->selected || timing.identity != owner->identity)
            continue;
        const float gpu_ms = static_cast<float>(static_cast<double>(end - begin) * 1000.0 /
                                                static_cast<double>(disjoint.Frequency));
        switch (effect_budget_sample(&owner->budget, gpu_ms, budget_ms(owner->settings), timing.frame_ms)) {
        case EffectBudgetAction::step_down:
        case EffectBudgetAction::step_up:
        case EffectBudgetAction::drop_to_floor:
            owner->step = owner->budget.step;
            owner->dirty = true;
            break;
        case EffectBudgetAction::disable:
            owner->verdict = EffectVerdict::overloaded;
            owner->verdict_identity = owner->identity;
            log_u64(LogLevel::warning, "render", "effect_overloaded_identity", owner->identity);
            d3d11_effect_clear(owner);
            break;
        case EffectBudgetAction::none:
            break;
        }
    }
    if (!pending)
        mark_idle(owner);
}

// ---- Selection probe ----------------------------------------------------------------------------
// One frame time (ms) of the package's shader on a `width` x `height` target, or a negative value when the
// device cannot time it. Runs frames synchronously (selection is a cold path) and gives the GPU a bounded
// time to answer.
float probe_size(D3D11EffectOwner* owner, ID3D11Device* device, ID3D11DeviceContext* context, u32 width,
                 u32 height) noexcept {
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    float result = -1;
    D3D11EffectTiming timing{};
    if (SUCCEEDED(device->CreateTexture2D(&description, nullptr, &texture)) &&
        SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, &view)) && create_timing(timing, device)) {
        EffectFrameConstants constants{};
        constants.dip = 1;
        constants.resolution[0] = static_cast<float>(width);
        constants.resolution[1] = static_cast<float>(height);
        constants.pointer[0] = constants.pointer[1] = -1;
        constants.intensity = 1;
        constants.dark = 1;
        effect_palette(kDefaultTheme, constants.palette);
        copy_bytes(constants.knobs, owner->knobs, sizeof(constants.knobs));
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context->OMSetRenderTargets(1, &view, nullptr);
        context->RSSetViewports(1, &viewport);
        bind_pass(owner, context);
        // The first frame pays for the driver's lazy state; the next three are timed.
        constexpr u32 kTimedFrames = 3;
        for (u32 frame = 0; frame <= kTimedFrames; ++frame) {
            constants.time = constants.frame = static_cast<float>(frame);
            context->UpdateSubresource(owner->frame_buffer, 0, nullptr, &constants, 0, 0);
            if (frame == 1) {
                context->Begin(timing.disjoint);
                context->End(timing.begin);
            }
            draw_strips(context, width, height);
        }
        context->End(timing.end);
        context->End(timing.disjoint);
        unbind_pass(context);
        context->Flush();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        u64 begin = 0, end = 0;
        const u64 deadline = GetTickCount64() + 250;
        HRESULT state = S_FALSE;
        while ((state = context->GetData(timing.disjoint, &disjoint, sizeof(disjoint), 0)) == S_FALSE &&
               GetTickCount64() < deadline)
            Sleep(1);
        if (state == S_OK && !disjoint.Disjoint && disjoint.Frequency &&
            context->GetData(timing.begin, &begin, sizeof(begin), 0) == S_OK &&
            context->GetData(timing.end, &end, sizeof(end), 0) == S_OK && end >= begin)
            result = static_cast<float>(static_cast<double>(end - begin) * 1000.0 /
                                        static_cast<double>(disjoint.Frequency) / kTimedFrames);
    }
    release_timing(timing);
    com_release(view);
    com_release(texture);
    return result;
}

// Fits the frame time at two sizes. False when the device could not time them: the effect then starts at
// the user's resolution and the controller does the rest.
bool run_probe(D3D11EffectOwner* owner, ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    constexpr u32 kSmallWidth = 64, kSmallHeight = 36, kLargeWidth = 192, kLargeHeight = 108;
    const float small = probe_size(owner, device, context, kSmallWidth, kSmallHeight);
    const float large = small >= 0 ? probe_size(owner, device, context, kLargeWidth, kLargeHeight) : -1.f;
    if (small < 0 || large < 0)
        return false;
    owner->probe = {small, large, kSmallWidth * kSmallHeight, kLargeWidth * kLargeHeight};
    return true;
}

// The step the controller starts at; true when the effect is too heavy for this GPU at any step.
bool start_controller(D3D11EffectOwner* owner) noexcept {
    const u32 ceiling = owner->settings.resolution_step;
    bool refuse = false;
    const u32 start = owner->probed ? effect_probe_start_step(owner->probe, owner->window_width,
                                                              owner->window_height, budget_ms(owner->settings),
                                                              ceiling, &refuse)
                                    : ceiling;
    effect_budget_start(&owner->budget, start, ceiling);
    owner->step = owner->budget.step;
    owner->dirty = true;
    return refuse;
}

bool bytes_same(const void* a, const void* b, usize bytes) noexcept { return bytes_equal(a, b, bytes); }

} // namespace

EffectSelectResult d3d11_effect_select(D3D11EffectOwner* owner, ID3D11Device* device, ID3D11DeviceContext* context,
                                       const BackgroundEffectSpec& spec, const EffectSettings& settings,
                                       u64 adapter_key, ID3D11ShaderResourceView* glyph_view, u32 window_width,
                                       u32 window_height) noexcept {
    const auto result = [](HRESULT error, EffectSelect state) noexcept { return EffectSelectResult{error, state}; };
    if (!owner || !device || !context || !spec.identity || !spec.bytecode.data || !spec.bytecode.count ||
        !effect_settings_valid(settings) || (spec.uses & ~static_cast<u32>(effect_use_all)) ||
        static_cast<u8>(spec.blend) > 2 || !window_width || !window_height)
        return result(E_INVALIDARG, EffectSelect::rejected);
    const bool same = owner->selected && owner->identity == spec.identity;
    owner->window_width = window_width;
    owner->window_height = window_height;
    if (same && owner->shader) {
        // The package is built: only the user's side and the knob values can have moved.
        bool changed = false;
        if (!bytes_same(owner->knobs, spec.knobs, sizeof(owner->knobs))) {
            copy_bytes(owner->knobs, spec.knobs, sizeof(owner->knobs));
            changed = true;
        }
        if (!bytes_same(&owner->settings, &settings, sizeof(settings))) {
            const bool restart = owner->settings.resolution_step != settings.resolution_step ||
                                 owner->settings.budget != settings.budget;
            owner->settings = settings;
            changed = true;
            if (restart)
                start_controller(owner);
        }
        owner->dirty = owner->dirty || changed;
        return result(S_OK, changed ? EffectSelect::changed : EffectSelect::unchanged);
    }
    if (!same)
        d3d11_effect_clear(owner);
    // The package's pixel shader, and its inputs.
    if (!create_pass_objects(owner, device))
        return result(E_FAIL, EffectSelect::rejected);
    ID3D11PixelShader* shader = nullptr;
    const HRESULT created = device->CreatePixelShader(spec.bytecode.data, spec.bytecode.count, nullptr, &shader);
    if (FAILED(created)) {
        d3d11_effect_clear(owner);
        return result(created, EffectSelect::rejected);
    }
    ID3D11ShaderResourceView* inputs[kD3D11EffectInputs]{};
    const HRESULT made = effect_inputs_create(device, spec, glyph_view, inputs);
    if (FAILED(made)) {
        com_release(shader);
        d3d11_effect_clear(owner);
        return result(made, EffectSelect::rejected);
    }
    for (u32 i = 0; i < kD3D11EffectInputs; ++i) {
        com_release(owner->inputs[i]);
        owner->inputs[i] = inputs[i];
    }
    com_release(owner->shader);
    owner->shader = shader;
    owner->selected = true;
    owner->identity = spec.identity;
    owner->adapter_key = adapter_key;
    owner->uses = spec.uses;
    owner->settings = settings;
    copy_bytes(owner->knobs, spec.knobs, sizeof(owner->knobs));
    if (!same) {
        owner->last_phase = owner->time = owner->frames = 0;
        owner->pointer[0] = owner->pointer[1] = -1;
        owner->verdict = EffectVerdict::none;
    }
    owner->probed = run_probe(owner, device, context);
    if (start_controller(owner)) {
        owner->verdict = EffectVerdict::refused;
        owner->verdict_identity = spec.identity;
        d3d11_effect_clear(owner);
        return result(S_OK, EffectSelect::refused);
    }
    owner->dirty = true;
    return result(S_OK, EffectSelect::changed);
}

bool d3d11_effect_clear(D3D11EffectOwner* owner) noexcept {
    if (!owner || !owner->selected)
        return false;
    // The shader and the inputs belong to the package; the pass's own objects stay for the next one.
    com_release(owner->shader);
    for (auto*& view : owner->inputs)
        com_release(view);
    release_target(owner);
    owner->selected = owner->probed = false;
    owner->active = false;
    owner->identity = owner->adapter_key = 0;
    owner->uses = 0;
    owner->step = 0;
    reset_value(&owner->budget);
    mark_idle(owner);
    return true;
}

void d3d11_effect_set_pointer(D3D11EffectOwner* owner, float x, float y) noexcept {
    if (owner) {
        owner->pointer[0] = is_finite(x) ? x : -1.f;
        owner->pointer[1] = is_finite(y) ? y : -1.f;
    }
}

bool d3d11_effect_begin_frame(D3D11EffectOwner* owner, ID3D11Device* device, ID3D11DeviceContext* context,
                              const Theme& theme, float phase, u32 content_width, u32 content_height,
                              u32 caption_height, u32 dpi, bool live_resize, u64 now_ms) noexcept {
    if (!owner || !device || !context)
        return false;
    owner->active = owner->rendered = false;
    owner->drawn_count = owner->draws_last_frame = 0;
    owner->layer_opacity = 1;
    owner->composite_bound = false;
    collect(owner, context);
    if (!owner->selected || !owner->shader || !content_width || !content_height)
        return true;
    const u32 window_height = content_height + caption_height;
    const float scale = static_cast<float>(dpi ? dpi : 96) / 96.f;
    owner->pixel_scale = scale;
    owner->caption_pixels = caption_height;
    owner->window_width = content_width;
    owner->window_height = window_height;
    const u32 used_width = effect_scaled_extent(content_width, owner->step);
    const u32 used_height = effect_scaled_extent(window_height, owner->step);
    phase = phase >= 0 && phase <= 3.402823466e38f ? phase : 0;
    EffectFrameConstants constants = owner->constants;
    effect_palette(theme, constants.palette);
    constants.dark = theme.appearance == ThemeAppearance::light ? 0.f : 1.f;
    constants.intensity = background_fx_intensity(theme);
    copy_bytes(constants.knobs, owner->knobs, sizeof(constants.knobs));
    constants.resolution[0] = static_cast<float>(used_width);
    constants.resolution[1] = static_cast<float>(used_height);
    constants.dip = scale * static_cast<float>(used_width) / static_cast<float>(content_width);
    // The pointer is a window position in pixels; the effect sees target pixels.
    const bool pointer = (owner->uses & effect_use_pointer) && owner->settings.pointer_allowed &&
                         owner->pointer[0] >= 0 && owner->pointer[1] >= 0;
    constants.pointer[0] = pointer ? owner->pointer[0] * static_cast<float>(used_width) / content_width : -1.f;
    constants.pointer[1] = pointer ? owner->pointer[1] * static_cast<float>(used_height) / window_height : -1.f;
    // What the picture depends on besides the clock: palette, flags, knobs, size and DPI.
    const bool repaint = !owner->content_valid || owner->dirty || used_width != owner->used_width ||
                         used_height != owner->used_height || dpi != owner->dpi ||
                         !bytes_same(constants.palette, owner->constants.palette, sizeof(constants.palette)) ||
                         constants.dark != owner->constants.dark || constants.intensity != owner->constants.intensity ||
                         !bytes_same(constants.knobs, owner->constants.knobs, sizeof(constants.knobs)) ||
                         !bytes_same(constants.pointer, owner->constants.pointer, sizeof(constants.pointer));
    // A window being dragged keeps the last picture, stretched.
    if ((repaint || phase != owner->last_phase) && !(live_resize && owner->content_valid)) {
        if (!ensure_target(owner, device, context, used_width, used_height)) {
            owner->dirty = true;
            return true;
        }
        const float delta = owner->frames > 0 && phase >= owner->last_phase && phase - owner->last_phase <= 1.f
                                ? phase - owner->last_phase
                                : 0.f;
        owner->time += delta * owner->settings.speed;
        constants.time = owner->time;
        constants.delta = delta;
        constants.frame = owner->frames;
        owner->frames += 1;
        owner->last_phase = phase;
        owner->constants = constants;
        owner->used_width = used_width;
        owner->used_height = used_height;
        owner->dpi = dpi;
        owner->dirty = false;
        // A frame in flight is named in the mapping until its timing resolves.
        if (owner->marker)
            *owner->marker = owner->identity;
        auto& timing = owner->timing[owner->timing_cursor];
        const bool timed = !timing.pending && create_timing(timing, device);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(used_width), static_cast<float>(used_height), 0, 1};
        // Slot 0 may still hold the target from the last frame's canvases.
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        context->OMSetRenderTargets(1, &owner->target_view, nullptr);
        context->RSSetViewports(1, &viewport);
        bind_pass(owner, context);
        context->UpdateSubresource(owner->frame_buffer, 0, nullptr, &constants, 0, 0);
        if (timed) {
            context->Begin(timing.disjoint);
            context->End(timing.begin);
        }
        draw_strips(context, used_width, used_height);
        if (timed) {
            context->End(timing.end);
            context->End(timing.disjoint);
            timing.identity = owner->identity;
            timing.frame_ms = now_ms;
            timing.pending = true;
            owner->timing_cursor = (owner->timing_cursor + 1) % kD3D11EffectTimingFrames;
        }
        unbind_pass(context);
        owner->content_valid = true;
        owner->rendered = true;
        ++owner->renders;
    }
    owner->active = owner->content_valid;
    return true;
}

bool d3d11_effect_draw(D3D11EffectOwner* owner, ID3D11DeviceContext* context, UiRect canvas, u32 quad_index,
                       float tile_radius) noexcept {
    if (!owner || !context || !owner->active)
        return false;
    for (u32 i = 0; i < owner->drawn_count; ++i)
        if (owner->drawn[i] == quad_index)
            return false;
    if (owner->drawn_count == kD3D11EffectCanvases || !ui_coordinate_valid(canvas.left) ||
        !ui_coordinate_valid(canvas.top) || !ui_coordinate_valid(canvas.right) ||
        !ui_coordinate_valid(canvas.bottom) || !(canvas.right > canvas.left && canvas.bottom > canvas.top))
        return false;
    owner->drawn[owner->drawn_count++] = quad_index;
    // A DIP maps to a window pixel, then to a share of the window, then into the part of the allocation drawn.
    const float allocated_width = static_cast<float>(owner->target_width);
    const float allocated_height = static_cast<float>(owner->target_height);
    const float used_width = static_cast<float>(owner->used_width), used_height = static_cast<float>(owner->used_height);
    const float share_x = used_width / allocated_width, share_y = used_height / allocated_height;
    const float window_width = static_cast<float>(owner->window_width), window_height = static_cast<float>(owner->window_height);
    float scale_x = owner->pixel_scale / window_width * share_x;
    float scale_y = owner->pixel_scale / window_height * share_y;
    float offset_x = 0.f;
    float offset_y = static_cast<float>(owner->caption_pixels) / window_height * share_y;
    if (tile_radius > 0) {
        // The tile shows the content area (below the caption) scaled by `cover` so it fills the rectangle; the
        // part that overflows is centred.
        const float caption = static_cast<float>(owner->caption_pixels) / owner->pixel_scale;
        const float content_width = window_width / owner->pixel_scale, content_height = window_height / owner->pixel_scale - caption;
        const float tile_width = canvas.right - canvas.left, tile_height = canvas.bottom - canvas.top;
        const float cover = maximum(tile_width / content_width, tile_height / content_height);
        const float window_dip_height = window_height / owner->pixel_scale;
        scale_x = share_x / (cover * content_width);
        scale_y = share_y / (cover * window_dip_height);
        offset_x = share_x * ((content_width - tile_width / cover) * .5f - canvas.left / cover) / content_width;
        offset_y = share_y * (caption + (content_height - tile_height / cover) * .5f - canvas.top / cover) / window_dip_height;
    }
    const EffectCompositeConstants constants{{canvas.left, canvas.top, canvas.right, canvas.bottom},
                                             {scale_x, scale_y, offset_x, offset_y},
                                             {.5f / allocated_width, .5f / allocated_height,
                                              (used_width - .5f) / allocated_width,
                                              (used_height - .5f) / allocated_height},
                                             owner->settings.opacity,
                                             tile_radius > 0 ? tile_radius : 0.f,
                                             {}};
    context->UpdateSubresource(owner->composite_buffer, 0, nullptr, &constants, 0, 0);
    // The strip topology, the layer transform and the bilinear sampler stay: the vertex shader reads only SV_VertexID.
    context->VSSetShader(owner->composite_vertex, nullptr, 0);
    if (!owner->composite_bound) {
        context->VSSetConstantBuffers(2, 1, &owner->composite_buffer);
        owner->composite_bound = true;
    }
    context->PSSetShader(owner->composite_pixel, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &owner->composite_buffer);
    context->PSSetShaderResources(0, 1, &owner->target_srv);
    const float opacity = owner->layer_opacity;
    const float factor = opacity >= 0 && opacity <= 1 ? opacity : 1;
    const float blend_factor[4]{factor, factor, factor, factor};
    context->OMSetBlendState(owner->blends[static_cast<u32>(owner->settings.blend)], blend_factor, 0xFFFFFFFFu);
    context->Draw(4, 0);
    ++owner->draws_last_frame;
    return true;
}

void d3d11_effect_end_frame(D3D11EffectOwner* owner, ID3D11DeviceContext* context) noexcept {
    if (!owner || !context || !owner->draws_last_frame)
        return;
    // Leave no view of the target bound: the next frame renders into it.
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
}

void d3d11_effect_release_device(D3D11EffectOwner* owner) noexcept {
    if (!owner)
        return;
    // A frame that was in flight is the removal's suspect; the host reads it from the verdict.
    if (owner->selected && owner->marker && *owner->marker == owner->identity) {
        owner->verdict = EffectVerdict::device_removed;
        owner->verdict_identity = owner->identity;
    }
    release_objects(owner);
    owner->active = false;
}

void d3d11_effect_release(D3D11EffectOwner* owner) noexcept {
    if (!owner)
        return;
    release_objects(owner);
    mark_idle(owner);
    reset_value(owner);
}

} // namespace maicord

namespace maicord {
void d3d11_effect_set_marker(D3D11EffectOwner* owner, volatile u64* marker) noexcept {
    if (owner)
        owner->marker = marker;
}
} // namespace maicord
