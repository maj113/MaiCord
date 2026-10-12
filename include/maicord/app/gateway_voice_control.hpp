#pragma once

#include "maicord/app/voice_connection_setup.hpp"

namespace maicord {
enum class GatewayVoiceError : u8 {
    none,
    state_timeout,
    invalid_event,
    storage,
    session_replaced
};
// Worker-only command/acknowledgement state. The UI posts a desired value, not
// a queue of obsolete channel selections. Never copy this credential owner.
struct GatewayVoiceControl {
    VoiceConnectionSetup setup;
    GatewayVoiceStateUpdate desired, pending, applied;
    u64 acknowledgement_deadline_ms;
    GatewayVoiceError error;
    bool requested, awaiting_acknowledgement, has_applied;
};
void gateway_voice_request(GatewayVoiceControl* voice, const GatewayVoiceStateUpdate& desired) noexcept;
// Peek does no I/O or mutation. Commit only after the entire command has been
// accepted by the worker's serialized send path; do not submit a second frame
// while that send is outstanding. Leave barriers may cancel an unfinished join.
bool gateway_voice_next(const GatewayVoiceControl& voice, GatewayVoiceStateUpdate* output) noexcept;
bool gateway_voice_submitted(GatewayVoiceControl* voice, const GatewayVoiceStateUpdate& command,
                             u64 now_ms) noexcept;
void gateway_voice_state(GatewayVoiceControl* voice, const GatewayVoiceState& state, u64 now_ms) noexcept;
void gateway_voice_server(GatewayVoiceControl* voice, const GatewayVoiceServer& server) noexcept;
bool gateway_voice_expire(GatewayVoiceControl* voice, u64 now_ms) noexcept;
void gateway_voice_cancel(GatewayVoiceControl* voice, GatewayVoiceError error) noexcept;
void gateway_voice_release(GatewayVoiceControl* voice) noexcept;
// Consumes secret-bearing dispatches entirely on the worker. A malformed voice
// event cancels voice setup, never the main Gateway or its message state.
bool gateway_voice_dispatch(GatewayVoiceControl* voice, const GatewayEnvelope& envelope, Slice<u32> scratch,
                            u32 maximum_depth, u64 now_ms) noexcept;
} // namespace maicord
