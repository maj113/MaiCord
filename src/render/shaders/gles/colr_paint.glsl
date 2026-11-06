// twin: src/render/shaders/colr_paint.hlsl 6bcdee881a9484ca
// colr_paint (STAGE_CS): the COLRv1 postfix paint evaluator. Results are premultiplied sRGB and retained in an RGBA
// atlas; no paint-tree work runs when drawing a cached glyph. Variants: COLR_STACK_SIZE (paint stack depth, 8),
// COLR_LAYERS (the sliced plan evaluator below) with COLR_SLICES threads per pixel (8).
#ifndef COLR_STACK_SIZE
#define COLR_STACK_SIZE 8
#endif
#define COLR_JOB_CAPACITY 64

#if COLR_LAYERS
// Each pixel's plan is cut into COLR_SLICES consecutive runs evaluated by separate threads: a pixel's layers are a
// serial chain of dependent loads.
#ifndef COLR_SLICES
#define COLR_SLICES 8
#endif
layout(local_size_x = 8, local_size_y = 8, local_size_z = COLR_SLICES) in;
shared vec4 slice_color[COLR_SLICES][64];
shared float slice_scale[COLR_SLICES][64];
#else
layout(local_size_x = 8, local_size_y = 8) in;
#endif

struct ColrJob {
    uvec2 destination_origin, destination_size;
    vec2 outline_origin;
    float pixels_per_em;
    uint first_node, allocation_padding, node_count;
    uint program_base, plan_offset;
};
// One dispatch paints up to COLR_JOB_CAPACITY glyphs: group z selects the job.
layout(std140, binding = 0) uniform ColrConstants { ColrJob jobs[COLR_JOB_CAPACITY]; };

layout(std430, binding = 0) readonly buffer Program { uint colr_program[]; }; //! t0
layout(rgba8, binding = 0) uniform writeonly highp image2D color_atlas; //! u0

// The job being painted.
uvec2 destination_origin, destination_size;
vec2 outline_origin;
float pixels_per_em;
uint first_node, allocation_padding, node_count, program_base, plan_offset;

