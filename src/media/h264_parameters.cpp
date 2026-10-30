#include "maicord/media/h264_parameters.hpp"
#include "maicord/core/memory.hpp"
#include "h264_bits.hpp"
#include "maicord/sys/crt.hpp"
// Header parsing, once per picture and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

// H.264 tables 7-3/7-4, in scan order (not raster order).
constexpr u8 default_small[2][16] = {
    {6,13,13,20,20,20,28,28,28,28,32,32,32,37,37,42},
    {10,14,14,20,20,20,24,24,24,24,27,27,27,30,30,34}};
constexpr u8 default_large[2][64] = {
    {6,10,10,13,11,13,16,16,16,16,18,18,18,18,18,23,
     23,23,23,23,23,25,25,25,25,25,25,25,27,27,27,27,
     27,27,27,27,29,29,29,29,29,29,29,31,31,31,31,31,
     31,33,33,33,33,33,36,36,36,36,38,38,38,40,40,42},
    {9,13,13,15,13,15,17,17,17,17,19,19,19,19,19,21,
     21,21,21,21,21,22,22,22,22,22,22,22,24,24,24,24,
     24,24,24,24,25,25,25,25,25,25,25,27,27,27,27,27,
     27,28,28,28,28,28,30,30,30,30,32,32,32,33,33,35}};
constexpr u16 sar[17][2] = {{0,0},{1,1},{12,11},{10,11},{16,11},{40,33},
    {24,11},{20,11},{32,11},{80,33},{18,11},{15,11},{64,33},
    {160,99},{4,3},{3,2},{2,1}};

H264Bits payload(ConstSlice<u8> nal, u8 type) noexcept {
    H264Bits bits{};
    if (!nal.data || nal.count < 2) bits.error = H264Error::invalid_input;
    else if ((nal.data[0] & 0x9Fu) != type || (nal.data[0] & 0x60u) == 0)
        bits.error = H264Error::malformed;
    else bits.source = {nal.data + 1, nal.count - 1};
    return bits;
}

void scaling_list(H264Bits* bits, u8* list, u32 length, const u8* defaults) noexcept {
    i32 previous = 8, next = 8;
    for (u32 i = 0; i < length && bits->error == H264Error::none; ++i) {
        if (next) next = (previous + bits->se(-128, 127) + 256) & 255;
        if (i == 0 && next == 0) { copy_bytes(list, defaults, length); return; }
        list[i] = static_cast<u8>(next ? next : previous);
        previous = list[i];
    }
}

void scaling(H264Bits* bits, H264Scaling* output, const H264Scaling* fallback,
             bool large) noexcept {
    for (u32 i = 0; i < 6; ++i) {
        const u8* defaults = default_small[i / 3];
        if (bits->bit()) scaling_list(bits, output->list4x4[i], 16, defaults);
        else copy_bytes(output->list4x4[i], (i == 0 || i == 3)
            ? (fallback ? fallback->list4x4[i] : defaults) : output->list4x4[i - 1], 16);
    }
    if (large) for (u32 i = 0; i < 2; ++i) {
        if (bits->bit()) scaling_list(bits, output->list8x8[i], 64, default_large[i]);
        else copy_bytes(output->list8x8[i], fallback ? fallback->list8x8[i] : default_large[i], 64);
    }
}

bool hrd(H264Bits* bits) noexcept {
    if (!bits->bit()) return false;
    const u32 count = bits->ue(31) + 1u;
    bits->read(8);
    for (u32 i = 0; i < count; ++i) { bits->ue(); bits->ue(); bits->bit(); }
    bits->read(20);
    return true;
}

void vui(H264Bits* bits, H264Sps* sps) noexcept {
    if (bits->bit()) {
        const u32 id = bits->read(8);
        if (id == 255) {
            sps->sar_width = static_cast<u16>(bits->read(16));
            sps->sar_height = static_cast<u16>(bits->read(16));
        } else if (id < 17) {
            sps->sar_width = sar[id][0]; sps->sar_height = sar[id][1];
        } else { bits->error = H264Error::malformed; return; }
        if (!sps->sar_width || !sps->sar_height) sps->sar_width = sps->sar_height = 1;
    }
    if (bits->bit()) bits->bit(); // overscan
    if (bits->bit()) {
        bits->read(3);
        sps->full_range = bits->bit();
        if (bits->bit()) {
            bits->read(16); // colour_primaries, transfer_characteristics
            sps->matrix = static_cast<u8>(bits->read(8));
        }
    }
    if (bits->bit()) { bits->ue(5); bits->ue(5); }
    if (bits->bit()) {
        sps->units_in_tick = bits->read(32);
        sps->time_scale = bits->read(32);
        bits->bit(); // fixed_frame_rate_flag
        if (!sps->units_in_tick || !sps->time_scale) bits->error = H264Error::malformed;
    }
    const bool nal_hrd = hrd(bits), vcl_hrd = hrd(bits);
    if (nal_hrd || vcl_hrd) bits->bit();
    bits->bit(); // pic_struct_present_flag
    sps->restriction = bits->bit();
    if (sps->restriction) {
        bits->bit(); bits->ue(16); bits->ue(16); bits->ue(16); bits->ue(16);
        sps->max_reorder = static_cast<u8>(bits->ue(16));
        sps->max_buffering = static_cast<u8>(bits->ue(16));
        if (sps->max_reorder > sps->max_buffering || sps->max_references > sps->max_buffering)
            bits->error = H264Error::malformed;
    }
}

} // namespace

