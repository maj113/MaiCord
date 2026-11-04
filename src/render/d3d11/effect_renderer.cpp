#include "maicord/render/d3d11/effect_renderer.hpp"
#include "maicord/render/effects/background_fx.hpp"

#include "effect_inputs.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/os/thread.hpp"

namespace maicord {
namespace {

void release_timing(D3D11EffectTiming& timing) noexcept {
    gpu_timer_release(&timing.timer);
    reset_value(&timing);
}

bool create_timing(D3D11EffectTiming& timing, GpuDevice* device) noexcept {
    if (gpu_timer_valid(timing.timer))
        return true;
    const bool ok = SUCCEEDED(gpu_timer_create(device, &timing.timer));
    if (!ok)
        release_timing(timing);
    return ok;
}

void release_target(D3D11EffectOwner* owner) noexcept {
    gpu_release(owner->target_srv);
    gpu_release(owner->target_view);
    gpu_release(owner->target);
    owner->target_width = owner->target_height = 0;
    owner->content_valid = false;
}

void release_graph_target(D3D11EffectGraphTarget& target) noexcept {
    for (u32 plane = 0; plane < 2; ++plane) {
        gpu_release(target.views[plane]);
        gpu_release(target.reads[plane]);
    }
    target = {};
}

// A graph's targets (made again, cleared, on the next frame) and, with `shaders`, its shaders.
void release_graph(D3D11EffectOwner* owner, bool shaders) noexcept {
    for (auto& target : owner->graph_targets)
        release_graph_target(target);
    if (shaders)
        for (auto& stage : owner->graph_shaders)
            gpu_release(stage.vertex);
}

// Everything but the selection: the next select (or begin_frame, after a failure) builds it again.
void release_objects(D3D11EffectOwner* owner) noexcept {
    release_target(owner);
    release_graph(owner, true);
    for (auto& timing : owner->timing)
        release_timing(timing);
    for (auto*& view : owner->inputs)
        gpu_release(view);
    for (auto*& state : owner->blends)
        gpu_release(state);
    for (auto*& state : owner->samplers)
        gpu_release(state);
    gpu_release(owner->rasterizer);
    gpu_release(owner->composite_buffer);
    gpu_release(owner->frame_buffer);
    gpu_release(owner->shader);
    gpu_release(owner->composite_pixel);
    gpu_release(owner->composite_vertex);
    gpu_release(owner->target_vertex);
    owner->composite_bound = false;
    owner->dirty = true;
}

void mark_idle(D3D11EffectOwner* owner) noexcept {
    if (owner->marker)
        *owner->marker = 0;
}

bool create_blend_states(D3D11EffectOwner* owner, GpuDevice* device) noexcept {
    // Source, destination, source alpha, destination alpha. The composite's modes first: the target holds
    // premultiplied colour, so every mode adds the source at the user's opacity (the blend factor) and weighs what
    // is behind it: normal by what the source leaves open, screen by one minus the source colour, add by one; only
    // normal composes alpha. Then a graph pass's alpha, add and premultiplied. Last the adapted screen and add: while
    // they cross-fade, the source plus what is behind weighed by the shader's second output; settled, what is behind
    // times the source.
    static constexpr GpuBlendFactor kModes[8][4]{
        {GpuBlendFactor::blend_factor, GpuBlendFactor::inv_src_alpha, GpuBlendFactor::blend_factor,
         GpuBlendFactor::inv_src_alpha},
        {GpuBlendFactor::blend_factor, GpuBlendFactor::inv_src_color, GpuBlendFactor::zero, GpuBlendFactor::one},
        {GpuBlendFactor::blend_factor, GpuBlendFactor::one, GpuBlendFactor::zero, GpuBlendFactor::one},
        {GpuBlendFactor::src_alpha, GpuBlendFactor::inv_src_alpha, GpuBlendFactor::one, GpuBlendFactor::inv_src_alpha},
        {GpuBlendFactor::one, GpuBlendFactor::one, GpuBlendFactor::one, GpuBlendFactor::one},
        {GpuBlendFactor::one, GpuBlendFactor::inv_src_alpha, GpuBlendFactor::one, GpuBlendFactor::inv_src_alpha},
        {GpuBlendFactor::blend_factor, GpuBlendFactor::src1_color, GpuBlendFactor::zero, GpuBlendFactor::one},
        {GpuBlendFactor::zero, GpuBlendFactor::src_color, GpuBlendFactor::zero, GpuBlendFactor::one}};
    for (u32 mode = 0; mode < 8; ++mode) {
        if (FAILED(gpu_blend_create(device, {true, kModes[mode][0], kModes[mode][1], kModes[mode][2], kModes[mode][3]},
                                    &owner->blends[mode])))
            return false;
    }
    return true;
}

bool create_sampler_states(D3D11EffectOwner* owner, GpuDevice* device) noexcept {
    // mc_point_wrap, mc_linear_wrap, mc_linear_clamp.
    const GpuSamplerDesc samplers[3]{{GpuFilter::point, GpuAddress::wrap},
                                     {GpuFilter::linear, GpuAddress::wrap},
                                     {GpuFilter::linear, GpuAddress::clamp}};
    for (u32 i = 0; i < 3; ++i) {
        if (FAILED(gpu_sampler_create(device, samplers[i], &owner->samplers[i])))
            return false;
    }
    return true;
}

bool create_buffer(GpuDevice* device, u32 bytes, GpuBuffer** buffer) noexcept {
    return SUCCEEDED(gpu_buffer_create(device, {bytes, GpuUsage::default_, gpu_bind_constant, false}, nullptr, buffer));
}

// Shaders, states and constants: independent of the package.
bool create_pass_objects(D3D11EffectOwner* owner, GpuDevice* device) noexcept {
    if (owner->target_vertex && owner->composite_vertex && owner->composite_pixel && owner->frame_buffer &&
        owner->composite_buffer && owner->rasterizer && owner->blends[7] && owner->samplers[2])
        return true;
    bool layout_failed = false;
    GpuStatus result = owner->target_vertex ? S_OK
                                          : gpu_vertex_shader_create(device, ShaderId::fx_target_vertex, {},
                                                                     &owner->target_vertex, nullptr, &layout_failed);
    if (SUCCEEDED(result) && !owner->composite_vertex)
        result = gpu_vertex_shader_create(device, ShaderId::fx_composite_vertex, {}, &owner->composite_vertex,
                                          nullptr, &layout_failed);
    if (SUCCEEDED(result) && !owner->composite_pixel)
        result = gpu_pixel_shader_create(device, ShaderId::fx_composite_pixel, &owner->composite_pixel);
    bool ok = SUCCEEDED(result) && (owner->frame_buffer || create_buffer(device, sizeof(EffectFrameConstants), &owner->frame_buffer)) &&
              (owner->composite_buffer ||
               create_buffer(device, sizeof(EffectCompositeConstants), &owner->composite_buffer));
    if (ok && !owner->rasterizer) {
        ok = SUCCEEDED(gpu_raster_create(device, &owner->rasterizer));
    }
    ok = ok && (owner->blends[7] || create_blend_states(owner, device)) &&
         (owner->samplers[2] || create_sampler_states(owner, device));
    if (!ok)
        log_u64(LogLevel::error, "render", "effect_pass_resource_error", static_cast<u32>(result));
    return ok;
}

// A render target cleared to transparent black, with a shader view when `read` is given; the texture stays with
// the views unless `keep` takes it. False leaves what was made in the outputs, for the caller to release.
bool make_target(GpuDevice* device, GpuContext* context, u32 width, u32 height, GpuFormat format,
                 GpuRtv** view, GpuSrv** read, GpuTexture** keep) noexcept {
    GpuTexture* texture = nullptr;
    const u8 bind = read ? static_cast<u8>(gpu_bind_rtv | gpu_bind_srv) : static_cast<u8>(gpu_bind_rtv);
    const bool ok = SUCCEEDED(gpu_texture_create(device, {width, height, format, GpuUsage::default_, bind}, nullptr,
                                                 &texture)) &&
                    SUCCEEDED(gpu_rtv_create(device, texture, view)) &&
                    (!read || SUCCEEDED(gpu_srv_create(device, texture, {GpuViewKind::texture, format}, read)));
    if (ok) {
        const float clear[4]{};
        gpu_clear_rtv(context, *view, clear);
    }
    if (keep)
        *keep = texture;
    else
        gpu_release(texture);
    return ok;
}

bool ensure_target(D3D11EffectOwner* owner, GpuDevice* device, GpuContext* context, u32 width,
                   u32 height) noexcept {
    const u32 bucket = kD3D11EffectTargetBucket;
    const u32 allocated_width = (width + bucket - 1) / bucket * bucket;
    const u32 allocated_height = (height + bucket - 1) / bucket * bucket;
    if (owner->target && owner->target_width == allocated_width && owner->target_height == allocated_height)
        return true;
    release_target(owner);
    // Unused texels beyond the viewport stay transparent.
    if (!make_target(device, context, allocated_width, allocated_height, GpuFormat::rgba8,
                     &owner->target_view, &owner->target_srv, &owner->target)) {
        release_target(owner);
        return false;
    }
    owner->target_width = allocated_width;
    owner->target_height = allocated_height;
    return true;
}

// The graph's targets for an output of `width` x `height`: a target whose size changes is made again, cleared.
bool ensure_graph(D3D11EffectOwner* owner, GpuDevice* device, GpuContext* context, u32 width,
                  u32 height) noexcept {
    static constexpr GpuFormat kFormats[3]{GpuFormat::rgba8, GpuFormat::rgba16f, GpuFormat::rgba32f};
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
void bind_pass(D3D11EffectOwner* owner, GpuContext* context) noexcept {
    gpu_set_strip_topology(context);
    gpu_set_input_layout(context, nullptr);
    gpu_set_raster(context, owner->rasterizer);
    gpu_set_blend(context, nullptr, nullptr);
    gpu_set_shader(context, owner->target_vertex);
    gpu_set_shader(context, owner->shader);
    gpu_set_constants(context, GpuStage::pixel, 0, 1, &owner->frame_buffer);
    gpu_set_samplers(context, GpuStage::pixel, 0, 3, owner->samplers);
    gpu_set_srvs(context, GpuStage::pixel, 0, kD3D11EffectInputs, owner->inputs);
}

// The pass's draws: the whole target in kD3D11EffectStrips horizontal strips, so no single draw runs long.
void draw_strips(GpuContext* context, u32 width, u32 height) noexcept {
    const u32 strip = (height + kD3D11EffectStrips - 1) / kD3D11EffectStrips;
    for (u32 top = 0; top < height; top += strip) {
        const GpuRect scissor{0, static_cast<i32>(top), static_cast<i32>(width),
                              static_cast<i32>(minimum(top + strip, height))};
        gpu_set_scissor(context, scissor);
        gpu_draw(context, 4, 0);
    }
}

// One frame into `view` (the output, `width` x `height`): the package's pixel shader over it, or each pass of the
// graph in order. A graph's vertex shaders see what its pixel shaders see.
void draw_frame(D3D11EffectOwner* owner, GpuContext* context, GpuRtv* view, u32 width,
                u32 height, EffectFrameConstants& constants) noexcept {
    static constexpr EffectGraphPass kSingle{0, 0, kEffectGraphOutput, 0, EffectPassBlend::opaque,
                                             EffectPassDraw::screen, false, {}};
    const EffectGraph& graph = owner->graph;
    const bool graphed = graph.pass_count != 0;
    bind_pass(owner, context);
    if (graphed) {
        gpu_set_constants(context, GpuStage::vertex, 0, 1, &owner->frame_buffer);
        gpu_set_samplers(context, GpuStage::vertex, 0, 3, owner->samplers);
        gpu_set_srvs(context, GpuStage::vertex, 0, kD3D11EffectInputs, owner->inputs);
    }
    for (u32 i = 0; i < (graphed ? graph.pass_count : 1u); ++i) {
        const EffectGraphPass& pass = graphed ? graph.passes[i] : kSingle;
        GpuRtv* target = view;
        u32 target_width = width, target_height = height, swap = 0;
        D3D11EffectGraphTarget* written = nullptr;
        GpuSrv* reads[kEffectGraphTargets]{};
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
            GpuSrv* none[kEffectGraphTargets]{};
            gpu_set_srvs(context, GpuStage::pixel, kD3D11EffectInputs, kEffectGraphTargets, none);
            gpu_set_srvs(context, GpuStage::vertex, kD3D11EffectInputs, kEffectGraphTargets, none);
        }
        gpu_set_render_target(context, target);
        gpu_set_viewport(context, 0, 0, static_cast<float>(target_width), static_cast<float>(target_height));
        if (graphed) {
            const float clear[4]{};
            if (pass.clear)
                gpu_clear_rtv(context, target, clear);
            gpu_set_srvs(context, GpuStage::pixel, kD3D11EffectInputs, kEffectGraphTargets, reads);
            gpu_set_srvs(context, GpuStage::vertex, kD3D11EffectInputs, kEffectGraphTargets, reads);
            gpu_set_shader(context, pass.vertex ? owner->graph_shaders[pass.vertex].vertex : owner->target_vertex);
            gpu_set_shader(context, pass.pixel ? owner->graph_shaders[pass.pixel].pixel : owner->shader);
            gpu_set_blend(context,
                          pass.blend == EffectPassBlend::opaque ? nullptr
                                                                : owner->blends[2 + static_cast<u32>(pass.blend)],
                          nullptr);
            constants.pass[0] = static_cast<float>(target_width);
            constants.pass[1] = static_cast<float>(target_height);
            constants.pass[2] = static_cast<float>(pass.count[0]);
            constants.pass[3] = static_cast<float>(pass.count[1]);
        }
        gpu_update_buffer(context, owner->frame_buffer, &constants, sizeof(constants));
        if (pass.draw == EffectPassDraw::screen) {
            draw_strips(context, target_width, target_height);
        } else {
            // grid(W, H): a strip of 2W vertices per row of cells; quads(N): N strips of four.
            const GpuRect scissor{0, 0, static_cast<i32>(target_width), static_cast<i32>(target_height)};
            const bool grid = pass.draw == EffectPassDraw::grid;
            gpu_set_scissor(context, scissor);
            gpu_draw_instanced(context, grid ? 2u * pass.count[0] : 4u, grid ? pass.count[1] - 1u : pass.count[0], 0, 0);
        }
        if (written)
            written->current ^= swap;
    }
}

void unbind_pass(GpuContext* context, bool graph) noexcept {
    constexpr u32 kSlots = kD3D11EffectInputs + kEffectGraphTargets;
    GpuSrv* views[kSlots]{};
    gpu_set_srvs(context, GpuStage::pixel, 0, graph ? kSlots : kD3D11EffectInputs, views);
    if (graph)
        gpu_set_srvs(context, GpuStage::vertex, 0, kSlots, views);
    gpu_set_render_target(context, nullptr);
}

float budget_ms(const EffectSettings& settings) noexcept {
    return kEffectBudgetMs[static_cast<u32>(settings.budget)];
}

// Reads finished frames without flushing; a frame still in flight stays for a later call. Feeds the budget
// controller; an overload turns the effect off.
void collect(D3D11EffectOwner* owner, GpuContext* context) noexcept {
    bool pending = false;
    for (auto& timing : owner->timing) {
        if (!timing.pending)
            continue;
        double gpu_milliseconds = 0;
        const GpuPoll polled = gpu_timer_poll(context, &timing.timer, false, &gpu_milliseconds);
        if (polled == GpuPoll::pending) {
            pending = true;
            continue;
        }
        timing.pending = false;
        if (polled == GpuPoll::invalid || !owner->selected || timing.identity != owner->identity)
            continue;
        const float gpu_ms = static_cast<float>(gpu_milliseconds);
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
float probe_size(D3D11EffectOwner* owner, GpuDevice* device, GpuContext* context, u32 width,
                 u32 height) noexcept {
    GpuRtv* view = nullptr;
    float result = -1;
    D3D11EffectTiming timing{};
    if (make_target(device, context, width, height, GpuFormat::rgba8, &view, nullptr, nullptr) &&
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
                gpu_timer_begin(context, &timing.timer);
            }
            draw_frame(owner, context, view, width, height, constants);
        }
        gpu_timer_end(context, &timing.timer);
        unbind_pass(context, owner->graph.pass_count != 0);
        gpu_flush(context);
        double milliseconds = 0;
        const u64 deadline = clock_monotonic_milliseconds() + 250;
        GpuPoll polled = GpuPoll::pending;
        while ((polled = gpu_timer_poll(context, &timing.timer, true, &milliseconds)) == GpuPoll::pending &&
               clock_monotonic_milliseconds() < deadline)
            thread_sleep(1);
        if (polled == GpuPoll::ready)
            result = static_cast<float>(milliseconds / kTimedFrames);
    }
    release_timing(timing);
    gpu_release(view);
    return result;
}

// Fits the frame time at two sizes. False when the device could not time them: the effect then starts at
// the user's resolution and the controller does the rest.
bool run_probe(D3D11EffectOwner* owner, GpuDevice* device, GpuContext* context) noexcept {
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

EffectSelectResult d3d11_effect_select(D3D11EffectOwner* owner, GpuDevice* device, GpuContext* context,
                                       const BackgroundEffectSpec& spec, const EffectSettings& settings,
                                       u64 adapter_key, GpuSrv* glyph_view, u32 window_width,
                                       u32 window_height) noexcept {
    const auto result = [](GpuStatus error, EffectSelect state) noexcept { return EffectSelectResult{error, state}; };
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
    GpuPixelShader* shader = nullptr;
    D3D11EffectStage stages[kEffectGraphShaders]{};
    GpuSrv* inputs[kD3D11EffectInputs]{};
    GpuStatus made = gpu_pixel_shader_create_blob(device, spec.bytecode, &shader);
    for (u32 i = 1; graph && SUCCEEDED(made) && i < minimum<u32>(graph->shader_count, kEffectGraphShaders); ++i) {
        made = graph->vertex_shaders >> i & 1
                   ? gpu_vertex_shader_create_blob(device, spec.shaders[i], &stages[i].vertex)
                   : gpu_pixel_shader_create_blob(device, spec.shaders[i], &stages[i].pixel);
    }
    if (SUCCEEDED(made))
        made = effect_inputs_create(device, spec, glyph_view, inputs);
    if (FAILED(made)) {
        gpu_release(shader);
        for (auto& stage : stages)
            gpu_release(stage.vertex);
        d3d11_effect_clear(owner);
        return result(made, EffectSelect::rejected);
    }
    for (u32 i = 0; i < kD3D11EffectInputs; ++i) {
        gpu_release(owner->inputs[i]);
        owner->inputs[i] = inputs[i];
    }
    gpu_release(owner->shader);
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
    gpu_release(owner->shader);
    for (auto*& view : owner->inputs)
        gpu_release(view);
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

bool d3d11_effect_begin_frame(D3D11EffectOwner* owner, GpuDevice* device, GpuContext* context,
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
        GpuSrv* none = nullptr;
        gpu_set_srvs(context, GpuStage::pixel, 0, 1, &none);
        if (timed) {
            gpu_timer_begin(context, &timing.timer);
        }
        draw_frame(owner, context, owner->target_view, used_width, used_height, constants);
        if (timed) {
            gpu_timer_end(context, &timing.timer);
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

bool d3d11_effect_draw(D3D11EffectOwner* owner, GpuContext* context, UiRect canvas, u32 quad_index,
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
    gpu_update_buffer(context, owner->composite_buffer, &constants, sizeof(constants));
    // The strip topology, the layer transform and the bilinear sampler stay: the vertex shader reads only SV_VertexID.
    gpu_set_shader(context, owner->composite_vertex);
    if (!owner->composite_bound) {
        gpu_set_constants(context, GpuStage::vertex, 2, 1, &owner->composite_buffer);
        owner->composite_bound = true;
    }
    gpu_set_shader(context, owner->composite_pixel);
    gpu_set_constants(context, GpuStage::pixel, 0, 1, &owner->composite_buffer);
    gpu_set_srvs(context, GpuStage::pixel, 0, 1, &owner->target_srv);
    const float blend_factor[4]{factor, factor, factor, factor};
    // Adapted, screen and add take their light from the background: while they cross-fade the shader's second
    // output weighs it, and once settled a plain multiply does.
    const u32 state = light > 0 && blend != 0 ? (light < 1 ? 6u : 7u) : blend;
    gpu_set_blend(context, owner->blends[state], blend_factor);
    gpu_draw(context, 4, 0);
    ++owner->draws_last_frame;
    return true;
}

void d3d11_effect_end_frame(D3D11EffectOwner* owner, GpuContext* context) noexcept {
    if (!owner || !context || !owner->draws_last_frame)
        return;
    // Leave no view of the target bound: the next frame renders into it.
    GpuSrv* none = nullptr;
    gpu_set_srvs(context, GpuStage::pixel, 0, 1, &none);
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
