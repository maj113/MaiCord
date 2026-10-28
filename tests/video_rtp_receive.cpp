#include "maicord/app/video_rtp_receive.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/media/rtp_av1.hpp"
#include "maicord/media/rtp_h264.hpp"
#include "maicord/media/rtp_h265.hpp"
#include "maicord/sys/crt.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/rtp_video_vectors.hpp"
bool equal_hex(ConstSlice<u8> bytes, const char* hex) noexcept {
    static u8 expected[16384];
    const u32 count = test_unhex(hex, expected);
    return bytes.count == count && bytes_equal(bytes.data, expected, count);
}
template <u32 N> ConstSlice<u8> view(const u8 (&bytes)[N]) noexcept {
    return {bytes, N};
}
// Finds an RTCP packet of type/FMT inside a compound; returns its offset+1.
u32 find_rtcp(ConstSlice<u8> compound, u8 type, u8 format) noexcept {
    u32 offset = 0;
    while (compound.count - offset >= 4) {
        const u32 bytes = (u32{byte_load_be16(compound.data + offset + 2)} + 1) * 4;
        if (bytes > compound.count - offset)
            return 0;
        if (compound[offset + 1] == type && (compound[offset] & 31) == format)
            return offset + 1;
        offset += bytes;
    }
    return 0;
}

void test_h264(Check check) noexcept {
    u8 out[64];
    RtpVideoAssembly a;
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 single[]{0x65, 0x88, 0x01};
    const u8 stap[]{0x78, 0x00, 0x02, 0x67, 0xaa, 0x00, 0x02, 0x68, 0xbb};
    check(rtp_h264_push(&a, view(single), false) == RtpDepacketize::appended &&
              rtp_h264_push(&a, view(stap), false) == RtpDepacketize::appended &&
              equal_hex({out, a.bytes}, "0000000165880100000001" "67aa" "0000000168bb") &&
              rtp_h264_keyframe(a) && !a.damaged,
          "H.264 single NAL and STAP-A emit 4-byte start codes byte-exactly");
    const u32 before = a.bytes;
    const u8 bad_staps[][6]{{0x78, 0x00, 0x05, 0x67, 0xaa, 0xbb}, {0x78, 0x00, 0x00, 0x00, 0x01, 0x67},
                            {0x78, 0x00, 0x01, 0x67, 0x00, 0x00}, {0x78, 0x00, 0x02, 0x7c, 0x85, 0x00}};
    bool rejected = true;
    for (const auto& bad : bad_staps)
        rejected = rejected && rtp_h264_push(&a, view(bad), false) == RtpDepacketize::damaged;
    const u8 trailing[]{0x78, 0x00, 0x01, 0x67, 0x00};
    const u8 unsupported[][2]{{0x79, 1}, {0x7a, 1}, {0x7b, 1}, {0x7d, 1}, {0x60, 1}, {0x7e, 1}, {0xe5, 1}};
    rejected = rejected && rtp_h264_push(&a, view(trailing), false) == RtpDepacketize::damaged;
    for (const auto& bad : unsupported)
        rejected = rejected && rtp_h264_push(&a, view(bad), false) == RtpDepacketize::damaged;
    check(rejected && a.bytes == before && a.damaged,
          "H.264 rejects malformed STAP-A, STAP-B/MTAP/FU-B, reserved types and forbidden bit without output");

    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 fu_start[]{0x7c, 0x85, 0xd1}, fu_middle[]{0x7c, 0x05, 0xd2}, fu_end[]{0x7c, 0x45, 0xd3};
    check(rtp_h264_push(&a, view(fu_start), false) == RtpDepacketize::appended && a.fu_active &&
              rtp_h264_push(&a, view(fu_middle), false) == RtpDepacketize::appended &&
              rtp_h264_push(&a, view(fu_end), false) == RtpDepacketize::appended && !a.fu_active &&
              equal_hex({out, a.bytes}, "0000000165d1d2d3") && rtp_h264_keyframe(a) && !a.damaged,
          "H.264 FU-A reassembles the NAL header from indicator NRI and FU type");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 both[]{0x7c, 0xc5, 0xd1}, wrong_type[]{0x7c, 0x01, 0xd2}, aggregate_type[]{0x7c, 0x98, 0x00};
    check(rtp_h264_push(&a, view(both), false) == RtpDepacketize::damaged &&
              rtp_h264_push(&a, view(fu_middle), false) == RtpDepacketize::damaged &&
              rtp_h264_push(&a, view(aggregate_type), false) == RtpDepacketize::damaged && !a.bytes,
          "H.264 FU-A rejects start+end, orphan continuations and aggregate FU types");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    rtp_h264_push(&a, view(single), false);
    const u32 kept = a.bytes;
    check(rtp_h264_push(&a, view(fu_start), false) == RtpDepacketize::appended &&
              rtp_h264_push(&a, view(fu_end), true) == RtpDepacketize::damaged && a.bytes == kept &&
              !a.fu_active && a.damaged,
          "H.264 sequence gap drops the partial FU-A and marks the unit damaged");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    rtp_h264_push(&a, view(fu_start), false);
    check(rtp_h264_push(&a, view(wrong_type), false) == RtpDepacketize::damaged && !a.bytes && a.damaged,
          "H.264 FU-A type change drops the fragment");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    rtp_h264_push(&a, view(fu_start), false);
    const u8 restart[]{0x7c, 0x81, 0xe1};
    check(rtp_h264_push(&a, view(restart), false) == RtpDepacketize::appended && a.damaged &&
              equal_hex({out, a.bytes}, "0000000161e1") && rtp_video_assembly_finish(&a) == 0 && a.damaged,
          "H.264 new FU start replaces an unterminated fragment; finish removes an open fragment");
    rtp_video_assembly_begin(&a, {out, 6});
    check(rtp_h264_push(&a, view(single), false) == RtpDepacketize::overflow && !a.bytes &&
              rtp_h264_push(&a, view(stap), false) == RtpDepacketize::overflow && !a.bytes,
          "H.264 output overflow keeps nothing from the payload");
    const u8 slice_start[]{0x41, 0x9a}, slice_next[]{0x41, 0x40}, fu_slice[]{0x5c, 0x81, 0x80},
        fu_tail[]{0x5c, 0x01, 0x80}, sps[]{0x67, 0x00}, stap_sps[]{0x78, 0x00, 0x02, 0x67, 0x42};
    check(rtp_h264_starts_access_unit(view(slice_start)) && !rtp_h264_starts_access_unit(view(slice_next)) &&
              rtp_h264_starts_access_unit(view(fu_slice)) && !rtp_h264_starts_access_unit(view(fu_tail)) &&
              rtp_h264_starts_access_unit(view(sps)) && rtp_h264_starts_access_unit(view(stap_sps)) &&
              !rtp_h264_starts_access_unit({nullptr, 0}),
          "H.264 access-unit start detection uses parameter sets and first_mb_in_slice");
}

