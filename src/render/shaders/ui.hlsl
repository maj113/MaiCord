cbuffer UiFrame : register(b0) {
    float2 coordinate_scale;
    float2 coordinate_translation;
    float group_opacity;
    // The global backdrop gradient's position along its axis for a point in
    // this pass's DIPs: saturate(dot(point, xy) + z). The axis is the palette
    // direction scaled by the backdrop surface (window caption and content)
    // that the gradient spans.
#if defined(__spirv__)
    // std140 aligns a float3 to 16 bytes, but the host packs it HLSL-style at offset 20.
    float backdrop_axis_x, backdrop_axis_y, backdrop_axis_z;
#else
    float3 backdrop_gradient_axis;
#endif
};

// D3D11BackgroundConstants: stop colours (colors[1].w flags a light theme),
// then direction XY and stop count (at most five).
cbuffer UiBackground : register(b1) {
    float4 background_colors[5];
    float4 background_controls;
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
    // Per-instance colours. Ordinary rectangles (parameters.y >= 0) arrive
    // premultiplied; every other kind keeps straight colour (see ui_vertex).
    nointerpolation float4 fill : COLOR0;
    nointerpolation float4 border : COLOR1;
    // xy: offset from the rectangle centre, zw: position in DIPs.
    float4 local_and_dip : TEXCOORD0;
    nointerpolation float4 parameters : TEXCOORD1;
    nointerpolation float2 half_size : TEXCOORD2;
};

static const float2 kQuadCorners[4] = {
    float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(0.0f, 1.0f), float2(1.0f, 1.0f),
};

VertexOutput ui_vertex(VertexInput input, uint vertex_id : SV_VertexID) {
    VertexOutput output;
    const float2 corner = kQuadCorners[vertex_id];
    const float2 position = lerp(input.rectangle.xy, input.rectangle.zw, corner);
    const float2 half_size = (input.rectangle.zw - input.rectangle.xy) * 0.5f;
    output.position = float4(position * coordinate_scale + coordinate_translation, 0.0f, 1.0f);
    const float kind = input.parameters.y;
    // Kind -6 stores gradient stop positions in both alphas.
    const float opacity = kind != -6.0f ? group_opacity : 1.0f;
    const float2 alpha = float2(input.fill.a, input.border.a) * opacity;
    // Ordinary rectangles (kind >= 0) arrive premultiplied: the pixel shader
    // would otherwise repeat these multiplications on every pixel.
    const float2 premultiply = kind >= 0.0f ? alpha : 1.0f;
    output.fill = float4(input.fill.rgb * premultiply.x, alpha.x);
    output.border = float4(input.border.rgb * premultiply.y, alpha.y);
    output.local_and_dip = float4(lerp(-half_size, half_size, corner), position);
    output.parameters = input.parameters;
    output.half_size = half_size;
    return output;
}

