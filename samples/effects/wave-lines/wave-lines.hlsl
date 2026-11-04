// Wave lines: a stack of thin horizontal lines rippling with a few overlaid sines, fading from the theme's
// accent to its positive colour down the window. Calm and cheap: each pixel sums only the few lines whose
// waves can reach it.
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
    // In window heights: y runs 0..1 down the window, and x keeps the waves' shape in any window.
    const float2 uv = pixel * (1.0 / mc.resolution.y);
    const float x = uv.x;
    const float time = mc.time * knob_speed;
    const float count = (float)knob_lines;
    // Lines sit on an even grid; each wave is a slow sum of three sines, phase-shifted line to line. A wave moves
    // its line at most `reach` from its place, and a line lights pixels less than `width` from it (both in uv).
    const float reach = knob_amplitude * (0.35 / count + 0.015);
    const float width = knob_thickness * mc.dip / mc.resolution.y, sharpness = 1.0 / width;
    // So only lines k with |uv.y - (k + 0.5) / count| < reach + width can light this pixel.
    const float centre = uv.y * count - 0.5, spread = (reach + width) * count + 0.01, gap = 1.0 / count;
    const float first = max(ceil(centre - spread), 0.0), last = min(floor(centre + spread), min(count, 16.0) - 1.0);
    float coverage = 0.0, along = 0.0;
    [loop] for (float k = first; k <= last; ++k) {
        const float base = (k + 0.5) * gap;
        const float wave = 0.5 * sin(x * 2.3 + time + k * 0.5) + 0.3 * sin(x * 4.1 - time * 1.3 + k * 0.9) +
                           0.2 * sin(x * 7.7 + time * 0.6 + k * 1.7);
        // Through the soft edge of the line's width.
        const float line_value = saturate(1.0 - abs(uv.y - base - wave * reach) * sharpness);
        coverage += line_value;
        along += line_value * base;
    }
    // Straight colour is the tint averaged over the lines that reach this pixel: accent at the top, positive below.
    const float3 tint = lerp(mc_role(MC_ROLE_ACCENT), mc_role(MC_ROLE_POSITIVE), along / max(coverage, 1e-4));
    return float4(tint, saturate(coverage) * lerp(0.5, 0.9, mc.intensity));
}
