#include "maicord/app/voice_packet_crypto.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
#include "maicord/protocol/voice_media_events.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/voice_packet_vectors.hpp"

void framing(Check check) noexcept {
    alignas(16) u8 packet[512]{};
    for (u32 csrcs = 0; csrcs != 16; ++csrcs) {
        for (u32 extension = 0; extension != 4; ++extension) {
            constexpr u32 paddings[]{0, 1, 255};
            for (u32 padding : paddings) {
                memset(packet, 0, sizeof(packet));
                packet[0] = static_cast<u8>(0x80 | csrcs | (extension ? 0x10 : 0) | (padding ? 0x20 : 0));
                packet[1] = 0xf8;
                packet[2] = 0xff;
                packet[3] = 0xfe;
                byte_store_be32(packet + 4, 0x12345678);
                byte_store_be32(packet + 8, 0x87654321);
                const u32 base = 12 + csrcs * 4;
                const u32 clear = base + (extension ? 4 : 0);
                const u32 ext_bytes = extension ? (extension - 1) * 4 : 0;
                if (extension) {
                    packet[base] = 0xbe;
                    packet[base + 1] = 0xde;
                    packet[base + 3] = static_cast<u8>(extension - 1);
                }
                const u32 bytes = clear + ext_bytes + 3 + padding;
                if (padding)
                    packet[bytes - 1] = static_cast<u8>(padding);
                MediaPacketLayout layout{};
                check(media_packet_plaintext({packet, bytes}, &layout) && layout.clear_bytes == clear &&
                          layout.extension_bytes == ext_bytes &&
                          layout.payload_offset == clear + ext_bytes && layout.payload_bytes == 3 &&
                          layout.padding_bytes == padding && layout.csrc_count == csrcs && layout.marker &&
                          layout.type == 120 && layout.sequence == 65534 &&
                          layout.timestamp == 0x12345678 && layout.ssrc == 0x87654321,
                      "RTP parses CSRC/extension/padding boundaries and network-order metadata");
                for (u32 cut = 0; cut < clear; ++cut) {
                    MediaPacketLayout preserved{};
                    preserved.ssrc = 123;
                    check(!media_packet_header({packet, cut}, &preserved) && preserved.ssrc == 123,
                          "RTP truncated clear header preserves output");
                }
            }
        }
    }
    const char* invalid[]{
        "407800010000000001020304",                 // Wrong version.
        "804800010000000001020304",                 // Forbidden RTP/RTCP-mux payload type.
        "907800010000000001020304bede0002ffff",     // Short extension data.
        "a0780001000000000102030400",               // Zero RTP padding count.
        "a0780001000000000102030402",               // Padding exceeds body.
        "80c9000201020304",                         // RTCP declared length exceeds packet.
        "80c900010102030480c90002",                 // Truncated compound member.
        "a0c90002010203040000000480c9000101020304", // Non-final padding.
        "a0c900020102030400000003",                 // Non-word RTCP padding.
        "a0c9000100000004"                          // Padding would cover the first clear SSRC.
    };
    for (const char* text : invalid) {
        const u32 bytes = test_unhex(text, packet);
        MediaPacketLayout layout{};
        layout.ssrc = 123;
        check(!media_packet_plaintext({packet, bytes}, &layout) && layout.ssrc == 123,
              "media framing rejects malformed RTP/RTCP without publishing metadata");
    }
    const u32 bytes = test_unhex(packet_vectors[0].plain, packet);
    check(!media_packet_plaintext({packet, bytes}, reinterpret_cast<MediaPacketLayout*>(packet)),
          "media framing output cannot alias its input");
    MediaPacketLayout empty{};
    check(!media_packet_header({nullptr, 12}, &empty), "media framing rejects null packet storage");
}

