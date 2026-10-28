#include "maicord/app/voice_connection_setup.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
namespace {
constexpr u64 kVoiceStateTimeoutMs = 30'000;

Utf8View view(const SecureBuffer& buffer, u32 offset, u32 bytes) noexcept {
    return {buffer.data ? reinterpret_cast<const char*>(buffer.data + offset) : nullptr, bytes};
}

bool advance(VoiceConnectionSetup* setup) noexcept {
    if (setup->generation == ~u64{0})
        return false;
    ++setup->generation;
    return true;
}

void clear_server(VoiceConnectionSetup* setup) noexcept {
    secure_buffer_clear(&setup->server_credentials);
    setup->token_bytes = setup->endpoint_bytes = 0;
}

bool measured(JsonString text, u32* bytes) noexcept {
    const auto result = json_string_decode_utf8(text, {});
    if ((!result.ok() && result.error != JsonStringDecodeError::output_too_small) || !result.bytes)
        return false;
    *bytes = result.bytes;
    return true;
}

bool reserve(SecureBuffer* buffer, u32 bytes) noexcept {
    if (buffer->data && buffer->capacity >= bytes)
        return true;
    SecureBuffer replacement{};
    if (!secure_buffer_initialize(&replacement, bytes).ok())
        return false;
    secure_buffer_release(buffer);
    *buffer = replacement;
    return true;
}

VoiceSetupResult evaluate(VoiceConnectionSetup* setup) noexcept {
    if (setup->has_state && setup->token_bytes && setup->endpoint_bytes) {
        setup->phase = VoiceSetupPhase::credentials_ready;
        return VoiceSetupResult::credentials_ready;
    }
    setup->phase = VoiceSetupPhase::awaiting_events;
    return VoiceSetupResult::updated;
}
} // namespace

void voice_setup_leave(VoiceConnectionSetup* setup) noexcept {
    if (!setup)
        return;
    advance(setup);
    secure_buffer_release(&setup->server_credentials);
    setup->guild_id = setup->channel_id = setup->state_deadline_ms = 0;
    setup->token_bytes = setup->endpoint_bytes = setup->flags = setup->present_flags = 0;
    setup->has_state = false;
    setup->server_id = 0;
    setup->stream = false;
    setup->stage = false;
    setup->phase = VoiceSetupPhase::idle;
}

void voice_setup_release(VoiceConnectionSetup* setup) noexcept {
    if (!setup)
        return;
    voice_setup_leave(setup);
    secure_buffer_release(&setup->login_session);
    setup->user_id = 0;
    // Preserve the generation across rebinding of this owner.
}

bool voice_setup_begin(VoiceConnectionSetup* setup, u64 guild_id, u64 channel_id, u64 now_ms) noexcept {
    if (!setup || !setup->user_id || !setup->login_session.bytes || !channel_id)
        return false;
    voice_setup_leave(setup);
    if (setup->generation == ~u64{0})
        return false;
    setup->guild_id = guild_id;
    setup->channel_id = channel_id;
    setup->state_deadline_ms = deadline_after(now_ms, kVoiceStateTimeoutMs);
    setup->phase = VoiceSetupPhase::awaiting_events;
    return true;
}

VoiceSetupResult voice_setup_bind_utf8(VoiceConnectionSetup* setup, u64 user_id, Utf8View session) noexcept {
    if (!setup) return VoiceSetupResult::invalid_input;
    voice_setup_release(setup);
    if (!user_id || !session.bytes || !session.data || !utf8_validate(session).valid)
        return VoiceSetupResult::invalid_input;
    if (!reserve(&setup->login_session, session.bytes)) return VoiceSetupResult::storage_failed;
    if (!secure_buffer_assign(&setup->login_session,
            {reinterpret_cast<const u8*>(session.data), session.bytes}).ok()) {
        voice_setup_release(setup);
        return VoiceSetupResult::storage_failed;
    }
    setup->user_id = user_id;
    return VoiceSetupResult::updated;
}

VoiceSetupResult voice_setup_state(VoiceConnectionSetup* setup, const GatewayVoiceState& event) noexcept {
    if (!setup || setup->phase == VoiceSetupPhase::idle || setup->stream || event.user_id != setup->user_id ||
        event.guild_id != setup->guild_id)
        return VoiceSetupResult::ignored;
    if (!event.channel_id ||
        !json_string_equals_utf8(event.session_id, view(setup->login_session, 0, setup->login_session.bytes))) {
        voice_setup_leave(setup);
        return VoiceSetupResult::disconnected;
    }
    if (event.channel_id != setup->channel_id)
        return VoiceSetupResult::ignored;
    setup->has_state = true;
    setup->state_deadline_ms = 0;
    setup->flags = (setup->flags & ~event.present_flags) | (event.flags & event.present_flags);
    setup->present_flags |= event.present_flags;
    return evaluate(setup);
}

