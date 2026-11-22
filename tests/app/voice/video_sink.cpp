#include "maicord/app/voice/receive.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "../../support/voice_log_sink.hpp"

// Call-video integration: video SSRC routing from voice receive into the RTP
// core, per-frame DAVE, the decoder boundary, keyframe requests, op 15 wants,
// the "video available" users list and stream/sink lifecycle.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
constexpr u64 kUser = 2, kSelf = 9;
constexpr u32 kAudio = 10, kVideo = 20, kRtx = 21;
struct Decoder {
    u8 frame[512];
    u32 bytes, frames, ssrc, timestamp, ended, ended_ssrc, stalls, stalled_ssrc;
    VoiceVideoCodec codec;
    bool keyframe, intact;
    bool decoded; // What the decoded callback answers.
};
bool push(void* context, u32 ssrc, VoiceVideoCodec codec, ConstSlice<u8> frame, u32 timestamp, bool keyframe,
          bool intact) noexcept {
    auto& decoder = *static_cast<Decoder*>(context);
    ++decoder.frames;
    decoder.codec = codec;
    decoder.bytes = frame.count < sizeof(decoder.frame) ? frame.count : 0;
    copy_bytes(decoder.frame, frame.data, decoder.bytes);
    decoder.ssrc = ssrc;
    decoder.timestamp = timestamp;
    decoder.keyframe = keyframe;
    decoder.intact = intact;
    return true;
}
void end(void* context, u32 ssrc) noexcept {
    auto& decoder = *static_cast<Decoder*>(context);
    ++decoder.ended;
    decoder.ended_ssrc = ssrc;
}
bool decoded(void* context, u32) noexcept {
    return static_cast<Decoder*>(context)->decoded;
}
void stalled(void* context, u32 ssrc) noexcept {
    auto& decoder = *static_cast<Decoder*>(context);
    ++decoder.stalls;
    decoder.stalled_ssrc = ssrc;
}
struct Dave {
    u64 user_id;
    u32 ssrc, calls, replays;
    bool reject;
    bool audio_seen, video_opened; // Counters follow arrival order: audio opened first makes a video open a replay.
};
DaveMediaResult open(void* context, u64 user_id, u32 ssrc, ConstSlice<u8> frame, Slice<u8>,
                     Slice<u8> output) noexcept {
    auto& dave = *static_cast<Dave*>(context);
    ++dave.calls;
    dave.user_id = user_id;
    dave.ssrc = ssrc;
    DaveMediaResult result{};
    if (dave.audio_seen && !dave.video_opened) {
        ++dave.replays;
        result.error = DaveMediaError::rejected;
        return result;
    }
    if (dave.reject || output.count < frame.count) {
        result.error = DaveMediaError::rejected;
        return result;
    }
    dave.video_opened = true;
    copy_bytes(output.data, frame.data, frame.count);
    result.bytes = frame.count;
    return result;
}
DaveMediaResult audio_unused(void*, const VoiceReceivedPacket&, ConstSlice<u8>, Slice<u8>, Slice<u8>) noexcept {
    DaveMediaResult result{};
    result.error = DaveMediaError::not_ready;
    return result;
}
u32 wakes;
void wake(void*) noexcept {
    ++wakes;
}
u32 rtp(u8* out, u8 type, u32 ssrc, u16 sequence, u32 timestamp, bool marker, ConstSlice<u8> payload) noexcept {
    out[0] = 0x80;
    out[1] = static_cast<u8>(type | (marker ? 0x80 : 0));
    out[2] = static_cast<u8>(sequence >> 8);
    out[3] = static_cast<u8>(sequence);
    byte_store_be32(out + 4, timestamp);
    byte_store_be32(out + 8, ssrc);
    copy_bytes(out + 12, payload.data, payload.count);
    return 12 + payload.count;
}
VoiceReceivedPacket received(const VoiceConnection& connection, ConstSlice<u8> plain) noexcept {
    VoiceReceivedPacket packet{};
    if (!media_packet_plaintext(plain, &packet.transport.layout))
        packet.transport.error = VoicePacketError::malformed_packet;
    packet.transport.bytes = plain.count;
    packet.source = voice_participants_source(connection.participants, packet.transport.layout.ssrc);
    packet.member = voice_participants_user(connection.participants, packet.source.user_id);
    return packet;
}
bool announce(VoiceConnection& connection, u8 opcode, Utf8View json) noexcept {
    static u32 scratch[1024];
    return voice_participants_event(&connection.participants, 1, opcode, {json, 0, JsonValueKind::object},
                                    {scratch, 1024}, 16)
        .ok();
}
// SPS, PPS and an IDR slice (first_mb_in_slice 0) as single NAL packets.
VoiceReceiveResult send_keyframe(VoiceReceive& receive, const VoiceConnection& connection, u16 sequence,
                                 u32 timestamp, u64 now_us, u32 ssrc = kVideo, u8 type = 101) noexcept {
    const u8 sps[]{0x67, 0x42, 0xc0, 0x1f, 0xda}, pps[]{0x68, 0xce, 0x3c, 0x80}, idr[]{0x65, 0x88, 0x84, 0x21, 0xa0};
    const ConstSlice<u8> nals[]{{sps, 5}, {pps, 4}, {idr, 5}};
    u8 plain[64];
    VoiceReceiveResult result = VoiceReceiveResult::video;
    for (u32 i = 0; i != 3; ++i) {
        const u32 bytes = rtp(plain, type, ssrc, static_cast<u16>(sequence + i), timestamp, i == 2, nals[i]);
        const auto value = voice_receive_packet(&receive, received(connection, {plain, bytes}), {plain, bytes},
                                                audio_unused, nullptr, now_us);
        if (value != VoiceReceiveResult::video)
            result = value;
    }
    return result;
}
constexpr u8 kAnnexB[]{0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80,
                       0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21, 0xa0};
