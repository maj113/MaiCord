#include "maicord/app/gateway/stream_control.hpp"
#include "maicord/app/voice/dave/frame_crypto.hpp"
#include "maicord/app/voice/dave/receive_sender.hpp"
#include "maicord/app/voice/go_live_send.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/video/rtp_av1.hpp"
#include "maicord/media/video/rtp_h264.hpp"
#include "maicord/media/video/rtp_h265.hpp"
#include "maicord/protocol/voice/dave_codec.hpp"
#include "maicord/protocol/voice/media_events.hpp"
#include "maicord/protocol/voice/video_signaling.hpp"
#include "maicord/sys/crt.hpp"
#include "../../support/hevc_stream_fixture.hpp"
#include "../../support/test_output.hpp"

// Go Live sending below the session: RTP packetization against the receive
// path's depacketizers, DAVE's sender-side codec handling against its
// receive side, the sender's RTX/PLI/REMB/transport-feedback handling and
// bandwidth estimation, and the gateway/voice writers the sender adds.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "../../data/av1_stream_vectors.hpp"

constexpr u32 kFrameBytes = 256 * 1024;
u8 g_bits[160 * 1024], g_stream[kFrameBytes], g_prepared[kFrameBytes + kFrameBytes / 4 + 4];
u8 g_sealed[kFrameBytes + kFrameBytes / 4 + 512], g_scratch[kFrameBytes + kFrameBytes / 4 + 4];
u8 g_received[kFrameBytes + 1024], g_opened[kFrameBytes + 1024], g_open_scratch[kFrameBytes + 1024];
u8 g_payload[kRtpVideoMaximumPayload];

RtpDepacketize push(RtpVideoPacketCodec codec, RtpVideoAssembly* assembly, ConstSlice<u8> payload) noexcept {
    return codec == RtpVideoPacketCodec::av1    ? rtp_av1_push(assembly, payload)
           : codec == RtpVideoPacketCodec::h265 ? rtp_h265_push(assembly, payload)
                                                : rtp_h264_push(assembly, payload);
}
struct Trip {
    u32 bytes, packets, largest;
    bool markers_ok, starts, keyframe, intact;
};
// Packetizes frame and depacketizes every payload as the receive path does.
Trip round_trip(RtpVideoPacketCodec codec, ConstSlice<u8> frame, bool keyframe,
                u32 max_payload = kRtpVideoMaximumPayload) noexcept {
    Trip trip{0, 0, 0, true, false, false, false};
    RtpVideoPacketizer packetizer;
    if (!rtp_packetizer_begin(&packetizer, codec, frame, keyframe, max_payload))
        return trip;
    RtpVideoAssembly assembly;
    rtp_video_assembly_begin(&assembly, {g_received, sizeof(g_received)});
    bool marker = false, ended = false;
    for (;;) {
        const u32 bytes = rtp_packetizer_next(&packetizer, {g_payload, sizeof(g_payload)}, &marker);
        if (!bytes)
            break;
        const ConstSlice<u8> payload{g_payload, bytes};
        if (!trip.packets)
            trip.starts = codec == RtpVideoPacketCodec::av1    ? rtp_av1_starts_temporal_unit(payload)
                          : codec == RtpVideoPacketCodec::h265 ? rtp_h265_starts_access_unit(payload)
                                                               : rtp_h264_starts_access_unit(payload);
        trip.markers_ok = trip.markers_ok && !ended;
        ended = marker;
        ++trip.packets;
        trip.largest = bytes > trip.largest ? bytes : trip.largest;
        if (push(codec, &assembly, payload) != RtpDepacketize::appended)
            return trip;
    }
    trip.markers_ok = trip.markers_ok && ended && !packetizer.failed;
    trip.bytes = rtp_video_assembly_finish(&assembly);
    trip.intact = !assembly.damaged;
    trip.keyframe = codec == RtpVideoPacketCodec::av1    ? rtp_av1_keyframe(assembly)
                    : codec == RtpVideoPacketCodec::h265 ? rtp_h265_keyframe(assembly)
                                                         : rtp_h264_keyframe(assembly);
    return trip;
}
bool received(ConstSlice<u8> expected, const Trip& trip) noexcept {
    return trip.intact && trip.bytes == expected.count && bytes_equal(g_received, expected.data, expected.count);
}

