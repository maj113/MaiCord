// twin: src/render/shaders/ui.hlsl b21393632491dca9
// Rectangles, borders, gradients, lines and icons of the UI pass: ui_vertex (STAGE_VS) and ui_pixel (STAGE_PS).

// xy: offset from the rectangle centre, zw: position in DIPs; parameters: x radius, y kind, zw per kind.
layout(location = 0) flat V2F vec4 v_fill;
layout(location = 1) flat V2F vec4 v_border;
layout(location = 2) V2F vec4 v_local_dip;
layout(location = 3) flat V2F vec4 v_params;
layout(location = 4) flat V2F vec2 v_half;

#if defined(STAGE_VS)

// Blocks both stages declare carry an instance name: a program holds one global name per member.
layout(std140, binding = 0) uniform UiFrameVs {
    vec2 coordinate_scale;
    vec2 coordinate_translation;
    float group_opacity;
} frame;

layout(location = 0) in vec4 rect;
layout(location = 1) in vec4 fill;
layout(location = 2) in vec4 border;
layout(location = 3) in vec4 params;

void main() {
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    vec2 position = mix(rect.xy, rect.zw, corner);
    vec2 half_size = (rect.zw - rect.xy) * 0.5;
    float kind = params.y;
    // Kind -6 stores gradient stop positions in both alphas.
    vec2 alpha = vec2(fill.a, border.a) * (kind != -6.0 ? frame.group_opacity : 1.0);
    // Ordinary rectangles (kind >= 0) arrive premultiplied.
    vec2 premultiply = kind >= 0.0 ? alpha : vec2(1.0);
    vec2 clip = position * frame.coordinate_scale + frame.coordinate_translation;
    gl_Position = vec4(clip.x, -clip.y, -1.0, 1.0);
    v_fill = vec4(fill.rgb * premultiply.x, alpha.x);
    v_border = vec4(border.rgb * premultiply.y, alpha.y);
    v_local_dip = vec4(mix(-half_size, half_size, corner), position);
    v_params = params;
    v_half = half_size;
}

#else

// The first three floats after group_opacity are the host's float3 backdrop axis.
layout(std140, binding = 8) uniform UiFramePs {
    vec4 frame_unused_;
    float group_opacity;
    float backdrop_axis_x, backdrop_axis_y, backdrop_axis_z;
} frame;

// Stop colours (background_colors[1].w flags a light theme), then direction XY and stop count (at most five).
layout(std140, binding = 9) uniform UiBackground {
    vec4 background_colors[5];
    vec4 background_controls;
};

layout(binding = 0) uniform highp sampler2D icon_atlas; //! t0
layout(location = 0) out vec4 out_color;

