#include "maicord/protocol/voice_video_signaling.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/crt.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
template <typename Write> void wire(Check check, Write write, Utf8View expected, const char* name) noexcept {
    static u8 output[1024];
    const auto measure = write(Slice<u8>{});
    bool ok = measure.error == GatewayCommandError::output_too_small && measure.bytes == expected.bytes;
    // Every short capacity reports the size and leaves the caller's bytes alone.
    for (u32 capacity = 0; ok && capacity < expected.bytes; capacity += 1 + capacity / 8) {
        memset(output, 0xCC, sizeof(output));
        const auto partial = write(Slice<u8>{output, capacity});
        ok = partial.error == GatewayCommandError::output_too_small && partial.bytes == expected.bytes &&
             output[0] == 0xCC && output[capacity ? capacity - 1 : 0] == 0xCC;
    }
    memset(output, 0xCC, sizeof(output));
    const auto written = write(Slice<u8>{output, expected.bytes});
    u32 scratch[512]{};
    VoiceGatewayEnvelope envelope{};
    ok = ok && written.ok() && written.bytes == expected.bytes && bytes_equal(output, expected.data, expected.bytes) &&
         output[expected.bytes] == 0xCC &&
         voice_gateway_decode_json({reinterpret_cast<const char*>(output), written.bytes}, {scratch, 512}, 24,
                                   &envelope)
             .ok();
    check(ok, name);
}
constexpr char kId[] = "0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f0";

