// Plasma: the classic sum of sines, tinted toward the theme so it never fights the UI.
//
// @effect   name="Plasma" authors="MaiCord" license="MPL-2.0"
// @knob speed  float 0..3     = 0.8  "Speed"
// @knob scale  float 0.5..4   = 1.6  "Scale"
// @knob mix    float 0.05..1  = 0.5  "Colour strength"
// @knob swirl  float 0..2     = 0.6  "Swirl"
// @uses blue_noise
// @blend normal
#include <maicord/sdk/background.hlsli>

#define knob_speed (mc_knobs[0].x)
#define knob_scale (mc_knobs[0].y)
#define knob_mix   (mc_knobs[0].z)
#define knob_swirl (mc_knobs[0].w)

float4 background(float2 pixel) {
    float2 uv = (pixel - 0.5 * mc.resolution) * (knob_scale / mc.resolution.y);
    const float time = mc.time * knob_speed;
    // A slow rotation about the centre adds the swirl.
    const float angle = knob_swirl * 0.25 * sin(time * 0.37);
    const float s = sin(angle), c = cos(angle);
    uv = float2(c * uv.x - s * uv.y, s * uv.x + c * uv.y);

    const float v = sin(uv.x * 3.0 + time) + sin(uv.y * 4.0 - time * 1.3) + sin((uv.x + uv.y) * 2.0 + time * 0.7) +
                    sin(length(uv) * 5.0 - time);
    // Three phases of the sum, cos(v + 0, 2.1, 4.2), from one sine and cosine of it; pulled toward the theme's accent
    // and positive colours.
    float sin_v, cos_v;
    sincos(v, sin_v, cos_v);
    const float3 wave =
        0.5 + 0.5 * (cos_v * float3(1.0, -0.50484610, -0.49026082) - sin_v * float3(0.0, 0.86320937, -0.87157577));
    const float3 palette = lerp(mc_role(MC_ROLE_ACCENT), mc_role(MC_ROLE_POSITIVE), wave.g);
    const float3 colour = lerp(mc_role(MC_ROLE_BACKGROUND), lerp(wave, palette, 0.55), knob_mix * mc.intensity);
    return float4(colour + mc_dither(pixel), 1.0);
}
