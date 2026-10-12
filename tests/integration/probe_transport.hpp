#pragma once

#include "maicord/core/byte_queue.hpp"
#include "maicord/platform/network.hpp"
#include "maicord/platform/tls_client.hpp"

namespace maicord {

inline constexpr u32 kProbeHandshakeBufferBytes = 256 * 1024;
inline constexpr u32 kProbeRecordBufferBytes = 64 * 1024;

struct ProbeTransport {
    NetworkLoop loop;
    NetworkConnection connection;
    TlsClient tls;
    ByteQueue incoming;
    u8 incoming_storage[kProbeHandshakeBufferBytes];
    u8 handshake_output[kProbeHandshakeBufferBytes];
    u8 record_output[kProbeRecordBufferBytes];
};

enum class ProbeReceiveState : u8 {
    plaintext,
    peer_closed,
    error,
};

struct ProbeReceiveResult {
    Slice<u8> plaintext;
    ProbeReceiveState state;
};

bool probe_transport_connect(ProbeTransport* transport, const wchar_t* hostname) noexcept;
bool probe_transport_send(ProbeTransport* transport, ConstSlice<u8> plaintext) noexcept;
ProbeReceiveResult probe_transport_receive(ProbeTransport* transport) noexcept;
void probe_transport_release(ProbeTransport* transport) noexcept;

} // namespace maicord
