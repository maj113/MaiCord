// twin: src/render/shaders/glyph_atlas.hlsl a9633909fbf78d91
// glyph_coverage (STAGE_CS): rasterizes glyph and icon outlines into the coverage atlas. GLYPH_TILE_CULLING builds the
// variant for unstroked rasters that are large or have a long command chain, which skips every command that cannot
// reach any sample of a thread group's tile. Other rasters run the plain loop of the default variant, whose
// per-command cost is lower.
layout(local_size_x = 8, local_size_y = 8) in;

struct GlyphPathCommand {
    vec2 point1;
    vec2 point2;
    vec2 point3;
    uint kind;
};

layout(std430, binding = 0) readonly buffer RasterData { uint raster_data[]; }; //! t0
// ES 3.1 storage images have no 8-bit single-channel format: the atlas is RGBA8 and coverage is stored in every channel.
layout(rgba8, binding = 0) uniform writeonly highp image2D coverage_atlas; //! u0

// GLYPH_JOBS is the job count a dispatch carries: the job headers fill the first GLYPH_JOBS records of the raster
// data (glyph_raster_batch_limit in C++).
#ifndef GLYPH_JOBS
#define GLYPH_JOBS 16
#endif

#if defined(GLYPH_TILE_CULLING)
void accumulate_crossing(vec2 sample_position, vec2 line_start, vec2 line_end, inout int winding) {
    bool crosses = (line_start.y > sample_position.y) != (line_end.y > sample_position.y);
    if (crosses) {
        float crossing_x = line_start.x + (sample_position.y - line_start.y) * (line_end.x - line_start.x) /
                                              (line_end.y - line_start.y);
        if (crossing_x > sample_position.x)
            winding += line_end.y > line_start.y ? 1 : -1;
    }
}

void accumulate_distance(vec2 sample_position, vec2 line_start, vec2 line_end, inout float minimum_distance_squared) {
    vec2 delta = line_end - line_start;
    float length_squared = dot(delta, delta);
    float projection =
        length_squared > 1.0e-12 ? clamp(dot(sample_position - line_start, delta) / length_squared, 0.0, 1.0) : 0.0;
    vec2 distance_vector = sample_position - (line_start + delta * projection);
    minimum_distance_squared = min(minimum_distance_squared, dot(distance_vector, distance_vector));
}
#else
void accumulate_line(vec2 sample_position, vec2 line_start, vec2 line_end, inout int winding,
                     inout float minimum_distance_squared) {
    vec2 delta = line_end - line_start;
    float length_squared = dot(delta, delta);
    float projection =
        length_squared > 1.0e-12 ? clamp(dot(sample_position - line_start, delta) / length_squared, 0.0, 1.0) : 0.0;
    vec2 distance_vector = sample_position - (line_start + delta * projection);
    minimum_distance_squared = min(minimum_distance_squared, dot(distance_vector, distance_vector));

    bool crosses = (line_start.y > sample_position.y) != (line_end.y > sample_position.y);
    if (crosses) {
        float crossing_x = line_start.x + (sample_position.y - line_start.y) * (line_end.x - line_start.x) /
                                              (line_end.y - line_start.y);
        if (crossing_x > sample_position.x)
            winding += line_end.y > line_start.y ? 1 : -1;
    }
}
#endif

// Stroked (icon) contours contribute only their centreline distance.
void accumulate_stroke(vec2 sample_position, vec2 line_start, vec2 line_end, inout float minimum_distance_squared) {
    vec2 delta = line_end - line_start;
    // A degenerate segment is a dot; the clamp keeps it at line_start.
    float projection = clamp(dot(sample_position - line_start, delta) / max(dot(delta, delta), 1.0e-12), 0.0, 1.0);
    vec2 distance_vector = sample_position - (line_start + delta * projection);
    minimum_distance_squared = min(minimum_distance_squared, dot(distance_vector, distance_vector));
}

// Cubic Bezier weights at t = k/12 (k = 1..12), rounded after each binary32 operation; verify with
// tools/verify_glyph_cubic_weights.py. Each literal is the shortest decimal that reads back as the same binary32.
const vec4 cubic_weights[12] = vec4[12](
    vec4(0.7702547, 0.21006946, 0.019097224, 0.00057870377),
    vec4(0.57870364, 0.3472222, 0.06944445, 0.00462963),
    vec4(0.421875, 0.421875, 0.140625, 0.015625),
    vec4(0.29629624, 0.4444444, 0.22222221, 0.03703704),
    vec4(0.1984954, 0.42534727, 0.30381945, 0.072337955),
    vec4(0.125, 0.375, 0.375, 0.125),
    vec4(0.07233798, 0.30381945, 0.42534718, 0.19849536),
    vec4(0.03703703, 0.2222222, 0.44444442, 0.29629633),
    vec4(0.015625, 0.140625, 0.421875, 0.421875),
    vec4(0.004629631, 0.069444455, 0.34722227, 0.57870364),
    vec4(0.0005787033, 0.019097213, 0.2100694, 0.7702547),
    vec4(0.0, 0.0, 0.0, 1.0));

