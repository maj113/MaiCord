// twin: src/render/shaders/vp8_filter.hlsl c5a9ee884169d643
// vp8_filter (STAGE_CS): the VP8 loop filter over the reconstructed planes, macroblocks on anti-diagonal steps.
layout(local_size_x = 16, local_size_y = 32) in;

layout(std140, binding = 0) uniform Vp8FilterConstants {
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

layout(std430, binding = 0) readonly buffer Modes { uint vp8_modes[]; }; //! t0
layout(std430, binding = 1) readonly buffer Coefficients { uint vp8_coefficients[]; }; //! t1
layout(std430, binding = 2) buffer Yuv { uint vp8_yuv[]; }; //! u0

// The VP8 loop filter (RFC 6386 section 15), bit for bit the serial raster order. Raster order filters a macroblock's
// vertical edges, then its horizontal edges. A vertical pass touches its macroblock's rows alone (and the three columns
// to the left), a horizontal pass its columns alone (and the three rows above), so the passes of the macroblocks on one
// anti-diagonal x + y touch disjoint samples, and every sample still sees raster order: the horizontal pass of (x, y)
// comes a step after the vertical pass of (x + 1, y - 1), which rewrote the right three columns of the macroblock
// above. A lane filters one line of a pass, all its edges in order, in registers. Thread row y holds macroblock row y:
// a step is the vertical pass of a diagonal's macroblocks or their horizontal pass, so each thread runs one pass a
// step. The lines cross between a macroblock's passes, and to the row below, in group memory (30208 bytes in all), four
// samples to a word:
// tile:   the macroblock after its vertical pass, by rows (luma 16 x 4 words, U and V 8 x 2);
// left:   its right four columns after its horizontal pass, by columns (luma 4 x 4, U and V 4 x 2);
// top:    its bottom four rows after its horizontal pass, a word per column (luma 16, U and V 8), by diagonal parity
//         (the row below reads a diagonal's while the row writes the next);
// corner: the bottom four rows of the left neighbour's right four columns after this macroblock's vertical pass (luma
//         4 words, U and V 4), read in the same diagonal's horizontal pass.
// Each sample is written to vp8_yuv once, by the last pass that can change it, and a pass reads the planes only where
// nothing has written yet, so group barriers suffice.
// What a thread carries from one step to the next it carries unconditionally: WARP stalls for minutes on a value that
// only some steps assign (a macroblock's limits kept from its vertical to its horizontal pass), so each pass reads them
// again.
shared uint macroblock_info[1024];
shared uint tile[32 * 96];
shared uint left_band[32 * 32];
shared uint top_band[2 * 32 * 32];
shared uint corner[32 * 12];

int signed_clip1(int value) {
    return clamp(value, -128, 127);
}

int signed_clip2(int value) {
    return clamp(value, -16, 15);
}

uint pack4(ivec4 v) {
    uvec4 shifted = uvec4(v) << uvec4(0u, 8u, 16u, 24u);
    return (shifted.x | shifted.y) | (shifted.z | shifted.w);
}

ivec4 unpack4(uint word) {
    return ivec4((uvec4(word, word, word, word) >> uvec4(0u, 8u, 16u, 24u)) & 255u);
}

ivec4 bytes_at(uint w0, uint w1, uint w2, uint w3, uint index) {
    return ivec4((uvec4(w0, w1, w2, w3) >> (index * 8u)) & 255u);
}

ivec4 load4(uint index) {
    return ivec4(vp8_yuv[index], vp8_yuv[index + 1u], vp8_yuv[index + 2u], vp8_yuv[index + 3u]);
}

// Three or four samples `pitch` bytes apart from byte `address`.
void store_three(uint address, uint pitch, ivec3 values) {
    vp8_yuv[address >> 2u] = uint(values.x);
    vp8_yuv[(address + pitch) >> 2u] = uint(values.y);
    vp8_yuv[(address + 2u * pitch) >> 2u] = uint(values.z);
}

void store_four(uint address, uint pitch, ivec4 values) {
    store_three(address, pitch, values.xyz);
    vp8_yuv[(address + 3u * pitch) >> 2u] = uint(values.w);
}

// Filters one line across the inner edge between p0 and q0 (RFC 6386 section 15) in place. An edge_limit of -1 leaves
// the line as it is.
void filter_line(int p3, int p2, inout int p1, inout int p0, inout int q0, inout int q1, int q2, int q3,
                 int edge_limit, int interior_limit, int hev_threshold, bool simple) {
    bool apply = 4 * abs(p0 - q0) + abs(p1 - q1) <= 2 * edge_limit + 1;
    bool hev = true;
    if (!simple) {
        int interior = max(max(max(abs(p3 - p2), abs(p2 - p1)), max(abs(p1 - p0), abs(q3 - q2))),
                           max(abs(q2 - q1), abs(q1 - q0)));
        apply = apply && interior <= interior_limit;
        hev = max(abs(p1 - p0), abs(q1 - q0)) > hev_threshold;
    }
    // The common adjustment (the simple filter, and edges of high variance) moves p0 and q0; an edge of low variance
    // moves two samples a side.
    int base = 3 * (q0 - p0) + (hev ? signed_clip1(p1 - q1) : 0);
    int adjustment1 = signed_clip2((base + 4) >> 3);
    int adjustment2 = signed_clip2((base + 3) >> 3);
    p0 = clamp(p0 + (apply ? adjustment2 : 0), 0, 255);
    q0 = clamp(q0 - (apply ? adjustment1 : 0), 0, 255);
    if (!simple) {
        int change1 = apply && !hev ? (adjustment1 + 1) >> 1 : 0;
        p1 = clamp(p1 + change1, 0, 255);
        q1 = clamp(q1 - change1, 0, 255);
    }
}

// A luma line (x) and a chroma line (y) at once across a macroblock edge or an inner one: lp and cp hold p3..p0, lq and
// cq q0..q3. A macroblock edge of low variance moves three samples a side. The simple filter sets the chroma limit to
// -1.
void filter_pair(inout ivec4 lp, inout ivec4 lq, inout ivec4 cp, inout ivec4 cq, ivec2 edge_limit, int interior_limit,
                 int hev_threshold, bool macroblock_edge, bool simple) {
    ivec2 p3 = ivec2(lp.x, cp.x), q3 = ivec2(lq.w, cq.w);
    ivec2 p2 = ivec2(lp.y, cp.y), p1 = ivec2(lp.z, cp.z), p0 = ivec2(lp.w, cp.w);
    ivec2 q0 = ivec2(lq.x, cq.x), q1 = ivec2(lq.y, cq.y), q2 = ivec2(lq.z, cq.z);
    bvec2 apply = lessThanEqual(4 * abs(p0 - q0) + abs(p1 - q1), 2 * edge_limit + 1);
    bvec2 hev = bvec2(true);
    if (!simple) {
        ivec2 interior = max(max(max(abs(p3 - p2), abs(p2 - p1)), max(abs(p1 - p0), abs(q3 - q2))),
                             max(abs(q2 - q1), abs(q1 - q0)));
        apply = bvec2(apply.x && interior.x <= interior_limit, apply.y && interior.y <= interior_limit);
        hev = greaterThan(max(abs(p1 - p0), abs(q1 - q0)), ivec2(hev_threshold));
    }
    bvec2 strong = macroblock_edge ? not(hev) : bvec2(false);
    ivec2 base = 3 * (q0 - p0) + mix(ivec2(0), clamp(p1 - q1, -128, 127), macroblock_edge ? bvec2(true) : hev);
    ivec2 adjustment1 = clamp((base + 4) >> 3, -16, 15);
    ivec2 adjustment2 = clamp((base + 3) >> 3, -16, 15);
    ivec2 weight = clamp(base, -128, 127);
    ivec2 strong0 = (27 * weight + 63) >> 7;
    ivec2 p0_change = mix(adjustment2, strong0, strong);
    ivec2 q0_change = mix(adjustment1, strong0, strong);
    ivec2 change1 = mix(mix((adjustment1 + 1) >> 1, ivec2(0), hev), (18 * weight + 63) >> 7, strong);
    ivec2 change2 = mix(ivec2(0), (9 * weight + 63) >> 7, strong);
    p0_change = mix(ivec2(0), p0_change, apply);
    q0_change = mix(ivec2(0), q0_change, apply);
    change1 = mix(ivec2(0), change1, apply);
    change2 = mix(ivec2(0), change2, apply);
    p0 = clamp(p0 + p0_change, 0, 255);
    q0 = clamp(q0 - q0_change, 0, 255);
    if (!simple) {
        p1 = clamp(p1 + change1, 0, 255);
        q1 = clamp(q1 - change1, 0, 255);
        if (macroblock_edge) {
            p2 = clamp(p2 + change2, 0, 255);
            q2 = clamp(q2 - change2, 0, 255);
        }
    }
    lp.yzw = ivec3(p2.x, p1.x, p0.x);
    cp.yzw = ivec3(p2.y, p1.y, p0.y);
    lq.xyz = ivec3(q0.x, q1.x, q2.x);
    cq.xyz = ivec3(q0.y, q1.y, q2.y);
}

int segment_filter(uint segment) {
    return segment == 0u ? segment_filter0 : segment == 1u ? segment_filter1 : segment == 2u ? segment_filter2
                                                                                            : segment_filter3;
}

// A macroblock's filter level (0-63) and luma mode from its mode record.
int macroblock_level(uint macroblock, out uint luma_mode) {
    // Mode and coefficient records are word multiples: luma mode in byte 16, segment in byte 18.
    uint modes = vp8_modes[(macroblock * mode_record_bytes + 16u) >> 2u];
    luma_mode = modes & 255u;
    int level = int(filter_level);
    if (segmentation_enabled != 0u) {
        int strength = segment_filter((modes >> 16) & 255u);
        level = segment_absolute_delta != 0u ? strength : level + strength;
    }
    if (use_filter_delta != 0u) {
        level += reference_delta0;
        if (luma_mode == 10u)
            level += mode_delta0;
    }
    return clamp(level, 0, 63);
}

// What a pass needs of its macroblock, a word of macroblock_info: the macroblock edges' limit + 1 (bits 0-7; 0 when the
// macroblock is not filtered), the inner edges' limit + 1 (bits 8-15; 0 without them), the interior limit (bits 16-21),
// the high edge variance threshold (bits 22-23), and which neighbours filter, and so rewrite samples of this macroblock
// or its left neighbour: bit 24 (x + 1, y) its right three columns, bit 25 (x, y + 1) its bottom three rows, bit 26
// (x - 1, y + 1) the left neighbour's bottom three rows.
uint macroblock_word(uint macroblock) {
    uint x = macroblock % macroblock_columns, y = macroblock / macroblock_columns;
    uint word = 0u;
    for (uint which = 0u; which < 4u; ++which) {
        // Itself, then the neighbours of bits 24, 25 and 26; x - 1 wraps past the picture.
        uint other_x = x + (which == 1u ? 1u : which == 3u ? ~0u : 0u), other_y = y + (which >> 1);
        uint luma_mode;
        int level = macroblock_level(other_y * macroblock_columns + other_x, luma_mode);
        if (other_x < macroblock_columns && other_y < macroblock_rows && level != 0) {
            if (which == 0u) {
                int interior_limit = level;
                if (sharpness != 0u) {
                    interior_limit >>= sharpness > 4u ? 2 : 1;
                    interior_limit = min(interior_limit, 9 - int(sharpness));
                }
                interior_limit = max(interior_limit, 1);
                bool inner = luma_mode == 10u ||
                             (vp8_coefficients[(macroblock * coefficient_record_bytes + 768u) >> 2u] & 255u) != 0u;
                uint limit = uint(2 * level + interior_limit) + 1u;
                word = (limit + 4u) | (inner ? limit << 8 : 0u) | (uint(interior_limit) << 16) |
                       (level >= 40 ? 2u << 22 : level >= 15 ? 1u << 22 : 0u);
            } else {
                word |= 1u << (23u + which);
            }
        }
    }
    return word;
}

struct Slot {
    uint x;
    uint y;
    int edge_limit;     // the macroblock edges' limit, -1 when the macroblock is not filtered
    int subblock_limit; // the inner edges' limit, -1 without them
    int interior_limit;
    int hev_threshold;
    uint neighbours; // bits 0-2: macroblock_info's bits 24-26
};

Slot macroblock_slot(uint x, uint y) {
    uint word = macroblock_info[y * macroblock_columns + x];
    Slot slot;
    slot.x = x;
    slot.y = y;
    slot.edge_limit = int(word & 255u) - 1;
    slot.subblock_limit = int((word >> 8) & 255u) - 1;
    slot.interior_limit = int((word >> 16) & 63u);
    slot.hev_threshold = int((word >> 22) & 3u);
    slot.neighbours = word >> 24;
    return slot;
}

// One pass over a macroblock. Lane i filters luma line i and (outside the simple filter, which leaves chroma
// alone) line i & 7 of U (lanes 0-7) or V (lanes 8-15): rows for the vertical edges (across), columns for the
// horizontal ones. A line is the neighbour's four samples (a, ca) and the macroblock's sixteen (b..e) or eight (cb, cc).
void filter_pass(Slot slot, bool across, uint parity, uint lane, bool simple) {
    uint plane = lane >> 3;
    uint chroma_line = lane & 7u;
    bool outer = across ? slot.x != 0u : slot.y != 0u;
    int edge_limit = outer ? slot.edge_limit : -1;
    uint luma = across ? (slot.y * 16u + lane) * luma_stride + slot.x * 16u
                       : slot.y * 16u * luma_stride + slot.x * 16u + lane;
    uint luma_step = across ? 1u : luma_stride;
    uint chroma = (plane == 0u ? u_plane_offset : v_plane_offset) +
                  (across ? (slot.y * 8u + chroma_line) * chroma_stride + slot.x * 8u
                          : slot.y * 8u * chroma_stride + slot.x * 8u + chroma_line);
    uint chroma_step = across ? 1u : chroma_stride;
    uint luma_band = slot.y * 32u;
    uint chroma_band = slot.y * 32u + 16u + plane * 8u;
    uint luma_tile = slot.y * 96u;
    uint chroma_tile = slot.y * 96u + 64u + plane * 16u;
    ivec4 a, b, c, d, e, ca, cb, cc;
    if (across) {
        // The macroblock's rows are untouched until now; the left neighbour's columns come from its horizontal pass.
        b = load4(luma);
        c = load4(luma + 4u);
        d = load4(luma + 8u);
        e = load4(luma + 12u);
        cb = load4(chroma);
        cc = load4(chroma + 4u);
        uint band = luma_band + (lane >> 2);
        a = bytes_at(left_band[band], left_band[band + 4u], left_band[band + 8u], left_band[band + 12u], lane & 3u);
        uint chroma_word = chroma_band + (chroma_line >> 2);
        ca = bytes_at(left_band[chroma_word], left_band[chroma_word + 2u], left_band[chroma_word + 4u],
                      left_band[chroma_word + 6u], chroma_line & 3u);
    } else {
        // The rows above come from the row above: its last horizontal pass, and in the right four columns its
        // vertical pass just now, when it had a macroblock to the right.
        a = ivec4(0);
        ca = ivec4(0);
        if (outer) {
            bool fresh_corner = slot.x + 1u < macroblock_columns;
            uint above = (slot.y - 1u) * 12u;
            uint above_top = (parity ^ 1u) * 1024u + (slot.y - 1u) * 32u;
            a = lane >= 12u && fresh_corner
                    ? bytes_at(corner[above], corner[above + 1u], corner[above + 2u], corner[above + 3u], lane - 12u)
                    : unpack4(top_band[above_top + lane]);
            uint chroma_corner = above + 4u + plane * 4u;
            ca = chroma_line >= 4u && fresh_corner
                     ? bytes_at(corner[chroma_corner], corner[chroma_corner + 1u], corner[chroma_corner + 2u],
                                corner[chroma_corner + 3u], chroma_line - 4u)
                     : unpack4(top_band[above_top + 16u + plane * 8u + chroma_line]);
        }
        uint word = luma_tile + (lane >> 2);
        uint shift = lane & 3u;
        b = bytes_at(tile[word], tile[word + 4u], tile[word + 8u], tile[word + 12u], shift);
        c = bytes_at(tile[word + 16u], tile[word + 20u], tile[word + 24u], tile[word + 28u], shift);
        d = bytes_at(tile[word + 32u], tile[word + 36u], tile[word + 40u], tile[word + 44u], shift);
        e = bytes_at(tile[word + 48u], tile[word + 52u], tile[word + 56u], tile[word + 60u], shift);
        uint chroma_word = chroma_tile + (chroma_line >> 2);
        uint chroma_shift = chroma_line & 3u;
        cb = bytes_at(tile[chroma_word], tile[chroma_word + 2u], tile[chroma_word + 4u], tile[chroma_word + 6u],
                      chroma_shift);
        cc = bytes_at(tile[chroma_word + 8u], tile[chroma_word + 10u], tile[chroma_word + 12u], tile[chroma_word + 14u],
                      chroma_shift);
    }

    // The edges in order along the line. The pair filtered slides a group of four samples an edge, the groups behind it
    // turning through the registers ahead: the macroblock edge and the first inner edge, luma and chroma together,
    // leave c a b in a b c and cc ca cb in ca cb cc; then luma's other two.
    for (uint paired = 0u; paired < 2u; ++paired) {
        int limit = paired == 0u ? edge_limit : slot.subblock_limit;
        filter_pair(a, b, ca, cb, ivec2(limit, simple ? -1 : limit), slot.interior_limit, slot.hev_threshold,
                    paired == 0u, simple);
        ivec4 luma_done = a, chroma_done = ca;
        a = b; b = c; c = luma_done;
        ca = cb; cb = cc; cc = chroma_done;
    }
    ivec4 before = a, after = d, ahead = e;
    for (uint single = 0u; single < 2u; ++single) {
        filter_line(before.x, before.y, before.z, before.w, after.x, after.y, after.z, after.w, slot.subblock_limit,
                    slot.interior_limit, slot.hev_threshold, simple);
        ivec4 done = before;
        before = after; after = ahead; ahead = done;
    }
    a = b; b = c; c = after; d = ahead; e = before;
    ivec4 chroma_first = cb;
    cb = cc; cc = ca; ca = chroma_first;

    if (across) {
        tile[luma_tile + lane * 4u] = pack4(b);
        tile[luma_tile + lane * 4u + 1u] = pack4(c);
        tile[luma_tile + lane * 4u + 2u] = pack4(d);
        tile[luma_tile + lane * 4u + 3u] = pack4(e);
        tile[chroma_tile + chroma_line * 2u] = pack4(cb);
        tile[chroma_tile + chroma_line * 2u + 1u] = pack4(cc);
        if (lane >= 12u)
            corner[slot.y * 12u + lane - 12u] = pack4(a);
        if (chroma_line >= 4u)
            corner[slot.y * 12u + 4u + plane * 4u + chroma_line - 4u] = pack4(ca);
    } else {
        uint top = parity * 1024u + slot.y * 32u;
        top_band[top + lane] = pack4(e);
        top_band[top + 16u + plane * 8u + chroma_line] = pack4(cc);
        if (lane >= 12u) {
            uint band = luma_band + (lane - 12u) * 4u;
            left_band[band] = pack4(b);
            left_band[band + 1u] = pack4(c);
            left_band[band + 2u] = pack4(d);
            left_band[band + 3u] = pack4(e);
        }
        if (chroma_line >= 4u) {
            uint band = chroma_band + (chroma_line - 4u) * 2u;
            left_band[band] = pack4(cb);
            left_band[band + 1u] = pack4(cc);
        }
    }

    // Each sample goes to memory once, from the last pass that can change it. The neighbour's three samples are final
    // after this pass, except that (x - 1, y + 1) rewrites the bottom three rows of the left neighbour. The
    // macroblock's own samples wait for its horizontal pass, its right three columns for (x + 1, y) when that filters,
    // its bottom three rows for (x, y + 1).
    uint luma_address = luma * 4u, luma_pitch = luma_step * 4u;
    uint chroma_address = chroma * 4u, chroma_pitch = chroma_step * 4u;
    if (outer && slot.edge_limit >= 0) {
        if (!across || lane < 13u || (slot.neighbours & 4u) == 0u)
            store_three(luma_address - 3u * luma_pitch, luma_pitch, a.yzw);
        if (!simple && (!across || chroma_line < 5u || (slot.neighbours & 4u) == 0u))
            store_three(chroma_address - 3u * chroma_pitch, chroma_pitch, ca.yzw);
    }
    if (!across && (lane < 13u || (slot.neighbours & 1u) == 0u)) {
        store_four(luma_address, luma_pitch, b);
        store_four(luma_address + 4u * luma_pitch, luma_pitch, c);
        store_four(luma_address + 8u * luma_pitch, luma_pitch, d);
        vp8_yuv[(luma_address + 12u * luma_pitch) >> 2u] = uint(e.x);
        if ((slot.neighbours & 2u) == 0u)
            store_three(luma_address + 13u * luma_pitch, luma_pitch, e.yzw);
    }
    if (!simple && !across && (chroma_line < 5u || (slot.neighbours & 1u) == 0u)) {
        store_four(chroma_address, chroma_pitch, cb);
        vp8_yuv[(chroma_address + 4u * chroma_pitch) >> 2u] = uint(cc.x);
        if ((slot.neighbours & 2u) == 0u)
            store_three(chroma_address + 5u * chroma_pitch, chroma_pitch, cc.yzw);
    }
}

void main() {
    for (uint k = 0u; k < 2u; ++k) {
        uint macroblock = gl_LocalInvocationIndex * 2u + k;
        if (macroblock < macroblock_columns * macroblock_rows)
            macroblock_info[macroblock] = macroblock_word(macroblock);
    }
    memoryBarrierShared();
    barrier();

    uint lane = gl_LocalInvocationID.x, row = gl_LocalInvocationID.y;
    bool simple = simple_filter != 0u;
    // Thread row y owns macroblock row y. Step 2d is the vertical pass of diagonal d and step 2d + 1 its horizontal
    // pass, on the same macroblocks.
    uint steps = 2u * (macroblock_columns + macroblock_rows - 1u);
    for (uint step_index = 0u; step_index < steps; ++step_index) {
        uint x = (step_index >> 1) - row;
        // A thread row above its diagonal wraps to a column past the picture.
        if (row < macroblock_rows && x < macroblock_columns)
            filter_pass(macroblock_slot(x, row), (step_index & 1u) == 0u, (step_index >> 1) & 1u, lane, simple);
        memoryBarrierShared();
        barrier();
    }
}