ConstSlice<u8> h264_idr() noexcept {
    using namespace h264_fixture;
    Bits bits{g_bits, sizeof(g_bits), 0, true};
    Output out{g_stream, sizeof(g_stream), 0, true};
    const Sps sps{0, 66, 20, 12, 2, 1, false, 0, 0, 0};
    write_idr(&out, &bits, sps, {0, 0}, 3, 0, 3); // Three slices: 4-byte, then 3-byte start codes.
    return {g_stream, out.ok ? out.count : 0};
}
ConstSlice<u8> h264_p() noexcept {
    using namespace h264_fixture;
    Bits bits{g_bits, sizeof(g_bits), 0, true};
    Output out{g_stream, sizeof(g_stream), 0, true};
    const Sps sps{0, 66, 20, 12, 2, 1, false, 0, 0, 0};
    write_p(&out, &bits, sps, {0, 0}, 1, 2);
    return {g_stream, out.ok ? out.count : 0};
}
ConstSlice<u8> h265_idr() noexcept {
    using namespace hevc_fixture;
    Bits bits{g_bits, sizeof(g_bits), 0, true};
    Output out{g_stream, sizeof(g_stream), 0, true};
    write_idr(&out, &bits, {0, 256, 64, 0, 4, 8, 2}, {0, 0}, 5);
    return {g_stream, out.ok ? out.count : 0};
}
ConstSlice<u8> av1_unit(u32 index) noexcept {
    return {g_stream, test_unhex(av1_fixture_units[index], g_stream)};
}
DavePreparedFrame prepare(DaveVideoCodec codec, ConstSlice<u8> frame) noexcept {
    static DaveClearRange ranges[kDaveMaximumClearRanges];
    return dave_frame_prepare(codec, frame, {g_prepared, sizeof(g_prepared)}, {ranges, kDaveMaximumClearRanges});
}