float background_hash(vec2 cell) {
    vec3 p = fract(cell.xyx * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// One gradient lookup: position in [0, 1] along the axis, already clamped.
vec3 background_gradient(float position) {
    if (background_controls.z <= 1.0)
        return background_colors[0].rgb;
    float selected = position * (background_controls.z - 1.0);
    uint first = min(uint(selected), uint(background_controls.z) - 2u);
    return mix(background_colors[first].rgb, background_colors[first + 1u].rgb, selected - float(first));
}

vec3 backdrop_gradient(vec2 dip_position) {
    return background_gradient(clamp(dot(dip_position, vec2(frame.backdrop_axis_x, frame.backdrop_axis_y)) + frame.backdrop_axis_z, 0.0, 1.0));
}

// Fixed screen-space noise of at most half an output byte.
vec3 background_dither(vec3 color, vec2 pixel) {
    return clamp(color + (background_hash(floor(pixel)) - 0.5) * (1.0 / 255.0), 0.0, 1.0);
}

float rounded_distance(vec2 local, vec2 half_size, float radius) {
    vec2 q = abs(local) - (half_size - radius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// Fill inside the border, then the border ring, antialiased; both colours premultiplied.
vec4 surface_pixel(vec4 fill, vec4 border, float dist, float border_width, float inverse_aa, float outer) {
    float inner = clamp(0.5 - (dist + border_width) * inverse_aa, 0.0, 1.0);
    return fill * inner + border * (outer - inner);
}

vec4 shade() {
    vec2 local = v_local_dip.xy;
    vec2 dip = v_local_dip.zw;
    vec2 half_size = v_half;
    float kind = v_params.y;
    // A non-negative kind is an ordinary rounded rectangle's border width.
    if (kind >= 0.0) {
        float dist = rounded_distance(local, half_size, v_params.x);
        // One pixel in DIPs; the distance field's own derivative doubles at a sharp corner.
        float inverse_aa = 1.0 / max(fwidth(local.x), 0.0001);
        return surface_pixel(v_fill, v_border, dist, kind, inverse_aa, clamp(0.5 - dist * inverse_aa, 0.0, 1.0));
    }
    if (kind == -3.0)
        return vec4(background_dither(backdrop_gradient(dip), gl_FragCoord.xy) * v_fill.a, v_fill.a);
    // The rounded rectangle every remaining kind but the icon, line and edge strip shapes itself with.
    float radius = min(v_params.x, min(half_size.x, half_size.y));
    float dist = rounded_distance(local, half_size, radius);
    float aa = max(fwidth(local.x), 0.0001);
    if (kind == -7.0 || kind == -8.0) {
        // Panels (-7) always tint toward the backdrop; plain surfaces (-8) only while a backdrop gradient is active.
        bool panel = kind == -7.0;
        bool light = background_colors[1].w > 0.5;
        vec3 fill = v_fill.rgb;
        if (panel || background_controls.z >= 1.0)
            fill = mix(light ? vec3(1.0) : vec3(0.0), backdrop_gradient(dip), clamp(v_params.w, 0.0, 1.0));
        fill = background_dither(fill, gl_FragCoord.xy);
        vec3 border = v_border.rgb;
        if (panel && v_params.z < 0.0)
            border = mix(fill, light ? vec3(0.0) : vec3(1.0), 0.12);
        float inverse_aa = 1.0 / aa;
        return surface_pixel(vec4(fill * v_fill.a, v_fill.a), vec4(border * v_border.a, v_border.a), dist,
                             min(panel ? abs(v_params.z) : 0.0, min(half_size.x, half_size.y)), inverse_aa,
                             clamp(0.5 - dist * inverse_aa, 0.0, 1.0));
    }
    if (kind == -9.0) {
        // Pixel-snapped icon read 1:1 from its atlas cell; a negative origin is an icon the atlas could not resolve.
        if (v_params.z < 0.0)
            discard;
        vec2 extent = floor(half_size * 2.0 * v_params.x + 0.5);
        vec2 texel = clamp(floor((local + half_size) * v_params.x), vec2(0.0), extent - 1.0);
        float coverage = texelFetch(icon_atlas, ivec2(v_params.zw + texel), 0).x;
        return vec4(v_fill.rgb * v_fill.a, v_fill.a) * coverage;
    }
    if (kind == -1.0) {
        vec2 a = -v_params.zw, segment = v_params.zw * 2.0;
        float t = clamp(dot(local - a, segment) / max(dot(segment, segment), 0.00001), 0.0, 1.0);
        float d = length(local - a - t * segment) - v_params.x;
        float coverage = clamp(0.5 - d / max(fwidth(d), 0.0001), 0.0, 1.0);
        return vec4(v_fill.rgb * v_fill.a, v_fill.a) * coverage;
    }
    if (kind == -10.0) {
        // The leading strip of a rounded rectangle v_params.z wide (an accent border); the shape extends past the quad.
        vec2 shape_half = vec2(v_params.z * 0.5, half_size.y);
        float d = rounded_distance(vec2(local.x + half_size.x - shape_half.x, local.y), shape_half,
                                   min(v_params.x, min(shape_half.x, shape_half.y)));
        float coverage = clamp(0.5 - d / max(fwidth(local.x), 0.0001), 0.0, 1.0);
        return vec4(v_fill.rgb * v_fill.a, v_fill.a) * coverage;
    }
    if (kind == -4.0) {
        vec2 uv = clamp((local + half_size) / max(half_size * 2.0, 0.0001), 0.0, 1.0);
        vec3 color = mix(vec3(1.0), v_fill.rgb, uv.x) * (1.0 - uv.y);
        float alpha = v_fill.a * clamp(0.5 - dist / aa, 0.0, 1.0);
        return vec4(color * alpha, alpha);
    }
    if (kind == -5.0) {
        float u = clamp((local.x + half_size.x) / max(half_size.x * 2.0, 0.0001), 0.0, 1.0);
        vec3 color = clamp(abs(fract(u + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
        float alpha = v_fill.a * clamp(0.5 - dist / aa, 0.0, 1.0);
        return vec4(color * alpha, alpha);
    }
    if (kind == -6.0) {
        vec2 direction = v_params.zw;
        float position = clamp(0.5 + dot(local, direction) / max(dot(half_size, abs(direction)) * 2.0, 0.0001), 0.0, 1.0);
        if (position < v_fill.a || (position >= v_border.a && v_border.a < 1.0))
            discard;
        vec3 color = mix(v_fill.rgb, v_border.rgb,
                         clamp((position - v_fill.a) / max(v_border.a - v_fill.a, 0.0001), 0.0, 1.0));
        float alpha = frame.group_opacity * clamp(0.5 - dist / aa, 0.0, 1.0);
        return vec4(color * alpha, alpha);
    }
    // -2: vertical two-colour gradient.
    vec4 gradient = mix(v_fill, v_border, clamp((local.y + half_size.y) / max(half_size.y * 2.0, 0.0001), 0.0, 1.0));
    return vec4(gradient.rgb * gradient.a, gradient.a) * clamp(0.5 - dist / aa, 0.0, 1.0);
}

void main() {
    out_color = shade();
}

#endif
