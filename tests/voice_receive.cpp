#include "maicord/app/voice_receive.hpp"
#include "maicord/app/voice_mixer.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/winsock.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
constexpr u32 kSsrc = 0x0a0b0c0d;
constexpr u64 kUser = 4242;
void store_be16(u8* out, u16 value) noexcept {
    out[0] = static_cast<u8>(value >> 8);
    out[1] = static_cast<u8>(value);
}
struct Capture {
    u8 frame[256];
    u32 bytes, calls;
    bool fail;
};
DaveMediaResult capture(void* context, const VoiceReceivedPacket&, ConstSlice<u8> frame, Slice<u8>,
                        Slice<u8> output) noexcept {
    auto& value = *static_cast<Capture*>(context);
    ++value.calls;
    DaveMediaResult result{};
    if (value.fail || frame.count > sizeof(value.frame)) {
        result.error = value.fail ? DaveMediaError::not_ready : DaveMediaError::output_capacity;
        return result;
    }
    copy_bytes(value.frame, frame.data, frame.count);
    copy_bytes(output.data, frame.data, frame.count);
    value.bytes = result.bytes = frame.count;
    return result;
}
DaveMediaResult passthrough(void* context, const VoiceReceivedPacket& packet, ConstSlice<u8> frame,
                            Slice<u8> scratch, Slice<u8> output) noexcept {
    return dave_session_decrypt(static_cast<DaveSession*>(context), packet.source.user_id, frame, GetTickCount64(),
                                scratch, output);
}
// Writes an RTP packet: optional CSRCs, a one-byte-header extension of
// `extension_words` 32-bit words, the payload and optional padding.
u32 rtp(u8* out, u8 type, u16 sequence, u32 csrcs, u32 extension_words, ConstSlice<u8> payload,
        u8 padding = 0) noexcept {
    out[0] = static_cast<u8>(0x80 | (padding ? 0x20 : 0) | (extension_words ? 0x10 : 0) | csrcs);
    out[1] = type;
    store_be16(out + 2, sequence);
    byte_store_be32(out + 4, sequence * 960u);
    byte_store_be32(out + 8, kSsrc);
    u32 at = 12;
    for (u32 i = 0; i < csrcs; ++i, at += 4)
        byte_store_be32(out + at, 0x11111111u * (i + 1));
    if (extension_words) {
        out[at] = 0xbe, out[at + 1] = 0xde;
        store_be16(out + at + 2, static_cast<u16>(extension_words));
        at += 4;
        for (u32 i = 0; i < extension_words * 4; ++i)
            out[at++] = static_cast<u8>(0xe0 + i);
    }
    copy_bytes(out + at, payload.data, payload.count);
    at += payload.count;
    for (u8 i = 0; i < padding; ++i)
        out[at++] = i + 1 == padding ? padding : 0;
    return at;
}
VoiceReceivedPacket received(ConstSlice<u8> plain, VoiceSourceKind kind = VoiceSourceKind::audio) noexcept {
    VoiceReceivedPacket packet{};
    if (!media_packet_plaintext(plain, &packet.transport.layout))
        packet.transport.error = VoicePacketError::malformed_packet;
    packet.transport.bytes = plain.count;
    packet.source = {kUser, 1, kSsrc, kSsrc, kind, true};
    packet.member = {kUser, 1, 0, 0, true, false, false};
    packet.setup_generation = packet.ready_revision = 1;
    return packet;
}
u32 opus_noise(u32 seed, u8* out) noexcept {
    u32 state = seed * 2654435761u + 7u;
    out[0] = 0xfc; // CELT FB 20 ms stereo.
    for (u32 i = 1; i < 101; ++i) {
        state = state * 1664525u + 1013904223u;
        out[i] = static_cast<u8>(state >> 24);
    }
    return 101;
}
void test_rtp_vectors(Check check) noexcept {
    static VoiceReceive receive;
    static Capture captured;
    check(voice_playback_initialize(&receive.playback), "Voice receive: playback initializes for RTP vectors");
    const u8 payload[]{0xfc, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    struct Vector {
        const char* name;
        u32 csrcs, extension_words;
        u8 padding;
    };
    const Vector vectors[]{
        {"Voice receive RTP: a bare header strips to the Opus payload", 0, 0, 0},
        {"Voice receive RTP: decrypted extension elements are stripped (rtpsize)", 0, 1, 0},
        {"Voice receive RTP: CSRCs plus a two-word extension are stripped", 2, 2, 0},
        {"Voice receive RTP: trailing padding is excluded from the payload", 0, 1, 3},
    };
    u8 plain[256];
    u16 sequence = 0;
    for (const auto& vector : vectors) {
        const u32 bytes =
            rtp(plain, kVoiceOpusPayloadType, sequence++, vector.csrcs, vector.extension_words, {payload, 10},
                vector.padding);
        captured = {};
        const auto packet = received({plain, bytes});
        const auto result = voice_receive_packet(&receive, packet, {plain, bytes}, capture, &captured, 1000);
        check(packet.transport.layout.payload_offset == 12 + vector.csrcs * 4 + (vector.extension_words ? 4 : 0) +
                                                           vector.extension_words * 4 &&
                  result == VoiceReceiveResult::queued && captured.bytes == 10 &&
                  bytes_equal(captured.frame, payload, 10),
              vector.name);
    }
    u32 bytes = rtp(plain, 111, sequence++, 0, 0, {payload, 10});
    captured = {};
    const bool other_type =
        voice_receive_packet(&receive, received({plain, bytes}), {plain, bytes}, capture, &captured, 0) ==
        VoiceReceiveResult::ignored;
    bytes = rtp(plain, kVoiceOpusPayloadType, sequence++, 0, 0, {payload, 10});
    const bool video = voice_receive_packet(&receive, received({plain, bytes}, VoiceSourceKind::video),
                                            {plain, bytes}, capture, &captured, 0) == VoiceReceiveResult::ignored;
    const u8 rtcp[]{0x80, 200, 0, 1, 0, 0, 0, 1};
    const bool control = voice_receive_packet(&receive, received({rtcp, 8}), {rtcp, 8}, capture, &captured, 0) ==
                         VoiceReceiveResult::ignored;
    check(other_type && video && control && !captured.calls,
          "Voice receive RTP: non-Opus payload types, video sources and RTCP never reach DAVE");
    captured.fail = true;
    check(voice_receive_packet(&receive, received({plain, bytes}), {plain, bytes}, capture, &captured, 0) ==
                  VoiceReceiveResult::lost &&
              receive.stats.lost == 1,
          "Voice receive RTP: DAVE not-ready/rejected frames are treated as loss");
    voice_receive_stop(&receive);
    check(voice_receive_packet(&receive, received({plain, bytes}), {plain, bytes}, capture, &captured, 0) ==
              VoiceReceiveResult::ignored,
          "Voice receive RTP: stopped playback ignores media");
}
// Loopback UDP -> transport decrypt -> RTP strip -> DAVE passthrough ->
// jitter -> Opus -> mixer, compared with direct playback of the same packets.
struct Loopback {
    NetworkLoop loop;
    VoiceUdpDiscovery discovery;
    VoiceUdpMedia media;
    VoicePacketCrypto receiver, sender;
    VoiceReceive receive;
    VoicePlayback reference;
    DaveSession dave;
    i16 heard[3840], expected[3840];
};
bool dispatch(void* context, ConstSlice<u8> bytes, const VoicePacketResult& result) noexcept {
    auto& value = *static_cast<Loopback*>(context);
    voice_receive_packet(&value.receive, received({bytes.data, result.bytes}), bytes, passthrough, &value.dave,
                         1'000'000 + u64{result.layout.sequence} * 20'000);
    return true;
}
bool pump(Loopback& value, bool (*done)(const Loopback&) noexcept) noexcept {
    const u64 deadline = GetTickCount64() + 5000;
    while (!done(value)) {
        if (GetTickCount64() >= deadline)
            return false;
        const auto next = network_loop_poll(&value.loop, 100);
        if (next.state == NetworkPollState::error)
            return false;
        if (next.state == NetworkPollState::completion) {
            voice_udp_media_completion(&value.media, next.completion);
            voice_udp_completion(&value.discovery, next.completion, GetTickCount64());
        }
        voice_udp_tick(&value.discovery, GetTickCount64());
    }
    return true;
}
void test_loopback(Check check) noexcept {
    static Loopback value;
    static u8 rx[kVoiceReceiveDatagramBytes], tx[512];
    if (!network_loop_initialize(&value.loop).ok()) {
        check(false, "Voice receive loopback: network loop starts");
        return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    i32 address_bytes = sizeof(address);
    const SOCKET peer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const DWORD timeout = 2000;
    u8 secret[32], wire[512], plain[256], opus[128];
    for (u32 i = 0; i < 32; ++i)
        secret[i] = static_cast<u8>(0x40 + i);
    sockaddr_in client{};
    i32 client_bytes = sizeof(client);
    bool ok = peer != INVALID_SOCKET && bind(peer, reinterpret_cast<const sockaddr*>(&address), address_bytes) == 0 &&
              getsockname(peer, reinterpret_cast<sockaddr*>(&address), &address_bytes) == 0 &&
              setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) ==
                  0 &&
              voice_packet_initialize(&value.receiver, 9, voice_transport_aes256_gcm_rtpsize, {secret, 32}).ok() &&
              voice_packet_initialize(&value.sender, 1, voice_transport_aes256_gcm_rtpsize, {secret, 32}).ok() &&
              dave_session_initialize(&value.dave, 1, 7, 1, 0) == MlsStatus::ok &&
              voice_playback_initialize(&value.receive.playback) && voice_playback_initialize(&value.reference) &&
              voice_udp_begin(&value.discovery, &value.loop, reinterpret_cast<const sockaddr*>(&address),
                              address_bytes, kSsrc, GetTickCount64()) &&
              recvfrom(peer, reinterpret_cast<char*>(wire), sizeof(wire), 0, reinterpret_cast<sockaddr*>(&client),
                       &client_bytes) == 74;
    if (ok) {
        clear_bytes(wire, 74);
        wire[1] = 2;
        wire[3] = 70;
        byte_store_be32(wire + 4, kSsrc);
        copy_bytes(wire + 8, "127.0.0.1", 9);
        wire[72] = 0x11, wire[73] = 0xd7;
        ok = sendto(peer, reinterpret_cast<const char*>(wire), 74, 0, reinterpret_cast<const sockaddr*>(&client),
                    client_bytes) == 74 &&
             pump(value, [](const Loopback& v) noexcept { return v.discovery.phase != VoiceUdpPhase::discovering; }) &&
             value.discovery.phase == VoiceUdpPhase::ready &&
             voice_udp_media_begin(&value.media, &value.discovery, &value.receiver, 9, {rx, sizeof(rx)}, {tx, 512},
                                   dispatch, &value);
    }
    check(ok, "Voice receive loopback: discovery completes and the media transport attaches");
    constexpr u16 kPackets = 6;
    for (u16 sequence = 0; ok && sequence < kPackets; ++sequence) {
        const u32 opus_bytes = opus_noise(sequence, opus);
        const u32 bytes = rtp(plain, kVoiceOpusPayloadType, sequence, 0, 1, {opus, opus_bytes});
        const auto sealed = voice_packet_seal(value.sender, 1, {plain, bytes}, {wire, sizeof(wire)});
        ok = sealed.ok() && sendto(peer, reinterpret_cast<const char*>(wire), static_cast<i32>(sealed.bytes), 0,
                                   reinterpret_cast<const sockaddr*>(&client), client_bytes) ==
                                static_cast<i32>(sealed.bytes);
        voice_playback_push(&value.reference, kSsrc, kUser, 1, sequence, sequence * 960u, {opus, opus_bytes},
                            1'000'000 + u64{sequence} * 20'000);
    }
    ok = ok && pump(value, [](const Loopback& v) noexcept { return v.media.received_packets >= kPackets; });
    check(ok && value.receive.stats.queued == kPackets && !value.media.rejected_packets,
          "Voice receive loopback: every sealed RTP packet is opened, stripped and queued");
    for (u32 at = 0; at < 1920; at += 480) {
        voice_playback_render(&value.receive.playback, {value.heard + at * 2, 960}, 2'000'000);
        voice_playback_render(&value.reference, {value.expected + at * 2, 960}, 2'000'000);
    }
    check(ok && voice_pcm_level({value.heard, 3840}) > 0 &&
              bytes_equal(value.heard, value.expected, sizeof(value.heard)),
          "Voice receive loopback: mixed PCM matches direct decoding of the sent Opus packets");
    voice_udp_media_stop(&value.media);
    voice_udp_close(&value.discovery);
    const bool drained = pump(value, [](const Loopback& v) noexcept {
        return voice_udp_drained(v.discovery) && voice_udp_media_drained(v.media);
    });
    check(drained, "Voice receive loopback: transport drains");
    voice_receive_stop(&value.receive);
    voice_playback_release(&value.reference);
    dave_session_release(&value.dave);
    voice_packet_release(&value.receiver);
    voice_packet_release(&value.sender);
    if (peer != INVALID_SOCKET)
        closesocket(peer);
    if (drained)
        network_loop_release(&value.loop);
}
// Opt-in (MAICORD_VOICE_AUDIO_OUTPUT=1): opens the real default endpoint.
struct Tone {
    volatile LONG64 frames;
    u32 phase;
};
void tone(void* context, Slice<i16> pcm, u64) noexcept {
    auto& value = *static_cast<Tone*>(context);
    for (u32 i = 0; i + 1 < pcm.count; i += 2) {
        // Quiet 375 Hz square wave.
        const i16 sample = (value.phase++ / 64) & 1 ? 600 : -600;
        pcm[i] = pcm[i + 1] = sample;
    }
    InterlockedAdd64(&value.frames, pcm.count / 2);
}
void test_device(Check check) noexcept {
    wchar_t flag[4]{};
    if (!GetEnvironmentVariableW(L"MAICORD_VOICE_AUDIO_OUTPUT", flag, 4) || flag[0] != L'1')
        return;
    static VoiceAudioOutput output;
    static Tone value;
    check(voice_audio_output_start_device(&output, nullptr, tone, &value), "Voice audio output: thread starts");
    const u64 deadline = GetTickCount64() + 3000;
    while (GetTickCount64() < deadline && ReadAcquire64(&value.frames) < 24000)
        Sleep(20);
    voice_audio_output_stop(&output);
    check(ReadAcquire64(&value.frames) >= 24000 && !voice_audio_output_running(output),
          "Voice audio output: WASAPI pulls 48 kHz PCM16 events and stops cleanly");
}
} // namespace
void test_voice_receive(Check check) noexcept {
    test_rtp_vectors(check);
    test_loopback(check);
    test_device(check);
}
} // namespace maicord
