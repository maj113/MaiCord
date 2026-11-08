// twin: src/render/shaders/vp8_predict.hlsl 8511e74d203a1798
// vp8_reconstruct (STAGE_CS): VP8 intra prediction (RFC 6386 12.2-12.3) of a whole keyframe in one group, after
// vp8_reconstruct.glsl's inverse transforms. Thread row y owns macroblock row y as four sub-rows of 4x4 subblocks,
// four lanes each (one sample row per lane). A sub-row takes its next subblock (all four at once in a 16x16
// macroblock) in a level once its left, top and top-right neighbours exist, so the sub-rows of a row and the rows
// below run as a staggered pipeline of about 4 * columns + 8 * rows levels; chroma takes one macroblock per level.
// Edges stay in shared memory and each lane keeps its position, modes and residuals (loaded a macroblock ahead) in
// registers: a level is one round of shared reads, the arithmetic, and one barrier.
#define VP8_ROWS 32
// Every level but the last advances some sub-row or chroma row, so 16 * 32 * 32 + 32 * 32 + 1 levels bound the loop;
// a frame takes about 4 * columns + 8 * rows.
#define VP8_LEVELS 17409u

layout(local_size_x = 16, local_size_y = VP8_ROWS) in;

layout(std140, binding = 0) uniform Vp8ReconstructionConstants {
    uint macroblock_columns;
    uint macroblock_rows;
    uint luma_stride;
    uint chroma_stride;
    uint chroma_rows;
    uint u_plane_offset;
    uint v_plane_offset;
    uint mode_record_bytes;
    uint coefficient_record_bytes;
};

layout(std430, binding = 0) readonly buffer Modes { uint vp8_modes[]; }; //! t0
layout(std430, binding = 1) readonly buffer Coefficients { uint vp8_coefficients[]; }; //! t1
layout(std430, binding = 2) buffer Yuv { uint vp8_yuv[]; }; //! u0

// Shared samples are four to a word (sample i in bits 8i..8i+7).
// Luma edges: the bottom row of each macroblock row by row parity (EDGE_LINE, 129 words each; word 4 * columns
// repeats the last sample, the top-right past the right edge), then the bottom rows of sub-rows 0..2 of each
// macroblock row as rings of eight subblock columns (EDGE_INNER).
#define EDGE_LINE 0u
#define EDGE_INNER 258u
shared uint gs_edges[258 + VP8_ROWS * 3 * 8];
// Bottom chroma rows: [parity][plane][word].
shared uint gs_chroma_line[2 * 2 * 64];
// Right column of each sub-row's last subblock in three slots by subblock column s (read s - 1, write s, clear
// s + 1); the four lanes OR their samples in.
shared uint gs_left[VP8_ROWS * 4 * 3];
// Right chroma columns: [row][plane][slot][half].
shared uint gs_chroma_left[VP8_ROWS * 2 * 3 * 2];
// Subblocks done per sub-row and chroma macroblocks done per row, by level parity: a level reads one copy and writes
// the other.
shared uint gs_progress[2 * VP8_ROWS * 4];
shared uint gs_chroma_progress[2 * VP8_ROWS];
shared uint gs_dc[VP8_ROWS];
// Per macroblock: luma mode (bits 0-3), chroma mode (4-6), has coefficients (7); four to a word.
shared uint gs_info[VP8_ROWS * 8];
// kSubblockCodes, copied once: a constant table indexed per lane would be a memory load in every level.
shared uint gs_codes[32];

uint Byte(uint word, uint index) { return (word >> (index * 8u)) & 255u; }
uint SumBytes(uint word) {
    uint pairs = (word & 0x00FF00FFu) + ((word >> 8u) & 0x00FF00FFu);
    return (pairs & 0xFFFFu) + (pairs >> 16u);
}
uint Pack(uvec4 v) { return v.x | (v.y << 8u) | (v.z << 16u) | (v.w << 24u); }

// Four bytes at once: floor((a + b) / 2) and (a + b + 1) / 2 per byte.
uint AverageDown(uint a, uint b) { return (a & b) + (((a ^ b) >> 1u) & 0x7F7F7F7Fu); }
uint AverageUp(uint a, uint b) { return (a | b) - (((a ^ b) >> 1u) & 0x7F7F7F7Fu); }
// The four bytes `offset` (1..3) bytes into the byte stream low, high.
uint Window(uint low, uint high, uint offset) { return (low >> (offset * 8u)) | (high << (32u - offset * 8u)); }

