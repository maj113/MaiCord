// The pass graph the background probe checks the graph pass against (interface 3). Each entry point is compiled on
// its own; the probe declares the graph as a manifest would:
//   targets  0: rgba32f 4x4, persistent    1: rgba16f at 50 % of the output
//   shaders  0 count_ps, 1 half_ps, 2 fill_ps, 3 grid_vs, 4 solid_ps, 5 quad_vs
//   passes   count_ps -> 0 (reads 0)            half_ps -> 1
//            fill_ps -> output (reads 0, 1)     grid_vs + solid_ps, grid(3, 2) -> output, add
//            quad_vs + solid_ps, quads(2) -> output, alpha
// After k frames since the last clear the output is (k / 16, 0, 0.5) opaque, plus green 0.5 on the right half and
// half-transparent white over two bars in the left half.
#define MC_GRAPH
#include <maicord/sdk/background.hlsli>

// Frames since the last clear: the last state plus one.
float4 count_ps(float4 position : SV_Position) : SV_Target {
    return mc_target0.Load(int3(int2(position.xy), 0)) + float4(1.0, 0.0, 0.0, 0.0);
}

// A constant the output reads back through a sampler from the half-size float target.
float4 half_ps(float4 position : SV_Position) : SV_Target {
    return float4(0.0, 0.0, 0.5, 1.0);
}

float4 fill_ps(float4 position : SV_Position) : SV_Target {
    const float frames = mc_target0.Load(int3(0, 0, 0)).x;
    return float4(frames / 16.0, 0.0, mc_target1.SampleLevel(mc_linear_clamp, mc_uv(position.xy), 0).b, 1.0);
}

struct Solid {
    float4 position : SV_Position;
    float4 colour : COLOR0;
};

// The right half of the output as a 3 x 2 grid of points.
Solid grid_vs(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    const float2 grid = float2(mc_grid_point(vertex_id, instance_id)) / (mc_pass.count - 1.0);
    Solid output;
    output.position = mc_clip(float2(0.5 + grid.x * 0.5, grid.y) * mc_pass.size);
    output.colour = float4(0.0, 0.5, 0.0, 0.0);
    return output;
}

// Bars a tenth of the output wide, from 0.05 to 0.45 down, at x 0.1 and 0.3.
Solid quad_vs(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    const float2 corner = float2(0.1 + 0.2 * instance_id, 0.05) + mc_quad_corner(vertex_id) * float2(0.1, 0.4);
    Solid output;
    output.position = mc_clip(corner * mc_pass.size);
    output.colour = float4(1.0, 1.0, 1.0, 0.5);
    return output;
}

float4 solid_ps(Solid input) : SV_Target {
    return input.colour;
}
