#pragma once
#include "maicord/app/voice_packet_crypto.hpp"
#include "maicord/app/voice_udp_discovery.hpp"

namespace maicord {
inline constexpr u32 kVoiceUdpMediaReceiveSlots = kNetworkDatagramReceiveSlots;
inline constexpr u32 kVoiceUdpMediaKernelReceiveBytes = 2u << 20;
enum class VoiceUdpMediaError : u8 {
    none,
    receive,
    send,
    generation,
    consumer,
    crypto
};
enum class VoiceUdpMediaSend : u8 {
    accepted,
    busy,
    invalid,
    failed
};
// Transport authentication only: the consumer MUST enforce replay/participant
// policy and DAVE before decoding. Packet and result are borrowed until return.
// Returning false stops transport. The callback may send or stop this owner;
// it must not recursively poll/deliver completions or destroy borrowed owners.
using VoiceUdpMediaReceive = bool (*)(void*, ConstSlice<u8>, const VoicePacketResult&) noexcept;
struct VoiceUdpMedia {
    VoiceUdpDiscovery* discovery;
    VoicePacketCrypto* crypto;
    Slice<u8> receive_buffer, send_buffer;
    VoiceUdpMediaReceive receive;
    void* context;
    u64 generation, received_packets, rejected_packets, sent_packets;
    u32 socket_generation, send_bytes, native_error;
    u32 receive_slots, slot_bytes; // receive_buffer is split into equal per-read slots.
    VoiceUdpMediaError error;
    VoicePacketError packet_error;
    bool stopping, dispatching;
};
// Worker-owned, no allocation/queue/thread/timer. Borrows two disjoint buffers
// and the retained discovery socket/key. receive_slots reads (each owning an
// equal slice of receive_buffer, at least 28 bytes) and one write run
// concurrently; completions are dispatched one datagram at a time.
// Busy sends leave caller data and the nonce untouched; accepted sends have
// already encrypted/copied into the retained send buffer so input may be reused.
// Buffers/owners stay alive until drained, including after stop/cancellation.
bool voice_udp_media_begin(VoiceUdpMedia* media, VoiceUdpDiscovery* discovery, VoicePacketCrypto* crypto,
                           u64 generation, Slice<u8> receive_buffer, Slice<u8> send_buffer,
                           VoiceUdpMediaReceive receive, void* context, u32 receive_slots = 1) noexcept;
VoiceUdpMediaSend voice_udp_media_send(VoiceUdpMedia* media, u64 generation,
                                       ConstSlice<u8> plaintext) noexcept;
void voice_udp_media_completion(VoiceUdpMedia* media, const NetworkCompletion& completion) noexcept;
void voice_udp_media_stop(VoiceUdpMedia* media) noexcept;
bool voice_udp_media_drained(const VoiceUdpMedia& media) noexcept;
} // namespace maicord
