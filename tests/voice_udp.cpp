#include "maicord/app/voice_udp_discovery.hpp"
#include "maicord/core/byte_order.hpp"

#include <Windows.h>
#include <WS2tcpip.h>

namespace maicord {
namespace {
void response(u8* packet, u16 port, Utf8View address) noexcept {
    clear_bytes(packet, 100);
    packet[1] = 2;
    packet[3] = 70;
    packet[4] = 1;
    packet[5] = 2;
    packet[6] = 3;
    packet[7] = 4;
    copy_bytes(packet + 8, address.data, address.bytes);
    packet[72] = static_cast<u8>(port >> 8);
    packet[73] = static_cast<u8>(port);
}
bool drain(VoiceUdpDiscovery* discovery, NetworkLoop* loop) noexcept {
    const u64 deadline = GetTickCount64() + 5000;
    while (!voice_udp_drained(*discovery)) {
        const u64 now = GetTickCount64();
        if (now >= deadline)
            return false;
        const auto next = network_loop_poll(loop, static_cast<u32>(deadline - now));
        if (next.state != NetworkPollState::completion)
            return false;
        voice_udp_completion(discovery, next.completion, GetTickCount64());
    }
    return true;
}
void loopback(void (*check)(bool, const char*) noexcept, i32 family) noexcept {
    // Static pending-operation owners/buffers outlive even a failed test drain.
    static NetworkLoop loops[2];
    static VoiceUdpDiscovery discoveries[2];
    static u8 pending_buffers[2][8];
    const u32 index = family == AF_INET ? 0 : 1;
    auto& loop = loops[index];
    auto& discovery = discoveries[index];
    if (!network_loop_initialize(&loop).ok()) {
        check(false, "UDP loop startup");
        return;
    }
    const SOCKET peer = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    const SOCKET stranger = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_storage address{};
    i32 address_bytes = 0;
    if (family == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(&address);
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_bytes = sizeof(*a);
    } else {
        auto* a = reinterpret_cast<sockaddr_in6*>(&address);
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_loopback;
        address_bytes = sizeof(*a);
    }
    const DWORD timeout = 2000;
    bool ok = peer != INVALID_SOCKET && stranger != INVALID_SOCKET &&
              bind(peer, reinterpret_cast<const sockaddr*>(&address), address_bytes) == 0 &&
              getsockname(peer, reinterpret_cast<sockaddr*>(&address), &address_bytes) == 0 &&
              setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                         sizeof(timeout)) == 0;
    check(ok, "UDP real IPv4/IPv6 loopback peer");
    sockaddr_storage client{};
    i32 client_bytes = sizeof(client);
    u8 received[128], packet[100];
    if (ok) {
        ok = voice_udp_begin(&discovery, &loop, reinterpret_cast<const sockaddr*>(&address), address_bytes,
                             0x01020304, GetTickCount64());
        check(ok && discovery.socket.send_pending && discovery.socket.receive_pending,
              "UDP discovery posts read and write concurrently");
        const auto generation = discovery.socket.generation;
        const SOCKET original_socket = discovery.socket.socket;
        check(!network_datagram_open(&loop, &discovery.socket, reinterpret_cast<const sockaddr*>(&address),
                                     address_bytes)
                      .ok() &&
                  discovery.socket.generation == generation,
              "UDP live owner cannot be reopened");
        const int bytes = recvfrom(peer, reinterpret_cast<char*>(received), sizeof(received), 0,
                                   reinterpret_cast<sockaddr*>(&client), &client_bytes);
        check(bytes == 74 && received[1] == 1 && received[3] == 70 &&
                  byte_load_be32(received + 4) == 0x01020304,
              "UDP discovery request has the documented on-wire header and SSRC");
        if (bytes == 74) {
            const u16 client_port =
                ntohs(family == AF_INET ? reinterpret_cast<sockaddr_in*>(&client)->sin_port
                                        : reinterpret_cast<sockaddr_in6*>(&client)->sin6_port);
            const auto valid_address = family == AF_INET ? utf8_literal("127.0.0.1") : utf8_literal("::1");
            response(packet, client_port, utf8_literal("198.51.100.23"));
            check(sendto(stranger, reinterpret_cast<const char*>(packet), 74, 0,
                         reinterpret_cast<const sockaddr*>(&client), client_bytes) == 74,
                  "UDP wrong-peer fixture sent");
            sendto(peer, "", 0, 0, reinterpret_cast<const sockaddr*>(&client), client_bytes);
            response(packet, client_port, valid_address);
            packet[7] ^= 1;
            sendto(peer, reinterpret_cast<const char*>(packet), 74, 0,
                   reinterpret_cast<const sockaddr*>(&client), client_bytes);
            response(packet, client_port, valid_address);
            sendto(peer, reinterpret_cast<const char*>(packet), 100, 0,
                   reinterpret_cast<const sockaddr*>(&client), client_bytes);
            sendto(peer, reinterpret_cast<const char*>(packet), 74, 0,
                   reinterpret_cast<const sockaddr*>(&client), client_bytes);
            const u64 deadline = GetTickCount64() + 5000;
            bool saw_empty = false, saw_truncated = false;
            while (discovery.phase == VoiceUdpPhase::discovering && GetTickCount64() < deadline) {
                const auto next = network_loop_poll(&loop, 500);
                if (next.state == NetworkPollState::completion) {
                    if (next.completion.kind == NetworkOperationKind::datagram_receive) {
                        saw_empty |= next.completion.succeeded && next.completion.transferred == 0;
                        saw_truncated |= !next.completion.succeeded;
                    }
                    voice_udp_completion(&discovery, next.completion, GetTickCount64());
                }
                voice_udp_tick(&discovery, GetTickCount64());
            }
            check(discovery.phase == VoiceUdpPhase::ready && discovery.external.port == client_port &&
                      discovery.external.address_bytes == valid_address.bytes &&
                      bytes_equal(discovery.external.address, valid_address.data, valid_address.bytes) &&
                      saw_empty && saw_truncated && discovery.socket.open &&
                      discovery.socket.socket == original_socket && !discovery.socket.send_pending &&
                      !discovery.socket.receive_pending && !discovery.deadline_ms,
                  "UDP discovery ignores empty/truncated/stale-SSRC/wrong-peer packets and retains its NAT "
                  "socket");
            // A zero-byte SEND is valid too; its completion must not close UDP.
            if (discovery.phase == VoiceUdpPhase::ready) {
                check(network_datagram_send(&discovery.socket, {}).ok(), "UDP supports zero-length send");
                const int empty = recvfrom(peer, reinterpret_cast<char*>(received), sizeof(received), 0,
                                           reinterpret_cast<sockaddr*>(&client), &client_bytes);
                check(empty == 0, "UDP peer receives one empty datagram");
                const auto next = network_loop_poll(&loop, 2000);
                check(next.state == NetworkPollState::completion &&
                          next.completion.kind == NetworkOperationKind::datagram_send &&
                          next.completion.succeeded && !discovery.socket.send_pending &&
                          discovery.socket.open,
                      "UDP zero-length send completion keeps peer association open");
            }
        }
        if (discovery.socket.open && !discovery.socket.receive_pending)
            check(network_datagram_receive(&discovery.socket, {pending_buffers[index], 8}).ok(),
                  "UDP cancellation read posted");
        voice_udp_close(&discovery);
        check(!network_datagram_open(&loop, &discovery.socket, reinterpret_cast<const sockaddr*>(&address),
                                     address_bytes)
                   .ok(),
              "UDP closed owner cannot be reused before cancellation drains");
        check(drain(&discovery, &loop) && discovery.phase == VoiceUdpPhase::closed &&
                  !discovery.external.address_bytes,
              "UDP pending cancellation drains and retires discovered address");
        if (voice_udp_drained(discovery)) {
            check(voice_udp_begin(&discovery, &loop, reinterpret_cast<const sockaddr*>(&address),
                                  address_bytes, 0x01020304, GetTickCount64()) &&
                      discovery.socket.generation > generation,
                  "UDP drained socket owner reopens with a new generation");
            voice_udp_tick(&discovery, discovery.deadline_ms);
            check(discovery.error == VoiceUdpError::timeout && !discovery.deadline_ms &&
                      drain(&discovery, &loop),
                  "UDP discovery deadline cancels both directions and never spins");
            // Consume the request from that cancelled attempt so the next
            // fixture's response targets the newly allocated local UDP port.
            recvfrom(peer, reinterpret_cast<char*>(received), sizeof(received), 0,
                     reinterpret_cast<sockaddr*>(&client), &client_bytes);
            check(voice_udp_begin(&discovery, &loop, reinterpret_cast<const sockaddr*>(&address),
                                  address_bytes, 0x01020304, GetTickCount64()),
                  "UDP late-response attempt starts");
            const auto request_bytes = recvfrom(peer, reinterpret_cast<char*>(received), sizeof(received),
                                                0, reinterpret_cast<sockaddr*>(&client), &client_bytes);
            check(request_bytes == 74, "UDP late-response request received");
            response(packet, 12345, family == AF_INET ? utf8_literal("127.0.0.1") : utf8_literal("::1"));
            sendto(peer, reinterpret_cast<const char*>(packet), 74, 0,
                   reinterpret_cast<const sockaddr*>(&client), client_bytes);
            bool observed = false;
            const auto observed_deadline = GetTickCount64() + 2000;
            while (GetTickCount64() < observed_deadline && !observed) {
                const auto next = network_loop_poll(&loop, 100);
                if (next.state != NetworkPollState::completion)
                    continue;
                observed = next.completion.kind == NetworkOperationKind::datagram_receive &&
                           next.completion.succeeded;
                voice_udp_completion(&discovery, next.completion,
                                     observed ? discovery.deadline_ms : GetTickCount64());
            }
            check(observed && discovery.error == VoiceUdpError::timeout && !discovery.received &&
                      drain(&discovery, &loop),
                  "UDP valid response at the deadline cannot revive an expired attempt");
        }
    }
    voice_udp_close(&discovery);
    if (peer != INVALID_SOCKET)
        closesocket(peer);
    if (stranger != INVALID_SOCKET)
        closesocket(stranger);
    if (drain(&discovery, &loop))
        network_loop_release(&loop);
}
} // namespace
void test_voice_udp(void (*check)(bool, const char*) noexcept) noexcept {
    u8 packet[100];
    for (u32 capacity = 0; capacity < 74; ++capacity) {
        memset(packet, 0xa5, sizeof(packet));
        bool unchanged = !voice_discovery_request(0x01020304, {packet, capacity});
        for (u8 value : packet)
            unchanged &= value == 0xa5;
        check(unchanged, "UDP discovery short request output is atomic");
    }
    voice_discovery_request(0x01020304, {packet, 100});
    check(packet[0] == 0 && packet[1] == 1 && packet[2] == 0 && packet[3] == 70 && packet[4] == 1 &&
              packet[7] == 4 && packet[74] == 0xa5,
          "UDP discovery exact request bytes");
    response(packet, 65535, utf8_literal("2001:db8::1"));
    VoiceDiscoveryAddress output{};
    check(voice_discovery_response({packet, 74}, 0x01020304, &output) && output.port == 65535 &&
              output.address_bytes == 11,
          "UDP discovery response decodes big-endian port and owned address");
    for (u32 size = 0; size <= 100; ++size) {
        if (size == 74)
            continue;
        output.port = 123;
        check(!voice_discovery_response({packet, size}, 0x01020304, &output) && output.port == 123,
              "UDP discovery requires exact datagram size and preserves output on error");
    }
    check(!voice_discovery_response({packet, 74}, 99, &output), "UDP discovery rejects another SSRC");
    packet[1] = 1;
    check(!voice_discovery_response({packet, 74}, 0x01020304, &output),
          "UDP discovery rejects request as response");
    packet[1] = 2;
    packet[3] = 69;
    check(!voice_discovery_response({packet, 74}, 0x01020304, &output),
          "UDP discovery validates declared body length");
    packet[3] = 70;
    packet[8] = 0;
    check(!voice_discovery_response({packet, 74}, 0x01020304, &output),
          "UDP discovery rejects empty address");
    response(packet, 0, utf8_literal("127.0.0.1"));
    check(!voice_discovery_response({packet, 74}, 0x01020304, &output), "UDP discovery rejects port zero");
    packet[73] = 1;
    memset(packet + 8, 'a', 64);
    check(!voice_discovery_response({packet, 74}, 0x01020304, &output),
          "UDP discovery requires bounded address terminator");
    loopback(check, AF_INET);
    loopback(check, AF_INET6);
}
} // namespace maicord
