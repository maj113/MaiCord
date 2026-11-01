#include "background_theme_internal.hpp"
#include "maicord_effect_ascii_rain.hpp"
#include "maicord_effect_aurora.hpp"
#include "maicord_effect_bokeh.hpp"
#include "maicord_effect_film_grain.hpp"
#include "maicord_effect_plasma.hpp"
#include "maicord_effect_reference.hpp"
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
};

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
};
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

EffectSettings settings_of(u32 resolution_step, EffectBlend blend = EffectBlend::normal, float opacity = 1,
                           EffectBudget budget = EffectBudget::high) noexcept {
    return {1.f, opacity, blend, static_cast<u8>(resolution_step), 2, budget, false};
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
bool budget_pixels(D3D11Renderer& r, const Theme& theme) noexcept {
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    const UiRect canvas{0, 0, width, height};
    Scene scene(r, theme, &canvas, 1, nullptr);
    if (!scene.built)
        return failure(160);
    StreamSwap restore{r, scene.stream, kDefaultTheme, true};
    install(r, scene);
    const BackgroundEffectSpec spec = reference_spec(.6f, 6000);
    const auto selected = renderer_select_effect(&r, spec, settings_of(4, EffectBlend::normal, 1, EffectBudget::low));
    if (selected.state == EffectSelect::rejected)
        return failure(161);
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
    log_u64(LogLevel::info, "probe", "effect_budget_gpu_samples", r.effect.gpu_samples);
    log_u64(LogLevel::info, "probe", "effect_budget_last_gpu_us", r.effect.last_gpu_microseconds);
    log_u64(LogLevel::info, "probe", "effect_budget_step", r.effect.step);
    log_u64(LogLevel::info, "probe", "effect_budget_renders", r.effect.renders);
    return reacted ? true : failure(164);
}

// ---- The samples: cost and captures -------------------------------------------------------------
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

// Draws the package for `frames` UI frames at one resolution step with the controller held, and averages the
// pass's own GPU time. False when the pass never timed a frame.
bool measure(D3D11Renderer& r, const Theme& theme, const BackgroundEffectSpec& spec, u32 step, u32 frames,
             u32* microseconds) noexcept {
    const auto selected = renderer_select_effect(&r, spec, settings_of(step, spec.blend, 1, EffectBudget::high));
    if (selected.state == EffectSelect::rejected || !r.effect.selected)
        return false;
    // The controller is held: this measures a step, not the controller.
    r.effect.step = step;
    r.effect.budget.step = static_cast<u8>(step);
    r.effect.budget.ceiling = static_cast<u8>(step);
    r.effect.budget.warmup = 0x7fffffff;
    r.effect.dirty = true;
    for (u32 i = 0; i < 6; ++i)
        if (!frame(r, theme, i * .05f))
            return false;
    r.graphics.context->Flush();
    Sleep(20);
    r.effect.total_gpu_microseconds = 0;
    r.effect.gpu_samples = 0;
    for (u32 i = 0; i < frames; ++i) {
        if (!frame(r, theme, .3f + i * .05f))
            return false;
        r.graphics.context->Flush();
        // Timings resolve a frame or two later; give the GPU room to finish between frames.
        Sleep(3);
    }
    Sleep(20);
    frame(r, theme, .3f + frames * .05f);
    if (!r.effect.gpu_samples)
        return false;
    *microseconds = static_cast<u32>(r.effect.total_gpu_microseconds / r.effect.gpu_samples);
    return true;
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
        // Cost at 1440p: the full target and the package's own resolution.
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
            u32 full = 0, own = 0;
            const u32 own_step = package.resolution ? package.resolution - 1u : kEffectDefaultResolutionStep;
            if (!measure(r, theme, spec, 4, 24, &full) || !measure(r, theme, spec, own_step, 24, &own))
                return failure(172);
            log_write(LogLevel::info, "probe", package.name);
            log_u64(LogLevel::info, "probe", "effect_gpu_us_1440p_full_resolution", full);
            log_u64(LogLevel::info, "probe", "effect_gpu_us_1440p_package_resolution", own);
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
    log_write(LogLevel::info, "probe", "effect_samples_render_move_and_were_timed_passed");
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
    return pass_pixels(r, theme) && budget_pixels(r, theme) && samples_pixels(r, theme) &&
           device_loss_pixels(r, theme);
}

} // namespace background_theme_detail
} // namespace maicord