void test_h265(Check check) noexcept {
    u8 out[64];
    RtpVideoAssembly a;
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 single[]{0x26, 0x01, 0xaf};
    const u8 ap[]{0x60, 0x01, 0x00, 0x03, 0x40, 0x01, 0xaa, 0x00, 0x03, 0x42, 0x01, 0xbb};
    check(rtp_h265_push(&a, view(single), false) == RtpDepacketize::appended &&
              rtp_h265_push(&a, view(ap), false) == RtpDepacketize::appended &&
              equal_hex({out, a.bytes}, "000000012601af" "00000001" "4001aa" "00000001" "4201bb") &&
              rtp_h265_keyframe(a),
          "H.265 single NAL and aggregation packet emit 4-byte start codes");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 s[]{0x62, 0x01, 0x93, 0xd1}, m[]{0x62, 0x01, 0x13, 0xd2}, e[]{0x62, 0x01, 0x53, 0xd3};
    check(rtp_h265_push(&a, view(s), false) == RtpDepacketize::appended &&
              rtp_h265_push(&a, view(m), false) == RtpDepacketize::appended &&
              rtp_h265_push(&a, view(e), false) == RtpDepacketize::appended &&
              equal_hex({out, a.bytes}, "000000012601d1d2d3") && rtp_h265_keyframe(a) && !a.damaged,
          "H.265 FU reconstructs the two-byte NAL header");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    rtp_h265_push(&a, view(s), false);
    check(rtp_h265_push(&a, view(e), true) == RtpDepacketize::damaged && !a.bytes && a.damaged,
          "H.265 sequence gap drops the partial FU");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 paci[]{0x64, 0x01, 0x00}, tid_zero[]{0x26, 0x00, 0x00}, forbidden[]{0xa6, 0x01, 0x00},
        ap_nested[]{0x60, 0x01, 0x00, 0x03, 0x62, 0x01, 0x00}, ap_short[]{0x60, 0x01, 0x00, 0x01, 0x40},
        fu_both[]{0x62, 0x01, 0xd3, 0x00}, fu_ap_type[]{0x62, 0x01, 0xb0, 0x00}, tiny[]{0x26};
    check(rtp_h265_push(&a, view(paci), false) == RtpDepacketize::damaged &&
              rtp_h265_push(&a, view(tid_zero), false) == RtpDepacketize::damaged &&
              rtp_h265_push(&a, view(forbidden), false) == RtpDepacketize::damaged &&
              rtp_h265_push(&a, view(ap_nested), false) == RtpDepacketize::damaged &&
              rtp_h265_push(&a, view(ap_short), false) == RtpDepacketize::damaged &&
              rtp_h265_push(&a, view(fu_both), false) == RtpDepacketize::damaged &&
              rtp_h265_push(&a, view(fu_ap_type), false) == RtpDepacketize::damaged &&
              rtp_h265_push(&a, view(tiny), false) == RtpDepacketize::damaged &&
              rtp_h265_push(&a, view(m), false) == RtpDepacketize::damaged && !a.bytes,
          "H.265 rejects PACI, TID 0, forbidden bit, nested/short aggregates and invalid FUs");
    rtp_video_assembly_begin(&a, {out, 8});
    check(rtp_h265_push(&a, view(ap), false) == RtpDepacketize::overflow && !a.bytes,
          "H.265 overflow keeps nothing");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 ap_one[]{0x60, 0x01, 0x00, 0x03, 0x44, 0x01, 0xc1}, layered[]{0x26, 0x09, 0xaf};
    check(rtp_h265_push(&a, view(ap_one), false) == RtpDepacketize::appended &&
              rtp_h265_push(&a, view(layered), false) == RtpDepacketize::appended &&
              equal_hex({out, a.bytes}, "000000014401c1" "000000012609af") && !a.damaged,
          "H.265 accepts a one-unit AP and passes other layers' NAL units to the decoder, which skips them");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 restart[]{0x62, 0x01, 0x93, 0xe1};
    check(rtp_h265_push(&a, view(s), false) == RtpDepacketize::appended &&
              rtp_h265_push(&a, view(restart), false) == RtpDepacketize::appended &&
              rtp_h265_push(&a, view(e), false) == RtpDepacketize::appended &&
              equal_hex({out, a.bytes}, "000000012601e1d3") && a.damaged,
          "H.265 FU restart drops the unfinished fragment and marks the unit damaged");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    check(rtp_h265_push(&a, view(e), false) == RtpDepacketize::damaged && !a.bytes &&
              rtp_h265_push(&a, view(s), false) == RtpDepacketize::appended && !rtp_video_assembly_finish(&a) &&
              a.damaged,
          "H.265 FU end without a start, and a start without an end, leave nothing");
    const u8 trail_start[]{0x02, 0x01, 0x80}, trail_next[]{0x02, 0x01, 0x40}, vps[]{0x40, 0x01},
        fu_start[]{0x62, 0x01, 0x81, 0x80}, ap_vps[]{0x60, 0x01, 0x00, 0x02, 0x40, 0x01};
    check(rtp_h265_starts_access_unit(view(trail_start)) && !rtp_h265_starts_access_unit(view(trail_next)) &&
              rtp_h265_starts_access_unit(view(vps)) && rtp_h265_starts_access_unit(view(fu_start)) &&
              rtp_h265_starts_access_unit(view(ap_vps)) && !rtp_h265_starts_access_unit(view(m)),
          "H.265 access-unit start detection uses first_slice_segment_in_pic_flag");
}

