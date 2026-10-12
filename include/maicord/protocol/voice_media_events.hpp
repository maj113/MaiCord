#pragma once
#include "maicord/protocol/gateway_events.hpp"

namespace maicord {
enum VoiceTransportModes : u32 {
    voice_transport_aes256_gcm_rtpsize = 1,
    voice_transport_xchacha20_poly1305_rtpsize = 2
};
// Recognized wire names, not a claim that an encryptor is implemented.
u32 voice_transport_mode(JsonString name) noexcept;
struct VoiceReady {
    JsonString ip;
    JsonValue modes, streams, experiments;
    u32 ssrc, recognized_modes;
    u16 port;
    bool has_streams, has_experiments;
};
struct VoiceSessionDescription {
    JsonString mode, sdp, audio_codec, video_codec, media_session_id, bandwidth_experiment;
    u32 recognized_mode, keyframe_interval_ms;
    u16 dave_protocol_version;
    bool udp, has_audio_codec, has_video_codec, has_media_session_id, has_keyframe_interval;
};
// Outputs publish only after the entire object validates. Strings/arrays borrow
// the input. Ready validates string arrays and object-array shape; stream object
// semantics belong to the video negotiation layer. Unknown modes are retained.
GatewayEventDecodeResult voice_decode_ready(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                            VoiceReady* output) noexcept;
// The optional key destination must have exactly 32 bytes and not alias input.
// UDP validates exactly 32 integers in [0,255]. Success copies into caller-owned
// protected storage; SDP success clears that destination. Failure preserves all
// outputs. The temporary key is erased on every exit and never part of an event.
GatewayEventDecodeResult voice_decode_session_description(JsonValue data, Slice<u32> scratch,
                                                          u32 maximum_depth,
                                                          VoiceSessionDescription* output,
                                                          Slice<u8> key_output = {}) noexcept;
} // namespace maicord
