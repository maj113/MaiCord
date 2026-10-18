#include "maicord/app/voice_connection.hpp"
#include "maicord/core/byte_order.hpp"

#include "maicord/sys/winsock.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
struct Capture {
    VoiceConnection* connection;
    VoiceReceivedPacket packet;
    u32 calls;
    bool current, borrowed;
};
bool capture(void* context, ConstSlice<u8> bytes, const VoiceReceivedPacket& packet) noexcept {
    auto& saved = *static_cast<Capture*>(context);
    ++saved.calls;
    saved.packet = packet;
    saved.current = voice_connection_media_current(*saved.connection, packet);
    saved.borrowed = bytes.data == saved.connection->media_io.receive_buffer.data &&
                     bytes.count == packet.transport.bytes;
    return true;
}
bool announce(VoiceConnection& connection, u8 opcode, Utf8View text) noexcept {
    u32 scratch[256];
    return voice_connection_participant(&connection, opcode, {text, 0, JsonValueKind::object},
                                        {scratch, 256}, 64)
        .ok();
}
bool deliver(VoiceConnection& connection, const VoiceConnectionSetup& setup, NetworkLoop& loop,
             VoicePacketCrypto& remote, SOCKET peer, bool rtcp, u32 ssrc, bool corrupt = false) noexcept {
    u8 plain[15]{0x80, 0x78, 0, 1, 0, 0, 3, 0xc0, 0, 0, 0, 0, 0xf8, 0xff, 0xfe};
    if (rtcp) {
        plain[1] = 201;
        plain[2] = 0;
        plain[3] = 1;
        byte_store_be32(plain + 4, ssrc);
    } else
        byte_store_be32(plain + 8, ssrc);
    u8 wire[64];
    const auto sealed =
        voice_packet_seal(remote, remote.generation, {plain, rtcp ? 8u : 15u}, {wire, sizeof(wire)});
    if (sealed.ok() && corrupt)
        wire[sealed.bytes - 5] ^= 1;
    sockaddr_in address{};
    int address_bytes = sizeof(address);
    if (!sealed.ok() ||
        getsockname(connection.udp.socket.socket, reinterpret_cast<sockaddr*>(&address), &address_bytes) !=
            0 ||
        sendto(peer, reinterpret_cast<const char*>(wire), sealed.bytes, 0,
               reinterpret_cast<const sockaddr*>(&address),
               address_bytes) != static_cast<i32>(sealed.bytes))
        return false;
    const auto received = network_loop_poll(&loop, 2000);
    if (received.state != NetworkPollState::completion || !received.completion.succeeded ||
        received.completion.kind != NetworkOperationKind::datagram_receive)
        return false;
    voice_connection_completion(&connection, setup, &loop, received.completion, 1002);
    return true;
}
bool finish(VoiceConnection& connection, const VoiceConnectionSetup& setup, NetworkLoop& loop) noexcept {
    voice_connection_stop(&connection);
    const auto deadline = GetTickCount64() + 5000;
    while (!voice_connection_drained(connection) && GetTickCount64() < deadline) {
        const auto completion = network_loop_poll(&loop, 100);
        if (completion.state == NetworkPollState::completion)
            voice_connection_completion(&connection, setup, &loop, completion.completion, 1003);
    }
    return voice_connection_drained(connection);
}
} // namespace
void test_voice_media_dispatch(Check check) noexcept {
    static VoiceConnection connection;
    static VoiceConnectionSetup setup;
    static VoicePacketCrypto remote;
    static NetworkLoop loop;
    static u8 rx[256], tx[256];
    if (!network_loop_initialize(&loop).ok()) {
        check(false, "mapped media loop startup");
        return;
    }
    sockaddr_in peer_address{};
    peer_address.sin_family = AF_INET;
    peer_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto peer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int peer_bytes = sizeof(peer_address);
    bool ok = peer != INVALID_SOCKET &&
              bind(peer, reinterpret_cast<const sockaddr*>(&peer_address), peer_bytes) == 0 &&
              getsockname(peer, reinterpret_cast<sockaddr*>(&peer_address), &peer_bytes) == 0;
    check(ok, "mapped media real UDP peer");
    Capture saved{&connection};
    if (ok) {
        const JsonString session{utf8_literal("synthetic-session"), 0, false};
        voice_setup_bind_utf8(&setup, 3, session.encoded);
        voice_setup_begin(&setup, 1, 2, 1000);
        voice_setup_server(&setup, {1,
                                    0,
                                    {utf8_literal("synthetic-token"), 0, false},
                                    {utf8_literal("127.0.0.1:65534"), 0, false},
                                    false});
        voice_setup_state(&setup, {1, 2, 3, session, 0, 0});
        connection.socket.phase = VoiceSocketPhase::open;
        connection.socket.setup_generation = setup.generation;
        connection.socket.sending = VoiceSocketOutput::application;
        connection.identify_submitted = true;
        connection.attempted_generation = setup.generation;
        VoiceReady ready{};
        ready.ip = {utf8_literal("127.0.0.1"), 0, false};
        ready.ssrc = 7;
        ready.port = ntohs(peer_address.sin_port);
        ready.recognized_modes = 1;
        ok = voice_connection_ready(&connection, ready) == VoiceMediaSetupError::none &&
             network_datagram_open(&loop, &connection.udp.socket,
                                   reinterpret_cast<const sockaddr*>(&peer_address), peer_bytes)
                 .ok();
        check(ok, "mapped media retained-socket fixture");
        if (ok) {
            connection.udp.phase = VoiceUdpPhase::ready;
            connection.udp_revision = connection.ready_revision;
            copy_bytes(connection.udp.external.address, "127.0.0.1", 9);
            connection.udp.external.address_bytes = 9;
            connection.udp.external.port = 1234;
            copy_bytes(connection.rtc_connection_id, "0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f0",
                       kVoiceRtcConnectionIdBytes);
            u8 command_bytes[1024];
            const auto command =
                voice_connection_protocol_command(connection, {command_bytes, sizeof(command_bytes)});
            u32 scratch[256];
            const auto description = utf8_literal(
                "{\"mode\":\"aead_aes256_gcm_rtpsize\",\"secret_key\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,"
                "15,"
                "16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31],\"dave_protocol_version\":0}");
            u8 secret[32];
            for (u32 i = 0; i != 32; ++i)
                secret[i] = static_cast<u8>(i);
            // Only acceptance is simulated. Subsequent encrypted datagrams and
            // controller IOCP delivery below use actual connected UDP sockets.
            ok = voice_connection_protocol_submitted(&connection, command, 1001) &&
                 voice_connection_description(&connection, {description, 0, JsonValueKind::object},
                                              {scratch, 256}, 64, 1001) == VoiceMediaSetupError::none &&
                 voice_packet_initialize(&remote, connection.ready_revision,
                                         voice_transport_aes256_gcm_rtpsize, {secret, 32})
                     .ok() &&
                 voice_connection_media_start(&connection, {rx, 256}, {tx, 256}, capture, &saved);
            check(ok, "mapped media starts after accepted Description with explicit consumer");
        }
    }
    if (ok) {
        check(!voice_connection_media_start(&connection, {rx, 256}, {tx, 256}, nullptr, nullptr) &&
                  !voice_connection_media_start(&connection, {rx, 256}, {rx, 256}, capture, nullptr) &&
                  connection.media_receive == capture && connection.media_context == &saved,
              "failed media attachment cannot replace the active consumer/context");
        check(deliver(connection, setup, loop, remote, peer, false, 10) && saved.calls == 0 &&
                  connection.unannounced_rtp_packets == 1 && connection.media_io.received_packets == 1,
              "authenticated unannounced RTP is counted and dropped without creating a member");
        check(announce(connection, 5, utf8_literal(R"({"user_id":"42","ssrc":10,"speaking":1})")) &&
                  deliver(connection, setup, loop, remote, peer, false, 10) && saved.calls == 1 &&
                  saved.current && saved.borrowed && saved.packet.source.user_id == 42 &&
                  saved.packet.source.ssrc == 10 && !saved.packet.member.connected,
              "announced RTP delivers borrowed bytes and source identity without fabricating recognized "
              "membership");
        auto old = saved.packet;
        const auto accepted = saved.calls;
        check(deliver(connection, setup, loop, remote, peer, false, 10, true) && saved.calls == accepted &&
                  connection.media_io.rejected_packets == 1 && connection.unannounced_rtp_packets == 1 &&
                  voice_connection_media_current(connection, old),
              "tampered packet is rejected before participant dispatch without changing source state");
        check(announce(connection, 11, utf8_literal(R"({"user_ids":["42"]})")) &&
                  voice_connection_media_current(connection, old) &&
                  deliver(connection, setup, loop, remote, peer, false, 10) &&
                  saved.packet.member.connected,
              "identity currency is independent of changing membership snapshots");
        old = saved.packet;
        check(announce(connection, 5, utf8_literal(R"({"user_id":"42","ssrc":10,"speaking":0})")) &&
                  announce(connection, 18, utf8_literal(R"({"user_id":"42","flags":7})")) &&
                  voice_connection_media_current(connection, old),
              "activity/flags changes preserve queued source identity");
        auto substituted = old;
        ++substituted.source.revision;
        check(!voice_connection_media_current(connection, substituted),
              "substituted source revision is not current");
        substituted = old;
        ++substituted.transport.layout.ssrc;
        check(!voice_connection_media_current(connection, substituted),
              "RTP header/source identity must match");
        substituted = old;
        ++substituted.setup_generation;
        check(!voice_connection_media_current(connection, substituted),
              "stale signaling generation is not current");
        substituted = old;
        substituted.transport.error = VoicePacketError::crypto;
        check(!voice_connection_media_current(connection, substituted),
              "failed transport cannot be treated as current media");
        check(announce(connection, 5, utf8_literal(R"({"user_id":"42","ssrc":11,"speaking":1})")) &&
                  !voice_connection_media_current(connection, old),
              "audio SSRC replacement invalidates previously delivered work");
        const auto calls = saved.calls;
        check(deliver(connection, setup, loop, remote, peer, false, 10) && saved.calls == calls &&
                  connection.unannounced_rtp_packets == 2 &&
                  deliver(connection, setup, loop, remote, peer, false, 11) &&
                  saved.packet.source.ssrc == 11 && saved.current,
              "retired source is dropped and its replacement resolves normally");
        old = saved.packet;
        check(announce(connection, 13, utf8_literal(R"({"user_id":"42"})")) &&
                  !voice_connection_media_current(connection, old) &&
                  deliver(connection, setup, loop, remote, peer, false, 11) &&
                  connection.unannounced_rtp_packets == 3,
              "disconnected member's queued and newly received source packets are rejected");
        check(announce(connection, 11, utf8_literal(R"({"user_ids":["42"]})")) &&
                  announce(connection, 5, utf8_literal(R"({"user_id":"42","ssrc":11,"speaking":1})")) &&
                  !voice_connection_media_current(connection, old) &&
                  deliver(connection, setup, loop, remote, peer, false, 11) &&
                  saved.packet.source.revision != old.source.revision,
              "same user/SSRC rejoin cannot revive pre-disconnect work");
        old = saved.packet;
        check(announce(connection, 5, utf8_literal(R"({"user_id":"43","ssrc":11,"speaking":1})")) &&
                  !voice_connection_media_current(connection, old) &&
                  deliver(connection, setup, loop, remote, peer, false, 11) &&
                  saved.packet.source.user_id == 43 && !saved.packet.member.connected,
              "reassigned SSRC resolves only to the new signaling owner and invalidates old work");
        check(announce(connection, 12, utf8_literal(R"({"user_id":"43","audio_ssrc":11,"video_ssrc":20,
            "streams":[{"type":"video","ssrc":20,"rtx_ssrc":21,"active":false}]})")) &&
                  deliver(connection, setup, loop, remote, peer, false, 21) &&
                  saved.packet.source.kind == VoiceSourceKind::rtx &&
                  saved.packet.source.primary_ssrc == 20 && !saved.packet.source.active && saved.current,
              "RTX retains announced primary and activity metadata for the future media pipeline");
        const auto rtp = saved.packet;
        check(deliver(connection, setup, loop, remote, peer, true, 0) && saved.current && saved.borrowed &&
                  saved.packet.transport.layout.kind == MediaPacketKind::rtcp &&
                  !saved.packet.source.user_id && !saved.packet.member.user_id,
              "RTCP peer feedback is never mislabeled as a user media source");
        const auto rtcp = saved.packet;
        substituted = rtcp;
        substituted.source.user_id = 43;
        check(!voice_connection_media_current(connection, substituted),
              "RTCP descriptor cannot claim participant identity");
        VoiceReady replacement{};
        replacement.ip = {utf8_literal("127.0.0.1"), 0, false};
        replacement.port = ntohs(peer_address.sin_port);
        replacement.ssrc = 8;
        replacement.recognized_modes = 1;
        check(voice_connection_ready(&connection, replacement) == VoiceMediaSetupError::none &&
                  !voice_connection_media_current(connection, rtp) &&
                  !voice_connection_media_current(connection, rtcp),
              "transport Ready invalidates both queued RTP and RTCP work");
    }
    check(finish(connection, setup, loop) && !connection.media_receive && !connection.media_context &&
              !voice_connection_media_current(connection, saved.packet),
          "mapped consumer detaches before cancellation completes");
    voice_packet_release(&remote);
    voice_setup_release(&setup);
    if (peer != INVALID_SOCKET)
        closesocket(peer);
    if (voice_connection_drained(connection))
        network_loop_release(&loop);
}
} // namespace maicord