void test_av1(Check check) noexcept {
    u8 out[64];
    RtpVideoAssembly a;
    // W=2: the first element has a length, the last runs to the end.
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 pair[]{0x20, 0x03, 0x08, 0xa1, 0xa2, 0x30, 0xb1, 0xb2, 0xb3};
    check(rtp_av1_push(&a, view(pair), false) == RtpDepacketize::appended &&
              rtp_video_assembly_finish(&a) == 8 && equal_hex({out, a.bytes}, "0a02a1a2" "30b1b2b3") &&
              rtp_av1_keyframe(a) && !a.damaged,
          "AV1 W=2 sizes every OBU but the last and treats a sequence header as a key unit");
    // W=0: every element has a length; an element's own obu_size is kept.
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 sized[]{0x00, 0x04, 0x1a, 0x02, 0xc1, 0xc2, 0x02, 0x20, 0xd1};
    check(rtp_av1_push(&a, view(sized), false) == RtpDepacketize::appended &&
              equal_hex({out, a.bytes}, "1a02c1c2" "20d1") && !rtp_av1_keyframe(a),
          "AV1 W=0 lengths every element and keeps a matching obu_size");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 wrong_size[]{0x00, 0x04, 0x1a, 0x05, 0xc1, 0xc2, 0x02, 0x20, 0xd1};
    check(rtp_av1_push(&a, view(wrong_size), false) == RtpDepacketize::damaged && a.damaged,
          "AV1 rejects an obu_size that disagrees with its element");
    // A fragment continued by the next packet, followed by another OBU.
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 first[]{0x50, 0x30, 0xaa, 0xbb}, second[]{0xa0, 0x02, 0xcc, 0xdd, 0x18, 0xee};
    check(rtp_av1_push(&a, view(first), false) == RtpDepacketize::appended && a.fu_active &&
              rtp_av1_push(&a, view(second), false) == RtpDepacketize::appended && !a.fu_active &&
              equal_hex({out, a.bytes}, "3204aabbccdd" "18ee") && !a.damaged,
          "AV1 joins Z/Y fragments and sizes the joined OBU once the next begins");
    // An OBU announced by a header-only packet and carried by the next one.
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 announce[]{0x50}, carried[]{0x90, 0x30, 0x77};
    check(rtp_av1_push(&a, view(announce), false) == RtpDepacketize::appended &&
              rtp_av1_push(&a, view(carried), false) == RtpDepacketize::appended &&
              equal_hex({out, a.bytes}, "3077") && (a.nal_types >> kAv1ObuFrame & 1),
          "AV1 accepts an empty announcing element");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 empty[]{0x10}, no_elements[]{0x20}, orphan[]{0x90, 0x30};
    check(rtp_av1_push(&a, view(empty), false) == RtpDepacketize::damaged &&
              rtp_av1_push(&a, view(no_elements), false) == RtpDepacketize::damaged &&
              rtp_av1_push(&a, view(orphan), false) == RtpDepacketize::damaged && !a.bytes,
          "AV1 rejects an announced OBU that never arrives, W without elements and an orphan continuation");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    rtp_av1_push(&a, view(first), false);
    const u8 restart[]{0x10, 0x30, 0x01};
    check(rtp_av1_push(&a, view(restart), false) == RtpDepacketize::damaged && !a.bytes && a.damaged,
          "AV1 Z clear while a fragment is open drops the fragment");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    rtp_av1_push(&a, view(pair), false);
    const u32 kept = a.bytes;
    check(rtp_av1_push(&a, view(first), false) == RtpDepacketize::appended &&
              rtp_av1_push(&a, view(second), true) == RtpDepacketize::damaged && a.damaged && !a.fu_active,
          "AV1 sequence gap drops the open fragment");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    rtp_av1_push(&a, view(pair), false);
    rtp_av1_push(&a, view(first), false);
    check(rtp_video_assembly_finish(&a) == kept + 1 && a.damaged,
          "AV1 finish removes a fragment the last packet left open");
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 long_size[]{0x00, 0x09, 0x30}, bad_leb[]{0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01},
        forbidden[]{0x10, 0xb0, 0x01};
    check(rtp_av1_push(&a, view(long_size), false) == RtpDepacketize::damaged &&
              rtp_av1_push(&a, view(bad_leb), false) == RtpDepacketize::damaged &&
              rtp_av1_push(&a, view(forbidden), false) == RtpDepacketize::damaged && !a.bytes,
          "AV1 rejects overlong element lengths, unterminated LEB128 and the forbidden bit");
    // Extension headers stay in front of the inserted size; N marks a key unit.
    rtp_video_assembly_begin(&a, {out, sizeof(out)});
    const u8 extended[]{0x28, 0x03, 0x34, 0x20, 0xe1, 0x30, 0xe2};
    check(rtp_av1_push(&a, view(extended), false) == RtpDepacketize::appended &&
              equal_hex({out, a.bytes}, "362001e1" "30e2") && rtp_av1_keyframe(a),
          "AV1 sizes an extended OBU after its extension byte and honours N");
    rtp_video_assembly_begin(&a, {out, 6});
    check(rtp_av1_push(&a, view(pair), false) == RtpDepacketize::overflow && !a.bytes,
          "AV1 output overflow keeps nothing from the payload");
    const u8 frame_start[]{0x10, 0x30, 0x00}, tile_group[]{0x00, 0x02, 0x20, 0x00}, delimiter[]{0x10, 0x10},
        continued[]{0x90, 0x30}, keyed[]{0x08, 0x01, 0x20};
    check(rtp_av1_starts_temporal_unit(view(frame_start)) && !rtp_av1_starts_temporal_unit(view(tile_group)) &&
              rtp_av1_starts_temporal_unit(view(delimiter)) && rtp_av1_starts_temporal_unit(view(pair)) &&
              !rtp_av1_starts_temporal_unit(view(continued)) && rtp_av1_starts_temporal_unit(view(keyed)) &&
              !rtp_av1_starts_temporal_unit({nullptr, 0}),
          "AV1 temporal-unit start detection reads Z, N and the first OBU type");
}

