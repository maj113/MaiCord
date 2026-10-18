#include "maicord/app/voice_connection.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>
#include <WS2tcpip.h>

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

JsonString text(Utf8View value) noexcept {
    return {value, 0, false};
}
bool command_equal(const VoiceConnection& connection, Utf8View expected) noexcept {
    const auto bytes = secure_buffer_view(connection.command);
    return bytes.count == expected.bytes && bytes_equal(bytes.data, expected.data, bytes.count);
}

void speaking_cases(Check check) noexcept {
    constexpr auto expected = utf8_literal("{\"op\":5,\"d\":{\"speaking\":5,\"delay\":0,\"ssrc\":4294967295}}");
    u8 output[96];
    const auto measured = voice_gateway_write_speaking(5, 0xFFFFFFFFu, {});
    check(measured.error == GatewayCommandError::output_too_small && measured.bytes == expected.bytes,
          "speaking size query matches the wire fixture");
    memset(output, 0xCC, sizeof(output));
    const auto short_write = voice_gateway_write_speaking(5, 0xFFFFFFFFu, {output, expected.bytes - 1});
    check(short_write.error == GatewayCommandError::output_too_small && output[0] == 0xCC,
          "short speaking output is untouched");
    const auto written = voice_gateway_write_speaking(5, 0xFFFFFFFFu, {output, sizeof(output)});
    check(written.ok() && written.bytes == expected.bytes && bytes_equal(output, expected.data, expected.bytes),
          "speaking writes microphone|priority flags, zero delay and the SSRC");
    check(voice_gateway_write_speaking(0, 1, {output, sizeof(output)}).ok() &&
              bytes_equal(output, "{\"op\":5,\"d\":{\"speaking\":0,", 26),
          "speaking zero clears the indicator");
    check(voice_gateway_write_speaking(8, 1, {output, sizeof(output)}).error == GatewayCommandError::invalid_input &&
              voice_gateway_write_speaking(1, 0, {output, sizeof(output)}).error ==
                  GatewayCommandError::invalid_input,
          "speaking rejects unknown flags and a zero SSRC");
}

void close_policy_cases(Check check) noexcept {
    struct Case {
        u16 code;
        VoiceCloseAction action;
    };
    constexpr Case cases[]{{4006, VoiceCloseAction::reconnect_fresh}, {4009, VoiceCloseAction::reconnect_fresh},
                           {4015, VoiceCloseAction::resume},          {0, VoiceCloseAction::resume},
                           {1006, VoiceCloseAction::resume},          {1001, VoiceCloseAction::resume},
                           {1000, VoiceCloseAction::stop},            {4004, VoiceCloseAction::stop},
                           {4014, VoiceCloseAction::stop},            {4017, VoiceCloseAction::stop},
                           {4021, VoiceCloseAction::stop},            {4022, VoiceCloseAction::stop}};
    bool all = true;
    for (const auto& c : cases)
        all &= voice_gateway_close_action(c.code) == c.action;
    check(all, "voice close codes map to fresh, resume or stop");
}

