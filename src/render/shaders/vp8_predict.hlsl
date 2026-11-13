// VP8 intra prediction (RFC 6386 12.2-12.3) of a whole keyframe in one group, after vp8_reconstruct.hlsl's
// inverse transforms. Thread row y owns macroblock row y as four sub-rows of 4x4 subblocks, four lanes each
// (one sample row per lane). A sub-row takes its next subblock (all four at once in a 16x16 macroblock) in a
// level once its left and top neighbours exist, and its top-right where its mode reads it, so the sub-rows of
// a row and the rows below run as a staggered pipeline of at most about 4 * columns + 8 * rows levels (half the
// rows' share where no mode reads the top-right); chroma takes one macroblock per level. Edges
// stay in shared memory and each lane keeps its position, modes and residuals (loaded a macroblock ahead) in
// registers: a level is one round of shared reads, the arithmetic, and one barrier.
#define VP8_ROWS 32
// Every level but the last advances some sub-row or chroma row, so 16 * 32 * 32 + 32 * 32 + 1 levels bound
// the loop; a frame takes at most about 4 * columns + 8 * rows.
#define VP8_LEVELS 17409u
// fxc's register budget assumes many resident groups; this is the dispatch's only group, so its registers
// cost no occupancy.
#pragma warning(disable : 4714)

