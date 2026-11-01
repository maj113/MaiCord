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

float4 background(float2 pixel) {
    const float2 uv = mc_uv(pixel);
    const float time = mc.time * knob_speed;
    // A fixed aspect so the curtains keep their shape in any window.
    const float x = uv.x * (mc.resolution.x / mc.resolution.y);

    float3 light = float3(0.0, 0.0, 0.0);
    [unroll] for (int i = 0; i < 4; ++i) {
        const float k = (float)i;
        // A wandering centre line for the curtain, and a ripple along it.
        const float line_y = 0.16 + 0.11 * k + 0.07 * sin(x * (1.1 + 0.3 * k) + time * (0.8 + 0.25 * k) + k * 2.1);
        const float ripple = 0.5 + 0.5 * sin(x * (5.0 + 1.7 * k) - time * (1.4 + 0.3 * k) + sin(time * 0.3 + k) * 2.0);
        // A hard lower edge on the line; the glow trails upward and away from it.
        const float above = exp(-max(uv.y - line_y, 0.0) * (5.0 - 0.6 * k) / knob_height);
        const float edge = saturate(1.0 - max(line_y - uv.y, 0.0) * 6.0);
        const float curtain = above * edge * (0.45 + 0.55 * ripple) * (i < knob_curtains ? 1.0 : 0.0);
        const float3 tint =
            lerp(mc_role(MC_ROLE_ACCENT), mc_role(MC_ROLE_POSITIVE), saturate(0.25 + 0.4 * k + 0.2 * sin(time + k)));
        light += tint * curtain * (0.55 - 0.1 * k);
    }
    // The theme's own strength: fainter in a light theme.
    const float amount = knob_strength * lerp(0.45, 1.0, mc.intensity) * lerp(0.6, 1.0, mc.dark);
    const float3 colour = mc_role(MC_ROLE_BACKGROUND) + light * amount;
    return float4(colour + mc_dither(pixel), 1.0);
}
