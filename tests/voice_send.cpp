#include "maicord/app/voice_send.hpp"
#include "maicord/app/voice_receive.hpp"
#include "maicord/app/voice_mixer.hpp"
#include "maicord/app/dave_receive_sender.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

#include <WS2tcpip.h>

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
constexpr u32 kSsrc = 0x0a0b0c0d;
constexpr u64 kUser = 4242;
constexpr u64 kStart = 1'000'000;

// Synthetic microphone: a sine (amplitude 0 = digital silence) in 10 ms chunks.
struct Tone {
    u32 phase;
};
void tone(Tone& t, i16* out, u32 count, i32 amplitude, u32 hz) noexcept {
    // Parabolic sine approximation; exact purity is irrelevant here.
    for (u32 i = 0; i < count; ++i, ++t.phase) {
        const u32 period = 48000 / hz;
        const i32 x = static_cast<i32>(t.phase % period) * 2000 / static_cast<i32>(period) - 1000; // -1000..1000
        const i32 y = 4 * x * (1000 - (x < 0 ? -x : x)) / 1000;                                  // -1000..1000
        out[i] = static_cast<i16>(-amplitude * y / 1000);
    }
}
// Feeds `frames` 20 ms frames as two 10 ms device packets each, at real-time stamps.
void feed(VoiceSend& send, Tone& t, u64* now, u32 frames, i32 amplitude) noexcept {
    i16 chunk[480];
    for (u32 i = 0; i < frames * 2; ++i) {
        tone(t, chunk, 480, amplitude, 400);
        *now += 10'000;
        voice_send_push(&send, {chunk, 480}, *now);
    }
}

