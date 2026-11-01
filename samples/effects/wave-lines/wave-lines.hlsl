// Wave lines: a stack of thin horizontal lines rippling with a few overlaid sines, fading from the theme's
// accent to its positive colour down the window. Calm and cheap: one loop of sixteen sines.
//
// @effect   name="Wave lines" authors="MaiCord" license="MPL-2.0"
// @knob lines     int   4..16     = 11   "Lines"
// @knob speed     float 0..2      = 0.45 "Speed"
// @knob amplitude float 0..1      = 0.55 "Wave height"
// @knob thickness float 0.5..3    = 1.1  "Line width (DIPs)"
// @blend screen
#include <maicord/sdk/background.hlsli>

#define knob_lines     ((int)mc_knobs[0].x)
#define knob_speed     (mc_knobs[0].y)
#define knob_amplitude (mc_knobs[0].z)
#define knob_thickness (mc_knobs[0].w)

float4 background(float2 pixel) {
    const float2 uv = mc_uv(pixel);
    const float x = uv.x * (mc.resolution.x / mc.resolution.y);
    const float time = mc.time * knob_speed;
    const float count = (float)knob_lines;
    // Lines sit on an even grid; each wave is a slow sum of three sines, phase-shifted line to line.
    float3 light = float3(0.0, 0.0, 0.0);
    float coverage = 0.0;
    [unroll] for (int i = 0; i < 16; ++i) {
        const float k = (float)i;
        const float base = (k + 0.5) / count;
        const float wave = 0.5 * sin(x * 2.3 + time + k * 0.5) + 0.3 * sin(x * 4.1 - time * 1.3 + k * 0.9) +
                           0.2 * sin(x * 7.7 + time * 0.6 + k * 1.7);
        const float y = base + wave * knob_amplitude * (0.35 / count + 0.015);
        // Distance from the line in DIPs, through the soft edge of its width.
        const float distance_dips = abs(uv.y - y) * mc.resolution.y / mc.dip;
        const float line_value = saturate(1.0 - distance_dips / knob_thickness) * (k < count ? 1.0 : 0.0);
        const float3 tint = lerp(mc_role(MC_ROLE_ACCENT), mc_role(MC_ROLE_POSITIVE), saturate(base));
        light += tint * line_value;
        coverage += line_value;
    }
    // Straight colour is the light averaged over the lines that reach this pixel.
    return float4(light / max(coverage, 1e-4), saturate(coverage) * lerp(0.5, 0.9, mc.intensity));
}
