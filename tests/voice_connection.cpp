#include "maicord/app/voice_connection.hpp"

#include <WS2tcpip.h>

namespace maicord {
namespace {
JsonString text(Utf8View value) noexcept {
    return {value, 0, false};
}
bool command_equal(const VoiceConnection& connection, Utf8View expected) noexcept {
    const auto bytes = secure_buffer_view(connection.command);
    return connection.command.locked && bytes.count == expected.bytes &&
           bytes_equal(bytes.data, expected.data, bytes.count);
}
bool poll(VoiceConnection* connection, VoiceConnectionSetup* setup, NetworkLoop* loop,
          u64 deadline) noexcept {
    const auto now = GetTickCount64();
    if (now >= deadline)
        return false;
    const auto next = network_loop_poll(loop, static_cast<u32>(deadline - now));
    if (next.state != NetworkPollState::completion)
        return false;
    voice_connection_completion(connection, *setup, loop, next.completion, GetTickCount64());
    return true;
}
} // namespace
void test_voice_connection(void (*check)(bool, const char*) noexcept) noexcept {
    static VoiceConnection connection;
    static VoiceConnectionSetup setup;
    static NetworkLoop loop;
    if (!network_loop_initialize(&loop).ok()) {
        check(false, "voice handoff loop startup");
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
    check(listening, "voice handoff loopback listener");
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
    GatewayVoiceState state{10, 20, 30, session, 0, 0};
    GatewayVoiceServer server{10, 0, text(utf8_literal("test-token-one")), text({endpoint, 10 + digits}),
                              false};
    check(voice_setup_bind_utf8(&setup, 30, session.encoded) == VoiceSetupResult::updated &&
              voice_setup_begin(&setup, 10, 20, GetTickCount64()),
          "voice handoff bound setup");
    voice_setup_server(&setup, server);
    voice_connection_drive(&connection, setup, &loop, GetTickCount64());
    check(connection.socket.phase == VoiceSocketPhase::empty && !connection.command.data,
          "voice handoff waits for both server and local state");
    voice_setup_state(&setup, state);
    voice_connection_drive(&connection, setup, &loop, GetTickCount64());
    constexpr auto first =
        utf8_literal("{\"op\":0,\"d\":{\"server_id\":\"10\",\"channel_id\":\"20\",\"session_id\":"
                     "\"test-session\",\"token\":\"test-token-one\",\"user_id\":\"30\",\"max_dave_protocol_"
                     "version\":1,\"video\":true,\"streams\":[{\"type\":\"video\",\"rid\":\"100\",\"quality\":100}]}}");
    check(connection.socket.phase == VoiceSocketPhase::connecting && command_equal(connection, first),
          "voice handoff builds the v9 Identify in locked storage before connecting");
    bool uuid = connection.rtc_connection_generation == setup.generation && connection.rtc_connection_id[14] == '4';
    for (u32 i = 0; i != kVoiceRtcConnectionIdBytes; ++i) {
        const char c = connection.rtc_connection_id[i];
        uuid &= i == 8 || i == 13 || i == 18 || i == 23 ? c == '-' : (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }
    check(uuid, "voice handoff generates a lowercase version-4 RTC connection id for the join");
    const auto previous = connection.command.data;
    const auto old_generation = connection.socket.setup_generation;
    for (u32 i = 0; i < 16; ++i)
        voice_connection_drive(&connection, setup, &loop, GetTickCount64());
    check(connection.command.data == previous && connection.attempted_generation == old_generation &&
              !connection.identify_submitted,
          "voice handoff repeated ticks preserve one pending command/attempt");

    server.token = text(utf8_literal("test-token-two"));
    check(voice_setup_server(&setup, server) == VoiceSetupResult::credentials_ready &&
              setup.generation != old_generation,
          "voice handoff same endpoint new token changes generation");
    voice_connection_drive(&connection, setup, &loop, GetTickCount64());
    check(connection.socket.phase == VoiceSocketPhase::closing && !connection.command.data &&
              connection.socket.setup_generation == old_generation,
          "voice handoff erases stale Identify and waits for old resolver drain");
    const u64 deadline = GetTickCount64() + 5000;
    bool reached = true;
    while (!(connection.socket.setup_generation == setup.generation &&
             connection.socket.tls.connection.receive_pending)) {
        if (!poll(&connection, &setup, &loop, deadline)) {
            reached = false;
            break;
        }
    }
    constexpr auto second =
        utf8_literal("{\"op\":0,\"d\":{\"server_id\":\"10\",\"channel_id\":\"20\",\"session_id\":"
                     "\"test-session\",\"token\":\"test-token-two\",\"user_id\":\"30\",\"max_dave_protocol_"
                     "version\":1,\"video\":true,\"streams\":[{\"type\":\"video\",\"rid\":\"100\",\"quality\":100}]}}");
    check(reached, "voice handoff latest generation did not reach TLS receive");
    check(connection.socket.error == VoiceSocketError::none, "voice handoff latest socket failed");
    check(reached && command_equal(connection, second) &&
              connection.attempted_generation == setup.generation,
          "voice handoff drain starts only latest credentials on real TLS socket");
    server.unavailable = true;
    voice_setup_server(&setup, server);
    voice_connection_drive(&connection, setup, &loop, GetTickCount64());
    check(!connection.command.data && !connection.identify_submitted,
          "voice handoff null endpoint immediately retires pending Identify");
    const u64 drain_deadline = GetTickCount64() + 5000;
    while (!voice_connection_drained(connection))
        if (!poll(&connection, &setup, &loop, drain_deadline))
            break;
    check(voice_connection_drained(connection) && !connection.socket.storage.data,
          "voice handoff cancellation drains before packet storage release");
    if (voice_connection_drained(connection)) {
        server.unavailable = false;
        server.endpoint = text(utf8_literal("bad/endpoint"));
        voice_setup_server(&setup, server);
        voice_setup_state(&setup, state);
        for (u32 i = 0; i < 16; ++i)
            voice_connection_drive(&connection, setup, &loop, GetTickCount64());
        check(connection.error == VoiceConnectionError::endpoint && !connection.command.data &&
                  connection.attempted_generation == setup.generation &&
                  !voice_socket_deadline(connection.socket),
              "voice handoff invalid endpoint has no automatic restart or timed spin");
        server.endpoint = text({endpoint, 10 + digits});
        server.token = text(utf8_literal("test-token-three"));
        voice_setup_server(&setup, server);
        voice_connection_drive(&connection, setup, &loop, GetTickCount64());
        const u64 connect_deadline = GetTickCount64() + 5000;
        bool connected = true;
        while (!connection.socket.tls.connection.receive_pending) {
            if (!poll(&connection, &setup, &loop, connect_deadline)) {
                connected = false;
                break;
            }
        }
        check(connected, "voice handoff rejection fixture reaches TLS");
        // Close all queued loopback peers, including a previously cancelled
        // connection still in the listener backlog. No handshake credentials
        // have been sent; the failure occurs during ClientHello.
        u_long nonblocking = 1;
        if (ioctlsocket(listener, FIONBIO, &nonblocking) == 0) {
            for (;;) {
                const SOCKET peer = accept(listener, nullptr, nullptr);
                if (peer == INVALID_SOCKET)
                    break;
                closesocket(peer);
            }
        }
        const u64 failed_deadline = GetTickCount64() + 5000;
        while (!voice_connection_drained(connection))
            if (!poll(&connection, &setup, &loop, failed_deadline))
                break;
        check(voice_connection_drained(connection) && connection.error == VoiceConnectionError::socket &&
                  !connection.command.data && !connection.socket.storage.data,
              "voice handoff immediate peer failure releases unsent Identify and reports error");
    }
    voice_setup_release(&setup);
    voice_connection_stop(&connection);
    closesocket(listener);
    if (voice_connection_drained(connection))
        network_loop_release(&loop);
}
} // namespace maicord