H264Nal h264_next_nal(H264NalCursor* cursor) noexcept {
    if (!cursor || !cursor->bytes.data || (cursor->length_bytes != 1 &&
        cursor->length_bytes != 2 && cursor->length_bytes != 4) ||
        cursor->offset > cursor->bytes.count) return {{}, H264Error::invalid_input};
    u32 offset = cursor->offset;
    if (offset == cursor->bytes.count) return {{}, H264Error::end};
    if (cursor->bytes.count - offset < cursor->length_bytes) return {{}, H264Error::truncated};
    u32 size = 0;
    for (u32 i = 0; i < cursor->length_bytes; ++i) size = (size << 8u) | cursor->bytes.data[offset++];
    if (!size) return {{}, H264Error::malformed};
    if (size > cursor->bytes.count - offset) return {{}, H264Error::truncated};
    const u8 header = cursor->bytes.data[offset];
    if ((header & 0x80u) || !(header & 31u)) return {{}, H264Error::malformed};
    cursor->offset = offset + size;
    return {{cursor->bytes.data + offset, size}, H264Error::none,
        static_cast<u8>(header & 31u), static_cast<u8>((header >> 5u) & 3u)};
}

H264Error h264_parse_sps(ConstSlice<u8> nal, H264Sps* output) noexcept {
    if (!output) return H264Error::invalid_input;
    H264Bits bits = payload(nal, 7);
    if (bits.error != H264Error::none) return bits.error;
    H264Sps sps{};
    memset(&sps.scaling, 16, sizeof(sps.scaling));
    sps.sar_width = sps.sar_height = 1;
    sps.matrix = 2;
    sps.max_reorder = sps.max_buffering = 16; // Unspecified, not a derived DPB limit.
    sps.profile = static_cast<u8>(bits.read(8));
    sps.constraints = static_cast<u8>(bits.read(8));
    sps.level = static_cast<u8>(bits.read(8));
    sps.id = static_cast<u8>(bits.ue(31));
    if (bits.error != H264Error::none) return bits.error;
    if (sps.constraints & 3u) return H264Error::malformed;
    if (sps.profile == 100) {
        const u32 chroma = bits.ue(3);
        if (bits.error != H264Error::none) return bits.error;
        if (chroma != 1) return H264Error::unsupported;
        const u32 luma = bits.ue(6), color = bits.ue(6);
        if (bits.error != H264Error::none) return bits.error;
        if (luma || color) return H264Error::unsupported;
        sps.transform_bypass = bits.bit();
        sps.scaling_present = bits.bit();
        if (sps.scaling_present) scaling(&bits, &sps.scaling, nullptr, true);
    } else if (sps.profile != 66 && sps.profile != 77) return H264Error::unsupported;
    sps.frame_num_bits = static_cast<u8>(bits.ue(12) + 4u);
    sps.poc_type = static_cast<u8>(bits.ue(2));
    if (sps.poc_type == 0) sps.poc_bits = static_cast<u8>(bits.ue(12) + 4u);
    if (sps.poc_type == 1) {
        sps.delta_always_zero = bits.bit();
        sps.offset_non_reference = bits.se();
        sps.offset_top_to_bottom = bits.se();
        sps.cycle_count = static_cast<u8>(bits.ue(255));
        for (u32 i = 0; i < sps.cycle_count; ++i) {
            sps.cycle_offsets[i] = bits.se(); sps.cycle_delta += sps.cycle_offsets[i];
        }
    }
    sps.max_references = static_cast<u8>(bits.ue(16));
    sps.gaps = bits.bit();
    sps.coded_width = (bits.ue(543) + 1u) * 16u;
    const u32 map_units = bits.ue(543) + 1u;
    sps.frame_only = bits.bit();
    if (!sps.frame_only) sps.mbaff = bits.bit();
    sps.coded_height = map_units * (sps.frame_only ? 16u : 32u);
    sps.direct_8x8 = bits.bit();
    u32 right = 0, bottom = 0;
    if (bits.bit()) {
        sps.crop_x = bits.ue(sps.coded_width / 2u) * 2u;
        right = bits.ue(sps.coded_width / 2u) * 2u;
        const u32 unit = sps.frame_only ? 2u : 4u;
        sps.crop_y = bits.ue(sps.coded_height / unit) * unit;
        bottom = bits.ue(sps.coded_height / unit) * unit;
    }
    if (bits.bit()) vui(&bits, &sps);
    if (!bits.finish()) return bits.error;
    if (sps.crop_x + right >= sps.coded_width || sps.crop_y + bottom >= sps.coded_height)
        return H264Error::malformed;
    sps.width = sps.coded_width - sps.crop_x - right;
    sps.height = sps.coded_height - sps.crop_y - bottom;
    *output = sps;
    return H264Error::none;
}

