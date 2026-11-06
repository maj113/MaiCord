// twin: src/render/shaders/media_draw.hlsl 678bc8d06128f3e4
// Media quads: media_vertex (STAGE_VS) and media_pixel (STAGE_PS). The atlases are paged surfaces: here each draw
// binds the one page its quads sample, and their v is in that page's rows (the renderer splits draws per page).

layout(location = 0) V2F vec2 v_uv; // atlas coordinate, already decoded
layout(location = 1) V2F float v_opacity;
layout(location = 2) V2F vec2 v_local;
layout(location = 3) V2F vec3 v_shape;
layout(location = 4) V2F vec4 v_clip_local;
layout(location = 5) V2F float v_clip_radius;
// Spoiler blur: standard deviation in layout units; and the atlas the quad samples (0 primary, 1 animation cache, 2
// preview). Then the decoded half-texel-inset source rectangle every blur tap stays inside.
layout(location = 6) flat V2F vec2 v_blur_source;
layout(location = 7) flat V2F vec4 v_bounds;

#if defined(STAGE_VS)

layout(std140, binding = 0) uniform UiFrameVs {
    vec2 coordinate_scale;
    vec2 coordinate_translation;
    float group_opacity;
};

layout(location = 0) in vec4 rect;
layout(location = 1) in vec4 uv_rect;
layout(location = 2) in vec4 params;
layout(location = 3) in vec4 rounded_clip;

// Signs select the atlas: x < 0 is the animation cache, y < 0 the preview atlas.
vec2 decode_coordinate(vec2 c) {
    return c.x < 0.0 ? vec2(-c.x - 1.0, c.y) : c.y < 0.0 ? vec2(c.x, -c.y - 1.0) : c;
}

void main() {
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    vec2 position = mix(rect.xy, rect.zw, corner);
    vec2 clip = position * coordinate_scale + coordinate_translation;
    gl_Position = vec4(clip.x, -clip.y, -1.0, 1.0);
    // A negative blur parameter carries orientation flags instead (cropped avatars and banners turned in 90 degree
    // steps): 1 swaps the texture axes, 2 mirrors u, 4 mirrors v; such quads are never blurred.
    vec2 uv = corner;
    uint orientation = params.w < 0.0 ? uint(-params.w + 0.5) & 7u : 0u;
    uv = (orientation & 1u) != 0u ? uv.yx : uv;
    uv.x = (orientation & 2u) != 0u ? 1.0 - uv.x : uv.x;
    uv.y = (orientation & 4u) != 0u ? 1.0 - uv.y : uv.y;
    vec2 first = decode_coordinate(uv_rect.xy), last = decode_coordinate(uv_rect.zw);
    v_uv = mix(first, last, uv);
    v_opacity = params.x * group_opacity;
    vec2 extent = (rect.zw - rect.xy) * 0.5;
    v_local = (corner * 2.0 - 1.0) * extent;
    v_shape = vec3(extent, params.y);
    v_clip_local = vec4(position - (rounded_clip.xy + rounded_clip.zw) * 0.5, (rounded_clip.zw - rounded_clip.xy) * 0.5);
    v_clip_radius = params.z;
    v_blur_source = vec2(max(params.w, 0.0), uv_rect.x < 0.0 ? 1.0 : uv_rect.y < 0.0 ? 2.0 : 0.0);
    v_bounds = vec4(min(first, last), max(first, last));
}

#else

layout(binding = 0) uniform highp sampler2D media_atlas; //! t0 s0
layout(binding = 1) uniform highp sampler2D animation_frame_atlas; //! t1 s0
layout(binding = 2) uniform highp sampler2D preview_media_atlas; //! t2 s0

layout(location = 0) out vec4 out_color;

// Each atlas has one mip: an explicit level selects a texture without implicit derivatives.
vec4 sample_level(uint source, vec2 c) {
    if (source == 1u) return textureLod(animation_frame_atlas, c, 0.0);
    if (source == 2u) return textureLod(preview_media_atlas, c, 0.0);
    return textureLod(media_atlas, c, 0.0);
}

// Disk of 32 taps (radius two deviations) rotated per pixel by interleaved gradient noise, so undersampling reads as
// an even frosted grain instead of ghosted copies or structured streaks. Tap radii are the quantiles of a 2D Gaussian
// truncated at two deviations, so the taps carry equal weight; the direction turns by the golden angle per tap.
const float radii[32] = float[32](
    0.16494, 0.28766, 0.37401, 0.44575, 0.50921, 0.56726, 0.62152, 0.67301,
    0.72242, 0.77026, 0.81692, 0.86271, 0.90788, 0.95266, 0.99725, 1.04182,
    1.08656, 1.13163, 1.17721, 1.22349, 1.27065, 1.31891, 1.36851, 1.41974,
    1.47291, 1.52844, 1.58679, 1.64861, 1.71469, 1.78612, 1.86442, 1.95184);

vec4 blurred_sample(uint source, vec2 center, vec2 dx, vec2 dy, float deviation, vec4 bounds, vec2 pixel) {
    float angle = 6.2831853 * fract(52.9829189 * fract(dot(floor(pixel), vec2(0.06711056, 0.00583715))));
    vec2 direction = vec2(cos(angle), sin(angle));
    const vec2 turn = vec2(-0.737368818, 0.675490360);
    vec4 sum = vec4(0.0);
    for (uint i = 0u; i < 32u; ++i) {
        vec2 offset = deviation * radii[i] * direction;
        vec2 c = clamp(center + offset.x * dx + offset.y * dy, bounds.xy, bounds.zw);
        // Atlas texels are premultiplied, so transparency never darkens the mean.
        sum += sample_level(source, c);
        direction = vec2(direction.x * turn.x - direction.y * turn.y, direction.x * turn.y + direction.y * turn.x);
    }
    return sum * (1.0 / 32.0);
}

void main() {
    uint source = uint(v_blur_source.y);
    vec2 dx = dFdx(v_uv), dy = dFdy(v_uv);
    // Layout units per target pixel, so the deviation follows the DPI scale.
    float units = max(length(vec2(dFdx(v_local.x), dFdy(v_local.x))), 1e-4);
    vec4 texel;
    if (v_blur_source.x > 0.0)
        texel = blurred_sample(source, v_uv, dx, dy, v_blur_source.x / units, v_bounds, gl_FragCoord.xy);
    else
        texel = sample_level(source, v_uv);
    // Radii are uniform per quad: square media (emoji, video, the viewer) skip both rounded-rectangle distances
    // instead of selecting them away.
    float coverage = 1.0;
    if (v_shape.z > 0.0) {
        float radius = min(v_shape.z, min(v_shape.x, v_shape.y));
        vec2 corner = abs(v_local) - v_shape.xy + radius;
        float dist = length(max(corner, 0.0)) + min(max(corner.x, corner.y), 0.0) - radius;
        float aa = max(fwidth(dist), 0.25);
        coverage = 1.0 - smoothstep(-aa, aa, dist);
    }
    if (v_clip_radius > 0.0) {
        float radius = min(v_clip_radius, min(v_clip_local.z, v_clip_local.w));
        vec2 q = abs(v_clip_local.xy) - v_clip_local.zw + radius;
        float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
        float aa = max(fwidth(dist), 0.25);
        coverage *= 1.0 - smoothstep(-aa, aa, dist);
    }
    out_color = vec4(texel.rgb * v_opacity, texel.a * v_opacity) * coverage;
}

#endif
