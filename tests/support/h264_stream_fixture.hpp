#pragma once

// Synthetic H.264 Annex B generator for stream decoder tests. Original
// content only: IDR pictures are I_PCM macroblocks with a deterministic
// per-macroblock luma pattern, P pictures are all P_Skip (an exact copy of
// the reference with deblocking disabled), so decoded pixels are predictable
// without a reference decoder. Streams are tiny except where a test asks for
// large geometry on purpose.

#include "maicord/core/memory.hpp"

namespace maicord::h264_fixture {

struct Bits {
    u8* data;
    u32 capacity; // bytes
    u64 count;    // bits
    bool ok;
    void put(u32 value, u32 bits) noexcept {
        if (bits > 32 || count + bits > u64{capacity} * 8u) { ok = false; return; }
        if (bits == 8 && count % 8u == 0) { data[count / 8u] = static_cast<u8>(value); count += 8; return; }
        for (u32 i = bits; i; --i, ++count) {
            const u8 bit = static_cast<u8>((value >> (i - 1u)) & 1u);
            u8& target = data[count / 8u];
            const u32 shift = 7u - static_cast<u32>(count % 8u);
            target = static_cast<u8>((target & ~(1u << shift)) | (bit << shift));
        }
    }
    void ue(u32 value) noexcept {
        const u64 code = u64{value} + 1u;
        u32 length = 0;
        for (u64 n = code; n > 1; n >>= 1u) ++length;
        put(0, length);
        put(1, 1);
        if (length) put(static_cast<u32>(code & ((u64{1} << length) - 1u)), length);
    }
    void se(i32 value) noexcept {
        ue(value <= 0 ? static_cast<u32>(-static_cast<i64>(value) * 2) : static_cast<u32>(value) * 2u - 1u);
    }
    void align_zero() noexcept { while (count % 8u) put(0, 1); }
    void trailing() noexcept { put(1, 1); align_zero(); }
};

struct Output {
    u8* data;
    u32 capacity, count;
    bool ok;
    void byte(u8 value) noexcept {
        if (count == capacity) { ok = false; return; }
        data[count++] = value;
    }
};

// Start code (4 or 3 bytes), header byte, then the RBSP with emulation
// prevention applied.
inline void emit(Output* output, u8 header, const Bits& bits, bool long_start = true) noexcept {
    if (!bits.ok || bits.count % 8u) { output->ok = false; return; }
    if (long_start) output->byte(0);
    output->byte(0); output->byte(0); output->byte(1);
    output->byte(header);
    u32 zeroes = 0;
    for (u64 i = 0; i < bits.count / 8u; ++i) {
        const u8 value = bits.data[i];
        if (zeroes == 2 && value <= 3) { output->byte(3); zeroes = 0; }
        output->byte(value);
        zeroes = value ? 0 : zeroes + 1u;
    }
}

struct Sps {
    u8 id;
    u8 profile;         // 66 (Constrained Baseline) or 100 (High)
    u32 width_mbs, height_mbs;
    u8 poc_type;        // 0 or 2
    u8 max_references;
    bool restriction;   // VUI bitstream_restriction present
    u8 max_reorder;
    u32 crop_bottom;    // In luma rows (even).
    u8 level;
};
struct Pps {
    u8 id, sps_id;
};
constexpr u32 kFrameNumBits = 4;
constexpr u32 kPocBits = 8;

inline void write_sps(Output* output, Bits* bits, const Sps& sps) noexcept {
    bits->count = 0; bits->ok = true;
    bits->put(sps.profile, 8);
    bits->put(sps.profile == 66 ? 0x40u : 0u, 8);
    bits->put(sps.level ? sps.level : 31u, 8);
    bits->ue(sps.id);
    if (sps.profile == 100) { bits->ue(1); bits->ue(0); bits->ue(0); bits->put(0, 1); bits->put(0, 1); }
    bits->ue(kFrameNumBits - 4u);
    bits->ue(sps.poc_type);
    if (sps.poc_type == 0) bits->ue(kPocBits - 4u);
    bits->ue(sps.max_references);
    bits->put(0, 1); // gaps_in_frame_num_value_allowed_flag
    bits->ue(sps.width_mbs - 1u);
    bits->ue(sps.height_mbs - 1u);
    bits->put(1, 1); // frame_mbs_only_flag
    bits->put(1, 1); // direct_8x8_inference_flag
    bits->put(sps.crop_bottom ? 1u : 0u, 1);
    if (sps.crop_bottom) { bits->ue(0); bits->ue(0); bits->ue(0); bits->ue(sps.crop_bottom / 2u); }
    bits->put(sps.restriction ? 1u : 0u, 1);
    if (sps.restriction) {
        bits->put(0, 1); bits->put(0, 1); bits->put(0, 1); bits->put(0, 1); bits->put(0, 1);
        bits->put(0, 1); bits->put(0, 1); bits->put(0, 1); // no aspect/overscan/signal/chroma/timing/HRD/pic_struct
        bits->put(1, 1);
        bits->put(1, 1); bits->ue(0); bits->ue(0); bits->ue(16); bits->ue(16);
        bits->ue(sps.max_reorder);
        bits->ue(sps.max_references > sps.max_reorder ? sps.max_references : sps.max_reorder);
    }
    bits->trailing();
    emit(output, 0x67, *bits);
}

inline void write_pps(Output* output, Bits* bits, const Pps& pps) noexcept {
    bits->count = 0; bits->ok = true;
    bits->ue(pps.id); bits->ue(pps.sps_id);
    bits->put(0, 1); bits->put(0, 1); bits->ue(0); // CAVLC, no bottom POC, one slice group
    bits->ue(0); bits->ue(0);                     // one default reference per list
    bits->put(0, 1); bits->put(0, 2);             // no weighted prediction
    bits->se(0); bits->se(0); bits->se(0);
    bits->put(1, 1); bits->put(0, 1); bits->put(0, 1); // deblocking control, no constrained intra/redundant
    bits->trailing();
    emit(output, 0x68, *bits);
}

inline void write_aud(Output* output, Bits* bits, bool intra) noexcept {
    bits->count = 0; bits->ok = true;
    bits->put(intra ? 0u : 1u, 3);
    bits->trailing();
    emit(output, 0x09, *bits);
}

constexpr u8 pcm_luma(u32 macroblock, u32 pattern) noexcept {
    return static_cast<u8>(16u + (macroblock * 29u + pattern * 53u) % 200u);
}
constexpr u8 pcm_chroma(u32 pattern) noexcept { return static_cast<u8>(64u + (pattern * 37u) % 128u); }

struct Slice {
    bool idr;
    u8 reference_idc; // 0 for a non-reference picture
    u16 frame_num, poc_lsb, idr_id;
    u32 first_mb, mb_count;
    u32 pattern;      // I_PCM pattern for IDR slices.
    // Compact IDR slice: only the first macroblock is I_PCM; the rest are
    // I_16x16 DC prediction without residual, so the whole slice repeats the
    // first macroblock's samples (deblocking is off). Real keyframe sizes;
    // drivers may refuse an all-PCM 1080p picture (MinCR) as a GPU error.
    bool compact;
};

inline void write_slice(Output* output, Bits* bits, const Sps& sps, const Pps& pps, const Slice& slice,
                        bool long_start = true) noexcept {
    bits->count = 0; bits->ok = true;
    bits->ue(slice.first_mb);
    bits->ue(slice.idr ? 7u : 5u);
    bits->ue(pps.id);
    bits->put(slice.frame_num, kFrameNumBits);
    if (slice.idr) bits->ue(slice.idr_id);
    if (sps.poc_type == 0) bits->put(slice.poc_lsb, kPocBits);
    if (!slice.idr) { bits->put(0, 1); bits->put(0, 1); } // no ref count override, no list modification
    if (slice.reference_idc) { bits->put(0, 1); if (slice.idr) bits->put(0, 1); }
    bits->se(0);  // slice_qp_delta
    bits->ue(1);  // disable_deblocking_filter_idc
    if (slice.idr) {
        const u8 chroma = pcm_chroma(slice.pattern);
        for (u32 i = 0; i < slice.mb_count && bits->ok; ++i) {
            if (slice.compact && i) {
                const u32 m = slice.first_mb + i, x = m % sps.width_mbs;
                const bool left = x > 0 && m - 1u >= slice.first_mb;
                const bool top = m >= slice.first_mb + sps.width_mbs;
                const u32 a = left && m - 1u == slice.first_mb ? 16u : 0u;
                const u32 b = top && m - sps.width_mbs == slice.first_mb ? 16u : 0u;
                const u32 nc = left && top ? (a + b + 1u) >> 1u : left ? a : top ? b : 0u;
                bits->ue(3); // I_16x16, DC prediction, no coded luma/chroma AC
                bits->ue(0); // intra_chroma_pred_mode: DC
                bits->se(0); // mb_qp_delta
                // Intra16x16DCLevel coeff_token for TotalCoeff 0 (Table 9-5).
                if (nc < 2) bits->put(1, 1);
                else if (nc < 4) bits->put(3, 2);
                else if (nc < 8) bits->put(15, 4);
                else bits->put(3, 6);
                continue;
            }
            bits->ue(25); // I_PCM
            bits->align_zero();
            const u8 luma = pcm_luma(slice.first_mb + i, slice.pattern);
            for (u32 n = 0; n < 256; ++n) bits->put(luma, 8);
            for (u32 n = 0; n < 128; ++n) bits->put(chroma, 8);
        }
    } else bits->ue(slice.mb_count); // mb_skip_run over the whole slice
    bits->trailing();
    const u8 header = static_cast<u8>((slice.reference_idc << 5u) | (slice.idr ? 5u : 1u));
    emit(output, header, *bits, long_start);
}

// IDR access unit: optional AUD, SPS, PPS, then `slices` equal slices.
inline void write_idr(Output* output, Bits* bits, const Sps& sps, const Pps& pps, u32 pattern, u16 idr_id,
                      u32 slices = 1, bool parameters = true, bool compact = false) noexcept {
    write_aud(output, bits, true);
    if (parameters) { write_sps(output, bits, sps); write_pps(output, bits, pps); }
    const u32 total = sps.width_mbs * sps.height_mbs;
    const u32 per = (total + slices - 1u) / slices;
    for (u32 first = 0; first < total; first += per) {
        Slice slice{true, 3, 0, 0, idr_id, first, first + per > total ? total - first : per, pattern, compact};
        write_slice(output, bits, sps, pps, slice, first == 0);
    }
}

inline void write_p(Output* output, Bits* bits, const Sps& sps, const Pps& pps, u16 frame_num, u16 poc_lsb,
                    bool reference = true, u32 slices = 1) noexcept {
    const u32 total = sps.width_mbs * sps.height_mbs;
    const u32 per = (total + slices - 1u) / slices;
    for (u32 first = 0; first < total; first += per) {
        Slice slice{false, static_cast<u8>(reference ? 2 : 0), frame_num, poc_lsb, 0, first,
                    first + per > total ? total - first : per, 0, false};
        write_slice(output, bits, sps, pps, slice);
    }
}

} // namespace maicord::h264_fixture
