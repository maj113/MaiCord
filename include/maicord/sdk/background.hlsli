// maicord/sdk/background.hlsli -- MaiCord background effect interface 3 (ps_5_0, vs_5_0).
//
// An effect is one function:
//
//     float4 background(float2 pixel);
//
// `pixel` is in effect-target pixels with a top-left origin and pixel centres
// at .5. The function returns straight (not premultiplied) colour and alpha;
// this file supplies main(), which premultiplies. The host renders the target
// at a reduced resolution and frame rate, then every background panel of the
// window samples it, so an effect is continuous across panels.
//
// Colour is display-referred, like a Shadertoy shader: whatever you return is
// what the screen shows, and the theme roles and colour knobs arrive as the
// sRGB values the UI itself uses. There is no gamma step after main().
//
// Knobs come from the generated `knob_<id>` defines (see the tool's output);
// a hand-written effect can read mc_knobs directly.
//
// Everything below is host-supplied; nothing here may be changed by an effect.
//
// Interface 2 adds mc_ambient (the local clock and the light or dark appearance) after mc_knobs, where an
// interface-1 effect never reads. The build tool writes "interface": 2 into the manifest of an effect that uses
// clock or appearance, and 1 otherwise, so an effect without them still runs on every MaiCord.
//
// Interface 3 adds the pass graph: an effect that defines MC_GRAPH before including this file writes its own entry
// points (one per pass, ps_5_0 or vs_5_0, declared with @target and @pass), reads the pass row mc_pass and the
// targets mc_target0..3, and draws host geometry by vertex ID (mc_grid_point, mc_quad_corner). TEXTURE2 and
// TEXTURE3 are interface 3 too. The tool packs such an effect as interface 3.

struct McFrame {
    float  time;        // seconds x the user's Speed; stops while the effect is paused
    float  delta;       // seconds since the previous effect frame
    float  frame;       // effect frames since the effect was selected
    float  dip;         // target pixels per DIP: DPI scale x resolution scale. Size things in DIPs with it.
    float2 resolution;  // effect target in pixels (the whole window x resolution scale)
    float2 pointer;     // pointer in target pixels; (-1,-1) unless the effect uses the pointer and the user allowed it
    float4 palette[8];  // theme roles: MC_ROLE_BACKGROUND, SURFACE, TEXT, MUTED, ACCENT, POSITIVE, WARNING, CRITICAL
    float  dark;        // 1 in a dark theme, and on a light one while MaiCord adapts the effect (see the README)
    float  intensity;   // theme strength 0..1, lower in light themes
    float2 reserved;
};
// Interface 2. Each value is 0 unless the effect declares its input. The host eases them: a change of the
// appearance, or a jump of the clock (a resume, a new time zone), glides over 1.5 s, so blend with them directly.
struct McAmbient {
    float day;          // @uses clock: local time of day, 0..1 from midnight (0.5 is noon), in whole minutes
    float year;         // @uses clock: local time of year, 0..1 from 1 January 00:00
    float month;        // @uses clock: the local month, 1..12 (a whole number; it does not glide)
    float dark;         // @uses appearance: 1 dark, 0 light. Dark when MaiCord's theme is (Dark, AMOLED, a dark
                        // custom theme) or when Windows' app mode is; unlike mc.dark it follows Windows and glides.
};
// Interface 3: the pass being drawn.
struct McPass {
    float2 size;        // the pass's target in pixels (mc.resolution for the output)
    float2 count;       // grid(W, H): W, H; quads(N): N, 1; a full-screen pass: 0, 0
};
cbuffer McBackground : register(b0) {
    McFrame   mc;
    float4    mc_knobs[8];
    McAmbient mc_ambient;
    McPass    mc_pass;
};

Texture2D<float4> mc_noise_tex : register(t0); // 256x256 RGBA8 white noise (PCG), point/wrap
Texture2D<float>  mc_glyph_tex : register(t1); // 16x16 glyph grid, 512x512 R8 coverage, cell = code & 255
Texture2D<float>  mc_blue_tex  : register(t2); // 64x64 blue noise R8, for dithering
Texture2D<float4> mc_tex0      : register(t3); // package TEXTURE0, or 1x1 black
Texture2D<float4> mc_tex1      : register(t4); // package TEXTURE1, or 1x1 black
Texture2D<float4> mc_tex2      : register(t5); // interface 3: package TEXTURE2, or 1x1 black
Texture2D<float4> mc_tex3      : register(t6); // interface 3: package TEXTURE3, or 1x1 black
// Interface 3: the graph's targets in @target order, bound for a pass that names them in `in=`. A persistent
// target read by the pass that writes it holds the last frame's state (zero after a clear).
Texture2D<float4> mc_target0   : register(t7);
Texture2D<float4> mc_target1   : register(t8);
Texture2D<float4> mc_target2   : register(t9);
Texture2D<float4> mc_target3   : register(t10);
SamplerState mc_point_wrap   : register(s0);
SamplerState mc_linear_wrap  : register(s1);
SamplerState mc_linear_clamp : register(s2);

