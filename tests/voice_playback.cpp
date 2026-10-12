#include "maicord/app/voice_mixer.hpp"
#include "maicord/app/voice_playback.hpp"
#include "maicord/app/soundboard_audio.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/voice_state_store.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
constexpr u64 kFrameUs = 20'000;
// Synthetic Opus packets: any payload is a decodable range-coded frame, so a
// seeded byte stream gives deterministic non-silent audio without an encoder.
enum class Kind : u8 { celt, silk_fec, silence };
u32 make_packet(Kind kind, u32 seed, u8* output) noexcept {
    if (kind == Kind::silence) {
        output[0] = 0xf8, output[1] = 0xff, output[2] = 0xfe;
        return 3;
    }
    u32 state = seed * 2654435761u + 12345u;
    const u32 bytes = kind == Kind::celt ? 121 : 61;
    for (u32 i = 1; i < bytes; ++i) {
        state = state * 1664525u + 1013904223u;
        output[i] = static_cast<u8>(state >> 24);
    }
    // CELT FB 20 ms stereo (config 31), or SILK WB 20 ms mono (config 9) whose
    // first frame byte sets VAD and LBRR, i.e. in-band FEC for the previous packet.
    output[0] = kind == Kind::celt ? 0xfc : 0x48;
    if (kind == Kind::silk_fec)
        output[1] |= 0xc0;
    return bytes;
}
struct DecoderPair {
    alignas(16) u8 a[32768], b[32768];
    OpusDecoder first, second;
};
void test_mixer(Check check) noexcept {
    i32 mix[4]{};
    const i16 pcm[4]{1000, -1000, 32767, -32768};
    voice_mixer_accumulate({mix, 4}, {pcm, 4}, 0);
    const bool silent = !mix[0] && !mix[3];
    voice_mixer_accumulate({mix, 4}, {pcm, 4}, 50);
    voice_mixer_accumulate({mix, 4}, {pcm, 4}, 200);
    check(silent && mix[0] == 2500 && mix[1] == -2500 && mix[2] == 32767 / 2 + 65534 &&
              mix[3] == -16384 - 65536,
          "Voice mixer: per-user gain 0/50/200 % scales before accumulation");
    voice_mixer_accumulate({mix, 4}, {pcm, 4}, 999);
    check(mix[0] == 4500, "Voice mixer: gain is capped at 200 %");
    bool monotonic = true;
    i16 previous = voice_mixer_soft_clip(-200000);
    for (i32 x = -200000; x <= 200000; x += 97) {
        const i16 y = voice_mixer_soft_clip(x);
        monotonic = monotonic && y >= previous && voice_mixer_soft_clip(-x) == -y;
        previous = y;
    }
    check(monotonic && voice_mixer_soft_clip(kVoiceSoftClipKnee) == kVoiceSoftClipKnee &&
              voice_mixer_soft_clip(-1234) == -1234 && voice_mixer_soft_clip(32767) < 32767 &&
              voice_mixer_soft_clip(32767) > 28000 && voice_mixer_soft_clip(2'000'000) <= 32767 &&
              voice_mixer_soft_clip(2'000'000) > 32700 && voice_mixer_soft_clip(-2'000'000) >= -32767,
          "Voice mixer: soft clip is identity below the knee, monotonic, symmetric and never wraps");
    const i32 sum[2]{65534, -65536};
    i16 out[2]{};
    voice_mixer_finish({sum, 2}, {out, 2});
    check(out[0] > 30000 && out[1] < -30000, "Voice mixer: two full-scale speakers saturate softly");
}
bool equal(const i16* a, const i16* b, u32 count) noexcept {
    return bytes_equal(a, b, count * static_cast<u32>(sizeof(i16)));
}
void test_concealment(Check check) noexcept {
    static DecoderPair pair;
    static i16 pcm[5760 * 2], fec[5760 * 2], plc[5760 * 2];
    const u32 bytes = opus_decoder_storage_bytes(2);
    auto& d = pair.first;
    check(bytes && bytes <= sizeof(pair.a) && opus_decoder_initialize(&d, {pair.a, bytes}, 2).ok(),
          "Opus PLC: stereo decoder initializes in caller storage");
    check(opus_decoder_conceal(&d, 960, {pcm, 1920}).ok() &&
              opus_decoder_conceal(&d, 961, {pcm, 1920}).error == OpusDecodeError::invalid_input &&
              opus_decoder_conceal(&d, 0, {pcm, 1920}).error == OpusDecodeError::invalid_input &&
              opus_decoder_conceal(&d, 5761, {pcm, 11520}).error == OpusDecodeError::invalid_input &&
              opus_decoder_conceal(&d, 960, {pcm, 1919}).error == OpusDecodeError::capacity &&
              opus_decoder_decode_fec(&d, {}, 960, {pcm, 1920}).error == OpusDecodeError::invalid_input &&
              opus_decoder_conceal(&d, 960, {reinterpret_cast<i16*>(pair.a), 1920}).error ==
                  OpusDecodeError::invalid_input,
          "Opus PLC: frame counts must be 2.5 ms multiples up to 120 ms with capacity and alias checks");
    u8 celt[200], silk[200], next[200];
    const u32 celt_bytes = make_packet(Kind::celt, 1, celt);
    const auto decoded = opus_decoder_decode(&d, {celt, celt_bytes}, {pcm, 1920});
    i32 energy = 0;
    for (u32 i = 0; i < 1920; ++i)
        energy |= pcm[i];
    const auto concealed = opus_decoder_conceal(&d, 960, {plc, 1920});
    i32 concealed_energy = 0;
    for (u32 i = 0; i < 1920; ++i)
        concealed_energy |= plc[i];
    check(decoded.ok() && decoded.frames == 960 && energy && concealed.ok() && concealed.frames == 960 &&
              concealed_energy,
          "Opus PLC: a lost frame after real audio is synthesized from decoder history");
    const u32 silk_bytes = make_packet(Kind::silk_fec, 2, silk);
    const u32 next_bytes = make_packet(Kind::silk_fec, 3, next);
    check(opus_packet_has_fec({next, next_bytes}) && !opus_packet_has_fec({celt, celt_bytes}) &&
              !opus_packet_has_fec({}) && opus_decoder_decode(&d, {silk, silk_bytes}, {pcm, 1920}).ok(),
          "Opus FEC: LBRR presence is detected from the following packet");
    // Clone the decoder so FEC and plain concealment start from the same history.
    copy_bytes(pair.b, pair.a, bytes);
    pair.second = {pair.b, bytes, 2, true};
    const auto recovered = opus_decoder_decode_fec(&d, {next, next_bytes}, 960, {fec, 1920});
    const auto guessed = opus_decoder_conceal(&pair.second, 960, {plc, 1920});
    check(recovered.ok() && recovered.frames == 960 && guessed.ok() && !equal(fec, plc, 1920),
          "Opus FEC: one lost frame is rebuilt from the next packet's LBRR, not merely concealed");
    copy_bytes(pair.b, pair.a, bytes);
    const auto celt_fec = opus_decoder_decode_fec(&d, {celt, celt_bytes}, 960, {fec, 1920});
    const auto celt_plc = opus_decoder_conceal(&pair.second, 960, {plc, 1920});
    check(celt_fec.ok() && celt_plc.ok() && equal(fec, plc, 1920),
          "Opus FEC: without LBRR the FEC path falls back to identical concealment");
    opus_decoder_release(&pair.first);
    opus_decoder_release(&pair.second);
}
// Pushes `count` packets from `first` (skipping `skip`) and renders `frames`.
struct Run {
    VoicePlayback* playback;
    u64 now_us;
};
void feed(Run& run, u32 ssrc, u64 user, Kind kind, u16 first, u16 count, i32 skip = -1, u32 flags = 1) noexcept {
    u8 packet[200];
    for (u16 i = 0; i < count; ++i) {
        const u16 sequence = static_cast<u16>(first + i);
        if (static_cast<i32>(sequence) == skip)
            continue;
        const u32 bytes = make_packet(kind, sequence + ssrc * 1000, packet);
        voice_playback_push(run.playback, ssrc, user, flags, sequence, sequence * 960u, {packet, bytes},
                            run.now_us + i * kFrameUs);
    }
}
u64 render(Run& run, i16* output, u32 frames) noexcept {
    u64 level = 0;
    for (u32 at = 0; at < frames; at += 480) {
        voice_playback_render(run.playback, {output + at * 2, 960}, run.now_us);
        level += voice_pcm_level({output + at * 2, 960});
        run.now_us += 10'000;
    }
    return level;
}
void test_playback(Check check) noexcept {
    static VoicePlayback playback;
    static i16 a[9600], b[9600], both[9600];
    check(voice_playback_initialize(&playback) && !voice_playback_initialize(&playback),
          "Voice playback: one bounded block for streams, decoders and mix");
    check(playback.stream_count == 0, "Voice playback: no stream or decoder is committed before a speaker");
    Run run{&playback, 1'000'000};
    feed(run, 1, 11, Kind::silence, 0, 6);
    const u64 silent = render(run, a, 1920);
    check(!silent && playback.stats.decoded == 2, "Voice playback: Opus silence frames decode to digital silence");
    check(playback.stream_count == 1, "Voice playback: one speaker commits one stream");
    voice_playback_clear(&playback);
    feed(run, 1, 11, Kind::celt, 0, 6);
    const u64 level_a = render(run, a, 4800);
    // Mix at 50 % so neither speaker alone reaches the soft-clip knee.
    voice_playback_set_user(&playback, 11, 50, false);
    voice_playback_set_user(&playback, 22, 50, false);
    voice_playback_clear(&playback);
    feed(run, 1, 11, Kind::celt, 0, 6);
    const u64 half = render(run, a, 4800);
    voice_playback_clear(&playback);
    feed(run, 2, 22, Kind::celt, 0, 6);
    render(run, b, 4800);
    voice_playback_clear(&playback);
    feed(run, 1, 11, Kind::celt, 0, 6);
    feed(run, 2, 22, Kind::celt, 0, 6);
    render(run, both, 4800);
    bool mixed = level_a > 0;
    for (u32 i = 0; i < 9600; ++i)
        mixed = mixed && both[i] == voice_mixer_soft_clip(i32{a[i]} + b[i]);
    check(mixed, "Voice playback: two speakers mix sample-exactly through the soft clipper");
    voice_playback_set_user(&playback, 22, 100, false);
    check(voice_playback_set_user(&playback, 11, 200, false) && !voice_playback_set_user(&playback, 11, 201, false),
          "Voice playback: per-user gain accepts 0-200 %");
    voice_playback_clear(&playback);
    feed(run, 1, 11, Kind::celt, 0, 6);
    const u64 loud = render(run, b, 4800);
    voice_playback_set_user(&playback, 11, 100, true);
    voice_playback_clear(&playback);
    feed(run, 1, 11, Kind::celt, 0, 6);
    const u64 muted = render(run, b, 4800);
    u64 users[4]{};
    u64 next = 0;
    const u32 muted_speaking = voice_playback_speaking(&playback, run.now_us, {users, 4}, &next);
    check(loud > level_a && half * 200 >= level_a * 95 && half * 200 <= level_a * 105 && !muted && !muted_speaking &&
              playback.user_count == 1,
          "Voice playback: 50 % gain halves, 200 % is louder, local mute is silent and hides speaking");
    voice_playback_set_user(&playback, 11, 100, false);
    check(!playback.user_count,
          "Voice playback: default settings remove the user entry");

    voice_playback_clear(&playback);
    const u64 concealed = playback.stats.concealed, recovered = playback.stats.recovered;
    feed(run, 1, 11, Kind::celt, 0, 6, 3);
    render(run, a, 4800);
    check(playback.stats.concealed == concealed + 1 && playback.stats.recovered == recovered,
          "Voice playback: a lost CELT packet is concealed");
    voice_playback_clear(&playback);
    feed(run, 1, 11, Kind::silk_fec, 0, 6, 3);
    render(run, a, 4800);
    check(playback.stats.recovered == recovered + 1, "Voice playback: a lost packet is recovered from SILK LBRR");

    voice_playback_clear(&playback);
    feed(run, 1, 11, Kind::celt, 0, 4);
    render(run, a, 1920);
    const u32 speaking = voice_playback_speaking(&playback, run.now_us, {users, 4}, &next);
    const u32 after = voice_playback_speaking(&playback, next, {users + 2, 2}, nullptr);
    check(speaking == 1 && users[0] == 11 && next > run.now_us && next <= run.now_us + kVoiceSpeakingHoldUs &&
              !after,
          "Voice playback: decoded energy marks the user speaking until the hold expires");

    const u64 decoded = playback.stats.decoded;
    voice_playback_set_deafened(&playback, true);
    feed(run, 1, 11, Kind::celt, 20, 4, -1, 1);
    feed(run, 2, 22, Kind::celt, 20, 4, -1, 2);
    bool deaf = true;
    render(run, a, 1920);
    for (u32 i = 0; i < 3840; ++i)
        deaf = deaf && !a[i];
    const u32 deaf_speaking = voice_playback_speaking(&playback, run.now_us, {users, 4}, nullptr);
    check(deaf && playback.stats.decoded == decoded && deaf_speaking == 1 && users[0] == 11,
          "Voice playback: deafen renders silence without decoding; speaking follows packets and op 5 flags");
    voice_playback_set_deafened(&playback, false);

    voice_playback_clear(&playback);
    const u64 dropped = playback.stats.dropped;
    u8 packet[4];
    const u32 bytes = make_packet(Kind::silence, 0, packet);
    for (u32 ssrc = 100; ssrc < 100 + kVoicePlaybackStreams + 1; ++ssrc)
        voice_playback_push(&playback, ssrc, ssrc, 1, 0, 0, {packet, bytes}, run.now_us);
    const bool full = playback.stats.dropped == dropped + 1 && playback.stream_count == kVoicePlaybackStreams;
    const bool evicted =
        voice_playback_push(&playback, 500, 500, 1, 0, 0, {packet, bytes}, run.now_us + kVoiceStreamIdleUs + 1);
    check(full && evicted, "Voice playback: the stream table is bounded and idle streams are reused");
    voice_playback_release(&playback);
    check(!voice_playback_push(&playback, 1, 11, 1, 0, 0, {packet, bytes}, 0),
          "Voice playback: released playback refuses packets");
}
void test_speaking_marks(Check check) noexcept {
    static VoiceState records[4];
    VoiceStateStore store{};
    voice_state_store_initialize(&store, {records, 4});
    voice_state_store_apply(&store, 1, 7, 11, voice_self_mute);
    voice_state_store_apply(&store, 1, 7, 22, 0);
    voice_state_store_apply(&store, 1, 8, 33, 0);
    const u64 speaking[]{11, 33};
    const u32 revision = store.revision;
    const bool marked = voice_state_store_set_speaking(&store, 7, {speaking, 2}) &&
                        store.revision == revision + 1 &&
                        !voice_state_store_set_speaking(&store, 7, {speaking, 2});
    voice_state_store_apply(&store, 1, 7, 11, 0);
    const auto* kept = voice_state_store_find(store, 1, 11);
    const auto* other = voice_state_store_find(store, 1, 33);
    check(marked && kept && kept->flags == voice_speaking && other && !other->flags &&
              !(voice_state_store_find(store, 1, 22)->flags & voice_speaking),
          "Voice speaking marks: only the connected channel's speakers are marked and survive Gateway updates");
    check(voice_state_store_set_speaking(&store, 0, {}) && !kept->flags,
          "Voice speaking marks: leaving clears every mark");
}
// Soundboard effects ride the call mix after the voices, scaled by the output
// volume, and deafening ends them instead of queueing them.
void test_soundboard_effects(Check check) noexcept {
    static VoicePlayback playback{};
    static SoundboardMixer mixer{};
    static i16 pcm[2 * 960], output[2 * 480];
    for (auto& sample : pcm)
        sample = 8000;
    auto* clip = soundboard_clip_from_pcm(42, {pcm, 2 * 960});
    const bool ready = clip && voice_playback_initialize(&playback);
    voice_playback_set_effects(&playback, &mixer);
    voice_playback_set_master(&playback, 50);
    soundboard_mixer_play(&mixer, clip, 1000, 9);
    voice_playback_render(&playback, {output, 2 * 480}, 1'000);
    check(ready && voice_playback_effects(&playback) == &mixer && output[0] >= 3999 && output[0] <= 4000 &&
              output[959] == output[0] && soundboard_mixer_active(&mixer) == 1,
          "Voice playback: soundboard sounds mix into the call output at the output volume");
    voice_playback_set_deafened(&playback, true);
    voice_playback_render(&playback, {output, 2 * 480}, 21'000);
    soundboard_mixer_play(&mixer, clip, 1000, 9);
    voice_playback_render(&playback, {output, 2 * 480}, 41'000);
    check(!output[0] && soundboard_mixer_active(&mixer) == 0,
          "Voice playback: deafened calls stop soundboard sounds instead of resuming them later");
    voice_playback_set_deafened(&playback, false);
    voice_playback_set_effects(&playback, nullptr);
    voice_playback_release(&playback);
    soundboard_clip_release(clip);
}
} // namespace
void test_voice_playback(Check check) noexcept {
    test_soundboard_effects(check);
    test_mixer(check);
    test_concealment(check);
    test_playback(check);
    test_speaking_marks(check);
}
} // namespace maicord