uint load1(uint at) { return colr_program[(program_base + at) >> 2u]; }
uvec2 load2(uint at) {
    uint i = (program_base + at) >> 2u;
    return uvec2(colr_program[i], colr_program[i + 1u]);
}
uvec4 load4(uint at) {
    uint i = (program_base + at) >> 2u;
    return uvec4(colr_program[i], colr_program[i + 1u], colr_program[i + 2u], colr_program[i + 3u]);
}
vec4 premultiply(vec4 c) { return vec4(c.rgb * c.a, c.a); }
vec3 clip_color(vec3 c) {
    float l = dot(c, vec3(0.3, 0.59, 0.11)), lo = min(c.r, min(c.g, c.b)), hi = max(c.r, max(c.g, c.b));
    if (lo < 0.0) c = l + (c - l) * l / max(l - lo, 1e-8);
    if (hi > 1.0) c = l + (c - l) * (1.0 - l) / max(hi - l, 1e-8);
    return c;
}
vec3 set_lum(vec3 c, float l) { return clip_color(c + l - dot(c, vec3(0.3, 0.59, 0.11))); }
float saturation(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }
vec3 set_sat(vec3 c, float s) {
    float lo = min(c.r, min(c.g, c.b)), hi = max(c.r, max(c.g, c.b));
    return hi > lo ? (c - lo) * s / (hi - lo) : vec3(0.0);
}
vec4 composite(vec4 s, vec4 d, uint mode) {
    if (mode == 0u) return vec4(0.0);
    if (mode == 1u) return s;
    if (mode == 2u) return d;
    if (mode == 3u) return s + d * (1.0 - s.a);
    if (mode == 4u) return d + s * (1.0 - d.a);
    if (mode == 5u) return s * d.a;
    if (mode == 6u) return d * s.a;
    if (mode == 7u) return s * (1.0 - d.a);
    if (mode == 8u) return d * (1.0 - s.a);
    if (mode == 9u) return s * d.a + d * (1.0 - s.a);
    if (mode == 10u) return d * s.a + s * (1.0 - d.a);
    if (mode == 11u) return s * (1.0 - d.a) + d * (1.0 - s.a);
    if (mode == 12u) return clamp(s + d, 0.0, 1.0);
    vec3 a = s.a > 0.0 ? s.rgb / s.a : vec3(0.0), b = d.a > 0.0 ? d.rgb / d.a : vec3(0.0), c = vec3(0.0);
    if (mode == 13u) c = a + b - a * b;
    if (mode == 14u) c = mix(1.0 - 2.0 * (1.0 - a) * (1.0 - b), 2.0 * a * b, lessThanEqual(b, vec3(0.5)));
    if (mode == 15u) c = min(a, b);
    if (mode == 16u) c = max(a, b);
    if (mode == 17u)
        c = mix(mix(min(vec3(1.0), b / max(1.0 - a, 1e-8)), vec3(1.0), greaterThanEqual(a, vec3(1.0))), vec3(0.0),
                lessThanEqual(b, vec3(0.0)));
    if (mode == 18u)
        c = mix(mix(1.0 - min(vec3(1.0), (1.0 - b) / max(a, 1e-8)), vec3(0.0), lessThanEqual(a, vec3(0.0))),
                vec3(1.0), greaterThanEqual(b, vec3(1.0)));
    if (mode == 19u) c = mix(1.0 - 2.0 * (1.0 - a) * (1.0 - b), 2.0 * a * b, lessThanEqual(a, vec3(0.5)));
    if (mode == 20u) {
        vec3 q = mix(sqrt(b), ((16.0 * b - 12.0) * b + 4.0) * b, lessThanEqual(b, vec3(0.25)));
        c = mix(b + (2.0 * a - 1.0) * (q - b), b - (1.0 - 2.0 * a) * b * (1.0 - b), lessThanEqual(a, vec3(0.5)));
    }
    if (mode == 21u) c = abs(b - a);
    if (mode == 22u) c = a + b - 2.0 * a * b;
    if (mode == 23u) c = a * b;
    if (mode >= 24u) {
        // Non-separable modes share one set_sat and set_lum: hue and saturation keep a (24) or b (25) with the
        // other's saturation; color and luminosity swap which side supplies the luminosity.
        vec3 x = (mode == 25u || mode == 27u) ? b : a;
        if (mode < 26u) x = set_sat(x, saturation(mode == 24u ? b : a));
        c = set_lum(x, dot(mode == 27u ? a : b, vec3(0.3, 0.59, 0.11)));
    }
    return vec4(s.rgb * (1.0 - d.a) + d.rgb * (1.0 - s.a) + s.a * d.a * c, s.a + d.a - s.a * d.a);
}
// Match Windows DrawPaintGlyphRun: interpolate straight sRGB stops, then premultiply. Premultiplying stops first
// changes transparent-gradient hues.
vec4 gradient(uint offset, uint count, uint extend, float t) {
    if (count == 0u) return vec4(0.0);
    float first = uintBitsToFloat(load1(offset));
    float last = uintBitsToFloat(load1(offset + (count - 1u) * 20u));
    float span = last - first;
    if (span > 0.0) {
        if (extend == 1u) t = first + fract((t - first) / span) * span;
        if (extend == 2u) t = first + (1.0 - abs(1.0 - ((t - first) / span - 2.0 * floor((t - first) / span / 2.0)))) * span;
    } else if (extend != 0u) return vec4(0.0);
    vec4 previous = uintBitsToFloat(load4(offset + 4u));
    if (t < first) return premultiply(previous);
    float previous_t = first;
    for (uint i = 1u; i < count; ++i) {
        float position = uintBitsToFloat(load1(offset + i * 20u));
        vec4 color = uintBitsToFloat(load4(offset + i * 20u + 4u));
        if (t < position)
            return premultiply(mix(previous, color, clamp((t - previous_t) / max(position - previous_t, 1e-12), 0.0, 1.0)));
        previous = color;
        previous_t = position;
    }
    return premultiply(previous);
}
// One outline edge: its distance only when within a pixel of p (a farther edge cannot change the saturated coverage)
// and its winding crossing.
void edge_step(vec4 edge, vec2 p, float gutter2, inout int winding, inout float distance2) {
    vec2 a = edge.xy, v = edge.zw - edge.xy, d = p - a;
    // Bounds gap from the edge's centre and half extent (cheaper than min/max of both ends).
    vec2 gap = max(abs(d - 0.5 * v) - 0.5 * abs(v), 0.0);
    if (dot(gap, gap) < gutter2) {
        float t = clamp(dot(d, v) * (1.0 / max(dot(v, v), 1e-20)), 0.0, 1.0);
        vec2 delta = d - t * v;
        distance2 = min(distance2, dot(delta, delta));
    }
    // Crossing right of p without a division: the sign of the side test against the edge direction replaces
    // comparing the crossing abscissa.
    if ((a.y <= p.y) != (edge.w <= p.y)) {
        float side = v.x * d.y - d.x * v.y;
        if (side * v.y > 0.0) winding += v.y > 0.0 ? 1 : -1;
    }
}
vec4 load_edge(uint offset, uint index) { return uintBitsToFloat(load4(offset + index * 16u)); }
float mask(uint offset, uint indices, uint count, uint fill, vec2 p) {
    float gutter2 = 1.0 / (pixels_per_em * pixels_per_em);
    int winding = 0;
    float distance2 = 1e20;
    // Two edges per step: both indices, then both edges, load together instead of one dependent pair per edge. A
    // missing second edge is one far from p, which adds neither distance nor winding.
    for (uint i = 0u; i < count; i += 2u) {
        uvec2 pair = load2(indices + i * 4u);
        vec4 first = load_edge(offset, pair.x), second = load_edge(offset, pair.y);
        if (i + 1u == count) second = vec4(1e9);
        edge_step(first, p, gutter2, winding, distance2);
        edge_step(second, p, gutter2, winding, distance2);
    }
    bool inside = fill != 0u ? winding != 0 : ((abs(winding) & 1) != 0);
    return clamp(0.5 + (inside ? 1.0 : -1.0) * sqrt(distance2) * pixels_per_em, 0.0, 1.0);
}
float shape_coverage(uint data, uvec2 pixel, vec2 p) {
    // Placement, header and bounds are independent loads of one record.
    uvec4 placement = load4(data + 16u), header = load4(data);
    vec4 bounds = uintBitsToFloat(load4(data + 32u));
    ivec2 local = ivec2(outline_origin) + ivec2(pixel - allocation_padding) - ivec2(placement.xy);
    float gutter = 1.0 / pixels_per_em;
    if (any(lessThan(local, ivec2(0))) || any(greaterThanEqual(local, ivec2(placement.zw))) ||
        any(lessThan(p, bounds.xy - gutter)) || any(greaterThan(p, bounds.zw + gutter)))
        return 0.0;
    uvec2 row = load2(header.w + uint(local.y) * 8u);
    return row.y == 0u ? 0.0 : mask(header.x, row.x, row.y, header.z, p);
}

