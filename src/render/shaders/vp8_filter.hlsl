cbuffer Vp8FilterConstants : register(b0) {
    uint macroblock_columns;
    uint macroblock_rows;
    uint luma_stride;
    uint chroma_stride;
    uint u_plane_offset;
    uint v_plane_offset;
    uint filter_level;
    uint sharpness;
    uint simple_filter;
    uint segmentation_enabled;
    uint segment_absolute_delta;
    uint use_filter_delta;
    int segment_filter0;
    int segment_filter1;
    int segment_filter2;
    int segment_filter3;
    int reference_delta0;
    int mode_delta0;
    uint mode_record_bytes;
    uint coefficient_record_bytes;
};

ByteAddressBuffer vp8_modes : register(t0);
ByteAddressBuffer vp8_coefficients : register(t1);
RWByteAddressBuffer vp8_yuv : register(u0);

uint load_mode_byte(uint address) {
    const uint packed = vp8_modes.Load(address & ~3u);
    return (packed >> ((address & 3u) * 8u)) & 255u;
}

uint load_coefficient_byte(uint address) {
    const uint packed = vp8_coefficients.Load(address & ~3u);
    return (packed >> ((address & 3u) * 8u)) & 255u;
}

uint sample_address(uint plane, uint stride, int x, int y) {
    return (plane + uint(y) * stride + uint(x)) * 4u;
}

int load_filter_sample(uint plane, uint stride, int x, int y) {
    return int(vp8_yuv.Load(sample_address(plane, stride, x, y)));
}

void store_filter_sample(uint plane, uint stride, int x, int y,
                         int value) {
    vp8_yuv.Store(sample_address(plane, stride, x, y),
                  uint(clamp(value, 0, 255)));
}

int signed_clip1(int value) {
    return clamp(value, -128, 127);
}

int signed_clip2(int value) {
    return clamp(value, -16, 15);
}

void filter_segment(uint plane, uint stride, int edge_x, int edge_y,
                    int step_x, int step_y, int edge_limit,
                    int interior_limit, int hev_threshold,
                    bool macroblock_edge, bool simple) {
    const int p3 = load_filter_sample(
        plane, stride, edge_x - 4 * step_x, edge_y - 4 * step_y);
    const int p2 = load_filter_sample(
        plane, stride, edge_x - 3 * step_x, edge_y - 3 * step_y);
    const int p1 = load_filter_sample(
        plane, stride, edge_x - 2 * step_x, edge_y - 2 * step_y);
    const int p0 = load_filter_sample(
        plane, stride, edge_x - step_x, edge_y - step_y);
    const int q0 = load_filter_sample(plane, stride, edge_x, edge_y);
    const int q1 = load_filter_sample(
        plane, stride, edge_x + step_x, edge_y + step_y);
    const int q2 = load_filter_sample(
        plane, stride, edge_x + 2 * step_x, edge_y + 2 * step_y);
    const int q3 = load_filter_sample(
        plane, stride, edge_x + 3 * step_x, edge_y + 3 * step_y);
    const int threshold = 2 * edge_limit + 1;
    if (4 * abs(p0 - q0) + abs(p1 - q1) > threshold) {
        return;
    }

    if (simple) {
        const int adjustment =
            3 * (q0 - p0) + signed_clip1(p1 - q1);
        const int adjustment1 = signed_clip2((adjustment + 4) >> 3);
        const int adjustment2 = signed_clip2((adjustment + 3) >> 3);
        store_filter_sample(plane, stride, edge_x - step_x,
                            edge_y - step_y, p0 + adjustment2);
        store_filter_sample(plane, stride, edge_x, edge_y,
                            q0 - adjustment1);
        return;
    }

    if (abs(p3 - p2) > interior_limit ||
        abs(p2 - p1) > interior_limit ||
        abs(p1 - p0) > interior_limit ||
        abs(q3 - q2) > interior_limit ||
        abs(q2 - q1) > interior_limit ||
        abs(q1 - q0) > interior_limit) {
        return;
    }
    const bool high_edge_variance =
        abs(p1 - p0) > hev_threshold ||
        abs(q1 - q0) > hev_threshold;
    if (high_edge_variance) {
        const int adjustment =
            3 * (q0 - p0) + signed_clip1(p1 - q1);
        const int adjustment1 = signed_clip2((adjustment + 4) >> 3);
        const int adjustment2 = signed_clip2((adjustment + 3) >> 3);
        store_filter_sample(plane, stride, edge_x - step_x,
                            edge_y - step_y, p0 + adjustment2);
        store_filter_sample(plane, stride, edge_x, edge_y,
                            q0 - adjustment1);
    } else if (macroblock_edge) {
        const int adjustment = signed_clip1(
            3 * (q0 - p0) + signed_clip1(p1 - q1));
        const int adjustment1 = (27 * adjustment + 63) >> 7;
        const int adjustment2 = (18 * adjustment + 63) >> 7;
        const int adjustment3 = (9 * adjustment + 63) >> 7;
        store_filter_sample(plane, stride, edge_x - 3 * step_x,
                            edge_y - 3 * step_y, p2 + adjustment3);
        store_filter_sample(plane, stride, edge_x - 2 * step_x,
                            edge_y - 2 * step_y, p1 + adjustment2);
        store_filter_sample(plane, stride, edge_x - step_x,
                            edge_y - step_y, p0 + adjustment1);
        store_filter_sample(plane, stride, edge_x, edge_y,
                            q0 - adjustment1);
        store_filter_sample(plane, stride, edge_x + step_x,
                            edge_y + step_y, q1 - adjustment2);
        store_filter_sample(plane, stride, edge_x + 2 * step_x,
                            edge_y + 2 * step_y, q2 - adjustment3);
    } else {
        const int adjustment = 3 * (q0 - p0);
        const int adjustment1 = signed_clip2((adjustment + 4) >> 3);
        const int adjustment2 = signed_clip2((adjustment + 3) >> 3);
        const int adjustment3 = (adjustment1 + 1) >> 1;
        store_filter_sample(plane, stride, edge_x - 2 * step_x,
                            edge_y - 2 * step_y, p1 + adjustment3);
        store_filter_sample(plane, stride, edge_x - step_x,
                            edge_y - step_y, p0 + adjustment2);
        store_filter_sample(plane, stride, edge_x, edge_y,
                            q0 - adjustment1);
        store_filter_sample(plane, stride, edge_x + step_x,
                            edge_y + step_y, q1 - adjustment3);
    }
}