#define MC_NOISE_SIZE 256.0
#define MC_GLYPH_CELLS 16.0

#define MC_ROLE_BACKGROUND 0
#define MC_ROLE_SURFACE    1
#define MC_ROLE_TEXT       2
#define MC_ROLE_MUTED      3
#define MC_ROLE_ACCENT     4
#define MC_ROLE_POSITIVE   5
#define MC_ROLE_WARNING    6
#define MC_ROLE_CRITICAL   7

// GLSL-style mod: the result has the sign of y, where fmod keeps the sign of x.
float  mc_mod(float x, float y)   { return x - y * floor(x / y); }
float2 mc_mod(float2 x, float y)  { return x - y * floor(x / y); }
float3 mc_mod(float3 x, float y)  { return x - y * floor(x / y); }
float4 mc_mod(float4 x, float y)  { return x - y * floor(x / y); }
float2 mc_mod(float2 x, float2 y) { return x - y * floor(x / y); }
float3 mc_mod(float3 x, float3 y) { return x - y * floor(x / y); }
float4 mc_mod(float4 x, float4 y) { return x - y * floor(x / y); }

float2 mc_uv(float2 pixel) { return pixel / mc.resolution; }
float3 mc_role(uint role)  { return mc.palette[role].rgb; }

// White noise, point sampled, wrapping: uv 0..1 spans the 256x256 texels.
float4 mc_noise(float2 uv)        { return mc_noise_tex.SampleLevel(mc_point_wrap, uv, 0); }
float4 mc_noise_smooth(float2 uv) { return mc_noise_tex.SampleLevel(mc_linear_wrap, uv, 0); }

// One sample of the glyph grid; uv 0..1 spans all 16x16 cells and wraps.
float mc_glyph(float2 grid_uv) { return mc_glyph_tex.SampleLevel(mc_linear_wrap, grid_uv, 0); }
// One glyph: `code` is a Latin-1 code point, `inside` runs 0..1 across its cell.
float mc_glyph_cell(uint code, float2 inside) {
    const float2 cell = float2(code & 15u, (code >> 4u) & 15u);
    return mc_glyph(((cell + saturate(inside)) / MC_GLYPH_CELLS));
}

// Blue noise in -0.5/255 .. 0.5/255: add it to a gradient to hide 8-bit banding.
float mc_dither(float2 pixel) {
    return (mc_blue_tex.Load(int3(int2(pixel) & 63, 0)) - 0.5) * (1.0 / 255.0);
}

// A texture-free PCG hash (Jarzynski and Olano, JCGT 2020): four values in [0, 1) from one point.
float4 mc_hash4(float2 p) {
    uint4 v = uint4(asuint(p.x), asuint(p.y), asuint(p.x + p.y), 0x9e3779b9u) * 1664525u + 1013904223u;
    v.x += v.y * v.w;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.w += v.y * v.z;
    v ^= v >> 16u;
    v.x += v.y * v.w;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.w += v.y * v.z;
    return (v >> 8u) * (1.0 / 16777216.0);
}

// Daylight from mc_ambient.day (@uses clock): 0 at night, 1 by day, crossing over an hour centred on the local
// sunrise and sunset hours (e.g. 6.5 and 19.0).
float mc_daylight(float sunrise, float sunset) {
    const float hour = mc_ambient.day * 24.0;
    return smoothstep(sunrise - 0.5, sunrise + 0.5, hour) * (1.0 - smoothstep(sunset - 0.5, sunset + 0.5, hour));
}

// Interface 3 geometry, by vertex ID alone (the host binds no vertex data). grid(W, H) draws H - 1 instances of a
// 2W-vertex triangle strip; this is the vertex's point of the W x H grid, 0..W-1 by 0..H-1.
uint2 mc_grid_point(uint vertex_id, uint instance_id) { return uint2(vertex_id >> 1, instance_id + (vertex_id & 1u)); }
// quads(N) draws N instances of a 4-vertex strip; this is the vertex's corner: (0,0) (1,0) (0,1) (1,1).
float2 mc_quad_corner(uint vertex_id) { return float2(vertex_id & 1u, vertex_id >> 1); }
// A point in the pass's target pixels (top-left origin) as an SV_Position.
float4 mc_clip(float2 pixel) { return float4(pixel / mc_pass.size * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }

#ifndef MC_GRAPH
float4 background(float2 pixel); // the author's function

float4 main(float4 position : SV_Position) : SV_Target {
    const float4 c = background(position.xy);
    return float4(c.rgb * c.a, c.a);
}
#endif
