// Spark ribbon: a pass graph (interface 3). A twisting ribbon drawn as a 96 x 24 grid of vertices accumulates
// additively into a half-size float target, so where the sheet folds over itself it glows brighter; a full-screen
// pass lays it over a gradient of the theme's background; a thousand sparks, simulated one per texel of a
// persistent float target, drift off the ribbon and fade, drawn as additive quads.
//
// @effect   name="Spark Ribbon" authors="MaiCord" license="MPL-2.0" version="1.0.0"
// @knob speed float 0..2 = 0.6 "Speed"
// @knob glow  float 0..1 = 0.6 % "Glow"
// @knob tint  color = #8CB8FF "Ribbon colour"
// @uses blue_noise
// @blend normal
// @target parts rgba32f 32x32 persistent
// @target glow  rgba16f 50%
// @pass simulate to=parts in=parts
// @pass ribbon vs=ribbon_vs grid=96x24 to=glow blend=add clear
// @pass compose in=glow
// @pass spark vs=spark_vs quads=1024 in=parts blend=add
#define MC_GRAPH
#include <maicord/sdk/background.hlsli>

#define knob_speed (mc_knobs[0].x)
#define knob_glow  (mc_knobs[0].y)
#define knob_tint  (mc_knobs[1].rgb)

// The ribbon's centre line: its height at u (0..1 across the output) and time t, in output uv.
float ribbon_y(float u, float t) {
    return 0.55 + 0.12 * sin(u * 5.0 + t * 0.7) + 0.05 * sin(u * 11.0 - t * 1.1);
}

// One spark per texel of `parts`: xy its place in output uv, z its age (0..1), w 1 once it has been born (a cleared
// target reads 0, so every spark is born on the first frame, at a random age so they don't all fade together).
float4 simulate(float4 position : SV_Position) : SV_Target {
    const int2 texel = int2(position.xy);
    const float id = (float)(texel.y * 32 + texel.x);
    float4 spark = mc_target0.Load(int3(texel, 0));
    const float t = mc.time * knob_speed;
    if (spark.w == 0.0 || spark.z >= 1.0) {
        // Born somewhere on the ribbon (see ribbon_vs).
        const float4 r = mc_hash4(float2(id, mc.frame));
        const float y = ribbon_y(r.x, t) + (r.y - 0.5) * 0.24 * sin(r.x * 3.0 + t * 0.5);
        return float4(r.x * 1.2 - 0.1, y, spark.w == 0.0 ? r.z : 0.0, 1.0);
    }
    // A slow swirl that carries the sparks off the ribbon.
    const float2 flow = float2(0.08 + 0.06 * sin(spark.y * 9.0 + t), 0.09 * cos(spark.x * 7.0 - t * 1.3));
    const float dt = min(mc.delta, 0.1) * (0.4 + knob_speed);
    spark.xy += flow * dt;
    spark.z += dt * (0.12 + 0.1 * frac(id * 0.618));
    return spark;
}

struct RibbonVertex {
    float4 position : SV_Position;
    float2 sheet : TEXCOORD0; // along, across the ribbon (0..1)
};

RibbonVertex ribbon_vs(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    const float2 sheet = float2(mc_grid_point(vertex_id, instance_id)) / (mc_pass.count - 1.0);
    const float t = mc.time * knob_speed;
    // The sheet turns as it runs, so it folds over itself where the twist crosses zero.
    const float twist = sin(sheet.x * 3.0 + t * 0.5);
    const float2 uv = float2(sheet.x * 1.2 - 0.1, ribbon_y(sheet.x, t) + (sheet.y - 0.5) * 0.24 * twist);
    RibbonVertex output;
    output.position = mc_clip(uv * mc_pass.size);
    output.sheet = sheet;
    return output;
}

// Thin bright lines across a faint body; overlapping layers add up in the float target.
float4 ribbon(RibbonVertex input) : SV_Target {
    const float stripes = pow(abs(cos(input.sheet.y * 3.14159265 * 6.0)), 16.0);
    const float body = 1.0 - abs(input.sheet.y * 2.0 - 1.0);
    return float4(knob_tint * (0.1 * body + 0.45 * stripes), 0.0);
}

float4 compose(float4 position : SV_Position) : SV_Target {
    const float2 uv = mc_uv(position.xy);
    const float3 sky = lerp(mc_role(MC_ROLE_BACKGROUND), mc_role(MC_ROLE_SURFACE), uv.y);
    const float3 glow = mc_target1.SampleLevel(mc_linear_clamp, uv, 0).rgb;
    return float4(sky + glow * (0.5 + knob_glow) * mc.intensity + mc_dither(position.xy), 1.0);
}

struct SparkVertex {
    float4 position : SV_Position;
    float2 corner : TEXCOORD0; // -1..1 across the quad
    float light : TEXCOORD1;
};

SparkVertex spark_vs(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    const float4 spark = mc_target0.Load(int3(instance_id & 31u, instance_id >> 5, 0));
    const float2 corner = mc_quad_corner(vertex_id) * 2.0 - 1.0;
    SparkVertex output;
    output.position = mc_clip(spark.xy * mc_pass.size + corner * 3.0 * mc.dip);
    output.corner = corner;
    // Fades in and out over its life; an unborn spark has no light.
    output.light = spark.w * sin(saturate(spark.z) * 3.14159265);
    return output;
}

float4 spark(SparkVertex input) : SV_Target {
    const float falloff = saturate(1.0 - dot(input.corner, input.corner));
    return float4(lerp(knob_tint, mc_role(MC_ROLE_TEXT), 0.5) * falloff * falloff * input.light * knob_glow, 0.0);
}
