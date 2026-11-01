// Background effect pass. fx_vertex covers one canvas rectangle for a plugin
// effect; for the built-in Stars and Comets it expands one small sprite per
// comet lane and per star cell (instances of one draw), clipped to the canvas
// and to the star's own cell, so fx_builtin shades only the pixels a star or
// comet can touch. Output is premultiplied and composited as
// dst = src.rgb + dst * (1 - src.a).

struct FxVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0; // canvas uv; the only input a plugin effect reads
    // Built-in sprites: DIP offset from the star center or comet head, and
    // the sprite's constants. Star: (radius, brightness, 1 / edge width, 0).
    // Comet: (axis, fade, 1).
    float2 local : TEXCOORD1;
    nointerpolation float4 shape : TEXCOORD2;
};

#if defined(FX_VERTEX)
cbuffer UiFrame : register(b0) {
    float2 coordinate_scale;
    float2 coordinate_translation;
};

// mc_fx_constants (rows 0..11) plus the host-only rows: the canvas (DIPs) and
// the built-in sprite grid (first star cell, cells per row, comet lanes; all
// zero for a plugin effect). The pixel stage sees it at b0, the vertex stage at b2.
cbuffer McFxVertex : register(b2) {
    float vertex_time : packoffset(c0.x);
    float2 vertex_resolution : packoffset(c1.x);
    float4 vertex_parameters[2] : packoffset(c10);
    float4 vertex_canvas : packoffset(c12);
    float4 vertex_grid : packoffset(c13);
};

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
    output.uv = corner;
    output.local = 0.0f;
    output.shape = 0.0f;
    float2 position;
    if (vertex_grid.z + vertex_grid.w == 0.0f) {
        position = lerp(vertex_canvas.xy, vertex_canvas.zw, corner);
    } else {
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
#else
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