void test_rtcp(Check check) noexcept {
    u8 out[256];
    const RtcpReportBlock blocks[]{{kOracleSsrc, 25, 7, 0x0001ffff, 123, 0x89abcdef, 0x00028000},
                                   {kOracleRtxSsrc + 9, 0, -3, 42, 0, 0, 0}};
    u32 bytes = rtcp_write_receiver_report({out, sizeof(out)}, 0, kOracleLocalSsrc, {blocks, 2});
    check(equal_hex({out, bytes}, rtcp_rr), "RTCP receiver report matches the oracle");
    bytes = rtcp_write_pli({out, sizeof(out)}, 0, kOracleLocalSsrc, kOracleSsrc);
    check(equal_hex({out, bytes}, rtcp_pli), "RTCP PLI matches the oracle");
    bytes = rtcp_write_fir({out, sizeof(out)}, 0, kOracleLocalSsrc, kOracleSsrc, 9);
    check(equal_hex({out, bytes}, rtcp_fir), "RTCP FIR matches the oracle");
    RtcpNackItem items[8];
    u32 consumed = 0;
    const u32 count = rtcp_nack_pack({rtcp_nack_sequences, sizeof(rtcp_nack_sequences) / 2}, {items, 8}, &consumed);
    bytes = rtcp_write_nack({out, sizeof(out)}, 0, kOracleLocalSsrc, kOracleSsrc, {items, count});
    check(count == rtcp_nack_items && consumed == sizeof(rtcp_nack_sequences) / 2 && equal_hex({out, bytes}, rtcp_nack),
          "RTCP generic NACK packs PID/BLP across the sequence wrap like the oracle");
    check(rtcp_nack_pack({rtcp_nack_sequences, sizeof(rtcp_nack_sequences) / 2}, {items, 2}, &consumed) == 2 &&
              consumed == 6,
          "RTCP NACK packing reports how many sequences fit");
    check(!rtcp_write_pli({out, 11}, 0, 1, 2) && !rtcp_write_receiver_report({out, 31}, 0, 1, {blocks, 1}) &&
              !rtcp_write_nack({out, sizeof(out)}, 0, 1, 2, {items, 0}) && !rtcp_write_fir({out, 40}, 24, 1, 2, 0),
          "RTCP writers refuse short output and empty NACKs");
    u8 compound[64];
    const u32 compound_bytes = test_unhex(rtcp_sr_compound, compound);
    RtcpSenderInfo info[2]{};
    check(rtcp_read_sender_reports({compound, compound_bytes}, {info, 2}) == 1 && info[0].ssrc == kOracleSsrc &&
              rtcp_compact_ntp(info[0].ntp_seconds, info[0].ntp_fraction) == 0x456789ab &&
              info[0].rtp_timestamp == 90000 && info[0].packet_count == 10 && info[0].octet_count == 9000,
          "RTCP sender report parsing yields LSR and counters");
}