// Intra 4x4 modes 2..9 (RFC 6386 12.3) predict every sample as a two- or three-tap average of the edge e(k): k < 0
// the left column (e(-1) = left[0], repeated past left[3]), e(0) the corner, k > 0 the top row with the top-right
// (repeated past it). For mode m, row y, sample x, byte x of kSubblockCodes[(m - 2) * 4 + y] selects the average:
// codes 0..15 are (e(k) + e(k + 1) + 1) / 2 and 16..31 (e(k - 1) + 2 e(k) + e(k + 1) + 2) / 4, with k = code % 16 - 7.
// Generated from the per-mode formulas and checked against them for every sample.
const uint kSubblockCodes[32] = uint[32](
    0x1B1A1918u, 0x1B1A1918u, 0x1B1A1918u, 0x1B1A1918u, 0x16161616u, 0x15151515u, 0x14141414u, 0x13131313u,
    0x1A191817u, 0x19181716u, 0x18171615u, 0x17161514u, 0x0A090807u, 0x1A191817u, 0x09080716u, 0x19181715u,
    0x1C1B1A19u, 0x1D1C1B1Au, 0x1E1D1C1Bu, 0x1F1E1D1Cu, 0x0B0A0908u, 0x1C1B1A19u, 0x1D0B0A09u, 0x1E1C1B1Au,
    0x19181706u, 0x17061605u, 0x16051504u, 0x15041403u, 0x14041505u, 0x13031404u, 0x12021303u, 0x11011202u);

// Row y of a 4x4 subblock in `mode` from its left column, corner, top row and top-right.
uvec4 PredictSubblockRow(uint mode, uint y, uint left, uint corner, uint top, uint top_right) {
    if (mode == 0u) return uvec4((SumBytes(top) + SumBytes(left) + 4u) >> 3u);
    if (mode == 1u) {
        ivec4 top_samples = ivec4(Byte(top, 0u), Byte(top, 1u), Byte(top, 2u), Byte(top, 3u));
        return uvec4(clamp(top_samples + (int(Byte(left, y)) - int(corner)), 0, 255));
    }
    // The edge as a byte stream from e(-8): left reversed, corner, top, top-right, then repeats. Two-tap averages of
    // e(k), e(k + 1) (`pairs`) and three-tap ones centred on e(k) (`triples`) for k = -7..8, word j holding
    // k = 4j - 7 .. 4j - 4.
    uint e0 = (left >> 24u) * 0x01010101u;
    uint e1 = (left >> 24u) | ((left >> 8u) & 0xFF00u) | ((left << 8u) & 0xFF0000u) | (left << 24u);
    uint e2 = corner | (top << 8u), e3 = (top >> 24u) | (top_right << 8u), e4 = (top_right >> 24u) * 0x01010101u;
    uvec4 pairs = uvec4(AverageUp(Window(e0, e1, 1u), Window(e0, e1, 2u)),
                        AverageUp(Window(e1, e2, 1u), Window(e1, e2, 2u)),
                        AverageUp(Window(e2, e3, 1u), Window(e2, e3, 2u)),
                        AverageUp(Window(e3, e4, 1u), Window(e3, e4, 2u)));
    uvec4 triples = uvec4(AverageUp(AverageDown(e0, Window(e0, e1, 2u)), Window(e0, e1, 1u)),
                          AverageUp(AverageDown(e1, Window(e1, e2, 2u)), Window(e1, e2, 1u)),
                          AverageUp(AverageDown(e2, Window(e2, e3, 2u)), Window(e2, e3, 1u)),
                          AverageUp(AverageDown(e3, Window(e3, e4, 2u)), Window(e3, e4, 1u)));
    uint codes = gs_codes[min(mode - 2u, 7u) * 4u + y];
    uvec4 value;
    for (uint x = 0u; x < 4u; ++x) {
        uint code = Byte(codes, x);
        uvec4 table = code < 16u ? pairs : triples;
        uvec2 half_table = (code & 8u) != 0u ? table.zw : table.xy;
        value[x] = Byte((code & 4u) != 0u ? half_table.y : half_table.x, code & 3u);
    }
    return value;
}
// 16x16 and chroma prediction (RFC 6386 12.2) of the four samples under `top` from the left sample, the corner and
// the DC value.
uvec4 PredictBig(uint mode, uint left, uint top, uint corner, uint dc) {
    ivec4 top_samples = ivec4(Byte(top, 0u), Byte(top, 1u), Byte(top, 2u), Byte(top, 3u));
    if (mode == 1u) return uvec4(clamp(top_samples + (int(left) - int(corner)), 0, 255));
    return mode == 2u ? uvec4(top_samples) : uvec4(mode == 0u ? dc : mode == 3u ? left : 128u);
}
// The 16x16 or 8x8 DC value from the sums of the edges that exist.
uint DCValue(uint top_sum, uint left_sum, bool has_top, bool has_left, uint shift) {
    if (!has_top && !has_left) return 128u;
    uint both = has_top && has_left ? 1u : 0u;
    uint sum = (has_top ? top_sum : 0u) + (has_left ? left_sum : 0u);
    return (sum + (1u << (shift - 1u + both))) >> (shift + both);
}

