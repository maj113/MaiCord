#pragma once

#include "maicord/platform/secure_buffer.hpp"
#include "maicord/protocol/gateway_voice.hpp"

namespace maicord {

enum class VoiceSetupPhase : u8 {
    idle,
    awaiting_events,
    credentials_ready
};
enum class VoiceSetupResult : u8 {
    ignored,
    updated,
    credentials_ready,
    disconnected,
    invalid_input,
    storage_failed
};

// One account worker owns this object. Do not copy it or send it to the UI.
// The login session and server credentials live only in locked, erased memory.
struct VoiceConnectionSetup {
    SecureBuffer login_session;
    SecureBuffer server_credentials;
    u64 user_id, guild_id, channel_id;
    u64 generation, state_deadline_ms;
    u32 token_bytes, endpoint_bytes;
    u32 flags, present_flags;
    VoiceSetupPhase phase;
    bool has_state;
    // Go Live: a stream connection identifies with the stream's media server
    // (rtc_server_id) instead of the guild/call, and requests a screen stream.
    u64 server_id;
    bool stream;
    // Stage channel: DAVE stays optional (protocol version 0 is accepted at
    // Session Description); every other call requires DAVE.
    bool stage;
};

// Fresh READY binding invalidates the previous call. Resuming the same Gateway
// session must not call bind. A zero-initialized owner requires release at teardown.
VoiceSetupResult voice_setup_bind(VoiceConnectionSetup* setup, u64 user_id, JsonString session) noexcept;
VoiceSetupResult voice_setup_bind_utf8(VoiceConnectionSetup* setup, u64 user_id, Utf8View session) noexcept;
// Call only for a serialized, submitted Opcode 4 join/move request. Mute/deafen
// changes in the same channel do not begin another negotiation. The Gateway has no
// request ID for guild server events; generation tags cannot disambiguate
// overlapping on-wire joins. The worker must serialize those requests. Incoming
// event strings must borrow the receive document, not this owner's storage.
bool voice_setup_begin(VoiceConnectionSetup* setup, u64 guild_id, u64 channel_id, u64 now_ms) noexcept;
void voice_setup_leave(VoiceConnectionSetup* setup) noexcept;
void voice_setup_release(VoiceConnectionSetup* setup) noexcept;
VoiceSetupResult voice_setup_state(VoiceConnectionSetup* setup, const GatewayVoiceState& event) noexcept;
VoiceSetupResult voice_setup_server(VoiceConnectionSetup* setup, const GatewayVoiceServer& event) noexcept;
bool voice_setup_expire(VoiceConnectionSetup* setup, u64 now_ms) noexcept;
// Go Live watch: the stream's voice server credentials arrive as STREAM_CREATE
// (rtc_server_id, rtc_channel_id) and STREAM_SERVER_UPDATE (token, endpoint).
// No voice state is involved; the bound login session identifies the viewer.
// A zero channel uses rtc_server_id - 1, as the official client does. Begin
// (or a changed media server) starts a new generation; repeated identical
// calls are ignored. The server deadline is the usual 30 s rendezvous.
bool voice_setup_stream_begin(VoiceConnectionSetup* setup, u64 server_id, u64 channel_id, u64 now_ms) noexcept;
VoiceSetupResult voice_setup_stream_server(VoiceConnectionSetup* setup, JsonString token, JsonString endpoint,
                                           bool unavailable) noexcept;
bool voice_setup_current(const VoiceConnectionSetup& setup, u64 generation) noexcept;

struct VoiceSetupCredentials {
    u64 generation, server_id, channel_id, user_id;
    Utf8View session, token, endpoint;
    bool stream; // Go Live viewer connection (screen stream parameters).
    bool stage;  // Stage channel call (DAVE optional).
};
// Views expire on the next mutating call. Consume them on the owning worker;
// async transport completions carry only generation, never borrowed pointers.
bool voice_setup_credentials(const VoiceConnectionSetup& setup, VoiceSetupCredentials* output) noexcept;

} // namespace maicord