void packetization(Check check) noexcept {
    // H.264: SPS/PPS/AUD aggregate in a STAP-A, the I_PCM slices fragment in FU-A.
    auto frame = h264_idr();
    auto plan = prepare(DaveVideoCodec::h264, frame);
    Trip trip = round_trip(RtpVideoPacketCodec::h264, frame, true);
    check(frame.count > 50'000 && plan.bytes && received({g_prepared, plan.bytes}, trip) && trip.markers_ok &&
              trip.starts && trip.keyframe && trip.packets > frame.count / kRtpVideoMaximumPayload &&
              trip.largest <= kRtpVideoMaximumPayload,
          "Go Live RTP: an H.264 IDR (3- and 4-byte start codes) packetizes into STAP-A/FU-A and depacketizes "
          "back to the same NAL units, keyframe and access-unit start detected");
    const u8 first_type = g_payload[0] & 31;
    trip = round_trip(RtpVideoPacketCodec::h264, {g_prepared, plan.bytes}, true);
    check(received({g_prepared, plan.bytes}, trip) && first_type == 28,
          "Go Live RTP: the 4-byte start-code form round-trips unchanged");
    frame = h264_p();
    plan = prepare(DaveVideoCodec::h264, frame);
    trip = round_trip(RtpVideoPacketCodec::h264, frame, false);
    check(plan.bytes && received({g_prepared, plan.bytes}, trip) && trip.packets == 1 && !trip.keyframe &&
              trip.markers_ok,
          "Go Live RTP: a small P picture is one single-NAL packet with the marker");
    // H.265: VPS/SPS/PPS in an AP, the IDR in FUs.
    frame = h265_idr();
    plan = prepare(DaveVideoCodec::h265, frame);
    trip = round_trip(RtpVideoPacketCodec::h265, frame, true, 300);
    check(frame.count > 1000 && plan.bytes && received({g_prepared, plan.bytes}, trip) && trip.keyframe &&
              trip.starts && trip.markers_ok && trip.largest <= 300,
          "Go Live RTP: an H.265 IRAP access unit round-trips through AP and FU packets");
    // AV1: the depacketizer's frame is DAVE's prepared form (sizes on all but the last OBU).
    for (u32 i = 0; i != kAv1FixtureFrames; ++i) {
        frame = av1_unit(i);
        plan = prepare(DaveVideoCodec::av1, frame);
        trip = round_trip(RtpVideoPacketCodec::av1, frame, i == 0, 200);
        if (!plan.bytes || !received({g_prepared, plan.bytes}, trip) || !trip.markers_ok || !trip.starts ||
            trip.keyframe != (i == 0)) {
            check(false, "Go Live RTP: AV1 temporal units round-trip as DAVE's prepared form");
            return;
        }
    }
    check(true, "Go Live RTP: AV1 temporal units round-trip as DAVE's prepared form (OBU elements, Z/Y, N)");
    const u8 broken[]{0x00, 0x00, 0x01, 0x80, 0x11};
    RtpVideoPacketizer packetizer;
    check(!rtp_packetizer_begin(&packetizer, RtpVideoPacketCodec::h264, {broken, 5}, false, 1100),
          "Go Live RTP: a NAL unit with the forbidden bit is refused before anything is sent");
    // The shared Annex B scanner against a plain byte-by-byte search, on inputs of four distinct bytes.
    u32 state = 12345;
    const auto next = [&state]() noexcept { return (state = state * 1664525u + 1013904223u) >> 16; };
    bool scans = true;
    for (u32 round = 0; scans && round != 5000; ++round) {
        u8 bytes[24];
        const u32 count = next() % 24;
        for (u32 i = 0; i != count; ++i)
            bytes[i] = static_cast<u8>(next() % 4);
        const u32 from = next() % (count + 1);
        u32 expected_start = count, expected_next = count;
        for (u32 i = from; i + 2 < count; ++i)
            if (!bytes[i] && !bytes[i + 1] && bytes[i + 2] == 1) {
                expected_start = i > from && !bytes[i - 1] ? i - 1 : i;
                expected_next = i + 3;
                break;
            }
        u32 start = 0;
        scans = annexb_next_unit({bytes, count}, from, &start) == expected_next && start == expected_start;
    }
    check(scans, "Go Live RTP: the Annex B scanner finds each start code and its fourth zero as a plain search does");
    static constexpr u32 kValues[]{0, 1, 127, 128, 16383, 16384, 0x0fffffff, 0xffffffff};
    bool leb = true;
    for (const u32 value : kValues) {
        u8 bytes[5];
        const u32 length = leb128_write(bytes, value);
        u64 decoded = 0;
        for (u32 i = 0; i != length; ++i)
            decoded |= u64{bytes[i] & 0x7fu} << (7 * i);
        leb = leb && length == leb128_bytes(value) && decoded == value && !(bytes[length - 1] & 0x80);
    }
    check(leb, "Go Live RTP: LEB128 sizes are written in their shortest form");
}

