// Soft bokeh: out-of-focus discs drifting slowly upward in three depth layers, in the theme's accent and text
// colours. Each disc has a faint body and a brighter rim; nothing is drawn between them.
//
// @effect   name="Soft bokeh" authors="MaiCord" license="MPL-2.0"
// @knob density float 0.1..0.9 = 0.42 "Density"
// @knob speed   float 0..2     = 0.5  "Drift speed"
// @knob size    float 0.5..2   = 1.0  "Disc size"
// @knob glow    float 0.2..1.5 = 0.8  "Brightness"
// @blend screen
#include <maicord/sdk/background.hlsli>

#define knob_density (mc_knobs[0].x)
#define knob_speed   (mc_knobs[0].y)
#define knob_size    (mc_knobs[0].z)
#define knob_glow    (mc_knobs[0].w)

float4 background(float2 pixel) {
    float3 light = float3(0.0, 0.0, 0.0);
    float alpha = 0.0;
    [unroll] for (int layer = 0; layer < 3; ++layer) {
        const float depth = (float)layer;
        // A grid of cells (in DIPs), scrolling upward; at most one disc per cell, kept inside it.
        const float pitch = (92.0 + 54.0 * depth) * knob_size * mc.dip;
        const float2 scrolled =
            pixel * (1.0 / pitch) + float2(depth * 3.7, mc.time * knob_speed * (0.025 + 0.015 * depth));
        const float2 cell = floor(scrolled);
        const float4 n = mc_hash4(cell + depth * 31.0);
        const float2 centre = cell + 0.3 + 0.4 * n.xy;
        const float radius = 0.17 + 0.13 * n.z;
        const float2 offset = scrolled - centre;
        // Only a cell with a disc, and only inside it, draws anything: a real branch, so whole blocks of pixels skip
        // the rest.
        [branch] if (n.w <= knob_density && dot(offset, offset) < radius * radius) {
            const float d = length(offset) / radius;
            // A body, and a rim a little brighter than it, both soft at the edge.
            const float body = smoothstep(1.0, 0.86, d) * 0.22;
            const float rim = smoothstep(0.72, 0.94, d) * smoothstep(1.0, 0.94, d) * 0.34;
            const float breathe = 0.65 + 0.35 * sin(mc.time * (0.3 + 0.5 * n.z) + n.x * 6.2831853);
            const float3 tint = lerp(mc_role(MC_ROLE_ACCENT), mc_role(MC_ROLE_TEXT), n.y * n.y);
            const float a = (body + rim) * breathe * (0.7 + 0.15 * depth);
            light += tint * a;
            alpha += a;
        }
    }
    // Straight colour is the light averaged over the discs; the sum, scaled, is the coverage.
    const float coverage = saturate(alpha * knob_glow * lerp(0.55, 1.0, mc.intensity));
    return float4(light * (1.0 / max(alpha, 1e-4)), coverage);
}