void dave_route_cases(Check check) noexcept {
    VoiceDaveInbox inbox{};
    const u8 welcome[]{0x01, 0x02, 0xAA};
    const u8 short_welcome[]{0x01, 0x02};
    check(voice_dave_route_binary(&inbox, {{welcome, 3}, 1, 30}) == VoiceDaveRoute::deferred &&
              inbox.last_transition_id == 0x0102 && inbox.last_opcode == 30,
          "DAVE welcome is deferred with its transition ID");
    check(voice_dave_route_binary(&inbox, {{welcome, 3}, 2, 25}) == VoiceDaveRoute::deferred &&
              voice_dave_route_binary(&inbox, {{welcome, 3}, 3, 27}) == VoiceDaveRoute::deferred &&
              voice_dave_route_binary(&inbox, {{welcome, 3}, 4, 29}) == VoiceDaveRoute::deferred &&
              inbox.deferred == 4,
          "DAVE external sender, proposals and announced commit are deferred");
    check(voice_dave_route_binary(&inbox, {{short_welcome, 2}, 5, 30}) == VoiceDaveRoute::rejected &&
              voice_dave_route_binary(&inbox, {{welcome, 3}, 6, 26}) == VoiceDaveRoute::rejected &&
              voice_dave_route_binary(&inbox, {{welcome, 0}, 7, 25}) == VoiceDaveRoute::rejected &&
              inbox.rejected == 3,
          "truncated or client-only DAVE binary frames are rejected");
    check(voice_dave_route_binary(&inbox, {{welcome, 3}, 8, 5}) == VoiceDaveRoute::not_dave &&
              voice_dave_route_json(&inbox, 21) == VoiceDaveRoute::deferred &&
              voice_dave_route_json(&inbox, 24) == VoiceDaveRoute::deferred &&
              voice_dave_route_json(&inbox, 23) == VoiceDaveRoute::rejected &&
              voice_dave_route_json(&inbox, 2) == VoiceDaveRoute::not_dave,
          "DAVE JSON transitions route; non-DAVE opcodes pass through");
}

bool drain(VoiceConnection* connection, VoiceConnectionSetup* setup, NetworkLoop* loop) noexcept {
    const u64 deadline = GetTickCount64() + 5000;
    while (!voice_connection_drained(*connection)) {
        const u64 now = GetTickCount64();
        if (now >= deadline)
            return false;
        const auto next = network_loop_poll(loop, static_cast<u32>(deadline - now));
        if (next.state == NetworkPollState::completion)
            voice_connection_completion(connection, *setup, loop, next.completion, GetTickCount64());
    }
    return true;
}
// Places the connection in the state a closed, authenticated socket leaves.
void closed_session(VoiceConnection* connection, const VoiceConnectionSetup& setup, u16 code,
                    bool described) noexcept {
    connection->socket.phase = VoiceSocketPhase::closed;
    connection->socket.setup_generation = setup.generation;
    connection->socket.close_code = code;
    connection->socket.error = VoiceSocketError::none;
    connection->attempted_generation = setup.generation;
    connection->identify_submitted = true;
    connection->media.has_ready = connection->media.has_description = described;
    connection->socket.session.heartbeat.has_sequence = true;
    connection->socket.session.heartbeat.sequence = 7;
}

