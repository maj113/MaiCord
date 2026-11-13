struct GlyphPathCommand {
    float2 point1;
    float2 point2;
    float2 point3;
    uint kind;
};

ByteAddressBuffer raster_data : register(t0);
RWTexture2D<unorm float> coverage_atlas : register(u0);

// GLYPH_JOBS is the job count a dispatch carries: the job headers fill the first
// GLYPH_JOBS records of the raster data (glyph_raster_batch_limit in C++). A
// dispatch is latency-bound, not throughput-bound, so more jobs per dispatch
// cost almost nothing.
#ifndef GLYPH_JOBS
#define GLYPH_JOBS 16
#endif

// Every raster is culled by tile: a thread group skips each command that cannot
// reach any sample of its 8 x 8 tile, and takes only the winding of one that lies
// right of it. Measured on the UI's own rasters, that pays at every size.
// A crossing right of the sample without a division: the side test's sign
// against the edge direction replaces the crossing abscissa (as colr_paint does).
// Across the sample row the end and start comparisons differ, and their
// difference is the edge's direction; elsewhere it is zero.
void accumulate_crossing(float2 sample_position, float2 line_start, float2 line_end, inout int winding) {
    const float2 delta = line_end - line_start, offset = sample_position - line_start;
    const float side = delta.x * offset.y - offset.x * delta.y;
    winding += side * delta.y > 0.0f ? int(line_end.y > sample_position.y) - int(line_start.y > sample_position.y) : 0;
}

void accumulate_distance(float2 sample_position, float2 line_start, float2 line_end,
                         inout float minimum_distance_squared) {
    const float2 delta = line_end - line_start;
    const float length_squared = dot(delta, delta);
    const float projection = length_squared > 1.0e-12f
                                 ? saturate(dot(sample_position - line_start, delta) /
                                            length_squared)
                                 : 0.0f;
    const float2 distance_vector = sample_position - (line_start + delta * projection);
    minimum_distance_squared = min(minimum_distance_squared, dot(distance_vector, distance_vector));
}

// Stroked (icon) contours contribute only their centreline distance.
void accumulate_stroke(float2 sample_position, float2 line_start, float2 line_end,
                       inout float minimum_distance_squared) {
    const float2 delta = line_end - line_start;
    // A degenerate segment is a dot; the clamp keeps it at line_start.
    const float projection =
        saturate(dot(sample_position - line_start, delta) / max(dot(delta, delta), 1.0e-12f));
    const float2 distance_vector = sample_position - (line_start + delta * projection);
    minimum_distance_squared = min(minimum_distance_squared, dot(distance_vector, distance_vector));
}

// Cubic Bezier weights at t = k/12 (k = 1..12), rounded after each binary32
// operation; verify with tools/verify_glyph_cubic_weights.py. FXC folds decimal
// constant expressions differently, so the bits are spelled out.
static const float4 cubic_weights[12] = {
    asfloat(uint4(0x3f452f69u, 0x3e571c73u, 0x3c9c71c8u, 0x3a17b427u)),
    asfloat(uint4(0x3f1425ecu, 0x3eb1c71cu, 0x3d8e38e4u, 0x3b97b427u)),
    asfloat(uint4(0x3ed80000u, 0x3ed80000u, 0x3e100000u, 0x3c800000u)),
    asfloat(uint4(0x3e97b424u, 0x3ee38e37u, 0x3e638e38u, 0x3d17b427u)),
    asfloat(uint4(0x3e4b4261u, 0x3ed9c71eu, 0x3e9b8e39u, 0x3d9425ecu)),
    asfloat(uint4(0x3e000000u, 0x3ec00000u, 0x3ec00000u, 0x3e000000u)),
    asfloat(uint4(0x3d9425efu, 0x3e9b8e39u, 0x3ed9c71bu, 0x3e4b425eu)),
    asfloat(uint4(0x3d17b424u, 0x3e638e37u, 0x3ee38e38u, 0x3e97b427u)),
    asfloat(uint4(0x3c800000u, 0x3e100000u, 0x3ed80000u, 0x3ed80000u)),
    asfloat(uint4(0x3b97b429u, 0x3d8e38e5u, 0x3eb1c71eu, 0x3f1425ecu)),
    asfloat(uint4(0x3a17b41fu, 0x3c9c71c2u, 0x3e571c6fu, 0x3f452f69u)),
    asfloat(uint4(0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u))
};

// Per-thread state shared by the helpers below and the command loop.
static float2 sample_position, tile_low, tile_high;
static float reach, minimum_distance_squared, stroke_distance_squared;
static int winding;

// What a segment or curve whose control points span [low, high] can change for
// any sample of this group's tile: 0 nothing, 1 only the winding, 2 the
// distance too. A distance beyond `reach` (one pixel past the stroke's half
// width, in em) saturates the coverage whatever its value, and a crossing needs
// the box to overlap the sample rows and reach the sample or lie to its right.
uint relevance(float2 low, float2 high) {
    if (tile_low.y - high.y > reach || low.y - tile_high.y > reach || tile_low.x - high.x > reach) {
        return 0;
    }
    return low.x - tile_high.x > reach ? 1u : 2u;
}

// The winding of a filled segment or curve from `start` to `end` that lies right
// of every sample of the tile: each of its segments that spans a sample's row
// crosses it right of the sample, so the crossings add up to the two ends' sides
// of the row (the last cubic weight is exactly the end point).
void accumulate_right(float2 start, float2 end) {
    winding += int(end.y > sample_position.y) - int(start.y > sample_position.y);
}

