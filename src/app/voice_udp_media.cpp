#include "maicord/app/voice_udp_media.hpp"
#include "maicord/core/byte_range.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
bool current(const VoiceUdpMedia& media) noexcept {
    return media.discovery && media.crypto && !media.stopping &&
           media.discovery->phase == VoiceUdpPhase::ready && media.discovery->socket.open &&
           media.discovery->socket.generation == media.socket_generation &&
           media.crypto->generation == media.generation;
}
void finish(VoiceUdpMedia* media) noexcept {
    if (!media->discovery || !media->stopping || media->dispatching ||
        !network_datagram_idle(media->discovery->socket))
        return;
    // A canceled read can contain bytes without a successful completion. Clear
    // the entire borrowed buffers once on retirement, never while kernel-owned.
    secure_clear_bytes(media->receive_buffer.data, media->receive_buffer.count);
    secure_clear_bytes(media->send_buffer.data, media->send_buffer.count);
    media->receive_buffer = media->send_buffer = {};
    media->discovery = nullptr;
    media->crypto = nullptr;
    media->receive = nullptr;
    media->context = nullptr;
    media->generation = 0;
    media->send_bytes = 0;
    media->receive_slots = media->slot_bytes = 0;
}
void fail(VoiceUdpMedia* media, VoiceUdpMediaError error, u32 native = 0) noexcept {
    media->error = error;
    media->native_error = native;
    voice_udp_media_stop(media);
}
Slice<u8> slot_buffer(const VoiceUdpMedia& media, u32 slot) noexcept {
    return {media.receive_buffer.data + usize{slot} * media.slot_bytes, media.slot_bytes};
}
bool read(VoiceUdpMedia* media, u32 slot) noexcept {
    const auto result =
        network_datagram_receive_slot(&media->discovery->socket, slot, slot_buffer(*media, slot));
    if (!result.ok())
        fail(media, VoiceUdpMediaError::receive, static_cast<u32>(result.native_error));
    return result.ok();
}
bool safe_buffer(ConstSlice<u8> buffer, const VoiceUdpMedia& media, const VoiceUdpDiscovery& discovery,
                 const VoicePacketCrypto& crypto) noexcept {
    return buffer.count >= 28 && byte_range_valid(buffer) &&
           !byte_ranges_overlap(buffer, {reinterpret_cast<const u8*>(&media), sizeof(media)}) &&
           !byte_ranges_overlap(buffer, {reinterpret_cast<const u8*>(&discovery), sizeof(discovery)}) &&
           !byte_ranges_overlap(buffer, {reinterpret_cast<const u8*>(&crypto), sizeof(crypto)}) &&
           !byte_ranges_overlap(buffer, {crypto.aes.object.data, crypto.aes.object.capacity}) &&
           !byte_ranges_overlap(buffer, {crypto.xchacha.storage.data, crypto.xchacha.storage.capacity});
}
} // namespace
bool voice_udp_media_drained(const VoiceUdpMedia& media) noexcept {
    return !media.discovery;
}
bool voice_udp_media_begin(VoiceUdpMedia* media, VoiceUdpDiscovery* discovery, VoicePacketCrypto* crypto,
                           u64 generation, Slice<u8> receive_buffer, Slice<u8> send_buffer,
                           VoiceUdpMediaReceive receive, void* context, u32 receive_slots) noexcept {
    if (!media || !voice_udp_media_drained(*media) || !discovery || !crypto || !receive || !generation ||
        crypto->generation != generation || discovery->phase != VoiceUdpPhase::ready ||
        !discovery->socket.open || !network_datagram_idle(discovery->socket) || !receive_slots ||
        receive_slots > kVoiceUdpMediaReceiveSlots || receive_buffer.count / receive_slots < 28 ||
        !safe_buffer({receive_buffer.data, receive_buffer.count}, *media, *discovery, *crypto) ||
        !safe_buffer({send_buffer.data, send_buffer.count}, *media, *discovery, *crypto) ||
        byte_ranges_overlap({receive_buffer.data, receive_buffer.count},
                            {send_buffer.data, send_buffer.count}))
        return false;
    reset_value(media);
    media->discovery = discovery;
    media->crypto = crypto;
    media->generation = generation;
    media->socket_generation = discovery->socket.generation;
    media->receive_buffer = receive_buffer;
    media->send_buffer = send_buffer;
    media->receive = receive;
    media->context = context;
    media->receive_slots = receive_slots;
    media->slot_bytes = receive_buffer.count / receive_slots;
    // Several posted reads plus a larger kernel buffer absorb video keyframe
    // bursts (hundreds of packets) while the worker is busy. Best effort.
    if (receive_slots > 1)
        network_datagram_set_receive_buffer(&discovery->socket, kVoiceUdpMediaKernelReceiveBytes);
    for (u32 slot = 0; slot != receive_slots; ++slot)
        if (!read(media, slot))
            return false;
    return true;
}
VoiceUdpMediaSend voice_udp_media_send(VoiceUdpMedia* media, u64 generation,
                                       ConstSlice<u8> plaintext) noexcept {
    if (!media || !current(*media) || generation != media->generation)
        return VoiceUdpMediaSend::invalid;
    if (media->discovery->socket.send_pending)
        return VoiceUdpMediaSend::busy;
    // Caller input cannot borrow a pending read, owner metadata, or retained
    // output: these can be overwritten while encrypting or polling completion.
    if (!byte_range_valid(plaintext) ||
        byte_ranges_overlap(plaintext, {media->receive_buffer.data, media->receive_buffer.count}) ||
        byte_ranges_overlap(plaintext, {media->send_buffer.data, media->send_buffer.count}) ||
        byte_ranges_overlap(plaintext, {reinterpret_cast<const u8*>(media), sizeof(*media)}) ||
        byte_ranges_overlap(plaintext,
                            {reinterpret_cast<const u8*>(media->discovery), sizeof(*media->discovery)}))
        return VoiceUdpMediaSend::invalid;
    const auto sealed = voice_packet_seal(*media->crypto, generation, plaintext, media->send_buffer);
    if (sealed.error == VoicePacketError::crypto || sealed.error == VoicePacketError::nonce_exhausted ||
        sealed.error == VoicePacketError::not_initialized) {
        media->packet_error = sealed.error;
        fail(media, VoiceUdpMediaError::crypto, static_cast<u32>(sealed.crypto.native_status));
        return VoiceUdpMediaSend::failed;
    }
    if (!sealed.ok())
        return VoiceUdpMediaSend::invalid;
    media->send_bytes = sealed.bytes;
    const auto sent =
        network_datagram_send(&media->discovery->socket, {media->send_buffer.data, sealed.bytes});
    if (!sent.ok()) {
        fail(media, VoiceUdpMediaError::send, static_cast<u32>(sent.native_error));
        return VoiceUdpMediaSend::failed;
    }
    return VoiceUdpMediaSend::accepted;
}
void voice_udp_media_stop(VoiceUdpMedia* media) noexcept {
    if (!media || !media->discovery)
        return;
    media->stopping = true;
    voice_udp_close(media->discovery);
    finish(media);
}
void voice_udp_media_completion(VoiceUdpMedia* media, const NetworkCompletion& completion) noexcept {
    if (!media || !media->discovery || completion.datagram != &media->discovery->socket ||
        (completion.kind != NetworkOperationKind::datagram_send &&
         completion.kind != NetworkOperationKind::datagram_receive))
        return;
    if (media->stopping) {
        finish(media);
        return;
    }
    if (completion.stale || !current(*media)) {
        fail(media, VoiceUdpMediaError::generation);
        return;
    }
    if (completion.kind == NetworkOperationKind::datagram_send) {
        if (!completion.succeeded || !media->send_bytes || completion.transferred != media->send_bytes) {
            fail(media, VoiceUdpMediaError::send, static_cast<u32>(completion.native_error));
            return;
        }
        secure_clear_bytes(media->send_buffer.data, media->send_bytes);
        media->send_bytes = 0;
        ++media->sent_packets;
        return;
    }
    const u32 slot = completion.datagram_slot;
    if (slot >= media->receive_slots) {
        fail(media, VoiceUdpMediaError::receive);
        return;
    }
    const auto buffer = slot_buffer(*media, slot);
    if (!completion.succeeded) {
        if (completion.native_error == ERROR_MORE_DATA || completion.native_error == WSAEMSGSIZE) {
            ++media->rejected_packets;
            secure_clear_bytes(buffer.data, buffer.count);
            read(media, slot);
        } else
            fail(media, VoiceUdpMediaError::receive, static_cast<u32>(completion.native_error));
        return;
    }
    if (completion.transferred > buffer.count) {
        fail(media, VoiceUdpMediaError::receive);
        return;
    }
    const auto packet =
        voice_packet_open(*media->crypto, media->generation, {buffer.data, completion.transferred}, buffer);
    bool keep = true;
    if (packet.ok()) {
        ++media->received_packets;
        media->dispatching = true;
        keep = media->receive(media->context, {buffer.data, packet.bytes}, packet);
        media->dispatching = false;
    } else {
        ++media->rejected_packets;
        media->packet_error = packet.error;
    }
    // Borrowed plaintext never persists between packets, even if the consumer
    // stopped the owner inside the callback. Retirement defers until it returns.
    secure_clear_bytes(buffer.data, completion.transferred);
    // A datagram that does not open (malformed, unauthenticated, or one the
    // AEAD refuses, such as an empty body) is one lost packet. Only a local
    // key/generation fault ends the transport.
    if (!packet.ok() && (packet.error == VoicePacketError::not_initialized ||
                         packet.error == VoicePacketError::stale_generation)) {
        media->packet_error = packet.error;
        fail(media, VoiceUdpMediaError::crypto, static_cast<u32>(packet.crypto.native_status));
        return;
    }
    if (!keep && !media->stopping)
        fail(media, VoiceUdpMediaError::consumer);
    else if (media->stopping)
        finish(media);
    else if (!current(*media))
        fail(media, VoiceUdpMediaError::generation);
    else
        read(media, slot);
}
} // namespace maicord
