#include "background_theme_internal.hpp"
#include "maicord_effect_ascii_rain.hpp"
#include "maicord_effect_aurora.hpp"
#include "maicord_effect_bokeh.hpp"
#include "maicord_effect_day_night.hpp"
#include "maicord_effect_film_grain.hpp"
#include "maicord_effect_graph_count.hpp"
#include "maicord_effect_graph_fill.hpp"
#include "maicord_effect_graph_grid.hpp"
#include "maicord_effect_graph_half.hpp"
#include "maicord_effect_graph_quad.hpp"
#include "maicord_effect_graph_solid.hpp"
#include "maicord_effect_plasma.hpp"
#include "maicord_effect_reference.hpp"
#include "maicord_effect_spark_ribbon_compose.hpp"
#include "maicord_effect_spark_ribbon_ribbon.hpp"
#include "maicord_effect_spark_ribbon_ribbon_vs.hpp"
#include "maicord_effect_spark_ribbon_simulate.hpp"
#include "maicord_effect_spark_ribbon_spark.hpp"
#include "maicord_effect_spark_ribbon_spark_vs.hpp"
#include "maicord_effect_starfield.hpp"
#include "maicord_effect_wave_lines.hpp"
#include "probe_capture.hpp"
#include "theme_views_probe.hpp"
#include "maicord/render/effects/background_effect.hpp"

