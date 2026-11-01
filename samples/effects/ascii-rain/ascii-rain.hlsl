// ASCII rain: columns of glyphs falling down the window, a bright head with a fading trail.
//
// An original effect in the spirit of the classic falling-code look; it ships with MaiCord under the project
// licence (MPL-2.0). Uses the 16x16 glyph grid built from the code font.
//
// @effect   name="ASCII rain" authors="MaiCord" license="MPL-2.0"
// @knob cell   float 8..32  = 14       "Glyph size (DIPs)"
// @knob speed  float 0.2..3 = 1        "Fall speed"
// @knob trail  float 4..40  = 18       "Trail length (glyphs)"
// @knob mirror toggle       = on       "Mirrored glyphs"
// @knob tint   color        = #26FF66  "Colour"
// @uses glyphs
// @blend screen
//
// The tool generates the accessors below from the annotations: scalars pack four to a row, a colour takes
// the .rgb of a row of its own, a toggle reads (row != 0).
#include <maicord/sdk/background.hlsli>

#define knob_cell   (mc_knobs[0].x)
#define knob_speed  (mc_knobs[0].y)
#define knob_trail  (mc_knobs[0].z)
#define knob_mirror (mc_knobs[0].w != 0)
#define knob_tint   (mc_knobs[1].rgb)

float4 background(float2 pixel) {
    const float cell = knob_cell * mc.dip;
    const float2 scaled = pixel / cell;
    const float2 grid = floor(scaled);
    float2 inside = scaled - grid;

    // One stream per column: its own pace, phase and loop length.
    const float4 column = mc_hash4(float2(grid.x, 17.0));
    const float rows = ceil(mc.resolution.y / cell);
    const float loop = rows + knob_trail + 8.0 + column.w * 20.0;
    const float pace = (0.5 + column.x) * 6.0 * knob_speed;
    const float head = mc_mod(mc.time * pace + column.y * loop, loop);

    // Rows behind the head fall off with their age.
    const float age = head - grid.y;
    if (age < 0.0 || age > knob_trail)
        return float4(0.0, 0.0, 0.0, 0.0);
    const float fade = pow(saturate(1.0 - age / knob_trail), 1.6);

    // The glyph changes a few times a second; the head flickers faster.
    const float tick = floor(mc.time * (age < 1.0 ? 14.0 : 3.0) + column.z * 8.0);
    const float code = 33.0 + floor(mc_hash4(float2(grid.x * 131.0 + tick, grid.y)).x * 222.0);
    if (knob_mirror && column.z > 0.5)
        inside.x = 1.0 - inside.x;
    // The glyphs sit centred in their cells; widen them to fill a square.
    inside.x = 0.5 + (inside.x - 0.5) * 0.55;
    inside.y = 0.1 + inside.y * 0.8;
    const float coverage = mc_glyph_cell((uint)code, inside);

    const float3 head_colour = lerp(knob_tint, float3(1.0, 1.0, 1.0), 0.75);
    const float3 colour = lerp(knob_tint * fade, head_colour, saturate(1.0 - age));
    return float4(colour * coverage, 1.0);
}
