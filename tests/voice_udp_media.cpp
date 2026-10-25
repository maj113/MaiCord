#include "maicord/app/voice_packet_crypto.hpp"
#include "maicord/app/voice_udp_media.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/winsock.hpp"
#include "test_output.hpp"
#include "maicord/protocol/voice_media_events.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/voice_packet_vectors.hpp"
bool zero(const u8* bytes, u32 count) noexcept {
    for (u32 i = 0; i != count; ++i)
        if (bytes[i])
            return false;
    return true;
}
struct Consumer {
    Check check;
    VoiceUdpMedia* media;
    ConstSlice<u8> expected;
    u32 counter, calls;
    bool stop, reject;
};
bool receive(void* context, ConstSlice<u8> packet, const VoicePacketResult& result) noexcept {
    auto& consumer = *static_cast<Consumer*>(context);
    ++consumer.calls;
    consumer.check(result.ok() && result.nonce == consumer.counter &&
                       packet.count == consumer.expected.count &&
                       bytes_equal(packet.data, consumer.expected.data, packet.count),
                   "UDP media dispatch contains exactly the authenticated independent fixture");
    if (consumer.stop) {
        voice_udp_media_stop(consumer.media);
        consumer.check(!voice_udp_media_drained(*consumer.media) &&
                           bytes_equal(packet.data, consumer.expected.data, packet.count),
                       "UDP callback stop defers retirement until borrowed plaintext is returned");
    }
    return !consumer.reject;
}
void deliver(VoiceUdpMedia& media, VoiceUdpDiscovery& discovery,
             const NetworkCompletion& completion) noexcept {
    voice_udp_media_completion(&media, completion);
    voice_udp_completion(&discovery, completion, GetTickCount64());
}
bool drain(VoiceUdpMedia& media, VoiceUdpDiscovery& discovery, NetworkLoop& loop) noexcept {
    voice_udp_media_stop(&media);
    voice_udp_close(&discovery);
    const auto deadline = GetTickCount64() + 5000;
    while (!voice_udp_drained(discovery) || !voice_udp_media_drained(media)) {
        if (GetTickCount64() >= deadline)
            return false;
        const auto next = network_loop_poll(&loop, 100);
        if (next.state == NetworkPollState::error)
            return false;
        if (next.state == NetworkPollState::completion)
            deliver(media, discovery, next.completion);
    }
    return true;
}
bool until(VoiceUdpMedia& media, VoiceUdpDiscovery& discovery, NetworkLoop& loop, u64 receives, u64 sends,
           u64 rejected) noexcept {
    const auto deadline = GetTickCount64() + 5000;
    while (media.received_packets < receives || media.sent_packets < sends ||
           media.rejected_packets < rejected) {
        if (!media.discovery || media.stopping || GetTickCount64() >= deadline)
            return false;
        const auto next = network_loop_poll(&loop, 100);
        if (next.state == NetworkPollState::error)
            return false;
        if (next.state == NetworkPollState::completion)
            deliver(media, discovery, next.completion);
    }
    return true;
}
void loopback(Check check, u32 index, i32 family, u32 mode, XChaChaBackend backend) noexcept {
    // Pending owners and buffers remain alive even on a failed drain assertion.
    static NetworkLoop loops[6];
    static VoiceUdpDiscovery discoveries[6];
    static VoiceUdpMedia transports[6];
    static VoicePacketCrypto keys[6];
    static u8 buffers[6][512];
    auto& loop = loops[index];
    auto& discovery = discoveries[index];
    auto& media = transports[index];
    auto& key = keys[index];
    u8* rx = buffers[index];
    u8* tx = rx + 256;
    // AES runs post three concurrent reads (85-byte slots); others keep one.
    const u32 slots = index % 3 == 0 ? 3 : 1;
    if (!network_loop_initialize(&loop).ok()) {
        check(false, "UDP media loop startup");
        return;
    }
    sockaddr_storage address{};
    i32 address_bytes;
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
    const auto peer = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    const DWORD timeout = 2000;
    bool ok = peer != INVALID_SOCKET &&
              bind(peer, reinterpret_cast<const sockaddr*>(&address), address_bytes) == 0 &&
              getsockname(peer, reinterpret_cast<sockaddr*>(&address), &address_bytes) == 0 &&
              setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                         sizeof(timeout)) == 0;
    check(ok, "UDP media IPv4/IPv6 peer binding");
    u8 secret[32], plain[128], wire[512], expected[256];
    for (u32 i = 0; i != 32; ++i)
        secret[i] = static_cast<u8>(i);
    Consumer consumer{check, &media};
    sockaddr_storage client{};
    i32 client_bytes = sizeof(client);
    if (ok) {
        ok = voice_packet_initialize(&key, 9, mode, {secret, 32}, backend).ok() &&
             voice_udp_begin(&discovery, &loop, reinterpret_cast<const sockaddr*>(&address), address_bytes,
                             0x01020304, GetTickCount64());
        check(ok, "UDP media begins with a real discovery and negotiated cipher");
    }
    if (ok) {
        const SOCKET original = discovery.socket.socket;
        check(!voice_udp_media_begin(&media, &discovery, &key, 9, {rx, 256}, {tx, 256}, receive, &consumer),
              "UDP media cannot overtake pending discovery");
        const auto bytes = recvfrom(peer, reinterpret_cast<char*>(wire), sizeof(wire), 0,
                                    reinterpret_cast<sockaddr*>(&client), &client_bytes);
        ok = bytes == 74;
        check(ok, "UDP media peer received discovery");
        if (ok) {
            clear_bytes(wire, 74);
            wire[1] = 2;
            wire[3] = 70;
            byte_store_be32(wire + 4, 0x01020304);
            const auto ip = family == AF_INET ? utf8_literal("127.0.0.1") : utf8_literal("::1");
            copy_bytes(wire + 8, ip.data, ip.bytes);
            wire[72] = static_cast<u8>(4567 >> 8);
            wire[73] = static_cast<u8>(4567);
            check(sendto(peer, reinterpret_cast<const char*>(wire), 74, 0,
                         reinterpret_cast<const sockaddr*>(&client), client_bytes) == 74,
                  "UDP media peer replies to discovery");
            const auto deadline = GetTickCount64() + 5000;
            while (discovery.phase == VoiceUdpPhase::discovering && GetTickCount64() < deadline) {
                const auto next = network_loop_poll(&loop, 100);
                if (next.state == NetworkPollState::completion)
                    voice_udp_completion(&discovery, next.completion, GetTickCount64());
                voice_udp_tick(&discovery, GetTickCount64());
            }
            ok = discovery.phase == VoiceUdpPhase::ready;
            check(ok, "UDP media discovery completes");
        }
        if (ok) {
            check(!voice_udp_media_begin(&media, &discovery, &key, 8, {rx, 256}, {tx, 256}, receive,
                                         &consumer) &&
                      !voice_udp_media_begin(&media, &discovery, &key, 9, {rx, 256}, {rx + 128, 256},
                                             receive, &consumer) &&
                      !voice_udp_media_begin(&media, &discovery, &key, 9, {rx, 27}, {tx, 256}, receive,
                                             &consumer) &&
                      !voice_udp_media_begin(&media, &discovery, &key, 9, {rx, 256}, {tx, 256}, nullptr,
                                             &consumer) &&
                      !voice_udp_media_begin(&media, &discovery, &key, 9,
                                             {reinterpret_cast<u8*>(&discovery), sizeof(discovery)},
                                             {tx, 256}, receive, &consumer),
                  "UDP media rejects stale keys, buffer overlap/owner aliases, short buffers and absent "
                  "consumer");
            check(!voice_udp_media_begin(&media, &discovery, &key, 9, {rx, 256}, {tx, 256}, receive,
                                         &consumer, kVoiceUdpMediaReceiveSlots + 1) &&
                      !voice_udp_media_begin(&media, &discovery, &key, 9, {rx, 200}, {tx, 256}, receive,
                                             &consumer, kVoiceUdpMediaReceiveSlots),
                  "UDP media rejects too many receive slots and slots under 28 bytes");
            ok = voice_udp_media_begin(&media, &discovery, &key, 9, {rx, 256}, {tx, 256}, receive,
                                       &consumer, slots);
            check(ok && discovery.socket.receive_pending && discovery.socket.socket == original &&
                      discovery.socket.extra_receive_pending == (slots == 3 ? 3 : 0),
                  "UDP media retains discovery socket/NAT binding and posts every receive slot");
        }
        if (ok) {
            check(!voice_udp_media_begin(&media, &discovery, &key, 9, {rx, 256}, {tx, 256}, receive,
                                         &consumer),
                  "UDP media cannot be reinitialized over pending I/O");
            u32 ordinal = 0;
            for (const auto& vector : packet_vectors) {
                const u32 plain_bytes = test_unhex(vector.plain, plain);
                const u32 wire_bytes = test_unhex(
                    mode == voice_transport_aes256_gcm_rtpsize ? vector.aes : vector.xchacha, expected);
                consumer.expected = {plain, plain_bytes};
                consumer.counter = vector.counter;
                // Synthetic counters are set to the independent oracle values.
                key.next_nonce = vector.counter;
                check(voice_udp_media_send(&media, 8, {plain, plain_bytes}) == VoiceUdpMediaSend::invalid &&
                          voice_udp_media_send(&media, 9, {rx, plain_bytes}) ==
                              VoiceUdpMediaSend::invalid &&
                          key.next_nonce == vector.counter,
                      "UDP media stale/aliased send cannot consume nonce or pending read storage");
                check(
                    voice_udp_media_send(&media, 9, {plain, plain_bytes}) == VoiceUdpMediaSend::accepted &&
                        discovery.socket.receive_pending && discovery.socket.send_pending &&
                        voice_udp_media_send(&media, 9, {plain, plain_bytes}) == VoiceUdpMediaSend::busy &&
                        key.next_nonce == static_cast<u64>(vector.counter) + 1,
                    "UDP media read/write overlap and busy send consumes no nonce");
                plain[plain_bytes - 1] ^= 0x55; // Accepted send no longer borrows input.
                const auto received = recvfrom(peer, reinterpret_cast<char*>(wire), sizeof(wire), 0,
                                               reinterpret_cast<sockaddr*>(&client), &client_bytes);
                check(received == static_cast<i32>(wire_bytes) && bytes_equal(wire, expected, wire_bytes),
                      "UDP media sends independent oracle ciphertext/tag/nonce as a single datagram");
                plain[plain_bytes - 1] ^= 0x55;
                // Feed independently generated inbound bytes, not only our own output.
                check(sendto(peer, reinterpret_cast<const char*>(expected), wire_bytes, 0,
                             reinterpret_cast<const sockaddr*>(&client),
                             client_bytes) == static_cast<i32>(wire_bytes),
                      "UDP media independent inbound fixture sent");
                ++ordinal;
                ok = until(media, discovery, loop, ordinal, ordinal, 0);
                check(ok && consumer.calls == ordinal && zero(rx, wire_bytes) && zero(tx, wire_bytes) &&
                          discovery.socket.receive_pending && !discovery.socket.send_pending,
                      "UDP media dispatches then erases used buffers and rearms without a timer");
                if (!ok)
                    break;
            }
        }
        if (ok && slots > 1) {
            // A burst arrives while several reads are posted; each slot rearms.
            const auto& vector = packet_vectors[0];
            const u32 wire_bytes =
                test_unhex(mode == voice_transport_aes256_gcm_rtpsize ? vector.aes : vector.xchacha, expected);
            consumer.expected = {plain, test_unhex(vector.plain, plain)};
            consumer.counter = vector.counter;
            const u64 previous = media.received_packets;
            for (u32 i = 0; i != 8; ++i)
                sendto(peer, reinterpret_cast<const char*>(expected), wire_bytes, 0,
                       reinterpret_cast<const sockaddr*>(&client), client_bytes);
            ok = until(media, discovery, loop, previous + 8, media.sent_packets, media.rejected_packets);
            check(ok && discovery.socket.receive_pending && discovery.socket.extra_receive_pending == 3 &&
                      zero(rx, 256),
                  "UDP media drains a datagram burst through concurrent receive slots");
            // Later checks pair receive and send counts; the burst had no sends.
            media.received_packets -= 8;
            consumer.calls -= 8;
        }
        if (ok) {
            const auto& vector = packet_vectors[0];
            const auto wire_bytes =
                test_unhex(mode == voice_transport_aes256_gcm_rtpsize ? vector.aes : vector.xchacha, expected);
            consumer.expected = {plain, test_unhex(vector.plain, plain)};
            consumer.counter = vector.counter;
            const u32 previous = consumer.calls;
            expected[wire_bytes - 5] ^= 1;
            sendto(peer, reinterpret_cast<const char*>(expected), wire_bytes, 0,
                   reinterpret_cast<const sockaddr*>(&client), client_bytes);
            sendto(peer, "", 0, 0, reinterpret_cast<const sockaddr*>(&client), client_bytes);
            sendto(peer, "bad", 3, 0, reinterpret_cast<const sockaddr*>(&client), client_bytes);
            clear_bytes(wire, sizeof(wire));
            sendto(peer, reinterpret_cast<const char*>(wire), sizeof(wire), 0,
                   reinterpret_cast<const sockaddr*>(&client), client_bytes);
            check(until(media, discovery, loop, previous, previous, 4) && consumer.calls == previous &&
                      media.error == VoiceUdpMediaError::none && discovery.socket.receive_pending,
                  "UDP media drops auth failures, empty, malformed and truncated packets without stopping");
            expected[wire_bytes - 5] ^= 1;
            consumer.stop = true;
            sendto(peer, reinterpret_cast<const char*>(expected), wire_bytes, 0,
                   reinterpret_cast<const sockaddr*>(&client), client_bytes);
            // Other posted slots retire through their canceled completions.
            check(until(media, discovery, loop, previous + 1, previous, 4) &&
                      (slots == 1 || drain(media, discovery, loop)) && consumer.calls == previous + 1 &&
                      voice_udp_media_drained(media) && zero(rx, 256) && zero(tx, 256) &&
                      voice_udp_drained(discovery),
                  "UDP media callback cancellation retires buffers after callback and does not rearm");
        }
    }
    check(drain(media, discovery, loop), "UDP media closes and drains all overlapped buffers");
    if (voice_udp_media_drained(media) && voice_udp_drained(discovery)) {
        // Reopen only after cancellation delivery. Exhaust the retained key,
        // then destroy it before receiving the canceled read's completion.
        check(network_datagram_open(&loop, &discovery.socket, reinterpret_cast<const sockaddr*>(&address),
                                    address_bytes)
                  .ok(),
              "UDP media drained socket can reopen");
        discovery.phase = VoiceUdpPhase::ready;
        consumer.stop = false;
        if (voice_udp_media_begin(&media, &discovery, &key, 9, {rx, 256}, {tx, 256}, receive, &consumer)) {
            // Reuse key counter only forward: prior suite consumed UINT32_MAX.
            check(voice_udp_media_send(&media, 9, {plain, test_unhex(packet_vectors[0].plain, plain)}) ==
                          VoiceUdpMediaSend::failed &&
                      media.error == VoiceUdpMediaError::crypto &&
                      media.packet_error == VoicePacketError::nonce_exhausted,
                  "UDP media nonce exhaustion stops instead of wrapping or retrying");
            voice_packet_release(&key);
            check(drain(media, discovery, loop), "UDP media can drain after cipher retirement");
        }
    }
    voice_packet_release(&key);
    if (peer != INVALID_SOCKET)
        closesocket(peer);
    if (voice_udp_drained(discovery) && voice_udp_media_drained(media))
        network_loop_release(&loop);
}
} // namespace
void test_voice_udp_media(Check check) noexcept {
    for (u32 family = 0; family != 2; ++family) {
        loopback(check, family * 3, family ? AF_INET6 : AF_INET, voice_transport_aes256_gcm_rtpsize,
                 XChaChaBackend::automatic);
        loopback(check, family * 3 + 1, family ? AF_INET6 : AF_INET,
                 voice_transport_xchacha20_poly1305_rtpsize, XChaChaBackend::automatic);
        loopback(check, family * 3 + 2, family ? AF_INET6 : AF_INET,
                 voice_transport_xchacha20_poly1305_rtpsize, XChaChaBackend::portable);
    }
}
} // namespace maicord
