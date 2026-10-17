#include "maicord/app/voice_connection.hpp"
#include "maicord/core/memory.hpp"

#include <WS2tcpip.h>

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
bool count_media(void* context, ConstSlice<u8>, const VoiceReceivedPacket&) noexcept {
    ++*static_cast<u32*>(context);
    return true;
}
struct StopConsumer {
    VoiceConnection* connection;
    u32 calls;
    bool reject;
};
bool stop_media(void* context, ConstSlice<u8>, const VoiceReceivedPacket&) noexcept {
    auto& consumer = *static_cast<StopConsumer*>(context);
    ++consumer.calls;
    if (!consumer.reject)
        voice_udp_media_stop(&consumer.connection->media_io);
    return !consumer.reject;
}
bool drain_connection(VoiceConnection& connection, const VoiceConnectionSetup& setup, NetworkLoop& loop,
                      u64 now) noexcept {
    const auto deadline = GetTickCount64() + 5000;
    while (!voice_connection_drained(connection)) {
        if (GetTickCount64() >= deadline)
            return false;
        const auto next = network_loop_poll(&loop, 100);
        if (next.state == NetworkPollState::error)
            return false;
        if (next.state == NetworkPollState::completion)
            voice_connection_completion(&connection, setup, &loop, next.completion, now);
    }
    return true;
}
bool boundary(VoiceConnection& connection, VoiceConnectionSetup& setup, NetworkLoop& loop,
              const sockaddr_in& peer, u32 modes, u64 now) noexcept {
    // Typed controller fixture after authenticated WS/UDP discovery. The UDP
    // socket is real and owned; no TLS negotiation or command send is claimed.
    connection = {};
    connection.socket.phase = VoiceSocketPhase::open;
    connection.socket.setup_generation = setup.generation;
    connection.socket.sending = VoiceSocketOutput::application;
    connection.identify_submitted = true;
    connection.attempted_generation = setup.generation;
    VoiceReady ready{};
    ready.ip = {utf8_literal("127.0.0.1"), 0, false};
    ready.port = ntohs(peer.sin_port);
    ready.ssrc = 7;
    ready.recognized_modes = modes;
    if (voice_connection_ready(&connection, ready) != VoiceMediaSetupError::none ||
        !network_datagram_open(&loop, &connection.udp.socket, reinterpret_cast<const sockaddr*>(&peer),
                               sizeof(peer))
             .ok())
        return false;
    connection.udp.phase = VoiceUdpPhase::ready;
    connection.udp_revision = connection.ready_revision;
    copy_bytes(connection.udp.external.address, "198.51.100.47", 13);
    connection.udp.external.address_bytes = 13;
    connection.udp.external.port = 4660;
    // prepare() generates this per join; the fixture skips Identify.
    copy_bytes(connection.rtc_connection_id, "0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f0", kVoiceRtcConnectionIdBytes);
    connection.rtc_connection_generation = setup.generation;
    voice_connection_drive(&connection, setup, &loop, now);
    return true;
}
void finish(Check check, VoiceConnection& connection) noexcept {
    voice_connection_stop(&connection);
    check(voice_connection_drained(connection) && !voice_connection_deadline(connection) &&
              !connection.media.packets.generation,
          "protocol teardown retires keys, sockets and deadline");
}
} // namespace

