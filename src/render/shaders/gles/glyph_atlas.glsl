// twin: src/render/shaders/glyph_atlas.hlsl 7c7c3bd11b160ab5
// glyph_coverage (STAGE_CS): rasterizes glyph and icon outlines into the coverage atlas. Every raster is culled by
// tile: a work group skips each command that cannot reach any sample of its 8 x 8 tile, and takes only the winding of
// one that lies right of it. Measured on the UI's own rasters, that pays at every size.
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

// A crossing right of the sample without a division: the side test's sign against the edge direction replaces the
// crossing abscissa (as colr_paint does). Across the sample row the end and start comparisons differ, and their
// difference is the edge's direction; elsewhere it is zero.
void accumulate_crossing(vec2 sample_position, vec2 line_start, vec2 line_end, inout int winding) {
    vec2 delta = line_end - line_start, offset = sample_position - line_start;
    float side = delta.x * offset.y - offset.x * delta.y;
    winding += side * delta.y > 0.0 ? int(line_end.y > sample_position.y) - int(line_start.y > sample_position.y) : 0;
}

void accumulate_distance(vec2 sample_position, vec2 line_start, vec2 line_end, inout float minimum_distance_squared) {
    vec2 delta = line_end - line_start;
    float length_squared = dot(delta, delta);
    float projection =
        length_squared > 1.0e-12 ? clamp(dot(sample_position - line_start, delta) / length_squared, 0.0, 1.0) : 0.0;
    vec2 distance_vector = sample_position - (line_start + delta * projection);
    minimum_distance_squared = min(minimum_distance_squared, dot(distance_vector, distance_vector));
}

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

// Per-thread state shared by the helpers below and the command loop.
vec2 sample_position, tile_low, tile_high;
float reach, minimum_distance_squared, stroke_distance_squared;
int winding;

// What a segment or curve whose control points span [low, high] can change for any sample of this group's tile:
// 0 nothing, 1 only the winding, 2 the distance too. A distance beyond `reach` (one pixel past the stroke's half
// width, in em) saturates the coverage whatever its value, and a crossing needs the box to overlap the sample rows
// and reach the sample or lie to its right.
uint relevance(vec2 low, vec2 high) {
    if (tile_low.y - high.y > reach || low.y - tile_high.y > reach || tile_low.x - high.x > reach)
        return 0u;
    return low.x - tile_high.x > reach ? 1u : 2u;
}

// The winding of a filled segment or curve from `start` to `end` that lies right of every sample of the tile: each of
// its segments that spans a sample's row crosses it right of the sample, so the crossings add up to the two ends'
// sides of the row (the last cubic weight is exactly the end point).
void accumulate_right(vec2 start, vec2 end) {
    winding += int(end.y > sample_position.y) - int(start.y > sample_position.y);
}

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
    bool stroking = stroke_half_width > 0.0;
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

    sample_position = outline_origin + (vec2(id.xy - allocation_padding) + 0.5) / pixels_per_em;
    winding = 0;
    // Infinity keeps the reduction valid even when sample coordinates are large.
    minimum_distance_squared = uintBitsToFloat(0x7f800000u);
    stroke_distance_squared = uintBitsToFloat(0x7f800000u);
    // Sample positions grow monotonically with the pixel, so these bound every sample of the group; the reach absorbs
    // rounding. A stroke's coverage ends half a pixel past its half-width.
    ivec2 first_pixel = ivec2(gl_WorkGroupID.xy * 8u) - int(allocation_padding);
    tile_low = outline_origin + (vec2(first_pixel) + 0.5) / pixels_per_em;
    tile_high = outline_origin + (vec2(first_pixel + 7) + 0.5) / pixels_per_em;
    reach = (1.0 + stroke_half_width) / pixels_per_em;
    vec2 current = vec2(0.0);
    bool contour_filled = false;

    // Contours arrive closed: GlyphRasterBatch::add uploads each end_closed as a line back to its contour's first
    // point, and end markers draw nothing.
    for (uint index = 0u; index < command_count; ++index) {
        GlyphPathCommand command = load_command(uint(GLYPH_JOBS) + (raster.w & 65535u) + index);
        // Lines and curves share one path (a fifth faster on AMD than a branch of each): a line is a curve's last
        // segment alone, whose weights give the end point exactly.
        if (command.kind == 2u || command.kind == 3u) {
            bool curve = command.kind == 3u;
            vec2 control = curve ? command.point2 : command.point1;
            vec2 end = curve ? command.point3 : command.point1;
            uint relevant = relevance(min(min(current, command.point1), min(control, end)),
                                      max(max(current, command.point1), max(control, end)));
            if (relevant == 2u) {
                // Uniform per command: a loop of each (2.7 % faster than one loop that branches per segment), so a
                // filled contour computes no stroke distance.
                vec2 segment_start = current;
                if (contour_filled) {
                    for (uint segment = curve ? 0u : 11u; segment < 12u; ++segment) {
                        vec4 weights = cubic_weights[segment];
                        vec2 segment_end = weights.x * current + weights.y * command.point1 + weights.z * control +
                                           weights.w * end;
                        accumulate_distance(sample_position, segment_start, segment_end, minimum_distance_squared);
                        accumulate_crossing(sample_position, segment_start, segment_end, winding);
                        segment_start = segment_end;
                    }
                } else if (stroking) {
                    for (uint segment = curve ? 0u : 11u; segment < 12u; ++segment) {
                        vec4 weights = cubic_weights[segment];
                        vec2 segment_end = weights.x * current + weights.y * command.point1 + weights.z * control +
                                           weights.w * end;
                        accumulate_stroke(sample_position, segment_start, segment_end, stroke_distance_squared);
                        segment_start = segment_end;
                    }
                }
            } else if (relevant != 0u && contour_filled) {
                accumulate_right(current, end);
            }
            current = end;
        } else if (command.kind <= 1u) {
            current = command.point1;
            contour_filled = command.kind == 0u;
        }
    }

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