void reconnect_cases(Check check) noexcept {
    static VoiceConnection connection;
    static VoiceConnectionSetup setup;
    static NetworkLoop loop;
    if (!network_loop_initialize(&loop).ok()) {
        check(false, "voice reconnect loop startup");
        return;
    }
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    const bool listening = listener != INVALID_SOCKET &&
                           bind(listener, reinterpret_cast<const sockaddr*>(&address), length) == 0 &&
                           getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
                           listen(listener, 4) == 0;
    check(listening, "voice reconnect loopback listener");
    if (!listening) {
        if (listener != INVALID_SOCKET)
            closesocket(listener);
        network_loop_release(&loop);
        return;
    }
    char endpoint[32] = "127.0.0.1:", reverse[6]{};
    u32 digits = 0, port = ntohs(address.sin_port);
    do {
        reverse[digits++] = static_cast<char>('0' + port % 10);
        port /= 10;
    } while (port);
    for (u32 i = 0; i < digits; ++i)
        endpoint[10 + i] = reverse[digits - 1 - i];
    const auto session = text(utf8_literal("test-session"));
    const GatewayVoiceState state{10, 20, 30, session, 0, 0};
    const GatewayVoiceServer server{10, 0, text(utf8_literal("tok")), text({endpoint, 10 + digits}), false};
    voice_setup_bind_utf8(&setup, 30, session.encoded);
    voice_setup_begin(&setup, 10, 20, GetTickCount64());
    voice_setup_server(&setup, server);
    voice_setup_state(&setup, state);

    u64 now = GetTickCount64();
    closed_session(&connection, setup, 4015, true);
    voice_connection_drive(&connection, setup, &loop, now);
    check(connection.reconnect_pending && connection.reconnect_action == VoiceCloseAction::resume &&
              connection.reconnect_attempts == 1 && connection.media.has_description &&
              connection.socket.phase == VoiceSocketPhase::closed && !connection.command.data &&
              voice_connection_deadline(connection) == connection.reconnect_deadline_ms,
          "4015 keeps media and schedules a delayed resume");
    voice_connection_drive(&connection, setup, &loop, now + 1);
    check(connection.socket.phase == VoiceSocketPhase::closed, "resume waits for its backoff deadline");
    voice_connection_drive(&connection, setup, &loop, now + 600);
    constexpr auto resume = utf8_literal("{\"op\":7,\"d\":{\"server_id\":\"10\",\"channel_id\":\"20\",\"session_id\":"
                                         "\"test-session\",\"token\":\"tok\",\"seq_ack\":7}}");
    check(connection.resumed && !connection.reconnect_pending && connection.socket.resume &&
              connection.socket.phase == VoiceSocketPhase::connecting && command_equal(connection, resume),
          "resume reconnects with session, token and the last sequence");
    voice_connection_stop(&connection);
    check(drain(&connection, &setup, &loop) && !connection.reconnect_attempts && !connection.resumed,
          "local stop drains and forgets reconnect state");

    now = GetTickCount64();
    closed_session(&connection, setup, 4006, true);
    voice_connection_drive(&connection, setup, &loop, now);
    check(connection.reconnect_pending && connection.reconnect_action == VoiceCloseAction::reconnect_fresh &&
              !connection.media.has_description,
          "4006 clears the session and schedules a fresh connect");
    voice_connection_drive(&connection, setup, &loop, now + 600);
    constexpr auto identify =
        utf8_literal("{\"op\":0,\"d\":{\"server_id\":\"10\",\"channel_id\":\"20\",\"session_id\":\"test-session\","
                     "\"token\":\"tok\",\"user_id\":\"30\",\"max_dave_protocol_version\":1,\"video\":true,"
                     "\"streams\":[{\"type\":\"video\",\"rid\":\"100\",\"quality\":100}]}}");
    check(!connection.resumed && connection.attempted_generation == setup.generation &&
              connection.socket.phase == VoiceSocketPhase::connecting && command_equal(connection, identify),
          "fresh connect re-identifies with the same credentials");
    voice_connection_stop(&connection);
    drain(&connection, &setup, &loop);

    closed_session(&connection, setup, 4015, false);
    voice_connection_drive(&connection, setup, &loop, GetTickCount64());
    check(connection.reconnect_action == VoiceCloseAction::reconnect_fresh,
          "resume before Session Description falls back to a fresh connect");
    voice_connection_stop(&connection);
    drain(&connection, &setup, &loop);

    closed_session(&connection, setup, 4014, true);
    connection.socket.error = VoiceSocketError::none;
    voice_connection_drive(&connection, setup, &loop, GetTickCount64());
    drain(&connection, &setup, &loop);
    voice_connection_drive(&connection, setup, &loop, GetTickCount64());
    check(!connection.reconnect_pending && connection.error == VoiceConnectionError::socket &&
              connection.socket.phase == VoiceSocketPhase::closed,
          "other close codes stop without reconnecting");

    closed_session(&connection, setup, 4015, true);
    connection.error = VoiceConnectionError::none;
    connection.reconnect_attempts = kVoiceReconnectAttempts;
    voice_connection_drive(&connection, setup, &loop, GetTickCount64());
    check(!connection.reconnect_pending, "reconnect attempts are bounded");
    voice_connection_stop(&connection);
    drain(&connection, &setup, &loop);
    voice_setup_release(&setup);
    closesocket(listener);
    if (voice_connection_drained(connection))
        network_loop_release(&loop);
}
} // namespace

void test_voice_gateway_policy(Check check) noexcept {
    speaking_cases(check);
    close_policy_cases(check);
    dave_route_cases(check);
    reconnect_cases(check);
}
} // namespace maicord