void test_voice_protocol_selection(Check check) noexcept {
    static VoiceConnection connection;
    static VoiceConnectionSetup setup;
    NetworkLoop loop{};
    if (!network_loop_initialize(&loop).ok()) {
        check(false, "protocol selection loop startup");
        return;
    }
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const SOCKET server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int bytes = sizeof(peer);
    const bool bound = server != INVALID_SOCKET &&
                       bind(server, reinterpret_cast<const sockaddr*>(&peer), bytes) == 0 &&
                       getsockname(server, reinterpret_cast<sockaddr*>(&peer), &bytes) == 0;
    check(bound, "protocol selection local UDP peer");
    if (bound) {
        const JsonString session{utf8_literal("synthetic-session"), 0, false};
        voice_setup_bind_utf8(&setup, 3, session.encoded);
        voice_setup_begin(&setup, 1, 2, 1000);
        voice_setup_server(&setup, {1,
                                    0,
                                    {utf8_literal("synthetic-token"), 0, false},
                                    {utf8_literal("127.0.0.1:65534"), 0, false},
                                    false});
        voice_setup_state(&setup, {1, 2, 3, session, 0, 0});
        constexpr auto description = utf8_literal(
            "{\"mode\":\"aead_aes256_gcm_rtpsize\",\"secret_key\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,"
            "16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31],\"dave_protocol_version\":0}");
        const JsonValue data{description, 0, JsonValueKind::object};
        u32 scratch[256];
        u8 payload[512];
        static u8 media_rx[256], media_tx[256];
        u32 media_calls = 0;
        check(boundary(connection, setup, loop, peer, 3, 1000), "protocol both-mode fixture");
        check(!voice_connection_media_start(&connection, {media_rx, 256}, {media_tx, 256}, count_media,
                                            &media_calls),
              "controller cannot begin media before accepted Description");
        const auto deadline = connection.protocol_deadline_ms;
        check(deadline == 21000 && voice_connection_deadline(connection) == deadline &&
                  !connection.media.selected_mode,
              "protocol backpressure has a finite worker deadline");
        check(voice_connection_description(&connection, data, {scratch, 256}, 16, 1001) ==
                      VoiceMediaSetupError::order &&
                  !connection.media.packets.generation && connection.protocol_deadline_ms == deadline,
              "description before accepted selection cannot install a key");
        const auto measured = voice_connection_protocol_command(connection, {});
        check(measured.ready_revision && measured.mode == voice_transport_aes256_gcm_rtpsize &&
                  measured.write.error == GatewayCommandError::output_too_small &&
                  !voice_connection_protocol_submitted(&connection, measured, 1001),
              "protocol size query prefers AES but cannot commit selection");
        const auto command = voice_connection_protocol_command(connection, {payload, sizeof(payload)});
        const auto expected = utf8_literal(
            R"({"op":1,"d":{"protocol":"udp","data":{"address":"198.51.100.47","port":4660,"mode":"aead_aes256_gcm_rtpsize"},"address":"198.51.100.47","port":4660,"mode":"aead_aes256_gcm_rtpsize","codecs":[{"name":"opus","type":"audio","priority":1000,"payload_type":120},{"name":"H264","type":"video","priority":1000,"payload_type":101,"rtx_payload_type":102,"encode":false,"decode":true}],"rtc_connection_id":"0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f0","experiments":[]}})");
        check(command.write.ok() && command.write.bytes == expected.bytes &&
                  bytes_equal(payload, expected.data, expected.bytes),
              "protocol command uses discovered external address/port instead of server endpoint");
        auto stale = command;
        ++stale.ready_revision;
        check(!voice_connection_protocol_submitted(&connection, stale, 1002) &&
                  !connection.media.selected_mode,
              "stale prepared protocol revision cannot commit");
        stale = command;
        stale.mode = voice_transport_xchacha20_poly1305_rtpsize;
        check(!voice_connection_protocol_submitted(&connection, stale, 1002),
              "protocol commit rejects a substituted mode");
        voice_connection_drive(&connection, setup, &loop, 2000);
        check(connection.protocol_deadline_ms == deadline && !connection.media.selected_mode,
              "busy protocol drive does not extend the deadline or claim a send");
        check(voice_connection_protocol_submitted(&connection, command, 2001) &&
                  !voice_connection_protocol_command(connection, {payload, sizeof(payload)}).ready_revision,
              "accepted protocol command commits once and cannot be resent");
        check(voice_connection_description(&connection, data, {scratch, 256}, 16, 2002) ==
                      VoiceMediaSetupError::none &&
                  connection.media.has_description && connection.media.packets.aes.handle &&
                  !connection.protocol_deadline_ms && !voice_connection_deadline(connection),
              "accepted description installs the selected key and retires negotiation deadline");
        check(voice_connection_description(&connection, data, {scratch, 256}, 16, 2003) ==
                  VoiceMediaSetupError::order,
              "duplicate accepted description cannot reset the key");
        const u8 media_plain[]{0x80, 0x78, 0, 1, 0, 0, 0, 1, 0, 0, 0, 7, 0xf8, 0xff, 0xfe};
        check(voice_connection_media_start(&connection, {media_rx, 256}, {media_tx, 256}, count_media,
                                           &media_calls) &&
                  voice_connection_media_send(&connection, connection.ready_revision,
                                              {media_plain, sizeof(media_plain)}) ==
                      VoiceUdpMediaSend::accepted &&
                  connection.udp.socket.send_pending && connection.udp.socket.receive_pending,
              "current Description hands its retained UDP socket to concurrent media I/O");
        voice_socket_stop(&connection.socket);
        voice_connection_drive(&connection, setup, &loop, 2004);
        check(!voice_connection_drained(connection) && !voice_udp_media_drained(connection.media_io) &&
                  !voice_connection_media_start(&connection, {media_rx, 256}, {media_tx, 256}, count_media,
                                                &media_calls),
              "ended signaling retains pending media buffers and cannot start a new consumer");
        check(drain_connection(connection, setup, loop, 2005) && media_calls == 0,
              "controller drains both media directions after key retirement without dispatching a canceled "
              "packet");
        check(!connection.identify_submitted && !connection.media.packets.generation &&
                  !connection.media.packets.aes.object.data && voice_connection_drained(connection),
              "ended signaling retires an installed media key without waiting for account release");
        finish(check, connection);

        check(boundary(connection, setup, loop, peer, 1, 2500), "media replacement fixture");
        const auto replacement_command =
            voice_connection_protocol_command(connection, {payload, sizeof(payload)});
        check(voice_connection_protocol_submitted(&connection, replacement_command, 2501) &&
                  voice_connection_description(&connection, data, {scratch, 256}, 16, 2502) ==
                      VoiceMediaSetupError::none &&
                  voice_connection_media_start(&connection, {media_rx, 256}, {media_tx, 256}, count_media,
                                               &media_calls),
              "media replacement starts an authenticated transport read");
        sockaddr_in local{};
        int local_bytes = sizeof(local);
        const auto old_socket_generation = connection.udp.socket.generation;
        const auto sealed =
            voice_packet_seal(connection.media.packets, connection.ready_revision,
                              {media_plain, sizeof(media_plain)}, {payload, sizeof(payload)});
        check(sealed.ok() &&
                  getsockname(connection.udp.socket.socket, reinterpret_cast<sockaddr*>(&local),
                              &local_bytes) == 0 &&
                  sendto(server, reinterpret_cast<const char*>(payload), sealed.bytes, 0,
                         reinterpret_cast<const sockaddr*>(&local),
                         local_bytes) == static_cast<i32>(sealed.bytes),
              "old-generation encrypted media is queued on the real socket");
        const auto queued = network_loop_poll(&loop, 2000);
        check(queued.state == NetworkPollState::completion && queued.completion.succeeded &&
                  queued.completion.kind == NetworkOperationKind::datagram_receive,
              "old media completion is observed before Ready replacement");
        VoiceReady replacement{};
        replacement.ip = {utf8_literal("127.0.0.1"), 0, false};
        replacement.port = ntohs(peer.sin_port);
        replacement.ssrc = 8;
        replacement.recognized_modes = 1;
        check(voice_connection_ready(&connection, replacement) == VoiceMediaSetupError::none,
              "Ready replacement retires the previous media key");
        if (queued.state == NetworkPollState::completion)
            voice_connection_completion(&connection, setup, &loop, queued.completion, 2503);
        bool erased = true;
        for (u8 byte : media_rx)
            erased &= byte == 0;
        check(media_calls == 0 && erased && voice_udp_media_drained(connection.media_io) &&
                  connection.udp.socket.generation > old_socket_generation &&
                  connection.udp.phase == VoiceUdpPhase::discovering &&
                  !connection.media.packets.generation,
              "old queued media is suppressed/erased before replacement discovery reuses the owner");
        voice_connection_stop(&connection);
        check(drain_connection(connection, setup, loop, 2504),
              "replacement discovery drains after media teardown");
        finish(check, connection);

        for (u32 reject = 0; reject != 2; ++reject) {
            check(boundary(connection, setup, loop, peer, 1, 2600), "media consumer stop fixture");
            const auto stop_command =
                voice_connection_protocol_command(connection, {payload, sizeof(payload)});
            StopConsumer consumer{&connection, 0, reject != 0};
            check(voice_connection_protocol_submitted(&connection, stop_command, 2601) &&
                      voice_connection_description(&connection, data, {scratch, 256}, 16, 2602) ==
                          VoiceMediaSetupError::none &&
                      voice_connection_media_start(&connection, {media_rx, 256}, {media_tx, 256},
                                                   stop_media, &consumer),
                  "media consumer stop attaches after Description");
            check(voice_connection_participant(
                      &connection, 5,
                      {utf8_literal(R"({"user_id":"42","ssrc":7,"speaking":1})"), 0, JsonValueKind::object},
                      {scratch, 256}, 64)
                      .ok(),
                  "consumer stop fixture announces its RTP source");
            const auto stop_packet =
                voice_packet_seal(connection.media.packets, connection.ready_revision,
                                  {media_plain, sizeof(media_plain)}, {payload, sizeof(payload)});
            local_bytes = sizeof(local);
            check(stop_packet.ok() &&
                      getsockname(connection.udp.socket.socket, reinterpret_cast<sockaddr*>(&local),
                                  &local_bytes) == 0 &&
                      sendto(server, reinterpret_cast<const char*>(payload), stop_packet.bytes, 0,
                             reinterpret_cast<const sockaddr*>(&local),
                             local_bytes) == static_cast<i32>(stop_packet.bytes),
                  "media consumer stop receives real encrypted datagram");
            check(drain_connection(connection, setup, loop, 2603) && consumer.calls == 1 &&
                      !connection.media.packets.generation &&
                      connection.socket.phase == VoiceSocketPhase::closed &&
                      (reject ? connection.error == VoiceConnectionError::datagram
                              : connection.error == VoiceConnectionError::none),
                  "consumer stop/rejection ends signaling and retires the key as well as UDP");
            finish(check, connection);
        }

        check(boundary(connection, setup, loop, peer, 2, 3000), "protocol XChaCha-only fixture");
        const auto xcommand = voice_connection_protocol_command(connection, {payload, sizeof(payload)});
        check(xcommand.write.ok() && xcommand.mode == voice_transport_xchacha20_poly1305_rtpsize &&
                  voice_connection_protocol_submitted(&connection, xcommand, 3001),
              "protocol chooses implemented XChaCha when AES is not offered");
        check(voice_connection_description(&connection, data, {scratch, 256}, 16, 3002) ==
                      VoiceMediaSetupError::mode &&
                  !connection.media.packets.generation,
              "server cannot change the accepted cipher in Description");
        const auto response_deadline = connection.protocol_deadline_ms;
        check(voice_connection_description(&connection, data, {scratch, 256}, 16, response_deadline) ==
                  VoiceMediaSetupError::order,
              "response at the negotiation deadline cannot install a key");
        voice_connection_drive(&connection, setup, &loop, response_deadline);
        check(connection.error == VoiceConnectionError::protocol_timeout &&
                  voice_connection_drained(connection) && !voice_connection_deadline(connection),
              "missing Description times out both transports without an idle loop");
        finish(check, connection);

        check(boundary(connection, setup, loop, peer, 1, 5000), "protocol stalled-write fixture");
        const auto pending = voice_connection_protocol_command(connection, {payload, sizeof(payload)});
        const auto send_deadline = connection.protocol_deadline_ms;
        check(!voice_connection_protocol_submitted(&connection, pending, send_deadline),
              "expired prepared send cannot commit selection");
        voice_connection_drive(&connection, setup, &loop, send_deadline);
        check(connection.error == VoiceConnectionError::protocol_timeout &&
                  voice_connection_drained(connection),
              "permanent WebSocket backpressure cannot hang protocol selection");
        finish(check, connection);

        check(boundary(connection, setup, loop, peer, 4, 6000), "protocol unknown-only fixture");
        check(connection.error == VoiceConnectionError::protocol && voice_connection_drained(connection) &&
                  !connection.media.selected_mode && !voice_connection_deadline(connection),
              "unknown-only offered modes fail without advertising a fabricated cipher");
        finish(check, connection);

        check(boundary(connection, setup, loop, peer, 1, 7000), "protocol replacement-deadline fixture");
        const auto old_deadline = connection.protocol_deadline_ms;
        voice_setup_server(&setup, {1,
                                    0,
                                    {utf8_literal("replacement-token"), 0, false},
                                    {utf8_literal("/invalid-endpoint"), 0, false},
                                    false});
        voice_connection_drive(&connection, setup, &loop, old_deadline);
        check(connection.attempted_generation == setup.generation &&
                  connection.error == VoiceConnectionError::endpoint && !connection.protocol_deadline_ms,
              "an expired old negotiation cannot prevent a fresh credential generation from starting");
        finish(check, connection);
    }
    voice_setup_release(&setup);
    voice_connection_stop(&connection);
    if (server != INVALID_SOCKET)
        closesocket(server);
    if (voice_connection_drained(connection))
        network_loop_release(&loop);
}
} // namespace maicord