// Transport-wide CC feedback (RTPFB FMT 15) and RTP header extension lookup.
// Expected bytes are derived by hand from draft-holmer-rmcat-transport-wide-
// cc-extensions-01: 64 ms reference ticks, 250 us truncated deltas.
void test_transport_feedback(Check check) noexcept {
    u8 out[256];
    // Received, received, lost, +68.75 ms (large), -1 ms (negative), +0.1 ms
    // (rounds to 0): one two-bit vector chunk 01 01 00 10 10 01.
    const u64 mixed[]{6'401'000, 6'401'250, 0, 6'470'000, 6'469'000, 6'469'100};
    auto written = rtcp_write_transport_feedback({out, sizeof(out)}, 0, kOracleLocalSsrc, kOracleSsrc, 0xfffe, 7,
                                                 {mixed, 6});
    check(written.packets == 6 && written.end == 32 &&
              equal_hex({out, written.end}, "8fcd000700c0ffee1234abcdfffe000600006407d4a404010113fffc00000000"),
          "Transport feedback: vector chunk with loss, large and negative deltas, zero padded");
    // Nine lost then eight received at 1 ms: two run-length chunks.
    u64 runs[17]{};
    for (u32 i = 0; i != 8; ++i)
        runs[9 + i] = 320'500 + i * 1000;
    written = rtcp_write_transport_feedback({out, sizeof(out)}, 0, kOracleLocalSsrc, kOracleSsrc, 10, 0, {runs, 17});
    check(written.packets == 17 &&
              equal_hex({out, written.end}, "8fcd000700c0ffee1234abcd000a0011000005000009200802040404" "04040404"),
          "Transport feedback: runs of seven or more statuses are run-length coded");
    // The room left bounds the statuses; a lost run alone is never sent.
    u64 steady[20];
    for (u32 i = 0; i != 20; ++i)
        steady[i] = 640'000 + i * 1000;
    memset(out, 0xAA, sizeof(out));
    written = rtcp_write_transport_feedback({out, 40}, 0, 1, 2, 3, 4, {steady, 20});
    check(written.packets == 18 && written.end == 40 && out[14] == 0 && out[15] == 18 && out[20] == 0x20 &&
              out[21] == 18,
          "Transport feedback: a short output reports the prefix that fits");
    memset(out, 0xAA, sizeof(out));
    check(!rtcp_write_transport_feedback({out, 23}, 0, 1, 2, 3, 4, {steady, 20}).packets &&
              !rtcp_write_transport_feedback({out, 24}, 0, 1, 2, 3, 4, {runs, 17}).packets &&
              !rtcp_write_transport_feedback({out, sizeof(out)}, 0, 1, 2, 3, 4, {runs, 9}).packets &&
              !rtcp_write_transport_feedback({out, sizeof(out)}, 0, 1, 2, 3, 4, {runs, 0}).packets && out[0] == 0xAA,
          "Transport feedback: no room for a received status, no received packet or no input writes nothing");
    const u64 far[]{1'000'000, 10'000'000};
    written = rtcp_write_transport_feedback({out, sizeof(out)}, 0, 1, 2, 3, 4, {far, 2});
    check(written.packets == 1, "Transport feedback: a delta beyond 16 bits ends the packet before it");

    // Recorder: sequence wrap, a loss, then a late packet rewinds the report.
    static RtcpTransportFeedbackState state;
    rtcp_transport_feedback_reset(&state);
    check(!rtcp_transport_feedback_deadline(state) &&
              rtcp_transport_feedback_build(&state, {out, sizeof(out)}, 8, 1, 1'000'000) == 8,
          "Transport feedback recorder: nothing recorded, nothing due");
    rtcp_transport_feedback_record(&state, 65534, 0x01020304, 1'000'000);
    rtcp_transport_feedback_record(&state, 65535, 0x01020304, 1'001'000);
    rtcp_transport_feedback_record(&state, 1, 0x01020304, 1'003'000);
    rtcp_transport_feedback_record(&state, 1, 0x01020304, 1'009'000);
    u32 end = rtcp_transport_feedback_build(&state, {out, sizeof(out)}, 0, kOracleLocalSsrc, 1'003'500);
    check(state.packets == 3 && rtcp_transport_feedback_deadline(state) == 0 &&
              equal_hex({out, end}, "8fcd000600c0ffee01020304fffe000400000f00d440a00408000000"),
          "Transport feedback recorder: wrapped sequences and a gap report once; duplicates keep the first arrival");
    rtcp_transport_feedback_record(&state, 0, 0x01020304, 1'004'000);
    check(rtcp_transport_feedback_deadline(state) == 1'053'500 &&
              rtcp_transport_feedback_build(&state, {out, sizeof(out)}, 0, kOracleLocalSsrc, 1'053'499) == 0,
          "Transport feedback recorder: the next packet waits for the 50 ms interval");
    end = rtcp_transport_feedback_build(&state, {out, sizeof(out)}, 0, kOracleLocalSsrc, 1'053'500);
    check(equal_hex({out, end}, "8fcd000600c0ffee010203040000000200000f01d800b0fffc000000"),
          "Transport feedback recorder: a late packet is reported again with its successor");
    // A jump past the window forgets what can no longer be reported; a loss
    // run longer than one feedback packet is shortened to fit beside the
    // next received status.
    rtcp_transport_feedback_record(&state, 3000, 0x01020304, 1'100'000);
    const bool windowed = state.highest - state.next == kRtcpTransportFeedbackWindow - 1;
    end = rtcp_transport_feedback_build(&state, {out, sizeof(out)}, 0, 1, 1'200'000);
    check(windowed && end == 28 && out[14] == 2 && out[15] == 0 && state.next == state.highest + 1 &&
              state.feedback == 3 && !rtcp_transport_feedback_deadline(state),
          "Transport feedback recorder: a wide jump keeps one window and reports its tail in one packet");

    // Header extensions: one-byte elements with padding, two-byte profile,
    // truncated elements and the reserved ID 15.
    u8 packet[40];
    const auto layout_of = [&](const char* hex, MediaPacketLayout* layout) noexcept {
        const u32 bytes = test_unhex(hex, packet);
        return media_packet_plaintext({packet, bytes}, layout) ? bytes : 0u;
    };
    MediaPacketLayout layout{};
    u32 bytes = layout_of("90650001000000001234abcdbede0002107f0051123400000102", &layout);
    auto found = rtp_header_extensions({packet, bytes}, layout, kRtpTransportSequenceExtensionId);
    check(bytes && found.found && found.value.count == 2 && byte_load_be16(found.value.data) == 0x1234 &&
              found.ids == 0x22 && layout.payload_bytes == 2,
          "RTP extensions: the transport sequence is found among padded one-byte elements");
    bytes = layout_of("90650001000000001234abcd100000010502abcd99", &layout);
    found = rtp_header_extensions({packet, bytes}, layout, 5);
    check(bytes && found.found && byte_load_be16(found.value.data) == 0xabcd && found.ids == 0x20,
          "RTP extensions: two-byte profile elements are found");
    bytes = layout_of("90650001000000001234abcdbede0001531234ff99", &layout);
    const bool truncated = bytes && !rtp_header_extensions({packet, bytes}, layout, 5).found;
    bytes = layout_of("90650001000000001234abcdbede0002f00051123400000099", &layout);
    const auto reserved = rtp_header_extensions({packet, bytes}, layout, 5);
    const bool reserved_parsed = bytes != 0;
    bytes = layout_of("80650001000000001234abcd99", &layout);
    check(truncated && reserved_parsed && !reserved.found && !reserved.ids && bytes &&
              !rtp_header_extensions({packet, bytes}, layout, 5).found,
          "RTP extensions: truncated elements, ID 15 and extension-less packets yield nothing");
}