void test_select_protocol(Check check) noexcept {
    VoiceSelectProtocolMedia selection{{utf8_literal("198.51.100.47"), 4660, voice_transport_aes256_gcm_rtpsize},
                                       {},
                                       {kId, kVoiceRtcConnectionIdBytes}};
    wire(check, [&](Slice<u8> out) noexcept { return voice_gateway_write_select_protocol_media(selection, out); },
         utf8_literal(
             R"({"op":1,"d":{"protocol":"udp","data":{"address":"198.51.100.47","port":4660,"mode":"aead_aes256_gcm_rtpsize"},"address":"198.51.100.47","port":4660,"mode":"aead_aes256_gcm_rtpsize","codecs":[{"name":"opus","type":"audio","priority":1000,"payload_type":120}],"rtc_connection_id":"0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f0","experiments":[]}})"),
         "Select Protocol: an audio-only offer carries Opus PT 120 without RTX or encode/decode flags");
    const VoiceCodecOffer h264[]{{VoiceVideoCodec::h264, false, true}};
    selection.video = {h264, 1};
    wire(check, [&](Slice<u8> out) noexcept { return voice_gateway_write_select_protocol_media(selection, out); },
         utf8_literal(
             R"({"op":1,"d":{"protocol":"udp","data":{"address":"198.51.100.47","port":4660,"mode":"aead_aes256_gcm_rtpsize"},"address":"198.51.100.47","port":4660,"mode":"aead_aes256_gcm_rtpsize","codecs":[{"name":"opus","type":"audio","priority":1000,"payload_type":120},{"name":"H264","type":"video","priority":1000,"payload_type":101,"rtx_payload_type":102,"encode":false,"decode":true}],"rtc_connection_id":"0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f0","experiments":[]}})"),
         "Select Protocol: H.264 decode-only is PT 101 with RTX 102 and encode false");
    const VoiceCodecOffer both[]{{VoiceVideoCodec::h265, false, true}, {VoiceVideoCodec::h264, true, true}};
    selection.video = {both, 2};
    selection.transport = {utf8_literal("2001:db8::1234"), 65535, voice_transport_xchacha20_poly1305_rtpsize};
    wire(check, [&](Slice<u8> out) noexcept { return voice_gateway_write_select_protocol_media(selection, out); },
         utf8_literal(
             R"({"op":1,"d":{"protocol":"udp","data":{"address":"2001:db8::1234","port":65535,"mode":"aead_xchacha20_poly1305_rtpsize"},"address":"2001:db8::1234","port":65535,"mode":"aead_xchacha20_poly1305_rtpsize","codecs":[{"name":"opus","type":"audio","priority":1000,"payload_type":120},{"name":"H265","type":"video","priority":1000,"payload_type":101,"rtx_payload_type":102,"encode":false,"decode":true},{"name":"H264","type":"video","priority":2000,"payload_type":103,"rtx_payload_type":104,"encode":true,"decode":true}],"rtc_connection_id":"0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f0","experiments":[]}})"),
         "Select Protocol: video codecs take PT 101+2i, RTX +1 and priority 1000(i+1) in offer order");

    u8 output[1024];
    const auto rejected = [&](const VoiceSelectProtocolMedia& value) noexcept {
        memset(output, 0xAA, sizeof(output));
        return !voice_gateway_write_select_protocol_media(value, {output, sizeof(output)}).ok() &&
               output[0] == 0xAA && output[1023] == 0xAA;
    };
    auto invalid = selection;
    invalid.rtc_connection_id = {"0F1E2D3C-4B5A-4978-8695-A4B3C2D1E0F0", 36};
    bool ok = rejected(invalid);
    invalid.rtc_connection_id = {kId, 35};
    ok &= rejected(invalid);
    invalid.rtc_connection_id = {"0f1e2d3c04b5a-4978-8695-a4b3c2d1e0f0", 36};
    ok &= rejected(invalid);
    check(ok, "Select Protocol: a missing, uppercase or misformatted RTC connection id is rejected unwritten");
    const VoiceCodecOffer duplicate[]{{VoiceVideoCodec::h264, false, true}, {VoiceVideoCodec::h264, false, true}};
    const VoiceCodecOffer idle[]{{VoiceVideoCodec::h264, false, false}};
    const VoiceCodecOffer unnamed[]{{VoiceVideoCodec::unknown, false, true}};
    invalid = selection;
    invalid.video = {duplicate, 2};
    ok = rejected(invalid);
    invalid.video = {idle, 1};
    ok &= rejected(invalid);
    invalid.video = {unnamed, 1};
    ok &= rejected(invalid);
    invalid.video = {nullptr, 1};
    ok &= rejected(invalid);
    invalid = selection;
    invalid.transport.mode = 3;
    ok &= rejected(invalid);
    invalid = selection;
    invalid.transport.port = 0;
    ok &= rejected(invalid);
    const Utf8View invalid_addresses[]{{}, {nullptr, 1}, {"\xff", 1}, {reinterpret_cast<const char*>(~usize{0} - 1), 4}};
    for (auto address : invalid_addresses) {
        invalid = selection;
        invalid.transport.address = address;
        ok &= rejected(invalid);
    }
    check(ok, "Select Protocol: duplicate, unusable or unnamed codecs and bad transport fields are rejected");
    copy_bytes(output, kId, 36);
    invalid = selection;
    invalid.rtc_connection_id = {reinterpret_cast<const char*>(output), 36};
    check(!voice_gateway_write_select_protocol_media(invalid, {output, sizeof(output)}).ok() &&
              bytes_equal(output, kId, 36),
          "Select Protocol: an RTC connection id aliasing the output is rejected");

    const u8 random[16]{0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00};
    char id[40];
    memset(id, '#', sizeof(id));
    check(voice_rtc_connection_id({random, 16}, {id, 40}) &&
              bytes_equal(id, "ffeeddcc-bbaa-4988-b766-554433221100", 36) && id[36] == '#' &&
              !voice_rtc_connection_id({random, 15}, {id, 40}) && !voice_rtc_connection_id({random, 16}, {id, 35}),
          "RTC connection id: 16 random bytes format as a lowercase RFC 4122 version-4 UUID");
}

