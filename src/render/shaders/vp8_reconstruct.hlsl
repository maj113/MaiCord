// Residual inverse DCT pass (vp8_inverse_dct) for vp8_reconstruct_tiled.hlsl.
// The single-group vp8_reconstruct entry point in this file is not built.
#ifndef MAICORD_VP8_AC3_FAST_PATH
#define MAICORD_VP8_AC3_FAST_PATH 0
#endif

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
// Immutable during a rank. All lanes read the same predicate before deciding
// whether to enter the dependency/barrier loop; inactive rows publish zero.
groupshared uint luma_is_i4[32];

uint load_byte(ByteAddressBuffer source, uint address) {
    const uint packed = source.Load(address & ~3u);
    return (packed >> ((address & 3u) * 8u)) & 255u;
}

// Vp8MacroblockCoefficients is 770 bytes, NOT dword aligned. Odd records
// need a three-dword load and a halfword splice; never assume Load2(src).
int4 load_coefficient_row(uint address) {
    uint2 packed;
    if ((address & 2u) == 0u) {
        packed = vp8_coefficients.Load2(address);
    } else {
        const uint3 words = vp8_coefficients.Load3(address & ~3u);
        packed = uint2((words.x >> 16u) | (words.y << 16u),
                       (words.y >> 16u) | (words.z << 16u));
    }
    return int4(asint(packed.x << 16u) >> 16, asint(packed.x) >> 16,
                asint(packed.y << 16u) >> 16, asint(packed.y) >> 16);
}

uint residual_address(uint macroblock, uint block) {
    return (v_plane_offset + chroma_stride * chroma_rows + macroblock * 384u + block * 16u) * 4u;
}