void vectors(Check check, u32 mode, XChaChaBackend backend) noexcept {
    u8 secret[32], plain[128], expected[160], output[192], opened[192];
    for (u32 i = 0; i != 32; ++i)
        secret[i] = static_cast<u8>(i);
    VoicePacketCrypto crypto{};
    check(voice_packet_initialize(&crypto, 7, mode, {secret, 32}, backend).ok(),
          "voice packet cipher initialization");
    if (!crypto.generation)
        return;
    for (const auto& vector : packet_vectors) {
        const u32 plain_bytes = test_unhex(vector.plain, plain);
        const u32 wire_bytes =
            test_unhex(mode == voice_transport_aes256_gcm_rtpsize ? vector.aes : vector.xchacha, expected);
        crypto.next_nonce = vector.counter; // Public test counter ranges include final legal values.
        memset(output, 0xa5, sizeof(output));
        const auto sealed = voice_packet_seal(crypto, 7, {plain, plain_bytes}, {output, sizeof(output)});
        check(sealed.ok() && sealed.bytes == wire_bytes && sealed.nonce == vector.counter &&
                  sealed.layout.clear_bytes == vector.clear_bytes &&
                  bytes_equal(output, expected, wire_bytes) &&
                  test_filled(output + wire_bytes, sizeof(output) - wire_bytes, 0xa5) &&
                  crypto.next_nonce == u64{vector.counter} + 1,
              "voice RTP/RTCP packet matches independent OpenSSL/libsodium wire fixture");
        memset(opened, 0xa5, sizeof(opened));
        const auto decoded = voice_packet_open(crypto, 7, {expected, wire_bytes}, {opened, sizeof(opened)});
        check(decoded.ok() && decoded.bytes == plain_bytes && decoded.nonce == vector.counter &&
                  decoded.layout.clear_bytes == vector.clear_bytes &&
                  bytes_equal(opened, plain, plain_bytes) &&
                  test_filled(opened + plain_bytes, sizeof(opened) - plain_bytes, 0xa5) &&
                  crypto.next_nonce == u64{vector.counter} + 1,
              "voice packet decrypts independent fixture without changing sender nonce");
        if (sealed.layout.kind == MediaPacketKind::rtp && sealed.layout.extension_bytes)
            check(decoded.layout.payload_offset == 32 && decoded.layout.payload_bytes == 7 &&
                      decoded.layout.padding_bytes == 3 && decoded.layout.extension_profile == 0xbede,
                  "voice packet strips encrypted extension contents and RTP padding from media view");
        copy_bytes(output, expected, wire_bytes);
        check(voice_packet_open(crypto, 7, {output, wire_bytes}, {output, sizeof(output)}).ok() &&
                  bytes_equal(output, plain, plain_bytes),
              "voice packet supports exact in-place decryption");
        // Every clear-header, encrypted-body, tag and nonce byte is protected.
        for (u32 i = 0; i != wire_bytes; ++i) {
            expected[i] ^= 1;
            memset(opened, 0xa5, sizeof(opened));
            const auto bad = voice_packet_open(crypto, 7, {expected, wire_bytes}, {opened, sizeof(opened)});
            check(!bad.ok() && !bad.bytes && !bad.nonce && !bad.layout.ssrc &&
                      test_filled(opened + plain_bytes, sizeof(opened) - plain_bytes, 0xa5),
                  "voice packet rejects alteration anywhere without publishing authenticated metadata");
            if (bad.error == VoicePacketError::crypto)
                check(test_filled(opened, plain_bytes, 0),
                      "voice packet authentication failure erases full plaintext packet");
            expected[i] ^= 1;
        }
    }
    memset(output, 0xa5, sizeof(output));
    const u32 bytes = test_unhex(packet_vectors[0].plain, plain);
    check(voice_packet_seal(crypto, 7, {plain, bytes}, {output, sizeof(output)}).error ==
                  VoicePacketError::nonce_exhausted &&
              crypto.next_nonce == (u64{1} << 32) && test_filled(output, sizeof(output), 0xa5),
          "voice transport consumes final legal nonce then refuses wrap without writing");
    check(voice_packet_initialize(&crypto, 8, mode, {secret, 32}, backend).error ==
                  AeadError::invalid_input &&
              crypto.generation == 7 && crypto.next_nonce == (u64{1} << 32),
          "voice key initialization cannot rewind a live counter or silently replace a key");
    voice_packet_release(&crypto);
    check(!crypto.generation && !crypto.mode && !crypto.aes.object.data && !crypto.xchacha.storage.data,
          "voice packet release destroys and erases both possible cipher owners");
}

