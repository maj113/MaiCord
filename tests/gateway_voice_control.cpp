#include "maicord/app/gateway_voice_control.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
void test_gateway_voice_control(void (*check)(bool, const char*) noexcept) noexcept {
    GatewayClient empty_client{};
    GatewayClientVoiceSnapshot snapshot{};
    snapshot.user_id = 99;
    check(!gateway_client_request_voice_state(&empty_client, {}) &&
              !gateway_client_request_voice_state(nullptr, {}) &&
              !gateway_client_voice_snapshot(&empty_client, &snapshot) && snapshot.user_id == 99,
          "worker voice API rejects an absent account without changing observer output");
    GatewayVoiceControl voice{};
    const auto session = utf8_literal("test-session");
    const GatewayVoiceStateUpdate join{10, 20, false, false, false};
    GatewayVoiceStateUpdate command{};
    const GatewayVoiceServer server{10,
                                    0,
                                    {utf8_literal("test-token"), 0, false},
                                    {utf8_literal("voice.example.test"), 0, false},
                                    false};
    GatewayVoiceState state{10, 20, 30, {session, 0, false}, 0, gateway_voice_self_mute};
    check(voice_setup_bind_utf8(&voice.setup, 30, session) == VoiceSetupResult::updated,
          "voice controller binds decoded READY session on demand");
    check(!gateway_voice_next(voice, &command), "idle voice owner has no command or polling work");
    gateway_voice_request(&voice, join);
    check(gateway_voice_next(voice, &command) && command.channel_id == 20 &&
              gateway_voice_submitted(&voice, command, 100),
          "requested join is committed at submission");
    check(!gateway_voice_next(voice, &command), "unacknowledged join is not retransmitted");
    gateway_voice_server(&voice, server);
    check(!gateway_voice_next(voice, &command) && voice.setup.phase == VoiceSetupPhase::awaiting_events,
          "server-first credentials do not acknowledge the join command");
    gateway_voice_state(&voice, state, 110);
    check(!voice.awaiting_acknowledgement && !voice.acknowledgement_deadline_ms &&
              voice.setup.phase == VoiceSetupPhase::credentials_ready &&
              !gateway_voice_next(voice, &command),
          "local state completes rendezvous and clears command deadline");
    const auto generation = voice.setup.generation;
    auto mute = join;
    mute.self_mute = true;
    gateway_voice_request(&voice, mute);
    check(gateway_voice_next(voice, &command) && command.self_mute &&
              gateway_voice_submitted(&voice, command, 120) && voice.setup.generation == generation,
          "same-channel mute uses Opcode 4 without replacing credentials");
    auto unmute = mute;
    unmute.self_mute = false;
    gateway_voice_request(&voice, unmute);
    check(!gateway_voice_next(voice, &command), "mute changes coalesce behind their acknowledgement");
    state.flags = gateway_voice_self_mute;
    gateway_voice_state(&voice, state, 121);
    check(gateway_voice_next(voice, &command) && !command.self_mute &&
              gateway_voice_submitted(&voice, command, 122),
          "latest mute state follows the previous acknowledgement");
    state.flags = 0;
    gateway_voice_state(&voice, state, 123);

    auto move = join;
    move.channel_id = 21;
    gateway_voice_request(&voice, move);
    check(gateway_voice_next(voice, &command) && command.channel_id == 21 &&
              gateway_voice_submitted(&voice, command, 130),
          "established channel moves do not require a leave round trip");
    check(!voice_setup_current(voice.setup, generation) && !voice.setup.server_credentials.bytes,
          "move invalidates credentials before any response arrives");
    move.channel_id = 22;
    gateway_voice_request(&voice, move);
    check(gateway_voice_next(voice, &command) && !command.channel_id && command.guild_id == 10 &&
              gateway_voice_submitted(&voice, command, 131),
          "rapid channel selection cancels an unfinished join with a leave barrier");
    move.channel_id = 23;
    gateway_voice_request(&voice, move);
    check(!gateway_voice_next(voice, &command),
          "newest destination coalesces while leave acknowledgement is pending");
    state.channel_id = 21;
    gateway_voice_state(&voice, state, 132);
    gateway_voice_server(&voice, server);
    check(voice.awaiting_acknowledgement && voice.setup.phase == VoiceSetupPhase::idle &&
              !voice.setup.server_credentials.data && !gateway_voice_next(voice, &command),
          "late cancelled-join state and server events cannot satisfy the leave barrier");
    state.channel_id = 0;
    gateway_voice_state(&voice, state, 133);
    check(gateway_voice_next(voice, &command) && command.channel_id == 23 &&
              gateway_voice_submitted(&voice, command, 134),
          "leave acknowledgement releases exactly the latest destination");
    state.channel_id = 23;
    gateway_voice_state(&voice, state, 135);
    check(!voice.acknowledgement_deadline_ms && !gateway_voice_expire(&voice, 50000),
          "acknowledged state awaiting server allocation does not keep an expired timer");
    move.channel_id = 24;
    gateway_voice_request(&voice, move);
    check(gateway_voice_next(voice, &command) && !command.channel_id,
          "moving while waiting for server allocation cancels that negotiation first");
    gateway_voice_submitted(&voice, command, 50000);
    check(!gateway_voice_expire(&voice, 79999) && gateway_voice_expire(&voice, 80000) &&
              !gateway_voice_expire(&voice, 80001) && !voice.acknowledgement_deadline_ms &&
              !gateway_voice_next(voice, &command) && voice.error == GatewayVoiceError::state_timeout,
          "leave timeout terminates once without a retry or zero-deadline loop");

    gateway_voice_request(&voice, join);
    check(gateway_voice_next(voice, &command) && gateway_voice_submitted(&voice, command, 90000),
          "a new explicit request can retry a timed-out negotiation");
    state.channel_id = 20;
    state.session_id = {utf8_literal("another-device"), 0, false};
    gateway_voice_state(&voice, state, 90001);
    check(voice.error == GatewayVoiceError::session_replaced && !gateway_voice_next(voice, &command),
          "another session does not trigger automatic join fights");
    state.session_id = {session, 0, false};
    gateway_voice_request(&voice, join);
    gateway_voice_next(voice, &command);
    gateway_voice_submitted(&voice, command, 91000);
    gateway_voice_state(&voice, state, 91001);
    gateway_voice_server(&voice, server);
    state.channel_id = 25;
    gateway_voice_state(&voice, state, 91002);
    check(voice.desired.channel_id == 25 && voice.setup.channel_id == 25 &&
              voice.setup.phase == VoiceSetupPhase::awaiting_events && !voice.setup.state_deadline_ms &&
              !gateway_voice_next(voice, &command),
          "server-driven move adopts its channel and waits for fresh credentials");
    gateway_voice_server(&voice, server);
    state.channel_id = 0;
    gateway_voice_state(&voice, state, 91003);
    check(!voice.requested && voice.setup.phase == VoiceSetupPhase::idle,
          "server disconnect cancels the desired join rather than automatically rejoining");

    // Actual wire-envelope parsing feeds the production dispatcher and owner.
    u32 scratch[512]{};
    auto dispatch = [&](Utf8View json, u64 now) noexcept {
        GatewayEnvelope envelope{};
        if (!gateway_decode_envelope(json, {scratch, 512}, 32, &envelope).ok())
            return false;
        return gateway_voice_dispatch(&voice, envelope, {scratch, 512}, 32, now);
    };
    gateway_voice_request(&voice, join);
    gateway_voice_next(voice, &command);
    gateway_voice_submitted(&voice, command, 100000);
    check(
        dispatch(
            utf8_literal(
                R"({"op":0,"s":10,"t":"VOICE_SERVER_UPDATE","d":{"guild_id":"10","token":"test-token","endpoint":"voice.example.test"}})"),
            100001),
        "server dispatch is consumed on worker without a UI payload");
    check(
        dispatch(
            utf8_literal(
                R"({"op":0,"s":11,"t":"VOICE_STATE_UPDATE_BATCH","d":{"voice_states":[{"guild_id":"10","user_id":"31","channel_id":"20","session_id":"other"},{"guild_id":"10","user_id":"30","channel_id":"20","session_id":"test-session"}]}})"),
            100002) &&
            voice.setup.phase == VoiceSetupPhase::credentials_ready && !voice.awaiting_acknowledgement,
        "batched voice states route the local acknowledgement with disjoint visitor scratch");
    check(
        !dispatch(utf8_literal(R"({"op":0,"s":12,"t":"MESSAGE_DELETE","d":{"id":"1","channel_id":"20"}})"),
                  100003),
        "unrelated events remain available to the normal Gateway dispatcher");
    check(
        dispatch(
            utf8_literal(
                R"({"op":0,"s":13,"t":"VOICE_STATE_UPDATE_BATCH","d":{"voice_states":[{"guild_id":"10","user_id":"30","channel_id":"20","session_id":"test-session"},{"user_id":false}]}})"),
            100004) &&
            voice.error == GatewayVoiceError::invalid_event && voice.setup.phase == VoiceSetupPhase::idle &&
            !voice.setup.server_credentials.data,
        "malformed batch retires any partially applied voice setup");
    const Utf8View malformed[] = {
        utf8_literal(
            R"({"op":0,"s":14,"t":"VOICE_STATE_UPDATE_BATCH","d":{"voice_states":[],"voice_states":[]}})"),
        utf8_literal(R"({"op":0,"s":14,"t":"VOICE_STATE_UPDATE_BATCH","d":{}})"),
        utf8_literal(R"({"op":0,"s":14,"t":"VOICE_SERVER_UPDATE","d":{"guild_id":"10","token":false}})")};
    for (const auto json : malformed) {
        gateway_voice_request(&voice, join);
        gateway_voice_next(voice, &command);
        gateway_voice_submitted(&voice, command, 101000);
        check(dispatch(json, 101001) && voice.error == GatewayVoiceError::invalid_event && !voice.requested,
              "malformed voice payload fails voice setup without a main Gateway failure");
    }
    check(dispatch(
              utf8_literal(R"({"op":0,"s":15,"t":"VOICE_SERVER_UPDATE","d":{"token":"idle-test-secret"}})"),
              102000) &&
              !voice.setup.server_credentials.data,
          "idle voice events remain off UI queues and allocate no credentials");
    gateway_voice_release(&voice);
    check(!voice.setup.login_session.data && !voice.setup.server_credentials.data && !voice.requested,
          "worker teardown releases protected voice state");
    // Decoded-session overload must not interpret a literal backslash as JSON.
    check(voice_setup_bind_utf8(&voice.setup, 30, utf8_literal("literal\\n-é")) ==
                  VoiceSetupResult::updated &&
              voice.setup.login_session.bytes == utf8_literal("literal\\n-é").bytes &&
              bytes_equal(voice.setup.login_session.data, "literal\\n-é", voice.setup.login_session.bytes),
          "decoded READY sessions are copied without reinterpreting JSON escapes");
    gateway_voice_release(&voice);
}
} // namespace maicord
