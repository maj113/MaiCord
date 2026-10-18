#include "maicord/app/voice_packet_crypto.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"


namespace maicord {
namespace {
VoicePacketResult failure(VoicePacketError error, AeadResult crypto = {}) noexcept {
    VoicePacketResult result{};
    result.error = error;
    result.crypto = crypto;
    return result;
}
bool touches_key(const VoicePacketCrypto& crypto, ConstSlice<u8> span) noexcept {
    return byte_ranges_overlap(span, {reinterpret_cast<const u8*>(&crypto), sizeof(crypto)}) ||
           byte_ranges_overlap(span, {crypto.aes.object.data, crypto.aes.object.capacity}) ||
           byte_ranges_overlap(span, {crypto.xchacha.storage.data, crypto.xchacha.storage.capacity});
}
VoicePacketError validate(const VoicePacketCrypto& crypto, u64 generation, ConstSlice<u8> input,
                          Slice<u8> output, u32 written) noexcept {
    if (!crypto.generation || (crypto.mode != voice_transport_aes256_gcm_rtpsize &&
                               crypto.mode != voice_transport_xchacha20_poly1305_rtpsize))
        return VoicePacketError::not_initialized;
    if (crypto.generation != generation)
        return VoicePacketError::stale_generation;
    if (!byte_range_valid(input) || !byte_range_valid({output.data, output.count}))
        return VoicePacketError::invalid_input;
    if (output.count < written)
        return VoicePacketError::output_too_small;
    const ConstSlice<u8> destination{output.data, written};
    if ((input.data != output.data && byte_ranges_overlap(input, destination)) ||
        touches_key(crypto, input) || touches_key(crypto, destination))
        return VoicePacketError::invalid_input;
    return VoicePacketError::none;
}
AeadResult crypt(VoicePacketCrypto& crypto, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                 Slice<u8> output, u8* tag, bool encrypt) noexcept {
    if (crypto.mode == voice_transport_aes256_gcm_rtpsize)
        return encrypt ? aes_gcm_encrypt(crypto.aes, {nonce.data, 12}, aad, input, output, {tag, 16})
                       : aes_gcm_decrypt(crypto.aes, {nonce.data, 12}, aad, input, {tag, 16}, output);
    return encrypt ? xchacha_encrypt(crypto.xchacha, nonce, aad, input, output, {tag, 16})
                   : xchacha_decrypt(crypto.xchacha, nonce, aad, input, {tag, 16}, output);
}
} // namespace

AeadResult voice_packet_initialize(VoicePacketCrypto* crypto, u64 generation, u32 mode,
                                   ConstSlice<u8> secret, XChaChaBackend backend) noexcept {
    if (!crypto || crypto->generation || crypto->mode || crypto->aes.handle || crypto->aes.object.data ||
        crypto->xchacha.storage.data || !generation || secret.count != 32 || !byte_range_valid(secret) ||
        touches_key(*crypto, secret) ||
        (mode != voice_transport_aes256_gcm_rtpsize && mode != voice_transport_xchacha20_poly1305_rtpsize))
        return {0, 0, AeadError::invalid_input};
    const auto result = mode == voice_transport_aes256_gcm_rtpsize
                            ? aes_gcm_initialize(&crypto->aes, secret)
                            : xchacha_initialize(&crypto->xchacha, secret, backend);
    if (!result.ok())
        return result;
    crypto->generation = generation;
    crypto->mode = mode;
    crypto->next_nonce = 0;
    return result;
}
void voice_packet_release(VoicePacketCrypto* crypto) noexcept {
    if (!crypto)
        return;
    aes_gcm_release(&crypto->aes);
    xchacha_release(&crypto->xchacha);
    *crypto = {};
}

VoicePacketResult voice_packet_seal(VoicePacketCrypto& crypto, u64 generation, ConstSlice<u8> plaintext,
                                    Slice<u8> packet) noexcept {
    if (plaintext.count > ~u32{0} - 20)
        return failure(VoicePacketError::invalid_input);
    const u32 required = plaintext.count + 20;
    const auto valid = validate(crypto, generation, plaintext, packet, required);
    if (valid != VoicePacketError::none)
        return failure(valid);
    MediaPacketLayout layout{};
    if (!media_packet_plaintext(plaintext, &layout))
        return failure(VoicePacketError::malformed_packet);
    if (crypto.next_nonce > ~u32{0})
        return failure(VoicePacketError::nonce_exhausted);
    // Reserve before invoking crypto. A failed send/crypto attempt cannot make
    // the next caller encrypt different plaintext with this nonce again.
    const u32 counter = static_cast<u32>(crypto.next_nonce++);
    u8 nonce[24]{};
    byte_store_be32(nonce, counter);
    const u32 body = plaintext.count - layout.clear_bytes;
    const auto encrypted =
        crypt(crypto, {nonce, 24}, {plaintext.data, layout.clear_bytes},
              {plaintext.data + layout.clear_bytes, body}, {packet.data + layout.clear_bytes, body},
              packet.data + plaintext.count, true);
    if (!encrypted.ok()) {
        secure_clear_bytes(packet.data, required);
        return failure(VoicePacketError::crypto, encrypted);
    }
    if (packet.data != plaintext.data)
        copy_bytes(packet.data, plaintext.data, layout.clear_bytes);
    byte_store_be32(packet.data + plaintext.count + 16, counter);
    return {required, counter, layout, {}, VoicePacketError::none};
}

VoicePacketResult voice_packet_open(VoicePacketCrypto& crypto, u64 generation, ConstSlice<u8> packet,
                                    Slice<u8> plaintext) noexcept {
    if (packet.count < 28)
        return failure(VoicePacketError::malformed_packet);
    const u32 bytes = packet.count - 20;
    const auto valid = validate(crypto, generation, packet, plaintext, bytes);
    if (valid != VoicePacketError::none)
        return failure(valid);
    MediaPacketLayout header{};
    if (!media_packet_header({packet.data, bytes}, &header))
        return failure(VoicePacketError::malformed_packet);
    u8 nonce[24]{};
    copy_bytes(nonce, packet.data + packet.count - 4, 4);
    const u32 counter = byte_load_be32(nonce);
    const u32 body = bytes - header.clear_bytes;
    const auto decrypted = crypt(
        crypto, {nonce, 24}, {packet.data, header.clear_bytes}, {packet.data + header.clear_bytes, body},
        {plaintext.data + header.clear_bytes, body}, const_cast<u8*>(packet.data + bytes), false);
    if (!decrypted.ok()) {
        secure_clear_bytes(plaintext.data, bytes);
        return failure(VoicePacketError::crypto, decrypted);
    }
    if (plaintext.data != packet.data)
        copy_bytes(plaintext.data, packet.data, header.clear_bytes);
    MediaPacketLayout layout{};
    if (!media_packet_plaintext({plaintext.data, bytes}, &layout)) {
        secure_clear_bytes(plaintext.data, bytes);
        return failure(VoicePacketError::malformed_packet);
    }
    return {bytes, counter, layout, {}, VoicePacketError::none};
}
} // namespace maicord