void lifecycle(Check check, u32 mode, XChaChaBackend backend) noexcept {
    u8 secret[32]{}, plain[128], packet[192], opened[192];
    VoicePacketCrypto crypto{};
    check(voice_packet_initialize(&crypto, 11, mode, {secret, 32}, backend).ok(),
          "voice lifecycle cipher initialization");
    if (!crypto.generation)
        return;
    const u32 bytes = test_unhex(packet_vectors[1].plain, plain);
    memset(packet, 0xa5, sizeof(packet));
    check(voice_packet_seal(crypto, 10, {plain, bytes}, {packet, sizeof(packet)}).error ==
                  VoicePacketError::stale_generation &&
              !crypto.next_nonce && test_filled(packet, sizeof(packet), 0xa5),
          "stale voice job cannot use current packet key");
    check(voice_packet_seal(crypto, 11, {plain, bytes}, {packet, bytes + 19}).error ==
                  VoicePacketError::output_too_small &&
              !crypto.next_nonce && test_filled(packet, sizeof(packet), 0xa5),
          "short voice packet output does not consume nonce");
    plain[0] ^= 0x40;
    check(voice_packet_seal(crypto, 11, {plain, bytes}, {packet, sizeof(packet)}).error ==
                  VoicePacketError::malformed_packet &&
              !crypto.next_nonce && test_filled(packet, sizeof(packet), 0xa5),
          "invalid outbound framing does not consume nonce");
    plain[0] ^= 0x40;
    copy_bytes(packet, plain, bytes);
    check(voice_packet_seal(crypto, 11, {packet, bytes}, {packet + 1, sizeof(packet) - 1}).error ==
                  VoicePacketError::invalid_input &&
              !crypto.next_nonce && bytes_equal(packet, plain, bytes),
          "voice seal rejects partial overlap before writing");
    auto* storage =
        mode == voice_transport_aes256_gcm_rtpsize ? crypto.aes.object.data : crypto.xchacha.storage.data;
    check(voice_packet_seal(crypto, 11, {plain, bytes}, {storage, bytes + 20}).error ==
                  VoicePacketError::invalid_input &&
              voice_packet_seal(crypto, 11, {storage, 12}, {packet, sizeof(packet)}).error ==
                  VoicePacketError::invalid_input &&
              !crypto.next_nonce,
          "voice packet input/output cannot alias cipher storage");
    const auto sealed = voice_packet_seal(crypto, 11, {packet, bytes}, {packet, sizeof(packet)});
    check(sealed.ok() && sealed.nonce == 0 && crypto.next_nonce == 1,
          "voice seal supports exact in-place output");
    memset(opened, 0xa5, sizeof(opened));
    check(voice_packet_open(crypto, 10, {packet, sealed.bytes}, {opened, sizeof(opened)}).error ==
                  VoicePacketError::stale_generation &&
              test_filled(opened, sizeof(opened), 0xa5),
          "stale voice receive job cannot decrypt with current packet key");
    check(voice_packet_open(crypto, 11, {packet, sealed.bytes}, {opened, bytes - 1}).error ==
                  VoicePacketError::output_too_small &&
              test_filled(opened, sizeof(opened), 0xa5),
          "voice open short output remains untouched");
    const u32 rtcp_bytes = test_unhex(packet_vectors[4].plain, plain);
    const auto rtcp = voice_packet_seal(crypto, 11, {plain, rtcp_bytes}, {opened, sizeof(opened)});
    check(rtcp.ok() && rtcp.nonce == 1 && crypto.next_nonce == 2,
          "RTP and RTCP consume one shared per-key transport nonce sequence");
    // Force the underlying key unavailable after packet setup. Reservation must
    // still be consumed; it cannot be recycled after a failed crypto operation.
    aes_gcm_release(&crypto.aes);
    xchacha_release(&crypto.xchacha);
    memset(packet, 0xa5, sizeof(packet));
    const auto failed = voice_packet_seal(crypto, 11, {plain, rtcp_bytes}, {packet, sizeof(packet)});
    check(failed.error == VoicePacketError::crypto && !failed.bytes && crypto.next_nonce == 3 &&
              test_filled(packet, rtcp_bytes + 20, 0) &&
              test_filled(packet + rtcp_bytes + 20, sizeof(packet) - rtcp_bytes - 20, 0xa5),
          "failed voice encryption consumes reservation and clears attempted packet");
    voice_packet_release(&crypto);
    check(voice_packet_seal(crypto, 11, {plain, rtcp_bytes}, {packet, sizeof(packet)}).error ==
              VoicePacketError::not_initialized,
          "released voice packet key cannot encrypt");
}

void authenticated_malformed(Check check) noexcept {
    // The low-level cipher deliberately authenticates an invalid padding count.
    // Open must reject it after authentication and erase plaintext, in-place too.
    u8 secret[32]{}, nonce[12]{}, packet[64]{}, opened[64];
    const u32 bytes = test_unhex("a078000100000000010203040000", packet);
    VoicePacketCrypto crypto{};
    check(voice_packet_initialize(&crypto, 1, voice_transport_aes256_gcm_rtpsize, {secret, 32}).ok(),
          "authenticated malformed fixture initialization");
    if (!crypto.generation)
        return;
    check(aes_gcm_encrypt(crypto.aes, {nonce, 12}, {packet, 12}, {packet + 12, bytes - 12},
                          {packet + 12, bytes - 12}, {packet + bytes, 16})
              .ok(),
          "authenticate malformed RTP fixture");
    memset(opened, 0xa5, sizeof(opened));
    const auto malformed = voice_packet_open(crypto, 1, {packet, bytes + 20}, {opened, sizeof(opened)});
    check(malformed.error == VoicePacketError::malformed_packet && !malformed.bytes &&
              test_filled(opened, bytes, 0) && test_filled(opened + bytes, sizeof(opened) - bytes, 0xa5),
          "authenticated invalid RTP padding erases plaintext");
    check(voice_packet_open(crypto, 1, {packet, bytes + 20}, {packet, sizeof(packet)}).error ==
                  VoicePacketError::malformed_packet &&
              test_filled(packet, bytes, 0),
          "authenticated malformed in-place packet is erased");
    voice_packet_release(&crypto);
}
} // namespace

void test_voice_packet_crypto(Check check) noexcept {
    framing(check);
    vectors(check, voice_transport_aes256_gcm_rtpsize, XChaChaBackend::automatic);
    lifecycle(check, voice_transport_aes256_gcm_rtpsize, XChaChaBackend::automatic);
    constexpr XChaChaBackend backends[]{XChaChaBackend::automatic, XChaChaBackend::portable};
    for (auto backend : backends) {
        vectors(check, voice_transport_xchacha20_poly1305_rtpsize, backend);
        lifecycle(check, voice_transport_xchacha20_poly1305_rtpsize, backend);
    }
    authenticated_malformed(check);
}
} // namespace maicord