GlyphPathCommand load_command(uint index) {
    uint at = 7u * index;
    GlyphPathCommand command;
    command.point1 = uintBitsToFloat(uvec2(raster_data[at], raster_data[at + 1u]));
    command.point2 = uintBitsToFloat(uvec2(raster_data[at + 2u], raster_data[at + 3u]));
    command.point3 = uintBitsToFloat(uvec2(raster_data[at + 4u], raster_data[at + 5u]));
    command.kind = raster_data[at + 6u];
    return command;
}

#if defined(GLYPH_TILE_CULLING)
// Per-thread state shared by the helpers below.
vec2 sample_position, tile_low, tile_high;
float reach, minimum_distance_squared, stroke_distance_squared;
int winding;

// What a segment or curve whose control points span [low, high] can change for any sample of this group's tile:
// 0 nothing, 1 only the winding, 2 the distance too. A distance beyond `reach` (one pixel, in em) saturates the
// coverage whatever its value, and a crossing needs the box to overlap the sample rows and reach the sample or lie to
// its right.
uint relevance(vec2 low, vec2 high) {
    if (tile_low.y - high.y > reach || low.y - tile_high.y > reach || tile_low.x - high.x > reach)
        return 0u;
    return low.x - tile_high.x > reach ? 1u : 2u;
}

// The relevance is uniform across the group, so the branch skips the distance work coherently.
void accumulate_segment(vec2 segment_start, vec2 segment_end, uint relevant) {
    if (relevant == 2u)
        accumulate_distance(sample_position, segment_start, segment_end, minimum_distance_squared);
    accumulate_crossing(sample_position, segment_start, segment_end, winding);
}

// `with_distance` is a literal at each call site: a winding-only curve keeps no distance arithmetic in its loop.
void accumulate_cubic(vec2 start, GlyphPathCommand command, bool with_distance) {
    vec2 segment_start = start;
    // Three copies keep loop overhead bounded without twelve copies of projection, distance and winding arithmetic.
    for (uint first_segment = 0u; first_segment < 12u; first_segment += 3u) {
        for (uint local_segment = 0u; local_segment < 3u; ++local_segment) {
            vec4 weights = cubic_weights[first_segment + local_segment];
            vec2 segment_end = weights.x * start + weights.y * command.point1 + weights.z * command.point2 +
                               weights.w * command.point3;
            if (with_distance)
                accumulate_distance(sample_position, segment_start, segment_end, minimum_distance_squared);
            accumulate_crossing(sample_position, segment_start, segment_end, winding);
            segment_start = segment_end;
        }
    }
}

void trace_commands(uint first_command, uint command_count) {
    vec2 current = vec2(0.0);
    vec2 contour_start = vec2(0.0);
    bool contour_filled = false;
    for (uint index = 0u; index < command_count; ++index) {
        GlyphPathCommand command = load_command(first_command + index);
        if (command.kind == 0u || command.kind == 1u) {
            current = command.point1;
            contour_start = current;
            contour_filled = command.kind == 0u;
        } else if (command.kind == 2u || command.kind == 5u) {
            vec2 end = command.kind == 2u ? command.point1 : contour_start;
            uint relevant = contour_filled ? relevance(min(current, end), max(current, end)) : 0u;
            if (relevant != 0u)
                accumulate_segment(current, end, relevant);
            current = end;
        } else if (command.kind == 3u) {
            uint relevant =
                contour_filled ? relevance(min(min(current, command.point1), min(command.point2, command.point3)),
                                           max(max(current, command.point1), max(command.point2, command.point3)))
                               : 0u;
            if (relevant == 2u)
                accumulate_cubic(current, command, true);
            else if (relevant != 0u)
                accumulate_cubic(current, command, false);
            current = command.point3;
        }
    }
}
#endif

