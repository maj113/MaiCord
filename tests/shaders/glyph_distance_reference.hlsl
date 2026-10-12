struct GlyphPathCommand {
    float2 point1;
    float2 point2;
    float2 point3;
    uint kind;
};

cbuffer GlyphRasterConstants : register(b0) {
    uint2 destination_origin;
    uint2 destination_size;
    uint command_count;
    uint fill_rule;
    float pixels_per_em;
    uint allocation_padding;
    float2 outline_origin;
    float2 reserved1;
};

StructuredBuffer<GlyphPathCommand> path_commands : register(t0);
RWTexture2D<unorm float> coverage_atlas : register(u0);

void accumulate_line(float2 sample_position, float2 line_start, float2 line_end,
                     inout int winding, inout float minimum_distance) {
    const float2 delta = line_end - line_start;
    const float length_squared = dot(delta, delta);
    const float projection = length_squared > 1.0e-12f
                                 ? saturate(dot(sample_position - line_start, delta) /
                                            length_squared)
                                 : 0.0f;
    minimum_distance = min(minimum_distance,
                           length(sample_position - (line_start + delta * projection)) *
                               pixels_per_em);

    const bool crosses = (line_start.y > sample_position.y) !=
                         (line_end.y > sample_position.y);
    if (crosses) {
        const float crossing_x = line_start.x +
            (sample_position.y - line_start.y) * (line_end.x - line_start.x) /
            (line_end.y - line_start.y);
        if (crossing_x > sample_position.x) {
            winding += line_end.y > line_start.y ? 1 : -1;
        }
    }
}

float2 cubic_point(float2 curve_start, float2 control1, float2 control2, float2 curve_end,
                   float amount) {
    const float inverse = 1.0f - amount;
    return inverse * inverse * inverse * curve_start +
           3.0f * inverse * inverse * amount * control1 +
           3.0f * inverse * amount * amount * control2 +
           amount * amount * amount * curve_end;
}

[numthreads(8, 8, 1)]
void glyph_coverage(uint3 dispatch_id : SV_DispatchThreadID) {
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

    const float2 sample_position =
        outline_origin + (float2(dispatch_id.xy - allocation_padding) + 0.5f) / pixels_per_em;
    float2 current = 0.0f;
    float2 contour_start = 0.0f;
    int winding = 0;
    float minimum_distance = 1.0e20f;
    bool contour_filled = false;

    [loop]
    for (uint index = 0; index < command_count; ++index) {
        const GlyphPathCommand command = path_commands[index];
        if (command.kind == 0 || command.kind == 1) {
            current = command.point1;
            contour_start = current;
            contour_filled = command.kind == 0;
        } else if (command.kind == 2) {
            if (contour_filled) {
                accumulate_line(sample_position, current, command.point1, winding,
                                minimum_distance);
            }
            current = command.point1;
        } else if (command.kind == 3) {
            float2 segment_start = current;
            [unroll]
            for (uint segment = 1; segment <= 12; ++segment) {
                const float amount = (float)segment / 12.0f;
                const float2 segment_end = cubic_point(
                    current, command.point1, command.point2, command.point3, amount);
                if (contour_filled) {
                    accumulate_line(sample_position, segment_start, segment_end, winding,
                                    minimum_distance);
                }
                segment_start = segment_end;
            }
            current = command.point3;
        } else if (command.kind == 5) {
            if (contour_filled) {
                accumulate_line(sample_position, current, contour_start, winding,
                                minimum_distance);
            }
            current = contour_start;
        }
    }

    const bool inside = fill_rule == 0 ? ((abs(winding) & 1) != 0) : winding != 0;
    const float signed_distance = inside ? -minimum_distance : minimum_distance;
    const float coverage = saturate(0.5f - signed_distance);
    coverage_atlas[destination_origin + dispatch_id.xy] = coverage;
}