// ---------------------------------------------------------------------------
// Receiver scenarios from the oracle.
struct Collector {
    const char* const* frames;
    const u8* delivered;
    u32 delivered_count, count, keyframes, references_lost;
    u32 sizes[64];
    bool match;
};
void collect(void* context, const VideoRtpFrame& frame) noexcept {
    auto& c = *static_cast<Collector*>(context);
    if (c.count < 64)
        c.sizes[c.count] = frame.annex_b.count;
    if (frame.keyframe)
        ++c.keyframes;
    if (frame.references_lost)
        ++c.references_lost;
    if (c.frames) {
        if (c.count >= c.delivered_count || !equal_hex(frame.annex_b, c.frames[c.delivered[c.count]]) ||
            frame.keyframe != (c.delivered[c.count] == 0) || frame.ssrc != kOracleSsrc || frame.user_id != 77)
            c.match = false;
    }
    ++c.count;
}
bool event_has_rtx(const RtpVideoScenario& scenario) noexcept {
    for (u32 i = 0; i != scenario.event_count; ++i)
        if (scenario.events[i].kind == 1)
            return true;
    return false;
}
VideoRtpReceiver receiver_storage;
bool start_receiver(VideoRtpReceiver& r, Collector& c) noexcept {
    r = {};
    const VideoRtpCodecBinding codecs[]{{101, 102, VideoRtpCodec::h264}, {103, 104, VideoRtpCodec::h265},
                                        {105, 106, VideoRtpCodec::av1}};
    return video_rtp_initialize(&r, kOracleLocalSsrc, collect, &c) && video_rtp_set_codecs(&r, {codecs, 3});
}
VideoRtpPush feed(VideoRtpReceiver& r, ConstSlice<u8> packet, bool rtx, u64 now_us) noexcept {
    MediaPacketLayout layout{};
    if (!media_packet_plaintext(packet, &layout))
        return VideoRtpPush::invalid;
    return video_rtp_receive(&r, {77, kOracleSsrc, rtx}, layout, packet, now_us);
}
void run_scenarios(Check check, const RtpVideoScenario* scenarios, u32 count, const char* const* packets,
                   const char* const* frames, u8 rtx_type) noexcept {
    static u8 packet[2048];
    for (u32 s = 0; s != count; ++s) {
        const auto& scenario = scenarios[s];
        Collector c{frames, scenario.delivered, scenario.delivered_count};
        c.match = true;
        auto& r = receiver_storage;
        if (!start_receiver(r, c)) {
            check(false, "video receiver initializes");
            return;
        }
        u64 now = 1'000'000;
        bool fed = true;
        for (u32 i = 0; i != scenario.event_count; ++i) {
            const auto& event = scenario.events[i];
            if (event.kind == 2) {
                now += u64{event.value} * 1000;
                video_rtp_tick(&r, now);
                continue;
            }
            now += 1000;
            u32 bytes = test_unhex(packets[event.value], packet + 2);
            u8* start = packet + 2;
            if (event.kind == 1) {
                // RFC 4588: RTX header, then the original sequence number.
                start = packet;
                copy_bytes(packet, packet + 2, 12);
                packet[12] = packet[2]; // Original sequence number.
                packet[13] = packet[3];
                packet[1] = static_cast<u8>((packet[1] & 0x80) | rtx_type);
                packet[2] = static_cast<u8>(event.rtx_sequence >> 8);
                packet[3] = static_cast<u8>(event.rtx_sequence);
                byte_store_be32(packet + 8, kOracleRtxSsrc);
                bytes += 2;
            }
            const auto result = feed(r, {start, bytes}, event.kind == 1, now);
            fed = fed && result != VideoRtpPush::invalid && result != VideoRtpPush::ignored;
        }
        now += 1'000'000;
        video_rtp_tick(&r, now);
        u8 rtcp[1500];
        const u32 rtcp_bytes = video_rtp_feedback(&r, now, {rtcp, sizeof(rtcp)});
        const bool pli = find_rtcp({rtcp, rtcp_bytes}, kRtcpPayloadFeedback, kRtcpFormatPli) != 0;
        const u64 deadline = video_rtp_deadline(r);
        const auto* stream = video_rtp_find_stream(r, kOracleSsrc);
        check(fed && c.match && c.count == scenario.delivered_count, scenario.name);
        check(pli == scenario.keyframe_request && rtcp_bytes && (!deadline || deadline > now) && stream &&
                  stream->rtx_ssrc == (event_has_rtx(scenario) ? kOracleRtxSsrc : 0) && !stream->missing,
              scenario.name);
        video_rtp_release(&r);
    }
}

u8 wire[2048];
u32 make(u8 type, u16 sequence, u32 timestamp, bool marker, u32 ssrc, ConstSlice<u8> payload,
         bool osn = false, u16 original = 0) noexcept {
    wire[0] = 0x80;
    wire[1] = static_cast<u8>((marker ? 0x80 : 0) | type);
    wire[2] = static_cast<u8>(sequence >> 8);
    wire[3] = static_cast<u8>(sequence);
    byte_store_be32(wire + 4, timestamp);
    byte_store_be32(wire + 8, ssrc);
    u32 offset = 12;
    if (osn) {
        wire[12] = static_cast<u8>(original >> 8);
        wire[13] = static_cast<u8>(original);
        offset = 14;
    }
    copy_bytes(wire + offset, payload.data, payload.count);
    return offset + payload.count;
}
VideoRtpPush send(VideoRtpReceiver& r, u16 sequence, u32 timestamp, ConstSlice<u8> payload, u64 now,
                  bool marker = true, u32 ssrc = kOracleSsrc) noexcept {
    MediaPacketLayout layout{};
    const u32 bytes = make(101, sequence, timestamp, marker, ssrc, payload);
    if (!media_packet_plaintext({wire, bytes}, &layout))
        return VideoRtpPush::invalid;
    return video_rtp_receive(&r, {77, ssrc, false}, layout, {wire, bytes}, now);
}
VideoRtpPush send_rtx(VideoRtpReceiver& r, u16 rtx_sequence, u16 original, u32 timestamp, ConstSlice<u8> payload,
                      u64 now) noexcept {
    MediaPacketLayout layout{};
    const u32 bytes = make(102, rtx_sequence, timestamp, true, kOracleRtxSsrc, payload, true, original);
    if (!media_packet_plaintext({wire, bytes}, &layout))
        return VideoRtpPush::invalid;
    return video_rtp_receive(&r, {77, kOracleSsrc, true}, layout, {wire, bytes}, now);
}

