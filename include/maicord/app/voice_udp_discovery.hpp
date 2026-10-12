#pragma once
#include "maicord/platform/network.hpp"
#include "maicord/protocol/voice_discovery.hpp"

namespace maicord {
enum class VoiceUdpPhase : u8 {
    empty,
    discovering,
    ready,
    closing,
    closed
};
enum class VoiceUdpError : u8 {
    none,
    open,
    send,
    receive,
    timeout
};
struct VoiceUdpDiscovery {
    NetworkDatagram socket;
    VoiceDiscoveryAddress external;
    u64 deadline_ms;
    u32 ssrc, native_error;
    VoiceUdpPhase phase;
    VoiceUdpError error;
    bool sent, received;
    u8 request[kVoiceDiscoveryBytes], response[kVoiceDiscoveryBytes + 1];
};
// Worker-only, one discovery per Ready/SSRC. No allocation or helper thread.
// Success leaves the SAME connected UDP socket open for subsequent RTP/media,
// preserving the discovered NAT mapping. It does not select an encryption mode.
bool voice_udp_begin(VoiceUdpDiscovery* discovery, NetworkLoop* loop, const sockaddr* peer, i32 peer_bytes,
                     u32 ssrc, u64 now_ms) noexcept;
void voice_udp_completion(VoiceUdpDiscovery* discovery, const NetworkCompletion& completion,
                          u64 now_ms) noexcept;
void voice_udp_tick(VoiceUdpDiscovery* discovery, u64 now_ms) noexcept;
void voice_udp_close(VoiceUdpDiscovery* discovery) noexcept;
bool voice_udp_drained(const VoiceUdpDiscovery& discovery) noexcept;
} // namespace maicord
