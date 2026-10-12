#include "maicord/app/voice_udp_discovery.hpp"
#include "maicord/core/deadline.hpp"

#include <Windows.h>
#include <WS2tcpip.h>

namespace maicord {
namespace {
void finish(VoiceUdpDiscovery* discovery) noexcept {
    if (discovery->phase != VoiceUdpPhase::closing || !network_datagram_idle(discovery->socket))
        return;
    SecureZeroMemory(discovery->request, sizeof(discovery->request));
    SecureZeroMemory(discovery->response, sizeof(discovery->response));
    discovery->external = {};
    discovery->phase = VoiceUdpPhase::closed;
}
void fail(VoiceUdpDiscovery* discovery, VoiceUdpError error, u32 native) noexcept {
    discovery->error = error;
    discovery->native_error = native;
    voice_udp_close(discovery);
}
void ready(VoiceUdpDiscovery* discovery) noexcept {
    if (discovery->sent && discovery->received) {
        discovery->phase = VoiceUdpPhase::ready;
        discovery->deadline_ms = 0;
    }
}
bool receive(VoiceUdpDiscovery* discovery) noexcept {
    const auto result =
        network_datagram_receive(&discovery->socket, {discovery->response, sizeof(discovery->response)});
    if (!result.ok())
        fail(discovery, VoiceUdpError::receive, static_cast<u32>(result.native_error));
    return result.ok();
}
} // namespace
bool voice_udp_drained(const VoiceUdpDiscovery& discovery) noexcept {
    return !discovery.socket.open && network_datagram_idle(discovery.socket);
}
void voice_udp_close(VoiceUdpDiscovery* discovery) noexcept {
    if (!discovery)
        return;
    discovery->deadline_ms = 0;
    discovery->phase = VoiceUdpPhase::closing;
    network_datagram_close(&discovery->socket);
    finish(discovery);
}
bool voice_udp_begin(VoiceUdpDiscovery* discovery, NetworkLoop* loop, const sockaddr* peer, i32 peer_bytes,
                     u32 ssrc, u64 now_ms) noexcept {
    if (!discovery || !voice_udp_drained(*discovery) || !ssrc)
        return false;
    const auto opened = network_datagram_open(loop, &discovery->socket, peer, peer_bytes);
    if (!opened.ok()) {
        fail(discovery, VoiceUdpError::open, static_cast<u32>(opened.native_error));
        return false;
    }
    discovery->phase = VoiceUdpPhase::discovering;
    discovery->error = VoiceUdpError::none;
    discovery->native_error = 0;
    discovery->external = {};
    discovery->ssrc = ssrc;
    discovery->sent = discovery->received = false;
    discovery->deadline_ms = deadline_after(now_ms, 5000);
    voice_discovery_request(ssrc, {discovery->request, sizeof(discovery->request)});
    if (!receive(discovery))
        return false;
    const auto sent =
        network_datagram_send(&discovery->socket, {discovery->request, sizeof(discovery->request)});
    if (!sent.ok()) {
        fail(discovery, VoiceUdpError::send, static_cast<u32>(sent.native_error));
        return false;
    }
    return true;
}
void voice_udp_completion(VoiceUdpDiscovery* discovery, const NetworkCompletion& completion,
                          u64 now_ms) noexcept {
    if (!discovery || completion.datagram != &discovery->socket ||
        (completion.kind != NetworkOperationKind::datagram_send &&
         completion.kind != NetworkOperationKind::datagram_receive))
        return;
    // A queued response observed at/after expiry cannot revive the attempt.
    voice_udp_tick(discovery, now_ms);
    if (discovery->phase == VoiceUdpPhase::closing) {
        finish(discovery);
        return;
    }
    if (discovery->phase != VoiceUdpPhase::discovering)
        return;
    if (completion.stale)
        return;
    if (completion.kind == NetworkOperationKind::datagram_send) {
        if (!completion.succeeded || completion.transferred != kVoiceDiscoveryBytes) {
            fail(discovery, VoiceUdpError::send, static_cast<u32>(completion.native_error));
            return;
        }
        SecureZeroMemory(discovery->request, sizeof(discovery->request));
        discovery->sent = true;
        ready(discovery);
        return;
    }
    if (!completion.succeeded) {
        // Windows IOCP reports the Win32 truncation status; the Winsock
        // equivalent can also occur. Never parse any partial datagram.
        if (completion.native_error == ERROR_MORE_DATA || completion.native_error == WSAEMSGSIZE) {
            receive(discovery);
            return;
        }
        fail(discovery, VoiceUdpError::receive, static_cast<u32>(completion.native_error));
        return;
    }
    VoiceDiscoveryAddress external{};
    IN_ADDR v4{};
    IN6_ADDR v6{};
    if (voice_discovery_response({discovery->response, completion.transferred}, discovery->ssrc,
                                 &external) &&
        (InetPtonA(AF_INET, external.address, &v4) == 1 ||
         InetPtonA(AF_INET6, external.address, &v6) == 1)) {
        discovery->external = external;
        discovery->received = true;
        SecureZeroMemory(discovery->response, sizeof(discovery->response));
        ready(discovery);
    } else
        receive(discovery); // Ignore unrelated/control/malformed packets without extending timeout.
}
void voice_udp_tick(VoiceUdpDiscovery* discovery, u64 now_ms) noexcept {
    if (!discovery)
        return;
    if (discovery->phase == VoiceUdpPhase::discovering && deadline_due(now_ms, discovery->deadline_ms))
        fail(discovery, VoiceUdpError::timeout, WAIT_TIMEOUT);
    else
        finish(discovery);
}
} // namespace maicord
