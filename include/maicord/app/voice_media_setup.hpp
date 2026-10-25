#pragma once
#include "maicord/platform/secure_buffer.hpp"
#include "maicord/app/voice_packet_crypto.hpp"
#include "maicord/protocol/voice_media_events.hpp"

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
// The SDK sockaddr_storage layout (checked in voice_media_setup.cpp).
struct VoiceEndpointAddress {
    alignas(8) u8 bytes[128];
};
struct VoiceMediaSetup {
    VoicePacketCrypto packets;
    VoiceEndpointAddress endpoint;
    u32 endpoint_bytes, ssrc, offered_modes, selected_mode;
    u32 keyframe_interval_ms; // Server-requested send keyframe interval; zero when unset.
    u16 dave_protocol_version;
    VoiceVideoCodec video_codec; // The call's negotiated video codec (op 4, then op 14).
    // Decodable bits offered in Select Protocol; senders may use any of them.
    u32 video_offer;
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