uint MacroblockInfo(uint x, uint y) { return x < macroblock_columns ? Byte(gs_info[y * 8u + (x >> 2u)], x & 3u) : 0u; }
// Residuals (vp8_reconstruct.glsl) follow the planes: per macroblock 192 words of raster rows, two signed 16-bit
// samples to a word. Loads are unconditional, of a macroblock clamped into the image (a skipped load would be waited
// for right away); a macroblock without coefficients masks its stale residuals at use. A word index into vp8_yuv.
uint ResidualAddress(uint x, uint y) {
    uint mb = min(y, macroblock_rows - 1u) * macroblock_columns + min(x, macroblock_columns - 1u);
    return v_plane_offset + chroma_stride * chroma_rows + mb * 192u;
}
uvec4 Load4(uint at) { return uvec4(vp8_yuv[at], vp8_yuv[at + 1u], vp8_yuv[at + 2u], vp8_yuv[at + 3u]); }
void Store4(uint at, uvec4 value) {
    vp8_yuv[at] = value.x;
    vp8_yuv[at + 1u] = value.y;
    vp8_yuv[at + 2u] = value.z;
    vp8_yuv[at + 3u] = value.w;
}
uvec4 AddResidual(uvec4 prediction, uvec2 packed_residual, uint info) {
    ivec4 residual = (info & 128u) != 0u
        ? ivec4(int(packed_residual.x << 16u) >> 16, int(packed_residual.x) >> 16, int(packed_residual.y << 16u) >> 16,
                int(packed_residual.y) >> 16)
        : ivec4(0);
    return uvec4(clamp(ivec4(prediction) + residual, 0, 255));
}
uvec2 Residual(uvec4 low, uvec4 high, uint k) {
    return k == 0u ? low.xy : k == 1u ? low.zw : k == 2u ? high.xy : high.zw;
}
uint NextSlot(uint slot) { return slot == 2u ? 0u : slot + 1u; }
uint PreviousSlot(uint slot) { return slot == 0u ? 2u : slot - 1u; }