cbuffer Vp8ReconstructionConstants : register(b0) {
    uint macroblock_columns;
    uint macroblock_rows;
    uint luma_stride;
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

// Shared samples are four to a word (sample i in bits 8i..8i+7).
// Luma edges: the bottom row of each macroblock row by row parity (EDGE_LINE, 129 words each; word
// 4 * columns repeats the last sample, the top-right past the right edge), then the bottom rows of sub-rows
// 0..2 of each macroblock row as rings of eight subblock columns (EDGE_INNER).
#define EDGE_LINE 0u
#define EDGE_INNER 258u
groupshared uint gs_edges[258 + VP8_ROWS * 3 * 8];
// Bottom chroma rows: [parity][plane][word].
groupshared uint gs_chroma_line[2 * 2 * 64];
// Right column of each sub-row's last subblock in three slots by subblock column s (read s - 1, write s,
// clear s + 1); the four lanes OR their samples in.
groupshared uint gs_left[VP8_ROWS * 4 * 3];
// Right chroma columns: [row][plane][slot][half].
groupshared uint gs_chroma_left[VP8_ROWS * 2 * 3 * 2];
// Subblocks done per sub-row and chroma macroblocks done per row, by level parity: a level reads one copy
// and writes the other.
groupshared uint gs_progress[2 * VP8_ROWS * 4];
groupshared uint gs_chroma_progress[2 * VP8_ROWS];
groupshared uint gs_dc[VP8_ROWS];
// Per macroblock: luma mode (bits 0-3), chroma mode (4-6), has coefficients (7); four to a word.
groupshared uint gs_info[VP8_ROWS * 8];
// kSubblockCodes, copied once: a constant table indexed per lane would be a memory load in every level.
groupshared uint gs_codes[32];

uint Byte(uint word, uint index) { return (word >> (index * 8u)) & 255u; }
// The sum of the bytes of words: PairSums of each added up (of 128 words at most), then folded.
uint PairSums(uint word) { return (word & 0x00FF00FFu) + ((word >> 8u) & 0x00FF00FFu); }
uint SumBytes(uint pair_sums) { return (pair_sums & 0xFFFFu) + (pair_sums >> 16u); }
uint Pack(uint4 v) { return v.x | (v.y << 8u) | (v.z << 16u) | (v.w << 24u); }

// Intra 4x4 modes 2..9 (RFC 6386 12.3) predict every sample as a two- or three-tap average of the edge
// e(k): k < 0 the left column (e(-1) = left[0], repeated past left[3]), e(0) the corner, k > 0 the top row
// with the top-right (repeated past it). For mode m, row y, sample x, byte x of kSubblockCodes[(m - 2) * 4 + y]
// selects the average: codes 0..15 are (e(k) + e(k + 1) + 1) / 2 and 16..31 (e(k - 1) + 2 e(k) + e(k + 1) + 2) / 4,
// with k = code % 16 - 7. Generated from the per-mode formulas and checked against them for every sample.
static const uint kSubblockCodes[32] = {
    0x1B1A1918u, 0x1B1A1918u, 0x1B1A1918u, 0x1B1A1918u, 0x16161616u, 0x15151515u, 0x14141414u, 0x13131313u,
    0x1A191817u, 0x19181716u, 0x18171615u, 0x17161514u, 0x0A090807u, 0x1A191817u, 0x09080716u, 0x19181715u,
    0x1C1B1A19u, 0x1D1C1B1Au, 0x1E1D1C1Bu, 0x1F1E1D1Cu, 0x0B0A0908u, 0x1C1B1A19u, 0x1D0B0A09u, 0x1E1C1B1Au,
    0x19181706u, 0x17061605u, 0x16051504u, 0x15041403u, 0x14041505u, 0x13031404u, 0x12021303u, 0x11011202u};

// Row y of a 4x4 subblock in `mode` from its left column, corner, top row and top-right.
uint4 PredictSubblockRow(uint mode, uint y, uint left, uint corner, uint top, uint top_right) {
    if (mode == 0u) return (SumBytes(PairSums(top) + PairSums(left)) + 4u) >> 3u;
    if (mode == 1u) {
        const int4 top_samples = int4(Byte(top, 0u), Byte(top, 1u), Byte(top, 2u), Byte(top, 3u));
        return uint4(clamp(top_samples + (int(Byte(left, y)) - int(corner)), 0, 255));
    }
    // The edge as a byte stream from e(-8): left reversed, corner, top, top-right, then repeats. A sample's
    // window is the three bytes e(k - 1), e(k), e(k + 1), from byte code % 16 of the stream.
    const uint e0 = (left >> 24u) * 0x01010101u;
    const uint e1 = (left >> 24u) | ((left >> 8u) & 0xFF00u) | ((left << 8u) & 0xFF0000u) | (left << 24u);
    const uint e2 = corner | (top << 8u), e3 = (top >> 24u) | (top_right << 8u), e4 = (top_right >> 24u) * 0x01010101u;
    const uint codes = gs_codes[min(mode - 2u, 7u) * 4u + y];
    const uint4 code = (uint4(codes, codes, codes, codes) >> uint4(0u, 8u, 16u, 24u)) & 255u;
    const uint4 word = (code >> 2u) & 3u, shift = (code & 3u) * 8u;
    const uint4 low = word == 0u ? e0 : word == 1u ? e1 : word == 2u ? e2 : e3;
    const uint4 high = word == 0u ? e1 : word == 1u ? e2 : word == 2u ? e3 : e4;
    // A shift by 32 has to drop the higher word: in two steps, as a shift counts modulo 32.
    const uint4 window = (low >> shift) | ((high << (31u - shift)) << 1u);
    // The two-tap average is the three-tap one with e(k + 1) in place of e(k - 1).
    const uint4 after = (window >> 16u) & 255u, before = code < 16u ? after : window & 255u;
    return (before + 2u * ((window >> 8u) & 255u) + after + 2u) >> 2u;
}

// 16x16 and chroma prediction (RFC 6386 12.2) of the four samples under `top` from the left sample, the
// corner and the DC value.
uint4 PredictBig(uint mode, uint left, uint top, uint corner, uint dc) {
    const int4 top_samples = int4(Byte(top, 0u), Byte(top, 1u), Byte(top, 2u), Byte(top, 3u));
    if (mode == 1u) return uint4(clamp(top_samples + (int(left) - int(corner)), 0, 255));
    return mode == 2u ? uint4(top_samples) : mode == 0u ? dc : mode == 3u ? left : 128u;
}
// The 16x16 or 8x8 DC value from the sums of the edges that exist.
uint DCValue(uint top_sum, uint left_sum, bool has_top, bool has_left, uint shift) {
    if (!has_top && !has_left) return 128u;
    const uint both = has_top && has_left ? 1u : 0u;
    const uint sum = (has_top ? top_sum : 0u) + (has_left ? left_sum : 0u);
    return (sum + (1u << (shift - 1u + both))) >> (shift + both);
}

uint MacroblockInfo(uint x, uint y) { return x < macroblock_columns ? Byte(gs_info[y * 8u + (x >> 2u)], x & 3u) : 0u; }
// Residuals (vp8_reconstruct.hlsl) follow the planes: per macroblock 192 words of raster rows, two signed
// 16-bit samples to a word. A lane loads them a macroblock ahead, whether or not that one has coefficients,
// clamped into the image; a macroblock without coefficients masks its stale residuals at use.
uint ResidualAddress(uint x, uint y) {
    const uint mb = min(y, macroblock_rows - 1u) * macroblock_columns + min(x, macroblock_columns - 1u);
    return (v_plane_offset + chroma_stride * chroma_rows + mb * 192u) * 4u;
}
uint4 AddResidual(uint4 prediction, uint2 packed, uint info) {
    const int4 residual = (info & 128u) != 0u ? int4(asint(packed.x << 16u) >> 16, asint(packed.x) >> 16,
                                                      asint(packed.y << 16u) >> 16, asint(packed.y) >> 16) : 0;
    return uint4(clamp(int4(prediction) + residual, 0, 255));
}
uint2 Residual(uint4 low, uint4 high, uint k) {
    return k == 0u ? low.xy : k == 1u ? low.zw : k == 2u ? high.xy : high.zw;
}
uint NextSlot(uint slot) { return slot == 2u ? 0u : slot + 1u; }
uint PreviousSlot(uint slot) { return slot == 0u ? 2u : slot - 1u; }

[numthreads(16, VP8_ROWS, 1)]
void vp8_reconstruct(uint3 tid : SV_GroupThreadID) {
    const uint lane = tid.x, y = tid.y;
    const uint columns = macroblock_columns, end = columns * 4u, last = macroblock_rows - 1u;
    const bool row_active = y < macroblock_rows;
    if (lane < 8u) {
        uint packed = 0u;
        [unroll] for (uint column = 0u; column < 4u; ++column) {
            const uint x = lane * 4u + column;
            if (row_active && x < columns) {
                const uint mb = y * columns + x;
                // Mode records are word multiples: luma mode in byte 16, chroma mode in byte 17.
                const uint modes = vp8_modes.Load(mb * mode_record_bytes + 16u);
                // Coefficient records are word multiples too: the flag is the low byte of word 192.
                const uint has_coefficients = vp8_coefficients.Load(mb * coefficient_record_bytes + 768u) & 255u;
                packed |= ((modes & 15u) | ((Byte(modes, 1u) & 7u) << 4u) | (has_coefficients != 0u ? 128u : 0u))
                          << (column * 8u);
            }
        }
        gs_info[y * 8u + lane] = packed;
    }
    if (y < 2u) gs_codes[y * 16u + lane] = kSubblockCodes[y * 16u + lane];
    if (lane < 12u) {
        gs_left[y * 12u + lane] = 0u;
        gs_chroma_left[y * 12u + lane] = 0u;
    }
    if (lane < 4u) gs_progress[y * 4u + lane] = row_active ? 0u : end;
    if (lane == 0u) gs_chroma_progress[y] = row_active ? 0u : columns;
    GroupMemoryBarrierWithGroupSync();

    // Luma: sub-row `by`, sample row `py`. Chroma: plane `plane`, sample row `cr`.
    const uint by = lane >> 2u, py = lane & 3u;
    const uint plane = lane >> 3u, cr = lane & 7u;
    const uint line_above = EDGE_LINE + ((y - 1u) & 1u) * 129u, line_own = EDGE_LINE + (y & 1u) * 129u;
    const uint inner_above = EDGE_INNER + (y * 3u + by - 1u) * 8u, inner_own = EDGE_INNER + (y * 3u + by) * 8u;
    const uint left_base = (y * 4u + by) * 3u;
    const uint chroma_above = ((((y - 1u) & 1u) * 2u) + plane) * 64u, chroma_own = ((y & 1u) * 2u + plane) * 64u;
    const uint chroma_left_base = (y * 2u + plane) * 6u;
    const uint luma_row = (y * 16u + by * 4u + py) * luma_stride;
    const uint chroma_row = (plane == 0u ? u_plane_offset : v_plane_offset) + (y * 8u + cr) * chroma_stride;
    const uint luma_offset = (by * 4u + py) * 32u, chroma_offset = 512u + plane * 128u + cr * 16u;

    // This sub-row: its next subblock s (s % 3 is its left slot), the macroblock's info, modes and residual
    // row, and the next macroblock's. Chroma: its next macroblock cx likewise.
    uint s = row_active ? 0u : end, slot = 0u;
    uint info = MacroblockInfo(0u, y), next_info = MacroblockInfo(1u, y);
    uint modes = vp8_modes.Load(min(y, last) * columns * mode_record_bytes + by * 4u);
    uint next_modes = vp8_modes.Load((min(y, last) * columns + min(1u, columns - 1u)) * mode_record_bytes + by * 4u);
    uint4 low = vp8_yuv.Load4(ResidualAddress(0u, y) + luma_offset);
    uint4 high = vp8_yuv.Load4(ResidualAddress(0u, y) + luma_offset + 16u);
    uint4 next_low = vp8_yuv.Load4(ResidualAddress(1u, y) + luma_offset);
    uint4 next_high = vp8_yuv.Load4(ResidualAddress(1u, y) + luma_offset + 16u);
    uint cx = row_active ? 0u : columns, chroma_slot = 0u;
    uint chroma_info = info, chroma_next_info = next_info;
    uint4 chroma = vp8_yuv.Load4(ResidualAddress(0u, y) + chroma_offset);
    uint4 next_chroma = vp8_yuv.Load4(ResidualAddress(1u, y) + chroma_offset);

    [loop] for (uint level = 0u; level < VP8_LEVELS; ++level) {
        const uint read = (level & 1u) * (VP8_ROWS * 4u), write = (VP8_ROWS * 4u) - read;
        const uint chroma_read = (level & 1u) * VP8_ROWS, chroma_write = VP8_ROWS - chroma_read;
        // A finished (or absent) sub-row only republishes its progress.
        if (s < end) {
            // Shared reads first: their addresses come from registers only.
            const uint x = s >> 2u, bx = s & 3u, luma_mode = info & 15u;
            const bool big = luma_mode != 10u;
            const uint above = y == 0u ? end : gs_progress[read + y * 4u - 1u];
            const uint previous = by == 0u ? above : gs_progress[read + y * 4u + by - 1u];
            const uint next = by == 3u ? end + 8u : gs_progress[read + y * 4u + by + 1u];
            const uint left_done = gs_progress[read + y * 4u + 3u];
            const uint first_done = gs_progress[read + y * 4u], second_done = gs_progress[read + y * 4u + 1u];
            const uint left = s == 0u ? 0x81818181u : gs_left[left_base + PreviousSlot(slot)];
            // A 16x16 sub-row reads the macroblock's top row and corner; a 4x4 one its own top, top-right and
            // corner (the right column's top-right is the macroblock's, from the row above).
            const bool from_line = by == 0u || big;
            const uint top_word = gs_edges[from_line ? line_above + s : inner_above + (s & 7u)];
            const uint top_right_word = gs_edges[from_line || bx == 3u ? line_above + s + 1u
                                                                       : inner_above + ((s + 1u) & 7u)];
            const uint corner_word = gs_edges[from_line ? line_above + max(s, 1u) - 1u : inner_above + ((s - 1u) & 7u)];
            const bool top_border = y == 0u && from_line;
            const uint top = top_border ? 0x7F7F7F7Fu : top_word;
            const uint top_right = y == 0u && (from_line || bx == 3u) ? 0x7F7F7F7Fu : top_right_word;
            const uint corner = top_border ? 127u : s == 0u ? 129u : Byte(corner_word, 3u);

            uint advance = 0u, right = 0u;
            if (!big) {
                // Left and top done, and the top-right where the mode reads it (2, 6 and 7: vertical, and the two
                // that run down to the left); the right column's top-right is the macroblock's, from the row above.
                // A sub-row that writes the line stays two subblocks behind the row above, which reads the same
                // line as its top; the sub-row below may lag at most six subblocks (its edge ring).
                const uint mode = Byte(modes, bx);
                const bool reads_top_right = mode == 2u || mode == 6u || mode == 7u;
                const bool line_top_right = by != 0u && bx == 3u;
                if (previous >= min(s + (reads_top_right && !line_top_right ? 2u : 1u), end) &&
                    ((by != 3u && !(reads_top_right && line_top_right)) || above >= min(s + 2u, end)) &&
                    s + 1u <= next + 7u) {
                    const uint4 value = AddResidual(PredictSubblockRow(mode, py, left, corner, top, top_right),
                                                    Residual(low, high, bx), info);
                    vp8_yuv.Store4((luma_row + s * 4u) * 4u, value);
                    right = value.w;
                    if (py == 3u) {
                        gs_edges[by != 3u ? inner_own + (s & 7u) : line_own + s] = Pack(value);
                        if (by == 3u && s + 1u == end) gs_edges[line_own + end] = value.w * 0x01010101u;
                    }
                    advance = 1u;
                }
            } else if ((first_done > s || (first_done == s && above >= min(s + 5u, end) && s <= second_done + 3u)) &&
                       previous >= s && (luma_mode != 0u || left_done >= s) && s + 4u <= next + 7u) {
                // A 16x16 sub-row needs only the macroblock's edges, DC every sub-row of the left macroblock.
                // The first sub-row waits for the top-right macroblock's first subblock like a 4x4 one, which
                // keeps the corner alive, and for its edge ring; the others go in the level it goes (each
                // repeats its test on the same progress) when they stand beside it, else once it has gone,
                // never ahead of the sub-row above (the row below reuses the row above's line).
                uint top_words[4];
                top_words[0] = top;
                [unroll] for (uint word = 1u; word < 4u; ++word)
                    top_words[word] = y == 0u ? 0x7F7F7F7Fu : gs_edges[line_above + s + word];
                // The DC sum: the top row and the left macroblock's right column (each sub-row's left slot
                // still holds it while the first sub-row is here). A sub-row that follows later reuses the first's.
                uint dc = 128u;
                if (luma_mode == 0u) {
                    if (first_done == s) {
                        uint left_sums = 0u;
                        [unroll] for (uint sub_row = 0u; sub_row < 4u; ++sub_row)
                            left_sums += PairSums(gs_left[y * 12u + sub_row * 3u + PreviousSlot(slot)]);
                        const uint top_sums = PairSums(top_words[0]) + PairSums(top_words[1]) +
                                              PairSums(top_words[2]) + PairSums(top_words[3]);
                        dc = DCValue(SumBytes(top_sums), SumBytes(left_sums), y != 0u, x != 0u, 4u);
                        if (lane == 0u) gs_dc[y] = dc;
                    } else {
                        dc = gs_dc[y];
                    }
                }
                [unroll] for (uint k = 0u; k < 4u; ++k) {
                    const uint4 value = AddResidual(PredictBig(luma_mode, Byte(left, py), top_words[k], corner, dc),
                                                    Residual(low, high, k), info);
                    vp8_yuv.Store4((luma_row + (s + k) * 4u) * 4u, value);
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
                InterlockedOr(gs_left[left_base + slot], right << (py * 8u));
                if (py == 0u) gs_left[left_base + NextSlot(slot)] = 0u;
                s += advance;
                slot = NextSlot(slot);
            }
            // Entering a macroblock takes the data loaded for it a level or more ago and loads the next
            // macroblock's, so no load is waited for where it is used.
            [branch] if (advance != 0u && (s & 3u) == 0u) {
                const uint next_x = min((s >> 2u) + 1u, columns - 1u);
                info = next_info;
                modes = next_modes;
                low = next_low;
                high = next_high;
                next_info = MacroblockInfo((s >> 2u) + 1u, y);
                next_modes = vp8_modes.Load((min(y, last) * columns + next_x) * mode_record_bytes + by * 4u);
                next_low = vp8_yuv.Load4(ResidualAddress(next_x, y) + luma_offset);
                next_high = vp8_yuv.Load4(ResidualAddress(next_x, y) + luma_offset + 16u);
            }
        }
        if (py == 0u) gs_progress[write + y * 4u + by] = s;

        // Chroma: one macroblock per level once the row above is two macroblocks ahead (its top and the
        // corner of the next one stay in the line until then).
        if (cx < columns) {
            if (y == 0u || gs_chroma_progress[chroma_read + y - 1u] >= min(cx + 2u, columns)) {
                const uint chroma_mode = (chroma_info >> 4u) & 7u;
                const uint top0 = y == 0u ? 0x7F7F7F7Fu : gs_chroma_line[chroma_above + cx * 2u];
                const uint top1 = y == 0u ? 0x7F7F7F7Fu : gs_chroma_line[chroma_above + cx * 2u + 1u];
                const uint corner = y == 0u ? 127u : cx == 0u ? 129u
                    : Byte(gs_chroma_line[chroma_above + max(cx * 2u, 1u) - 1u], 3u);
                const uint left_word = chroma_left_base + PreviousSlot(chroma_slot) * 2u;
                const uint left0 = cx == 0u ? 0x81818181u : gs_chroma_left[left_word];
                const uint left1 = cx == 0u ? 0x81818181u : gs_chroma_left[left_word + 1u];
                const uint dc = DCValue(SumBytes(PairSums(top0) + PairSums(top1)),
                                        SumBytes(PairSums(left0) + PairSums(left1)), y != 0u, cx != 0u, 3u);
                const uint left_sample = Byte(cr < 4u ? left0 : left1, cr & 3u);
                // The row's two words, one after the other: the same speed as side by side, and half the code.
                uint4 value;
                [loop] for (uint part = 0u; part < 2u; ++part) {
                    value = AddResidual(PredictBig(chroma_mode, left_sample, part == 0u ? top0 : top1, corner, dc),
                                        part == 0u ? chroma.xy : chroma.zw, chroma_info);
                    vp8_yuv.Store4((chroma_row + cx * 8u + part * 4u) * 4u, value);
                    if (cr == 7u) gs_chroma_line[chroma_own + cx * 2u + part] = Pack(value);
                }
                InterlockedOr(gs_chroma_left[chroma_left_base + chroma_slot * 2u + (cr >> 2u)],
                              value.w << ((cr & 3u) * 8u));
                if ((cr & 3u) == 0u) gs_chroma_left[chroma_left_base + NextSlot(chroma_slot) * 2u + (cr >> 2u)] = 0u;
                ++cx;
                chroma_slot = NextSlot(chroma_slot);
                chroma_info = chroma_next_info;
                chroma = next_chroma;
                chroma_next_info = MacroblockInfo(cx + 1u, y);
                next_chroma = vp8_yuv.Load4(ResidualAddress(cx + 1u, y) + chroma_offset);
            }
        }
        if (lane == 0u) gs_chroma_progress[chroma_write + y] = cx;
        // The state before this level (no thread writes that copy now), acted on after the barrier: a level
        // with nothing left to do is a no-op.
        const bool finished =
            gs_progress[read + last * 4u + 3u] >= end && gs_chroma_progress[chroma_read + last] >= columns;
        GroupMemoryBarrierWithGroupSync();
        if (finished) break;
    }
}
