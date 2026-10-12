#include "maicord/app/gateway_voice_control.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
namespace {
bool same_channel(const GatewayVoiceStateUpdate& a, const GatewayVoiceStateUpdate& b) noexcept {
    return a.channel_id == b.channel_id && (!a.channel_id || a.guild_id == b.guild_id);
}
bool same(const GatewayVoiceStateUpdate& a, const GatewayVoiceStateUpdate& b) noexcept {
    return same_channel(a, b) &&
           (!a.channel_id ||
            (a.self_mute == b.self_mute && a.self_deaf == b.self_deaf && a.self_video == b.self_video));
}
bool local_session(const GatewayVoiceControl& voice, const GatewayVoiceState& state) noexcept {
    return state.user_id == voice.setup.user_id &&
           json_string_equals_utf8(state.session_id,
                                   {reinterpret_cast<const char*>(voice.setup.login_session.data),
                                    voice.setup.login_session.bytes});
}
} // namespace

void gateway_voice_request(GatewayVoiceControl* voice, const GatewayVoiceStateUpdate& desired) noexcept {
    if (!voice)
        return;
    voice->desired = desired;
    voice->requested = true;
    voice->error = GatewayVoiceError::none;
    // Same-channel requests (mute, or a re-request after a server move) carry
    // the channel kind for the connection's DAVE policy.
    if (desired.channel_id && desired.channel_id == voice->setup.channel_id && !voice->setup.stream)
        voice->setup.stage = desired.stage;
}

bool gateway_voice_next(const GatewayVoiceControl& voice, GatewayVoiceStateUpdate* output) noexcept {
    if (!output || !voice.requested || voice.error != GatewayVoiceError::none || !voice.setup.user_id)
        return false;
    const auto& setup = voice.setup;
    const GatewayVoiceStateUpdate current{setup.guild_id, setup.channel_id, false, false, false};
    if (voice.awaiting_acknowledgement) {
        if (!voice.pending.channel_id || same_channel(voice.pending, voice.desired))
            return false;
        // Cancel the previous join before submitting another join. Its late
        // state/server events cannot satisfy a pending null-channel barrier.
        *output = {voice.pending.guild_id, 0, voice.desired.self_mute, voice.desired.self_deaf, false};
        return true;
    }
    if (voice.has_applied && same(voice.desired, voice.applied))
        return false;
    if (setup.phase == VoiceSetupPhase::awaiting_events && !same_channel(current, voice.desired)) {
        *output = {setup.guild_id, 0, voice.desired.self_mute, voice.desired.self_deaf, false};
        return true;
    }
    *output = voice.desired;
    return true;
}

bool gateway_voice_submitted(GatewayVoiceControl* voice, const GatewayVoiceStateUpdate& command,
                             u64 now_ms) noexcept {
    if (!voice)
        return false;
    GatewayVoiceStateUpdate expected{};
    if (!gateway_voice_next(*voice, &expected) || !same(command, expected))
        return false;
    auto& setup = voice->setup;
    if (!command.channel_id)
        voice_setup_leave(&setup);
    else if (setup.phase == VoiceSetupPhase::idle || command.guild_id != setup.guild_id ||
             command.channel_id != setup.channel_id) {
        if (!voice_setup_begin(&setup, command.guild_id, command.channel_id, now_ms))
            return false;
        setup.stage = command.stage;
    }
    voice->pending = command;
    voice->awaiting_acknowledgement = true;
    voice->acknowledgement_deadline_ms = deadline_after(now_ms, 30'000);
    return true;
}

void gateway_voice_cancel(GatewayVoiceControl* voice, GatewayVoiceError error) noexcept {
    if (!voice)
        return;
    voice_setup_leave(&voice->setup);
    voice->requested = voice->awaiting_acknowledgement = voice->has_applied = false;
    voice->acknowledgement_deadline_ms = 0;
    voice->desired = voice->pending = voice->applied = {};
    voice->error = error;
}

void gateway_voice_release(GatewayVoiceControl* voice) noexcept {
    if (!voice)
        return;
    gateway_voice_cancel(voice, GatewayVoiceError::none);
    voice_setup_release(&voice->setup);
}

void gateway_voice_state(GatewayVoiceControl* voice, const GatewayVoiceState& state, u64 now_ms) noexcept {
    if (!voice || !voice->requested || state.user_id != voice->setup.user_id)
        return;
    const u64 guild = voice->awaiting_acknowledgement ? voice->pending.guild_id : voice->setup.guild_id;
    if (!local_session(*voice, state)) {
        if (state.guild_id == guild)
            gateway_voice_cancel(voice, GatewayVoiceError::session_replaced);
        return;
    }
    if (voice->awaiting_acknowledgement) {
        if (state.guild_id != voice->pending.guild_id || state.channel_id != voice->pending.channel_id)
            return;
        voice->applied = voice->pending;
        voice->has_applied = true;
        voice->awaiting_acknowledgement = false;
        voice->acknowledgement_deadline_ms = 0;
    } else if (!state.channel_id) {
        if (state.guild_id == guild)
            gateway_voice_cancel(voice, GatewayVoiceError::none);
        return;
    } else if (voice->setup.phase != VoiceSetupPhase::idle &&
               (state.guild_id != voice->setup.guild_id || state.channel_id != voice->setup.channel_id)) {
        // A server-driven move is authoritative once no requested state is
        // awaiting acknowledgement. Await a fresh server credential pair.
        if (!voice_setup_begin(&voice->setup, state.guild_id, state.channel_id, now_ms)) {
            gateway_voice_cancel(voice, GatewayVoiceError::storage);
            return;
        }
        voice->desired.guild_id = state.guild_id;
        voice->desired.channel_id = state.channel_id;
        voice->applied = voice->desired;
        voice->has_applied = true;
    }
    voice_setup_state(&voice->setup, state);
}

void gateway_voice_server(GatewayVoiceControl* voice, const GatewayVoiceServer& server) noexcept {
    if (!voice || !voice->requested)
        return;
    const auto result = voice_setup_server(&voice->setup, server);
    if (result == VoiceSetupResult::invalid_input || result == VoiceSetupResult::storage_failed)
        gateway_voice_cancel(voice, result == VoiceSetupResult::storage_failed
                                        ? GatewayVoiceError::storage
                                        : GatewayVoiceError::invalid_event);
}

bool gateway_voice_expire(GatewayVoiceControl* voice, u64 now_ms) noexcept {
    if (!voice || !deadline_due(now_ms, voice->acknowledgement_deadline_ms))
        return false;
    gateway_voice_cancel(voice, GatewayVoiceError::state_timeout);
    return true;
}
} // namespace maicord