void test_receiver_feedback(Check check) noexcept {
    Collector c{};
    auto& r = receiver_storage;
    if (!start_receiver(r, c)) {
        check(false, "video receiver initializes");
        return;
    }
    const VideoRtpCodecBinding duplicate[]{{101, 102, VideoRtpCodec::h264}, {102, 0, VideoRtpCodec::h265}};
    const VideoRtpCodecBinding reserved[]{{72, 0, VideoRtpCodec::h264}};
    check(!video_rtp_set_codecs(&r, {duplicate, 2}) && !video_rtp_set_codecs(&r, {reserved, 1}) &&
              r.codec_count == 3,
          "video codec table rejects payload type collisions and RTCP-mux reserved types");
    const u8 idr[]{0x65, 0x88, 1, 2}, inter[]{0x41, 0x9a, 3, 4};
    u64 now = 5'000'000;
    send(r, 100, 0, view(idr), now);
    check(c.count == 0, "video stream start waits briefly for packets reordered before the first");
    now += 30'000;
    video_rtp_tick(&r, now);
    send(r, 101, 3000, view(inter), now += 1000);
    send(r, 103, 9000, view(inter), now += 1000);
    const auto* stream = video_rtp_find_stream(r, kOracleSsrc);
    check(c.count == 2 && c.keyframes == 1 && stream && stream->missing == 1,
          "video frames are emitted in order and a gap blocks later frames");
    now += 10'000;
    u8 rtcp[1500];
    u32 bytes = video_rtp_feedback(&r, now, {rtcp, sizeof(rtcp)});
    const u32 nack = find_rtcp({rtcp, bytes}, kRtcpTransportFeedback, kRtcpFormatGenericNack);
    check(bytes && rtcp[1] == kRtcpReceiverReport && (rtcp[0] & 31) == 1 &&
              byte_load_be32(rtcp + 4) == kOracleLocalSsrc && byte_load_be32(rtcp + 8) == kOracleSsrc &&
              (byte_load_be32(rtcp + 12) & 0xffffff) == 1 && byte_load_be32(rtcp + 16) == 103 && nack &&
              byte_load_be32(rtcp + nack + 7) == kOracleSsrc && byte_load_be16(rtcp + nack + 11) == 102 &&
              byte_load_be16(rtcp + nack + 13) == 0,
          "video feedback is RR (loss 1, highest 103) followed by a generic NACK for the gap");
    check(!video_rtp_feedback(&r, now + 1000, {rtcp, sizeof(rtcp)}),
          "video feedback is not rebuilt before the NACK retry or report interval");
    check(send_rtx(r, 9000, 102, 6000, view(inter), now += 1000) == VideoRtpPush::recovered && c.count == 4 &&
              stream->missing == 0 && stream->stats.rtx_packets == 1 && stream->rtx_ssrc == kOracleRtxSsrc &&
              send_rtx(r, 9001, 102, 6000, view(inter), now += 1000) == VideoRtpPush::late,
          "RTX unwraps the original sequence, fills the gap and releases blocked frames");
    send(r, 105, 15000, view(inter), now += 1000);
    u32 nacks = 0;
    for (u32 i = 0; i != 10; ++i) {
        now += 60'000;
        bytes = video_rtp_feedback(&r, now, {rtcp, sizeof(rtcp)});
        nacks += find_rtcp({rtcp, bytes}, kRtcpTransportFeedback, kRtcpFormatGenericNack) != 0;
    }
    check(nacks == r.config.max_nacks && c.count == 4, "a missing packet is NACKed at most max_nacks times");
    video_rtp_tick(&r, now);
    check(c.count == 5 && c.references_lost == 1 && stream->keyframe_needed && stream->stats.losses == 1,
          "an unrecovered gap is declared lost and the next detectable unit is delivered as reference-lost");
    bytes = video_rtp_feedback(&r, now, {rtcp, sizeof(rtcp)});
    const u32 pli = find_rtcp({rtcp, bytes}, kRtcpPayloadFeedback, kRtcpFormatPli);
    const u32 pli_bytes = bytes;
    const u32 pli_ssrc = pli ? byte_load_be32(rtcp + pli + 7) : 0;
    bytes = video_rtp_feedback(&r, now + 1000, {rtcp, sizeof(rtcp)});
    check(pli && pli_bytes && pli_ssrc == kOracleSsrc &&
              !find_rtcp({rtcp, bytes}, kRtcpPayloadFeedback, kRtcpFormatPli),
          "loss requests a keyframe with a rate-limited PLI");
    send(r, 106, 18000, view(idr), now += 1000);
    check(c.count == 6 && c.keyframes == 2 && !stream->keyframe_needed && !stream->references_lost,
          "a keyframe ends the keyframe request and restores references");
    r.config.use_fir = true;
    video_rtp_request_keyframe(&r, kOracleSsrc);
    now += 400'000;
    bytes = video_rtp_feedback(&r, now, {rtcp, sizeof(rtcp)});
    const u32 fir1 = find_rtcp({rtcp, bytes}, kRtcpPayloadFeedback, kRtcpFormatFir);
    const u8 first_sequence = fir1 ? rtcp[fir1 + 15] : 0xff;
    now += 400'000;
    bytes = video_rtp_feedback(&r, now, {rtcp, sizeof(rtcp)});
    const u32 fir2 = find_rtcp({rtcp, bytes}, kRtcpPayloadFeedback, kRtcpFormatFir);
    check(fir1 && fir2 && first_sequence == 0 && rtcp[fir2 + 15] == 1 &&
              byte_load_be32(rtcp + fir2 + 11) == kOracleSsrc,
          "FIR mode requests keyframes with an incrementing command sequence");
    u8 compound[64];
    video_rtp_receive_rtcp(&r, {compound, test_unhex(rtcp_sr_compound, compound)}, now);
    now += 1'000'000;
    bytes = video_rtp_feedback(&r, now, {rtcp, sizeof(rtcp)});
    check(bytes && byte_load_be32(rtcp + 24) == 0x456789ab && byte_load_be32(rtcp + 28) == 65536,
          "receiver reports carry LSR and DLSR from the sender report");
    video_rtp_release(&r);
}