namespace {
VoiceSetupResult assign_server(VoiceConnectionSetup* setup, JsonString token, JsonString endpoint,
                               bool unavailable) noexcept {
    if (unavailable) {
        if (!advance(setup)) {
            voice_setup_leave(setup);
            return VoiceSetupResult::disconnected;
        }
        clear_server(setup);
        return evaluate(setup);
    }
    if (setup->token_bytes &&
        json_string_equals_utf8(token, view(setup->server_credentials, 0, setup->token_bytes)) &&
        json_string_equals_utf8(endpoint,
                                view(setup->server_credentials, setup->token_bytes, setup->endpoint_bytes)))
        return VoiceSetupResult::ignored;
    // Invalidate the previous transport before replacing either credential.
    if (!advance(setup)) {
        voice_setup_leave(setup);
        return VoiceSetupResult::disconnected;
    }
    clear_server(setup);
    u32 token_bytes = 0, endpoint_bytes = 0;
    if (!measured(token, &token_bytes) || !measured(endpoint, &endpoint_bytes) ||
        endpoint_bytes > ~u32{0} - token_bytes) {
        voice_setup_leave(setup);
        return VoiceSetupResult::invalid_input;
    }
    const u32 bytes = token_bytes + endpoint_bytes;
    if (!reserve(&setup->server_credentials, bytes)) {
        voice_setup_leave(setup);
        return VoiceSetupResult::storage_failed;
    }
    auto* data = reinterpret_cast<char*>(setup->server_credentials.data);
    if (!json_string_decode_utf8(token, {data, token_bytes}).ok() ||
        !json_string_decode_utf8(endpoint, {data + token_bytes, endpoint_bytes}).ok()) {
        voice_setup_leave(setup);
        return VoiceSetupResult::invalid_input;
    }
    setup->server_credentials.bytes = bytes;
    setup->token_bytes = token_bytes;
    setup->endpoint_bytes = endpoint_bytes;
    return evaluate(setup);
}
} // namespace

VoiceSetupResult voice_setup_server(VoiceConnectionSetup* setup, const GatewayVoiceServer& event) noexcept {
    if (!setup || setup->phase == VoiceSetupPhase::idle || setup->stream || event.guild_id != setup->guild_id ||
        (event.channel_id && event.channel_id != setup->channel_id) ||
        (!event.guild_id && !event.channel_id))
        return VoiceSetupResult::ignored;
    return assign_server(setup, event.token, event.endpoint, event.unavailable);
}

bool voice_setup_stream_begin(VoiceConnectionSetup* setup, u64 server_id, u64 channel_id, u64 now_ms) noexcept {
    if (!setup || !setup->user_id || !setup->login_session.bytes || server_id < 2)
        return false;
    if (!channel_id)
        channel_id = server_id - 1;
    if (setup->stream && setup->phase != VoiceSetupPhase::idle && setup->server_id == server_id &&
        setup->channel_id == channel_id)
        return true;
    voice_setup_leave(setup);
    if (setup->generation == ~u64{0})
        return false;
    setup->server_id = server_id;
    setup->channel_id = channel_id;
    setup->stream = true;
    setup->has_state = true; // The stream has no voice state of its own.
    setup->state_deadline_ms = deadline_after(now_ms, kVoiceStateTimeoutMs);
    setup->phase = VoiceSetupPhase::awaiting_events;
    return true;
}

VoiceSetupResult voice_setup_stream_server(VoiceConnectionSetup* setup, JsonString token, JsonString endpoint,
                                           bool unavailable) noexcept {
    if (!setup || setup->phase == VoiceSetupPhase::idle || !setup->stream)
        return VoiceSetupResult::ignored;
    const auto result = assign_server(setup, token, endpoint, unavailable);
    if (result == VoiceSetupResult::credentials_ready)
        setup->state_deadline_ms = 0;
    return result;
}

bool voice_setup_current(const VoiceConnectionSetup& setup, u64 generation) noexcept {
    return generation && generation == setup.generation &&
           setup.phase == VoiceSetupPhase::credentials_ready;
}

bool voice_setup_credentials(const VoiceConnectionSetup& setup, VoiceSetupCredentials* output) noexcept {
    if (!output || !voice_setup_current(setup, setup.generation))
        return false;
    *output = {setup.generation,
               setup.server_id ? setup.server_id : setup.guild_id ? setup.guild_id : setup.channel_id,
               setup.channel_id,
               setup.user_id,
               view(setup.login_session, 0, setup.login_session.bytes),
               view(setup.server_credentials, 0, setup.token_bytes),
               view(setup.server_credentials, setup.token_bytes, setup.endpoint_bytes),
               setup.stream,
               setup.stage && !setup.stream};
    return true;
}

} // namespace maicord
