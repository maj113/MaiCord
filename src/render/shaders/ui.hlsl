cbuffer UiFrame : register(b0) {
    float2 coordinate_scale;
    float2 coordinate_translation;
    float group_opacity;
    float3 frame_reserved;
};

cbuffer UiBackground : register(b1) {
    float4 background_colors[5];
    float4 background_controls;
    float4 background_motion;
};

// R8 coverage written by the icon atlas compute rasterizer.
Texture2D<float> icon_atlas : register(t0);

struct VertexInput {
    float4 rectangle : RECT;
    float4 fill : COLOR0;
    float4 border : COLOR1;
    float4 parameters : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    nointerpolation float4 fill : COLOR0;
    nointerpolation float4 border : COLOR1;
    float4 local_and_half_size : TEXCOORD0;
    nointerpolation float4 parameters : TEXCOORD1;
    nointerpolation float primitive_opacity : TEXCOORD2;
    float2 dip_position : TEXCOORD3;
};

static const float2 kQuadCorners[6] = {
    float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(0.0f, 1.0f),
    float2(0.0f, 1.0f), float2(1.0f, 0.0f), float2(1.0f, 1.0f),
};

VertexOutput ui_vertex(VertexInput input, uint vertex_id : SV_VertexID) {
    VertexOutput output;
    const float2 corner = kQuadCorners[vertex_id];
    const float2 position = lerp(input.rectangle.xy, input.rectangle.zw, corner);
    const float2 half_size = (input.rectangle.zw - input.rectangle.xy) * 0.5f;
    output.position = float4(position * coordinate_scale + coordinate_translation, 0.0f, 1.0f);
    output.dip_position = position;
    output.fill = input.fill;
    output.border = input.border;
    output.primitive_opacity = group_opacity;
    if (input.parameters.y != -6.0f) {
        output.fill.a *= group_opacity;
        output.border.a *= group_opacity;
    }
    output.local_and_half_size = float4(lerp(-half_size, half_size, corner), half_size);
    output.parameters = input.parameters;
    return output;
}