void test_receiver_bounds(Check check) noexcept {
    Collector c{};
    auto& r = receiver_storage;
    static u8 payload[kVideoRtpPacketBytes + 8];
    if (!start_receiver(r, c)) {
        check(false, "video receiver initializes");
        return;
    }
    u64 now = 1'000'000;
    for (u32 i = 0; i != sizeof(payload); ++i)
        payload[i] = static_cast<u8>(i * 7 + 1);
    payload[0] = 0x7c; // FU-A, IDR, first_mb_in_slice 0.
    payload[1] = 0x85;
    payload[2] = 0x80;
    check(send(r, 10, 0, {payload, kVideoRtpPacketBytes + 1}, now) == VideoRtpPush::ignored &&
              video_rtp_find_stream(r, kOracleSsrc)->stats.oversized == 1,
          "video payloads larger than a slot are refused");
    MediaPacketLayout layout{};
    u32 bytes = make(96, 11, 0, true, kOracleSsrc, {payload, 4});
    media_packet_plaintext({wire, bytes}, &layout);
    check(video_rtp_receive(&r, {77, kOracleSsrc, false}, layout, {wire, bytes}, now) == VideoRtpPush::ignored,
          "video packets with an unnegotiated payload type are ignored");
    bytes = make(102, 1, 0, false, kOracleRtxSsrc, {payload, 1});
    media_packet_plaintext({wire, bytes}, &layout);
    check(video_rtp_receive(&r, {77, kOracleSsrc, true}, layout, {wire, bytes}, now) == VideoRtpPush::padding,
          "RTX probe padding without an original sequence is ignored");
    video_rtp_release(&r);

    // A 2.2 MB access unit exceeds the frame cap: dropped, keyframe requested.
    c = {};
    start_receiver(r, c);
    const u32 fragments = 1600;
    for (u32 i = 0; i != fragments; ++i) {
        payload[1] = static_cast<u8>(0x05 | (i == 0 ? 0x80 : 0) | (i + 1 == fragments ? 0x40 : 0));
        send(r, static_cast<u16>(60000 + i), 0, {payload, 1400}, now, i + 1 == fragments);
    }
    const u8 idr[]{0x65, 0x88, 1};
    now += 30'000;
    video_rtp_tick(&r, now);
    const auto* stream = video_rtp_find_stream(r, kOracleSsrc);
    check(c.count == 0 && stream && stream->stats.damaged_frames == 1 && stream->keyframe_needed &&
              stream->base == stream->highest + 1,
          "video access units over the 2 MiB cap are dropped without overrunning the frame buffer");
    send(r, static_cast<u16>(60000 + fragments), 3000, view(idr), now += 1000);
    check(c.count == 1 && c.sizes[0] == 7, "the stream recovers with the next intact keyframe");
    // An unterminated unit longer than the window is discarded from the front.
    payload[1] = 0x85;
    send(r, static_cast<u16>(62000), 6000, {payload, 1200}, now += 1000, false);
    payload[1] = 0x05;
    for (u32 i = 1; i != kVideoRtpPacketSlots + 100; ++i)
        send(r, static_cast<u16>(62000 + i), 6000, {payload, 1200}, now, false);
    check(c.count == 1 && stream->highest + 1 - stream->base <= kVideoRtpPacketSlots &&
              stream->stats.discarded_packets >= 100 && stream->keyframe_needed,
          "the reorder window stays bounded when a unit never ends");
    // A sequence jump wider than the NACK range is skipped at once.
    send(r, static_cast<u16>(62000 + kVideoRtpPacketSlots + 100 + 300), 9000, view(idr), now += 1000);
    u8 rtcp[1500];
    bytes = video_rtp_feedback(&r, now, {rtcp, sizeof(rtcp)});
    check(c.count == 2 && !stream->missing &&
              !find_rtcp({rtcp, bytes}, kRtcpTransportFeedback, kRtcpFormatGenericNack),
          "wide gaps are not NACKed and do not delay the next keyframe");
    // A sender restart (sustained old sequence numbers) resynchronizes.
    const u16 restart = static_cast<u16>(static_cast<u16>(stream->highest) - 5000);
    for (u32 i = 0; i != 130; ++i)
        send(r, static_cast<u16>(restart + i), 12000 + i * 3000, view(idr), now += 1000);
    video_rtp_tick(&r, now += 30'000);
    check(stream->stats.late >= 127 && static_cast<u16>(stream->highest) == restart + 129 && c.count >= 3,
          "sustained late packets restart the sequence space");
    // Zero-payload packets advance the sequence without a gap.
    const u16 next = static_cast<u16>(stream->highest + 1);
    check(send(r, next, 0, {payload, 0}, now += 1000, false) == VideoRtpPush::padding &&
              send(r, static_cast<u16>(next + 1), 900000, view(idr), now += 1000) == VideoRtpPush::stored &&
              !stream->missing,
          "padding packets occupy sequence numbers without creating gaps");
    video_rtp_release(&r);

    // Stream table capacity and idle eviction.
    c = {};
    start_receiver(r, c);
    bool accepted = true;
    for (u32 i = 0; i != kVideoRtpMaxStreams; ++i)
        accepted = accepted && send(r, 1, 0, view(idr), now, true, 0x5000 + i) == VideoRtpPush::stored;
    const bool refused = send(r, 1, 0, view(idr), now, true, 0x6000) == VideoRtpPush::ignored;
    now += 6'000'000;
    check(accepted && refused && send(r, 1, 0, view(idr), now, true, 0x6000) == VideoRtpPush::stored &&
              video_rtp_find_stream(r, 0x6000),
          "video stream capacity is bounded; idle streams are evicted for new SSRCs");
    video_rtp_remove_stream(&r, 0x6000);
    check(!video_rtp_find_stream(r, 0x6000), "video streams can be removed");
    video_rtp_release(&r);
}
} // namespace

void test_video_rtp_receive(Check check) noexcept {
    test_h264(check);
    test_h265(check);
    test_av1(check);
    test_rtcp(check);
    test_transport_feedback(check);
    run_scenarios(check, h264_scenarios, sizeof(h264_scenarios) / sizeof(h264_scenarios[0]), h264_packets,
                  h264_frames, 102);
    run_scenarios(check, h265_scenarios, sizeof(h265_scenarios) / sizeof(h265_scenarios[0]), h265_packets,
                  h265_frames, 104);
    run_scenarios(check, av1_scenarios, sizeof(av1_scenarios) / sizeof(av1_scenarios[0]), av1_packets, av1_frames,
                  106);
    test_receiver_feedback(check);
    test_receiver_bounds(check);
}
} // namespace maicord