// Session Description after a Select Protocol that announced these codecs in this order (payload types 101, 103, 105).
void describe(VoiceConnection& connection, VoiceVideoCodec first, VoiceVideoCodec second = VoiceVideoCodec::none,
              VoiceVideoCodec third = VoiceVideoCodec::none) noexcept {
    const VoiceVideoCodec order[]{first, second, third};
    auto& media = connection.media;
    media.has_description = true;
    media.video_offer = 0;
    media.video_codec_count = 0;
    for (const auto codec : order)
        if (codec != VoiceVideoCodec::none) {
            media.video_offer |= voice_video_codec_bit(codec);
            media.video_codecs[media.video_codec_count++] = static_cast<u8>(codec);
        }
}

struct Rig {
    VoiceConnection connection;
    VoiceReceive receive;
    Decoder decoder;
    Dave dave;
};
// The one fixture, reset for the next test.
Rig& fresh_rig() noexcept {
    static Rig rig;
    rig.connection = {};
    clear_bytes(&rig.receive, sizeof(rig.receive));
    rig.decoder = {};
    rig.dave = {};
    return rig;
}

void routing(Check check) noexcept {
    auto& [connection, receive, decoder, dave] = fresh_rig();
    wakes = 0;
    check(announce(connection, 11, utf8_literal(R"({"user_ids":["2","3"]})")) &&
              announce(connection, 12, utf8_literal(R"({"user_id":"2","audio_ssrc":10,"video_ssrc":20,"streams":[
                  {"type":"video","rid":"100","ssrc":20,"rtx_ssrc":21,"active":true,"quality":100},
                  {"type":"video","rid":"50","ssrc":24,"rtx_ssrc":25,"active":true,"quality":50}]})")) &&
              announce(connection, 12, utf8_literal(R"({"user_id":"3","audio_ssrc":30,"video_ssrc":0,"streams":[
                  {"type":"screen","rid":"100","ssrc":40,"rtx_ssrc":41,"active":false,"quality":100}]})")) &&
              announce(connection, 12, utf8_literal(R"({"user_id":"9","audio_ssrc":50,"video_ssrc":60,"streams":[
                  {"type":"video","rid":"100","ssrc":60,"rtx_ssrc":61,"active":true,"quality":100}]})")),
          "Video sink fixture: participants announce camera, simulcast and inactive screen streams");
    receive.video.decrypt = open;
    receive.video.decrypt_context = &dave;
    receive.video.frames = {&decoder, push, end};
    receive.video.wake = wake;

    // Before Session Description negotiates a codec nothing is allocated.
    check(send_keyframe(receive, connection, 100, 9000, 1'000'000) == VoiceReceiveResult::ignored &&
              !receive.video.rtp_ready && !receive.video.buffer && !dave.calls,
          "Video sink: video RTP before a negotiated codec is ignored without allocating buffers");
    describe(connection, VoiceVideoCodec::h264);
    connection.media.ssrc = 7;
    voice_video_sink_drive(&receive.video, &connection, false, 1000, 1'000'000);
    check(send_keyframe(receive, connection, 100, 9000, 1'000'000) == VoiceReceiveResult::video &&
              receive.video.rtp_ready && receive.video.summary.packets == 3 && !decoder.frames,
          "Video sink: mapped camera RTP of the negotiated payload type routes to the RTP core");
    voice_video_sink_drive(&receive.video, &connection, false, 1030, 1'030'000);
    check(decoder.frames == 1 && decoder.ssrc == kVideo && decoder.timestamp == 9000 && decoder.keyframe &&
              decoder.intact && decoder.codec == VoiceVideoCodec::h264 && decoder.bytes == sizeof(kAnnexB) &&
              bytes_equal(decoder.frame, kAnnexB, sizeof(kAnnexB)) &&
              dave.calls == 1 && dave.user_id == kUser && dave.ssrc == kVideo && receive.video.buffer &&
              receive.video.summary.frames == 1,
          "Video sink: an assembled access unit is DAVE-opened for its sender and pushed as Annex B");
    // Audio from the same participant never enters the video pipeline.
    u8 plain[64];
    const u8 opus[]{0xfc, 1, 2, 3};
    u32 bytes = rtp(plain, kVoiceOpusPayloadType, kAudio, 1, 960, false, {opus, 4});
    check(voice_receive_packet(&receive, received(connection, {plain, bytes}), {plain, bytes}, audio_unused,
                               nullptr, 1'030'000) != VoiceReceiveResult::video &&
              receive.video.summary.packets == 3,
          "Video sink: audio SSRCs stay on the Opus path");
    // RTX (RFC 4588) of the camera stream maps to its primary SSRC.
    const u8 retransmitted[]{0, 103, 0x41, 0x9a, 0x02};
    bytes = rtp(plain, 102, kRtx, 7, 12000, true, {retransmitted, 5});
    const auto* stream = video_rtp_find_stream(receive.video.rtp, kVideo);
    check(voice_receive_packet(&receive, received(connection, {plain, bytes}), {plain, bytes}, audio_unused,
                               nullptr, 1'040'000) == VoiceReceiveResult::video &&
              stream && stream->stats.rtx_packets == 1 && !video_rtp_find_stream(receive.video.rtp, kRtx) &&
              decoder.frames == 2 && !decoder.keyframe && decoder.intact && decoder.bytes == 7,
          "Video sink: RTX packets reach the primary camera stream and complete its next frame");
    // Unknown payload types and an unannounced SSRC are ignored.
    bytes = rtp(plain, 96, kVideo, 104, 15000, true, {opus, 4});
    check(voice_receive_packet(&receive, received(connection, {plain, bytes}), {plain, bytes}, audio_unused,
                               nullptr, 1'040'000) == VoiceReceiveResult::ignored,
          "Video sink: a video SSRC with an unnegotiated payload type is ignored");

    // Decoder-thread keyframe request: queued, deduplicated, applied on drive.
    voice_video_sink_request_keyframe(&receive.video, kVideo);
    voice_video_sink_request_keyframe(&receive.video, kVideo);
    stream = video_rtp_find_stream(receive.video.rtp, kVideo);
    const bool before = stream && !stream->keyframe_needed;
    voice_video_sink_drive(&receive.video, &connection, false, 1050, 1'050'000);
    check(before && wakes >= 1 && stream->keyframe_needed && receive.video.keyframe_requests == 1 &&
              !receive.video.pending_keyframes[0] && voice_video_sink_deadline(receive.video),
          "Video sink: decoder keyframe requests wake the worker and schedule a PLI on the next drive");
    // A DAVE rejection drops the frame and asks for a new keyframe.
    dave.reject = true;
    send_keyframe(receive, connection, 104, 18000, 1'100'000);
    check(decoder.frames == 2,
          "Video sink: a frame DAVE rejects never reaches the decoder");
    dave.reject = false;
    // The rejected keyframe was a reference: later frames reach the decoder
    // marked damaged (it freezes and keeps asking) until a keyframe arrives.
    const u8 delta[]{0x41, 0x9a, 0x02, 0x10};
    bytes = rtp(plain, 101, kVideo, 107, 21000, true, {delta, 4});
    voice_receive_packet(&receive, received(connection, {plain, bytes}), {plain, bytes}, audio_unused, nullptr,
                         1'101'000);
    check(decoder.frames == 3 && !decoder.keyframe && !decoder.intact,
          "Video sink: a frame after a lost reference is forwarded as not intact");
    send_keyframe(receive, connection, 108, 24000, 1'102'000);
    check(decoder.frames == 4 && decoder.keyframe && decoder.intact,
          "Video sink: the next keyframe is intact again");

    // Transport-wide feedback for the connection's stamped packets rides the
    // sink's RTCP: due at once, last in the compound, from the audio SSRC.
    rtcp_transport_feedback_record(&connection.transport_feedback, 500, kVideo, 1'102'000);
    rtcp_transport_feedback_record(&connection.transport_feedback, 501, kVideo, 1'102'500);
    const bool scheduled = rtcp_transport_feedback_deadline(connection.transport_feedback) == 1;
    const u32 feedback = voice_video_sink_feedback(&receive.video, &connection, 1'103'000);
    u32 at = 0, last = 0;
    while (at < feedback && feedback - at >= 4) {
        last = at;
        at += (u32{byte_load_be16(receive.video.feedback + at + 2)} + 1) * 4;
    }
    const u8* twcc = receive.video.feedback + last;
    check(scheduled && feedback && at == feedback && twcc[0] == 0x8f && twcc[1] == 205 &&
              byte_load_be32(twcc + 4) == 7 && byte_load_be32(twcc + 8) == kVideo &&
              byte_load_be16(twcc + 12) == 500 && byte_load_be16(twcc + 14) == 2 &&
              receive.video.summary.transport_feedback == 1 &&
              !rtcp_transport_feedback_deadline(connection.transport_feedback) &&
              !voice_video_sink_feedback(&receive.video, &connection, 1'103'000),
          "Video sink: transport-wide feedback follows the RTP core's RTCP and is sent once");

    VoiceVideoUser users[4];
    const u32 count = voice_video_users(connection, &receive.video, kSelf, {users, 4});
    check(count == 1 && users[0].user_id == kUser && users[0].ssrc == kVideo && users[0].quality == 100 &&
              users[0].kind == VoiceSourceKind::video && users[0].frames >= 1,
          "Video available: active remote cameras list once at the highest quality; inactive and self excluded");
    check(voice_video_users(connection, nullptr, 0, {users, 4}) == 2 && users[0].user_id == kUser &&
              users[1].user_id == kSelf && !users[0].frames,
          "Video available: the list is sorted by user and needs no media");

    // Op 15: stopping a user's video asks for quality 0 on its video SSRCs.
    receive.video.unwanted[0] = kUser; // as the gateway worker installs per-user wants
    receive.video.unwanted_count = 1;
    voice_video_sink_drive(&receive.video, &connection, false, 1110, 1'110'000);
    bool zero = connection.sink_wants_configured && connection.sink_want_count == 2 &&
                connection.sink_any_quality == 100;
    for (u32 i = 0; zero && i != connection.sink_want_count; ++i)
        zero = (connection.sink_wants[i].ssrc == kVideo || connection.sink_wants[i].ssrc == 24) &&
               !connection.sink_wants[i].quality;
    check(zero, "Video sink: a stopped user's camera SSRCs get sink wants quality 0, others stay at any:100");
    receive.video.unwanted_count = 0;
    voice_video_sink_drive(&receive.video, &connection, false, 1120, 1'120'000);
    check(connection.sink_wants_configured && !connection.sink_want_count && connection.sink_any_quality == 100,
          "Video sink: resuming restores the default wants");

    // Leaving (op 13) ends the stream at the decoder and frees its window.
    check(announce(connection, 13, utf8_literal(R"({"user_id":"2"})")), "Video sink fixture: user leaves");
    voice_video_sink_drive(&receive.video, &connection, false, 1130, 1'130'000);
    check(!video_rtp_find_stream(receive.video.rtp, kVideo) && decoder.ended == 1 && decoder.ended_ssrc == kVideo,
          "Video sink: a departed sender's stream ends at the decoder");
    voice_receive_stop(&receive);
    check(!receive.video.rtp_ready && !receive.video.buffer && !receive.video.rtp.frame && !receive.video.unwanted_count,
          "Video sink: stopping the call frees the RTP core and frame buffers");
    voice_participants_release(&connection.participants);
}