void test_sink_wants(Check check) noexcept {
    wire(check, [](Slice<u8> out) noexcept { return voice_gateway_write_media_sink_wants(100, {}, out); },
         utf8_literal(R"({"op":15,"d":{"any":100}})"),
         "Media sink wants: the default asks for any stream at full quality");
    const VoiceSinkWant wants[]{{4000000000u, 100}, {7, 0}, {300, 50}};
    wire(check, [&](Slice<u8> out) noexcept { return voice_gateway_write_media_sink_wants(0, {wants, 3}, out); },
         utf8_literal(R"({"op":15,"d":{"7":0,"300":50,"4000000000":100,"any":0}})"),
         "Media sink wants: per-SSRC qualities precede any in ascending integer-key order");
    const VoiceSinkWant duplicate[]{{7, 0}, {7, 100}}, zero[]{{0, 100}}, high[]{{9, 101}}, top[]{{~u32{0}, 0}};
    u8 output[128];
    memset(output, 0xAA, sizeof(output));
    check(!voice_gateway_write_media_sink_wants(101, {}, {output, 128}).ok() &&
              !voice_gateway_write_media_sink_wants(100, {duplicate, 2}, {output, 128}).ok() &&
              !voice_gateway_write_media_sink_wants(100, {zero, 1}, {output, 128}).ok() &&
              !voice_gateway_write_media_sink_wants(100, {high, 1}, {output, 128}).ok() &&
              !voice_gateway_write_media_sink_wants(100, {top, 1}, {output, 128}).ok() &&
              !voice_gateway_write_media_sink_wants(100, {nullptr, 1}, {output, 128}).ok() && output[0] == 0xAA,
          "Media sink wants: quality above 100, duplicate, zero or non-index SSRCs are rejected unwritten");
}

void test_session_update(Check check) noexcept {
    u32 scratch[256];
    const auto decode = [&](Utf8View text, VoiceSessionUpdate* update) noexcept {
        return voice_decode_session_update({text, 0, JsonValueKind::object}, {scratch, 256}, 16, update).ok();
    };
    VoiceSessionUpdate update{};
    check(decode(utf8_literal(
                     R"({"audio_codec":"opus","video_codec":"H264","media_session_id":"abc","keyframe_interval":2000,"extra":[1]})"),
                 &update) &&
              update.has_video_codec && update.video == VoiceVideoCodec::h264 &&
              update.has_keyframe_interval && update.keyframe_interval_ms == 2000,
          "Session Update: video codec and keyframe interval decode; unknown fields are ignored");
    update = {};
    check(decode(utf8_literal(R"({"audio_codec":null,"video_codec":"AV2","media_session_id":null})"), &update) &&
              update.has_video_codec && update.video == VoiceVideoCodec::unknown &&
              !update.has_keyframe_interval,
          "Session Update: null fields are absent and an unrecognized codec name is unknown");
    update = {};
    check(decode(utf8_literal(R"({})"), &update) && !update.has_video_codec, "Session Update: an empty update decodes");
    const Utf8View invalid[]{utf8_literal(R"({"video_codec":"H264","video_codec":"VP8"})"),
                             utf8_literal(R"({"video_codec":5})"), utf8_literal(R"({"video_codec":""})"),
                             utf8_literal(R"({"keyframe_interval":-1})"),
                             utf8_literal(R"({"keyframe_interval":4294967296})")};
    bool preserved = true;
    for (const auto text : invalid) {
        update.keyframe_interval_ms = 77;
        preserved &= !decode(text, &update) && update.keyframe_interval_ms == 77;
    }
    check(preserved, "Session Update: duplicate or mistyped fields fail and preserve the output");
    const struct {
        Utf8View name;
        VoiceVideoCodec codec;
    } names[]{{utf8_literal("H264"), VoiceVideoCodec::h264}, {utf8_literal("H265"), VoiceVideoCodec::h265},
              {utf8_literal("VP8"), VoiceVideoCodec::vp8},   {utf8_literal("VP9"), VoiceVideoCodec::vp9},
              {utf8_literal("AV1"), VoiceVideoCodec::av1},   {utf8_literal("h264"), VoiceVideoCodec::unknown}};
    bool named = voice_video_codec({}) == VoiceVideoCodec::none;
    for (const auto& entry : names)
        named &= voice_video_codec({entry.name, 0, false}) == entry.codec;
    check(named, "Video codec names match the case-sensitive wire names");
}
} // namespace

void test_voice_video_signaling(Check check) noexcept {
    test_select_protocol(check);
    test_sink_wants(check);
    test_session_update(check);
}
} // namespace maicord
