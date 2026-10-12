// Tiled VP8 intra prediction. Each group owns two macroblock rows and eight
// ranks of (mb_x + 2*mb_y, mb_y). Ordered dispatches publish external tile
// borders; borders inside a tile stay in shared memory.
#define VP8_TILE_ROWS 2
#define RESIDUAL_BLOCK_BYTES 64u

cbuffer Vp8ReconstructionConstants : register(b0) {
    uint image_width; uint image_height;
    uint macroblock_columns; uint macroblock_rows;
    uint final_wave_rank; uint luma_stride; uint luma_rows; uint chroma_stride;
    uint chroma_rows; uint u_plane_offset; uint v_plane_offset; uint mode_record_bytes;
    uint coefficient_record_bytes; uint reserved0; uint reserved1; uint reserved2;
};
cbuffer Vp8TileConstants : register(b1) {
    uint tile_diagonal;
    uint first_tile_row;
    uint tile_rank_span;
    uint tile_reserved;
};
ByteAddressBuffer vp8_modes : register(t0);
ByteAddressBuffer vp8_coefficients : register(t1);
RWByteAddressBuffer vp8_yuv : register(u0);

uint ReadModeByte(uint a) {
    return (vp8_modes.Load(a & ~3u) >> ((a & 3u) * 8u)) & 255u;
}
uint HasCoefficients(uint mb) {
    uint a = mb * coefficient_record_bytes + 768u;
    return (vp8_coefficients.Load(a & ~3u) >> ((a & 3u) * 8u)) & 255u;
}
uint ResidualBase(uint mb) {
    return (v_plane_offset + chroma_stride * chroma_rows) * 4u + mb * 24u * RESIDUAL_BLOCK_BYTES;
}

int ReadResidual(uint mb_base, uint block, uint x, uint y) {
    return asint(vp8_yuv.Load(mb_base + block * RESIDUAL_BLOCK_BYTES + y * 16u + x * 4u));
}

// Bottom cache: four generations, each [Y16,U8,V8]. Right cache: [Y16,U8,V8].
groupshared uint gs_bottom[VP8_TILE_ROWS * 4 * 32];
groupshared uint gs_right[VP8_TILE_ROWS * 32];
groupshared uint gs_top[VP8_TILE_ROWS * 36];       // [Y20,U8,V8]
groupshared uint gs_left[VP8_TILE_ROWS * 32];      // [Y16,U8,V8]
groupshared uint gs_corner[VP8_TILE_ROWS * 3];
groupshared uint gs_dc[VP8_TILE_ROWS * 3];
groupshared uint gs_y[VP8_TILE_ROWS * 256];
groupshared uint gs_luma_mode[VP8_TILE_ROWS];
groupshared uint gs_chroma_mode[VP8_TILE_ROWS];
groupshared uint gs_has_coefficients[VP8_TILE_ROWS];
groupshared uint gs_modes4[VP8_TILE_ROWS * 4];

uint PlaneOffset(uint p) { return p == 0u ? 0u : (p == 1u ? u_plane_offset : v_plane_offset); }
uint PlaneStride(uint p) { return p == 0u ? luma_stride : chroma_stride; }
uint Extent(uint p) { return p == 0u ? 16u : 8u; }
uint BorderOffset(uint p) { return p == 0u ? 0u : (p == 1u ? 16u : 24u); }
uint TopOffset(uint p) { return p == 0u ? 0u : (p == 1u ? 20u : 28u); }

