#pragma once
#include "maicord/app/voice/packet_crypto.hpp"
#include "maicord/platform/net/network.hpp"
#include "maicord/protocol/voice/media_events.hpp"

namespace maicord {
struct VoiceSessionUpdate;
enum class VoiceVideoCodec : u8;

enum class VoiceMediaSetupError : u8 {
    none,
    decode,
    endpoint,
    order,
    mode,
    dave_version,
    storage,
    crypto
};
// UDP negotiation state, not an active media connection. Owned numeric address
// survives the receive document; the transport key is locked and erased. No raw
// key, token, SDP or Session Description is suitable for a UI snapshot/log.
// `endpoint` is the media server's IP address and port (family 0 until Ready is decoded).
struct VoiceMediaSetup {
    VoicePacketCrypto packets;
    NetworkAddress endpoint;
    u32 ssrc, offered_modes, selected_mode;
    u32 video_ssrc, video_rtx_ssrc; // Ready's assigned local video send SSRCs (zero: none).
    u32 keyframe_interval_ms; // Server-requested send keyframe interval; zero when unset.
    u16 dave_protocol_version;
    VoiceVideoCodec video_codec; // The call's negotiated video codec (op 4, then op 14).
    // Codec bits offered in Select Protocol; senders may use any of them.
    u32 video_offer;
    // VoiceVideoCodec of offer i (payload type 101 + 2i, RTX the next).
    u8 video_codecs[8];
    u8 video_codec_count;
    bool has_ready, has_description;
};
VoiceMediaSetupError voice_media_ready(VoiceMediaSetup* setup, const VoiceReady& ready) noexcept;
// Invoke only after a Select Protocol send is accepted. The caller is
// responsible for implementing the chosen encryptor; wire recognition alone
// does not authorize selecting or advertising a capability.
bool voice_media_protocol_selected(VoiceMediaSetup* setup, u32 mode) noexcept;
VoiceMediaSetupError voice_media_description(VoiceMediaSetup* setup, JsonValue data, Slice<u32> scratch,
                                             u32 maximum_depth, u16 advertised_dave_version,
                                             u64 ready_generation) noexcept;
// Session Update (op 14) after Description: codec renegotiation for the call.
// Fields absent from the update keep their values.
bool voice_media_session_update(VoiceMediaSetup* setup, const VoiceSessionUpdate& update) noexcept;
void voice_media_clear(VoiceMediaSetup* setup) noexcept;
} // namespace maicord
