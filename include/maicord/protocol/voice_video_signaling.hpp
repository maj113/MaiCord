#pragma once
#include "maicord/protocol/voice_gateway.hpp"
#include "maicord/protocol/voice_media_events.hpp"

namespace maicord {
// Video negotiation on the voice gateway: Select Protocol (op 1) codec list,
// Session Update (op 14) and media sink wants (op 15). Identify streams are
// written by voice_gateway_write_identify.
enum class VoiceVideoCodec : u8 {
    none,
    h264,
    h265,
    vp8,
    vp9,
    av1,
    unknown // A named codec this client does not recognize.
};
// Wire names are case-sensitive ("H264", "H265", "VP8", "VP9", "AV1").
VoiceVideoCodec voice_video_codec(JsonString name) noexcept;
// Opus is always payload type 120. Video codec i (in priority order) uses
// payload type 101 + 2i and its RTX stream the following odd type.
inline constexpr u8 kVoiceVideoPayloadTypeBase = 101;
inline constexpr u32 kVoiceVideoMaximumCodecs = 8;
inline constexpr u32 kVoiceRtcConnectionIdBytes = 36;
struct VoiceCodecOffer {
    VoiceVideoCodec codec;
    bool encode, decode;
};
struct VoiceSelectProtocolMedia {
    VoiceGatewaySelectProtocol transport;
    // Priority order; empty when video is unsupported (audio-only offer).
    ConstSlice<VoiceCodecOffer> video;
    // Lowercase 8-4-4-4-12 UUID text; one per RTC connection (call join).
    Utf8View rtc_connection_id;
};
// {"op":1,"d":{"protocol":"udp","data":{address,port,mode},address,port,mode,
//  "codecs":[...],"rtc_connection_id":"...","experiments":[]}}: the transport
// fields sit beside "data" as well. Measures before writing; any error leaves
// output unchanged; null/zero output is a size query.
GatewayCommandWriteResult voice_gateway_write_select_protocol_media(const VoiceSelectProtocolMedia& selection,
                                                                    Slice<u8> output) noexcept;
// Formats 16 random bytes as a version-4 RFC 4122 UUID (36 characters).
bool voice_rtc_connection_id(ConstSlice<u8> random, Slice<char> output) noexcept;

// Media sink wants (op 15): the quality (0-100) this client wants per remote
// SSRC, with "any" for unlisted SSRCs. 100 is full quality, 0 stops the stream.
// Entries need not be sorted; they are written in ascending SSRC order before
// "any" (JavaScript integer-key order).
struct VoiceSinkWant {
    u32 ssrc;
    u8 quality;
};
inline constexpr u8 kVoiceSinkWantsDefaultQuality = 100;
GatewayCommandWriteResult voice_gateway_write_media_sink_wants(u8 any_quality, ConstSlice<VoiceSinkWant> wants,
                                                               Slice<u8> output) noexcept;

// Session Update (op 14) from the server; every field is optional and null is
// treated as absent. Strings borrow the input. Failure preserves output.
struct VoiceSessionUpdate {
    JsonString audio_codec, video_codec, media_session_id;
    u32 keyframe_interval_ms;
    VoiceVideoCodec video;
    bool has_audio_codec, has_video_codec, has_media_session_id, has_keyframe_interval;
};
GatewayEventDecodeResult voice_decode_session_update(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                                     VoiceSessionUpdate* output) noexcept;
} // namespace maicord