#include "maicord/core/hash.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace background_theme_detail {
namespace {

// ---- Packages ----------------------------------------------------------------------------------------
struct Package {
    const char* name;
    const u8* code;
    u32 bytes;
    u32 uses;
    EffectBlend blend;
    u8 resolution, frame_rate; // step + 1, as the package requests them
    float knobs[12];
    const EffectGraph* graph;      // a pass graph (interface 3), with its shaders
    const ConstSlice<u8>* shaders;
};

#define MC_SHADER(array) ConstSlice<u8>{array, static_cast<u32>(sizeof(array))}
// spark-ribbon's graph, as its @target and @pass lines declare it: shaders simulate, ribbon_vs, ribbon, compose,
// spark_vs, spark.
const ConstSlice<u8> kSparkShaders[]{MC_SHADER(g_maicord_effect_spark_ribbon_simulate),
                                     MC_SHADER(g_maicord_effect_spark_ribbon_ribbon_vs),
                                     MC_SHADER(g_maicord_effect_spark_ribbon_ribbon),
                                     MC_SHADER(g_maicord_effect_spark_ribbon_compose),
                                     MC_SHADER(g_maicord_effect_spark_ribbon_spark_vs),
                                     MC_SHADER(g_maicord_effect_spark_ribbon_spark)};
const EffectGraph kSparkGraph{
    4, 2, 6, 1u << 1 | 1u << 4,
    {{32, 32, 0, EffectTargetFormat::rgba32f, true}, {0, 0, 50, EffectTargetFormat::rgba16f, false}},
    {{0, 0, 0, 1, EffectPassBlend::opaque, EffectPassDraw::screen, false, {}},
     {1, 2, 1, 0, EffectPassBlend::add, EffectPassDraw::grid, true, {96, 24}},
     {0, 3, kEffectGraphOutput, 2, EffectPassBlend::opaque, EffectPassDraw::screen, false, {}},
     {4, 5, kEffectGraphOutput, 1, EffectPassBlend::add, EffectPassDraw::quads, false, {1024, 1}}}};
// tests/shaders/background_effect_graph.hlsl: see its header for what it draws.
const ConstSlice<u8> kGraphShaders[]{MC_SHADER(g_maicord_effect_graph_count), MC_SHADER(g_maicord_effect_graph_half),
                                     MC_SHADER(g_maicord_effect_graph_fill),  MC_SHADER(g_maicord_effect_graph_grid),
                                     MC_SHADER(g_maicord_effect_graph_solid), MC_SHADER(g_maicord_effect_graph_quad)};
const EffectGraph kGraph{
    5, 2, 6, 1u << 3 | 1u << 5,
    {{4, 4, 0, EffectTargetFormat::rgba32f, true}, {0, 0, 50, EffectTargetFormat::rgba16f, false}},
    {{0, 0, 0, 1, EffectPassBlend::opaque, EffectPassDraw::screen, false, {}},
     {0, 1, 1, 0, EffectPassBlend::opaque, EffectPassDraw::screen, false, {}},
     {0, 2, kEffectGraphOutput, 3, EffectPassBlend::opaque, EffectPassDraw::screen, false, {}},
     {3, 4, kEffectGraphOutput, 0, EffectPassBlend::add, EffectPassDraw::grid, false, {3, 2}},
     {5, 4, kEffectGraphOutput, 0, EffectPassBlend::alpha, EffectPassDraw::quads, false, {2, 1}}}};
// The budget's graph: the reference effect's costly loop draws into a full-size target, and a cheap pass is all the
// output gets; the GPU time the controller sees has to be the whole graph's.
const ConstSlice<u8> kBurnShaders[]{MC_SHADER(g_maicord_effect_reference), MC_SHADER(g_maicord_effect_graph_half)};
const EffectGraph kBurnGraph{2, 1, 2, 0,
                             {{0, 0, 100, EffectTargetFormat::rgba8, false}},
                             {{0, 0, 0, 0, EffectPassBlend::opaque, EffectPassDraw::screen, false, {}},
                              {0, 1, kEffectGraphOutput, 0, EffectPassBlend::opaque, EffectPassDraw::screen, false, {}}}};
#undef MC_SHADER

#define MC_PACKAGE(array) array, static_cast<u32>(sizeof(array))
// The shipped samples, as their .effect sidecars describe them.
const Package kSamples[]{
    {"ascii-rain", MC_PACKAGE(g_maicord_effect_ascii_rain), effect_use_glyphs, EffectBlend::screen, 4, 4,
     {14, 1, 18, 1, .15f, 1, .4f, 0}},
    {"aurora", MC_PACKAGE(g_maicord_effect_aurora), effect_use_blue_noise, EffectBlend::normal, 3, 3,
     {.35f, .7f, .75f, 3}},
    {"starfield", MC_PACKAGE(g_maicord_effect_starfield), 0, EffectBlend::add, 3, 3, {.14f, .5f, 1.2f, .6f}},
    {"plasma", MC_PACKAGE(g_maicord_effect_plasma), effect_use_blue_noise, EffectBlend::normal, 3, 3,
     {.8f, 1.6f, .5f, .6f}},
    {"bokeh", MC_PACKAGE(g_maicord_effect_bokeh), 0, EffectBlend::screen, 2, 3, {.42f, .5f, 1, .8f}},
    {"wave-lines", MC_PACKAGE(g_maicord_effect_wave_lines), 0, EffectBlend::screen, 3, 3, {11, .45f, .55f, 1.1f}},
    {"film-grain", MC_PACKAGE(g_maicord_effect_film_grain), effect_use_noise, EffectBlend::normal, 5, 2,
     {.1f, 1, 12}},
    {"day-night", MC_PACKAGE(g_maicord_effect_day_night), effect_use_blue_noise | effect_use_clock | effect_use_appearance,
     EffectBlend::normal, 3, 2, {2, 6.5f, 19, .6f, .5f}},
    {"spark-ribbon", MC_PACKAGE(g_maicord_effect_spark_ribbon_simulate), effect_use_blue_noise, EffectBlend::normal, 3, 3,
     {.6f, .6f, 0, 0, 140 / 255.f, 184 / 255.f, 1}, &kSparkGraph, kSparkShaders},
};
constexpr u32 kDayNight = 7;
#undef MC_PACKAGE

u64 identity_of(const u8* code, u32 bytes) noexcept {
    return hash64_bytes(kHash64Seed, code, bytes) | 1;
}

BackgroundEffectSpec spec_of(const Package& package) noexcept {
    BackgroundEffectSpec spec{};
    spec.bytecode = {package.code, package.bytes};
    spec.identity = identity_of(package.code, package.bytes);
    spec.uses = package.uses;
    spec.blend = package.blend;
    spec.requested_resolution = package.resolution;
    spec.requested_frame_rate = package.frame_rate;
    for (u32 i = 0; i < 12; ++i)
        spec.knobs[i / 4][i % 4] = package.knobs[i];
    spec.graph = package.graph;
    spec.shaders = package.shaders;
    return spec;
}

BackgroundEffectSpec reference_spec(float coverage, float burn = 0) noexcept {
    BackgroundEffectSpec spec{};
    spec.bytecode = {g_maicord_effect_reference, sizeof(g_maicord_effect_reference)};
    spec.identity = identity_of(g_maicord_effect_reference, sizeof(g_maicord_effect_reference));
    spec.blend = EffectBlend::normal;
    spec.knobs[0][0] = coverage;
    spec.knobs[1][0] = burn;
    return spec;
}

// A graph spec: shader 0 is its first; the identity differs from a single pass of the same shader.
BackgroundEffectSpec graph_spec(const EffectGraph& graph, const ConstSlice<u8>* shaders, float burn = 0) noexcept {
    BackgroundEffectSpec spec{};
    spec.bytecode = shaders[0];
    spec.identity = identity_of(shaders[0].data, shaders[0].count) ^ 2;
    spec.blend = EffectBlend::normal;
    spec.knobs[0][0] = 1;
    spec.knobs[1][0] = burn;
    spec.graph = &graph;
    spec.shaders = shaders;
    return spec;
}

EffectSettings settings_of(u32 resolution_step, EffectBlend blend = EffectBlend::normal, float opacity = 1,
                           EffectBudget budget = EffectBudget::high) noexcept {
    return {1.f, opacity, blend, static_cast<u8>(resolution_step), 2, budget, false};
}

// The moment a sample with ambient inputs is shown at: 19:15 on 21 June, at dusk, in the default theme's dark
// appearance. The pass resets the inputs when it selects another package, so this follows each selection.
void sample_moment(D3D11Renderer& r, u32 uses) noexcept {
    if (!(uses & kEffectAmbientUses))
        return;
    const float moment[4]{19.25f / 24, (171 + 19.25f / 24) / 365, 6, 1};
    renderer_set_effect_ambient(&r, moment);
}

bool frame(D3D11Renderer& r, const Theme& theme, float phase) noexcept {
    return renderer_set_background_phase(&r, phase) && d3d11_renderer_draw_stream(&r, theme, true, true, nullptr);
}

// One window-sized stream: the flat base, then canvases the effect draws over, in DIPs.
struct Scene {
    PaintStream stream{};
    RendererTextStorage empty{};
    float width, height;
    bool built;
    Scene(D3D11Renderer& r, const Theme& theme, const UiRect* canvases, u32 count, const Color* widget) noexcept
        : width(r.width * 96.f / r.dpi), height(r.height * 96.f / r.dpi), built(false) {
        built = paint_stream_initialize(&stream) && paint_stream_reset(&stream, width, height) &&
                paint_stream_push(&stream, 0, 0, width, height, theme.base_lowest);
        for (u32 i = 0; built && i < count; ++i) {
            built = ui_background_paint_canvas(&stream, canvases[i], theme, theme.base_low);
            // A widget sits over the first canvas's corner.
            if (built && i == 0 && widget)
                built = paint_stream_push(&stream, canvases[0].left, canvases[0].top,
                                          canvases[0].left + (canvases[0].right - canvases[0].left) * .2f,
                                          canvases[0].top + (canvases[0].bottom - canvases[0].top) * .2f, *widget);
        }
    }
};

void install(D3D11Renderer& r, Scene& scene) noexcept {
    r.paint_stream = scene.stream;
    r.content_text = &scene.empty;
    r.media_quad_count = r.media_batch_count = 0;
}

// Straight colour over base: accent * a + base * (1 - a).
void mix(const Color& base, const Color& ink, float a, float* rgb) noexcept {
    rgb[0] = ink.red * a + base.red * (1 - a);
    rgb[1] = ink.green * a + base.green * (1 - a);
    rgb[2] = ink.blue * a + base.blue * (1 - a);
}

// The pixels of a frame that moved off the base colour by more than `tolerance` levels.
u32 pixels_off_base(const PixelReadback& pixels, const Color& base, i32 tolerance) noexcept {
    if (!pixels.readable)
        return 0;
    const i32 want[3]{static_cast<i32>(base.red * 255 + .5f), static_cast<i32>(base.green * 255 + .5f),
                      static_cast<i32>(base.blue * 255 + .5f)};
    u32 off = 0;
    for (u32 y = 0; y < pixels.height; ++y) {
        const auto* row = static_cast<const u8*>(pixels.mapped.pData) + y * pixels.mapped.RowPitch;
        for (u32 x = 0; x < pixels.width; ++x) {
            const u8* p = row + x * 4;
            const i32 channel[3]{p[pixels.bgra ? 2 : 0], p[1], p[pixels.bgra ? 0 : 2]};
            bool moved = false;
            for (u32 c = 0; c < 3; ++c) {
                const i32 delta = channel[c] - want[c];
                moved = moved || delta > tolerance || delta < -tolerance;
            }
            off += moved;
        }
    }
    return off;
}

// ---- The renderer's pass --------------------------------------------------------------------------
bool pass_pixels(D3D11Renderer& r, const Theme& theme) noexcept {
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    // Two canvases with the half-window boundary between them, and a third astride it.
    const UiRect canvases[]{{width * .05f, height * .2f, width * .4f, height * .6f},
                            {width * .6f, height * .2f, width * .95f, height * .6f},
                            {width * .4f, height * .75f, width * .6f, height * .9f}};
    const Color widget{.8f, .05f, .05f, 1};
    Scene scene(r, theme, canvases, 3, &widget);
    // base, canvas, widget, canvas, canvas
    if (!scene.built || scene.stream.count != 5 || !ui_background_canvas_quad(scene.stream.quads[1]) ||
        ui_background_canvas_quad(scene.stream.quads[2]) || !ui_background_canvas_quad(scene.stream.quads[3]) ||
        !ui_background_canvas_quad(scene.stream.quads[4]))
        return failure(140);
    StreamSwap restore{r, scene.stream, kDefaultTheme, true};
    install(r, scene);
    float expected[3];
    constexpr float kCoverage = .6f;
    mix(theme.base_low, theme.accent, kCoverage, expected);
    const float base[]{theme.base_low.red, theme.base_low.green, theme.base_low.blue};
    const float outside[]{theme.base_lowest.red, theme.base_lowest.green, theme.base_lowest.blue};
    const float ink[]{widget.red, widget.green, widget.blue};

    // 1. Select; one redraw of the target; the canvases composite it.
    const BackgroundEffectSpec spec = reference_spec(kCoverage);
    const auto selected = renderer_select_effect(&r, spec, settings_of(4));
    if (selected.state != EffectSelect::changed || FAILED(selected.native_error) || !r.effect.selected)
        return failure(141);
    const u32 renders = r.effect.renders;
    if (!frame(r, theme, 0) || r.effect.renders != renders + 1 || !r.effect.rendered || r.effect.draws_last_frame != 3 ||
        r.background_fx.source != BackgroundFxSource::none)
        return failure(142);
    {
        PixelReadback pixels{r};
        // Window-space continuity: the left half is the accent mix in both the first canvas and the one astride
        // the boundary, the right half is bare in the second and in the astride one's right half.
        if (!pixels.color(.22f, .4f, expected) || !pixels.color(.78f, .4f, base) ||
            !pixels.color(.45f, .825f, expected) || !pixels.color(.55f, .825f, base) ||
            !pixels.color(.05f + .35f * .1f, .2f + .4f * .1f, ink) || !pixels.color(.02f, .02f, outside))
            return failure(143);
    }

    // 2. The frame cap: a frame at the same clock reuses the target; a new clock draws it once more.
    if (!frame(r, theme, 0) || r.effect.renders != renders + 1 || r.effect.rendered || r.effect.draws_last_frame != 3)
        return failure(144);
    if (!frame(r, theme, .05f) || r.effect.renders != renders + 2)
        return failure(145);

    // 3. Pause: while the window is dragged the last picture stays, whatever the clock does.
    r.live_resize = true;
    const bool paused = frame(r, theme, .1f) && r.effect.renders == renders + 2 && r.effect.draws_last_frame == 3;
    r.live_resize = false;
    if (!paused || !frame(r, theme, .1f) || r.effect.renders != renders + 3)
        return failure(146);

    // 4. Resolution: the lowest step renders a quarter-size target that still fills the canvases.
    if (renderer_select_effect(&r, spec, settings_of(0)).state != EffectSelect::changed || r.effect.step != 0 ||
        !frame(r, theme, .15f) || r.effect.used_width != effect_scaled_extent(r.width, 0) ||
        r.effect.used_height != effect_scaled_extent(r.height + r.caption_height, 0) || r.effect.used_width >= r.width)
        return failure(147);
    {
        PixelReadback pixels{r};
        if (!pixels.color(.22f, .4f, expected) || !pixels.color(.78f, .4f, base))
            return failure(148);
    }
    if (renderer_select_effect(&r, spec, settings_of(4)).state != EffectSelect::changed)
        return failure(149);
    if (r.effect.step != r.effect.budget.step || !frame(r, theme, .2f))
        return failure(150);

    // 5. Blends: normal, screen and add over the base, and the user's opacity.
    float screen[3], add[3], half[3];
    const float src[]{theme.accent.red * kCoverage, theme.accent.green * kCoverage, theme.accent.blue * kCoverage};
    for (u32 c = 0; c < 3; ++c) {
        screen[c] = src[c] + base[c] * (1 - src[c]);
        add[c] = minimum(src[c] + base[c], 1.f);
    }
    mix(theme.base_low, theme.accent, kCoverage * .5f, half);
    const struct {
        EffectBlend blend;
        float opacity;
        const float* want;
    } cases[]{{EffectBlend::screen, 1, screen}, {EffectBlend::add, 1, add}, {EffectBlend::normal, .5f, half}};
    for (const auto& blend_case : cases) {
        if (renderer_select_effect(&r, spec, settings_of(4, blend_case.blend, blend_case.opacity)).state ==
                EffectSelect::rejected ||
            !frame(r, theme, .25f + static_cast<float>(blend_case.blend) * .05f))
            return failure(151);
        PixelReadback pixels{r};
        if (!pixels.color(.22f, .4f, blend_case.want) || !pixels.color(.78f, .4f, base))
            return failure(152 + static_cast<u32>(blend_case.blend));
    }
    if (renderer_select_effect(&r, spec, settings_of(4)).state == EffectSelect::rejected)
        return failure(155);

    // 6. None: no pass at all, no draw, the base alone.
    renderer_clear_effect(&r);
    const u32 after = r.effect.renders;
    if (r.effect.selected || !frame(r, theme, .5f) || r.effect.renders != after || r.effect.draws_last_frame ||
        r.effect.active)
        return failure(156);
    {
        PixelReadback pixels{r};
        if (!pixels.color(.22f, .4f, base))
            return failure(157);
    }

    // 7. Bytecode the device rejects: the failure reaches the host, nothing is selected.
    u8 corrupt[sizeof(g_maicord_effect_reference)];
    copy_bytes(corrupt, g_maicord_effect_reference, sizeof(corrupt));
    for (u32 i = 32; i < sizeof(corrupt); ++i)
        corrupt[i] ^= 0x5a;
    BackgroundEffectSpec bad = spec;
    bad.bytecode = {corrupt, sizeof(corrupt)};
    bad.identity = identity_of(corrupt, sizeof(corrupt));
    const auto rejected = renderer_select_effect(&r, bad, settings_of(4));
    if (rejected.state != EffectSelect::rejected || !FAILED(rejected.native_error) || r.effect.selected)
        return failure(158);
    log_write(LogLevel::info, "probe",
              "effect_pass_window_space_continuity_frame_cap_pause_resolution_blends_opacity_none_and_rejection_passed");
    return true;
}

// ---- Budget ------------------------------------------------------------------------------------------
// A package that costs real GPU time under the Low budget: the probe starts it low, or refuses it, or the
// controller steps it down, or turns it off. Any of those is the budget at work; none of them is a stuck effect.
// `probe_ms` is the selection probe's frame time at its larger size (0 when the device could not time it).
bool budget_pixels(D3D11Renderer& r, const Theme& theme, const BackgroundEffectSpec& spec, float* probe_ms) noexcept {
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    const UiRect canvas{0, 0, width, height};
    Scene scene(r, theme, &canvas, 1, nullptr);
    if (!scene.built)
        return failure(160);
    StreamSwap restore{r, scene.stream, kDefaultTheme, true};
    install(r, scene);
    r.effect.probe = {};
    const auto selected = renderer_select_effect(&r, spec, settings_of(4, EffectBlend::normal, 1, EffectBudget::low));
    if (selected.state == EffectSelect::rejected)
        return failure(161);
    *probe_ms = r.effect.probe.large_ms;
    bool reacted = selected.state == EffectSelect::refused;
    if (reacted) {
        if (r.effect.selected || r.effect.verdict != EffectVerdict::refused)
            return failure(162);
        r.effect.verdict = EffectVerdict::none;
    } else {
        const u32 start = r.effect.step;
        reacted = start < 4;
        // Real frames for a second and a half: the window of the controller is one second.
        for (u32 i = 0; i < 90 && !reacted; ++i) {
            if (!frame(r, theme, i * .02f))
                return failure(163);
            r.graphics.context->Flush();
            Sleep(17);
            reacted = !r.effect.selected || r.effect.step < start;
        }
        if (!r.effect.selected)
            r.effect.verdict = EffectVerdict::none;
    }
    log_u64(LogLevel::info, "probe", "effect_budget_heavy_package_reacted", reacted);
    log_u64(LogLevel::info, "probe", "effect_budget_step", r.effect.step);
    log_u64(LogLevel::info, "probe", "effect_budget_renders", r.effect.renders);
    return reacted ? true : failure(164);
}

// ---- The samples: renders and captures ----------------------------------------------------------
bool capture_wanted() noexcept {
    return GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_DIR", nullptr, 0) != 0;
}

// "effect-<name>-<kind>[-NN].bmp"
u32 capture_name(wchar_t (&buffer)[96], const char* name, const wchar_t* kind, i32 index) noexcept {
    u32 at = 0;
    const auto put = [&](const wchar_t* text) noexcept {
        for (u32 i = 0; text[i] && at < 90; ++i)
            buffer[at++] = text[i];
    };
    put(L"effect-");
    for (u32 i = 0; name[i] && at < 90; ++i)
        buffer[at++] = static_cast<wchar_t>(name[i]);
    buffer[at++] = L'-';
    put(kind);
    if (index >= 0) {
        buffer[at++] = L'-';
        buffer[at++] = static_cast<wchar_t>(L'0' + index / 10);
        buffer[at++] = static_cast<wchar_t>(L'0' + index % 10);
    }
    put(L".bmp");
    buffer[at] = 0;
    return at;
}

bool write_frame(D3D11Renderer& r, const char* name, const wchar_t* kind, i32 index) noexcept {
    ProbeTargetReadback readback;
    if (probe_target_readback(r, &readback) != 0)
        return false;
    wchar_t file[96];
    capture_name(file, name, kind, index);
    return probe_write_capture(readback, file);
}

// Draws the package for `frames` UI frames at one resolution step with the controller held. False when the
// package is not selected, a frame fails, or the pass redraws nothing.
bool render_step(D3D11Renderer& r, const Theme& theme, const BackgroundEffectSpec& spec, u32 step,
                 u32 frames) noexcept {
    const auto selected = renderer_select_effect(&r, spec, settings_of(step, spec.blend, 1, EffectBudget::high));
    if (selected.state == EffectSelect::rejected || !r.effect.selected)
        return false;
    sample_moment(r, spec.uses);
    // The controller is held: this renders a step, not the controller.
    r.effect.step = step;
    r.effect.budget.step = static_cast<u8>(step);
    r.effect.budget.ceiling = static_cast<u8>(step);
    r.effect.budget.warmup = 0x7fffffff;
    r.effect.dirty = true;
    for (u32 i = 0; i < 6; ++i)
        if (!frame(r, theme, i * .05f))
            return false;
    const u32 renders = r.effect.renders;
    for (u32 i = 0; i < frames; ++i)
        if (!frame(r, theme, .3f + i * .05f))
            return false;
    return r.effect.selected && r.effect.renders > renders;
}

bool samples_pixels(D3D11Renderer& r, const Theme& theme) noexcept {
    const u32 width = r.width, height = r.height;
    struct Restore {
        D3D11Renderer& r;
        u32 width, height;
        ~Restore() noexcept { renderer_resize(&r, width, height); }
    } restore_size{r, width, height};
    const bool capture = capture_wanted();
    for (const auto& package : kSamples) {
        const BackgroundEffectSpec spec = spec_of(package);
        // 1440p: the full target and the package's own resolution each redraw.
        if (!renderer_resize(&r, 2560, 1440) || r.width != 2560)
            return failure(170);
        {
            const float w = r.width * 96.f / r.dpi, h = r.height * 96.f / r.dpi;
            const UiRect canvas{0, 0, w, h};
            Scene scene(r, theme, &canvas, 1, nullptr);
            if (!scene.built)
                return failure(171);
            StreamSwap restore{r, scene.stream, kDefaultTheme, true};
            install(r, scene);
            const u32 own_step = package.resolution ? package.resolution - 1u : kEffectDefaultResolutionStep;
            if (!render_step(r, theme, spec, 4, 24) || !render_step(r, theme, spec, own_step, 24))
                return failure(172);
            log_write(LogLevel::info, "probe", package.name);
            renderer_clear_effect(&r);
        }
        // The picture: a still and a short loop at the catalog's preview size, and proof it is drawn and moves.
        if (!renderer_resize(&r, 640, 360) || r.width != 640)
            return failure(173);
        const float w = r.width * 96.f / r.dpi, h = r.height * 96.f / r.dpi;
        const UiRect canvas{0, 0, w, h};
        Scene scene(r, theme, &canvas, 1, nullptr);
        if (!scene.built)
            return failure(174);
        StreamSwap restore{r, scene.stream, kDefaultTheme, true};
        install(r, scene);
        // The probe renders at the preview's own resolution; the package's knobs stay as shipped.
        if (renderer_select_effect(&r, spec, settings_of(4, spec.blend)).state == EffectSelect::rejected)
            return failure(175);
        sample_moment(r, spec.uses);
        r.effect.step = 4;
        r.effect.budget.step = r.effect.budget.ceiling = 4;
        r.effect.budget.warmup = 0x7fffffff;
        r.effect.dirty = true;
        u64 first = 0, second = 0;
        u32 moved = 0;
        if (!frame(r, theme, 2.0f))
            return failure(176);
        {
            PixelReadback pixels{r};
            first = pixels.hash();
            moved = pixels_off_base(pixels, theme.base_low, 3);
        }
        if (capture && !write_frame(r, package.name, L"still", -1))
            return failure(177);
        if (!frame(r, theme, 2.6f))
            return failure(178);
        {
            PixelReadback pixels{r};
            second = pixels.hash();
        }
        // Every sample draws something, and draws something different later.
        log_u64(LogLevel::info, "probe", "effect_sample_pixels_off_base", moved);
        if (moved < 64 || first == second)
            return failure(179);
        if (capture)
            for (i32 i = 0; i < 48; ++i)
                if (!frame(r, theme, 2.0f + static_cast<float>(i) * .05f) || !write_frame(r, package.name, L"loop", i))
                    return failure(180);
        renderer_clear_effect(&r);
    }
    log_write(LogLevel::info, "probe", "effect_samples_render_and_move_passed");
    return true;
}

// ---- Pass graph ---------------------------------------------------------------------------------------
// The test graph (tests/shaders/background_effect_graph.hlsl) `frames` frames after its state was cleared, which takes
// every pass: the persistent count, the half-size float target read back, the grid added, the quads alpha-blended.
bool graph_shows(D3D11Renderer& r, float frames) noexcept {
    PixelReadback pixels{r};
    const float red = frames / 16;
    const float base[]{red, 0, .5f}, right[]{red, .5f, .5f}, bar[]{.5f + red * .5f, .5f, .75f};
    return pixels.color(.25f, .7f, base) && pixels.color(.75f, .7f, right) && pixels.color(.15f, .25f, bar) &&
           pixels.color(.35f, .25f, bar) && pixels.color(.25f, .25f, base);
}

// Selects a graph at the full resolution with the controller held.
bool select_graph(D3D11Renderer& r, const BackgroundEffectSpec& spec) noexcept {
    if (renderer_select_effect(&r, spec, settings_of(4)).state == EffectSelect::rejected || !r.effect.selected)
        return false;
    r.effect.step = 4;
    r.effect.budget.step = r.effect.budget.ceiling = 4;
    r.effect.budget.warmup = 0x7fffffff;
    r.effect.dirty = true;
    return true;
}

bool graph_pixels(D3D11Renderer& r, const Theme& theme) noexcept {
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    const UiRect canvas{0, 0, width, height};
    Scene scene(r, theme, &canvas, 1, nullptr);
    if (!scene.built)
        return failure(220);
    StreamSwap restore{r, scene.stream, kDefaultTheme, true};
    install(r, scene);
    const BackgroundEffectSpec spec = graph_spec(kGraph, kGraphShaders);
    // 1. The first frame after the selection draws one of each pass; the probe's frames left no state behind.
    if (!select_graph(r, spec) || r.effect.graph.pass_count != 5 || !frame(r, theme, 0) || !graph_shows(r, 1))
        return failure(221);
    // The fixed persistent target has two planes; the scaled one is half the output, one plane.
    const D3D11EffectGraphTarget& counter = r.effect.graph_targets[0];
    const D3D11EffectGraphTarget& scaled = r.effect.graph_targets[1];
    if (counter.width != 4 || counter.height != 4 || !counter.views[1] || scaled.views[1] ||
        scaled.width != effect_target_extent(kGraph.targets[1], r.effect.used_width, false) ||
        scaled.height != effect_target_extent(kGraph.targets[1], r.effect.used_height, true) ||
        scaled.width * 2 < r.effect.used_width)
        return failure(222);
    // 2. The state survives frames: each redraw counts one more, and a frame at the same clock draws nothing.
    const u32 renders = r.effect.renders;
    if (!frame(r, theme, .05f) || !frame(r, theme, .1f) || !frame(r, theme, .1f) || r.effect.renders != renders + 2 ||
        !graph_shows(r, 3))
        return failure(223);
    if (capture_wanted() && !write_frame(r, "graph-test", L"frame", 3))
        return failure(224);
    // 3. Selecting it again after another effect starts the state over.
    if (renderer_select_effect(&r, reference_spec(.6f), settings_of(4)).state == EffectSelect::rejected ||
        !frame(r, theme, .15f) || !select_graph(r, spec) || !frame(r, theme, .2f) || !graph_shows(r, 1))
        return failure(225);
    // 4. So does a device reset: the objects go with the device, and the host's next selection rebuilds them.
    if (!renderer_recover_device(&r) || r.effect.shader || r.effect.graph_targets[0].views[0] ||
        r.effect.graph_shaders[3].vertex || !select_graph(r, spec))
        return failure(226);
    install(r, scene);
    r.retained_presentation.valid = false;
    if (!frame(r, theme, .25f) || !frame(r, theme, .3f) || !graph_shows(r, 2))
        return failure(227);
    // 5. Clearing it releases the graph's shaders and targets.
    renderer_clear_effect(&r);
    if (r.effect.graph.pass_count || r.effect.graph_targets[0].views[0] || r.effect.graph_targets[1].views[0] ||
        r.effect.graph_shaders[5].vertex)
        return failure(228);
    log_write(LogLevel::info, "probe", "effect_graph_passes_targets_geometry_blends_and_persistent_state_passed");
    return true;
}

// The probe times the whole graph: a graph whose costly pass draws only into an intermediate target measures like
// the same shader drawn straight to the output.
bool graph_budget(float single_ms, float graph_ms) noexcept {
    log_u64(LogLevel::info, "probe", "effect_budget_single_probe_us", static_cast<u64>(single_ms * 1000));
    log_u64(LogLevel::info, "probe", "effect_budget_graph_probe_us", static_cast<u64>(graph_ms * 1000));
    return single_ms <= 0 || graph_ms <= 0 || graph_ms >= single_ms * .5f ? true : failure(229);
}

// ---- Ambient inputs ---------------------------------------------------------------------------------
// Mean of r + g + b over the frame, in levels (0..765).
u32 mean_level(const PixelReadback& pixels) noexcept {
    if (!pixels.readable || !pixels.width || !pixels.height)
        return 0;
    u64 sum = 0;
    for (u32 y = 0; y < pixels.height; ++y) {
        const auto* row = static_cast<const u8*>(pixels.mapped.pData) + y * pixels.mapped.RowPitch;
        for (u32 x = 0; x < pixels.width; ++x)
            sum += u32{row[x * 4]} + row[x * 4 + 1] + row[x * 4 + 2];
    }
    return static_cast<u32>(sum / (u64{pixels.width} * pixels.height));
}

// The day-night sample under eased inputs, as the host feeds them, with the effect's clock standing still: a sweep
// of the hours (light appearance) brightens at noon and darkens at night, and Windows turning dark glides the sky from
// day to night, every eased step redrawing the target once and nothing redrawing once it settles. Captured as
// effect-day-night-hour-NN.bmp and effect-day-night-transition-NN.bmp.
bool ambient_pixels(D3D11Renderer& r, const Theme& theme) noexcept {
    const u32 width = r.width, height = r.height;
    struct Restore {
        D3D11Renderer& r;
        u32 width, height;
        ~Restore() noexcept { renderer_resize(&r, width, height); }
    } restore_size{r, width, height};
    if (!renderer_resize(&r, 640, 360) || r.width != 640)
        return failure(200);
    const float w = r.width * 96.f / r.dpi, h = r.height * 96.f / r.dpi;
    const UiRect canvas{0, 0, w, h};
    Scene scene(r, theme, &canvas, 1, nullptr);
    if (!scene.built)
        return failure(201);
    StreamSwap restore{r, scene.stream, kDefaultTheme, true};
    install(r, scene);
    const Package& package = kSamples[kDayNight];
    const BackgroundEffectSpec spec = spec_of(package);
    if (renderer_select_effect(&r, spec, settings_of(4, spec.blend)).state == EffectSelect::rejected)
        return failure(202);
    r.effect.step = 4;
    r.effect.budget.step = r.effect.budget.ceiling = 4;
    r.effect.budget.warmup = 0x7fffffff;
    r.effect.dirty = true;
    const bool capture = capture_wanted();
    constexpr float kPhase = 2.f;
    EffectAmbientSources sources{{2026, 6, 21, 0, 0}, false, false};
    float values[4]{};
    // The hours of 21 June, light: each settles at once (a new selection), so the sky follows the clock.
    u32 levels[24]{};
    for (u16 hour = 0; hour < 24; ++hour) {
        EffectAmbient fresh{};
        sources.local.hour = hour;
        effect_ambient_sample(&fresh, spec.uses, sources, 1000, true, values);
        renderer_set_effect_ambient(&r, values);
        if (!frame(r, theme, kPhase))
            return failure(203);
        {
            PixelReadback pixels{r};
            levels[hour] = mean_level(pixels);
        }
        if (capture && !write_frame(r, package.name, L"hour", hour))
            return failure(204);
    }
    log_u64(LogLevel::info, "probe", "effect_ambient_midnight_level", levels[0]);
    log_u64(LogLevel::info, "probe", "effect_ambient_noon_level", levels[12]);
    if (!(levels[12] > levels[0] * 3 && levels[12] > levels[22] * 3 && levels[9] > levels[3]))
        return failure(205);
    // Noon, light; then Windows turns dark. Frames every 50 ms with the effect clock held.
    EffectAmbient ambient{};
    sources.local.hour = 12;
    u64 now = 100000;
    effect_ambient_sample(&ambient, spec.uses, sources, now, true, values);
    renderer_set_effect_ambient(&r, values);
    if (!frame(r, theme, kPhase))
        return failure(206);
    sources.system_dark = true;
    constexpr u32 kStep = 50, kSteps = kEffectAmbientEaseMs / kStep;
    u32 redraws = 0, easing = 0, previous = 765, first = 0, last = 0; // 765: the brightest mean level
    bool darkening = true;
    for (u32 i = 0; i <= kSteps + 10; ++i, now += kStep) {
        effect_ambient_sample(&ambient, spec.uses, sources, now, true, values);
        easing += effect_ambient_easing(ambient, now) ? 1u : 0u;
        renderer_set_effect_ambient(&r, values);
        const u32 renders = r.effect.renders;
        if (!frame(r, theme, kPhase))
            return failure(207);
        redraws += r.effect.renders - renders;
        {
            PixelReadback pixels{r};
            const u32 level = mean_level(pixels);
            darkening = darkening && level <= previous + 1;
            previous = level;
            first = i == 0 ? level : first;
            last = level;
        }
        if (capture && !write_frame(r, package.name, L"transition", static_cast<i32>(i)))
            return failure(208);
    }
    log_u64(LogLevel::info, "probe", "effect_ambient_transition_redraws", redraws);
    log_u64(LogLevel::info, "probe", "effect_ambient_transition_easing_frames", easing);
    log_u64(LogLevel::info, "probe", "effect_ambient_transition_monotonic", darkening);
    log_u64(LogLevel::info, "probe", "effect_ambient_transition_day_level", first);
    log_u64(LogLevel::info, "probe", "effect_ambient_transition_night_level", last);
    // The first sample is still the old value: kSteps steps move it, and each redraws once; the rest redraw nothing.
    if (redraws != kSteps || easing != kSteps || !darkening || !(first > last * 3) || values[effect_ambient_dark] != 1)
        return failure(209);
    renderer_clear_effect(&r);
    log_write(LogLevel::info, "probe", "effect_ambient_inputs_follow_the_clock_and_glide_with_the_appearance_passed");
    return true;
}

// ---- Device loss ---------------------------------------------------------------------------------
bool device_loss_pixels(D3D11Renderer& r, const Theme& theme) noexcept {
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    const UiRect canvas{width * .1f, height * .2f, width * .9f, height * .8f};
    Scene scene(r, theme, &canvas, 1, nullptr);
    if (!scene.built)
        return failure(190);
    StreamSwap restore{r, scene.stream, kDefaultTheme, true};
    install(r, scene);
    float expected[3];
    mix(theme.base_low, theme.accent, .6f, expected);
    const BackgroundEffectSpec spec = reference_spec(.6f);
    if (renderer_select_effect(&r, spec, settings_of(4)).state != EffectSelect::changed || !frame(r, theme, 0))
        return failure(191);
    {
        PixelReadback pixels{r};
        if (!pixels.color(.25f, .5f, expected))
            return failure(192);
    }
    // A healthy device reports no loss.
    if (renderer_device_lost(&r) || r.device_lost)
        return failure(193);

    // A frame in flight when the device goes is the one blamed. (The renderer sees a removal through a failed
    // present or draw; here the recovery runs as that path runs it.)
    volatile u64 marker = r.effect.identity;
    d3d11_effect_set_marker(&r.effect, &marker);
    ID3D11Device* const before = r.graphics.device;
    const u32 generation = r.device_generation;
    if (!renderer_recover_device(&r))
        return failure(194);
    if (r.device_generation != generation + 1 || r.device_lost || !r.graphics.device || !r.graphics.target ||
        !r.graphics.swap_chain || !r.effect.selected || r.effect.shader || r.effect.verdict != EffectVerdict::device_removed ||
        r.effect.verdict_identity != spec.identity)
        return failure(195);
    log_u64(LogLevel::info, "probe", "effect_device_recovery_new_device", r.graphics.device != before);
    r.effect.verdict = EffectVerdict::none;
    d3d11_effect_set_marker(&r.effect, nullptr);

    // The host selects the effect again, as it does every frame; the same picture comes back on the new device.
    if (renderer_select_effect(&r, spec, settings_of(4)).state == EffectSelect::rejected)
        return failure(196);
    install(r, scene);
    r.retained_presentation.valid = false;
    if (!frame(r, theme, 0))
        return failure(197);
    {
        PixelReadback pixels{r};
        if (!pixels.color(.25f, .5f, expected))
            return failure(198);
    }
    // Presenting works on the new swap chain, and the loss flag stays clear.
    if (!probe_present(&r, theme) || r.device_lost)
        return failure(199);
    log_write(LogLevel::info, "probe", "effect_device_recovery_rebuilt_renderer_and_effect_passed");
    return true;
}

} // namespace

bool effect_package_pixels(D3D11Renderer& r) noexcept {
    // Idle: with no effect selected the pass owns no device objects and draws nothing.
    if (r.effect.selected || r.effect.target || r.effect.shader || r.effect.draws_last_frame)
        return failure(139);
    const Theme theme = kDefaultTheme;
    float single_ms = 0, graph_ms = 0;
    return pass_pixels(r, theme) && budget_pixels(r, theme, reference_spec(.6f, 6000), &single_ms) &&
           budget_pixels(r, theme, graph_spec(kBurnGraph, kBurnShaders, 6000), &graph_ms) &&
           graph_budget(single_ms, graph_ms) && graph_pixels(r, theme) && samples_pixels(r, theme) &&
           ambient_pixels(r, theme) && device_loss_pixels(r, theme);
}

} // namespace background_theme_detail
} // namespace maicord