H264Error h264_parse_pps(ConstSlice<u8> nal, const H264Sps& sps, H264Pps* output) noexcept {
    if (!output) return H264Error::invalid_input;
    H264Bits bits = payload(nal, 8);
    if (bits.error != H264Error::none) return bits.error;
    H264Pps pps{};
    copy_value(&pps.scaling, sps.scaling);
    pps.id = static_cast<u8>(bits.ue(255));
    pps.sps_id = static_cast<u8>(bits.ue(31));
    if (pps.sps_id != sps.id) return H264Error::malformed;
    pps.cabac = bits.bit();
    pps.bottom_poc_present = bits.bit();
    const u32 groups = bits.ue(7);
    if (bits.error != H264Error::none) return bits.error;
    if (groups) return H264Error::unsupported;
    pps.references_l0 = static_cast<u8>(bits.ue(31) + 1u);
    pps.references_l1 = static_cast<u8>(bits.ue(31) + 1u);
    pps.weighted_pred = bits.bit();
    pps.weighted_bipred = static_cast<u8>(bits.read(2));
    if (pps.weighted_bipred > 2) return H264Error::malformed;
    pps.initial_qp = static_cast<signed char>(bits.se(-26, 25));
    pps.initial_qs = static_cast<signed char>(bits.se(-26, 25));
    pps.chroma_qp = static_cast<signed char>(bits.se(-12, 12));
    pps.second_chroma_qp = pps.chroma_qp;
    pps.deblocking_control = bits.bit();
    pps.constrained_intra = bits.bit();
    pps.redundant_count = bits.bit();
    H264Bits end = bits;
    if (!end.finish()) {
        pps.transform_8x8 = bits.bit();
        pps.scaling_present = bits.bit();
        if (pps.scaling_present) scaling(&bits, &pps.scaling,
            sps.scaling_present ? &sps.scaling : nullptr, pps.transform_8x8);
        pps.second_chroma_qp = static_cast<signed char>(bits.se(-12, 12));
        if (sps.profile != 100 && (pps.transform_8x8 || pps.scaling_present))
            return H264Error::unsupported;
    }
    if (!bits.finish()) return bits.error;
    copy_value(output, pps);
    return H264Error::none;
}

u8 h264_sps_dpb_frames(const H264Sps& sps) noexcept {
    const u8 references = sps.max_references ? sps.max_references : 1;
    if (sps.restriction) {
        return static_cast<u8>(minimum<u32>(16, maximum<u32>(maximum<u32>(sps.max_buffering, references), 1)));
    }
    u32 dpb_mbs = 0;
    switch (sps.level) {
    case 9: case 10: dpb_mbs = 396; break;
    case 11: dpb_mbs = 900; break; // Also level 1b (396): the larger bound is safe.
    case 12: case 13: case 20: dpb_mbs = 2'376; break;
    case 21: dpb_mbs = 4'752; break;
    case 22: case 30: dpb_mbs = 8'100; break;
    case 31: dpb_mbs = 18'000; break;
    case 32: dpb_mbs = 20'480; break;
    case 40: case 41: dpb_mbs = 32'768; break;
    case 42: dpb_mbs = 34'816; break;
    case 50: dpb_mbs = 110'400; break;
    case 51: case 52: dpb_mbs = 184'320; break;
    case 60: case 61: case 62: dpb_mbs = 696'320; break;
    default: return 16;
    }
    const u32 frame_mbs = (sps.coded_width / 16u) * (sps.coded_height / 16u);
    const u32 frames = frame_mbs ? minimum<u32>(16, dpb_mbs / frame_mbs) : 0;
    // A level too small for the picture or its own reference count is a
    // mislabelled stream; keep the syntax maximum for it.
    return frames && frames >= references ? static_cast<u8>(frames) : 16;
}

u8 h264_sps_output_reorder(const H264Sps& sps) noexcept {
    return sps.restriction ? static_cast<u8>(minimum<u32>(sps.max_reorder, 16)) : h264_sps_dpb_frames(sps);
}

} // namespace maicord
#pragma clang attribute pop