bool start_code_in(ConstSlice<u8> bytes) noexcept {
    for (u32 i = 0; i + 2 < bytes.count; ++i)
        if (!bytes[i] && !bytes[i + 1] && bytes[i + 2] == 1)
            return true;
    return false;
}
// Seals a prepared frame as dave_session_encrypt_video does (retrying while a
// start code shows), packetizes it and opens what the receive path rebuilds.
bool dave_trip(Check check, DaveVideoCodec codec, ConstSlice<u8> frame, bool keyframe, const char* label,
               DaveFrameSealer& sealer, DaveReceiveSender& opener, KdfSha256Workspace& kdf) noexcept {
    static DaveClearRange ranges[kDaveMaximumClearRanges];
    const auto plan = dave_frame_prepare(codec, frame, {g_prepared, sizeof(g_prepared)}, {ranges, kDaveMaximumClearRanges});
    DaveSealResult sealed{};
    for (u32 attempt = 0; attempt != 10; ++attempt) {
        sealed = dave_frame_seal_ranges(sealer, kdf, {g_prepared, plan.bytes}, {ranges, plan.range_count},
                                        {g_scratch, sizeof(g_scratch)}, {g_sealed, sizeof(g_sealed)});
        if (!sealed.ok() || codec == DaveVideoCodec::av1 ||
            dave_frame_start_code_free({g_sealed, sealed.bytes}, {ranges, plan.range_count}))
            break;
    }
    const Trip trip = round_trip(static_cast<RtpVideoPacketCodec>(codec), {g_sealed, sealed.bytes}, keyframe);
    const auto opened = dave_receive_sender_open(opener, {77, 4242}, kdf, {g_received, trip.bytes}, 1,
                                                 {g_open_scratch, sizeof(g_open_scratch)}, {g_opened, sizeof(g_opened)});
    // Clear ranges (NAL headers, start codes, OBU headers) stay readable; nothing else does.
    bool clear_kept = true;
    for (u32 i = 0; i != plan.range_count; ++i)
        clear_kept = clear_kept && bytes_equal(g_sealed + ranges[i].offset, g_prepared + ranges[i].offset,
                                               ranges[i].bytes);
    const bool ok = plan.bytes && sealed.ok() && trip.intact && trip.bytes == sealed.bytes &&
                    bytes_equal(g_received, g_sealed, sealed.bytes) && opened.ok() && opened.bytes == plan.bytes &&
                    bytes_equal(g_opened, g_prepared, plan.bytes) && clear_kept && plan.range_count &&
                    !bytes_equal(g_sealed, g_prepared, plan.bytes) &&
                    (codec == DaveVideoCodec::av1 ||
                     !start_code_in({g_received + ranges[plan.range_count - 1].offset +
                                         ranges[plan.range_count - 1].bytes,
                                     trip.bytes - ranges[plan.range_count - 1].offset -
                                         ranges[plan.range_count - 1].bytes}));
    check(ok, label);
    return ok;
}

void dave_video(Check check) noexcept {
    KdfSha256Workspace kdf{};
    DaveFrameSealer sealer{};
    DaveReceiveSender opener{};
    u8 seed[16];
    for (u32 i = 0; i != 16; ++i)
        seed[i] = static_cast<u8>(0x31 + i * 7);
    dave_frame_sealer_initialize(&sealer, seed);
    const bool ready = kdf_sha256_initialize(&kdf).ok() &&
                       dave_receive_sender_initialize(&opener, {77, 4242}, 0) == DaveSenderError::none &&
                       dave_receive_sender_transition(opener, {77, 4242}, 1, {seed, 16}, 0, 1000).ok();
    check(ready, "Go Live DAVE: the sender ratchet and the receiving member share a seed");
    if (ready) {
        dave_trip(check, DaveVideoCodec::h264, h264_idr(), true,
                  "Go Live DAVE: an H.264 IDR sealed with clear NAL and slice headers survives RTP and opens on "
                  "the receive side",
                  sealer, opener, kdf);
        dave_trip(check, DaveVideoCodec::h264, h264_p(), false, "Go Live DAVE: an H.264 P picture opens", sealer,
                  opener, kdf);
        dave_trip(check, DaveVideoCodec::h265, h265_idr(), true,
                  "Go Live DAVE: an H.265 IRAP access unit (clear VPS/SPS/PPS, NAL headers) opens", sealer, opener,
                  kdf);
        dave_trip(check, DaveVideoCodec::av1, av1_unit(0), true,
                  "Go Live DAVE: an AV1 key temporal unit (clear OBU headers and sizes) opens", sealer, opener, kdf);
        dave_trip(check, DaveVideoCodec::av1, av1_unit(1), false, "Go Live DAVE: an AV1 inter temporal unit opens",
                  sealer, opener, kdf);
    }
    dave_receive_sender_release(&opener);
    dave_frame_sealer_release(&sealer);
    kdf_sha256_release(&kdf);
}

