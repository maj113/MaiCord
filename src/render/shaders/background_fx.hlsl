// Background effect pass. fx_vertex covers one canvas rectangle; fx_builtin
// is MaiCord's Stars and Comets written against the public mc.fx@1 constant
// buffer, exactly as a plugin effect would be. Output is premultiplied and
// composited as dst = src.rgb + dst * (1 - src.a).

#if defined(FX_VERTEX)
cbuffer UiFrame : register(b0) {
    float2 coordinate_scale;
    float2 coordinate_translation;
    float group_opacity;
    float3 frame_reserved;
};

// mc_fx_constants (rows 0..11) plus the host-only canvas row (DIPs). The
// pixel stage sees it at b0, the vertex stage at b2.
cbuffer McFxVertex : register(b2) {
    float4 vertex_canvas : packoffset(c12);
};

struct FxVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

FxVertexOutput fx_vertex(uint vertex_id : SV_VertexID) {
    // Triangle strip: (0,0) (1,0) (0,1) (1,1).
    const float2 corner = float2(vertex_id & 1u, vertex_id >> 1);
    const float2 position = lerp(vertex_canvas.xy, vertex_canvas.zw, corner);
    FxVertexOutput output;
    output.position = float4(position * coordinate_scale + coordinate_translation, 0.0f, 1.0f);
    output.uv = corner;
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

float fx_hash(float2 cell) {
    float3 p = frac(float3(cell.xyx) * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float4 fx_builtin(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    const float speed = fx_parameters[0].x;
    const float density = saturate(fx_parameters[0].y);
    const float strength = saturate(fx_parameters[0].z);
    const uint mask = (uint)fx_parameters[0].w;
    const float2 extent = fx_parameters[1].xy;
    const float time = fx_time * speed;
    // Global DIP coordinate: overlapping canvases share one star field.
    const float2 coordinate = lerp(fx_canvas.xy, fx_canvas.zw, uv);
    float detail = 0.0f;
    if (mask & 1u) {
        const float pitch = lerp(180.0f, 48.0f, density);
        const float2 cell = floor(coordinate / pitch);
        const float seed = fx_hash(cell);
        const float2 center = (cell + float2(0.15f + seed * 0.7f, 0.15f + fx_hash(cell + 13.0f) * 0.7f)) * pitch;
        const float distance = length(coordinate - center);
        const float star = saturate((1.1f + seed * 0.8f - distance) / max(fwidth(distance), 0.5f)) +
                           exp2(-distance * distance * 0.14f) * 0.12f;
        // Each fixed star has its own random-looking pulse schedule. Window
        // seeds alter pulse timing without moving stars or synchronizing them.
        const float clock = time / (2.5f + seed * 5.0f) + seed * 11.0f;
        const float window = floor(clock), progress = frac(clock);
        const float pulse_seed = fx_hash(cell + float2(window * 17.1f, window * 0.7f));
        const float center_time = 0.2f + pulse_seed * 0.6f;
        const float width_time = 0.08f + fx_hash(cell + window + 29.0f) * 0.12f;
        const float offset = (progress - center_time) / width_time;
        const float pulse = exp2(-offset * offset * 4.0f) * smoothstep(0.0f, 0.08f, progress) *
                            (1.0f - smoothstep(0.92f, 1.0f, progress));
        detail += star * (0.07f + pulse * 0.93f);
    }
    if (mask & 2u) {
        // Density admits one to four staggered paths across the window.
        const uint lane_count = clamp((uint)ceil(density * 4.0f), 1u, 4u);
        for (uint lane = 0u; lane < lane_count; ++lane) {
            // A bounded lane gets a new rain origin and slope each time
            // window. Different periods avoid a synchronized repeated train.
            // Fixed lane inputs let fxc fold the seed hashes and reciprocals.
            const float seed = lane == 0u ? fx_hash(float2(37.0f, 19.0f))
                               : lane == 1u ? fx_hash(float2(38.0f, 19.0f))
                               : lane == 2u ? fx_hash(float2(39.0f, 19.0f))
                                            : fx_hash(float2(40.0f, 19.0f));
            const float rate = lane == 0u   ? 1.0f / (7.0f + fx_hash(float2(37.0f, 19.0f)) * 8.0f)
                               : lane == 1u ? 1.0f / (7.0f + fx_hash(float2(38.0f, 19.0f)) * 8.0f)
                               : lane == 2u ? 1.0f / (7.0f + fx_hash(float2(39.0f, 19.0f)) * 8.0f)
                                            : 1.0f / (7.0f + fx_hash(float2(40.0f, 19.0f)) * 8.0f);
            const float clock = time * rate + 0.16f + seed * 0.45f;
            const float window = floor(clock), cycle = frac(clock);
            const float event_seed = fx_hash(float2(lane + 71.0f, window + 53.0f));
            const float slope = (event_seed < 0.5f ? -1.0f : 1.0f) *
                                (0.18f + fx_hash(float2(lane + 17.0f, window + 91.0f)) * 0.24f);
            const float2 path = float2((extent.y + 96.0f) * slope, extent.y + 96.0f);
            const float2 axis = normalize(path);
            const float origin_x = extent.x * (0.225f + event_seed * 0.55f) - path.x * 0.5f;
            const float2 center = float2(origin_x, -48.0f) + path * cycle;
            const float2 delta = coordinate - center;
            const float along = dot(delta, axis), across = dot(delta, float2(-axis.y, axis.x));
            const float tail = saturate(1.0f + along / 36.0f) * step(along, 0.0f) * exp2(-across * across * 0.7f);
            const float head = exp2(-dot(delta, delta) * 0.5f);
            const float fade = smoothstep(0.03f, 0.13f, cycle) * (1.0f - smoothstep(0.82f, 0.96f, cycle));
            detail += (tail * 0.8f + head) * fade;
        }
    }
    // Ink follows the foreground mode: additive light ink on a dark theme,
    // a lerp toward dark ink on a light one.
    const float amount = min(saturate(detail) * strength * 0.38f, 0.42f);
    if (fx_dark > 0.5f)
        return float4(float3(0.82f, 0.9f, 1.0f) * amount, 0.0f);
    return float4(float3(0.1f, 0.18f, 0.3f) * amount, amount);
}
#endif
