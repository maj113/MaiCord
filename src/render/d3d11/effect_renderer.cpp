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

void release_graph_target(D3D11EffectGraphTarget& target) noexcept {
    for (u32 plane = 0; plane < 2; ++plane) {
        com_release(target.views[plane]);
        com_release(target.reads[plane]);
    }
    target = {};
}

// A graph's targets (made again, cleared, on the next frame) and, with `shaders`, its shaders.
void release_graph(D3D11EffectOwner* owner, bool shaders) noexcept {
    for (auto& target : owner->graph_targets)
        release_graph_target(target);
    if (shaders)
        for (auto& stage : owner->graph_shaders)
            com_release(stage.vertex);
}

// Everything but the selection: the next select (or begin_frame, after a failure) builds it again.
void release_objects(D3D11EffectOwner* owner) noexcept {
    release_target(owner);
    release_graph(owner, true);
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
    // Source, destination, source alpha, destination alpha. The composite's modes first: the target holds
    // premultiplied colour, so every mode adds the source at the user's opacity (the blend factor) and weighs what
    // is behind it: normal by what the source leaves open, screen by one minus the source colour, add by one; only
    // normal composes alpha. Then a graph pass's alpha, add and premultiplied. Last the adapted screen and add: while
    // they cross-fade, the source plus what is behind weighed by the shader's second output; settled, what is behind
    // times the source.
    static constexpr u8 kModes[8][4]{
        {D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_SRC_ALPHA},
        {D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_ZERO, D3D11_BLEND_ONE},
        {D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE},
        {D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA},
        {D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ONE},
        {D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA},
        {D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_SRC1_COLOR, D3D11_BLEND_ZERO, D3D11_BLEND_ONE},
        {D3D11_BLEND_ZERO, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_ZERO, D3D11_BLEND_ONE}};
    for (u32 mode = 0; mode < 8; ++mode) {
        D3D11_BLEND_DESC description{};
        auto& target = description.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = static_cast<D3D11_BLEND>(kModes[mode][0]);
        target.DestBlend = static_cast<D3D11_BLEND>(kModes[mode][1]);
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = static_cast<D3D11_BLEND>(kModes[mode][2]);
        target.DestBlendAlpha = static_cast<D3D11_BLEND>(kModes[mode][3]);
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
        owner->composite_buffer && owner->rasterizer && owner->blends[7] && owner->samplers[2])
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
    ok = ok && (owner->blends[7] || create_blend_states(owner, device)) &&
         (owner->samplers[2] || create_sampler_states(owner, device));
    if (!ok)
        log_u64(LogLevel::error, "render", "effect_pass_resource_error", static_cast<u32>(result));
    return ok;
}

// A render target cleared to transparent black, with a shader view when `read` is given; the texture stays with
// the views unless `keep` takes it. False leaves what was made in the outputs, for the caller to release.
bool make_target(ID3D11Device* device, ID3D11DeviceContext* context, u32 width, u32 height, DXGI_FORMAT format,
                 ID3D11RenderTargetView** view, ID3D11ShaderResourceView** read, ID3D11Texture2D** keep) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | (read ? D3D11_BIND_SHADER_RESOURCE : 0u);
    ID3D11Texture2D* texture = nullptr;
    const bool ok = SUCCEEDED(device->CreateTexture2D(&description, nullptr, &texture)) &&
                    SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, view)) &&
                    (!read || SUCCEEDED(device->CreateShaderResourceView(texture, nullptr, read)));
    if (ok) {
        const float clear[4]{};
        context->ClearRenderTargetView(*view, clear);
    }
    if (keep)
        *keep = texture;
    else
        com_release(texture);
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
    // Unused texels beyond the viewport stay transparent.
    if (!make_target(device, context, allocated_width, allocated_height, DXGI_FORMAT_R8G8B8A8_UNORM,
                     &owner->target_view, &owner->target_srv, &owner->target)) {
        release_target(owner);
        return false;
    }
    owner->target_width = allocated_width;
    owner->target_height = allocated_height;
    return true;
}

