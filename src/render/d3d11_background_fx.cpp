#include "maicord/render/d3d11_background_fx.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/shader_bytecode.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

u64 code_hash(ConstSlice<u8> code) noexcept {
    u64 value = 0xcbf29ce484222325ull ^ code.count;
    for (usize i = 0; i < code.count; ++i)
        value = (value ^ code.data[i]) * 0x100000001b3ull;
    return value;
}

void release_timing(D3D11BackgroundFxTiming& timing) noexcept {
    com_release(timing.disjoint);
    for (auto*& stamp : timing.stamps)
        com_release(stamp);
    reset_value(&timing);
}

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

bool create_timing(D3D11BackgroundFxTiming& timing, ID3D11Device* device) noexcept {
    if (timing.disjoint)
        return true;
    const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    const D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
    bool ok = SUCCEEDED(device->CreateQuery(&disjoint, &timing.disjoint));
    for (auto*& query : timing.stamps)
        ok = ok && SUCCEEDED(device->CreateQuery(&stamp, &query));
    if (!ok)
        release_timing(timing);
    return ok;
}

// Reads finished frames without flushing; a frame still in flight stays for
// a later call. Feeds the guard; a trip turns the plugin effect off.
void collect(D3D11BackgroundFxOwner* owner, ID3D11DeviceContext* context) noexcept {
    for (auto& timing : owner->timing) {
        if (!timing.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        const HRESULT state =
            context->GetData(timing.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (state == S_FALSE)
            continue;
        u64 ticks = 0;
        bool ready = SUCCEEDED(state), waiting = false;
        for (u32 i = 0; ready && i < timing.draws; ++i) {
            u64 begin = 0, end = 0;
            const HRESULT a = context->GetData(timing.stamps[i * 2], &begin, sizeof(begin),
                                               D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const HRESULT b = context->GetData(timing.stamps[i * 2 + 1], &end, sizeof(end),
                                               D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (a == S_FALSE || b == S_FALSE) {
                waiting = true;
                break;
            }
            ready = SUCCEEDED(a) && SUCCEEDED(b) && end >= begin;
            ticks += ready ? end - begin : 0;
        }
        if (waiting)
            continue;
        timing.pending = false;
        if (!ready || disjoint.Disjoint || !disjoint.Frequency || !owner->plugin_shader ||
            timing.owner_id != owner->plugin_owner_id || timing.effect_id != owner->plugin_effect_id)
            continue;
        if (background_fx_guard_sample(&owner->guard,
                                       static_cast<float>(static_cast<double>(ticks) * 1000.0 /
                                                          static_cast<double>(disjoint.Frequency)),
                                       timing.frame_ms)) {
            owner->tripped_owner_id = owner->plugin_owner_id;
            owner->tripped_effect_id = owner->plugin_effect_id;
            owner->tripped = true;
            log_u64(LogLevel::warning, "render", "background_fx_guard_owner", owner->plugin_owner_id);
            d3d11_background_fx_clear_plugin(owner);
            return;
        }
    }
}

} // namespace

HRESULT d3d11_background_fx_select_plugin(D3D11BackgroundFxOwner* owner, ID3D11Device* device, u64 owner_id,
                                          u64 effect_id, u64 revision, ConstSlice<u8> bytecode,
                                          const float* parameters, float phase_seconds, bool* changed) noexcept {
    if (changed)
        *changed = false;
    if (!owner || !device || !owner_id || !effect_id || !bytecode.data || !bytecode.count || !parameters)
        return E_INVALIDARG;
    const bool same = owner->plugin_shader && owner->plugin_owner_id == owner_id &&
                      owner->plugin_effect_id == effect_id;
    if (same && owner->plugin_revision == revision)
        return S_OK;
    const u64 hash = code_hash(bytecode);
    bool updated = false;
    if (!same || hash != owner->plugin_code_hash) {
        ID3D11PixelShader* shader = nullptr;
        const HRESULT result = device->CreatePixelShader(bytecode.data, bytecode.count, nullptr, &shader);
        if (FAILED(result)) {
            if (d3d11_background_fx_clear_plugin(owner) && changed)
                *changed = true;
            return result;
        }
        com_release(owner->plugin_shader);
        owner->plugin_shader = shader;
        owner->plugin_code_hash = hash;
        if (!same) {
            owner->plugin_owner_id = owner_id;
            owner->plugin_effect_id = effect_id;
            owner->plugin_time_base = owner->plugin_last_phase = phase_seconds;
            reset_value(&owner->guard);
        }
        updated = true;
    }
    if (!bytes_equal(owner->plugin_parameters, parameters, sizeof(owner->plugin_parameters))) {
        copy_bytes(owner->plugin_parameters, parameters, sizeof(owner->plugin_parameters));
        updated = true;
    }
    owner->plugin_revision = revision;
    if (changed)
        *changed = updated;
    return S_OK;
}

bool d3d11_background_fx_clear_plugin(D3D11BackgroundFxOwner* owner) noexcept {
    if (!owner || !owner->plugin_shader)
        return false;
    com_release(owner->plugin_shader);
    owner->plugin_owner_id = owner->plugin_effect_id = owner->plugin_code_hash = 0;
    owner->plugin_revision = 0;
    reset_value(&owner->guard);
    if (owner->source == BackgroundFxSource::plugin)
        owner->source = BackgroundFxSource::none;
    return true;
}

bool d3d11_background_fx_begin_frame(D3D11BackgroundFxOwner* owner, ID3D11Device* device,
                                     ID3D11DeviceContext* context, const Theme& theme, float phase_seconds,
                                     u32 width, u32 height, u32 dpi, u64 now_ms) noexcept {
    if (!owner || !device || !context)
        return false;
    owner->source = BackgroundFxSource::none;
    owner->drawn_count = 0;
    owner->draws_last_frame = 0;
    owner->vertex_constants_bound = false;
    owner->layer_opacity = 1;
    owner->timing_frame = nullptr;
    collect(owner, context);
    const bool builtin = background_fx_builtin_mask(theme.background.effect) &&
                         background_effects_active(theme.background);
    if ((!owner->plugin_shader && !builtin) || owner->failed || !width || !height)
        return true;
    if (!create_resources(owner, device)) {
        // Effects are decoration: the frame draws without them.
        owner->failed = true;
        return true;
    }
    const float scale = static_cast<float>(dpi ? dpi : 96) / 96.f;
    const float phase = phase_seconds >= 0 && phase_seconds <= 3.402823466e38f ? phase_seconds : 0;
    owner->frame_pixel_scale = scale;
    owner->frame_window_width = width / scale;
    owner->frame_window_height = height / scale;
    const UiRect unit{0, 0, 1, 1};
    if (owner->plugin_shader) {
        const float delta = phase - owner->plugin_last_phase;
        owner->plugin_last_phase = phase;
        if (phase < owner->plugin_time_base)
            owner->plugin_time_base = phase;
        if (!background_fx_pack(&owner->frame, theme, phase - owner->plugin_time_base, delta, unit, scale))
            return true;
        copy_bytes(owner->frame.parameters, owner->plugin_parameters, sizeof(owner->frame.parameters));
        owner->source = BackgroundFxSource::plugin;
        auto& timing = owner->timing[owner->timing_cursor];
        if (!timing.pending && create_timing(timing, device)) {
            timing.owner_id = owner->plugin_owner_id;
            timing.effect_id = owner->plugin_effect_id;
            timing.frame_ms = now_ms;
            timing.draws = 0;
            timing.begun = false;
            owner->timing_frame = &timing;
        }
        return true;
    }
    if (background_fx_pack_builtin(&owner->frame, theme, phase, unit, scale, owner->frame_window_width,
                                   owner->frame_window_height))
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
    owner->drawn[owner->drawn_count++] = quad_index;
    frame.resolution[0] = (canvas.right - canvas.left) * scale;
    frame.resolution[1] = (canvas.bottom - canvas.top) * scale;
    frame.origin[0] = canvas.left * scale;
    frame.origin[1] = canvas.top * scale;
    frame.canvas[0] = canvas.left;
    frame.canvas[1] = canvas.top;
    frame.canvas[2] = canvas.right;
    frame.canvas[3] = canvas.bottom;
    context->UpdateSubresource(owner->constant_buffer, 0, nullptr, &frame, 0, 0);
    const bool plugin = owner->source == BackgroundFxSource::plugin;
    // The strip topology and the bound input layout stay: the vertex shader reads only SV_VertexID.
    context->VSSetShader(owner->vertex_shader, nullptr, 0);
    // VS b2 is the effect's alone: bound once per frame.
    if (!owner->vertex_constants_bound) {
        context->VSSetConstantBuffers(2, 1, &owner->constant_buffer);
        owner->vertex_constants_bound = true;
    }
    context->PSSetShader(plugin ? owner->plugin_shader : owner->builtin_shader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &owner->constant_buffer);
    // Plugin shaders get no textures or samplers; the built-in shader samples none.
    if (plugin) {
        ID3D11ShaderResourceView* no_view = nullptr;
        context->PSSetShaderResources(0, 1, &no_view);
    }
    const float opacity = owner->layer_opacity;
    const float factor = opacity >= 0 && opacity <= 1 ? opacity : 1;
    const float blend_factor[4]{factor, factor, factor, factor};
    context->OMSetBlendState(owner->blend_state, blend_factor, 0xFFFFFFFFu);
    auto* timing = plugin ? owner->timing_frame : nullptr;
    if (timing && timing->draws < kD3D11BackgroundFxTimedDraws) {
        if (!timing->begun) {
            context->Begin(timing->disjoint);
            timing->begun = true;
        }
        context->End(timing->stamps[timing->draws * 2]);
        context->Draw(4, 0);
        context->End(timing->stamps[timing->draws * 2 + 1]);
        ++timing->draws;
    } else {
        context->Draw(4, 0);
    }
    ++owner->draws_last_frame;
    return true;
}

void d3d11_background_fx_end_frame(D3D11BackgroundFxOwner* owner, ID3D11DeviceContext* context) noexcept {
    if (!owner || !context)
        return;
    auto* timing = owner->timing_frame;
    owner->timing_frame = nullptr;
    if (!timing || !timing->begun)
        return;
    context->End(timing->disjoint);
    timing->pending = true;
    owner->timing_cursor = (owner->timing_cursor + 1) % kD3D11BackgroundFxTimingFrames;
}

void d3d11_background_fx_release(D3D11BackgroundFxOwner* owner) noexcept {
    if (!owner)
        return;
    for (auto& timing : owner->timing)
        release_timing(timing);
    com_release(owner->plugin_shader);
    com_release(owner->blend_state);
    com_release(owner->constant_buffer);
    com_release(owner->builtin_shader);
    com_release(owner->vertex_shader);
    reset_value(owner);
}

} // namespace maicord
