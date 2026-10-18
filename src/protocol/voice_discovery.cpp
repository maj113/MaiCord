#include "maicord/protocol/voice_discovery.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {
bool voice_discovery_request(u32 ssrc, Slice<u8> output) noexcept {
    if (!ssrc || output.count < kVoiceDiscoveryBytes)
        return false;
    clear_bytes(output.data, kVoiceDiscoveryBytes);
    output[1] = 1;
    output[3] = 70;
    byte_store_be32(output.data + 4, ssrc);
    return true;
}
bool voice_discovery_response(ConstSlice<u8> packet, u32 ssrc, VoiceDiscoveryAddress* output) noexcept {
    if (packet.count != kVoiceDiscoveryBytes || !ssrc || byte_load_be16(packet.data) != 2 ||
        byte_load_be16(packet.data + 2) != 70 || byte_load_be32(packet.data + 4) != ssrc)
        return false;
    u32 length = 0;
    while (length < 64 && packet[8 + length])
        ++length;
    const u16 port = byte_load_be16(packet.data + 72);
    if (!length || length == 64 || !port)
        return false;
    VoiceDiscoveryAddress result{};
    copy_bytes(result.address, packet.data + 8, length);
    result.address_bytes = length;
    result.port = port;
    *output = result;
    return true;
}
} // namespace maicord