// The graph's targets for an output of `width` x `height`: a target whose size changes is made again, cleared.
bool ensure_graph(D3D11EffectOwner* owner, ID3D11Device* device, ID3D11DeviceContext* context, u32 width,
                  u32 height) noexcept {
    static constexpr DXGI_FORMAT kFormats[3]{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                             DXGI_FORMAT_R32G32B32A32_FLOAT};
    for (u32 i = 0; i < owner->graph.target_count; ++i) {
        const EffectGraphTarget& spec = owner->graph.targets[i];
        D3D11EffectGraphTarget& target = owner->graph_targets[i];
        const u32 target_width = effect_target_extent(spec, width, false);
        const u32 target_height = effect_target_extent(spec, height, true);
        if (target.views[0] && target.width == target_width && target.height == target_height)
            continue;
        release_graph_target(target);
        for (u32 plane = 0; plane <= u32{spec.persistent}; ++plane)
            if (!make_target(device, context, target_width, target_height, kFormats[static_cast<u32>(spec.format)],
                             &target.views[plane], &target.reads[plane], nullptr)) {
                release_graph_target(target);
                return false;
            }
        target.width = target_width;
        target.height = target_height;
    }
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

// One frame into `view` (the output, `width` x `height`): the package's pixel shader over it, or each pass of the
// graph in order. A graph's vertex shaders see what its pixel shaders see.
void draw_frame(D3D11EffectOwner* owner, ID3D11DeviceContext* context, ID3D11RenderTargetView* view, u32 width,
                u32 height, EffectFrameConstants& constants) noexcept {
    static constexpr EffectGraphPass kSingle{0, 0, kEffectGraphOutput, 0, EffectPassBlend::opaque,
                                             EffectPassDraw::screen, false, {}};
    const EffectGraph& graph = owner->graph;
    const bool graphed = graph.pass_count != 0;
    bind_pass(owner, context);
    if (graphed) {
        context->VSSetConstantBuffers(0, 1, &owner->frame_buffer);
        context->VSSetSamplers(0, 3, owner->samplers);
        context->VSSetShaderResources(0, kD3D11EffectInputs, owner->inputs);
    }
    for (u32 i = 0; i < (graphed ? graph.pass_count : 1u); ++i) {
        const EffectGraphPass& pass = graphed ? graph.passes[i] : kSingle;
        ID3D11RenderTargetView* target = view;
        u32 target_width = width, target_height = height, swap = 0;
        D3D11EffectGraphTarget* written = nullptr;
        ID3D11ShaderResourceView* reads[kEffectGraphTargets]{};
        for (u32 t = 0; t < graph.target_count; ++t)
            if (pass.inputs >> t & 1)
                reads[t] = owner->graph_targets[t].reads[owner->graph_targets[t].current];
        if (pass.target != kEffectGraphOutput) {
            // A persistent target draws its other plane: the pass reads the last state and writes the next.
            written = &owner->graph_targets[pass.target];
            swap = graph.targets[pass.target].persistent;
            target = written->views[written->current ^ swap];
            target_width = written->width;
            target_height = written->height;
        }
        if (graphed) {
            // The last pass's inputs leave before their texture can be this pass's target.
            ID3D11ShaderResourceView* none[kEffectGraphTargets]{};
            context->PSSetShaderResources(kD3D11EffectInputs, kEffectGraphTargets, none);
            context->VSSetShaderResources(kD3D11EffectInputs, kEffectGraphTargets, none);
        }
        context->OMSetRenderTargets(1, &target, nullptr);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(target_width), static_cast<float>(target_height), 0, 1};
        context->RSSetViewports(1, &viewport);
        if (graphed) {
            const float clear[4]{};
            if (pass.clear)
                context->ClearRenderTargetView(target, clear);
            context->PSSetShaderResources(kD3D11EffectInputs, kEffectGraphTargets, reads);
            context->VSSetShaderResources(kD3D11EffectInputs, kEffectGraphTargets, reads);
            context->VSSetShader(pass.vertex ? owner->graph_shaders[pass.vertex].vertex : owner->target_vertex, nullptr, 0);
            context->PSSetShader(pass.pixel ? owner->graph_shaders[pass.pixel].pixel : owner->shader, nullptr, 0);
            context->OMSetBlendState(pass.blend == EffectPassBlend::opaque ? nullptr
                                                                           : owner->blends[2 + static_cast<u32>(pass.blend)],
                                     nullptr, 0xFFFFFFFFu);
            constants.pass[0] = static_cast<float>(target_width);
            constants.pass[1] = static_cast<float>(target_height);
            constants.pass[2] = static_cast<float>(pass.count[0]);
            constants.pass[3] = static_cast<float>(pass.count[1]);
        }
        context->UpdateSubresource(owner->frame_buffer, 0, nullptr, &constants, 0, 0);
        if (pass.draw == EffectPassDraw::screen) {
            draw_strips(context, target_width, target_height);
        } else {
            // grid(W, H): a strip of 2W vertices per row of cells; quads(N): N strips of four.
            const D3D11_RECT scissor{0, 0, static_cast<LONG>(target_width), static_cast<LONG>(target_height)};
            const bool grid = pass.draw == EffectPassDraw::grid;
            context->RSSetScissorRects(1, &scissor);
            context->DrawInstanced(grid ? 2u * pass.count[0] : 4u, grid ? pass.count[1] - 1u : pass.count[0], 0, 0);
        }
        if (written)
            written->current ^= swap;
    }
}

