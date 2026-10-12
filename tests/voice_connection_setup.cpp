#include "maicord/app/voice_connection_setup.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
JsonString text(Utf8View value, bool escaped = false) noexcept {
    return {value, 0, escaped};
}
bool equal(Utf8View left, Utf8View right) noexcept {
    return left.bytes == right.bytes && bytes_equal(left.data, right.data, left.bytes);
}
bool zero(const SecureBuffer& buffer) noexcept {
    for (u32 i = 0; i < buffer.capacity; ++i)
        if (buffer.data[i])
            return false;
    return !buffer.bytes;
}
} // namespace

void test_voice_connection_setup(void (*check)(bool, const char*) noexcept) noexcept {
    const auto session = text(utf8_literal("local-test-session"));
    GatewayVoiceState state{10, 20, 30, session, gateway_voice_self_mute, gateway_voice_self_mute};
    GatewayVoiceServer server{10, 0, text(utf8_literal("test-token-one")),
                              text(utf8_literal("voice.example.test:443")), false};
    VoiceConnectionSetup setup{};
    VoiceSetupCredentials credentials{91, 92, 93, 94, {}, {}, {}};
    check(!voice_setup_begin(&setup, 10, 20, 1) && !voice_setup_credentials(setup, &credentials) &&
              credentials.generation == 91,
          "unbound setup cannot join or overwrite credential output");
    check(voice_setup_bind(&setup, 30, session) == VoiceSetupResult::updated && setup.login_session.locked,
          "local login session is held in locked memory");
    check(voice_setup_begin(&setup, 10, 20, 100), "guild voice setup begins");
    const auto request_generation = setup.generation;
    check(voice_setup_server(&setup, server) == VoiceSetupResult::updated &&
              !voice_setup_credentials(setup, &credentials) && setup.state_deadline_ms == 30100,
          "server-first event cannot expose credentials before local state acknowledgement");
    check(voice_setup_state(&setup, state) == VoiceSetupResult::credentials_ready &&
              voice_setup_credentials(setup, &credentials) && credentials.server_id == 10 &&
              credentials.channel_id == 20 && credentials.user_id == 30 &&
              equal(credentials.session, session.encoded) &&
              equal(credentials.token, server.token.encoded) &&
              equal(credentials.endpoint, server.endpoint.encoded) && !setup.state_deadline_ms,
          "guild rendezvous exposes owned credentials and retires the state deadline");
    auto generation = credentials.generation;
    check(!voice_setup_current(setup, request_generation) && voice_setup_current(setup, generation),
          "only the current credential generation may complete transport work");
    check(voice_setup_server(&setup, server) == VoiceSetupResult::ignored && setup.generation == generation,
          "duplicate server credentials do not churn a transport");
    auto unrelated = server;
    unrelated.guild_id = 11;
    check(voice_setup_server(&setup, unrelated) == VoiceSetupResult::ignored &&
              voice_setup_current(setup, generation),
          "unrelated guild events preserve active credentials");
    unrelated = server;
    unrelated.channel_id = 21;
    check(voice_setup_server(&setup, unrelated) == VoiceSetupResult::ignored,
          "explicit server channel must match the selected channel");
    auto other_state = state;
    other_state.user_id = 31;
    other_state.session_id = text(utf8_literal("other-session"));
    check(voice_setup_state(&setup, other_state) == VoiceSetupResult::ignored,
          "other members cannot replace the local connection");
    other_state = state;
    other_state.channel_id = 21;
    check(voice_setup_state(&setup, other_state) == VoiceSetupResult::ignored,
          "a state acknowledgement for another requested channel is ignored");
    other_state = state;
    other_state.flags = gateway_voice_self_deaf;
    other_state.present_flags = gateway_voice_self_deaf;
    check(voice_setup_state(&setup, other_state) == VoiceSetupResult::credentials_ready &&
              setup.flags == (gateway_voice_self_mute | gateway_voice_self_deaf) &&
              setup.generation == generation,
          "partial flag updates preserve omitted flags without restarting transport");

    // Equal-length replacement must reuse the allocation, erase the old pair,
    // and still invalidate callbacks even when the endpoint does not change.
    auto* allocation = setup.server_credentials.data;
    server.token = text(utf8_literal("test-token-two"));
    check(voice_setup_server(&setup, server) == VoiceSetupResult::credentials_ready &&
              setup.server_credentials.data == allocation && !voice_setup_current(setup, generation) &&
              voice_setup_credentials(setup, &credentials) &&
              equal(credentials.token, server.token.encoded),
          "same endpoint with a new token replaces credentials without another allocation");
    generation = setup.generation;
    server.endpoint = text(utf8_literal("other.example.test:8443"));
    check(voice_setup_server(&setup, server) == VoiceSetupResult::credentials_ready &&
              !voice_setup_current(setup, generation),
          "endpoint change invalidates the previous transport");
    generation = setup.generation;
    server.unavailable = true;
    check(voice_setup_server(&setup, server) == VoiceSetupResult::updated &&
              !voice_setup_current(setup, generation) && zero(setup.server_credentials) &&
              !voice_setup_credentials(setup, &credentials) && setup.has_state && !setup.state_deadline_ms,
          "null endpoint wipes the entire credential allocation and awaits reallocation without a busy "
          "deadline");
    server.unavailable = false;
    check(voice_setup_server(&setup, server) == VoiceSetupResult::credentials_ready,
          "server reallocation can reuse an acknowledged local state");

    check(voice_setup_begin(&setup, 10, 21, 200) && !setup.server_credentials.data && !setup.has_state,
          "serialized channel move clears both halves of the previous negotiation");
    state.channel_id = 21;
    check(voice_setup_state(&setup, state) == VoiceSetupResult::updated &&
              !voice_setup_credentials(setup, &credentials) && !setup.state_deadline_ms,
          "state-first event cannot reuse old server credentials");
    check(voice_setup_server(&setup, server) == VoiceSetupResult::credentials_ready,
          "state-first negotiation becomes ready when its server event arrives");
    generation = setup.generation;
    other_state = state;
    other_state.session_id = text(utf8_literal("another-device-session"));
    check(voice_setup_state(&setup, other_state) == VoiceSetupResult::disconnected &&
              !voice_setup_current(setup, generation) && !setup.server_credentials.data &&
              setup.login_session.bytes,
          "another local-user session disconnects this call without losing login identity");
    check(voice_setup_server(&setup, server) == VoiceSetupResult::ignored,
          "late server events cannot resurrect a disconnected call");

    check(voice_setup_begin(&setup, 0, 99, 1000), "private call begins with a null guild");
    server.guild_id = 0;
    server.channel_id = 98;
    check(voice_setup_server(&setup, server) == VoiceSetupResult::ignored,
          "private server event cannot attach to another private channel");
    server.channel_id = 0;
    check(voice_setup_server(&setup, server) == VoiceSetupResult::ignored,
          "private server event requires channel routing");
    server.channel_id = 99;
    state.guild_id = 0;
    state.channel_id = 99;
    check(voice_setup_state(&setup, state) == VoiceSetupResult::updated &&
              voice_setup_server(&setup, server) == VoiceSetupResult::credentials_ready &&
              voice_setup_credentials(setup, &credentials) && credentials.server_id == 99,
          "private Identify server ID is the private channel ID");
    state.channel_id = 0;
    check(voice_setup_state(&setup, state) == VoiceSetupResult::disconnected &&
              setup.phase == VoiceSetupPhase::idle,
          "local leave acknowledgement retires the call");

    voice_setup_begin(&setup, 10, 20, 500);
    server.guild_id = 10;
    server.channel_id = 0;
    check(voice_setup_server(&setup, server) == VoiceSetupResult::updated && setup.server_credentials.bytes,
          "server-first credentials remain protected while waiting for state");
    check(!voice_setup_expire(&setup, 30499) && voice_setup_expire(&setup, 30500) &&
              !voice_setup_expire(&setup, ~u64{0}) && !setup.state_deadline_ms &&
              !setup.server_credentials.data,
          "state acknowledgement timeout fires once and removes its deadline");
    voice_setup_begin(&setup, 10, 20, ~u64{0} - 10);
    check(setup.state_deadline_ms == ~u64{0} && !voice_setup_expire(&setup, ~u64{0} - 1) &&
              voice_setup_expire(&setup, ~u64{0}),
          "state deadline saturates rather than wrapping");

    // Ownership and JSON escaping: no input buffer or unescaped temporary is retained.
    char encoded_session[] = "local-\\u00e9-\\ud83d\\ude00";
    check(voice_setup_bind(&setup, 30, text({encoded_session, sizeof(encoded_session) - 1}, true)) ==
              VoiceSetupResult::updated,
          "escaped UTF-8 session binds without an unprotected decoded temporary");
    clear_bytes(encoded_session, sizeof(encoded_session));
    voice_setup_begin(&setup, 10, 20, 100);
    state = {10, 20, 30, text(utf8_literal("local-é-😀")), 0, 0};
    check(voice_setup_state(&setup, state) == VoiceSetupResult::updated,
          "session equality handles escaped Unicode after the source input is destroyed");
    char encoded_token[] = "test\\u002dtoken";
    server = {10, 0, text({encoded_token, sizeof(encoded_token) - 1}, true),
              text(utf8_literal("voice.example.test")), false};
    check(voice_setup_server(&setup, server) == VoiceSetupResult::credentials_ready,
          "escaped server credentials decode directly into protected storage");
    clear_bytes(encoded_token, sizeof(encoded_token));
    check(voice_setup_credentials(setup, &credentials) &&
              equal(credentials.token, utf8_literal("test-token")),
          "credential ownership survives reused receive buffers");
    generation = setup.generation;
    server.token = text(utf8_literal("test-token"));
    check(voice_setup_server(&setup, server) == VoiceSetupResult::ignored && setup.generation == generation,
          "equivalent escaped and unescaped credentials do not change generations");
    server.token = text(utf8_literal("\\uZZZZ"), true);
    check(voice_setup_server(&setup, server) == VoiceSetupResult::invalid_input &&
              !setup.server_credentials.data && !voice_setup_current(setup, generation),
          "invalid replacement fails closed and retires the old credentials");
    generation = setup.generation;
    check(voice_setup_bind(&setup, 0, session) == VoiceSetupResult::invalid_input &&
              !setup.login_session.data && !setup.user_id && setup.generation > generation,
          "invalid fresh binding cannot leave credentials from a previous login active");

    // Repeated lifecycles exercise release/rebind and ensure generations never reset.
    for (u32 i = 0; i < 64; ++i) {
        generation = setup.generation;
        check(voice_setup_bind(&setup, 30, session) == VoiceSetupResult::updated &&
                  setup.generation > generation,
              "rebind retains monotonic callback identity");
        voice_setup_release(&setup);
        check(!setup.login_session.data && !setup.server_credentials.data && !setup.user_id,
              "teardown releases both protected allocations");
    }
    voice_setup_bind(&setup, 30, session);
    setup.generation = ~u64{0} - 1;
    check(!voice_setup_begin(&setup, 10, 20, 0) && setup.phase == VoiceSetupPhase::idle &&
              !voice_setup_current(setup, ~u64{0}),
          "generation exhaustion cannot wrap and revive stale work");
    voice_setup_release(&setup);

    check(!json_string_equals_utf8(text(utf8_literal("\\uD800"), true), utf8_literal("x")) &&
              !json_string_equals_utf8(text(utf8_literal("a")), utf8_literal("ab")) &&
              !json_string_equals_utf8(text(utf8_literal("ab")), utf8_literal("a")) &&
              !json_string_equals_utf8(text(utf8_literal("x")), {"\xFF", 1}),
          "decoded UTF-8 equality rejects invalid input and unequal lengths");
}
} // namespace maicord