// Every offered codec is bound: H265 first (payload type 101), H264 second (103).
void codec_binding(Check check) noexcept {
    auto& [connection, receive, decoder, dave] = fresh_rig();
    check(announce(connection, 12, utf8_literal(R"({"user_id":"2","audio_ssrc":10,"video_ssrc":20,"streams":[
              {"type":"video","rid":"100","ssrc":20,"rtx_ssrc":21,"active":true,"quality":100}]})")),
          "Video codecs fixture: camera announced");
    receive.video.decrypt = open;
    receive.video.decrypt_context = &dave;
    receive.video.frames = {&decoder, push, end};
    describe(connection, VoiceVideoCodec::h265, VoiceVideoCodec::h264);
    connection.media.ssrc = 7;
    voice_video_sink_drive(&receive.video, &connection, false, 1000, 1'000'000);
    // VPS, SPS, PPS and an IDR_N_LP first slice segment as single NAL packets.
    const u8 vps[]{0x40, 0x01, 0x0c}, sps[]{0x42, 0x01, 0x01}, pps[]{0x44, 0x01, 0xc1}, idr[]{0x28, 0x01, 0xaf, 0x10};
    const ConstSlice<u8> nals[]{{vps, 3}, {sps, 3}, {pps, 3}, {idr, 4}};
    u8 plain[64];
    for (u32 i = 0; i != 4; ++i) {
        const u32 bytes = rtp(plain, 101, kVideo, static_cast<u16>(10 + i), 3000, i == 3, nals[i]);
        voice_receive_packet(&receive, received(connection, {plain, bytes}), {plain, bytes}, audio_unused, nullptr,
                             1'000'000);
    }
    voice_video_sink_drive(&receive.video, &connection, false, 1030, 1'030'000);
    check(decoder.frames == 1 && decoder.codec == VoiceVideoCodec::h265 && decoder.keyframe && decoder.intact &&
              decoder.bytes == 4 * 4 + 13,
          "Video sink: payload type 101 of an H265-first offer reaches the decoder as H.265");
    send_keyframe(receive, connection, 14, 6000, 1'040'000, kVideo, 103);
    voice_video_sink_drive(&receive.video, &connection, false, 1070, 1'070'000);
    check(decoder.frames == 2 && decoder.codec == VoiceVideoCodec::h264 && decoder.keyframe,
          "Video sink: the second offer (H264, payload type 103) is bound too; a stream may switch codec");
    voice_receive_stop(&receive);
    voice_participants_release(&connection.participants);
}