void main() {
    uint lane = gl_LocalInvocationID.x, y = gl_LocalInvocationID.y;
    uint columns = macroblock_columns, end = columns * 4u, last = macroblock_rows - 1u;
    bool row_active = y < macroblock_rows;
    if (lane < 8u) {
        uint packed_info = 0u;
        for (uint column = 0u; column < 4u; ++column) {
            uint x = lane * 4u + column;
            if (row_active && x < columns) {
                uint mb = y * columns + x;
                // Mode records are word multiples: luma mode in byte 16, chroma mode in byte 17.
                uint modes = vp8_modes[(mb * mode_record_bytes + 16u) >> 2u];
                uint coefficient_address = mb * coefficient_record_bytes + 768u;
                uint has_coefficients = Byte(vp8_coefficients[coefficient_address >> 2u], coefficient_address & 3u);
                packed_info |= ((modes & 15u) | ((Byte(modes, 1u) & 7u) << 4u) | (has_coefficients != 0u ? 128u : 0u))
                               << (column * 8u);
            }
        }
        gs_info[y * 8u + lane] = packed_info;
    }
    if (y < 2u) gs_codes[y * 16u + lane] = kSubblockCodes[y * 16u + lane];
    if (lane < 12u) {
        gs_left[y * 12u + lane] = 0u;
        gs_chroma_left[y * 12u + lane] = 0u;
    }
    if (lane < 4u) gs_progress[y * 4u + lane] = row_active ? 0u : end;
    if (lane == 0u) gs_chroma_progress[y] = row_active ? 0u : columns;
    memoryBarrierShared();
    barrier();

    // Luma: sub-row `by`, sample row `py`. Chroma: plane `plane`, sample row `cr`.
    uint by = lane >> 2u, py = lane & 3u;
    uint plane = lane >> 3u, cr = lane & 7u;
    uint line_above = EDGE_LINE + ((y - 1u) & 1u) * 129u, line_own = EDGE_LINE + (y & 1u) * 129u;
    uint inner_above = EDGE_INNER + (y * 3u + by - 1u) * 8u, inner_own = EDGE_INNER + (y * 3u + by) * 8u;
    uint left_base = (y * 4u + by) * 3u;
    uint chroma_above = ((((y - 1u) & 1u) * 2u) + plane) * 64u, chroma_own = ((y & 1u) * 2u + plane) * 64u;
    uint chroma_left_base = (y * 2u + plane) * 6u;
    uint luma_row = (y * 16u + by * 4u + py) * luma_stride;
    uint chroma_row = (plane == 0u ? u_plane_offset : v_plane_offset) + (y * 8u + cr) * chroma_stride;
    uint luma_offset = (by * 4u + py) * 8u, chroma_offset = 128u + plane * 32u + cr * 4u;

    // This sub-row: its next subblock s (s % 3 is its left slot), the macroblock's info, modes and residual row, and
    // the next macroblock's. Chroma: its next macroblock cx likewise.
    uint s = row_active ? 0u : end, slot = 0u;
    uint info = MacroblockInfo(0u, y), next_info = MacroblockInfo(1u, y);
    uint modes = vp8_modes[(min(y, last) * columns * mode_record_bytes >> 2u) + by];
    uint next_modes = vp8_modes[((min(y, last) * columns + min(1u, columns - 1u)) * mode_record_bytes >> 2u) + by];
    uvec4 low = Load4(ResidualAddress(0u, y) + luma_offset);
    uvec4 high = Load4(ResidualAddress(0u, y) + luma_offset + 4u);
    uvec4 next_low = Load4(ResidualAddress(1u, y) + luma_offset);
    uvec4 next_high = Load4(ResidualAddress(1u, y) + luma_offset + 4u);
    uint cx = row_active ? 0u : columns, chroma_slot = 0u;
    uint chroma_info = info, chroma_next_info = next_info;
    uvec4 chroma = Load4(ResidualAddress(0u, y) + chroma_offset);
    uvec4 next_chroma = Load4(ResidualAddress(1u, y) + chroma_offset);

    for (uint level = 0u; level < VP8_LEVELS; ++level) {
        uint read = (level & 1u) * uint(VP8_ROWS * 4), write = uint(VP8_ROWS * 4) - read;
        uint chroma_read = (level & 1u) * uint(VP8_ROWS), chroma_write = uint(VP8_ROWS) - chroma_read;
        // A finished (or absent) sub-row only republishes its progress.
        if (s < end) {
            // Shared reads first: their addresses come from registers only.
            uint x = s >> 2u, bx = s & 3u, luma_mode = info & 15u;
            bool big = luma_mode != 10u;
            uint above = y == 0u ? end : gs_progress[read + y * 4u - 1u];
            uint previous = by == 0u ? above : gs_progress[read + y * 4u + by - 1u];
            uint next = by == 3u ? end + 8u : gs_progress[read + y * 4u + by + 1u];
            uint left_done = gs_progress[read + y * 4u + 3u];
            uint left = s == 0u ? 0x81818181u : gs_left[left_base + PreviousSlot(slot)];
            // A 16x16 sub-row reads the macroblock's top row and corner; a 4x4 one its own top, top-right and corner
            // (the right column's top-right is the macroblock's, from the row above).
            bool from_line = by == 0u || big;
            uint top_word = gs_edges[from_line ? line_above + s : inner_above + (s & 7u)];
            uint top_right_word = gs_edges[from_line || bx == 3u ? line_above + s + 1u : inner_above + ((s + 1u) & 7u)];
            uint corner_word = gs_edges[from_line ? line_above + max(s, 1u) - 1u : inner_above + ((s - 1u) & 7u)];
            bool top_border = y == 0u && from_line;
            uint top = top_border ? 0x7F7F7F7Fu : top_word;
            uint top_right = y == 0u && (from_line || bx == 3u) ? 0x7F7F7F7Fu : top_right_word;
            uint corner = top_border ? 127u : s == 0u ? 129u : Byte(corner_word, 3u);

            uint advance = 0u, right = 0u;
            if (!big) {
                // Left, top and top-right done; the sub-row below may lag at most six subblocks (its edge ring).
                if (previous >= min(s + (by != 0u && bx == 3u ? 1u : 2u), end) && s + 1u <= next + 7u) {
                    uvec4 value = AddResidual(PredictSubblockRow(Byte(modes, bx), py, left, corner, top, top_right),
                                              Residual(low, high, bx), info);
                    Store4(luma_row + s * 4u, value);
                    right = value.w;
                    if (py == 3u) {
                        gs_edges[by != 3u ? inner_own + (s & 7u) : line_own + s] = Pack(value);
                        if (by == 3u && s + 1u == end) gs_edges[line_own + end] = value.w * 0x01010101u;
                    }
                    advance = 1u;
                }
            } else if ((by == 0u ? above >= min(s + 5u, end) : previous >= s + 4u) &&
                       (luma_mode != 0u || left_done >= s) && s + 4u <= next + 7u) {
                // A 16x16 sub-row needs only the macroblock's edges, DC every sub-row of the left macroblock. Sub-rows
                // keep their order (the row below reuses the row above's line), and the first waits for the top-right
                // macroblock's first subblock like a 4x4 one, which keeps the corner alive.
                uint top_words[4];
                top_words[0] = top;
                for (uint word = 1u; word < 4u; ++word)
                    top_words[word] = y == 0u ? 0x7F7F7F7Fu : gs_edges[line_above + s + word];
                // The DC sum: the top row and the left macroblock's right column (each sub-row's left slot still
                // holds it). Sub-row 0 computes it, the others reuse it.
                uint dc = 128u;
                if (luma_mode == 0u) {
                    if (by == 0u) {
                        uint left_sum = 0u;
                        for (uint sub_row = 0u; sub_row < 4u; ++sub_row)
                            left_sum += SumBytes(gs_left[y * 12u + sub_row * 3u + PreviousSlot(slot)]);
                        uint top_sum = SumBytes(top_words[0]) + SumBytes(top_words[1]) + SumBytes(top_words[2]) +
                                       SumBytes(top_words[3]);
                        dc = DCValue(top_sum, left_sum, y != 0u, x != 0u, 4u);
                        if (py == 0u) gs_dc[y] = dc;
                    } else {
                        dc = gs_dc[y];
                    }
                }
                for (uint k = 0u; k < 4u; ++k) {
                    uvec4 value = AddResidual(PredictBig(luma_mode, Byte(left, py), top_words[k], corner, dc),
                                              Residual(low, high, k), info);
                    Store4(luma_row + (s + k) * 4u, value);
                    right = value.w;
                    // The row below reads the whole line, a 4x4 sub-row below only the corner word.
                    if (py == 3u) {
                        if (by == 3u) gs_edges[line_own + s + k] = Pack(value);
                        else if (k == 3u) gs_edges[inner_own + ((s + 3u) & 7u)] = Pack(value);
                        if (by == 3u && k == 3u && s + 4u == end) gs_edges[line_own + end] = value.w * 0x01010101u;
                    }
                }
                advance = 4u;
            }
            if (advance != 0u) {
                atomicOr(gs_left[left_base + slot], right << (py * 8u));
                if (py == 0u) gs_left[left_base + NextSlot(slot)] = 0u;
                s += advance;
                slot = NextSlot(slot);
            }
            // Entering a macroblock takes the data loaded for it a level or more ago; the next macroblock's is loaded
            // every level, unconditionally, so no load is waited for inside a branch.
            bool entered = advance != 0u && (s & 3u) == 0u;
            uint next_x = min((s >> 2u) + 1u, columns - 1u);
            info = entered ? next_info : info;
            modes = entered ? next_modes : modes;
            low = entered ? next_low : low;
            high = entered ? next_high : high;
            next_info = MacroblockInfo((s >> 2u) + 1u, y);
            next_modes = vp8_modes[((min(y, last) * columns + next_x) * mode_record_bytes >> 2u) + by];
            next_low = Load4(ResidualAddress(next_x, y) + luma_offset);
            next_high = Load4(ResidualAddress(next_x, y) + luma_offset + 4u);
        }
        if (py == 0u) gs_progress[write + y * 4u + by] = s;

        // Chroma: one macroblock per level once the row above is two macroblocks ahead (its top and the corner of the
        // next one stay in the line until then).
        if (cx < columns) {
            bool chroma_entered = false;
            if (y == 0u || gs_chroma_progress[chroma_read + y - 1u] >= min(cx + 2u, columns)) {
                uint chroma_mode = (chroma_info >> 4u) & 7u;
                uint top0 = y == 0u ? 0x7F7F7F7Fu : gs_chroma_line[chroma_above + cx * 2u];
                uint top1 = y == 0u ? 0x7F7F7F7Fu : gs_chroma_line[chroma_above + cx * 2u + 1u];
                uint corner = y == 0u ? 127u : cx == 0u ? 129u
                    : Byte(gs_chroma_line[chroma_above + max(cx * 2u, 1u) - 1u], 3u);
                uint left_word = chroma_left_base + PreviousSlot(chroma_slot) * 2u;
                uint left0 = cx == 0u ? 0x81818181u : gs_chroma_left[left_word];
                uint left1 = cx == 0u ? 0x81818181u : gs_chroma_left[left_word + 1u];
                uint dc = DCValue(SumBytes(top0) + SumBytes(top1), SumBytes(left0) + SumBytes(left1), y != 0u, cx != 0u,
                                  3u);
                uint left_sample = Byte(cr < 4u ? left0 : left1, cr & 3u);
                uvec4 value0 = AddResidual(PredictBig(chroma_mode, left_sample, top0, corner, dc), chroma.xy, chroma_info);
                uvec4 value1 = AddResidual(PredictBig(chroma_mode, left_sample, top1, corner, dc), chroma.zw, chroma_info);
                Store4(chroma_row + cx * 8u, value0);
                Store4(chroma_row + cx * 8u + 4u, value1);
                atomicOr(gs_chroma_left[chroma_left_base + chroma_slot * 2u + (cr >> 2u)], value1.w << ((cr & 3u) * 8u));
                if ((cr & 3u) == 0u) gs_chroma_left[chroma_left_base + NextSlot(chroma_slot) * 2u + (cr >> 2u)] = 0u;
                if (cr == 7u) {
                    gs_chroma_line[chroma_own + cx * 2u] = Pack(value0);
                    gs_chroma_line[chroma_own + cx * 2u + 1u] = Pack(value1);
                }
                ++cx;
                chroma_slot = NextSlot(chroma_slot);
                chroma_entered = true;
            }
            chroma_info = chroma_entered ? chroma_next_info : chroma_info;
            chroma = chroma_entered ? next_chroma : chroma;
            chroma_next_info = MacroblockInfo(cx + 1u, y);
            next_chroma = Load4(ResidualAddress(cx + 1u, y) + chroma_offset);
        }
        if (lane == 0u) gs_chroma_progress[chroma_write + y] = cx;
        // The state before this level (no invocation writes that copy now), acted on after the barrier: a level with
        // nothing left to do is a no-op.
        bool finished = gs_progress[read + last * 4u + 3u] >= end && gs_chroma_progress[chroma_read + last] >= columns;
        memoryBarrierShared();
        barrier();
        if (finished)
            break;
    }
}
