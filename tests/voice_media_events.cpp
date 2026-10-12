#include "maicord/app/voice_media_setup.hpp"
#include "maicord/protocol/voice_gateway.hpp"

#include <WS2tcpip.h>

namespace maicord {
namespace {
JsonValue object(Utf8View input) noexcept {
    return {input, 0, JsonValueKind::object};
}
bool filled(ConstSlice<u8> bytes, u8 value) noexcept {
    for (u32 i = 0; i < bytes.count; ++i)
        if (bytes[i] != value)
            return false;
    return true;
}
constexpr auto ready_json = utf8_literal(
    "{\"ssrc\":4294967295,\"ip\":\"2001:db8::1\",\"port\":65535,"
    "\"m\\u006fdes\":[\"future_mode\",\"aead_aes256_gcm_rtpsize\",\"aead_xchacha20_poly1305_rtpsize\"],"
    "\"streams\":[{\"type\":\"video\",\"rid\":\"100\",\"ssrc\":20,\"rtx_ssrc\":21}],"
    "\"experiments\":[\"fixed_keyframe_interval\"],\"heartbeat_interval\":\"ignored\"}");
constexpr auto description_json = utf8_literal(
    "{\"mode\":\"aead_aes256_gcm_rtpsize\",\"secret_key\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,"
    "16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31],\"audio_codec\":\"opus\",\"video_codec\":\"H264\","
    "\"media_session_id\":\"fixture\",\"keyframe_interval\":5000,\"dave_protocol_version\":1}");
} // namespace
void test_voice_media_events(void (*check)(bool, const char*) noexcept) noexcept {
    u32 scratch[512];
    VoiceReady ready{};
    check(voice_decode_ready(object(ready_json), {scratch, 512}, 16, &ready).ok() &&
              ready.ssrc == ~u32{0} && ready.port == 65535 && ready.recognized_modes == 3 &&
              ready.has_streams && ready.has_experiments && ready.streams.kind == JsonValueKind::array,
          "voice Ready decodes endpoint/modes and preserves streams/experiments");
    const Utf8View invalid_ready[]{
        utf8_literal("{}"),
        utf8_literal("{\"ssrc\":0,\"ip\":\"x\",\"port\":1,\"modes\":[\"x\"]}"),
        utf8_literal("{\"ssrc\":4294967296,\"ip\":\"x\",\"port\":1,\"modes\":[\"x\"]}"),
        utf8_literal("{\"ssrc\":1,\"ip\":\"x\",\"port\":65536,\"modes\":[\"x\"]}"),
        utf8_literal("{\"ssrc\":1,\"ip\":\"x\",\"port\":0,\"modes\":[\"x\"]}"),
        utf8_literal("{\"ssrc\":1,\"ip\":\"x\",\"port\":1,\"modes\":[]}"),
        utf8_literal("{\"ssrc\":1,\"ip\":\"x\",\"port\":1,\"modes\":[1]}"),
        utf8_literal("{\"ssrc\":1,\"ip\":\"x\",\"port\":1,\"modes\":[\"x\"],\"streams\":[1]}"),
        utf8_literal("{\"ssrc\":1,\"ip\":\"x\",\"port\":1,\"modes\":[\"x\"],\"experiments\":[null]}"),
        utf8_literal("{\"ssrc\":1,\"ip\":\"x\",\"port\":1,\"modes\":[\"x\"],\"p\\u006frt\":2}")};
    for (const auto input : invalid_ready) {
        VoiceReady output = ready;
        check(!voice_decode_ready(object(input), {scratch, 512}, 16, &output).ok() &&
                  output.ip.encoded.data == ready.ip.encoded.data && output.port == 65535 &&
                  output.ssrc == ~u32{0},
              "voice Ready malformed/duplicate fields preserve previous output");
    }
    u8 key[32];
    memset(key, 0xa5, sizeof(key));
    VoiceSessionDescription description{};
    check(voice_decode_session_description(object(description_json), {scratch, 512}, 16, &description,
                                           {key, 32})
                  .ok() &&
              description.udp && description.recognized_mode == 1 &&
              description.dave_protocol_version == 1 && description.has_audio_codec &&
              description.has_video_codec && description.keyframe_interval_ms == 5000,
          "voice Session Description validates UDP key and negotiated metadata");
    bool exact = true;
    for (u32 i = 0; i < 32; ++i)
        exact &= key[i] == i;
    check(exact, "voice transport key copies exactly the validated 32 bytes");
    // Every wrong key length, and each position containing a non-byte, must
    // preserve both outputs, including after earlier key values were decoded.
    for (u32 variant = 0; variant < 68; ++variant) {
        char json[512];
        u32 used = 0;
        const auto add = [&](Utf8View value) noexcept {
            copy_bytes(json + used, value.data, value.bytes);
            used += value.bytes;
        };
        add(utf8_literal("{\"mode\":\"aead_aes256_gcm_rtpsize\",\"secret_key\":["));
        const u32 count = variant < 34 ? variant : 32;
        for (u32 i = 0; i < count; ++i) {
            if (i)
                add(utf8_literal(","));
            add(variant >= 34 && i == variant - 34 ? utf8_literal("256") : utf8_literal("0"));
        }
        add(utf8_literal("]"));
        if (variant == 66)
            add(utf8_literal(",\"mode\":\"duplicate\""));
        if (variant == 67)
            add(utf8_literal(",\"dave_protocol_version\":65536"));
        add(utf8_literal("}"));
        if (variant == 32)
            continue; // The one valid length fixture.
        memset(key, 0xa5, sizeof(key));
        description.dave_protocol_version = 77;
        check(!voice_decode_session_description(object({json, used}), {scratch, 512}, 16, &description,
                                                {key, 32})
                      .ok() &&
                  filled({key, 32}, 0xa5) && description.dave_protocol_version == 77,
              "voice malformed key/late metadata preserves key and description outputs");
    }
    const auto sdp = object(utf8_literal("{\"sdp\":\"v=0\\r\\n\",\"audio_codec\":\"opus\"}"));
    check(voice_decode_session_description(sdp, {scratch, 512}, 16, &description, {key, 32}).ok() &&
              !description.udp && description.sdp.encoded.bytes && filled({key, 32}, 0),
          "voice SDP alternative has no transport key and clears successful key destination");
    const Utf8View invalid_descriptions[]{utf8_literal("{}"),
                                          utf8_literal("{\"mode\":\"x\"}"),
                                          utf8_literal("{\"sdp\":\"x\",\"mode\":\"x\"}"),
                                          utf8_literal("{\"sdp\":\"x\",\"video_codec\":5}"),
                                          utf8_literal("{\"sdp\":\"x\",\"dave_protocol_version\":-1}"),
                                          utf8_literal("{\"sdp\":\"x\",\"keyframe_interval\":1.5}")};
    for (const auto input : invalid_descriptions)
        check(!voice_decode_session_description(object(input), {scratch, 512}, 16, &description).ok(),
              "voice Session Description rejects mixed/incomplete transports and malformed metadata");
    memset(key, 0xa5, sizeof(key));
    check(!voice_decode_session_description(object(description_json), {scratch, 2}, 16, &description,
                                            {key, 32})
                  .ok() &&
              filled({key, 32}, 0xa5),
          "voice structural exhaustion preserves secret destination");

    VoiceMediaSetup media{};
    check(voice_media_ready(&media, ready) == VoiceMediaSetupError::none && media.has_ready &&
              reinterpret_cast<const sockaddr_storage&>(media.endpoint).ss_family == AF_INET6 && media.endpoint_bytes == sizeof(sockaddr_in6),
          "voice media setup owns a numeric IPv6 endpoint beyond input lifetime");
    check(voice_media_description(&media, object(description_json), {scratch, 512}, 16, 1, 7) ==
                  VoiceMediaSetupError::order &&
              !media.packets.aes.object.data,
          "voice rejects unsolicited description before protocol selection");
    check(voice_media_protocol_selected(&media, 2) &&
              voice_media_description(&media, object(description_json), {scratch, 512}, 16, 1, 7) ==
                  VoiceMediaSetupError::mode &&
              !media.packets.aes.object.data && !media.has_description,
          "voice server cannot substitute another transport mode after selection");
    check(voice_media_ready(&media, ready) == VoiceMediaSetupError::none,
          "voice mode fixture resets with Ready");
    check(!voice_media_protocol_selected(&media, 3) && voice_media_protocol_selected(&media, 1) &&
              !voice_media_protocol_selected(&media, 2),
          "voice selects one offered mode per Ready");
    check(voice_media_description(&media, object(description_json), {scratch, 512}, 16, 0, 7) ==
                  VoiceMediaSetupError::dave_version &&
              !media.packets.aes.object.data,
          "voice rejects DAVE versions beyond the actual Identify advertisement");
    check(voice_media_description(&media, object(description_json), {scratch, 512}, 16, 1, 7) ==
                  VoiceMediaSetupError::none &&
              media.packets.aes.object.locked && media.packets.generation == 7 && media.has_description,
          "voice negotiated transport key lives only in its locked owner");
    auto* installed = media.packets.aes.object.data;
    const u8 rtp[]{0x80, 0x78, 0, 1, 0, 0, 3, 0xc0, 1, 2, 3, 4, 0xf8, 0xff, 0xfe};
    u8 wire[64], restored[64];
    const auto sealed = voice_packet_seal(media.packets, 7, {rtp, sizeof(rtp)}, {wire, sizeof(wire)});
    const auto opened =
        voice_packet_open(media.packets, 7, {wire, sealed.bytes}, {restored, sizeof(restored)});
    check(sealed.ok() && sealed.nonce == 0 && opened.ok() && opened.bytes == sizeof(rtp) &&
              bytes_equal(restored, rtp, sizeof(rtp)) && media.packets.next_nonce == 1,
          "accepted voice Session Description installs an operational packet cipher");
    check(voice_media_description(&media, object(description_json), {scratch, 512}, 16, 1, 7) ==
                  VoiceMediaSetupError::order &&
              media.packets.aes.object.data == installed && media.packets.next_nonce == 1,
          "voice duplicate description cannot silently replace installed key");
    auto invalid = ready;
    invalid.ip = {utf8_literal("127.0.0.1\\u0000evil"), 0, true};
    check(voice_media_ready(&media, invalid) == VoiceMediaSetupError::endpoint &&
              media.packets.aes.object.data == installed,
          "voice numeric endpoint rejects embedded NUL without clobbering negotiation");
    invalid.ip = {utf8_literal("voice.example.test"), 0, false};
    check(voice_media_ready(&media, invalid) == VoiceMediaSetupError::endpoint,
          "voice Ready IP cannot become an unexpected DNS lookup");
    invalid.ip = {utf8_literal("127.0.0.1"), 0, false};
    check(voice_media_ready(&media, invalid) == VoiceMediaSetupError::none &&
              !media.packets.aes.object.data && !media.selected_mode && reinterpret_cast<const sockaddr_storage&>(media.endpoint).ss_family == AF_INET,
          "voice replacement Ready retires previous key and protocol selection");
    check(voice_packet_seal(media.packets, 7, {rtp, sizeof(rtp)}, {wire, sizeof(wire)}).error ==
              VoicePacketError::not_initialized,
          "replacement Ready disables the old packet key");
    constexpr auto xdescription =
        utf8_literal("{\"mode\":\"aead_xchacha20_poly1305_rtpsize\",\"secret_key\":[0,1,2,3,4,5,6,7,8,9,10,"
                     "11,12,13,14,15,"
                     "16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31],\"dave_protocol_version\":1}");
    check(voice_media_protocol_selected(&media, 2) &&
              voice_media_description(&media, object(xdescription), {scratch, 512}, 16, 1, 8) ==
                  VoiceMediaSetupError::none &&
              media.packets.xchacha.storage.locked && !media.packets.aes.object.data &&
              media.packets.generation == 8,
          "replacement voice description installs the selected XChaCha cipher in its new generation");
    check(voice_packet_seal(media.packets, 7, {rtp, sizeof(rtp)}, {wire, sizeof(wire)}).error ==
                  VoicePacketError::stale_generation &&
              !media.packets.next_nonce,
          "old queued media cannot consume a replacement key nonce");
    check(voice_packet_seal(media.packets, 8, {rtp, sizeof(rtp)}, {wire, sizeof(wire)}).ok(),
          "new voice generation encrypts with the selected replacement cipher");
    voice_media_clear(&media);
    check(!media.has_ready && !media.packets.aes.object.data && !media.packets.xchacha.storage.data &&
              !media.endpoint_bytes,
          "voice media teardown clears owned state");
    SecureZeroMemory(key, sizeof(key));
}
} // namespace maicord