// An AV1-capable device offers AV1 first: payload type 101 carries AV1 OBUs.
void av1_binding(Check check) noexcept {
    auto& [connection, receive, decoder, dave] = fresh_rig();
    check(announce(connection, 12, utf8_literal(R"({"user_id":"2","audio_ssrc":10,"video_ssrc":20,"streams":[
              {"type":"video","rid":"100","ssrc":20,"rtx_ssrc":21,"active":true,"quality":100}]})")),
          "AV1 video fixture: camera announced");
    receive.video.decrypt = open;
    receive.video.decrypt_context = &dave;
    receive.video.frames = {&decoder, push, end};
    describe(connection, VoiceVideoCodec::av1, VoiceVideoCodec::h264);
    connection.media.ssrc = 7;
    voice_video_sink_drive(&receive.video, &connection, false, 1000, 1'000'000);
    // N=1, W=2: a sequence header element, then a frame OBU to the end.
    const u8 unit[]{0x28, 0x03, 0x08, 0x00, 0x00, 0x30, 0x10, 0x20};
    u8 plain[64];
    const u32 bytes = rtp(plain, 101, kVideo, 10, 3000, true, {unit, sizeof(unit)});
    voice_receive_packet(&receive, received(connection, {plain, bytes}), {plain, bytes}, audio_unused, nullptr,
                         1'000'000);
    voice_video_sink_drive(&receive.video, &connection, false, 1030, 1'030'000);
    const u8 expected[]{0x0a, 0x02, 0x00, 0x00, 0x30, 0x10, 0x20};
    check(decoder.frames == 1 && decoder.codec == VoiceVideoCodec::av1 && decoder.keyframe && decoder.intact &&
              decoder.bytes == sizeof(expected) && bytes_equal(decoder.frame, expected, sizeof(expected)),
          "Video sink: payload type 101 of an AV1-first offer reaches the decoder as depacketized AV1");
    send_keyframe(receive, connection, 11, 6000, 1'040'000, kVideo, 103);
    voice_video_sink_drive(&receive.video, &connection, false, 1070, 1'070'000);
    check(decoder.frames == 2 && decoder.codec == VoiceVideoCodec::h264,
          "Video sink: H264 stays bound at payload type 103 behind AV1");
    voice_receive_stop(&receive);
    voice_participants_release(&connection.participants);
}

