cbuffer Vp8ReconstructionConstants : register(b0) {
    uint image_width;
    uint image_height;
    uint macroblock_columns;
    uint macroblock_rows;
    uint final_wave_rank;
    uint luma_stride;
    uint luma_rows;
    uint chroma_stride;
    uint chroma_rows;
    uint u_plane_offset;
    uint v_plane_offset;
    uint mode_record_bytes;
    uint coefficient_record_bytes;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

ByteAddressBuffer vp8_modes : register(t0);
ByteAddressBuffer vp8_coefficients : register(t1);
RWByteAddressBuffer vp8_yuv : register(u0);
static uint pixel_lane;
groupshared uint luma_edges[32 * 37];

uint load_byte(ByteAddressBuffer source, uint address) {
    const uint packed = source.Load(address & ~3u);
    return (packed >> ((address & 3u) * 8u)) & 255u;
}

int load_coefficient(uint macroblock, uint block, uint coefficient) {
    const uint address = macroblock * coefficient_record_bytes +
                         (block * 16u + coefficient) * 2u;
    const uint packed = vp8_coefficients.Load(address & ~3u);
    const uint shift = (address & 2u) * 8u;
    const uint value = (packed >> shift) & 65535u;
    return value >= 32768u ? int(value) - 65536 : int(value);
}

uint load_sample(uint plane, uint stride, int x, int y) {
    if (y < 0) {
        return 127u;
    }
    if (x < 0) {
        return 129u;
    }
    const uint bounded_x = min(uint(x), stride - 1u);
    return vp8_yuv.Load((plane + uint(y) * stride + bounded_x) * 4u);
}

void store_sample(uint plane, uint stride, uint x, uint y, uint value) {
    vp8_yuv.Store((plane + y * stride + x) * 4u, value);
}

uint average2(uint a, uint b) {
    return (a + b + 1u) >> 1u;
}

uint average3(uint a, uint b, uint c) {
    return (a + 2u * b + c + 2u) >> 2u;
}

uint true_motion(uint left, uint top, uint top_left) {
    return uint(clamp(int(left) + int(top) - int(top_left), 0, 255));
}

void predict_plane(uint plane, uint stride, uint macroblock_x,
                   uint macroblock_y, uint extent, uint mode) {
    const uint origin_x = macroblock_x * extent;
    const uint origin_y = macroblock_y * extent;
    uint dc = 128u;
    if (mode == 0u) {
        const bool has_top = macroblock_y != 0u;
        const bool has_left = macroblock_x != 0u;
        uint sum = has_top && has_left ? extent : extent >> 1u;
        if (has_top) {
            for (uint top_index = 0u; top_index < extent; ++top_index) {
                sum += load_sample(plane, stride, int(origin_x + top_index),
                                   int(origin_y) - 1);
            }
        }
        if (has_left) {
            for (uint left_index = 0u; left_index < extent; ++left_index) {
                sum += load_sample(plane, stride, int(origin_x) - 1,
                                   int(origin_y + left_index));
            }
        }
        if (has_left && has_top) {
            dc = sum >> (extent == 16u ? 5u : 4u);
        } else if (has_left || has_top) {
            dc = sum >> (extent == 16u ? 4u : 3u);
        }
    }

    const uint top_left = load_sample(
        plane, stride, int(origin_x) - 1, int(origin_y) - 1);
    for (uint y = 0u; y < extent; ++y) {
        const uint left = load_sample(
            plane, stride, int(origin_x) - 1, int(origin_y + y));
        for (uint x = pixel_lane; x < extent; x += 4u) {
            const uint top = load_sample(
                plane, stride, int(origin_x + x), int(origin_y) - 1);
            uint prediction = dc;
            if (mode == 1u) {
                prediction = true_motion(left, top, top_left);
            } else if (mode == 2u) {
                prediction = top;
            } else if (mode == 3u) {
                prediction = left;
            }
            store_sample(plane, stride, origin_x + x, origin_y + y,
                         prediction);
        }
    }
}

uint top_neighbor(uint index, uint4 top, uint4 right) {
    return index < 4u ? top[min(index, 3u)] : right[min(index - 4u, 3u)];
}

uint neighbor(int position, uint4 top, uint4 right, uint4 left, uint corner) {
    if (position == 0) return corner;
    return position > 0 ? top_neighbor(uint(max(position - 1, 0)), top, right)
                        : left[uint(clamp(-position - 1, 0, 3))];
}

uint directional(int delta, uint4 top, uint4 right, uint4 left, uint corner) {
    const int offset = int(uint(max(delta, 0)) >> 1u);
    const uint forward = (delta & 1) == 0
        ? average2(neighbor(offset, top, right, left, corner), neighbor(offset + 1, top, right, left, corner))
        : average3(neighbor(offset, top, right, left, corner), neighbor(offset + 1, top, right, left, corner),
                   neighbor(offset + 2, top, right, left, corner));
    const uint backward = average3(neighbor(delta, top, right, left, corner),
        neighbor(delta + 1, top, right, left, corner), neighbor(delta + 2, top, right, left, corner));
    return delta < 0 ? backward : forward;
}

uint4 predict_luma4(uint mode, uint4 top, uint4 right, uint4 left, uint corner) {
    const uint x = pixel_lane & 3u;
    uint4 column = 0u;
    [loop] for (uint y = 0u; y < 4u; ++y) {
        uint value = 0u;
        if (mode == 0u) value = (4u + top.x + top.y + top.z + top.w +
                                left.x + left.y + left.z + left.w) >> 3u;
        else if (mode == 1u) value = true_motion(left[y], top[x], corner);
        else if (mode == 2u) value = average3(x == 0u ? corner : top[max(x, 1u) - 1u],
            top[x], top_neighbor(x + 1u, top, right));
        else if (mode == 3u) value = average3(y == 0u ? corner : left[max(y, 1u) - 1u],
            left[y], left[min(y + 1u, 3u)]);
        else if (mode == 4u) {
            const int delta = int(x) - int(y);
            value = average3(neighbor(delta - 1, top, right, left, corner),
                neighbor(delta, top, right, left, corner), neighbor(delta + 1, top, right, left, corner));
        } else if (mode == 5u) value = directional(2 * int(x) - int(y), top, right, left, corner);
        else if (mode == 6u) {
            const uint offset = x + y;
            value = average3(top_neighbor(offset, top, right), top_neighbor(offset + 1u, top, right),
                top_neighbor(offset + 2u, top, right));
        } else if (mode == 7u) {
            const uint offset = x + (y >> 1u);
            value = x == 3u && y >= 2u ? (y == 2u ? average3(right.x, right.y, right.z) : average3(right.y, right.z, right.w)) :
                (y & 1u) == 0u ? average2(top_neighbor(offset, top, right), top_neighbor(offset + 1u, top, right)) :
                average3(top_neighbor(offset, top, right), top_neighbor(offset + 1u, top, right),
                    top_neighbor(offset + 2u, top, right));
        } else if (mode == 8u) {
            value = directional(2 * int(y) - int(x), left, left.wwww, top, corner);
        } else {
            const uint offset = y + (x >> 1u);
            value = (x & 1u) == 0u ? average2(left[min(offset, 3u)], left[min(offset + 1u, 3u)]) :
                average3(left[min(offset, 3u)], left[min(offset + 1u, 3u)], left[min(offset + 2u, 3u)]);
        }
        column = uint4(column.yzw, value);
    }
    return column;
}

int multiply_shift16(int value, uint factor) {
    const uint magnitude = value < 0 ? uint(-value) : uint(value);
    const uint low_product = (magnitude & 65535u) * factor;
    uint quotient = (magnitude >> 16u) * factor +
                    (low_product >> 16u);
    if (value < 0 && (low_product & 65535u) != 0u) {
        ++quotient;
    }
    return value < 0 ? -int(quotient) : int(quotient);
}

int multiply1(int value) {
    return value + multiply_shift16(value, 20091u);
}

int multiply2(int value) {
    return multiply_shift16(value, 35468u);
}

uint residual_address(uint macroblock, uint block) {
    return (v_plane_offset + chroma_stride * chroma_rows + macroblock * 384u + block * 16u) * 4u;
}

// Inverse transforms have no prediction dependencies. Dispatch them across
// the GPU before the much smaller dependency-ordered prediction pass.
[numthreads(64, 1, 1)]
void vp8_inverse_dct(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint macroblock = dispatch_id.x / 24u;
    const uint block = dispatch_id.x % 24u;
    if (macroblock >= macroblock_columns * macroblock_rows) return;
    int input[16];
    int temporary[16];
    [unroll] for (uint index = 0u; index < 16u; ++index) {
        input[index] = load_coefficient(macroblock, block, index);
    }
    [unroll] for (uint column = 0u; column < 4u; ++column) {
        const int a = input[column] + input[8u + column];
        const int b = input[column] - input[8u + column];
        const int c = multiply2(input[4u + column]) -
                      multiply1(input[12u + column]);
        const int d = multiply1(input[4u + column]) +
                      multiply2(input[12u + column]);
        temporary[column * 4u + 0u] = a + d;
        temporary[column * 4u + 1u] = b + c;
        temporary[column * 4u + 2u] = b - c;
        temporary[column * 4u + 3u] = a - d;
    }
    [unroll] for (uint y = 0u; y < 4u; ++y) {
        const int dc = temporary[y] + 4;
        const int a = dc + temporary[8u + y];
        const int b = dc - temporary[8u + y];
        const int c = multiply2(temporary[4u + y]) -
                      multiply1(temporary[12u + y]);
        const int d = multiply1(temporary[4u + y]) +
                      multiply2(temporary[12u + y]);
        vp8_yuv.Store4(residual_address(macroblock, block) + y * 16u,
            asuint(int4(a + d, b + c, b - c, a - d) >> 3));
    }
}

void add_inverse_dct(uint plane, uint stride, uint origin_x,
                     uint origin_y, uint macroblock, uint block) {
    [unroll] for (uint y = 0u; y < 4u; ++y) {
        const int4 residual = asint(vp8_yuv.Load4(residual_address(macroblock, block) + y * 16u));
        {
            const uint x = pixel_lane;
            const uint predicted = load_sample(
                plane, stride, int(origin_x + x), int(origin_y + y));
            const uint reconstructed = uint(clamp(
                int(predicted) + residual[x], 0, 255));
            store_sample(plane, stride, origin_x + x, origin_y + y,
                         reconstructed);
        }
    }
}

void reconstruct_macroblock(uint macroblock_x, uint macroblock_y, bool active) {
    const uint macroblock = active ? macroblock_y * macroblock_columns + macroblock_x : 0u;
    const uint mode_address = macroblock * mode_record_bytes;
    const uint luma_mode = load_byte(vp8_modes, mode_address + 16u);
    const uint chroma_mode = load_byte(vp8_modes, mode_address + 17u);
    const uint has_coefficients = load_byte(vp8_coefficients, macroblock * coefficient_record_bytes + 768u);
    const uint luma_origin_x = macroblock_x * 16u;
    const uint luma_origin_y = macroblock_y * 16u;
    const uint edges = macroblock_y * 37u;
    uint4 top_right = 0u;
    uint macroblock_corner = 0u;
    if (active) {
        if (luma_mode == 10u) {
            top_right = uint4(load_sample(0u, luma_stride, int(luma_origin_x + 16u), int(luma_origin_y) - 1),
                load_sample(0u, luma_stride, int(luma_origin_x + 17u), int(luma_origin_y) - 1),
                load_sample(0u, luma_stride, int(luma_origin_x + 18u), int(luma_origin_y) - 1),
                load_sample(0u, luma_stride, int(luma_origin_x + 19u), int(luma_origin_y) - 1));
            macroblock_corner = load_sample(0u, luma_stride, int(luma_origin_x) - 1, int(luma_origin_y) - 1);
            for (uint edge = pixel_lane; edge < 16u; edge += 4u) {
                luma_edges[edges + edge] = load_sample(0u, luma_stride, int(luma_origin_x + edge), int(luma_origin_y) - 1);
                luma_edges[edges + 16u + edge] = load_sample(0u, luma_stride, int(luma_origin_x) - 1, int(luma_origin_y + edge));
            }
        } else predict_plane(0u, luma_stride, macroblock_x, macroblock_y, 16u, luma_mode);
    }
    GroupMemoryBarrierWithGroupSync();
    // Keep only the prediction boundary on-chip. Independent transform
    // residuals are fused with prediction before their one final output store.
    // All rows, including inactive rows, participate in the group barriers.
    for (uint y = 0u; y < 4u; ++y) {
        for (uint x = 0u; x < 4u; ++x) {
            uint4 column = 0u;
            uint next_corner = 0u;
            if (active) {
                const uint block = y * 4u + x;
                if (luma_mode == 10u) {
                    const uint top_index = edges + x * 4u;
                    const uint left_index = edges + (x == 0u ? 16u + y * 4u : 32u);
                    const uint4 top = uint4(luma_edges[top_index], luma_edges[top_index + 1u],
                        luma_edges[top_index + 2u], luma_edges[top_index + 3u]);
                    const uint4 right = x == 3u ? top_right : uint4(luma_edges[top_index + 4u],
                        luma_edges[top_index + 5u], luma_edges[top_index + 6u], luma_edges[top_index + 7u]);
                    const uint4 left = uint4(luma_edges[left_index], luma_edges[left_index + 1u],
                        luma_edges[left_index + 2u], luma_edges[left_index + 3u]);
                    const uint corner = x != 0u ? luma_edges[edges + 36u] :
                        y == 0u ? macroblock_corner : luma_edges[edges + 16u + y * 4u - 1u];
                    next_corner = top.w;
                    column = predict_luma4(load_byte(vp8_modes, mode_address + block), top, right, left, corner);
                    [unroll] for (uint row = 0u; row < 4u; ++row) {
                        const int residual = has_coefficients == 0u ? 0 :
                            asint(vp8_yuv.Load(residual_address(macroblock, block) + row * 16u + pixel_lane * 4u));
                        column[row] = uint(clamp(int(column[row]) + residual, 0, 255));
                        store_sample(0u, luma_stride, luma_origin_x + x * 4u + pixel_lane,
                            luma_origin_y + y * 4u + row, column[row]);
                    }
                } else if (has_coefficients != 0u) add_inverse_dct(0u, luma_stride,
                    luma_origin_x + x * 4u, luma_origin_y + y * 4u, macroblock, block);
            }
            GroupMemoryBarrierWithGroupSync();
            if (active && luma_mode == 10u) {
                luma_edges[edges + x * 4u + pixel_lane] = column.w;
                if (pixel_lane == 3u) {
                    luma_edges[edges + 32u] = column.x;
                    luma_edges[edges + 33u] = column.y;
                    luma_edges[edges + 34u] = column.z;
                    luma_edges[edges + 35u] = column.w;
                    luma_edges[edges + 36u] = next_corner;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    if (active) {
        const uint chroma_origin_x = macroblock_x * 8u;
        const uint chroma_origin_y = macroblock_y * 8u;
        predict_plane(u_plane_offset, chroma_stride, macroblock_x, macroblock_y, 8u, chroma_mode);
        predict_plane(v_plane_offset, chroma_stride, macroblock_x, macroblock_y, 8u, chroma_mode);
        if (has_coefficients != 0u) {
            for (uint plane_index = 0u; plane_index < 2u; ++plane_index) {
                const uint plane = plane_index == 0u ? u_plane_offset : v_plane_offset;
                for (uint y = 0u; y < 2u; ++y)
                    for (uint x = 0u; x < 2u; ++x)
                        add_inverse_dct(plane, chroma_stride, chroma_origin_x + x * 4u,
                            chroma_origin_y + y * 4u, macroblock, 16u + plane_index * 4u + y * 2u + x);
            }
        }
    }
}

[numthreads(4, 32, 1)]
void vp8_reconstruct(uint3 group_thread_id : SV_GroupThreadID) {
    pixel_lane = group_thread_id.x;
    const uint macroblock_y = group_thread_id.y;
    for (uint rank = 0u; rank <= final_wave_rank; ++rank) {
        const uint macroblock_x = rank - macroblock_y * 2u;
        const bool active = macroblock_y < macroblock_rows && rank >= macroblock_y * 2u &&
                            macroblock_x < macroblock_columns;
        reconstruct_macroblock(macroblock_x, macroblock_y, active);
        AllMemoryBarrierWithGroupSync();
    }
}
