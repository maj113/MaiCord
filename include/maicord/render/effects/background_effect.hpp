#pragma once
#include "maicord/core/base.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/render/effects/background_fx_data.hpp"
#include "maicord/ui/theme/theme.hpp"

// Package background effects (maicord/sdk/background.hlsli): the data the
// renderer's effect pass takes, and the CPU-side policy around it (the
// resolution and frame-rate steps, the GPU budget controller, the cost probe's
// extrapolation). Nothing here touches D3D11.
namespace maicord {

// ---- Steps ---------------------------------------------------------------------------------
// Resolution is a share of the window (both axes); frame rate caps how often the
// effect target is drawn. A step index addresses these tables; a stored "step
// value" is index + 1, and zero means "not set".
inline constexpr u32 kEffectResolutionSteps = 5, kEffectFrameRateSteps = 5;
inline constexpr u8 kEffectResolutionPercent[kEffectResolutionSteps]{25, 35, 50, 75, 100};
inline constexpr u8 kEffectFramesPerSecond[kEffectFrameRateSteps]{10, 15, 24, 30, 60};
// What applies when neither the spec nor the user says anything (50 %, 24 fps).
inline constexpr u32 kEffectDefaultResolutionStep = 2, kEffectDefaultFrameRateStep = 2;
// While the window is not focused (EffectUnfocused::slow).
inline constexpr u32 kEffectUnfocusedFramesPerSecond = 10;

enum class EffectBlend : u8 { normal, screen, add };
enum class EffectBudget : u8 { low, balanced, high };
enum class EffectUnfocused : u8 { keep, slow, pause };
inline constexpr float kEffectBudgetMs[3]{0.5f, 1.5f, 4.f};
// Built-in inputs a package declares (@uses); an undeclared input is not bound and costs no memory. Clock and
// appearance are interface 2: they fill mc_ambient, which interface-1 shaders never declare. TEXTURE2 and TEXTURE3
// are interface 3.
enum EffectUse : u32 {
    effect_use_noise = 1,
    effect_use_glyphs = 2,
    effect_use_blue_noise = 4,
    effect_use_pointer = 8,
    effect_use_texture0 = 16,
    effect_use_texture1 = 32,
    effect_use_clock = 64,
    effect_use_appearance = 128,
    effect_use_texture2 = 256,
    effect_use_texture3 = 512,
    effect_use_all = 1023,
};
// The inputs a package can only declare under manifest interface 2, and under interface 3.
inline constexpr u32 kEffectAmbientUses = effect_use_clock | effect_use_appearance;
inline constexpr u32 kEffectGraphUses = effect_use_texture2 | effect_use_texture3;

// A step value from the first of: the per-shader override, the global setting, the spec's request,
// then the default index. All are index + 1 (0 = unset); the result is an index.
constexpr u32 effect_resolve_step(u8 shader, u8 global, u8 requested, u32 fallback, u32 count) noexcept {
    const u32 value = shader ? shader : global ? global : requested ? requested : fallback + 1;
    return value > count ? count - 1 : value - 1;
}

// ---- Constants -----------------------------------------------------------------------------
inline constexpr u32 kEffectKnobRows = 8;

// McBackground (b0): McFrame (176 bytes), mc_knobs[8], then mc_ambient (interface 2) and mc_pass (interface 3).
// Older shaders end their cbuffer before the rows they don't know, so their layout is unchanged. The roles are the
// theme's, in the sRGB values the UI draws with.
enum EffectAmbientValue : u32 { effect_ambient_day, effect_ambient_year, effect_ambient_month, effect_ambient_dark };
struct EffectFrameConstants {
    float time, delta, frame, dip;
    float resolution[2], pointer[2];
    float palette[kBackgroundFxPaletteRoles][4];
    float dark, intensity, reserved[2];
    float knobs[kEffectKnobRows][4];
    float ambient[4]; // EffectAmbientValue order; zero for an input the package does not declare
    float pass[4];    // mc_pass: the pass's target in pixels, then its draw count (EffectGraphPass::count)
};
static_assert(sizeof(EffectFrameConstants) == 336);
static_assert(__builtin_offsetof(EffectFrameConstants, palette) == 32 &&
              __builtin_offsetof(EffectFrameConstants, dark) == 160 &&
              __builtin_offsetof(EffectFrameConstants, knobs) == 176 &&
              __builtin_offsetof(EffectFrameConstants, ambient) == 304 &&
              __builtin_offsetof(EffectFrameConstants, pass) == 320);

// ---- Pass graph (interface 3) ---------------------------------------------------------------------
// Up to kEffectGraphPasses passes, each a pixel shader drawn over its whole target (the host's strip) or a vertex
// shader over host geometry, into the output or one of up to kEffectGraphTargets intermediate targets. Geometry is
// vertex IDs only: grid(W, H) is H - 1 instanced strips of 2W vertices, quads(N) is N instanced 4-vertex strips. A
// persistent target has two planes that swap after each pass that writes it, so the pass reads the last state and
// writes the next; its planes are cleared when the effect is selected, after a device reset and on a size change,
// and never read back. Memory is bounded: fixed targets together (both planes) at most kEffectGraphFixedBytes, and
// targets sized by a share of the output at most kEffectGraphScaledBytes per output pixel.
inline constexpr u32 kEffectGraphPasses = 6, kEffectGraphTargets = 4, kEffectGraphShaders = 12;
inline constexpr u32 kEffectGraphExtent = 512, kEffectGraphFixedBytes = 8u << 20, kEffectGraphScaledBytes = 16;
inline constexpr u32 kEffectGridExtent = 256, kEffectQuadsMaximum = 16384;
inline constexpr u8 kEffectGraphOutput = 0xFF;
enum class EffectTargetFormat : u8 { rgba8, rgba16f, rgba32f };
inline constexpr u8 kEffectTargetTexelBytes[3]{4, 8, 16};
enum class EffectPassBlend : u8 { opaque, alpha, add, premultiplied };
enum class EffectPassDraw : u8 { screen, grid, quads };
struct EffectGraphTarget {
    u16 width, height; // a fixed size (0 when scaled)
    u8 scale;          // percent of the output on both axes, 1..100 (0 when fixed)
    EffectTargetFormat format;
    bool persistent;
};
struct EffectGraphPass {
    u8 vertex, pixel; // shader indices; vertex 0 is the host's full-screen strip (shader 0 is a pixel shader)
    u8 target;        // a target index, or kEffectGraphOutput
    u8 inputs;        // a bit per target, bound at t7 + index (a persistent target it writes: the last state)
    EffectPassBlend blend;
    EffectPassDraw draw;
    bool clear;       // to transparent black before the pass draws
    u16 count[2];     // grid: columns and rows of points; quads: the count and 1; screen: 0
};
// pass_count 0: the single-pass effect of interfaces 1 and 2.
struct EffectGraph {
    u8 pass_count, target_count, shader_count;
    u16 vertex_shaders; // a bit per shader index
    EffectGraphTarget targets[kEffectGraphTargets];
    EffectGraphPass passes[kEffectGraphPasses];
};
// A target's width (`vertical`: height) for an output extent of `output` pixels on that axis.
constexpr u32 effect_target_extent(const EffectGraphTarget& target, u32 output, bool vertical) noexcept {
    if (!target.scale)
        return vertical ? target.height : target.width;
    const u32 scaled = (output * target.scale + 50) / 100;
    return scaled ? scaled : 1;
}

// The composite pass's constants: a canvas rectangle in DIPs, the map from a DIP to a position in the effect
// target (uv = dip * scale + offset), the used part of the target (inset half a texel, for the bilinear
// fetch), the user's opacity and, for a tile, its corner radius in DIPs (0: the plain rectangle). VS b2 and
// PS b0 both see all of it.
struct EffectCompositeConstants {
    float canvas[4];
    float uv[4];
    float limits[4];
    float opacity, radius, reserved[2];
};
static_assert(sizeof(EffectCompositeConstants) == 64);

constexpr void effect_palette(const Theme& theme, float (&palette)[kBackgroundFxPaletteRoles][4]) noexcept {
    const Color roles[kBackgroundFxPaletteRoles]{theme.base_low,   theme.surface,
                                                 theme.text_primary, theme.tokens.text.muted,
                                                 theme.accent,     theme.tokens.accent.success,
                                                 theme.tokens.accent.warning, theme.tokens.accent.danger};
    for (u32 i = 0; i < kBackgroundFxPaletteRoles; ++i) {
        palette[i][0] = roles[i].red;
        palette[i][1] = roles[i].green;
        palette[i][2] = roles[i].blue;
        palette[i][3] = 1;
    }
}

// ---- Ambient inputs (mc_ambient, interface 2) ---------------------------------------------------------
// The local clock (@uses clock) and the appearance (@uses appearance). The host samples its sources every frame
// and eases each value toward them, so a shader reads plain floats and blends. An appearance change, or a jump of
// the clock (a resume, a changed time zone, a simulated time), runs kEffectAmbientEaseMs on a smoothstep; the clock
// moving on its own is followed as it is. Where the window cannot show a change moving (hidden, covered, reduced
// motion, or not focused unless the effect keeps moving then) every value settles at once.
inline constexpr u32 kEffectAmbientEaseMs = 1500;
// The time of day moves in whole minutes; more than two of them between samples is a jump.
inline constexpr float kEffectClockMinute = 1.f / 1440.f;
inline constexpr float kEffectClockJump = 2 * kEffectClockMinute;

// A local date and time (SYSTEMTIME's fields).
struct EffectLocalTime {
    u16 year, month, day, hour, minute;
};
constexpr bool effect_leap_year(u32 year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}
constexpr u32 effect_month_days(u32 year, u32 month) noexcept {
    constexpr u8 days[12]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month >= 1 && month <= 12 ? days[month - 1] + (month == 2 && effect_leap_year(year)) : 0;
}
constexpr bool effect_local_time_valid(const EffectLocalTime& time) noexcept {
    return time.day >= 1 && time.day <= effect_month_days(time.year, time.month) && time.hour < 24 && time.minute < 60;
}

// mc_ambient's clock values: the time of day and of the year in 0..1 (from midnight, from 1 January 00:00) and the
// month 1..12. A time that is not valid reads as midnight on 1 January.
struct EffectClock {
    float day, year, month;
};
constexpr EffectClock effect_clock(const EffectLocalTime& time) noexcept {
    if (!effect_local_time_valid(time))
        return {0, 0, 1};
    u32 before = 0;
    for (u32 month = 1; month < time.month; ++month)
        before += effect_month_days(time.year, month);
    const float day = static_cast<float>(time.hour * 60u + time.minute) * kEffectClockMinute;
    const float days = effect_leap_year(time.year) ? 366.f : 365.f;
    return {day, (static_cast<float>(before + time.day - 1u) + day) / days, static_cast<float>(time.month)};
}

// Dark wins: the effect turns dark when MaiCord's theme base is dark (Dark, AMOLED or a dark custom theme) or when
// Windows' app mode is. The effect sits under the theme's text: a light picture would break a dark theme's
// contrast, while a light theme's plates are opaque enough to keep its text readable over a night picture.
constexpr float effect_appearance_dark(bool theme_dark, bool system_dark) noexcept {
    return theme_dark || system_dark ? 1.f : 0.f;
}

// One eased value; a cyclic one (0..1 that wraps, as the clock does) moves the short way round.
struct EffectEase {
    float from, to;
    u64 start_ms;
    bool running;
};
constexpr float effect_cyclic_delta(float delta) noexcept {
    return delta >= .5f ? delta - 1 : delta < -.5f ? delta + 1 : delta;
}
constexpr bool effect_ease_running(const EffectEase& ease, u64 now_ms) noexcept {
    return ease.running && (now_ms < ease.start_ms || now_ms - ease.start_ms < kEffectAmbientEaseMs);
}
constexpr float effect_ease_value(const EffectEase& ease, u64 now_ms, bool cyclic) noexcept {
    if (!effect_ease_running(ease, now_ms))
        return ease.to;
    const float t = static_cast<float>(now_ms > ease.start_ms ? now_ms - ease.start_ms : 0) /
                    static_cast<float>(kEffectAmbientEaseMs);
    const float s = t * t * (3 - 2 * t);
    if (!cyclic)
        return ease.from + (ease.to - ease.from) * s;
    const float value = ease.from + effect_cyclic_delta(ease.to - ease.from) * s;
    return value < 0 ? value + 1 : value >= 1 ? value - 1 : value;
}
// Points `ease` at `target`. A change of at most `drift` is followed at once (a transition under way keeps its
// course to the new end); a larger one eases from where the value is now, or settles when `animate` is false.
constexpr void effect_ease_toward(EffectEase* ease, float target, u64 now_ms, bool animate, bool cyclic,
                                  float drift) noexcept {
    if (ease->running && !effect_ease_running(*ease, now_ms)) {
        ease->from = ease->to;
        ease->running = false;
    }
    if (target == ease->to && (animate || !ease->running))
        return;
    if (!animate) {
        *ease = {target, target, now_ms, false};
        return;
    }
    const float change = cyclic ? effect_cyclic_delta(target - ease->to) : target - ease->to;
    if (change <= drift && change >= -drift) {
        ease->to = target;
        if (!ease->running)
            ease->from = target;
        return;
    }
    *ease = {effect_ease_value(*ease, now_ms, cyclic), target, now_ms, true};
}

// What the host reads each frame: the local time, MaiCord's theme base and Windows' app mode.
struct EffectAmbientSources {
    EffectLocalTime local;
    bool theme_dark, system_dark;
};
struct EffectAmbient {
    EffectEase day, year, dark;
    float month;
    bool started; // the first sample settles at once
};
// One frame's sample for a package that declares `uses`: retargets from the sources, then writes mc_ambient
// (zero for an input the package does not declare). `animate` false settles every change at once.
constexpr void effect_ambient_sample(EffectAmbient* ambient, u32 uses, const EffectAmbientSources& sources,
                                     u64 now_ms, bool animate, float (&values)[4]) noexcept {
    const bool ease = animate && ambient->started;
    ambient->started = true;
    for (float& value : values)
        value = 0;
    if (uses & effect_use_clock) {
        const EffectClock clock = effect_clock(sources.local);
        effect_ease_toward(&ambient->day, clock.day, now_ms, ease, true, kEffectClockJump);
        effect_ease_toward(&ambient->year, clock.year, now_ms, ease, true, kEffectClockJump / 365.f);
        ambient->month = clock.month;
        values[effect_ambient_day] = effect_ease_value(ambient->day, now_ms, true);
        values[effect_ambient_year] = effect_ease_value(ambient->year, now_ms, true);
        values[effect_ambient_month] = ambient->month;
    }
    if (uses & effect_use_appearance) {
        effect_ease_toward(&ambient->dark, effect_appearance_dark(sources.theme_dark, sources.system_dark), now_ms,
                           ease, false, 0);
        values[effect_ambient_dark] = effect_ease_value(ambient->dark, now_ms, false);
    }
}
// A value of the last sample is still easing: the host keeps drawing frames until every one settles.
constexpr bool effect_ambient_easing(const EffectAmbient& ambient, u64 now_ms) noexcept {
    return effect_ease_running(ambient.day, now_ms) || effect_ease_running(ambient.year, now_ms) ||
           effect_ease_running(ambient.dark, now_ms);
}

// ---- What the renderer is given ---------------------------------------------------------------
struct EffectTexturePixels {
    // Premultiplication-free RGBA8 (the package's PNG, decoded), or with `rgba32f` RGBA32F texels (an interface-3
    // data texture, borrowed from the package); rows contiguous.
    const void* pixels;
    u16 width, height;
    bool rgba32f;
};

inline constexpr u32 kEffectTextureCount = 4; // TEXTURE2 and TEXTURE3 are interface 3
inline constexpr u32 kEffectTextureMaximumExtent = 512;

// A loaded effect package. The bytes are borrowed for the call.
struct BackgroundEffectSpec {
    ConstSlice<u8> bytecode; // DXBC, ps_5_0 (a graph's shader 0)
    u64 identity;            // a hash of the package; persisted choices and refusals are keyed by it
    u32 uses;                // EffectUse mask
    EffectBlend blend;       // the package's default blend
    u8 requested_resolution, requested_frame_rate; // step + 1, 0 = no request
    float knobs[kEffectKnobRows][4];
    EffectTexturePixels textures[kEffectTextureCount];
    const EffectGraph* graph;      // null: the single pass
    const ConstSlice<u8>* shaders; // with a graph: its graph->shader_count shaders ([0] is `bytecode`)
};

// The user's side: universal knobs and the steps they resolved to.
struct EffectSettings {
    float speed;   // 0..3
    float opacity; // 0..1
    EffectBlend blend;
    u8 resolution_step, frame_rate_step; // indices; the resolution is the ceiling the controller never exceeds
    EffectBudget budget;
    bool pointer_allowed;
};

constexpr bool effect_settings_valid(const EffectSettings& settings) noexcept {
    return is_finite(settings.speed) && settings.speed >= 0 && settings.speed <= 3 && is_finite(settings.opacity) &&
           settings.opacity >= 0 && settings.opacity <= 1 && static_cast<u8>(settings.blend) <= 2 &&
           settings.resolution_step < kEffectResolutionSteps && settings.frame_rate_step < kEffectFrameRateSteps &&
           static_cast<u8>(settings.budget) <= 2;
}

// ---- GPU budget controller ---------------------------------------------------------------------
// Every effect frame's GPU time is a sample. Over its budget for a second: one resolution step down.
// Under a third of it for five: one step back up, never above the user's step. One stall past 8 ms:
// straight to the lowest step. Over budget at the lowest step for three seconds, or a single frame past
// 50 ms: the effect is turned off.
inline constexpr float kEffectStallMs = 8, kEffectRunawayMs = 50;
inline constexpr u64 kEffectWindowMs = 1000, kEffectCalmMs = 5000, kEffectOverloadMs = 3000;
// Samples ignored after a step change: the first frame at a new size pays for lazily built driver state.
inline constexpr u32 kEffectWarmupSamples = 2;

struct EffectBudgetState {
    u64 window_start_ms, calm_since_ms, overload_since_ms;
    float window_total_ms;
    u32 window_frames, warmup;
    u8 step, ceiling;
};

enum class EffectBudgetAction : u8 { none, step_down, step_up, drop_to_floor, disable };

// Starts the controller at `step` under a user ceiling.
constexpr void effect_budget_start(EffectBudgetState* state, u32 step, u32 ceiling) noexcept {
    *state = {};
    state->ceiling = static_cast<u8>(ceiling);
    state->step = static_cast<u8>(step < ceiling ? step : ceiling);
    state->warmup = kEffectWarmupSamples;
}

constexpr EffectBudgetAction effect_budget_sample(EffectBudgetState* state, float gpu_ms, float budget_ms,
                                                  u64 now_ms) noexcept {
    if (!is_finite(gpu_ms) || gpu_ms < 0 || !(budget_ms > 0))
        return EffectBudgetAction::none;
    if (gpu_ms > kEffectRunawayMs)
        return EffectBudgetAction::disable;
    if (state->warmup) {
        --state->warmup;
        return EffectBudgetAction::none;
    }
    if (gpu_ms > kEffectStallMs && state->step) {
        state->step = 0;
        state->warmup = kEffectWarmupSamples;
        state->window_frames = 0;
        state->window_total_ms = 0;
        state->calm_since_ms = 0;
        return EffectBudgetAction::drop_to_floor;
    }
    if (!state->window_frames || now_ms < state->window_start_ms) {
        state->window_start_ms = now_ms;
        state->window_total_ms = 0;
        state->window_frames = 0;
    }
    state->window_total_ms += gpu_ms;
    ++state->window_frames;
    if (now_ms - state->window_start_ms < kEffectWindowMs)
        return EffectBudgetAction::none;
    const float average = state->window_total_ms / static_cast<float>(state->window_frames);
    const u64 window_start = state->window_start_ms;
    state->window_total_ms = 0;
    state->window_frames = 0;
    if (average > budget_ms) {
        state->calm_since_ms = 0;
        if (state->step) {
            --state->step;
            state->warmup = kEffectWarmupSamples;
            state->overload_since_ms = 0;
            return EffectBudgetAction::step_down;
        }
        if (!state->overload_since_ms)
            state->overload_since_ms = window_start ? window_start : 1;
        return now_ms - state->overload_since_ms >= kEffectOverloadMs ? EffectBudgetAction::disable
                                                                      : EffectBudgetAction::none;
    }
    state->overload_since_ms = 0;
    if (average >= budget_ms * (1.f / 3.f)) {
        state->calm_since_ms = 0;
        return EffectBudgetAction::none;
    }
    if (!state->calm_since_ms) {
        state->calm_since_ms = window_start ? window_start : 1;
        return EffectBudgetAction::none;
    }
    if (now_ms - state->calm_since_ms >= kEffectCalmMs && state->step < state->ceiling) {
        ++state->step;
        state->warmup = kEffectWarmupSamples;
        state->calm_since_ms = 0;
        return EffectBudgetAction::step_up;
    }
    return EffectBudgetAction::none;
}

// ---- Selection probe -----------------------------------------------------------------------------
// Two tiny renders (frame time in ms at two pixel counts) fix a line: a fixed cost plus a per-pixel one.
struct EffectProbe {
    float small_ms, large_ms;
    u32 small_pixels, large_pixels;
};
constexpr float effect_probe_estimate(const EffectProbe& probe, u64 pixels) noexcept {
    const float fixed = probe.small_ms < probe.large_ms ? probe.small_ms : probe.large_ms;
    const float extra = probe.large_ms > probe.small_ms ? probe.large_ms - probe.small_ms : 0.f;
    const float span = probe.large_pixels > probe.small_pixels
                           ? static_cast<float>(probe.large_pixels - probe.small_pixels)
                           : 1.f;
    const float scaled = pixels > probe.small_pixels ? static_cast<float>(pixels - probe.small_pixels) : 0.f;
    return fixed + extra * scaled / span;
}
inline constexpr float kEffectRefuseMs = 16;

constexpr u32 effect_scaled_extent(u32 extent, u32 step) noexcept {
    const u32 scaled = (extent * kEffectResolutionPercent[step] + 50) / 100;
    return scaled ? scaled : 1;
}
constexpr u64 effect_step_pixels(u32 width, u32 height, u32 step) noexcept {
    return static_cast<u64>(effect_scaled_extent(width, step)) * effect_scaled_extent(height, step);
}

// The highest step at or under `ceiling` whose estimated frame fits the budget (step 0 when none does);
// `refuse` is set when even the lowest step is estimated beyond kEffectRefuseMs.
constexpr u32 effect_probe_start_step(const EffectProbe& probe, u32 width, u32 height, float budget_ms,
                                      u32 ceiling, bool* refuse) noexcept {
    *refuse = effect_probe_estimate(probe, effect_step_pixels(width, height, 0)) > kEffectRefuseMs;
    u32 step = ceiling;
    while (step && effect_probe_estimate(probe, effect_step_pixels(width, height, step)) > budget_ms)
        --step;
    return step;
}

// ---- Seconds per frame -----------------------------------------------------------------------------
constexpr u32 effect_interval_ms(u32 frame_rate_step, bool slow) noexcept {
    const u32 fps = slow && kEffectUnfocusedFramesPerSecond < kEffectFramesPerSecond[frame_rate_step]
                        ? kEffectUnfocusedFramesPerSecond
                        : kEffectFramesPerSecond[frame_rate_step];
    return (1000 + fps / 2) / fps;
}

// What the renderer decided about a selected effect, for the host to act on.
enum class EffectVerdict : u8 {
    none,
    refused,        // the cost probe: too heavy for this GPU even at the lowest resolution
    overloaded,     // the budget controller turned it off
    device_removed, // the GPU reset while this effect's frame was in flight
};

// ---- The user's settings ---------------------------------------------------------------------------------
// The global rows of Settings > Appearance.
struct EffectUserSettings {
    u8 resolution, frame_rate; // step + 1; 0 = automatic (the package's request, then the default)
    EffectBudget budget;
    EffectUnfocused unfocused;
};
inline constexpr EffectUserSettings kEffectUserDefaults{0, 0, EffectBudget::balanced, EffectUnfocused::slow};

// One package's own: the universal knobs, and overrides that win over the global rows.
struct EffectShaderSettings {
    u64 identity;
    u8 resolution, frame_rate; // step + 1; 0 = follow the global row
    u8 speed;                  // twentieths of a second per second: 0..60
    u8 opacity;                // percent
    u8 blend;                  // 0 = the package's own, else EffectBlend + 1
    u8 pointer;                // 1 = the effect may read the pointer
    u8 reserved[2];
};
static_assert(sizeof(EffectShaderSettings) == 16);
constexpr EffectShaderSettings effect_shader_defaults(u64 identity) noexcept {
    return {identity, 0, 0, 20, 100, 0, 0, {}};
}
constexpr bool effect_shader_settings_valid(const EffectShaderSettings& value) noexcept {
    return value.resolution <= kEffectResolutionSteps && value.frame_rate <= kEffectFrameRateSteps &&
           value.speed <= 60 && value.opacity <= 100 && value.blend <= 3 && value.pointer <= 1;
}
constexpr bool effect_user_settings_valid(const EffectUserSettings& value) noexcept {
    return value.resolution <= kEffectResolutionSteps && value.frame_rate <= kEffectFrameRateSteps &&
           static_cast<u8>(value.budget) <= 2 && static_cast<u8>(value.unfocused) <= 2;
}

// The renderer's settings for a package: the per-shader override wins over the global row, which wins over
// what the package requests, which wins over the defaults (50 %, 24 fps).
constexpr EffectSettings effect_resolve_settings(const EffectUserSettings& user, const EffectShaderSettings& shader,
                                                 u8 requested_resolution, u8 requested_frame_rate,
                                                 EffectBlend package_blend) noexcept {
    EffectSettings settings{};
    settings.speed = static_cast<float>(shader.speed) * .05f;
    settings.opacity = static_cast<float>(shader.opacity) * .01f;
    settings.blend = shader.blend ? static_cast<EffectBlend>(shader.blend - 1) : package_blend;
    settings.resolution_step = static_cast<u8>(effect_resolve_step(shader.resolution, user.resolution,
                                                                   requested_resolution, kEffectDefaultResolutionStep,
                                                                   kEffectResolutionSteps));
    settings.frame_rate_step = static_cast<u8>(effect_resolve_step(shader.frame_rate, user.frame_rate,
                                                                   requested_frame_rate, kEffectDefaultFrameRateStep,
                                                                   kEffectFrameRateSteps));
    settings.budget = user.budget;
    settings.pointer_allowed = shader.pointer != 0;
    return settings;
}
} // namespace maicord
