// Film grain: a faint, constantly changing speckle over everything, light and dark, for a printed-paper
// feel. Subtle by default; it adds no colour of its own.
//
// @effect   name="Film grain" authors="MaiCord" license="MPL-2.0"
// @knob amount float 0.02..0.4 = 0.1  "Amount"
// @knob size   float 0.5..4    = 1.0  "Grain size (DIPs)"
// @knob rate   float 1..30     = 12   "Changes per second"
// @uses noise
// @blend normal
#include <maicord/sdk/background.hlsli>

#define knob_amount (mc_knobs[0].x)
#define knob_size   (mc_knobs[0].y)
#define knob_rate   (mc_knobs[0].z)

float4 background(float2 pixel) {
    // The speckle changes a few times a second, not every frame: a stepped offset into the noise.
    const float step_index = floor(mc.time * knob_rate);
    const float2 jump = mc_hash4(float2(step_index, 5.0)).xy;
    const float2 uv = pixel / (knob_size * mc.dip * MC_NOISE_SIZE) + jump;
    const float value = mc_noise(uv).x;
    // Light grain on dark themes, dark on light ones; strongest at the extremes of the noise.
    const float strength = abs(value - 0.5) * 2.0;
    const float3 ink = lerp(float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0), value > 0.5 ? mc.dark : 1.0 - mc.dark);
    return float4(ink, strength * strength * knob_amount * lerp(0.6, 1.0, mc.intensity));
}