int load_residual(uint macroblock, uint block, uint x, uint y) {
    return asint(vp8_yuv.Load(residual_address(macroblock, block) + y * 16u + x * 4u));
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
                   uint macroblock_y, uint extent, uint mode,
                   uint macroblock, uint first_block, bool has_coefficients) {
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
            // True-motion prediction is clamped before residual addition; keep
            // the two clamps separate. Each lane reads only its own residual and
            // writes its pixel once.
            if (has_coefficients) {
                const uint block = first_block + (y >> 2u) * (extent >> 2u) + (x >> 2u);
                prediction = uint(clamp(int(prediction) +
                    load_residual(macroblock, block, x & 3u, y & 3u), 0, 255));
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

// Exact floor(value * factor / 65536), including negative values. The
// low-word product is unsigned; a full signed product would overflow in pass 2.
int multiply_shift16(int value, uint factor) {
    return (value >> 16) * int(factor) +
           int(((uint(value) & 65535u) * factor) >> 16u);
}

int4 inverse_dct_row(int4 value) {
    const int dc = value.x + 4;
    const int a = dc + value.z;
    const int b = dc - value.z;
    const int c = multiply_shift16(value.y, 35468u) -
                 (value.w + multiply_shift16(value.w, 20091u));
    const int d = value.y + multiply_shift16(value.y, 20091u) +
                 multiply_shift16(value.w, 35468u);
    return int4(a + d, b + c, b - c, a - d) >> 3;
}

// Transforms remain independent of the dependency-ordered prediction pass.
[numthreads(64, 1, 1)]
void vp8_inverse_dct(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint macroblock = dispatch_id.x / 24u;
    const uint block = dispatch_id.x % 24u;
    if (macroblock >= macroblock_columns * macroblock_rows) return;
    // Prediction checks the same flag: stale residuals are left untouched and
    // never consumed.
    if (load_byte(vp8_coefficients, macroblock * coefficient_record_bytes + 768u) == 0u) return;

    const uint source = macroblock * coefficient_record_bytes + block * 32u;
    const int4 v0 = load_coefficient_row(source);
    const int4 v1 = load_coefficient_row(source + 8u);
    const int4 v2 = load_coefficient_row(source + 16u);
    const int4 v3 = load_coefficient_row(source + 24u);
    const uint destination = residual_address(macroblock, block);
    const int ac = v0.y | v0.z | v0.w | v1.x | v1.y | v1.z | v1.w |
                   v2.x | v2.y | v2.z | v2.w | v3.x | v3.y | v3.z | v3.w;
    if (ac == 0) {
        const int dc = (v0.x + 4) >> 3;
        [unroll] for (uint y = 0u; y < 4u; ++y)
            vp8_yuv.Store4(destination + y * 16u, asuint(int4(dc, dc, dc, dc)));
        return;
    }
#if MAICORD_VP8_AC3_FAST_PATH
    const int other = v0.z | v0.w | v1.y | v1.z | v1.w |
                      v2.x | v2.y | v2.z | v2.w | v3.x | v3.y | v3.z | v3.w;
    if (other == 0) {
        // Only positions 0, 1, 4 are nonzero. Products of int16 inputs fit int32.
        const int c1 = (v0.y * 35468) >> 16;
        const int d1 = v0.y + ((v0.y * 20091) >> 16);
        const int c4 = (v1.x * 35468) >> 16;
        const int d4 = v1.x + ((v1.x * 20091) >> 16);
        const int4 horizontal = int4(d1, c1, -c1, -d1);
        const int4 vertical = int4(d4, c4, -c4, -d4) + (v0.x + 4);
        [unroll] for (uint y = 0u; y < 4u; ++y)
            vp8_yuv.Store4(destination + y * 16u, asuint((horizontal + vertical[y]) >> 3));
        return;
    }
#endif
    // Pass 1 sees signed int16 inputs; unlike pass 2, direct products are safe.
    const int4 a = v0 + v2;
    const int4 b = v0 - v2;
    const int4 c = ((v1 * 35468) >> 16) - (v3 + ((v3 * 20091) >> 16));
    const int4 d = v1 + ((v1 * 20091) >> 16) + ((v3 * 35468) >> 16);
    vp8_yuv.Store4(destination, asuint(inverse_dct_row(a + d)));
    vp8_yuv.Store4(destination + 16u, asuint(inverse_dct_row(b + c)));
    vp8_yuv.Store4(destination + 32u, asuint(inverse_dct_row(b - c)));
    vp8_yuv.Store4(destination + 48u, asuint(inverse_dct_row(a - d)));
}

void reconstruct_macroblock(uint macroblock_x, uint macroblock_y, bool active) {
    const uint macroblock = active ? macroblock_y * macroblock_columns + macroblock_x : 0u;
    const uint mode_address = macroblock * mode_record_bytes;
    uint luma_mode = 0u;
    uint chroma_mode = 0u;
    uint has_coefficients = 0u;
    [branch] if (active) {
        luma_mode = load_byte(vp8_modes, mode_address + 16u);
        chroma_mode = load_byte(vp8_modes, mode_address + 17u);
        has_coefficients = load_byte(vp8_coefficients, macroblock * coefficient_record_bytes + 768u);
    }
    const uint luma_origin_x = macroblock_x * 16u;
    const uint luma_origin_y = macroblock_y * 16u;
    const uint edges = macroblock_y * 37u;
    uint4 top_right = 0u;
    uint macroblock_corner = 0u;
    if (pixel_lane == 0u) luma_is_i4[macroblock_y] = active && luma_mode == 10u ? 1u : 0u;
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
        } else predict_plane(0u, luma_stride, macroblock_x, macroblock_y, 16u, luma_mode,
                             macroblock, 0u, has_coefficients != 0u);
    }
    GroupMemoryBarrierWithGroupSync();
    // Keep only the prediction boundary on-chip. Independent transform
    // residuals are fused with prediction before their one final output store.
    // All rows, including inactive rows, participate in the group barriers.
    bool any_i4 = false;
    [unroll] for (uint rank_row = 0u; rank_row < 32u; ++rank_row)
        any_i4 = any_i4 || luma_is_i4[rank_row] != 0u;
    // This branch is group-uniform. Do not replace it with luma_mode == 10:
    // mixed modes and inactive rows must still reach EVERY loop barrier.
    if (any_i4) {
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
                    }
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
    }
    if (active) {
        predict_plane(u_plane_offset, chroma_stride, macroblock_x, macroblock_y, 8u, chroma_mode,
                      macroblock, 16u, has_coefficients != 0u);
        predict_plane(v_plane_offset, chroma_stride, macroblock_x, macroblock_y, 8u, chroma_mode,
                      macroblock, 20u, has_coefficients != 0u);
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