float background_hash(float2 cell) {
    float3 p = frac(float3(cell.xyx) * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

// One gradient lookup: position in [0, 1] along the axis, already clamped.
float3 background_gradient(float position) {
    if (background_controls.z <= 1.0f)
        return background_colors[0].rgb;
    const float selected = position * (background_controls.z - 1.0f);
    const uint first = min((uint)selected, (uint)background_controls.z - 2u);
    return lerp(background_colors[first].rgb, background_colors[first + 1u].rgb, selected - first);
}

// The global gradient spans the whole backdrop surface, so it continues from
// the window caption into the content.
float3 backdrop_gradient(float2 dip_position) {
#if defined(__spirv__)
    return background_gradient(
        saturate(dot(dip_position, float2(backdrop_axis_x, backdrop_axis_y)) + backdrop_axis_z));
#else
    return background_gradient(saturate(dot(dip_position, backdrop_gradient_axis.xy) + backdrop_gradient_axis.z));
#endif
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

// Fill inside the border, then the border ring, antialiased. Both colours are
// premultiplied.
float4 surface_pixel(float4 fill, float4 border, float signed_distance, float border_width,
                     float inverse_antialias_width, float outer_coverage) {
    const float fill_coverage = saturate(0.5f - (signed_distance + border_width) * inverse_antialias_width);
    return fill * fill_coverage + border * (outer_coverage - fill_coverage);
}

float4 ui_pixel(VertexOutput input) : SV_Target0 {
    const float2 local = input.local_and_dip.xy;
    const float2 dip_position = input.local_and_dip.zw;
    const float2 half_size = input.half_size;
    // A non-negative y is an ordinary rounded rectangle's border width: most
    // of every frame's pixels skip the special kinds (negative y) below.
    if (input.parameters.y >= 0.0f) {
        // PaintStream already clamps both values to the rectangle's half extent.
        const float signed_distance = rounded_distance(local, half_size, input.parameters.x);
        // The antialiasing width is one pixel in DIPs. The distance field has unit
        // gradient except at a sharp corner's kink, where its own derivative
        // would double and leave the corner pixel partly transparent.
        const float inverse_antialias_width = 1.0f / max(fwidth(local.x), 0.0001f);
        const float outer_coverage = saturate(0.5f - signed_distance * inverse_antialias_width);
        return surface_pixel(input.fill, input.border, signed_distance, input.parameters.y,
                             inverse_antialias_width, outer_coverage);
    }
    // Every paint producer emits only the kinds tested below (PaintStream and
    // the surface, icon and gradient helpers); the last one, -2, needs no test.
    // Kinds are ordered by the pixels they cover: the canvas shapes nothing,
    // and the surfaces before it share the rounded rectangle below.
    if (input.parameters.y == -3.0f) {
        return float4(background_dither(backdrop_gradient(dip_position), input.position.xy) * input.fill.a,
                      input.fill.a);
    }
    // The rounded rectangle every remaining kind but the icon, line and edge
    // strip shapes itself with.
    const float radius = min(input.parameters.x, min(half_size.x, half_size.y));
    const float signed_distance = rounded_distance(local, half_size, radius);
    const float antialias_width = max(fwidth(local.x), 0.0001f);
    if (input.parameters.y == -7.0f || input.parameters.y == -8.0f) {
        // Panels (-7) always tint toward the backdrop; plain surfaces (-8) only
        // while a backdrop gradient is active. Stars, Comets and plugin effects
        // draw in their own pass over the finished canvas (background_fx.hlsl).
        const bool panel = input.parameters.y == -7.0f;
        const bool light = background_colors[1].w > 0.5f;
        float3 fill = input.fill.rgb;
        if (panel || background_controls.z >= 1.0f) {
            const float3 mode_color = light ? float3(1, 1, 1) : float3(0, 0, 0);
            fill = lerp(mode_color, backdrop_gradient(dip_position), saturate(input.parameters.w));
        }
        fill = background_dither(fill, input.position.xy);
        float3 border = input.border.rgb;
        if (panel && input.parameters.z < 0.0f) {
            const float3 edge_tone = light ? float3(0, 0, 0) : float3(1, 1, 1);
            border = lerp(fill, edge_tone, 0.12f);
        }
        const float inverse_antialias_width = 1.0f / antialias_width;
        return surface_pixel(float4(fill * input.fill.a, input.fill.a),
                             float4(border * input.border.a, input.border.a), signed_distance,
                             min(panel ? abs(input.parameters.z) : 0.0f, min(half_size.x, half_size.y)),
                             inverse_antialias_width, saturate(0.5f - signed_distance * inverse_antialias_width));
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
    if (input.parameters.y == -10.0f) {
        // The leading strip of a rounded rectangle parameters.z wide (an
        // accent border): the shape extends past the quad's right edge.
        const float2 shape_half = float2(input.parameters.z * 0.5f, half_size.y);
        const float distance =
            rounded_distance(float2(local.x + half_size.x - shape_half.x, local.y), shape_half,
                             min(input.parameters.x, min(shape_half.x, shape_half.y)));
        const float coverage = saturate(0.5f - distance / max(fwidth(local.x), 0.0001f));
        return float4(input.fill.rgb * input.fill.a, input.fill.a) * coverage;
    }
    if (input.parameters.y == -4.0f) {
        const float2 uv = saturate((local + half_size) / max(half_size * 2.0f, 0.0001f));
        const float3 color = lerp(float3(1, 1, 1), input.fill.rgb, uv.x) * (1.0f - uv.y);
        const float alpha = input.fill.a * saturate(0.5f - signed_distance / antialias_width);
        return float4(color * alpha, alpha);
    }
    if (input.parameters.y == -5.0f) {
        const float u = saturate((local.x + half_size.x) / max(half_size.x * 2.0f, 0.0001f));
        const float3 color =
            saturate(abs(frac(u + float3(0, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f);
        const float alpha = input.fill.a * saturate(0.5f - signed_distance / antialias_width);
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
        const float alpha = group_opacity * saturate(0.5f - signed_distance / antialias_width);
        return float4(color * alpha, alpha);
    }
    // -2: vertical two-colour gradient.
    const float4 gradient =
        lerp(input.fill, input.border, saturate((local.y + half_size.y) / max(half_size.y * 2.0f, 0.0001f)));
    return float4(gradient.rgb * gradient.a, gradient.a) * saturate(0.5f - signed_distance / antialias_width);
}