// Only called for an external MB border: top row/corner, or left column.
// A sample produced by this group MUST be read from shared memory, never UAV.
// All other samples were produced by an earlier DISPATCH, or are image borders.
uint LoadHalo(uint p, int x, int y, uint mb_y, uint base_y, uint base_rank) {
    if (y < 0) return 127u; // top takes precedence at the upper-left corner
    if (x < 0) return 129u;
    uint extent = Extent(p), stride = PlaneStride(p);
    x = min(x, int(macroblock_columns * extent) - 1);
    uint sample_mb_x = uint(x) / extent;
    uint sample_mb_y = uint(y) / extent;
    uint sample_rank = sample_mb_x + 2u * sample_mb_y;
    bool in_tile = sample_mb_x < macroblock_columns &&
                   sample_mb_y >= base_y && sample_mb_y < base_y + VP8_TILE_ROWS &&
                   sample_rank >= base_rank && sample_rank - base_rank < tile_rank_span;
    if (in_tile) {
        uint row = sample_mb_y - base_y;
        uint off = BorderOffset(p);
        if (sample_mb_y == mb_y)
            return gs_right[row * 32u + off + uint(y) % extent];
        return gs_bottom[(row * 4u + (sample_rank & 3u)) * 32u + off + uint(x) % extent];
    }
    return vp8_yuv.Load((PlaneOffset(p) + uint(y) * stride + uint(x)) * 4u);
}
uint DCValue(uint row, uint p, bool has_top, bool has_left) {
    uint extent = Extent(p);
    if (!has_top && !has_left) return 128u;
    uint sum = has_top && has_left ? extent : extent / 2u;
    [loop] for (uint k = 0u; k < extent; ++k) {
        if (has_top)  sum += gs_top[row * 36u + TopOffset(p) + k];
        if (has_left) sum += gs_left[row * 32u + BorderOffset(p) + k];
    }
    uint shift = (p == 0u ? 4u : 3u) + (has_top && has_left ? 1u : 0u);
    return sum >> shift;
}
uint Clip(int v) { return uint(clamp(v, 0, 255)); }
uint TM(uint l, uint t, uint c) { return Clip(int(l) + int(t) - int(c)); }
uint Avg2(uint a, uint b) { return (a + b + 1u) >> 1u; }
uint Avg3(uint a, uint b, uint c) { return (a + 2u * b + c + 2u) >> 2u; }
uint PredictBig(uint row, uint p, uint mode, uint x, uint y) {
    uint value = 128u;
    if (mode == 0u) value = gs_dc[row * 3u + p];
    else if (mode == 2u) value = gs_top[row * 36u + TopOffset(p) + x];
    else if (mode == 3u) value = gs_left[row * 32u + BorderOffset(p) + y];
    else if (mode == 1u) value = TM(gs_left[row * 32u + BorderOffset(p) + y],
        gs_top[row * 36u + TopOffset(p) + x], gs_corner[row * 3u + p]);
    return value;
}
uint I4Top(uint row, uint bx, uint by, uint i) {
    i = min(i, 7u);
    uint value = 0u;
    // Every subblock row in the rightmost column reuses the original top-right.
    if (bx == 3u && i >= 4u) value = gs_top[row * 36u + 12u + i];
    else if (by == 0u) value = gs_top[row * 36u + bx * 4u + i];
    else value = gs_y[row * 256u + (by * 4u - 1u) * 16u + bx * 4u + i];
    return value;
}
uint I4Left(uint row, uint bx, uint by, uint i) {
    i = min(i, 3u);
    uint value = 0u;
    if (bx == 0u) value = gs_left[row * 32u + by * 4u + i];
    else value = gs_y[row * 256u + (by * 4u + i) * 16u + bx * 4u - 1u];
    return value;
}
uint I4Corner(uint row, uint bx, uint by) {
    uint value = 0u;
    if (by == 0u) value = bx == 0u ? gs_corner[row * 3u] : gs_top[row * 36u + bx * 4u - 1u];
    else if (bx == 0u) value = gs_left[row * 32u + by * 4u - 1u];
    else value = gs_y[row * 256u + (by * 4u - 1u) * 16u + bx * 4u - 1u];
    return value;
}
uint I4Neighbor(uint row, uint bx, uint by, int pos, bool transpose_axes) {
    uint value = 0u;
    if (pos == 0) value = I4Corner(row, bx, by);
    else if (transpose_axes) value = pos > 0 ? I4Left(row, bx, by, uint(pos - 1))
        : I4Top(row, bx, by, min(uint(-pos - 1), 3u));
    else value = pos > 0 ? I4Top(row, bx, by, uint(pos - 1)) : I4Left(row, bx, by, uint(-pos - 1));
    return value;
}
uint I4Directional(uint row, uint bx, uint by, int delta, bool transpose_axes) {
    int o = delta < 0 ? delta : int(uint(delta) >> 1u);
    uint a = I4Neighbor(row, bx, by, o, transpose_axes);
    uint b = I4Neighbor(row, bx, by, o + 1, transpose_axes);
    if (delta >= 0 && (delta & 1) == 0) return Avg2(a, b);
    return Avg3(a, b, I4Neighbor(row, bx, by, o + 2, transpose_axes));
}
uint Predict4(uint row, uint bx, uint by, uint mode, uint x, uint y) {
    uint value = 0u;
    if (mode == 0u) {
        uint sum = 4u;
        [unroll] for (uint i = 0u; i < 4u; ++i) sum += I4Top(row, bx, by, i) + I4Left(row, bx, by, i);
        value = sum >> 3u;
    } else if (mode == 1u) value = TM(I4Left(row, bx, by, y), I4Top(row, bx, by, x), I4Corner(row, bx, by));
    else if (mode == 2u) value = Avg3(x == 0u ? I4Corner(row, bx, by) : I4Top(row, bx, by, x - 1u),
        I4Top(row, bx, by, x), I4Top(row, bx, by, x + 1u));
    else if (mode == 3u) value = Avg3(y == 0u ? I4Corner(row, bx, by) : I4Left(row, bx, by, y - 1u),
        I4Left(row, bx, by, y), I4Left(row, bx, by, y + 1u));
    else if (mode == 4u) {
        int d = int(x) - int(y);
        value = Avg3(I4Neighbor(row, bx, by, d - 1, false), I4Neighbor(row, bx, by, d, false), I4Neighbor(row, bx, by, d + 1, false));
    } else if (mode == 5u) value = I4Directional(row, bx, by, 2 * int(x) - int(y), false);
    else if (mode == 6u) {
        uint o = x + y;
        value = Avg3(I4Top(row, bx, by, o), I4Top(row, bx, by, o + 1u), I4Top(row, bx, by, o + 2u));
    } else if (mode == 7u) {
        if (x == 3u && y >= 2u) {
            uint k = y + 2u;
            value = Avg3(I4Top(row, bx, by, k), I4Top(row, bx, by, k + 1u), I4Top(row, bx, by, k + 2u));
        } else {
            uint o = x + (y >> 1u);
            uint a = I4Top(row, bx, by, o), b = I4Top(row, bx, by, o + 1u);
            value = (y & 1u) == 0u ? Avg2(a, b) : Avg3(a, b, I4Top(row, bx, by, o + 2u));
        }
    } else if (mode == 8u) value = I4Directional(row, bx, by, 2 * int(y) - int(x), true);
    else {
        uint o = y + (x >> 1u);
        uint a = I4Left(row, bx, by, o), b = I4Left(row, bx, by, o + 1u);
        value = (x & 1u) == 0u ? Avg2(a, b) : Avg3(a, b, I4Left(row, bx, by, o + 2u));
    }
    return value;
}