// Linux announces hardware decoders first: with H.264 and H.265 on the GPU and AV1 only in dav1d, the order is H265,
// H264, AV1. The receiver binds that order, not the codec bits' order (AV1, H265, H264), which would decode every
// payload type as the wrong codec (issue 16).
void announced_order(Check check) noexcept {
    auto& [connection, receive, decoder, dave] = fresh_rig();
    check(announce(connection, 12, utf8_literal(R"({"user_id":"2","audio_ssrc":10,"video_ssrc":20,"streams":[
              {"type":"video","rid":"100","ssrc":20,"rtx_ssrc":21,"active":true,"quality":100}]})")),
          "Announced codec order fixture: camera announced");
    receive.video.decrypt = open;
    receive.video.decrypt_context = &dave;
    receive.video.frames = {&decoder, push, end};
    describe(connection, VoiceVideoCodec::h265, VoiceVideoCodec::h264, VoiceVideoCodec::av1);
    connection.media.ssrc = 7;
    voice_video_sink_drive(&receive.video, &connection, false, 1000, 1'000'000);
    const u8 vps[]{0x40, 0x01, 0x0c}, sps[]{0x42, 0x01, 0x01}, pps[]{0x44, 0x01, 0xc1}, idr[]{0x28, 0x01, 0xaf, 0x10};
    const ConstSlice<u8> nals[]{{vps, 3}, {sps, 3}, {pps, 3}, {idr, 4}};
    u8 plain[64];
    for (u32 i = 0; i != 4; ++i) {
        const u32 bytes = rtp(plain, 101, kVideo, static_cast<u16>(10 + i), 3000, i == 3, nals[i]);
        voice_receive_packet(&receive, received(connection, {plain, bytes}), {plain, bytes}, audio_unused, nullptr,
                             1'000'000);
    }
    voice_video_sink_drive(&receive.video, &connection, false, 1030, 1'030'000);
    check(decoder.frames == 1 && decoder.codec == VoiceVideoCodec::h265 && decoder.keyframe,
          "Video sink: payload type 101 of an H265, H264, AV1 announcement reaches the decoder as H.265");
    send_keyframe(receive, connection, 14, 6000, 1'040'000, kVideo, 103);
    voice_video_sink_drive(&receive.video, &connection, false, 1070, 1'070'000);
    check(decoder.frames == 2 && decoder.codec == VoiceVideoCodec::h264 && decoder.keyframe,
          "Video sink: payload type 103 of an H265, H264, AV1 announcement reaches the decoder as H.264");
    const u8 unit[]{0x28, 0x03, 0x08, 0x00, 0x00, 0x30, 0x10, 0x20};
    const u32 bytes = rtp(plain, 105, kVideo, 17, 9000, true, {unit, sizeof(unit)});
    voice_receive_packet(&receive, received(connection, {plain, bytes}), {plain, bytes}, audio_unused, nullptr,
                         1'080'000);
    voice_video_sink_drive(&receive.video, &connection, false, 1110, 1'110'000);
    check(decoder.frames == 3 && decoder.codec == VoiceVideoCodec::av1 && decoder.keyframe,
          "Video sink: payload type 105 of an H265, H264, AV1 announcement reaches the decoder as AV1");
    voice_receive_stop(&receive);
    voice_participants_release(&connection.participants);
}