void unbind_pass(ID3D11DeviceContext* context, bool graph) noexcept {
    constexpr u32 kSlots = kD3D11EffectInputs + kEffectGraphTargets;
    ID3D11ShaderResourceView* views[kSlots]{};
    context->PSSetShaderResources(0, graph ? kSlots : kD3D11EffectInputs, views);
    if (graph)
        context->VSSetShaderResources(0, kSlots, views);
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
// One frame time (ms) of the package (its whole graph) on a `width` x `height` output, or a negative value when
// the device cannot time it. Runs frames synchronously (selection is a cold path) and gives the GPU a bounded
// time to answer.
float probe_size(D3D11EffectOwner* owner, ID3D11Device* device, ID3D11DeviceContext* context, u32 width,
                 u32 height) noexcept {
    ID3D11RenderTargetView* view = nullptr;
    float result = -1;
    D3D11EffectTiming timing{};
    if (make_target(device, context, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, &view, nullptr, nullptr) &&
        ensure_graph(owner, device, context, width, height) && create_timing(timing, device)) {
        EffectFrameConstants constants{};
        constants.dip = 1;
        constants.resolution[0] = static_cast<float>(width);
        constants.resolution[1] = static_cast<float>(height);
        constants.pointer[0] = constants.pointer[1] = -1;
        constants.intensity = 1;
        constants.dark = 1;
        effect_palette(kDefaultTheme, constants.palette);
        copy_bytes(constants.knobs, owner->knobs, sizeof(constants.knobs));
        // The first frame pays for the driver's lazy state; the next three are timed.
        constexpr u32 kTimedFrames = 3;
        for (u32 frame = 0; frame <= kTimedFrames; ++frame) {
            constants.time = constants.frame = static_cast<float>(frame);
            if (frame == 1) {
                context->Begin(timing.disjoint);
                context->End(timing.begin);
            }
            draw_frame(owner, context, view, width, height, constants);
        }
        context->End(timing.end);
        context->End(timing.disjoint);
        unbind_pass(context, owner->graph.pass_count != 0);
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
        // The package is built: only the user's side and the knob values can have moved. New knob values reach the
        // next frame through its inputs (d3d11_effect_begin_frame), so a running clock draws them on its next tick.
        bool changed = false;
        if (!bytes_same(owner->knobs, spec.knobs, sizeof(owner->knobs))) {
            copy_bytes(owner->knobs, spec.knobs, sizeof(owner->knobs));
            changed = true;
        }
        if (!bytes_same(&owner->settings, &settings, sizeof(settings))) {
            const bool restart = owner->settings.resolution_step != settings.resolution_step ||
                                 owner->settings.budget != settings.budget;
            owner->settings = settings;
            owner->dirty = changed = true;
            if (restart)
                start_controller(owner);
        }
        return result(S_OK, changed ? EffectSelect::changed : EffectSelect::unchanged);
    }
    if (!same)
        d3d11_effect_clear(owner);
    // The package's shaders (a graph's: shader 0 and the rest, each of its stage) and its inputs. A graph is one
    // mcfx_parse accepted.
    if (!create_pass_objects(owner, device))
        return result(E_FAIL, EffectSelect::rejected);
    const EffectGraph* graph = spec.graph && spec.shaders ? spec.graph : nullptr;
    ID3D11PixelShader* shader = nullptr;
    D3D11EffectStage stages[kEffectGraphShaders]{};
    ID3D11ShaderResourceView* inputs[kD3D11EffectInputs]{};
    HRESULT made = device->CreatePixelShader(spec.bytecode.data, spec.bytecode.count, nullptr, &shader);
    for (u32 i = 1; graph && SUCCEEDED(made) && i < minimum<u32>(graph->shader_count, kEffectGraphShaders); ++i) {
        const ConstSlice<u8>& code = spec.shaders[i];
        made = graph->vertex_shaders >> i & 1
                   ? device->CreateVertexShader(code.data, code.count, nullptr, &stages[i].vertex)
                   : device->CreatePixelShader(code.data, code.count, nullptr, &stages[i].pixel);
    }
    if (SUCCEEDED(made))
        made = effect_inputs_create(device, spec, glyph_view, inputs);
    if (FAILED(made)) {
        com_release(shader);
        for (auto& stage : stages)
            com_release(stage.vertex);
        d3d11_effect_clear(owner);
        return result(made, EffectSelect::rejected);
    }
    for (u32 i = 0; i < kD3D11EffectInputs; ++i) {
        com_release(owner->inputs[i]);
        owner->inputs[i] = inputs[i];
    }
    com_release(owner->shader);
    owner->shader = shader;
    copy_bytes(owner->graph_shaders, stages, sizeof(stages));
    owner->graph = graph ? *graph : EffectGraph{};
    owner->selected = true;
    owner->identity = spec.identity;
    owner->adapter_key = adapter_key;
    owner->uses = spec.uses;
    owner->settings = settings;
    copy_bytes(owner->knobs, spec.knobs, sizeof(owner->knobs));
    if (!same) {
        owner->last_phase = owner->time = owner->frames = 0;
        owner->pointer[0] = owner->pointer[1] = -1;
        for (float& value : owner->ambient)
            value = 0;
        owner->light = 0;
        owner->verdict = EffectVerdict::none;
    }
    owner->probed = run_probe(owner, device, context);
    // The probe's frames leave no state behind: the graph's targets are made again, cleared, for the first frame.
    release_graph(owner, false);
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
    // The shaders and the inputs belong to the package; the pass's own objects stay for the next one.
    com_release(owner->shader);
    for (auto*& view : owner->inputs)
        com_release(view);
    release_target(owner);
    release_graph(owner, true);
    owner->graph = {};
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
                              const Theme& theme, float phase, bool clock_running, u32 content_width,
                              u32 content_height, u32 caption_height, u32 dpi, bool live_resize, u64 now_ms) noexcept {
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
    // Adapted to a light appearance, the effect draws for the theme's dark twin (effect_light_palette).
    effect_light_palette(constants.palette, owner->light);
    constants.dark = theme.appearance == ThemeAppearance::light ? owner->light : 1.f;
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
    // mc_ambient: the clock and the appearance only when the package declares them.
    const bool clock = (owner->uses & effect_use_clock) != 0;
    for (u32 i = effect_ambient_day; i <= effect_ambient_month; ++i)
        constants.ambient[i] = clock ? owner->ambient[i] : 0.f;
    constants.ambient[effect_ambient_dark] =
        (owner->uses & effect_use_appearance) != 0 ? owner->ambient[effect_ambient_dark] : 0.f;
    // What the picture depends on besides the clock: palette, flags, knobs, ambient inputs, size and DPI. While the
    // clock runs, a change of inputs waits for its next tick, so every frame the effect draws moves time on.
    const bool inputs_changed =
        !bytes_same(constants.palette, owner->constants.palette, sizeof(constants.palette)) ||
        constants.dark != owner->constants.dark || constants.intensity != owner->constants.intensity ||
        !bytes_same(constants.knobs, owner->constants.knobs, sizeof(constants.knobs)) ||
        !bytes_same(constants.ambient, owner->constants.ambient, sizeof(constants.ambient)) ||
        !bytes_same(constants.pointer, owner->constants.pointer, sizeof(constants.pointer));
    const bool repaint = !owner->content_valid || owner->dirty || used_width != owner->used_width ||
                         used_height != owner->used_height || dpi != owner->dpi || (inputs_changed && !clock_running);
    // A window being dragged keeps the last picture, stretched.
    if ((repaint || phase != owner->last_phase) && !(live_resize && owner->content_valid)) {
        if (!ensure_target(owner, device, context, used_width, used_height) ||
            !ensure_graph(owner, device, context, used_width, used_height)) {
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
        // Slot 0 may still hold the target from the last frame's canvases.
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        if (timed) {
            context->Begin(timing.disjoint);
            context->End(timing.begin);
        }
        draw_frame(owner, context, owner->target_view, used_width, used_height, constants);
        if (timed) {
            context->End(timing.end);
            context->End(timing.disjoint);
            timing.identity = owner->identity;
            timing.frame_ms = now_ms;
            timing.pending = true;
            owner->timing_cursor = (owner->timing_cursor + 1) % kD3D11EffectTimingFrames;
        }
        unbind_pass(context, owner->graph.pass_count != 0);
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
    const float opacity = owner->layer_opacity;
    const float factor = opacity >= 0 && opacity <= 1 ? opacity : 1;
    const u32 blend = static_cast<u32>(owner->settings.blend);
    const float light = owner->light;
    const EffectCompositeConstants constants{{canvas.left, canvas.top, canvas.right, canvas.bottom},
                                             {scale_x, scale_y, offset_x, offset_y},
                                             {.5f / allocated_width, .5f / allocated_height,
                                              (used_width - .5f) / allocated_width,
                                              (used_height - .5f) / allocated_height},
                                             owner->settings.opacity,
                                             tile_radius > 0 ? tile_radius : 0.f,
                                             light,
                                             factor,
                                             static_cast<float>(blend),
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
    const float blend_factor[4]{factor, factor, factor, factor};
    // Adapted, screen and add take their light from the background: while they cross-fade the shader's second
    // output weighs it, and once settled a plain multiply does.
    const u32 state = light > 0 && blend != 0 ? (light < 1 ? 6u : 7u) : blend;
    context->OMSetBlendState(owner->blends[state], blend_factor, 0xFFFFFFFFu);
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