[numthreads(64, VP8_TILE_ROWS, 1)]
void vp8_reconstruct(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint tile_y = first_tile_row + gid.x;
    uint tile_u = tile_diagonal - tile_y;
    uint base_y = tile_y * VP8_TILE_ROWS;
    uint base_rank = tile_u * tile_rank_span;
    if (base_y >= macroblock_rows || tile_rank_span == 0u) return; // group-uniform
    uint end_y = min(base_y + VP8_TILE_ROWS, macroblock_rows);
    uint min_rank = 2u * base_y;
    uint max_rank = (macroblock_columns - 1u) + 2u * (end_y - 1u);
    uint begin = max(base_rank, min_rank);
    uint end = min(base_rank + tile_rank_span, max_rank + 1u);
    if (begin >= end) return; // group-uniform: this skew tile misses the image

    uint lane = tid.x, row = tid.y, my = base_y + row;
    uint by = lane >> 4u, px = lane & 3u, py = (lane >> 2u) & 3u;
    [loop] for (uint rank = begin; rank < end; ++rank) {
        int signed_mx = int(rank) - 2 * int(my);
        bool active = my < macroblock_rows && signed_mx >= 0 && uint(signed_mx) < macroblock_columns;
        uint mx = active ? uint(signed_mx) : 0u;
        uint mb = my * macroblock_columns + mx;
        uint mode_addr = mb * mode_record_bytes;
        if (lane == 0u) {
            gs_luma_mode[row] = active ? ReadModeByte(mode_addr + 16u) : 0u;
            gs_chroma_mode[row] = active ? ReadModeByte(mode_addr + 17u) : 0u;
            gs_has_coefficients[row] = active ? HasCoefficients(mb) : 0u;
        }
        if (active) {
            if (lane < 4u) {
                // Safe even if the mode record has no dword alignment.
                uint a = mode_addr + lane * 4u;
                if ((a & 3u) == 0u) gs_modes4[row * 4u + lane] = vp8_modes.Load(a);
                else gs_modes4[row * 4u + lane] = ReadModeByte(a) | (ReadModeByte(a + 1u) << 8u) |
                    (ReadModeByte(a + 2u) << 16u) | (ReadModeByte(a + 3u) << 24u);
            }
            if (lane < 20u)
                gs_top[row * 36u + lane] = LoadHalo(0u, int(mx * 16u + lane), int(my * 16u) - 1, my, base_y, base_rank);
            if (lane < 16u)
                gs_left[row * 32u + lane] = LoadHalo(0u, int(mx * 16u) - 1, int(my * 16u + lane), my, base_y, base_rank);
            if (lane < 8u) {
                [unroll] for (uint p = 1u; p < 3u; ++p) {
                    gs_top[row * 36u + TopOffset(p) + lane] = LoadHalo(p, int(mx * 8u + lane), int(my * 8u) - 1, my, base_y, base_rank);
                    gs_left[row * 32u + BorderOffset(p) + lane] = LoadHalo(p, int(mx * 8u) - 1, int(my * 8u + lane), my, base_y, base_rank);
                }
            }
            if (lane < 3u) {
                uint extent = Extent(lane);
                gs_corner[row * 3u + lane] = LoadHalo(lane, int(mx * extent) - 1, int(my * extent) - 1, my, base_y, base_rank);
            }
        }
        GroupMemoryBarrierWithGroupSync();

        int4 ry = int4(0, 0, 0, 0);
        int ru = 0, rv = 0;
        if (active) {
            // Three lanes compute the three DC predictors once, not once/pixel.
            if (lane < 3u) {
                uint mode = lane == 0u ? gs_luma_mode[row] : gs_chroma_mode[row];
                gs_dc[row * 3u + lane] = mode == 0u ? DCValue(row, lane, my != 0u, mx != 0u) : 128u;
            }
            if (gs_has_coefficients[row] != 0u) {
                uint rb = ResidualBase(mb);
                // Each lane selects ONLY its own residual, never a redundant int4.
                ry = int4(ReadResidual(rb, by * 4u, px, py), ReadResidual(rb, by * 4u + 1u, px, py),
                          ReadResidual(rb, by * 4u + 2u, px, py), ReadResidual(rb, by * 4u + 3u, px, py));
                uint cx = lane & 7u, cy = lane >> 3u;
                uint cb = (cy >> 2u) * 2u + (cx >> 2u);
                ru = ReadResidual(rb, 16u + cb, cx & 3u, cy & 3u);
                rv = ReadResidual(rb, 20u + cb, cx & 3u, cy & 3u);
            }
        }
        GroupMemoryBarrierWithGroupSync();

        if (active) {
            // Chroma has no intra-MB prediction dependencies: two pixels/lane.
            uint cx = lane & 7u, cy = lane >> 3u;
            uint u = Clip(int(PredictBig(row, 1u, gs_chroma_mode[row], cx, cy)) + ru);
            uint v = Clip(int(PredictBig(row, 2u, gs_chroma_mode[row], cx, cy)) + rv);
            uint uvpos = (my * 8u + cy) * chroma_stride + mx * 8u + cx;
            vp8_yuv.Store((u_plane_offset + uvpos) * 4u, u);
            vp8_yuv.Store((v_plane_offset + uvpos) * 4u, v);
            if (cx == 7u) {
                gs_right[row * 32u + 16u + cy] = u;
                gs_right[row * 32u + 24u + cy] = v;
            }
            if (cy == 7u) {
                uint b = (row * 4u + (rank & 3u)) * 32u;
                gs_bottom[b + 16u + cx] = u; gs_bottom[b + 24u + cx] = v;
            }
            if (gs_luma_mode[row] != 10u) {
                // All sixteen 4x4 residual blocks can be applied in parallel.
                [unroll] for (uint bx = 0u; bx < 4u; ++bx) {
                    uint x = bx * 4u + px, y = by * 4u + py;
                    gs_y[row * 256u + y * 16u + x] =
                        Clip(int(PredictBig(row, 0u, gs_luma_mode[row], x, y)) + ry[bx]);
                }
            }
        }
        // Every lane evaluates the SAME predicate from immutable per-row modes.
        // No per-row/per-lane conditional barrier is used.
        bool any_i4 = false;
        [unroll] for (uint r = 0u; r < VP8_TILE_ROWS; ++r)
            any_i4 = any_i4 || (gs_luma_mode[r] == 10u);
        if (any_i4) {
            // Dependencies: left, top, top-right. bx+by is NOT safe.
            // Four subblock rows, sixteen pixel lanes per row, ten phases.
            [loop] for (uint phase = 0u; phase < 10u; ++phase) {
                int sx = int(phase) - 2 * int(by);
                if (active && gs_luma_mode[row] == 10u && sx >= 0 && sx < 4) {
                    uint bx = uint(sx);
                    uint mode = (gs_modes4[row * 4u + by] >> (bx * 8u)) & 255u;
                    uint value = Clip(int(Predict4(row, bx, by, mode, px, py)) + ry[bx]);
                    gs_y[row * 256u + (by * 4u + py) * 16u + bx * 4u + px] = value;
                }
                GroupMemoryBarrierWithGroupSync();
            }
        }
        if (active) {
            [unroll] for (uint bx = 0u; bx < 4u; ++bx) {
                uint x = bx * 4u + px, y = by * 4u + py;
                // When I16 is used, this is the SAME lane that wrote the value.
                uint value = gs_y[row * 256u + y * 16u + x];
                vp8_yuv.Store(((my * 16u + y) * luma_stride + mx * 16u + x) * 4u, value);
                if (x == 15u) gs_right[row * 32u + y] = value;
                if (y == 15u) gs_bottom[(row * 4u + (rank & 3u)) * 32u + x] = value;
            }
        }
        // The next rank uses only shared borders for intra-tile dependencies.
        // Global stores are consumed only by later dispatches, so no UAV fence
        // or inter-group synchronization is needed.
        GroupMemoryBarrierWithGroupSync();
    }
}