// The stall check reads the voice log: video_stalled records and the connection kind of the last one.
u32 stall_records;
u64 stall_stream;
bool same_text(const char* text, const char* expected) noexcept {
    while (*text && *text == *expected) {
        ++text;
        ++expected;
    }
    return *text == *expected;
}
void count_stalls(VoiceLogLevel level, const char* event, ConstSlice<VoiceLogField> fields) noexcept {
    if (level == VoiceLogLevel::warning && same_text(event, "video_stalled")) {
        ++stall_records;
        stall_stream = fields.count && same_text(fields[0].key, "stream") ? fields[0].value : ~u64{0};
    }
}
// A keyframe every half second from from_us through to_us, each followed by a drive. Returns the next sequence.
u16 keyframes_until(Rig& rig, u16 sequence, u64 from_us, u64 to_us) noexcept {
    for (u64 t = from_us; t <= to_us; t += 500'000) {
        send_keyframe(rig.receive, rig.connection, sequence, static_cast<u32>(t / 1000 * 90), t);
        sequence = static_cast<u16>(sequence + 3);
        voice_video_sink_drive(&rig.receive.video, &rig.connection, false, t / 1000, t);
    }
    return sequence;
}
Rig& stall_rig(Check check, bool picture) noexcept {
    auto& rig = fresh_rig();
    check(announce(rig.connection, 12, utf8_literal(R"({"user_id":"2","audio_ssrc":10,"video_ssrc":20,"streams":[
              {"type":"video","rid":"100","ssrc":20,"rtx_ssrc":21,"active":true,"quality":100}]})")),
          "Video stall fixture: camera announced");
    rig.receive.video.decrypt = open;
    rig.receive.video.decrypt_context = &rig.dave;
    rig.receive.video.frames = {&rig.decoder, push, end, nullptr, decoded, stalled};
    rig.decoder.decoded = picture;
    describe(rig.connection, VoiceVideoCodec::h264);
    rig.connection.media.ssrc = 7;
    voice_video_sink_drive(&rig.receive.video, &rig.connection, false, 1000, 1'000'000);
    return rig;
}
// A stream that receives packets for five seconds and shows no picture cannot be played: the sink logs it once and
// reports it to the decoder side. A pause in the packets starts the wait again. A decoded picture ends the check.
void stall(Check check) noexcept {
    const VoiceLogSink previous = g_voice_log_sink;
    g_voice_log_sink = count_stalls;
    stall_records = 0;
    auto& rig = stall_rig(check, false);
    u16 sequence = keyframes_until(rig, 100, 1'000'000, 3'000'000);
    voice_video_sink_drive(&rig.receive.video, &rig.connection, false, 4500, 4'500'000); // No packet since 3 s.
    sequence = keyframes_until(rig, sequence, 6'000'000, 10'500'000);
    check(!rig.decoder.stalls && !stall_records && rig.decoder.frames &&
              voice_video_sink_deadline(rig.receive.video) <= 11'000,
          "Video stall: a pause in the packets starts the wait again, and the sink wakes when the wait ends");
    sequence = keyframes_until(rig, sequence, 11'000'000, 11'000'000);
    check(rig.decoder.stalls == 1 && rig.decoder.stalled_ssrc == kVideo && stall_records == 1 && !stall_stream,
          "Video stall: five seconds of packets without a picture are reported and logged once");
    keyframes_until(rig, sequence, 11'500'000, 13'000'000);
    check(rig.decoder.stalls == 1 && stall_records == 1, "Video stall: a stream is reported only once");
    voice_receive_stop(&rig.receive);
    voice_participants_release(&rig.connection.participants);

    auto& shown = stall_rig(check, true);
    keyframes_until(shown, 100, 1'000'000, 8'000'000);
    check(!shown.decoder.stalls && stall_records == 1 && shown.decoder.frames,
          "Video stall: a stream with a decoded picture is never reported");
    voice_receive_stop(&shown.receive);
    voice_participants_release(&shown.connection.participants);
    g_voice_log_sink = previous;
}