void main() {
    uvec3 id = gl_GlobalInvocationID;
    uint offset = id.z * 7u;
    uvec4 raster = uvec4(raster_data[offset], raster_data[offset + 1u], raster_data[offset + 2u],
                         raster_data[offset + 3u]);
    vec3 values = uintBitsToFloat(uvec3(raster_data[offset + 4u], raster_data[offset + 5u], raster_data[offset + 6u]));
    uvec2 destination_origin = uvec2(raster.x & 65535u, raster.x >> 16u);
    uvec2 destination_size = uvec2(raster.y & 65535u, raster.y >> 16u);
    uint command_count = raster.z & 65535u, fill_rule = (raster.z >> 16u) & 1u, allocation_padding = raster.w >> 16u;
    // Stroke half-width in 1/256 pixels; zero (all text) leaves coverage unchanged.
    float stroke_half_width = float(raster.z >> 17u) * (1.0 / 256.0);
#if defined(GLYPH_TILE_CULLING)
    const bool stroking = false; // Stroked rasters take the default variant.
#else
    bool stroking = stroke_half_width > 0.0;
#endif
    float pixels_per_em = values.x;
    vec2 outline_origin = values.yz;
    if (id.x >= destination_size.x || id.y >= destination_size.y)
        return;

    // Linear filtering can reach outside the glyph's UV rectangle. Initialize the allocator-owned gutter on every
    // raster, including recycled shelves.
    if (any(lessThan(id.xy, uvec2(allocation_padding))) ||
        any(greaterThanEqual(id.xy, destination_size - allocation_padding))) {
        imageStore(coverage_atlas, ivec2(destination_origin + id.xy), vec4(0.0));
        return;
    }

#if defined(GLYPH_TILE_CULLING)
    // Sample positions grow monotonically with the pixel, so these bound every sample of the group; the reach absorbs
    // rounding.
    ivec2 first_pixel = ivec2(gl_WorkGroupID.xy * 8u) - int(allocation_padding);
    tile_low = outline_origin + (vec2(first_pixel) + 0.5) / pixels_per_em;
    tile_high = outline_origin + (vec2(first_pixel + 7) + 0.5) / pixels_per_em;
    reach = 1.0 / pixels_per_em;
    sample_position = outline_origin + (vec2(id.xy - allocation_padding) + 0.5) / pixels_per_em;
    winding = 0;
    // Infinity keeps the reduction valid even when sample coordinates are large.
    minimum_distance_squared = uintBitsToFloat(0x7f800000u);
    stroke_distance_squared = uintBitsToFloat(0x7f800000u);
    trace_commands(uint(GLYPH_JOBS) + (raster.w & 65535u), command_count);
#else
    vec2 sample_position = outline_origin + (vec2(id.xy - allocation_padding) + 0.5) / pixels_per_em;
    vec2 current = vec2(0.0);
    vec2 contour_start = vec2(0.0);
    int winding = 0;
    // Infinity keeps the reduction valid even when sample coordinates are large.
    float minimum_distance_squared = uintBitsToFloat(0x7f800000u);
    float stroke_distance_squared = uintBitsToFloat(0x7f800000u);
    bool contour_filled = false;

    for (uint index = 0u; index < command_count; ++index) {
        GlyphPathCommand command = load_command(uint(GLYPH_JOBS) + (raster.w & 65535u) + index);
        if (command.kind == 0u || command.kind == 1u) {
            current = command.point1;
            contour_start = current;
            contour_filled = command.kind == 0u;
        } else if (command.kind == 2u) {
            // Uniform per command: a filled contour skips the stroke distance instead of computing it and selecting
            // it away.
            if (contour_filled)
                accumulate_line(sample_position, current, command.point1, winding, minimum_distance_squared);
            else if (stroking)
                accumulate_stroke(sample_position, current, command.point1, stroke_distance_squared);
            current = command.point1;
        } else if (command.kind == 3u) {
            if (contour_filled) {
                vec2 segment_start = current;
                // Three copies keep loop overhead bounded without twelve copies of projection, distance and winding
                // arithmetic in the shader.
                for (uint first_segment = 0u; first_segment < 12u; first_segment += 3u) {
                    for (uint local_segment = 0u; local_segment < 3u; ++local_segment) {
                        vec4 weights = cubic_weights[first_segment + local_segment];
                        vec2 segment_end = weights.x * current + weights.y * command.point1 +
                                           weights.z * command.point2 + weights.w * command.point3;
                        accumulate_line(sample_position, segment_start, segment_end, winding,
                                        minimum_distance_squared);
                        segment_start = segment_end;
                    }
                }
            } else if (stroking) {
                vec2 segment_start = current;
                for (uint segment = 0u; segment < 12u; ++segment) {
                    vec4 weights = cubic_weights[segment];
                    vec2 segment_end = weights.x * current + weights.y * command.point1 + weights.z * command.point2 +
                                       weights.w * command.point3;
                    accumulate_stroke(sample_position, segment_start, segment_end, stroke_distance_squared);
                    segment_start = segment_end;
                }
            }
            current = command.point3;
        } else if (command.kind == 5u) {
            if (contour_filled)
                accumulate_line(sample_position, current, contour_start, winding, minimum_distance_squared);
            else if (stroking)
                accumulate_stroke(sample_position, current, contour_start, stroke_distance_squared);
            current = contour_start;
        }
    }
#endif

    bool inside = fill_rule == 0u ? ((abs(winding) & 1) != 0) : winding != 0;
    // Square root and the positive pixel scale are monotonic, so they apply once to the minimum rather than per
    // segment.
    float dist = sqrt(minimum_distance_squared) * pixels_per_em;
    float signed_distance = inside ? -dist : dist;
    float coverage = clamp(0.5 - signed_distance, 0.0, 1.0);
    if (stroking) {
        // Filled contours are stroked too, giving round joins on solid shapes.
        float stroke_distance = sqrt(min(minimum_distance_squared, stroke_distance_squared)) * pixels_per_em;
        coverage = max(coverage, clamp(0.5 + stroke_half_width - stroke_distance, 0.0, 1.0));
    }
    imageStore(coverage_atlas, ivec2(destination_origin + id.xy), vec4(coverage));
}
