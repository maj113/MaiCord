// Aurora: soft curtains of light drifting across the top of the window, tinted by the theme's accent and
// positive roles over its background. Cheap: a few sines per pixel, no textures.
//
// @effect   name="Aurora" authors="MaiCord" license="MPL-2.0"
// @knob speed    float 0..2   = 0.35 "Drift speed"
// @knob strength float 0.2..1 = 0.7  "Brightness"
// @knob height   float 0.3..1 = 0.75 "Reach (share of the window)"
// @knob curtains int   1..4   = 3    "Curtains"
// @uses blue_noise
// @blend normal
#include <maicord/sdk/background.hlsli>

#define knob_speed    (mc_knobs[0].x)
#define knob_strength (mc_knobs[0].y)
#define knob_height   (mc_knobs[0].z)
#define knob_curtains ((int)mc_knobs[0].w)

// (sin, cos) of an angle turned by the angle whose (sin, cos) is `by`.
float2 turned(float2 angle, float2 by) {
    return float2(angle.x * by.y + angle.y * by.x, angle.y * by.y - angle.x * by.x);
}

float4 background(float2 pixel) {
    const float2 uv = pixel * (1.0 / mc.resolution);
    const float time = mc.time * knob_speed;
    // A fixed aspect so the curtains keep their shape in any window.
    const float x = pixel.x * (1.0 / mc.resolution.y);

    // The per-pixel angles step by a fixed amount from one curtain to the next, so they are turned rather than
    // evaluated again: the wandering of the centre line and the ripple along it, as (sin, cos).
    float2 wander, wander_step, ripple, ripple_step;
    sincos(x * 1.1 + time * 0.8, wander.x, wander.y);
    sincos(x * 0.3 + time * 0.25 + 2.1, wander_step.x, wander_step.y);
    sincos(x * 5.0 - time * 1.4, ripple.x, ripple.y);
    sincos(x * 1.7 - time * 0.3, ripple_step.x, ripple_step.y);
    // Accent and positive light, summed apart: the tint is a blend of the two.
    float accent = 0.0, positive = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        const float k = (float)i;
        // A wandering centre line for the curtain, and a ripple along it that also sways (the same for every pixel).
        const float line_y = 0.16 + 0.11 * k + 0.07 * wander.x;
        float2 sway;
        sincos(sin(time * 0.3 + k) * 2.0, sway.x, sway.y);
        const float ripple_value = 0.5 + 0.5 * turned(ripple, sway).x;
        // A hard lower edge on the line; the glow trails upward and away from it.
        const float above = exp2(-max(uv.y - line_y, 0.0) * ((5.0 - 0.6 * k) * 1.44269504 / knob_height));
        const float edge = saturate(1.0 - max(line_y - uv.y, 0.0) * 6.0);
        const float curtain =
            above * edge * (0.45 + 0.55 * ripple_value) * ((0.55 - 0.1 * k) * (i < knob_curtains ? 1.0 : 0.0));
        const float share = saturate(0.25 + 0.4 * k + 0.2 * sin(time + k));
        accent += curtain * (1.0 - share);
        positive += curtain * share;
        wander = turned(wander, wander_step);
        ripple = turned(ripple, ripple_step);
    }
    const float3 light = mc_role(MC_ROLE_ACCENT) * accent + mc_role(MC_ROLE_POSITIVE) * positive;
    // The theme's own strength: fainter in a light theme.
    const float amount = knob_strength * lerp(0.45, 1.0, mc.intensity) * lerp(0.6, 1.0, mc.dark);
    const float3 colour = mc_role(MC_ROLE_BACKGROUND) + light * amount;
    return float4(colour + mc_dither(pixel), 1.0);
}