void main() {
#if COLR_LAYERS
    uint slice = gl_LocalInvocationID.z, lane = gl_LocalInvocationID.y * 8u + gl_LocalInvocationID.x;
    uvec3 thread = uvec3(gl_WorkGroupID.xy * 8u + gl_LocalInvocationID.xy, gl_WorkGroupID.z);
#else
    uvec3 thread = gl_GlobalInvocationID;
#endif
    ColrJob job = jobs[thread.z];
    destination_origin = job.destination_origin;
    destination_size = job.destination_size;
    outline_origin = job.outline_origin;
    pixels_per_em = job.pixels_per_em;
    first_node = job.first_node;
    allocation_padding = job.allocation_padding;
    node_count = job.node_count;
    program_base = job.program_base;
    plan_offset = job.plan_offset;
#if COLR_LAYERS
    // Every thread of the group reaches the barrier below; a thread outside the glyph's body runs no layers.
    bool inside = all(lessThan(thread.xy, destination_size));
    bool body = inside && all(greaterThanEqual(thread.xy, uvec2(allocation_padding))) &&
                all(lessThan(thread.xy, destination_size - allocation_padding));
    vec4 accumulated = vec4(0.0);
    float scale = 1.0;
    vec2 p = vec2(0.0);
    uint at = first_node;
    uvec2 plan = uvec2(0u, node_count);
    uint first_index = 0u, end_index = 0u;
    if (body) {
        p = (outline_origin + vec2(thread.xy - allocation_padding) + 0.5) / pixels_per_em;
        if (plan_offset != 0u) {
            uint columns = (destination_size.x + 7u) / 8u;
            plan = load2(plan_offset + ((thread.y / 8u) * columns + thread.x / 8u) * 8u);
        }
        // Without a plan the nodes chain by link and slice 0 walks them all.
        first_index = plan_offset != 0u ? slice * plan.y / uint(COLR_SLICES) : 0u;
        end_index = plan_offset != 0u ? (slice + 1u) * plan.y / uint(COLR_SLICES) : (slice == 0u ? plan.y : 0u);
    }
#else
    if (any(greaterThanEqual(thread.xy, destination_size))) return;
    if (any(lessThan(thread.xy, uvec2(allocation_padding))) ||
        any(greaterThanEqual(thread.xy, destination_size - allocation_padding))) {
        imageStore(color_atlas, ivec2(destination_origin + thread.xy), vec4(0.0));
        return;
    }
    vec2 p = (outline_origin + vec2(thread.xy - allocation_padding) + 0.5) / pixels_per_em;
    vec4 values[COLR_STACK_SIZE];
    uint top = 0u, at = first_node;
    uint first_index = 0u, end_index = node_count;
#endif
    for (uint index = first_index; index < end_index; ++index) {
#if COLR_LAYERS
        if (plan_offset != 0u) at = load1(plan.x + index * 4u);
#endif
        uvec4 header = load4(at);
        uint kind = header.y, data = header.z, count = header.w;
#if COLR_LAYERS
        // The CPU admits only a left-associated source-over chain with root clips. No general paint stack or
        // composite node evaluation is needed.
        if (kind == 6u) { at = header.x; continue; }
        vec4 leaf = vec4(0.0);
#endif
        bool shaped = kind >= 8u && kind <= 11u;
        float coverage = 1.0;
        // The node's own fields load with its header, ahead of the mask lookup.
        uvec2 extra = load2(at + 16u);
        vec4 m = uintBitsToFloat(load4(at + 48u)), a = uintBitsToFloat(load4(at + 24u));
        vec2 translation = uintBitsToFloat(load2(at + 64u)), b = uintBitsToFloat(load2(at + 40u));
        if (shaped || kind == 4u) coverage = shape_coverage(shaped ? extra.y : data, thread.xy, p);
        if (shaped) {
            if (coverage == 0.0) {
#if !COLR_LAYERS
                values[top++] = vec4(0.0);
#endif
                at = header.x;
                continue;
            }
            kind -= 8u;
        }
        vec2 q = vec2(p.x * m.x + p.y * m.z, p.x * m.y + p.y * m.w) + translation;
        uint mode = extra.x;
#if COLR_LAYERS
        if (kind == 0u) leaf = premultiply(a);
#else
        if (kind == 0u) values[top++] = premultiply(a);
#endif
        else if (kind <= 3u) {
            float t = 0.0;
            bool valid = mode != 0xffffffffu;
            if (kind == 1u) {
                vec2 d = a.zw - a.xy, h = b.xy - a.xy, v = q - a.xy;
                float det = d.x * h.y - d.y * h.x;
                valid = valid && abs(det) > 1e-12;
                t = (v.x * h.y - v.y * h.x) / (valid ? det : 1.0);
            } else if (kind == 2u) {
                vec2 d = a.zw - a.xy, v = q - a.xy;
                float r0 = b.x, dr = b.y - b.x;
                float A = dot(d, d) - dr * dr, B = -2.0 * (dot(v, d) + r0 * dr), C = dot(v, v) - r0 * r0;
                float discriminant = B * B - 4.0 * A * C;
                if (abs(A) < 1e-10) {
                    valid = valid && abs(B) > 1e-10;
                    t = -C / (valid ? B : 1.0);
                    valid = valid && (r0 + t * dr >= 0.0);
                } else {
                    valid = valid && discriminant >= 0.0;
                    float root = sqrt(max(0.0, discriminant));
                    float t0 = (-B - root) / (2.0 * A), t1 = (-B + root) / (2.0 * A);
                    bool v0 = r0 + t0 * dr >= 0.0, v1 = r0 + t1 * dr >= 0.0;
                    valid = valid && (v0 || v1);
                    t = v0 && v1 ? max(t0, t1) : (v0 ? t0 : t1);
                }
            } else {
                vec2 d = q - a.xy;
                float angle = atan(-d.y, d.x) * 57.2957795131;
                if (angle < 0.0) angle += 360.0;
                if (a.w == a.z) {
                    valid = valid && mode == 0u;
                    t = angle < a.z ? -1e20 : 1e20;
                } else {
                    t = (angle - a.z) / (a.w - a.z);
                }
            }
#if COLR_LAYERS
            leaf = valid ? gradient(data, count, mode, t) : vec4(0.0);
#else
            values[top++] = valid ? gradient(data, count, mode, t) : vec4(0.0);
#endif
#if !COLR_LAYERS
        } else if (kind == 4u) {
            values[top - 1u] *= coverage;
#endif
        } else if (kind == 5u) {
            // Clip rectangle is in local space, including rotations and shear.
            vec2 pixel_extent = 0.5 * vec2(length(m.xz), length(m.yw)) / pixels_per_em;
            vec2 clip_coverage = clamp((min(q - a.xy, a.zw - q) + pixel_extent) / max(2.0 * pixel_extent, 1e-12), 0.0, 1.0);
#if COLR_LAYERS
            float clip = clip_coverage.x * clip_coverage.y;
            accumulated *= clip;
            scale *= clip;
#else
            values[top - 1u] *= clip_coverage.x * clip_coverage.y;
#endif
#if !COLR_LAYERS
        } else if (kind == 6u) {
            vec4 upper = values[--top], lower = values[top - 1u];
            // Select operands before inlining the full composite evaluator.
            values[top - 1u] = composite(count != 0u ? upper : lower, count != 0u ? lower : upper, mode);
        } else {
            values[top++] = vec4(0.0);
#endif
        }
#if COLR_LAYERS
        if (shaped) {
            leaf *= coverage;
            accumulated = leaf + accumulated * (1.0 - leaf.a);
            scale *= 1.0 - leaf.a;
        }
#else
        if (shaped) values[top - 1u] *= coverage;
#endif
        at = header.x;
    }
#if COLR_LAYERS
    slice_color[slice][lane] = accumulated;
    slice_scale[slice][lane] = scale;
    memoryBarrierShared();
    barrier();
    if (slice == 0u && inside) {
        // Source-over and root clips are affine in the colour below them (below*scale + color), so the slices
        // combine in layer order.
        vec4 result = slice_color[0][lane];
        for (uint other = 1u; other < uint(COLR_SLICES); ++other)
            result = result * slice_scale[other][lane] + slice_color[other][lane];
        imageStore(color_atlas, ivec2(destination_origin + thread.xy), body ? result : vec4(0.0));
    }
#else
    imageStore(color_atlas, ivec2(destination_origin + thread.xy), top == 1u ? values[0] : vec4(0.0));
#endif
}
