#include "maicord/app/voice_connection.hpp"
#include "maicord/core/byte_order.hpp"

#include <WS2tcpip.h>

namespace maicord {
namespace {
bool next(VoiceConnection* connection, VoiceConnectionSetup* setup, NetworkLoop* loop,
          u64 deadline) noexcept {
    const u64 now = GetTickCount64();
    if (now >= deadline)
        return false;
    const auto result = network_loop_poll(loop, static_cast<u32>(deadline - now));
    if (result.state != NetworkPollState::completion)
        return false;
    voice_connection_completion(connection, *setup, loop, result.completion, GetTickCount64());
    return true;
}
} // namespace
void test_voice_udp_handoff(void (*check)(bool, const char*) noexcept) noexcept {
    static VoiceConnection connection;
    static VoiceConnectionSetup setup;
    static NetworkLoop loop;
    if (!network_loop_initialize(&loop).ok()) {
        check(false, "UDP handoff loop startup");
        return;
    }
    SOCKET servers[2]{INVALID_SOCKET, INVALID_SOCKET};
    sockaddr_in addresses[2]{};
    bool opened = true;
    for (u32 i = 0; i < 2; ++i) {
        servers[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        addresses[i].sin_family = AF_INET;
        addresses[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof(sockaddr_in);
        const DWORD timeout = 2000;
        opened &= servers[i] != INVALID_SOCKET &&
                  bind(servers[i], reinterpret_cast<const sockaddr*>(&addresses[i]), size) == 0 &&
                  getsockname(servers[i], reinterpret_cast<sockaddr*>(&addresses[i]), &size) == 0 &&
                  setsockopt(servers[i], SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                             sizeof(timeout)) == 0;
    }
    check(opened, "UDP handoff local replacement peers");
    if (opened) {
        const JsonString session{utf8_literal("synthetic-session"), 0, false};
        const GatewayVoiceState state{1, 2, 3, session, 0, 0};
        const GatewayVoiceServer server{1,
                                        0,
                                        {utf8_literal("synthetic-token"), 0, false},
                                        {utf8_literal("127.0.0.1:65534"), 0, false},
                                        false};
        check(voice_setup_bind(&setup, 3, session) == VoiceSetupResult::updated &&
                  voice_setup_begin(&setup, 1, 2, GetTickCount64()),
              "UDP handoff setup fixture");
        voice_setup_server(&setup, server);
        voice_setup_state(&setup, state);
        VoiceReady ready{};
        ready.ip = {utf8_literal("127.0.0.1"), 0, false};
        ready.port = ntohs(addresses[0].sin_port);
        ready.ssrc = 7;
        ready.recognized_modes = voice_transport_aes256_gcm_rtpsize;
        check(voice_connection_ready(&connection, ready) == VoiceMediaSetupError::order &&
                  !connection.udp.socket.open,
              "UDP handoff rejects Ready before Identify submission");
        // Controller fixture at the authenticated WebSocket boundary. No TLS
        // or authentication is simulated on the wire; datagrams below are real.
        connection.socket.phase = VoiceSocketPhase::open;
        connection.socket.setup_generation = setup.generation;
        connection.attempted_generation = setup.generation;
        connection.identify_submitted = true;
        // Keep application output occupied in this controller-only WebSocket
        // fixture. Select Protocol prepare/accept is tested at that boundary;
        // only UDP discovery below actually goes over the wire.
        connection.socket.sending = VoiceSocketOutput::application;
        copy_bytes(connection.rtc_connection_id, "0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f0", kVoiceRtcConnectionIdBytes);
        check(voice_connection_ready(&connection, ready) == VoiceMediaSetupError::none,
              "UDP handoff consumes typed Ready");
        voice_connection_drive(&connection, setup, &loop, GetTickCount64());
        check(connection.udp.phase == VoiceUdpPhase::discovering &&
                  voice_connection_deadline(connection) == connection.udp.deadline_ms,
              "UDP handoff starts discovery and contributes the worker deadline");
        u8 packet[100]{};
        sockaddr_in client{};
        int client_bytes = sizeof(client);
        check(recvfrom(servers[0], reinterpret_cast<char*>(packet), sizeof(packet), 0,
                       reinterpret_cast<sockaddr*>(&client), &client_bytes) == 74 &&
                  byte_load_be32(packet + 4) == 7,
              "UDP handoff first Ready sends its own SSRC");
        const auto original_generation = connection.udp.socket.generation;
        ready.port = ntohs(addresses[1].sin_port);
        ready.ssrc = 8;
        check(voice_connection_ready(&connection, ready) == VoiceMediaSetupError::none &&
                  connection.udp.socket.open,
              "UDP replacement Ready defers socket mutation until callback return");
        voice_connection_drive(&connection, setup, &loop, GetTickCount64());
        check(connection.udp.phase == VoiceUdpPhase::closing &&
                  connection.udp_revision != connection.ready_revision &&
                  connection.udp.socket.generation == original_generation,
              "UDP replacement cannot reuse buffers while old datagrams own them");
        const u64 replacement_deadline = GetTickCount64() + 5000;
        while (connection.udp_revision != connection.ready_revision)
            if (!next(&connection, &setup, &loop, replacement_deadline))
                break;
        check(connection.udp.phase == VoiceUdpPhase::discovering &&
                  connection.udp.socket.generation > original_generation,
              "UDP drain starts replacement discovery without dispatching old completion to new owner");
        check(recvfrom(servers[1], reinterpret_cast<char*>(packet), sizeof(packet), 0,
                       reinterpret_cast<sockaddr*>(&client), &client_bytes) == 74 &&
                  byte_load_be32(packet + 4) == 8,
              "UDP replacement sends only the latest Ready endpoint/SSRC");
        packet[1] = 2;
        copy_bytes(packet + 8, "198.51.100.47", 13);
        packet[72] = 0x12;
        packet[73] = 0x34;
        check(sendto(servers[1], reinterpret_cast<const char*>(packet), 74, 0,
                     reinterpret_cast<const sockaddr*>(&client), client_bytes) == 74,
              "UDP replacement response sent");
        const u64 discovery_deadline = GetTickCount64() + 5000;
        while (connection.udp.phase == VoiceUdpPhase::discovering)
            if (!next(&connection, &setup, &loop, discovery_deadline))
                break;
        check(connection.udp.phase == VoiceUdpPhase::ready && connection.udp.external.port == 0x1234 &&
                  connection.protocol_deadline_ms && !connection.media.selected_mode,
              "UDP result waits under a negotiation deadline while WebSocket output is busy");
        u8 command_bytes[512];
        const auto command = voice_connection_protocol_command(connection, {command_bytes, sizeof(command_bytes)});
        check(command.ready_revision == connection.ready_revision && command.write.ok() &&
                  command.mode == voice_transport_aes256_gcm_rtpsize && !connection.media.selected_mode,
              "UDP discovery prepares selection without committing before send acceptance");
        check(voice_connection_protocol_submitted(&connection, command, GetTickCount64()) &&
                  connection.media.selected_mode == voice_transport_aes256_gcm_rtpsize &&
                  !voice_connection_protocol_submitted(&connection, command, GetTickCount64()),
              "only one accepted protocol command commits the offered cipher");
        ++ready.ssrc;
        check(voice_connection_ready(&connection, ready) == VoiceMediaSetupError::none,
              "UDP timeout replacement Ready");
        check(!voice_connection_protocol_submitted(&connection, command, GetTickCount64()) &&
                  !connection.protocol_deadline_ms && !connection.media.selected_mode,
              "replacement Ready rejects stale selection and clears prior negotiation deadline");
        voice_connection_drive(&connection, setup, &loop, GetTickCount64());
        voice_connection_drive(&connection, setup, &loop, connection.udp.deadline_ms);
        check(connection.error == VoiceConnectionError::datagram &&
                  connection.socket.phase == VoiceSocketPhase::closed &&
                  !voice_connection_drained(connection) && !voice_connection_deadline(connection),
              "UDP timeout stops signaling but combined shutdown waits for pending datagrams");
        const u64 drain_deadline = GetTickCount64() + 5000;
        while (!voice_connection_drained(connection))
            if (!next(&connection, &setup, &loop, drain_deadline))
                break;
        check(voice_connection_drained(connection) && !connection.media.has_ready &&
                  !connection.udp.external.address_bytes,
              "UDP and signaling teardown finish before account owner can be freed");
    }
    voice_setup_release(&setup);
    voice_connection_stop(&connection);
    for (SOCKET peer : servers)
        if (peer != INVALID_SOCKET)
            closesocket(peer);
    if (voice_connection_drained(connection))
        network_loop_release(&loop);
}
} // namespace maicord