float background_hash(float2 cell) {
    float3 p = frac(float3(cell.xyx) * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float3 background_gradient(float2 local, float2 half_size) {
    if (background_controls.z <= 1.0f)
        return background_colors[0].rgb;
    const float2 direction = background_controls.xy;
    const float projection = max(dot(half_size, abs(direction)) * 2.0f, 0.0001f);
    const float position = saturate(0.5f + dot(local, direction) / projection);
    const float stops = clamp(background_controls.z, 2.0f, 5.0f);
    const float selected = position * (stops - 1.0f);
    const uint first = min((uint)selected, (uint)stops - 2u);
    return lerp(background_colors[first].rgb, background_colors[first + 1u].rgb, selected - first);
}

float3 background_dither(float3 color, float2 pixel_position) {
    // Fixed screen-space noise, no phase term or extra texture. At most half
    // an output byte before composition, never animated sparkling.
    return saturate(color + (background_hash(floor(pixel_position)) - 0.5f) / 255.0f);
}

float rounded_distance(float2 local, float2 half_size, float radius) {
    const float2 corner_distance = abs(local) - (half_size - radius);
    return length(max(corner_distance, 0.0f)) + min(max(corner_distance.x, corner_distance.y), 0.0f) - radius;
}

// Premultiplied fill inside the border, then the border ring, antialiased.
float4 surface_pixel(float4 fill, float4 border, float signed_distance, float border_width,
                     float antialias_width, float outer_coverage) {
    const float fill_coverage = saturate(0.5f - (signed_distance + border_width) / antialias_width);
    const float border_coverage = outer_coverage - fill_coverage;
    return float4(fill.rgb * fill.a, fill.a) * fill_coverage +
           float4(border.rgb * border.a, border.a) * border_coverage;
}

float4 ui_pixel(VertexOutput input) : SV_Target0 {
    const float2 local = input.local_and_half_size.xy;
    const float2 half_size = input.local_and_half_size.zw;
    // A non-negative y is an ordinary rounded rectangle's border width: most
    // of every frame's pixels skip the special kinds (negative y) below.
    if (input.parameters.y >= 0.0f) {
        const float radius = min(input.parameters.x, min(half_size.x, half_size.y));
        const float border_width = min(input.parameters.y, min(half_size.x, half_size.y));
        const float signed_distance = rounded_distance(local, half_size, radius);
        const float antialias_width = max(fwidth(signed_distance), 0.0001f);
        const float outer_coverage = saturate(0.5f - signed_distance / antialias_width);
        return surface_pixel(input.fill, input.border, signed_distance, border_width, antialias_width,
                             outer_coverage);
    }
    if (input.parameters.y == -3.0f) {
        return float4(background_dither(background_gradient(local, half_size), input.position.xy) *
                          input.fill.a,
                      input.fill.a);
    }
    if (input.parameters.y == -9.0f) {
        // Pixel-snapped icon, read 1:1 from its atlas cell. A negative origin
        // marks an icon the atlas could not resolve this frame.
        if (input.parameters.z < 0.0f)
            discard;
        const float2 extent = floor(half_size * 2.0f * input.parameters.x + 0.5f);
        const float2 texel = clamp(floor((local + half_size) * input.parameters.x), 0.0f, extent - 1.0f);
        const float coverage = icon_atlas.Load(int3(input.parameters.zw + texel, 0));
        return float4(input.fill.rgb * input.fill.a, input.fill.a) * coverage;
    }
    if (input.parameters.y == -1.0f) {
        const float2 a = -input.parameters.zw, segment = input.parameters.zw * 2.0f;
        const float t = saturate(dot(local - a, segment) / max(dot(segment, segment), 0.00001f));
        const float distance = length(local - a - t * segment) - input.parameters.x;
        const float coverage = saturate(0.5f - distance / max(fwidth(distance), 0.0001f));
        return float4(input.fill.rgb * input.fill.a, input.fill.a) * coverage;
    }
    const float radius = min(input.parameters.x, min(half_size.x, half_size.y));
    const float border_width = min(input.parameters.y == -7.0f   ? abs(input.parameters.z)
                                   : input.parameters.y == -8.0f ? 0.0f
                                                                 : input.parameters.y,
                                   min(half_size.x, half_size.y));
    const float signed_distance = rounded_distance(local, half_size, radius);
    const float antialias_width = max(fwidth(signed_distance), 0.0001f);
    const float outer_coverage = saturate(0.5f - signed_distance / antialias_width);
    if (input.parameters.y == -4.0f) {
        const float2 uv = saturate((local + half_size) / max(half_size * 2.0f, 0.0001f));
        const float3 color = lerp(float3(1, 1, 1), input.fill.rgb, uv.x) * (1.0f - uv.y);
        const float alpha = input.fill.a * outer_coverage;
        return float4(color * alpha, alpha);
    }
    if (input.parameters.y == -5.0f) {
        const float u = saturate((local.x + half_size.x) / max(half_size.x * 2.0f, 0.0001f));
        const float3 color =
            saturate(abs(frac(u + float3(0, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f);
        const float alpha = input.fill.a * outer_coverage;
        return float4(color * alpha, alpha);
    }
    if (input.parameters.y == -6.0f) {
        const float2 direction = input.parameters.zw;
        const float position =
            saturate(0.5f + dot(local, direction) / max(dot(half_size, abs(direction)) * 2.0f, 0.0001f));
        if (position < input.fill.a || (position >= input.border.a && input.border.a < 1.0f))
            discard;
        const float3 color =
            lerp(input.fill.rgb, input.border.rgb,
                 saturate((position - input.fill.a) / max(input.border.a - input.fill.a, 0.0001f)));
        const float alpha = input.primitive_opacity * outer_coverage;
        return float4(color * alpha, alpha);
    }
    if (input.parameters.y == -2.0f) {
        const float4 gradient = lerp(input.fill, input.border,
                                     saturate((local.y + half_size.y) / max(half_size.y * 2.0f, 0.0001f)));
        return float4(gradient.rgb * gradient.a, gradient.a) * outer_coverage;
    }
    float4 surface_fill = input.fill;
    float4 surface_border = input.border;
    if (input.parameters.y == -8.0f) {
        const float2 global_half_size = abs(1.0f / coordinate_scale);
        if (background_controls.z >= 1.0f) {
            const float3 gradient =
                background_gradient(input.dip_position - global_half_size, global_half_size);
            const float mode = background_colors[1].w > 0.5f ? 1.0f : 0.0f;
            surface_fill.rgb = lerp(mode.xxx, gradient, saturate(input.parameters.w));
        }
        // Stars, Comets and plugin effects draw in their own pass over the
        // finished canvas (background_fx.hlsl).
        surface_fill.rgb = background_dither(surface_fill.rgb, input.position.xy);
    }
    if (input.parameters.y == -7.0f) {
        const float2 global_half_size = abs(1.0f / coordinate_scale);
        const float3 gradient =
            background_gradient(input.dip_position - global_half_size, global_half_size);
        const bool light = background_colors[1].w > 0.5f;
        const float3 mode_color = light ? float3(1, 1, 1) : float3(0, 0, 0);
        surface_fill.rgb =
            background_dither(lerp(mode_color, gradient, saturate(input.parameters.w)), input.position.xy);
        if (input.parameters.z < 0.0f) {
            const float3 edge_tone = light ? float3(0, 0, 0) : float3(1, 1, 1);
            surface_border.rgb = lerp(surface_fill.rgb, edge_tone, 0.12f);
        }
    }
    return surface_pixel(surface_fill, surface_border, signed_distance, border_width, antialias_width,
                         outer_coverage);
}
