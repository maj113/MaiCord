// The Go Live encoder's pure parts on both systems, without a GPU: Discord's quality presets and their Nitro and server
// boost limits, the encoded extent and bitrate of each quality, the codec tier choice, the rewriting of hardware
// encoders' output into the access units the stream sender takes, and the AV1 sequence and frame header OBUs the VA-API
// encoder hands the driver, parsed back with MaiCord's own AV1 parser.
#include "../../support/test_output.hpp"
#include "../../../src/platform/encode/access_unit.hpp"
#include "../../../src/platform/encode/av1_headers.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/video/av1.hpp"
#include "maicord/media/video/stream_quality.hpp"
#include "maicord/platform/encode/gpu_stream_encoder.hpp"

namespace maicord {
namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool same(StreamQuality a, StreamQuality b) noexcept {
    return a.resolution == b.resolution && a.fps == b.fps;
}

void presets() noexcept {
    constexpr StreamPreset video = StreamPreset::video, text = StreamPreset::text, custom = StreamPreset::custom;
    // premium_type: 0 none, 1 Nitro Classic, 2 Nitro, 3 Nitro Basic.
    check(stream_quality_allowed(custom, {720, 30}, 0, 0) && stream_quality_allowed(custom, {480, 15}, 0, 0),
          "720p and 480p at 30 fps and below are everyone's");
    check(!stream_quality_allowed(custom, {720, 60}, 0, 0) && stream_quality_allowed(custom, {720, 60}, 1, 0) &&
              stream_quality_allowed(custom, {720, 60}, 0, 1) && !stream_quality_allowed(custom, {720, 60}, 3, 0),
          "720p60 takes Nitro Classic, Nitro or a level 1 server (not Basic)");
    check(!stream_quality_allowed(custom, {1080, 60}, 0, 1) && stream_quality_allowed(custom, {1080, 60}, 0, 2) &&
              stream_quality_allowed(custom, {1440, 30}, 1, 0),
          "1080p and 1440p take Nitro Classic, Nitro or a level 2 server");
    check(!stream_quality_allowed(custom, {0, 60}, 1, 3) && stream_quality_allowed(custom, {0, 60}, 2, 0),
          "source resolution above 5 fps is Nitro's alone");
    check(stream_quality_allowed(text, {0, 5}, 0, 0) && !stream_quality_allowed(custom, {0, 5}, 0, 0),
          "source at 5 fps only with Clearer Text");
    check(!stream_quality_allowed(custom, {1080, 24}, 2, 3), "frame rates outside Discord's are refused");
    check(same(stream_preset_quality(video, {}, 2, 0), {1440, 60}) && same(stream_preset_quality(video, {}, 0, 1), {720, 60}) &&
              same(stream_preset_quality(video, {}, 0, 0), {720, 30}),
          "Smoother Video tries 1440p60, 1080p60, 720p60, 720p30");
    check(same(stream_preset_quality(text, {}, 2, 0), {0, 15}) && same(stream_preset_quality(text, {}, 0, 0), {0, 5}),
          "Clearer Text tries source at 15 fps, then 5");
    check(same(stream_preset_quality(custom, {1080, 60}, 1, 0), {1080, 60}) &&
              same(stream_preset_quality(custom, {1080, 60}, 0, 0), kStreamDefaultQuality),
          "a custom quality the user may not stream falls back to 720p30");
}

void extents() noexcept {
    auto is = [](StreamExtent e, u32 w, u32 h) noexcept { return e.width == w && e.height == h; };
    check(is(stream_encode_extent(1080, 3840, 2160), 1920, 1080) && is(stream_encode_extent(720, 2560, 1440), 1280, 720),
          "16:9 sources fill the box");
    check(is(stream_encode_extent(480, 1920, 1080), 640, 360) && is(stream_encode_extent(720, 1000, 1000), 720, 720),
          "other shapes fit inside it");
    check(is(stream_encode_extent(1080, 800, 601), 800, 600) && is(stream_encode_extent(0, 1921, 1081), 1920, 1080),
          "smaller sources keep their size, made even");
    check(is(stream_encode_extent(0, 7680, 4320), 4096, 2304) && is(stream_encode_extent(720, 0, 10), 0, 0),
          "sources past 4096x2304 fit into it; nothing from an empty source");
    const StreamBitrate low = stream_quality_bitrate({720, 30}), high = stream_quality_bitrate({1080, 60});
    check(low.maximum == 3'500'000 && high.maximum == 9'000'000 && stream_quality_bitrate({0, 5}).maximum == 3'500'000 &&
              stream_quality_bitrate({0, 15}).maximum == 9'000'000 && low.minimum == 500'000 && low.start == 600'000,
          "Discord's bitrate bounds");
}

void tiers() noexcept {
    constexpr u32 h264 = gpu_stream_codec_bit(GpuStreamCodec::h264), h265 = gpu_stream_codec_bit(GpuStreamCodec::h265),
                  av1 = gpu_stream_codec_bit(GpuStreamCodec::av1);
    GpuStreamCodec codec{};
    check(gpu_stream_encoder_pick(h264 | h265 | av1, h264 | h265 | av1, &codec) && codec == GpuStreamCodec::av1,
          "AV1 first");
    check(gpu_stream_encoder_pick(h264 | h265 | av1, h264 | h265, &codec) && codec == GpuStreamCodec::h265,
          "then H.265 when the stream takes no AV1");
    check(gpu_stream_encoder_pick(h264 | h265, h264 | av1, &codec) && codec == GpuStreamCodec::h264,
          "then H.264");
    check(!gpu_stream_encoder_pick(h265 | av1, h264, &codec) && !gpu_stream_encoder_pick(0, h264 | h265 | av1, &codec),
          "no common codec, no stream");
}

void annexb() noexcept {
    EncodedUnitBuffer out{};
    // A keyframe with an access unit delimiter and 3-byte start codes, no parameter sets: the kept ones go first.
    constexpr u8 sps_pps[]{0, 0, 0, 1, 0x67, 0x42, 0xE0, 0x1F, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80};
    constexpr u8 idr[]{0, 0, 1, 0x09, 0xF0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x00};
    constexpr u8 expected[]{0, 0, 0, 1, 0x67, 0x42, 0xE0, 0x1F, 0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80,
                            0, 0, 0, 1, 0x65, 0x88, 0x84};
    check(encoded_annexb_unit(GpuStreamCodec::h264, {idr, sizeof(idr)}, {sps_pps, sizeof(sps_pps)}, true, &out) &&
              out.count == sizeof(expected) && bytes_equal(out.data, expected, sizeof(expected)),
          "H.264 keyframes lead with their parameter sets, 4-byte start codes, no delimiters or trailing zeros");
    // A P picture keeps no parameter sets; one that carries an SPS is not given another.
    constexpr u8 p[]{0, 0, 0, 1, 0x41, 0x9A, 0x02};
    check(encoded_annexb_unit(GpuStreamCodec::h264, {p, sizeof(p)}, {sps_pps, sizeof(sps_pps)}, false, &out) &&
              out.count == sizeof(p) && bytes_equal(out.data, p, sizeof(p)),
          "P pictures pass unchanged");
    check(encoded_annexb_unit(GpuStreamCodec::h264, {sps_pps, sizeof(sps_pps)}, {sps_pps, sizeof(sps_pps)}, true, &out) &&
              out.count == sizeof(sps_pps) + 1,
          "a keyframe with its SPS gets no second one");
    // H.265: the AUD (35) goes, the VPS (32) stays.
    constexpr u8 hevc[]{0, 0, 1, 0x46, 0x01, 0x50, 0, 0, 0, 1, 0x40, 0x01, 0x0C, 0, 0, 1, 0x26, 0x01, 0xAF};
    constexpr u8 hevc_expected[]{0, 0, 0, 1, 0x40, 0x01, 0x0C, 0, 0, 0, 1, 0x26, 0x01, 0xAF};
    check(encoded_annexb_unit(GpuStreamCodec::h265, {hevc, sizeof(hevc)}, {}, true, &out) &&
              out.count == sizeof(hevc_expected) && bytes_equal(out.data, hevc_expected, sizeof(hevc_expected)),
          "H.265 delimiters go");
    constexpr u8 only_delimiter[]{0, 0, 0, 1, 0x09, 0xF0};
    check(!encoded_annexb_unit(GpuStreamCodec::h264, {only_delimiter, sizeof(only_delimiter)}, {}, false, &out),
          "nothing left is no unit");
    encoded_unit_release(&out);
}

void av1() noexcept {
    EncodedUnitBuffer out{};
    ConstSlice<u8> found{};
    // Temporal delimiter, sequence header (with size), padding, then a frame OBU without obu_size.
    constexpr u8 input[]{0x12, 0x00, 0x0A, 0x02, 0xAB, 0xCD, 0x7A, 0x01, 0x00, 0x30, 0x11, 0x22, 0x33};
    constexpr u8 expected[]{0x0A, 0x02, 0xAB, 0xCD, 0x32, 0x03, 0x11, 0x22, 0x33};
    check(encoded_av1_unit({input, sizeof(input)}, {}, true, &out, &found) && out.count == sizeof(expected) &&
              bytes_equal(out.data, expected, sizeof(expected)) && found.count == 4 && found.data == out.data,
          "AV1: no temporal delimiter or padding, every OBU sized, the sequence header found");
    // A keyframe without a sequence header gets the kept one.
    constexpr u8 header[]{0x0A, 0x02, 0xAB, 0xCD};
    constexpr u8 frame[]{0x32, 0x02, 0x44, 0x55};
    constexpr u8 keyed[]{0x0A, 0x02, 0xAB, 0xCD, 0x32, 0x02, 0x44, 0x55};
    check(encoded_av1_unit({frame, sizeof(frame)}, {header, sizeof(header)}, true, &out, &found) &&
              out.count == sizeof(keyed) && bytes_equal(out.data, keyed, sizeof(keyed)),
          "AV1 keyframes lead with the sequence header");
    check(encoded_av1_unit({frame, sizeof(frame)}, {header, sizeof(header)}, false, &out, &found) &&
              out.count == sizeof(frame) && !found.count,
          "other AV1 units pass unchanged");
    constexpr u8 truncated[]{0x32, 0x05, 0x44};
    check(!encoded_av1_unit({truncated, sizeof(truncated)}, {}, false, &out, &found), "malformed AV1 is refused");
    encoded_unit_release(&out);
}

u32 read_bits(const u8* data, u32 position, u32 bits) noexcept {
    u32 value = 0;
    for (u32 i = 0; i < bits; ++i, ++position)
        value = value << 1 | (data[position >> 3] >> (7 - (position & 7)) & 1);
    return value;
}

u32 bits_for(u32 value) noexcept {
    u32 bits = 1;
    while (value >> bits)
        ++bits;
    return bits;
}

// One frame header written, then parsed: every field it sets comes back, and the layout points at the fields.
bool frame_round_trip(const Av1SequenceSettings& s, const Av1SequenceHeader& parsed, Av1References& references,
                      const Av1FrameSettings& f, u32 size_bytes) noexcept {
    u8 bytes[256]{};
    Av1FrameHeaderLayout layout{};
    const u32 count = av1_write_frame_header(s, f, size_bytes, bytes, sizeof(bytes), &layout);
    u32 offset = 0;
    Av1Obu obu{};
    Av1FrameHeader h{};
    if (!count || av1_next_obu({bytes, count}, &offset, &obu) != Av1Status::ok || offset != count ||
        obu.type != kAv1ObuFrameHeader || obu.payload.count != count - 1 - size_bytes ||
        av1_parse_frame_header(parsed, references, obu.payload, 0, 0, &h) != Av1Status::ok)
        return false;
    bool ok = h.frame_type == (f.key ? av1_key_frame : av1_inter_frame) && h.show_frame && !h.show_existing_frame &&
              h.error_resilient_mode == f.key && !h.disable_cdf_update && !h.disable_frame_end_update_cdf &&
              !h.allow_screen_content_tools && h.order_hint == f.order_hint && h.refresh_frame_flags == 0xFF &&
              h.primary_ref_frame == (f.key ? kAv1PrimaryRefNone : f.primary_ref_frame) &&
              h.upscaled_width == s.width && h.frame_width == s.width && h.frame_height == s.height &&
              h.render_width == s.width && h.render_height == s.height && h.tile_columns == 1 && h.tile_rows == 1 &&
              h.base_q_idx == f.base_q_idx && !h.delta_q_y_dc && !h.delta_q_u_dc && !h.delta_q_u_ac &&
              h.qm_y == 0xff && !h.segmentation_enabled && !h.delta_q_present && !h.coded_lossless &&
              h.loop_filter_level[0] == f.loop_filter_level[0] && h.loop_filter_level[1] == f.loop_filter_level[1] &&
              h.loop_filter_sharpness == f.loop_filter_sharpness && !h.loop_filter_delta_enabled &&
              h.tx_mode == (f.tx_mode_select ? 2 : 1) && !h.reference_select && !h.skip_mode_present &&
              !h.allow_warped_motion && !h.reduced_tx_set && !h.use_superres &&
              h.header_bytes <= obu.payload.count && obu.payload.count - h.header_bytes <= 1;
    const bool chroma_levels = f.loop_filter_level[0] || f.loop_filter_level[1];
    ok = ok && h.loop_filter_level[2] == (chroma_levels ? f.loop_filter_level[2] : 0) &&
         h.loop_filter_level[3] == (chroma_levels ? f.loop_filter_level[3] : 0);
    if (!f.key) {
        ok = ok && h.allow_high_precision_mv == f.allow_high_precision_mv &&
             h.interpolation_filter == f.interpolation_filter && !h.is_motion_mode_switchable &&
             !h.use_ref_frame_mvs;
        for (u32 i = 0; i < kAv1RefsPerFrame; ++i)
            ok = ok && h.ref_frame_idx[i] == f.ref_slot && h.gm_type[i] == 0;
    }
    if (s.enable_cdef) {
        ok = ok && h.cdef_damping_minus_3 == f.cdef_damping_minus_3 && h.cdef_bits == f.cdef_bits;
        for (u32 i = 0; i < (1u << f.cdef_bits); ++i)
            ok = ok && h.cdef_y_primary[i] == f.cdef_y_strengths[i] >> 2 &&
                 h.cdef_y_secondary[i] == (f.cdef_y_strengths[i] & 3) &&
                 h.cdef_uv_primary[i] == f.cdef_uv_strengths[i] >> 2 &&
                 h.cdef_uv_secondary[i] == (f.cdef_uv_strengths[i] & 3);
    }
    // The positions the driver patches.
    ok = ok && layout.obu_size_byte == 1 && layout.bits == count * 8 &&
         read_bits(bytes, layout.qindex, 8) == f.base_q_idx && read_bits(bytes, layout.segmentation, 1) == 0 &&
         read_bits(bytes, layout.loop_filter, 6) == f.loop_filter_level[0] &&
         layout.loop_filter > layout.segmentation && layout.cdef > layout.loop_filter &&
         layout.cdef_size == (s.enable_cdef ? 4 + 12 * (1u << f.cdef_bits) : 0);
    if (s.enable_cdef)
        ok = ok && read_bits(bytes, layout.cdef, 2) == f.cdef_damping_minus_3 &&
             read_bits(bytes, layout.cdef + 2, 2) == f.cdef_bits;
    av1_update_references(references, h);
    return ok;
}

void av1_headers() noexcept {
    const Av1SequenceSettings sequences[]{{1280, 720, 9, true},   {1920, 1080, 9, false}, {3840, 2160, 13, true},
                                          {4096, 2304, 13, true}, {640, 360, 5, true},    {2, 2, 0, false},
                                          {1366, 768, 8, true}};
    u32 frames = 0;
    for (const Av1SequenceSettings& s : sequences) {
        u8 bytes[64]{};
        const u32 count = av1_write_sequence_header(s, bytes, sizeof(bytes));
        u32 offset = 0;
        Av1Obu obu{};
        Av1SequenceHeader h{};
        const bool parsed = count && av1_next_obu({bytes, count}, &offset, &obu) == Av1Status::ok && offset == count &&
                            obu.type == kAv1ObuSequenceHeader &&
                            av1_parse_sequence_header(obu.payload, &h) == Av1Status::ok;
        check(parsed && h.profile == 0 && h.bit_depth == 8 && !h.reduced_still_picture_header &&
                  h.operating_points == 1 && h.operating_point_idc == 0 && !h.decoder_model_info_present &&
                  h.max_width == s.width && h.max_height == s.height && h.frame_width_bits == bits_for(s.width - 1) &&
                  h.frame_height_bits == bits_for(s.height - 1) && !h.frame_id_bits && !h.use_128x128_superblock &&
                  !h.enable_filter_intra && !h.enable_intra_edge_filter && !h.enable_interintra_compound &&
                  !h.enable_masked_compound && !h.enable_warped_motion && !h.enable_dual_filter &&
                  h.enable_order_hint && h.order_hint_bits == 8 && !h.enable_jnt_comp && !h.enable_ref_frame_mvs &&
                  h.force_screen_content_tools == 0 && !h.enable_superres && h.enable_cdef == s.enable_cdef &&
                  !h.enable_restoration && !h.mono_chrome && h.subsampling_x && h.subsampling_y && !h.color_range &&
                  h.matrix_coefficients == 1 && !h.separate_uv_delta_q && !h.film_grain_params_present,
              "AV1 sequence headers round-trip");
        if (!parsed)
            continue;
        static Av1References references;
        reset_value(&references);
        Av1FrameSettings key{};
        key.key = true;
        key.base_q_idx = 128;
        key.loop_filter_level[0] = key.loop_filter_level[1] = key.loop_filter_level[2] = key.loop_filter_level[3] = 10;
        key.cdef_y_strengths[0] = key.cdef_uv_strengths[0] = 4 << 2 | 1;
        key.tx_mode_select = true;
        check(frame_round_trip(s, h, references, key, 4), "AV1 key frame headers round-trip");
        ++frames;
        // Inter frames over every interpolation filter, CDEF strength count, quantizer edge, loop filter shape,
        // transform mode, primary reference, reference slot and obu_size width.
        for (u32 i = 0; i < 40; ++i) {
            Av1FrameSettings f{};
            f.order_hint = static_cast<u8>(1 + i * 7);
            f.primary_ref_frame = i % 3 ? 0 : 7;
            f.ref_slot = static_cast<u8>(i % 8);
            f.interpolation_filter = static_cast<u8>(i % 5);
            f.allow_high_precision_mv = i & 1;
            f.base_q_idx = static_cast<u8>(i % 4 == 0 ? 1 : i % 4 == 1 ? 255 : 37 * i % 254 + 1);
            const bool no_luma = i % 6 == 2;
            f.loop_filter_level[0] = static_cast<u8>(no_luma ? 0 : (i * 5) % 64);
            f.loop_filter_level[1] = static_cast<u8>(no_luma ? 0 : (i * 11 + 3) % 64);
            f.loop_filter_level[2] = static_cast<u8>((i * 13 + 1) % 64);
            f.loop_filter_level[3] = static_cast<u8>((i * 17 + 2) % 64);
            f.loop_filter_sharpness = static_cast<u8>(i % 8);
            f.cdef_damping_minus_3 = static_cast<u8>(i % 4);
            f.cdef_bits = static_cast<u8>(i / 4 % 4);
            for (u32 j = 0; j < 8; ++j) {
                f.cdef_y_strengths[j] = static_cast<u8>((i + 3 * j) % 64);
                f.cdef_uv_strengths[j] = static_cast<u8>((2 * i + 5 * j) % 64);
            }
            f.tx_mode_select = i % 2 == 0;
            const u32 size_bytes = 1 + i % 8;
            check(frame_round_trip(s, h, references, f, size_bytes), "AV1 inter frame headers round-trip");
            ++frames;
        }
    }
    check(frames == 7 * 41, "every AV1 configuration was parsed");
    u8 small[4];
    Av1FrameHeaderLayout layout{};
    Av1FrameSettings f{};
    f.key = true;
    f.base_q_idx = 10;
    check(!av1_write_sequence_header(sequences[0], small, sizeof(small)) &&
              !av1_write_frame_header(sequences[0], f, 4, small, sizeof(small), &layout),
          "AV1 headers that do not fit write nothing");
    f.base_q_idx = 0;
    u8 room[64];
    check(!av1_write_frame_header(sequences[0], f, 4, room, sizeof(room), &layout), "a lossless quantizer is refused");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_stream_encoder_units_entry) {
    using namespace maicord;
    presets();
    extents();
    tiers();
    annexb();
    av1();
    av1_headers();
    test_exit(g_failures, "maicord_stream_encoder_units");
}