int segment_filter(uint segment) {
    return segment == 0u ? segment_filter0
         : segment == 1u ? segment_filter1
         : segment == 2u ? segment_filter2
                         : segment_filter3;
}

[numthreads(16, 16, 1)]
void vp8_filter(uint3 group_thread_id : SV_GroupThreadID) {
    const uint lane = group_thread_id.x;
    // Each cohort owns one independent macroblock on rank x + 2*y.
    const uint rank_count = macroblock_columns + 2u * macroblock_rows - 2u;
    for (uint rank = 0u; rank < rank_count; ++rank) {
        const uint first_row = rank < macroblock_columns ? 0u :
            (rank - (macroblock_columns - 1u) + 1u) / 2u;
        const uint last_row = min(macroblock_rows - 1u, rank / 2u);
        if (first_row > last_row) continue; // Uniform empty rank.
        const uint row = first_row + group_thread_id.y;
        const bool valid = row <= last_row;
        const uint macroblock_y = valid ? row : 0u;
        const uint macroblock_x = valid ? rank - 2u * row : 0u;
        const uint macroblock = macroblock_y * macroblock_columns + macroblock_x;
        const uint mode_address = macroblock * mode_record_bytes;
        uint luma_mode = 0u, segment = 0u, has_coefficients = 0u;
        if (valid) {
            luma_mode = load_mode_byte(mode_address + 16u);
            segment = load_mode_byte(mode_address + 18u);
            has_coefficients = load_coefficient_byte(
                macroblock * coefficient_record_bytes + 768u);
        }
        int level = int(filter_level);
        if (segmentation_enabled != 0u) {
            level = segment_absolute_delta != 0u
                        ? segment_filter(segment)
                        : level + segment_filter(segment);
        }
        if (use_filter_delta != 0u) {
            level += reference_delta0;
            if (luma_mode == 10u) {
                level += mode_delta0;
            }
        }
        level = clamp(level, 0, 63);
        int interior_limit = level;
        if (sharpness != 0u) {
            interior_limit >>= sharpness > 4u ? 2 : 1;
            interior_limit = min(interior_limit, 9 - int(sharpness));
        }
        interior_limit = max(interior_limit, 1);
        const int subblock_limit = 2 * level + interior_limit;
        const int macroblock_limit = subblock_limit + 4;
        const int hev_threshold = level >= 40 ? 2 : level >= 15 ? 1 : 0;
        const bool inner_edges = luma_mode == 10u ||
                                 has_coefficients != 0u;
        const uint luma_x = macroblock_x * 16u;
        const uint luma_y = macroblock_y * 16u;
        const uint chroma_x = macroblock_x * 8u;
        const uint chroma_y = macroblock_y * 8u;

        // Eight edge passes in order: the macroblock edge then three inner
        // edges between columns, then the same between rows. Each pass reads
        // what the previous one wrote, so a barrier follows every pass.
        [loop] for (uint pass_index = 0u; pass_index < 8u; ++pass_index) {
            const bool across = pass_index < 4u;
            const uint edge_index = pass_index & 3u;
            const bool macroblock_edge = edge_index == 0u;
            const bool wanted = macroblock_edge ? (across ? macroblock_x : macroblock_y) != 0u
                                                : inner_edges;
            if (valid && level != 0 && wanted) {
                const int limit = macroblock_edge ? macroblock_limit : subblock_limit;
                // Luma, then (outside the simple filter, on the first two
                // edges, for eight lanes) the U and V planes.
                [loop] for (uint plane = 0u; plane < 3u; ++plane) {
                    if (plane != 0u && (simple_filter != 0u || lane >= 8u || edge_index >= 2u))
                        break;
                    const uint offset = plane == 0u ? 0u : plane == 1u ? u_plane_offset : v_plane_offset;
                    const uint stride = plane == 0u ? luma_stride : chroma_stride;
                    const uint origin_x = plane == 0u ? luma_x : chroma_x;
                    const uint origin_y = plane == 0u ? luma_y : chroma_y;
                    const uint along = edge_index * 4u;
                    filter_segment(offset, stride,
                                   int(across ? origin_x + along : origin_x + lane),
                                   int(across ? origin_y + lane : origin_y + along),
                                   across ? 1 : 0, across ? 0 : 1, limit, interior_limit,
                                   hev_threshold, macroblock_edge, simple_filter != 0u);
                }
            }
            AllMemoryBarrierWithGroupSync();
        }
    }
}
