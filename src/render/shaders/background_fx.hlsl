// Background effect pass.
//
// Built-in Stars and Comets: fx_vertex expands one small sprite per comet lane
// and per star cell (instances of one draw), clipped to the canvas and to the
// star's own cell, so fx_builtin shades only the pixels a star or comet can
// touch. Output is premultiplied and composited as dst = src.rgb + dst * (1 - src.a).
//
// Package effects: fx_target_vertex covers the whole effect target for the
// package's pixel shader (maicord/sdk/background.hlsli). Each background
// canvas then composites that target: fx_composite_vertex places the canvas
// rectangle and maps it into the target, fx_composite_pixel samples it.

#if defined(FX_VERTEX)
struct FxVertexOutput {
    float4 position : SV_Position;
    // DIP offset from the star center or comet head, and the sprite's
    // constants. Star: (radius, brightness, 1 / edge width, 0). Comet: (axis, fade, 1).
    float2 local : TEXCOORD0;
    nointerpolation float4 shape : TEXCOORD1;
};

cbuffer UiFrame : register(b0) {
    float2 coordinate_scale;
    float2 coordinate_translation;
};

// BackgroundFxConstants (rows 0..11) plus the host-only rows: the canvas (DIPs)
// and the sprite grid (first star cell, cells per row, comet lanes). The pixel
// stage sees it at b0, the vertex stage at b2.
#if defined(__spirv__)
// The same offsets without packoffset: std140 admits no gaps, so the members the layout skips are named and each
// member starts where std140 places it (scalars, never a float3, fill the registers the host struct leaves open).
cbuffer McFxVertex : register(b2) {
    float vertex_time;
    float vertex_pad0_unused_, vertex_pad1_unused_, vertex_pad2_unused_;
    float2 vertex_resolution;
    float2 vertex_pad3_unused_;
    float4 vertex_gap_unused_[8];
    float4 vertex_parameters[2];
    float4 vertex_canvas;
    float4 vertex_grid;
};
#else
cbuffer McFxVertex : register(b2) {
    float vertex_time : packoffset(c0.x);
    float2 vertex_resolution : packoffset(c1.x);
    float4 vertex_parameters[2] : packoffset(c10);
    float4 vertex_canvas : packoffset(c12);
    float4 vertex_grid : packoffset(c13);
};
#endif

// PCG3D (Jarzynski and Olano, JCGT 2020): three decorrelated values in [0, 1)
// from three integers. Pure 32-bit integer arithmetic: the same stars on every GPU.
float3 fx_hash(uint3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return (v >> 8u) * (1.0f / 16777216.0f);
}

// Bounds beyond which neither a star's glow nor a comet's trail reaches 0.5 of
// an 8-bit level at full strength.
static const float kStarReach = 6.0f;
static const float kCometTail = 36.0f, kCometHead = 5.0f, kCometWidth = 4.0f;