void test_blocks(Check check) noexcept {
    u8 header[12];
    const u8 expected[]{0x80, 0x78, 0x12, 0x34, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x02, 0x03, 0x04};
    check(voice_rtp_header({header, 12}, 0x1234, 0x89abcdef, 0x01020304) == 12 && bytes_equal(header, expected, 12) &&
              !voice_rtp_header({header, 11}, 0, 0, 0),
          "Voice send RTP: version 2, Opus payload type 120, big-endian sequence/timestamp/SSRC");

    static i16 pcm[960];
    for (u32 i = 0; i < 960; ++i)
        pcm[i] = i & 1 ? 32767 : -32767;
    const bool full = voice_level({pcm, 960}) == 100;
    for (u32 i = 0; i < 960; ++i)
        pcm[i] = i & 1 ? 3300 : -3300; // -19.9 dBFS
    const bool minus20 = voice_level({pcm, 960}) == 80;
    clear_bytes(pcm, sizeof(pcm));
    check(full && minus20 && voice_level({pcm, 960}) == 0 && voice_level_from_energy(1) < 15,
          "Voice send level: full scale 100, -20 dBFS 80, silence 0 on the shared dB scale");

    static VoiceFramer framer;
    clear_bytes(&framer, sizeof(framer));
    static i16 burst[960 * 9], frame[960];
    for (u32 i = 0; i < 960 * 9; ++i)
        burst[i] = static_cast<i16>(i / 960);
    u64 index = 0, wake = 0;
    voice_framer_push(&framer, {burst, 1440});
    bool ok = voice_framer_pop(&framer, kStart, {frame, 960}, &index, &wake) && index == 0 && frame[0] == 0 &&
              !voice_framer_pop(&framer, kStart, {frame, 960}, &index, &wake) && wake == 0;
    // A three-frame burst (one frame of backlog plus two) is spread 20 ms apart.
    voice_framer_push(&framer, {burst + 1440, 960 * 2 + 480});
    ok = ok && voice_framer_pop(&framer, kStart + 20'000, {frame, 960}, &index, &wake) && index == 1 &&
         !voice_framer_pop(&framer, kStart + 20'000, {frame, 960}, &index, &wake) && wake == kStart + 40'000 &&
         voice_framer_pop(&framer, kStart + 39'000, {frame, 960}, &index, &wake) && index == 2 &&
         !voice_framer_pop(&framer, kStart + 39'000, {frame, 960}, &index, &wake) &&
         voice_framer_pop(&framer, kStart + 60'000, {frame, 960}, &index, &wake) && index == 3 && frame[0] == 3;
    check(ok, "Voice send framing: 10 ms chunks become 20 ms frames released at most every 20 ms");
    // A device clock running ahead builds backlog; beyond two frames they leave at once.
    clear_bytes(&framer, sizeof(framer));
    voice_framer_push(&framer, {burst, 960 * 4});
    u32 released = 0;
    while (voice_framer_pop(&framer, kStart, {frame, 960}, &index, &wake))
        ++released;
    voice_framer_push(&framer, {burst, 960 * 9});
    u32 dropped_index = 0, first_after_stall = ~0u;
    while (voice_framer_pop(&framer, kStart + 500'000, {frame, 960}, &index, &wake))
        first_after_stall = first_after_stall == ~0u ? static_cast<u32>(index) : first_after_stall,
        dropped_index = static_cast<u32>(index);
    check(released == 2 && framer.dropped_frames == 3 && first_after_stall == 5 && dropped_index == 10,
          "Voice send pacing: backlog beyond two frames drains, a stalled ring drops the oldest audio and keeps "
          "capture time");

    VoiceActivity activity{};
    bool vad = voice_activity_update(&activity, 60, 50, false);
    u32 held = 0;
    for (u32 i = 0; i < 20 && voice_activity_update(&activity, 30, 50, false); ++i)
        ++held;
    check(vad && held == kVoiceActivityHangoverFrames && !activity.active && activity.threshold == 50,
          "Voice activity: speech above the threshold opens at once and holds 300 ms of hangover");
    activity = {};
    for (u32 i = 0; i < 200; ++i)
        voice_activity_update(&activity, 30, 50, true);
    const u8 floor_threshold = activity.threshold;
    vad = voice_activity_update(&activity, 45, 50, true);
    const bool loud = voice_activity_update(&activity, 60, 50, true);
    check(floor_threshold >= 46 && floor_threshold <= 50 && !vad && loud,
          "Voice activity: automatic sensitivity keeps the threshold about 18 dB above the noise floor");

    VoicePushToTalk ptt{};
    const bool idle = !voice_push_to_talk_update(&ptt, false, kStart, 20);
    const bool pressed = voice_push_to_talk_update(&ptt, true, kStart, 20);
    const bool released_hold = voice_push_to_talk_update(&ptt, false, kStart + 100'000, 20) &&
                               voice_push_to_talk_update(&ptt, false, kStart + 119'000, 20);
    const bool ended = !voice_push_to_talk_update(&ptt, false, kStart + 120'000, 20);
    voice_push_to_talk_update(&ptt, true, kStart + 200'000, 20);
    voice_push_to_talk_update(&ptt, false, kStart + 210'000, 20);
    const bool repressed = voice_push_to_talk_update(&ptt, true, kStart + 225'000, 20) &&
                           voice_push_to_talk_update(&ptt, false, kStart + 240'000, 20) &&
                           voice_push_to_talk_update(&ptt, false, kStart + 259'000, 20);
    check(idle && pressed && released_hold && ended && repressed,
          "Push-to-talk: transmits while held plus the release delay; a re-press restarts the delay");

    VoiceTransmitter tx{};
    using A = VoiceTransmitAction;
    const bool wants[]{true, true, false, false, false, false, false, false, true, false, false, true};
    const A expected_actions[]{A::start,   A::voice,    A::silence, A::silence, A::silence, A::silence,
                               A::tail_end, A::idle,    A::start,   A::silence, A::silence, A::voice};
    ok = true;
    for (u32 i = 0; i < 12; ++i)
        ok = ok && voice_transmit_next(&tx, wants[i]) == expected_actions[i];
    check(ok, "Voice transmit: start, voice, five silence frames ending the spurt; speech in the tail resumes "
              "without a new start");
}

// Fake transport: records Speaking and RTP packets; DAVE passthrough.
struct Recorder {
    bool ready, busy_once;
    u32 packets, speaking_calls, last_speaking;
    u32 order[64]; // 1000 + flags for Speaking, else payload bytes.
    u16 sequences[64];
    u32 timestamps[64];
    u8 payloads[64][4];
    u32 count;
};
bool recorder_ready(void* c) noexcept {
    return static_cast<Recorder*>(c)->ready;
}
DaveSealResult recorder_seal(void*, ConstSlice<u8> opus, Slice<u8> output) noexcept {
    copy_bytes(output.data, opus.data, opus.count);
    return {opus.count, 0, 0, DaveSealError::none};
}
VoiceUdpMediaSend recorder_send(void* c, ConstSlice<u8> packet) noexcept {
    auto& r = *static_cast<Recorder*>(c);
    if (r.busy_once) {
        r.busy_once = false;
        return VoiceUdpMediaSend::busy;
    }
    if (r.count < 64) {
        r.order[r.count] = packet.count - 12;
        r.sequences[r.count] = static_cast<u16>(packet[2] << 8 | packet[3]);
        r.timestamps[r.count] = byte_load_be32(packet.data + 4);
        copy_bytes(r.payloads[r.count], packet.data + 12, minimum(4u, packet.count - 12));
        ++r.count;
    }
    ++r.packets;
    return VoiceUdpMediaSend::accepted;
}
VoiceSocketSend recorder_speaking(void* c, u32 flags) noexcept {
    auto& r = *static_cast<Recorder*>(c);
    ++r.speaking_calls;
    r.last_speaking = flags;
    if (r.count < 64)
        r.order[r.count++] = 1000 + flags;
    return VoiceSocketSend::accepted;
}

void test_pipeline(Check check) noexcept {
    static VoiceSend send;
    static Recorder rec;
    send = {};
    rec = {};
    rec.ready = true;
    check(voice_send_open(&send), "Voice send pipeline: shared block and encoder allocate");
    voice_send_set_control(&send, {100, 20, VoiceInputMode::voice_activity, 50, 0, false, true});
    const VoiceSendTransport transport{&rec, 7, kSsrc, recorder_ready, recorder_seal, recorder_send, recorder_speaking};
    Tone t{};
    u64 now = kStart;
    // 10 loud frames, then 30 quiet (-58 dBFS) ones: start + 9 voice + 15 hangover, 5-frame tail.
    for (u32 i = 0; i < 40; ++i) {
        feed(send, t, &now, 1, i < 10 ? 12000 : 60);
        if (i == 3)
            rec.busy_once = true;
        voice_send_flush(&send, transport);
    }
    bool speaking_first = rec.order[0] == 1001 && rec.count > 2;
    u32 audio = 0, silence = 0, last_audio = 0;
    bool consecutive = true, paced = true, tail_ok = true;
    for (u32 i = 1; i < rec.count; ++i) {
        if (rec.order[i] >= 1000)
            continue;
        const u32 at = audio + silence;
        if (at && (rec.sequences[i] != static_cast<u16>(rec.sequences[last_audio] + 1) ||
                   rec.timestamps[i] - rec.timestamps[last_audio] != 960))
            consecutive = paced = false;
        const bool is_silence = rec.order[i] == 3 && bytes_equal(rec.payloads[i], kVoiceSilenceFrame, 3);
        if (is_silence)
            ++silence;
        else {
            ++audio;
            tail_ok = tail_ok && !silence;
        }
        last_audio = i;
    }
    check(speaking_first && audio == 25 && consecutive && paced,
          "Voice send pipeline: Speaking precedes audio; a busy transport keeps order; RTP sequence +1 and "
          "timestamp +960 per frame");
    check(silence == kVoiceSilenceTailFrames && tail_ok && rec.order[rec.count - 1] == 1000 &&
              rec.speaking_calls == 2 && !voice_send_queued(send),
          "Voice send pipeline: five 0xF8 0xFF 0xFE silence frames end the spurt, then Speaking off");

    // DAVE not ready (joining or recovering): frames are dropped, nothing is announced.
    const u32 before = rec.count;
    rec.ready = false;
    feed(send, t, &now, 3, 12000);
    voice_send_flush(&send, transport);
    check(rec.count == before && !voice_send_queued(send) && send.dropped_packets >= 3,
          "Voice send pipeline: frames are dropped while DAVE cannot encrypt");

    // Muted (send off) mid-spurt: the tail still goes out.
    rec.ready = true;
    rec.count = 0;
    feed(send, t, &now, 2, 12000);
    voice_send_set_control(&send, {100, 20, VoiceInputMode::voice_activity, 50, 0, false, false});
    feed(send, t, &now, 6, 12000);
    voice_send_flush(&send, transport);
    check(rec.count >= 7 && rec.order[rec.count - 1] == 1000 && rec.order[rec.count - 2] == 3,
          "Voice send pipeline: muting mid-sentence finishes with the silence tail and Speaking off");

    // Push-to-talk with a probed key and the 20 ms release delay.
    struct Key {
        bool down;
    } key{};
    voice_send_set_key_probe(
        &send, [](void* c, u8 vk) noexcept { return vk == 0x56 && static_cast<Key*>(c)->down; }, &key);
    voice_send_set_control(&send, {100, 20, VoiceInputMode::push_to_talk, 50, 0x56, false, true});
    rec.count = 0;
    feed(send, t, &now, 3, 12000);
    const bool silent_when_up = rec.count == 0 && !voice_send_queued(send);
    key.down = true;
    feed(send, t, &now, 4, 0);
    voice_send_flush(&send, transport);
    const bool transmits_when_down = rec.count == 5 && rec.order[0] == 1001 && send.cells.key_down;
    key.down = false;
    feed(send, t, &now, 8, 0);
    voice_send_flush(&send, transport);
    check(silent_when_up && transmits_when_down && rec.order[rec.count - 1] == 1000 &&
              !voice_send_speaking(send),
          "Voice send push-to-talk: the key gates transmission regardless of level, release ends with the tail");
    voice_send_close(&send);
}

// Capture pipeline -> transport encryption -> loopback UDP -> transport
// decryption -> RTP strip -> DAVE -> jitter buffer -> Opus -> mixer.
struct Loopback {
    NetworkLoop loop;
    VoiceUdpDiscovery discovery;
    VoiceUdpMedia media;
    VoicePacketCrypto sender, receiver;
    VoiceSend send;
    VoiceReceive receive;
    DaveSession passthrough;
    DaveFrameSealer sealer;
    DaveReceiveSender opener;
    KdfSha256Workspace kdf;
    bool synthetic_group;
    u32 sent;
    i16 heard[960 * 2 * 12];
};
bool loop_ready(void*) noexcept {
    return true;
}
DaveSealResult loop_seal(void* c, ConstSlice<u8> opus, Slice<u8> output) noexcept {
    auto& v = *static_cast<Loopback*>(c);
    return v.synthetic_group ? dave_frame_seal(v.sealer, v.kdf, opus, output)
                             : dave_session_encrypt(&v.passthrough, opus, output);
}
VoiceUdpMediaSend loop_send(void* c, ConstSlice<u8> packet) noexcept {
    auto& v = *static_cast<Loopback*>(c);
    const auto sent = voice_udp_media_send(&v.media, 1, packet);
    v.sent += sent == VoiceUdpMediaSend::accepted;
    return sent;
}
VoiceSocketSend loop_speaking(void*, u32) noexcept {
    return VoiceSocketSend::accepted;
}
DaveMediaResult loop_open(void* c, const VoiceReceivedPacket&, ConstSlice<u8> frame, Slice<u8> scratch,
                          Slice<u8> output) noexcept {
    auto& v = *static_cast<Loopback*>(c);
    DaveMediaResult result{};
    if (!v.synthetic_group)
        return dave_session_decrypt(&v.passthrough, kUser, frame, 1, scratch, output);
    const auto opened = dave_receive_sender_open(v.opener, {77, kUser}, v.kdf, frame, 1, scratch, output);
    result.bytes = opened.bytes;
    if (!opened.ok())
        result.error = DaveMediaError::not_ready;
    return result;
}
bool deliver(Loopback& v, u32 timeout) noexcept {
    const auto next = network_loop_poll(&v.loop, timeout);
    if (next.state == NetworkPollState::error)
        return false;
    if (next.state == NetworkPollState::completion) {
        voice_udp_media_completion(&v.media, next.completion);
        voice_udp_completion(&v.discovery, next.completion, GetTickCount64());
    }
    voice_udp_tick(&v.discovery, GetTickCount64());
    return true;
}
bool ignore(void*, ConstSlice<u8>, const VoicePacketResult&) noexcept {
    return true;
}
// Zero crossings of the left channel: a 400 Hz tone crosses 800 times a second.
u32 crossings(const i16* stereo, u32 frames) noexcept {
    u32 count = 0;
    for (u32 i = 1; i < frames; ++i)
        count += (stereo[(i - 1) * 2] < 0) != (stereo[i * 2] < 0);
    return count;
}
void loopback_case(Check check, bool synthetic_group) noexcept {
    auto* v = static_cast<Loopback*>(VirtualAlloc(nullptr, sizeof(Loopback), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!v || !network_loop_initialize(&v->loop).ok()) {
        check(false, "Voice send loopback: network loop starts");
        return;
    }
    v->synthetic_group = synthetic_group;
    static u8 rx[2048], tx[2048], wire[2048], plain[2048];
    u8 secret[32], seed[16];
    for (u32 i = 0; i < 32; ++i)
        secret[i] = static_cast<u8>(0x60 + i);
    for (u32 i = 0; i < 16; ++i)
        seed[i] = static_cast<u8>(0xa0 + i);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    i32 address_bytes = sizeof(address);
    sockaddr_in client{};
    i32 client_bytes = sizeof(client);
    const SOCKET peer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const DWORD timeout = 2000;
    bool ok = peer != INVALID_SOCKET && bind(peer, reinterpret_cast<const sockaddr*>(&address), address_bytes) == 0 &&
              getsockname(peer, reinterpret_cast<sockaddr*>(&address), &address_bytes) == 0 &&
              setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) ==
                  0 &&
              voice_packet_initialize(&v->sender, 1, voice_transport_aes256_gcm_rtpsize, {secret, 32}).ok() &&
              voice_packet_initialize(&v->receiver, 9, voice_transport_aes256_gcm_rtpsize, {secret, 32}).ok() &&
              kdf_sha256_initialize(&v->kdf).ok() &&
              dave_session_initialize(&v->passthrough, 1, kUser, 1, 0) == MlsStatus::ok &&
              dave_frame_sealer_initialize(&v->sealer, {seed, 16}).ok() &&
              dave_receive_sender_initialize(&v->opener, {77, kUser}, 0) == DaveSenderError::none &&
              dave_receive_sender_transition(v->opener, {77, kUser}, 1, {seed, 16}, 0, 1000).ok() &&
              voice_playback_initialize(&v->receive.playback) && voice_send_open(&v->send) &&
              voice_udp_begin(&v->discovery, &v->loop, reinterpret_cast<const sockaddr*>(&address), address_bytes,
                              kSsrc, GetTickCount64()) &&
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
                    client_bytes) == 74;
        const u64 deadline = GetTickCount64() + 3000;
        while (ok && v->discovery.phase == VoiceUdpPhase::discovering && GetTickCount64() < deadline)
            ok = deliver(*v, 50);
        ok = ok && v->discovery.phase == VoiceUdpPhase::ready &&
             voice_udp_media_begin(&v->media, &v->discovery, &v->sender, 1, {rx, sizeof(rx)}, {tx, sizeof(tx)},
                                   ignore, v);
    }
    const char* label = synthetic_group ? "Voice send loopback (synthetic DAVE group): transport attaches"
                                        : "Voice send loopback (DAVE passthrough): transport attaches";
    check(ok, label);
    voice_send_set_control(&v->send, {100, 20, VoiceInputMode::voice_activity, 50, 0, false, true});
    const VoiceSendTransport transport{v, 3, kSsrc, loop_ready, loop_seal, loop_send, loop_speaking};
    Tone t{};
    u64 now = kStart;
    constexpr u32 kFrames = 14;
    u32 received = 0;
    for (u32 f = 0; ok && f < kFrames; ++f) {
        feed(v->send, t, &now, 1, 10000);
        const u64 deadline = GetTickCount64() + 2000;
        while (ok && voice_send_queued(v->send) && GetTickCount64() < deadline) {
            voice_send_flush(&v->send, transport);
            ok = deliver(*v, voice_send_queued(v->send) ? 5 : 0);
        }
        // The peer is the voice server: open and hand the RTP to the receive path.
        while (ok && received < v->sent) {
            const i32 bytes = recv(peer, reinterpret_cast<char*>(wire), sizeof(wire), 0);
            const auto opened = bytes > 0 ? voice_packet_open(v->receiver, 9, {wire, static_cast<u32>(bytes)},
                                                              {plain, sizeof(plain)})
                                          : VoicePacketResult{};
            ok = bytes > 0 && opened.ok();
            if (!ok)
                break;
            VoiceReceivedPacket packet{};
            packet.transport = opened;
            packet.source = {kUser, 1, kSsrc, kSsrc, VoiceSourceKind::audio, true};
            packet.member = {kUser, 1, 0, 0, true, false, false};
            packet.setup_generation = packet.ready_revision = 1;
            ok = voice_receive_packet(&v->receive, packet, {plain, opened.bytes}, loop_open, v,
                                      kStart + u64{received} * 20'000) == VoiceReceiveResult::queued;
            ++received;
        }
    }
    for (u32 at = 0; ok && at < 960 * 12; at += 960)
        voice_playback_render(&v->receive.playback, {v->heard + at * 2, 960 * 2}, kStart + 400'000);
    // The jitter buffer keeps only its 40-80 ms target of the burst; judge the
    // steady 80 ms after the first (warm-up) frame.
    const u32 level = voice_pcm_level({v->heard + 960 * 2 * 2, 960 * 2 * 4});
    const u32 zero = crossings(v->heard + 960 * 2 * 2, 960 * 4);
    check(ok && received == kFrames && v->receive.stats.queued == kFrames && level > 3000 && zero >= 58 &&
              zero <= 70,
          synthetic_group ? "Voice send loopback (synthetic DAVE group): every frame arrives, decrypts and decodes "
                            "to the 400 Hz input tone"
                          : "Voice send loopback (DAVE passthrough): every frame arrives and decodes to the 400 Hz "
                            "input tone");
    voice_udp_media_stop(&v->media);
    voice_udp_close(&v->discovery);
    const u64 deadline = GetTickCount64() + 3000;
    while ((!voice_udp_drained(v->discovery) || !voice_udp_media_drained(v->media)) && GetTickCount64() < deadline)
        deliver(*v, 20);
    const bool drained = voice_udp_drained(v->discovery) && voice_udp_media_drained(v->media);
    voice_send_close(&v->send);
    voice_receive_stop(&v->receive);
    dave_receive_sender_release(&v->opener);
    dave_frame_sealer_release(&v->sealer);
    dave_session_release(&v->passthrough);
    kdf_sha256_release(&v->kdf);
    voice_packet_release(&v->sender);
    voice_packet_release(&v->receiver);
    if (peer != INVALID_SOCKET)
        closesocket(peer);
    if (drained) {
        network_loop_release(&v->loop);
        VirtualFree(v, 0, MEM_RELEASE);
    }
}

// Opt-in (MAICORD_VOICE_AUDIO_CAPTURE=1): opens the real default microphone.
struct Captured {
    volatile LONG64 samples, ticks;
};
void count_samples(void* context, ConstSlice<i16> mono, u64) noexcept {
    InterlockedAdd64(&static_cast<Captured*>(context)->samples, mono.count);
}
u64 count_ticks(void* context, u64) noexcept {
    InterlockedIncrement64(&static_cast<Captured*>(context)->ticks);
    return 0;
}
void test_device(Check check) noexcept {
    wchar_t flag[4]{};
    if (!GetEnvironmentVariableW(L"MAICORD_VOICE_AUDIO_CAPTURE", flag, 4) || flag[0] != L'1')
        return;
    static VoiceAudioDevice devices[16];
    const u32 count = voice_audio_devices(true, {devices, 16});
    check(count && devices[0].name_bytes, "Voice audio capture: the microphone list has a named default first");
    static VoiceAudioCapture capture;
    static Captured value;
    check(voice_audio_capture_start(&capture, nullptr, true, count_samples, count_ticks, &value),
          "Voice audio capture: thread starts");
    const u64 deadline = GetTickCount64() + 3000;
    while (GetTickCount64() < deadline && ReadAcquire64(&value.samples) < 48000)
        Sleep(20);
    const auto status = voice_audio_capture_status(capture);
    voice_audio_capture_stop(&capture);
    check(status.state == VoiceCaptureState::capturing && status.opens >= 1 &&
              ReadAcquire64(&value.samples) >= 48000 && ReadAcquire64(&value.ticks) > 20 &&
              !voice_audio_capture_running(capture),
          "Voice audio capture: WASAPI delivers 48 kHz mono events and stops cleanly");
}
} // namespace
void test_voice_send(Check check) noexcept {
    test_blocks(check);
    test_pipeline(check);
    loopback_case(check, false);
    loopback_case(check, true);
    test_device(check);
}
} // namespace maicord