[numthreads(8, 8, 1)]
void glyph_coverage(uint3 dispatch_id : SV_DispatchThreadID, uint3 group_id : SV_GroupID) {
    const uint offset=dispatch_id.z*28;
    const uint4 raster=raster_data.Load4(offset);
    const float3 values=asfloat(raster_data.Load3(offset+16));
    const uint2 destination_origin=uint2(raster.x&65535,raster.x>>16),destination_size=uint2(raster.y&65535,raster.y>>16);
    const uint command_count=raster.z&65535,fill_rule=(raster.z>>16)&1,allocation_padding=raster.w>>16;
    // Stroke half-width in 1/256 pixels; zero (all text) leaves coverage unchanged.
    const float stroke_half_width=(raster.z>>17)/256.0f;
    const bool stroking=stroke_half_width>0.0f;
    const float pixels_per_em=values.x;
    const float2 outline_origin=values.yz;
    if (dispatch_id.x >= destination_size.x || dispatch_id.y >= destination_size.y) {
        return;
    }

    // Linear filtering can reach outside the glyph's UV rectangle. Initialize
    // the allocator-owned gutter on every raster, including recycled shelves.
    if (any(dispatch_id.xy < allocation_padding) ||
        any(dispatch_id.xy >= destination_size - allocation_padding)) {
        coverage_atlas[destination_origin + dispatch_id.xy] = 0.0f;
        return;
    }

    sample_position =
        outline_origin + (float2(dispatch_id.xy - allocation_padding) + 0.5f) / pixels_per_em;
    winding = 0;
    // Infinity keeps the reduction valid even when sample coordinates are large.
    minimum_distance_squared = asfloat(0x7f800000u);
    stroke_distance_squared = asfloat(0x7f800000u);
    // Sample positions grow monotonically with the pixel, so these bound every
    // sample of the group; the reach absorbs rounding. A stroke's coverage ends
    // half a pixel past its half-width.
    const int2 first_pixel = int2(group_id.xy * 8) - int(allocation_padding);
    tile_low = outline_origin + (float2(first_pixel) + 0.5f) / pixels_per_em;
    tile_high = outline_origin + (float2(first_pixel + 7) + 0.5f) / pixels_per_em;
    reach = (1.0f + stroke_half_width) / pixels_per_em;
    float2 current = 0.0f;
    bool contour_filled = false;

    // Contours arrive closed: GlyphRasterBatch::add uploads each end_closed as a
    // line back to its contour's first point, and end markers draw nothing.
    [loop]
    for (uint index = 0; index < command_count; ++index) {
        const uint at=28*(GLYPH_JOBS+(raster.w&65535)+index);
        const uint4 first=raster_data.Load4(at);
        const uint3 last=raster_data.Load3(at+16);
        GlyphPathCommand command;
        command.point1=asfloat(first.xy);command.point2=asfloat(first.zw);
        command.point3=asfloat(last.xy);command.kind=last.z;
        // Lines and curves share one path (a fifth faster on AMD than a branch of each):
        // a line is a curve's last segment alone, whose weights give the end point exactly.
        if (command.kind == 2 || command.kind == 3) {
            const bool curve = command.kind == 3;
            const float2 control = curve ? command.point2 : command.point1;
            const float2 end = curve ? command.point3 : command.point1;
            const uint relevant = relevance(min(min(current, command.point1), min(control, end)),
                                            max(max(current, command.point1), max(control, end)));
            [branch] if (relevant == 2) {
                // Uniform per command: a loop of each (2.7 % faster than one loop that
                // branches per segment), so a filled contour computes no stroke distance.
                float2 segment_start = current;
                [branch] if (contour_filled) {
                    [loop]
                    for (uint segment = curve ? 0 : 11; segment < 12; ++segment) {
                        const float4 weights = cubic_weights[segment];
                        const float2 segment_end = weights.x * current + weights.y * command.point1 +
                                                   weights.z * control + weights.w * end;
                        accumulate_distance(sample_position, segment_start, segment_end, minimum_distance_squared);
                        accumulate_crossing(sample_position, segment_start, segment_end, winding);
                        segment_start = segment_end;
                    }
                } else if (stroking) {
                    [loop]
                    for (uint segment = curve ? 0 : 11; segment < 12; ++segment) {
                        const float4 weights = cubic_weights[segment];
                        const float2 segment_end = weights.x * current + weights.y * command.point1 +
                                                   weights.z * control + weights.w * end;
                        accumulate_stroke(sample_position, segment_start, segment_end, stroke_distance_squared);
                        segment_start = segment_end;
                    }
                }
            } else if (relevant && contour_filled) {
                accumulate_right(current, end);
            }
            current = end;
        } else if (command.kind <= 1) {
            current = command.point1;
            contour_filled = command.kind == 0;
        }
    }

    const bool inside = fill_rule == 0 ? ((abs(winding) & 1) != 0) : winding != 0;
    // Square root and the positive pixel scale are monotonic, so they apply
    // once to the minimum rather than per segment.
    const float distance = sqrt(minimum_distance_squared) * pixels_per_em;
    const float signed_distance = inside ? -distance : distance;
    float coverage = saturate(0.5f - signed_distance);
    if (stroking) {
        // Filled contours are stroked too, giving round joins on solid shapes.
        const float stroke_distance =
            sqrt(min(minimum_distance_squared, stroke_distance_squared)) * pixels_per_em;
        coverage = max(coverage, saturate(0.5f + stroke_half_width - stroke_distance));
    }
    coverage_atlas[destination_origin + dispatch_id.xy] = coverage;
}