FxVertexOutput fx_vertex(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    // Triangle strip: (0,0) (1,0) (0,1) (1,1).
    const float2 corner = float2(vertex_id & 1u, vertex_id >> 1);
    FxVertexOutput output;
    float2 position;
    {
        const float speed = vertex_parameters[0].x;
        const float density = saturate(vertex_parameters[0].y);
        const float time = vertex_time * speed;
        // Comet lanes come first, then one star per grid cell. Either hashes
        // its key to seed a clock whose window then selects the second hash.
        const bool comet = instance_id < (uint)vertex_grid.w;
        const uint columns = max((uint)vertex_grid.z, 1u);
        const uint index = instance_id - (uint)vertex_grid.w;
        const int2 cell_index = int2((int)vertex_grid.x + (int)(index % columns),
                                     (int)vertex_grid.y + (int)(index / columns));
        const uint2 key = comet ? uint2(instance_id, 0x7fffffffu) : uint2(cell_index);
        const float3 base = fx_hash(uint3(key, 0u));
        // Per-lane periods differ so comets do not fire in lockstep.
        const float clock = comet ? time / (7.0f + base.x * 8.0f) + 0.16f + base.x * 0.45f
                                  : time / (2.5f + base.x * 5.0f) + base.x * 11.0f;
        const float window = floor(clock), progress = frac(clock);
        const float2 event = fx_hash(uint3(key, (uint)window + 1u)).xy;
        float2 low, high, anchor;
        if (comet) {
            // Density admits one to four staggered paths across the window.
            // Each time window draws a new comet origin and slope.
            const float slope = (event.x < 0.5f ? -1.0f : 1.0f) * (0.18f + event.y * 0.24f);
            const float2 extent = vertex_parameters[1].xy;
            const float2 path = float2((extent.y + 96.0f) * slope, extent.y + 96.0f);
            const float2 axis = normalize(path);
            anchor = float2(extent.x * (0.225f + event.x * 0.55f) - path.x * 0.5f, -48.0f) + path * progress;
            const float fade = smoothstep(0.03f, 0.13f, progress) * (1.0f - smoothstep(0.82f, 0.96f, progress));
            const float2 tail_end = anchor - axis * kCometTail, head_end = anchor + axis * kCometHead;
            const float2 reach = float2(abs(axis.y), abs(axis.x)) * kCometWidth;
            low = min(tail_end, head_end) - reach;
            high = fade > 0.0f ? max(tail_end, head_end) + reach : low;
            output.shape = float4(axis, fade, 1.0f);
        } else {
            // One fixed star per grid cell, with its own random-looking pulse
            // schedule; window hashes alter pulse timing without moving stars.
            const float pitch = lerp(180.0f, 48.0f, density);
            const float2 cell = float2(cell_index);
            anchor = (cell + (0.15f + base.yz * 0.7f)) * pitch;
            const float offset = (progress - (0.2f + event.x * 0.6f)) / (0.08f + event.y * 0.12f);
            const float pulse = exp2(-offset * offset * 4.0f) * smoothstep(0.0f, 0.08f, progress) *
                                (1.0f - smoothstep(0.92f, 1.0f, progress));
            // Edge ramp: one device pixel (about 1.2 DIPs across a diagonal), at least half a DIP.
            const float pixels_per_dip = vertex_resolution.x / (vertex_canvas.z - vertex_canvas.x);
            low = max(anchor - kStarReach, cell * pitch);
            high = min(anchor + kStarReach, (cell + 1.0f) * pitch);
            output.shape = float4(1.1f + base.x * 0.8f, 0.07f + pulse * 0.93f, min(pixels_per_dip * (1.0f / 1.2f), 2.0f), 0.0f);
        }
        low = max(low, vertex_canvas.xy);
        high = max(min(high, vertex_canvas.zw), low);
        position = lerp(low, high, corner);
        output.local = position - anchor;
    }
    output.position = float4(position * coordinate_scale + coordinate_translation, 0.0f, 1.0f);
    return output;
}
#elif defined(FX_TARGET)
// The whole effect target as a triangle strip: (0,0) (1,0) (0,1) (1,1), top-left origin.
float4 fx_target_vertex(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 corner = float2(vertex_id & 1u, vertex_id >> 1);
    return float4(corner.x * 2.0f - 1.0f, 1.0f - corner.y * 2.0f, 0.0f, 1.0f);
}
#elif defined(FX_COMPOSITE_VERTEX)
cbuffer UiFrame : register(b0) {
    float2 coordinate_scale;
    float2 coordinate_translation;
};

// One canvas in DIPs, and the map from a DIP to a position in the effect
// target: uv = dip * scale + offset.
cbuffer McFxComposite : register(b2) {
    float4 composite_canvas : packoffset(c0);
    float4 composite_uv : packoffset(c1);
};

struct FxCompositeOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 dip : TEXCOORD1;
};

FxCompositeOutput fx_composite_vertex(uint vertex_id : SV_VertexID) {
    const float2 corner = float2(vertex_id & 1u, vertex_id >> 1);
    const float2 position = lerp(composite_canvas.xy, composite_canvas.zw, corner);
    FxCompositeOutput output;
    output.position = float4(position * coordinate_scale + coordinate_translation, 0.0f, 1.0f);
    output.uv = position * composite_uv.xy + composite_uv.zw;
    output.dip = position;
    return output;
}
#elif defined(FX_COMPOSITE_PIXEL)
cbuffer McFxCompositePixel : register(b0) {
    float4 composite_skip[2] : packoffset(c0);
    float4 composite_clamp : packoffset(c2); // the used part of the target, inset half a texel
    float composite_opacity : packoffset(c3.x);
    float composite_radius : packoffset(c3.y); // a tile's corner radius in DIPs; 0 fills the canvas rectangle
    float composite_light : packoffset(c3.z);  // the light adaptation's amount (effect_light_composite)
    float composite_layer : packoffset(c3.w);  // the paint layer's opacity (the blend factor)
    float composite_blend : packoffset(c4.x);  // EffectBlend: 0 normal, 1 screen, 2 add
};
Texture2D<float4> fx_target : register(t0);
SamplerState fx_sampler : register(s0); // the pipeline's bilinear clamp sampler

// The colour, and the weight of what is behind it: only the adapted screen and add's blend state reads the weight.
struct FxCompositeTargets {
#if defined(__spirv__)
    // Dual-source blending (GL_EXT_blend_func_extended): both outputs are colour 0, the second at index 1.
    [[vk::location(0), vk::index(0)]] float4 color : SV_Target0;
    [[vk::location(0), vk::index(1)]] float4 weight : SV_Target1;
#else
    float4 color : SV_Target0;
    float4 weight : SV_Target1;
#endif
};