// The sender against a recording transport.
constexpr u32 kLinkPackets = 192;
struct Link {
    u8 packets[kLinkPackets][kGoLiveDatagramBytes];
    u16 bytes[kLinkPackets];
    u32 count;
    bool busy;
};
VoiceUdpMediaSend capture(void* context, ConstSlice<u8> packet) noexcept {
    auto& link = *static_cast<Link*>(context);
    if (link.busy || link.count == kLinkPackets)
        return VoiceUdpMediaSend::busy;
    copy_bytes(link.packets[link.count], packet.data, packet.count);
    link.bytes[link.count++] = static_cast<u16>(packet.count);
    return VoiceUdpMediaSend::accepted;
}
u32 rtcp_feedback(u8* out, u8 type, u8 format, u32 media, ConstSlice<u8> fci) noexcept {
    const u32 bytes = 12 + fci.count;
    out[0] = static_cast<u8>(0x80 | format);
    out[1] = type;
    out[2] = 0;
    out[3] = static_cast<u8>(bytes / 4 - 1);
    byte_store_be32(out + 4, 99);
    byte_store_be32(out + 8, media);
    copy_bytes(out + 12, fci.data, fci.count);
    return bytes;
}

void sender(Check check) noexcept {
    static GoLiveSender s;
    static Link link;
    reset_value(&s);
    reset_value(&link);
    const GoLiveSendLink to{&link, capture};
    const GoLiveSendStreams streams{100, 200, 201, 103, 104, RtpVideoPacketCodec::h264};
    const StreamBitrate bounds{500'000, 3'500'000, 600'000};
    check(go_live_sender_start(&s, streams, bounds, 1'000'000), "Go Live sender: starts");
    // One keyframe-sized access unit: paced, not all at once.
    const auto frame = h264_idr();
    auto buffer = go_live_sender_frame_buffer(&s);
    copy_bytes(buffer.data, frame.data, frame.count);
    check(go_live_sender_frame(&s, frame.count, 9000, true) && !go_live_sender_frame_buffer(&s).data,
          "Go Live sender: takes one access unit at a time");
    u64 now = 1'000'000;
    u64 next = go_live_sender_pump(&s, to, now);
    const u32 burst = link.count;
    check(burst > 0 && burst < 20 && next > now, "Go Live sender: the pacer sends a burst, then waits for budget");
    for (u32 i = 0; i != 2000 && (s.frame_active || s.datagram_bytes) && link.count != kLinkPackets; ++i) {
        now = next > now ? next : now + 1000;
        next = go_live_sender_pump(&s, to, now);
    }
    const u8* first = link.packets[0];
    const u8* last = link.packets[link.count - 1];
    const bool headers = first[0] == 0x90 && (first[1] & 0x7f) == 103 && byte_load_be32(first + 8) == 200 &&
                         byte_load_be32(first + 4) == 9000 && first[12] == 0xbe && first[13] == 0xde &&
                         (first[16] >> 4) == kGoLiveTransportSequenceExtensionId &&
                         (first[19] >> 4) == kGoLivePlayoutDelayExtensionId;
    check(headers && (last[1] & 0x80) && !(first[1] & 0x80),
          "Go Live sender: video RTP carries the payload type, SSRC, timestamp, the transport sequence and "
          "playout-delay extensions, and the marker on the frame's last packet");
    const u16 lost = byte_load_be16(link.packets[3] + 2);
    // NACK of one packet: retransmitted on the RTX stream with its original sequence.
    u8 rtcp[64];
    const u8 fci[4]{static_cast<u8>(lost >> 8), static_cast<u8>(lost), 0, 0};
    u32 bytes = rtcp_feedback(rtcp, kRtcpTransportFeedback, kRtcpFormatGenericNack, 200, {fci, 4});
    go_live_sender_rtcp(&s, {rtcp, bytes}, now);
    link.count = 0;
    go_live_sender_pump(&s, to, now + 100'000);
    const u8* rtx = link.packets[0];
    check(link.count >= 1 && (rtx[1] & 0x7f) == 104 && byte_load_be32(rtx + 8) == 201 &&
              byte_load_be16(rtx + 24) == lost,
          "Go Live sender: a generic NACK retransmits the packet as RTX (payload type 104, SSRC 201, original "
          "sequence first)");
    // PLI and FIR ask the encoder for a keyframe.
    bytes = rtcp_feedback(rtcp, kRtcpPayloadFeedback, kRtcpFormatPli, 200, {});
    go_live_sender_rtcp(&s, {rtcp, bytes}, now);
    const bool pli = go_live_sender_take_keyframe(&s) && !go_live_sender_take_keyframe(&s);
    u8 fir[8]{};
    byte_store_be32(fir, 200);
    bytes = rtcp_feedback(rtcp, kRtcpPayloadFeedback, kRtcpFormatFir, 0, {fir, 8});
    go_live_sender_rtcp(&s, {rtcp, bytes}, now);
    check(pli && go_live_sender_take_keyframe(&s), "Go Live sender: PLI and FIR request a keyframe");
    // REMB caps the estimate (18-bit mantissa 150000, exponent 3: 1.2 Mb/s).
    u8 remb[12]{'R', 'E', 'M', 'B', 1, 0, 0, 0, 0, 0, 0, 200};
    const u32 field = (3u << 18) | 150'000u;
    remb[5] = static_cast<u8>(field >> 16);
    remb[6] = static_cast<u8>(field >> 8);
    remb[7] = static_cast<u8>(field);
    s.bandwidth.target = 3'000'000;
    bytes = rtcp_feedback(rtcp, kRtcpPayloadFeedback, kRtcpFormatApplication, 0, {remb, 12});
    go_live_sender_rtcp(&s, {rtcp, bytes}, now);
    check(s.bandwidth.remb == 1'200'000 && s.bandwidth.target == 1'200'000,
          "Go Live sender: REMB caps the send target");
    // Audio: Opus with the SOUNDSHARE speaking extension.
    const u8 opus[]{0xfc, 1, 2, 3};
    link.count = 0;
    go_live_sender_audio(&s, {opus, 4}, 960);
    go_live_sender_pump(&s, to, now + 200'000);
    const u8* audio = link.packets[0];
    check(link.count >= 1 && (audio[1] & 0x7f) == 120 && byte_load_be32(audio + 8) == 100 &&
              (audio[16] >> 4) == kGoLiveSpeakingExtensionId && audio[17] == 4 && bytes_equal(audio + 20, opus, 4),
          "Go Live sender: stream audio is Opus on the audio SSRC with the speaking extension set to SOUNDSHARE");
    // A sender report once a second.
    link.count = 0;
    go_live_sender_pump(&s, to, now + 2'000'000);
    bool report = false;
    for (u32 i = 0; i != link.count; ++i)
        report = report || (link.packets[i][1] == kRtcpSenderReport && byte_load_be32(link.packets[i] + 4) == 200);
    check(report, "Go Live sender: sends RTCP sender reports for the video SSRC");
    go_live_sender_stop(&s);
}

void transport_feedback(Check check) noexcept {
    // Our feedback writer's packet read back by the sender's reader.
    u64 arrivals[6]{1'000'000, 1'000'250, 0, 1'002'000, 1'002'250, 1'010'000};
    u8 packet[128];
    const auto written = rtcp_write_transport_feedback({packet, sizeof(packet)}, 0, 9, 200, 40000, 3, {arrivals, 6});
    struct Seen {
        RtcpTransportArrival arrivals[16];
        u32 count;
    } seen{};
    RtcpFeedbackVisitor visitor{};
    visitor.context = &seen;
    visitor.transport = +[](void* context, ConstSlice<RtcpTransportArrival> list) noexcept {
        auto& s = *static_cast<Seen*>(context);
        for (u32 i = 0; i != list.count && s.count != 16; ++i)
            s.arrivals[s.count++] = list[i];
    };
    rtcp_read_feedback({packet, written.end}, visitor);
    bool ok = seen.count == 6;
    for (u32 i = 0; ok && i != 6; ++i)
        ok = seen.arrivals[i].sequence == static_cast<u16>(40000 + i) && seen.arrivals[i].received == (arrivals[i] != 0) &&
             (!arrivals[i] || seen.arrivals[i].arrival_us - seen.arrivals[0].arrival_us ==
                                  static_cast<i64>(arrivals[i] - arrivals[0]));
    check(written.packets == 6 && ok,
          "Go Live RTCP: transport-wide feedback reads back every packet's arrival (and loss) at 250 us steps");
    // Sender report fields.
    RtcpSenderInfo info[1];
    const u32 end = rtcp_write_sender_report({packet, sizeof(packet)}, 0, {200, 3'900'000'000u, 1u << 31, 90'000, 7, 7000});
    check(end == kRtcpSenderReportBytes && rtcp_read_sender_reports({packet, end}, {info, 1}) == 1 &&
              info[0].ssrc == 200 && info[0].ntp_seconds == 3'900'000'000u && info[0].ntp_fraction == 1u << 31,
          "Go Live RTCP: sender reports carry the SSRC and NTP time");
}

void bandwidth(Check check) noexcept {
    GoLiveBandwidth b;
    const StreamBitrate bounds{500'000, 9'000'000, 2'000'000};
    // Steady delivery: the estimate grows.
    go_live_bandwidth_reset(&b, bounds, 0);
    u64 send = 0;
    i64 arrival = 100'000;
    for (u32 batch = 1; batch != 20; ++batch) {
        for (u32 i = 0; i != 20; ++i, send += 2'000, arrival += 2'000)
            go_live_bandwidth_packet(&b, send, arrival, 1200, true);
        go_live_bandwidth_update(&b, batch * 100'000);
    }
    const u32 grown = b.target;
    // Growing queueing delay (overuse): 1200 bytes delivered every 3 ms (3.2 Mb/s) while sent every 2 ms; the
    // target backs off to 85 % of what got through.
    go_live_bandwidth_reset(&b, bounds, 0);
    send = 0;
    arrival = 100'000;
    for (u32 batch = 1; batch != 20; ++batch) {
        for (u32 i = 0; i != 20; ++i, send += 2'000, arrival += 3'000)
            go_live_bandwidth_packet(&b, send, arrival, 1200, true);
        go_live_bandwidth_update(&b, batch * 100'000);
    }
    const u32 congested = b.target;
    // Heavy loss: halves with the loss fraction.
    go_live_bandwidth_reset(&b, bounds, 0);
    for (u32 i = 0; i != 10; ++i)
        go_live_bandwidth_packet(&b, i * 1000, 100'000 + i * 1000, 1200, i < 6);
    go_live_bandwidth_update(&b, 50'000);
    check(grown > bounds.start && congested <= 3'200'000u * 85 / 100 && congested > 2'000'000 &&
              b.target < bounds.start && b.target >= bounds.minimum,
          "Go Live bandwidth: steady delivery raises the target; growing delay backs off to 85 % of the delivered "
          "rate; heavy loss cuts it, never below the minimum");
}

void signaling(Check check) noexcept {
    u8 out[512];
    const auto key = gateway_stream_key_for(1001, 2002, 3003);
    auto written = gateway_write_stream_create(key, {out, sizeof(out)});
    check(written.ok() && bytes_equal(out,
                                      R"({"op":18,"d":{"type":"guild","guild_id":"1001","channel_id":"2002","preferred_region":null}})",
                                      written.bytes),
          "Go Live gateway: STREAM_CREATE (op 18) for a guild channel");
    written = gateway_write_stream_create(gateway_stream_key_for(0, 2002, 3003), {out, sizeof(out)});
    check(written.ok() &&
              bytes_equal(out, R"({"op":18,"d":{"type":"call","guild_id":null,"channel_id":"2002","preferred_region":null}})",
                          written.bytes),
          "Go Live gateway: STREAM_CREATE for a private call");
    written = gateway_write_stream_set_paused(key, true, {out, sizeof(out)});
    check(written.ok() && bytes_equal(out, R"({"op":22,"d":{"stream_key":"guild:1001:2002:3003","paused":true}})",
                                      written.bytes),
          "Go Live gateway: STREAM_SET_PAUSED (op 22)");
    const VoiceVideoState video{11, {12, 13, 9'000'000, 0, 0, 60, 100, true}};
    written = voice_gateway_write_video(video, {out, sizeof(out)});
    check(written.ok() &&
              bytes_equal(out,
                          R"({"op":12,"d":{"audio_ssrc":11,"video_ssrc":12,"rtx_ssrc":13,"streams":[{"type":"video","rid":"100","ssrc":12,"active":true,"quality":100,"rtx_ssrc":13,"max_bitrate":9000000,"max_framerate":60,"max_resolution":{"type":"source","width":0,"height":0}}]}})",
                          written.bytes),
          "Go Live voice: Video (op 12) announces the stream, its RTX SSRC and limits");
    written = voice_gateway_write_video({11, {}}, {out, sizeof(out)});
    check(written.ok() && bytes_equal(out, R"({"op":12,"d":{"audio_ssrc":11,"video_ssrc":0,"rtx_ssrc":0,"streams":[]}})",
                                      written.bytes),
          "Go Live voice: op 12 with no stream clears video");
    VoiceCodecOffer offers[8];
    const u32 encodable = voice_video_codec_bit(VoiceVideoCodec::h264) | voice_video_codec_bit(VoiceVideoCodec::h265);
    const u32 count = voice_video_offers(voice_video_codec_bit(VoiceVideoCodec::av1), encodable, {offers, 8});
    check(count == 3 && offers[0].codec == VoiceVideoCodec::av1 && !offers[0].encode && offers[0].decode &&
              offers[1].codec == VoiceVideoCodec::h265 && offers[1].encode &&
              offers[2].codec == VoiceVideoCodec::h264 && offers[2].encode,
          "Go Live voice: Select Protocol offers encoded and decoded codecs in AV1, H265, H264 order");
    // Ready: the server's assigned send stream.
    const char ready[] =
        R"({"ssrc":5,"ip":"127.0.0.1","port":1,"modes":["aead_aes256_gcm_rtpsize"],"streams":[{"type":"video","ssrc":7,"rtx_ssrc":8,"rid":"50","quality":50,"active":false},{"type":"video","ssrc":9,"rid":"100","quality":100,"active":false}]})";
    u32 scratch[512];
    VoiceReady decoded{};
    const bool parsed =
        voice_decode_ready({{ready, sizeof(ready) - 1}, 0, JsonValueKind::object}, {scratch, 512}, 16, &decoded).ok();
    check(parsed && decoded.video_ssrc == 9 && decoded.video_rtx_ssrc == 10,
          "Go Live voice: Ready's full-quality stream gives the video SSRC (RTX SSRC + 1 when omitted)");
}

void own_stream(Check check) noexcept {
    static GatewayStreamControl control;
    reset_value(&control);
    const char session[] = "session-1";
    gateway_stream_bind(&control, 3003, {session, sizeof(session) - 1});
    const auto key = gateway_stream_key_for(1001, 2002, 3003);
    GatewayStreamCommand command{};
    u8 out[256];
    const bool created = gateway_stream_create_own(&control, key, 0) && gateway_stream_next(control, &command) &&
                         command.kind == GatewayStreamCommandKind::create &&
                         gateway_stream_write(command, {out, sizeof(out)}).ok() &&
                         gateway_stream_submitted(&control, command);
    check(created && !gateway_stream_create_own(&control, gateway_stream_key_for(1001, 2002, 4004), 0),
          "Go Live control: our own stream starts with STREAM_CREATE; another user's key is refused");
    GatewayStreamCreate event{};
    event.key = key;
    event.rtc_server_id = 5005;
    event.viewer_count = 2;
    event.viewer_ids[0] = 7;
    event.viewer_ids[1] = 8;
    gateway_stream_create(&control, event, 0);
    check(control.phase == GatewayStreamPhase::awaiting_server && control.viewer_count == 2 &&
              control.viewer_ids[1] == 8 && control.setup.stream,
          "Go Live control: STREAM_CREATE keeps the viewer IDs and opens the stream server setup");
    const bool paused = gateway_stream_set_paused(&control, true) && gateway_stream_set_paused(&control, false) &&
                        gateway_stream_next(control, &command) && command.kind == GatewayStreamCommandKind::resume &&
                        control.queue_count == 1;
    gateway_stream_submitted(&control, command);
    gateway_stream_stop(&control, GatewayStreamEnd::stopped);
    check(paused && gateway_stream_next(control, &command) && command.kind == GatewayStreamCommandKind::remove,
          "Go Live control: a pause then resume not yet sent collapses to the latest; stop sends STREAM_DELETE");
    gateway_stream_release(&control);
}
} // namespace

void test_go_live_send(Check check) noexcept {
    packetization(check);
    dave_video(check);
    sender(check);
    transport_feedback(check);
    bandwidth(check);
    signaling(check);
    own_stream(check);
}
} // namespace maicord