void dave_frames(Check check) noexcept {
    static VoiceConnection connection;
    connection = {};
    check(announce(connection, 12, utf8_literal(R"({"user_id":"2","audio_ssrc":10,"video_ssrc":20,"streams":[
              {"type":"video","ssrc":20,"rtx_ssrc":21,"active":true}]})")),
          "Video DAVE fixture: camera announced");
    u8 output[64], scratch[64];
    const ConstSlice<u8> frame{kAnnexB, sizeof(kAnnexB)};
    auto result = voice_connection_dave_decrypt_frame(&connection, kUser, kVideo, frame, {scratch, 64},
                                                      {output, 64}, 1);
    check(result.error == DaveMediaError::not_ready, "Video DAVE: frames wait for the call's DAVE session");
    check(dave_session_initialize(&connection.dave_session, 1, kSelf, 0) == MlsStatus::ok,
          "Video DAVE fixture: protocol version 0 session");
    result = voice_connection_dave_decrypt_frame(&connection, kUser, kVideo, frame, {scratch, 64}, {output, 64}, 1);
    check(result.ok() && result.passthrough && result.bytes == sizeof(kAnnexB) &&
              bytes_equal(output, kAnnexB, sizeof(kAnnexB)),
          "Video DAVE: unencrypted frames pass through only while the negotiated version is zero");
    check(voice_connection_dave_decrypt_frame(&connection, kUser, kAudio, frame, {scratch, 64}, {output, 64}, 1)
                      .error == DaveMediaError::invalid_input &&
              voice_connection_dave_decrypt_frame(&connection, kUser, kRtx, frame, {scratch, 64}, {output, 64}, 1)
                      .error == DaveMediaError::invalid_input &&
              voice_connection_dave_decrypt_frame(&connection, 3, kVideo, frame, {scratch, 64}, {output, 64}, 1)
                      .error == DaveMediaError::invalid_input,
          "Video DAVE: a frame must come from the user's current camera/screen SSRC");
    dave_session_release(&connection.dave_session);
    voice_participants_release(&connection.participants);
}
// The stream's first unit completes inside its start wait and the sender's audio for the same key arrives before the
// wait ends: the unit is DAVE-opened when it completes (so its counter precedes the audio's), held, and emitted
// once after the wait without a second open (which a replay window would refuse).
void start_with_audio(Check check) noexcept {
    auto& [connection, receive, decoder, dave] = fresh_rig();
    check(announce(connection, 11, utf8_literal(R"({"user_ids":["2"]})")) &&
              announce(connection, 12, utf8_literal(R"({"user_id":"2","audio_ssrc":10,"video_ssrc":20,"streams":[
                  {"type":"video","rid":"100","ssrc":20,"rtx_ssrc":21,"active":true,"quality":100}]})")),
          "Video sink start: the sender announces a camera");
    receive.video.decrypt = open;
    receive.video.decrypt_context = &dave;
    receive.video.frames = {&decoder, push, end};
    receive.video.wake = wake;
    describe(connection, VoiceVideoCodec::h264);
    connection.media.ssrc = 7;
    voice_video_sink_drive(&receive.video, &connection, false, 1000, 1'000'000);
    const u64 buffers = 2 * u64{kVideoRtpFrameBytes};
    const auto committed = [&]() noexcept { return (receive.video.buffer ? buffers : 0) + receive.video.held_committed; };
    check(!committed(), "Video sink start: an idle sink commits no frame memory");
    send_keyframe(receive, connection, 100, 9000, 1'000'000);
    check(dave.calls == 1 && !decoder.frames && !dave.replays,
          "Video sink start: a whole first unit is DAVE-opened as it completes and held through the start wait");
    check(committed() == buffers + 4096,
          "Video sink start: a held unit commits only the pages it needs (one page here) beyond the two buffers");
    dave.audio_seen = true; // The sender's audio for this key is opened as it arrives, inside the wait.
    voice_video_sink_drive(&receive.video, &connection, false, 1010, 1'010'000);
    check(!decoder.frames, "Video sink start: the held unit stays back until the start wait is over");
    voice_video_sink_drive(&receive.video, &connection, false, 1030, 1'030'000);
    check(decoder.frames == 1 && decoder.keyframe && decoder.intact && decoder.bytes == sizeof(kAnnexB) &&
              bytes_equal(decoder.frame, kAnnexB, sizeof(kAnnexB)) && dave.calls == 1 && !dave.replays,
          "Video sink start: audio arriving during the wait causes no decrypt failure and the unit is emitted once");
    check(committed() == buffers,
          "Video sink start: the held unit's pages are decommitted once it is emitted");
    voice_receive_stop(&receive);
    check(!committed() && !receive.video.held,
          "Video sink start: stopping frees the buffers and the reservation");
    voice_participants_release(&connection.participants);
}
// The sender's DAVE counters as a strict receiver window sees them: each unit carries its counter (the IDR unit 1,
// the P unit 3), the test opens the audio counters 2 and 4 as they arrive, and a counter at or below the newest one
// is refused.
struct CountedDave {
    u32 newest, refused, opened[4], opens;
};
bool counted_open(CountedDave& dave, u32 counter) noexcept {
    if (counter <= dave.newest) {
        ++dave.refused;
        return false;
    }
    dave.newest = counter;
    return true;
}
DaveMediaResult open_counted(void* context, u64, u32, ConstSlice<u8> frame, Slice<u8>, Slice<u8> output) noexcept {
    auto& dave = *static_cast<CountedDave*>(context);
    u32 counter = 3;
    for (u32 i = 4; i < frame.count; ++i)
        if (frame.data[i - 4] == 0 && frame.data[i - 3] == 0 && frame.data[i - 2] == 0 && frame.data[i - 1] == 1 &&
            (frame.data[i] & 0x1f) == 5)
            counter = 1;
    DaveMediaResult result{};
    if (output.count < frame.count || !counted_open(dave, counter)) {
        result.error = DaveMediaError::rejected;
        return result;
    }
    if (dave.opens < 4)
        dave.opened[dave.opens] = counter;
    ++dave.opens;
    copy_bytes(output.data, frame.data, frame.count);
    result.bytes = frame.count;
    return result;
}
// Two units complete inside the stream's start wait, with the sender's audio after each: both units are DAVE-opened
// as they complete, in arrival order, held, and emitted after the wait without a second open.
void start_with_two_units(Check check) noexcept {
    auto& [connection, receive, decoder, dave] = fresh_rig();
    static CountedDave counted;
    counted = {};
    check(announce(connection, 11, utf8_literal(R"({"user_ids":["2"]})")) &&
              announce(connection, 12, utf8_literal(R"({"user_id":"2","audio_ssrc":10,"video_ssrc":20,"streams":[
                  {"type":"video","rid":"100","ssrc":20,"rtx_ssrc":21,"active":true,"quality":100}]})")),
          "Video sink start, two units: the sender announces a camera");
    receive.video.decrypt = open_counted;
    receive.video.decrypt_context = &counted;
    receive.video.frames = {&decoder, push, end};
    receive.video.wake = wake;
    describe(connection, VoiceVideoCodec::h264);
    connection.media.ssrc = 7;
    voice_video_sink_drive(&receive.video, &connection, false, 1000, 1'000'000);
    send_keyframe(receive, connection, 100, 9000, 1'000'000);
    const bool audio_2 = counted_open(counted, 2);
    const u8 slice[]{0x41, 0x9a, 0x02, 0x03};
    u8 plain[64];
    const u32 bytes = rtp(plain, 101, kVideo, 103, 12000, true, {slice, sizeof(slice)});
    voice_receive_packet(&receive, received(connection, {plain, bytes}), {plain, bytes}, audio_unused, nullptr,
                         1'005'000);
    const bool audio_4 = counted_open(counted, 4);
    check(audio_2 && audio_4 && counted.opens == 2 && counted.opened[0] == 1 && counted.opened[1] == 3 &&
              !decoder.frames,
          "Video sink start, two units: each unit is DAVE-opened as it completes, before the audio after it");
    voice_video_sink_drive(&receive.video, &connection, false, 1030, 1'030'000);
    check(decoder.frames == 2 && counted.opens == 2 && !counted.refused && decoder.bytes == 8 &&
              decoder.timestamp == 12000,
          "Video sink start, two units: both held units are emitted in order after the wait, with no second open");
    check(!receive.video.held_committed,
          "Video sink start, two units: the held pages are decommitted once the last unit is emitted");
    voice_receive_stop(&receive);
    voice_participants_release(&connection.participants);
}
} // namespace

void test_voice_video_sink(Check check) noexcept {
    routing(check);
    start_with_audio(check);
    start_with_two_units(check);
    codec_binding(check);
    av1_binding(check);
    announced_order(check);
    stall(check);
    dave_frames(check);
}
} // namespace maicord