// The target holds premultiplied colour; the user's opacity scales all four channels.
#if defined(MC_COMPOSITE_COLOR) || defined(MC_COMPOSITE_WEIGHT)
#define FX_COMPOSITE_BOTH fx_composite_both
#else
#define FX_COMPOSITE_BOTH fx_composite_pixel
#endif
FxCompositeTargets FX_COMPOSITE_BOTH(float4 position : SV_Position, float2 uv : TEXCOORD0, float2 dip : TEXCOORD1) {
    float4 color = fx_target.SampleLevel(fx_sampler, clamp(uv, composite_clamp.xy, composite_clamp.zw), 0.0f) *
                   composite_opacity;
    // A tile is a rounded rectangle: coverage from the signed distance to its outline, one pixel wide.
    [branch] if (composite_radius > 0.0f) {
        const float2 half_size = (composite_skip[0].zw - composite_skip[0].xy) * 0.5f;
        const float2 q = abs(dip - (composite_skip[0].xy + half_size)) - (half_size - composite_radius);
        const float edge = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - composite_radius;
        color *= saturate(0.5f - edge / max(fwidth(edge), 0.0001f));
    }
    FxCompositeTargets output;
    output.color = color;
    output.weight = 1.0f;
    // A light appearance: the picture's BT.709 luma flips (y -> 1 - y) with its chroma kept. Normal flips the
    // premultiplied colour (by alpha - 2y); screen and add multiply what is behind by the flipped light instead of
    // adding it. composite_light cross-fades from the plain composite.
    [branch] if (composite_light > 0.0f) {
        color = saturate(color);
        const float luma = dot(color.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        if (composite_blend == 0.0f) {
            output.color.rgb = lerp(color.rgb, clamp(color.rgb + (color.a - 2.0f * luma), 0.0f, color.a), composite_light);
        } else {
            const float3 multiply = 1.0f - composite_layer * (1.0f - saturate(1.0f + color.rgb - 2.0f * luma));
            const float3 plain = composite_blend == 1.0f ? 1.0f - color.rgb : 1.0f;
            output.weight.rgb = lerp(plain, multiply, composite_light);
            // Settled, nothing is added: a plain multiply blend takes the weight as the colour.
            output.color.rgb = composite_light < 1.0f ? color.rgb * (1.0f - composite_light) : output.weight.rgb;
        }
    }
    return output;
}
#if defined(MC_COMPOSITE_COLOR) || defined(MC_COMPOSITE_WEIGHT)
// The OpenGL ES backend, on a device without dual-source blending, draws the composite twice: the weight as the only
// output (blended as dst * weight), then the colour (added at the blend factor).
float4 fx_composite_pixel(float4 position : SV_Position, float2 uv : TEXCOORD0, float2 dip : TEXCOORD1) : SV_Target0 {
    const FxCompositeTargets both = fx_composite_both(position, uv, dip);
#if defined(MC_COMPOSITE_WEIGHT)
    return both.weight;
#else
    return both.color;
#endif
}
#endif
#else
struct FxVertexOutput {
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
    nointerpolation float4 shape : TEXCOORD1;
};

cbuffer McFx : register(b0) {
    float fx_time;
    float fx_delta;
    float fx_intensity;
    float fx_dark;
    float2 fx_resolution;
    float2 fx_origin;
    float4 fx_palette[8];
    float4 fx_parameters[2];
    float4 fx_canvas;
};

float4 fx_builtin(FxVertexOutput input) : SV_Target {
    const float squared = dot(input.local, input.local);
    float detail;
    [branch] if (input.shape.w == 0.0f) {
        // Core with a one-pixel ramp, plus a compact glow, (1 - d^2 / 64)^6,
        // standing in for exp2(-0.14 d^2) (within half a level at full strength).
        const float falloff = saturate(1.0f - squared * (1.0f / 64.0f));
        const float cubed = falloff * falloff * falloff;
        detail = (saturate((input.shape.x - sqrt(squared)) * input.shape.z) + cubed * cubed * 0.12f) * input.shape.y;
    } else {
        const float along = dot(input.local, input.shape.xy);
        const float across = dot(input.local, float2(-input.shape.y, input.shape.x));
        const float tail = saturate(1.0f + along * (1.0f / 36.0f)) * step(along, 0.0f) * exp2(-across * across * 0.7f);
        detail = (tail * 0.8f + exp2(-squared * 0.5f)) * input.shape.z;
    }
    // Ink follows the foreground mode: additive light ink on a dark theme,
    // a lerp toward dark ink on a light one.
    const float amount = saturate(detail) * saturate(fx_parameters[0].z) * 0.38f;
    if (fx_dark > 0.5f)
        return float4(float3(0.82f, 0.9f, 1.0f) * amount, 0.0f);
    return float4(float3(0.1f, 0.18f, 0.3f) * amount, amount);
}
#endif
