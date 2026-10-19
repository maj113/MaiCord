#pragma once

// Synthetic H.265 Annex B generator for stream decoder tests. Original
// content only: IRAP pictures are PCM coding units (one per CTU) with a
// deterministic per-CTU luma level, P pictures are all cu_skip with one merge
// candidate (an exact copy of the reference, deblocking and SAO off), so
// decoded pixels are predictable without a reference decoder. The CABAC
// encoder follows H.265 9.3.4.

#include "h264_stream_fixture.hpp"

namespace maicord::hevc_fixture {

using h264_fixture::Bits;
using h264_fixture::Output;

// H.265 Table 9-52 and 9-53.
constexpr u8 kRangeLps[64][4] = {
    {128, 176, 208, 240}, {128, 167, 197, 227}, {128, 158, 187, 216}, {123, 150, 178, 205},
    {116, 142, 169, 195}, {111, 135, 160, 185}, {105, 128, 152, 175}, {100, 122, 144, 166},
    {95, 116, 137, 158},  {90, 110, 130, 150},  {85, 104, 123, 142},  {81, 99, 117, 135},
    {77, 94, 111, 128},   {73, 89, 105, 122},   {69, 85, 100, 116},   {66, 80, 95, 110},
    {62, 76, 90, 104},    {59, 72, 86, 99},     {56, 69, 81, 94},     {53, 65, 77, 89},
    {51, 62, 73, 85},     {48, 59, 69, 80},     {46, 56, 66, 76},     {43, 53, 63, 72},
    {41, 50, 59, 69},     {39, 48, 56, 65},     {37, 45, 54, 62},     {35, 43, 51, 59},
    {33, 41, 48, 56},     {32, 39, 46, 53},     {30, 37, 43, 50},     {29, 35, 41, 48},
    {27, 33, 39, 45},     {26, 31, 37, 43},     {24, 30, 35, 41},     {23, 28, 33, 39},
    {22, 27, 32, 37},     {21, 26, 30, 35},     {20, 24, 29, 33},     {19, 23, 27, 31},
    {18, 22, 26, 30},     {17, 21, 25, 28},     {16, 20, 23, 27},     {15, 19, 22, 25},
    {14, 18, 21, 24},     {14, 17, 20, 23},     {13, 16, 19, 22},     {12, 15, 18, 21},
    {12, 14, 17, 20},     {11, 14, 16, 19},     {11, 13, 15, 18},     {10, 12, 15, 17},
    {10, 12, 14, 16},     {9, 11, 13, 15},      {9, 11, 12, 14},      {8, 10, 12, 14},
    {8, 9, 11, 13},       {7, 9, 11, 12},       {7, 9, 10, 12},       {7, 8, 10, 11},
    {6, 8, 9, 11},        {6, 7, 9, 10},        {6, 7, 8, 9},         {2, 2, 2, 2}};
constexpr u8 kTransitionLps[64] = {0,  0,  1,  2,  2,  4,  4,  5,  6,  7,  8,  9,  9,  11, 11, 12,
                                   13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
                                   24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33,
                                   33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63};

struct Context {
    u8 state, mps;
};
// 9.3.2.2 with SliceQpY 26.
constexpr Context context(u32 init) noexcept {
    const i32 m = static_cast<i32>(init >> 4) * 5 - 45, n = static_cast<i32>((init & 15u) << 3) - 16;
    i32 pre = ((m * 26) >> 4) + n;
    pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
    return pre <= 63 ? Context{static_cast<u8>(63 - pre), 0} : Context{static_cast<u8>(pre - 64), 1};
}
constexpr u32 kPartModeInit = 184, kSkipInit[3] = {197, 185, 201};

struct Cabac {
    Bits* bits;
    u32 low, range, outstanding;
    bool first;
    void start() noexcept {
        low = 0;
        range = 510;
        outstanding = 0;
        first = true;
    }
    void put(u32 bit) noexcept {
        if (first)
            first = false;
        else
            bits->put(bit, 1);
        for (; outstanding; --outstanding)
            bits->put(1u - bit, 1);
    }
    void renormalize() noexcept {
        while (range < 256) {
            if (low < 256)
                put(0);
            else if (low >= 512) {
                low -= 512;
                put(1);
            } else {
                low -= 256;
                ++outstanding;
            }
            range <<= 1;
            low <<= 1;
        }
    }
    void decision(Context& c, u32 bin) noexcept {
        const u32 lps = kRangeLps[c.state][(range >> 6) & 3u];
        range -= lps;
        if (bin != c.mps) {
            low += range;
            range = lps;
            if (!c.state)
                c.mps = static_cast<u8>(1u - c.mps);
            c.state = kTransitionLps[c.state];
        } else if (c.state < 62)
            ++c.state;
        renormalize();
    }
    // A terminating 1 flushes; its last written bit is the stop bit.
    void terminate(u32 bin) noexcept {
        range -= 2;
        if (!bin) {
            renormalize();
            return;
        }
        low += range;
        range = 2;
        renormalize();
        put((low >> 9) & 1u);
        bits->put(((low >> 7) & 3u) | 1u, 2);
    }
};

struct Sps {
    u8 id;
    u32 width, height; // Coded size, a multiple of the CTB size.
    u32 crop_bottom;   // Luma rows (even).
    u8 ctb_log2;       // 4 or 5; CTB = minimum CB = PCM size.
    u8 pcm_bits;       // PCM sample depth 1-8.
    u8 max_buffering;  // sps_max_dec_pic_buffering_minus1 + 1.
    u8 bit_depth = 8;  // 10 makes a Main10 SPS.
    u8 level = 93;
};
struct Pps {
    u8 id, sps_id;
};
constexpr u32 kPocBits = 8;
constexpr u8 kTrailR = 1, kRaslN = 8, kIdrWRadl = 19, kIdrNLp = 20, kCra = 21;

inline void emit_hevc(Output* output, u8 type, const Bits& bits) noexcept {
    if (!bits.ok || bits.count % 8u) {
        output->ok = false;
        return;
    }
    output->byte(0);
    output->byte(0);
    output->byte(0);
    output->byte(1);
    output->byte(static_cast<u8>(type << 1));
    output->byte(1); // nuh_layer_id 0, nuh_temporal_id_plus1 1
    u32 zeroes = 0;
    for (u64 i = 0; i < bits.count / 8u; ++i) {
        const u8 value = bits.data[i];
        if (zeroes == 2 && value <= 3) {
            output->byte(3);
            zeroes = 0;
        }
        output->byte(value);
        zeroes = value ? 0 : zeroes + 1u;
    }
}

inline void profile_tier_level(Bits* bits, u8 level, u8 bit_depth) noexcept {
    const u32 profile = bit_depth == 8 ? 1u : 2u;
    bits->put(0, 2);                                         // general_profile_space
    bits->put(0, 1);                                         // general_tier_flag
    bits->put(profile, 5);                                   // Main or Main10
    bits->put(profile == 1 ? 0x60000000u : 0x20000000u, 32); // compatibility flags
    bits->put(0x9, 4);                                       // progressive, frame only
    bits->put(0, 32);
    bits->put(0, 12);
    bits->put(level, 8);
}

inline void write_hevc_vps(Output* output, Bits* bits, const Sps& sps) noexcept {
    bits->count = 0;
    bits->ok = true;
    bits->put(0, 4);
    bits->put(3, 2); // base layer internal and available
    bits->put(0, 6);
    bits->put(0, 3);
    bits->put(1, 1);
    bits->put(0xffff, 16);
    profile_tier_level(bits, sps.level, sps.bit_depth);
    bits->put(1, 1);
    bits->ue(sps.max_buffering - 1u);
    bits->ue(0);
    bits->ue(0);
    bits->put(0, 6);
    bits->ue(0);
    bits->put(0, 1); // no timing info
    bits->put(0, 1); // no extension
    bits->trailing();
    emit_hevc(output, 32, *bits);
}

inline void write_hevc_sps(Output* output, Bits* bits, const Sps& sps) noexcept {
    bits->count = 0;
    bits->ok = true;
    bits->put(0, 4);
    bits->put(0, 3);
    bits->put(1, 1);
    profile_tier_level(bits, sps.level, sps.bit_depth);
    bits->ue(sps.id);
    bits->ue(1); // 4:2:0
    bits->ue(sps.width);
    bits->ue(sps.height);
    bits->put(sps.crop_bottom ? 1u : 0u, 1);
    if (sps.crop_bottom) {
        bits->ue(0);
        bits->ue(0);
        bits->ue(0);
        bits->ue(sps.crop_bottom / 2u);
    }
    bits->ue(sps.bit_depth - 8u);
    bits->ue(sps.bit_depth - 8u);
    bits->ue(kPocBits - 4u);
    bits->put(1, 1);
    bits->ue(sps.max_buffering - 1u);
    bits->ue(0); // no reordering
    bits->ue(0);
    bits->ue(sps.ctb_log2 - 3u); // minimum CB = CTB
    bits->ue(0);
    bits->ue(0);                 // 4x4 minimum transform
    bits->ue(sps.ctb_log2 - 2u); // maximum transform = CTB (16 or 32)
    bits->ue(0);
    bits->ue(0);
    bits->put(0, 1); // scaling lists
    bits->put(0, 1); // AMP
    bits->put(0, 1); // SAO
    bits->put(1, 1); // PCM
    bits->put(sps.pcm_bits - 1u, 4);
    bits->put(sps.pcm_bits - 1u, 4);
    bits->ue(sps.ctb_log2 - 3u);
    bits->ue(0);
    bits->put(1, 1); // PCM loop filter disabled
    bits->ue(1);     // one short-term RPS: the previous picture
    bits->ue(1);
    bits->ue(0);
    bits->ue(0);
    bits->put(1, 1);
    bits->put(0, 1); // no long-term references
    bits->put(0, 1); // no temporal MVP
    bits->put(0, 1); // no strong intra smoothing
    bits->put(0, 1); // no VUI
    bits->put(0, 1); // no extension
    bits->trailing();
    emit_hevc(output, 33, *bits);
}

inline void write_hevc_pps(Output* output, Bits* bits, const Pps& pps) noexcept {
    bits->count = 0;
    bits->ok = true;
    bits->ue(pps.id);
    bits->ue(pps.sps_id);
    bits->put(0, 1); // dependent slices
    bits->put(0, 1); // output flag
    bits->put(0, 3);
    bits->put(0, 1); // sign data hiding
    bits->put(0, 1); // cabac_init_present
    bits->ue(0);
    bits->ue(0);
    bits->se(0); // init_qp_minus26
    bits->put(0, 1);
    bits->put(0, 1);
    bits->put(0, 1); // no cu_qp_delta
    bits->se(0);
    bits->se(0);
    bits->put(0, 1);
    bits->put(0, 1);
    bits->put(0, 1);
    bits->put(0, 1); // transquant bypass
    bits->put(0, 1); // tiles
    bits->put(0, 1); // entropy sync
    bits->put(0, 1); // loop filter across slices
    bits->put(1, 1); // deblocking control present
    bits->put(0, 1); // no override
    bits->put(1, 1); // deblocking disabled
    bits->put(0, 1); // scaling lists
    bits->put(0, 1); // lists modification
    bits->ue(0);
    bits->put(0, 1);
    bits->put(0, 1); // no extension
    bits->trailing();
    emit_hevc(output, 34, *bits);
}

constexpr u32 ctb_size(const Sps& sps) noexcept {
    return 1u << sps.ctb_log2;
}
constexpr u32 ctb_columns(const Sps& sps) noexcept {
    return sps.width >> sps.ctb_log2;
}
constexpr u32 ctb_count(const Sps& sps) noexcept {
    return ctb_columns(sps) * (sps.height >> sps.ctb_log2);
}
// PCM levels per CTU and per picture; decoded samples are level << (8 - pcm_bits).
constexpr u32 luma_level(const Sps& sps, u32 ctu, u32 pattern) noexcept {
    return (ctu * 29u + pattern * 53u + 7u) % (1u << sps.pcm_bits);
}
constexpr u32 chroma_level(const Sps& sps, u32 pattern, u32 plane) noexcept {
    return (pattern * 37u + plane * 101u + 3u) % (1u << sps.pcm_bits);
}
constexpr u8 sample(const Sps& sps, u32 level) noexcept {
    return static_cast<u8>(level << (8u - sps.pcm_bits));
}

struct Slice {
    u8 type;
    u32 first, count; // CTUs
    u16 poc_lsb;
    u32 pattern;
};

inline void write_slice(Output* output, Bits* bits, const Sps& sps, const Pps& pps,
                        const Slice& slice) noexcept {
    bits->count = 0;
    bits->ok = true;
    const bool irap = slice.type >= 16, idr = slice.type == kIdrWRadl || slice.type == kIdrNLp;
    bits->put(slice.first ? 0u : 1u, 1);
    if (irap)
        bits->put(0, 1); // no_output_of_prior_pics_flag
    bits->ue(pps.id);
    if (slice.first) {
        u32 address_bits = 0;
        while ((1u << address_bits) < ctb_count(sps))
            ++address_bits;
        bits->put(slice.first, address_bits);
    }
    bits->ue(irap ? 2u : 1u);
    if (!idr) {
        bits->put(slice.poc_lsb, kPocBits);
        // P: the SPS short-term RPS (previous picture). CRA: an empty RPS in
        // the header, since an IRAP refers to nothing.
        bits->put(irap ? 0u : 1u, 1);
        if (irap) {
            bits->put(0, 1); // inter_ref_pic_set_prediction_flag
            bits->ue(0);
            bits->ue(0);
        }
    }
    if (!irap) {
        bits->put(0, 1); // no reference count override
        bits->ue(4);     // MaxNumMergeCand 1
    }
    bits->se(0); // slice_qp_delta
    bits->put(1, 1);
    bits->align_zero();
    Cabac cabac{bits};
    cabac.start();
    Context part = context(kPartModeInit),
            skip[3] = {context(kSkipInit[0]), context(kSkipInit[1]), context(kSkipInit[2])};
    const u32 columns = ctb_columns(sps), size = ctb_size(sps), chroma = size * size / 4u;
    for (u32 i = 0; i < slice.count && bits->ok; ++i) {
        const u32 ctu = slice.first + i;
        if (irap) {
            cabac.decision(part, 1); // PART_2Nx2N
            cabac.terminate(1);      // pcm_flag
            bits->align_zero();
            const u32 luma = luma_level(sps, ctu, slice.pattern);
            for (u32 n = 0; n < size * size; ++n)
                bits->put(luma, sps.pcm_bits);
            for (u32 plane = 0; plane < 2; ++plane)
                for (u32 n = 0; n < chroma; ++n)
                    bits->put(chroma_level(sps, slice.pattern, plane), sps.pcm_bits);
            cabac.start();
        } else {
            const bool left = ctu % columns && ctu > slice.first, above = ctu >= slice.first + columns;
            cabac.decision(skip[(left ? 1 : 0) + (above ? 1 : 0)], 1);
        }
        cabac.terminate(i + 1 == slice.count); // end_of_slice_segment_flag
    }
    bits->align_zero();
    emit_hevc(output, slice.type, *bits);
}

// slice_ctus CTUs per slice (0: one slice).
inline void write_picture(Output* output, Bits* bits, const Sps& sps, const Pps& pps, u8 type, u16 poc_lsb,
                          u32 pattern, u32 slice_ctus = 0) noexcept {
    const u32 total = ctb_count(sps), per = slice_ctus ? slice_ctus : total;
    for (u32 first = 0; first < total; first += per)
        write_slice(output, bits, sps, pps,
                    {type, first, first + per > total ? total - first : per, poc_lsb, pattern});
}

// IRAP access unit with VPS/SPS/PPS.
inline void write_idr(Output* output, Bits* bits, const Sps& sps, const Pps& pps, u32 pattern,
                      u32 slice_ctus = 0, u8 type = kIdrNLp) noexcept {
    write_hevc_vps(output, bits, sps);
    write_hevc_sps(output, bits, sps);
    write_hevc_pps(output, bits, pps);
    write_picture(output, bits, sps, pps, type, 0, pattern, slice_ctus);
}

} // namespace maicord::hevc_fixture
