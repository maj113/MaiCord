#include "maicord/media/mp3_stream.hpp"
#include "fixtures/mp3_original_tone.hpp"

namespace maicord {
namespace {
bool close(float a, float b, float tolerance = 0.0001f) noexcept {
    const float difference = a - b;
    return difference >= -tolerance && difference <= tolerance;
}
u32 silence(u8* bytes, u32 count, bool stereo = false, bool vbr = false) noexcept {
    u32 offset = 0;
    for (u32 i = 0; i < count; ++i) {
        const bool high = vbr && (i & 1);
        const u32 frame_bytes = high ? 522u : 417u;
        clear_bytes(bytes + offset, frame_bytes);
        bytes[offset] = 0xff;
        bytes[offset + 1] = 0xfb;
        bytes[offset + 2] = high ? 0xa0 : 0x90;
        bytes[offset + 3] = stereo ? 0 : 0xc0;
        offset += frame_bytes;
    }
    return offset;
}
void big32(u8* p, u32 value) noexcept {
    p[0] = static_cast<u8>(value >> 24);
    p[1] = static_cast<u8>(value >> 16);
    p[2] = static_cast<u8>(value >> 8);
    p[3] = static_cast<u8>(value);
}
struct Cancel {
    u32 calls{}, after{};
};
bool cancel(void* context) noexcept {
    auto& state = *static_cast<Cancel*>(context);
    return ++state.calls >= state.after;
}
} // namespace
void test_mp3_stream(void (*check)(bool, const char*) noexcept) noexcept {
    alignas(kMp3DecoderStateAlignment) u8 storage[8192]{}, saved[8192]{};
    float pcm[kMp3MaximumFrames * 2 + 2]{};
    u8 bytes[64 * 522 + 256]{};
    const u32 state_bytes = mp3_decoder_storage_bytes();
    check(state_bytes && state_bytes <= sizeof(storage),
          "MP3 caller-owned low-level state is bounded below8KiB");
    if (!state_bytes || state_bytes > sizeof(storage))
        return;
    Mp3Decoder decoder{};
    check(
        mp3_decoder_initialize(&decoder, {storage + 1, state_bytes}, 44100, 1) == Mp3Error::invalid_input &&
            mp3_decoder_initialize(&decoder, {storage, state_bytes - 1}, 44100, 1) == Mp3Error::capacity &&
            mp3_decoder_initialize(&decoder, {storage, state_bytes}, 12345, 1) == Mp3Error::invalid_input &&
            mp3_decoder_initialize(&decoder, {storage, state_bytes}, 44100, 3) == Mp3Error::invalid_input &&
            !decoder.initialized && mp3_decoder_reset(&decoder) == Mp3Error::invalid_input,
        "MP3 rejects invalid state alignment, capacity, native rate and channel count before mutation");
    const u32 mono_bytes = silence(bytes, 4);
    Mp3Stream stream{};
    check(mp3_stream_initialize(&stream, {bytes, mono_bytes}) == Mp3Error::none && stream.ready &&
              stream.sample_rate == 44100 && stream.channels == 1 && stream.frame_count == 4 &&
              stream.raw_frames == 4608 && stream.total_frames == 4608 && !stream.gapless,
          "MP3 original four-frame mono silence has exact decoded duration without inferred trim");
    check(mp3_decoder_initialize(&decoder, {storage, state_bytes}, 44100, 1) == Mp3Error::none,
          "MP3 native decoder binds caller state only");
    copy_bytes(saved, storage, state_bytes);
    for (float& sample : pcm)
        sample = 123;
    const auto packet = mp3_stream_next(&stream);
    check(packet.ok() && packet.bytes.count == 417 && packet.duration_frames == 1152 &&
              !packet.trim_front_frames && !packet.trim_back_frames &&
              mp3_decoder_decode(&decoder, packet.bytes, {pcm, 1151}).error == Mp3Error::capacity &&
              mp3_decoder_decode(&decoder, {storage, 4}, {pcm, 2304}).error == Mp3Error::invalid_input &&
              mp3_decoder_decode(&decoder, packet.bytes, {reinterpret_cast<float*>(storage), 2304}).error ==
                  Mp3Error::invalid_input &&
              bytes_equal(saved, storage, state_bytes) && pcm[0] == 123,
          "MP3 output and state alias/capacity preflight preserve native state and PCM");
    auto decoded = mp3_decoder_decode(&decoder, packet.bytes, {pcm, 1152});
    bool silent = decoded.ok() && decoded.frames == 1152 && pcm[1152] == 123;
    for (u32 i = 0; i < 1152; ++i)
        silent = silent && pcm[i] == 0;
    check(silent && mp3_decoder_reset(&decoder) == Mp3Error::none,
          "MP3 real LayerIII mono decoder emits1152 silence frames and preserves sentinel");
    mp3_decoder_release(&decoder);

    // MPEG2 and2.5 LayerIII use one576-sample granule and smaller side info.
    for (u32 lower = 0; lower < 2; ++lower) {
        const u32 rate = lower ? 11025u : 22050u, size = lower ? 208u : 104u;
        clear_bytes(bytes, size * 2);
        for (u32 i = 0; i < 2; ++i) {
            bytes[i * size] = 0xff;
            bytes[i * size + 1] = lower ? 0xe3 : 0xf3;
            bytes[i * size + 2] = 0x40;
            bytes[i * size + 3] = 0xc0;
        }
        bool low_ok = mp3_stream_initialize(&stream, {bytes, size * 2}) == Mp3Error::none &&
                      stream.sample_rate == rate && stream.frames_per_packet == 576 &&
                      stream.total_frames == 1152 &&
                      mp3_decoder_initialize(&decoder, {storage, state_bytes}, rate, 1) == Mp3Error::none;
        for (u32 i = 0; i < 2 && low_ok; ++i) {
            const auto next = mp3_stream_next(&stream);
            decoded = mp3_decoder_decode(&decoder, next.bytes, {pcm, 576});
            low_ok = next.ok() && decoded.ok() && decoded.frames == 576;
            for (u32 j = 0; j < 576; ++j)
                low_ok = low_ok && pcm[j] == 0;
        }
        check(low_ok, "MP3 real MPEG2/2.5 mono decode retains native rate and576-frame granule duration");
        mp3_decoder_release(&decoder);
    }
    const u32 stereo_bytes = silence(bytes, 3, true, true);
    check(mp3_stream_initialize(&stream, {bytes, stereo_bytes}) == Mp3Error::none && stream.channels == 2 &&
              stream.total_frames == 3456 &&
              mp3_decoder_initialize(&decoder, {storage, state_bytes}, 44100, 2) == Mp3Error::none,
          "MP3 stereo VBR permits bitrate changes while retaining native format and exact frame duration");
    bool stereo_ok = true;
    for (u32 i = 0; i < 3; ++i) {
        const auto next = mp3_stream_next(&stream);
        for (float& sample : pcm)
            sample = 123;
        decoded = mp3_decoder_decode(&decoder, next.bytes, {pcm, 2304});
        stereo_ok = stereo_ok && next.ok() && decoded.ok() && decoded.frames == 1152 && pcm[2304] == 123;
        for (u32 j = 0; j < 2304; ++j)
            stereo_ok = stereo_ok && pcm[j] == 0;
    }
    check(stereo_ok && mp3_stream_next(&stream).error == Mp3Error::end_of_stream,
          "MP3 real stereo VBR decodes every frame and reaches exact terminal boundary");
    mp3_decoder_release(&decoder);

    silence(bytes, 4);
    Mp3Stream retained = stream;
    const u32 original_count = stream.frame_count;
    Cancel stopped{0, 3};
    check(mp3_stream_initialize(&retained, {bytes, 1668}, cancel, &stopped) == Mp3Error::cancelled &&
              retained.frame_count == original_count &&
              mp3_stream_initialize(&retained, {bytes, 1667}) == Mp3Error::truncated &&
              retained.frame_count == original_count,
          "MP3 scan cancellation and truncated last frame preserve prior descriptor");
    bytes[417 + 2] = 0x94; // Switch44.1k to48k inside one source.
    check(mp3_stream_initialize(&retained, {bytes, 1668}) == Mp3Error::unsupported,
          "MP3 format transitions reject instead of silently changing output format");
    bytes[417 + 2] = 0x90;
    bytes[2] = 0;
    check(mp3_stream_initialize(&retained, {bytes, 1668}) == Mp3Error::unsupported,
          "MP3 free-format requires unsupported boundary authority and is explicitly refused");
    silence(bytes, 4);
    bytes[4] = 0xff;
    bytes[5] = 0x80;
    check(mp3_stream_initialize(&retained, {bytes, 1668}) == Mp3Error::malformed,
          "MP3 initial cut reservoir cannot invent absent source bytes");
    clear_bytes(bytes, 10);
    copy_bytes(bytes, "ID3", 3);
    bytes[3] = 4;
    silence(bytes + 10, 4);
    check(mp3_stream_initialize(&stream, {bytes, 1678}) == Mp3Error::none && stream.audio_begin == 10,
          "MP3 bounded ID3v2 prefix excludes metadata from frame authority");
    bytes[6] = 0x80;
    check(mp3_stream_initialize(&retained, {bytes, 1678}) == Mp3Error::malformed,
          "MP3 ID3 syncsafe size high-bit corruption is rejected");
    bytes[6] = 0;
    bytes[8] = 127;
    bytes[9] = 127;
    check(mp3_stream_initialize(&retained, {bytes, 1678}) == Mp3Error::truncated,
          "MP3 oversized ID3 metadata cannot advance outside the borrowed source");
    clear_bytes(bytes, 20);
    copy_bytes(bytes, "ID3", 3);
    bytes[3] = 4;
    bytes[5] = 0x10;
    copy_bytes(bytes + 10, "3DI", 3);
    copy_bytes(bytes + 13, bytes + 3, 7);
    silence(bytes + 20, 4);
    check(mp3_stream_initialize(&stream, {bytes, 1688}) == Mp3Error::none && stream.audio_begin == 20,
          "MP3 ID3v2.4 footer must match bounded header and is excluded from PCM source");
    bytes[19] = 1;
    check(mp3_stream_initialize(&retained, {bytes, 1688}) == Mp3Error::malformed,
          "MP3 inconsistent ID3 footer cannot redirect frame origin");
    silence(bytes, 4);
    clear_bytes(bytes + 1668, 160);
    copy_bytes(bytes + 1668, "APETAGEX", 8);
    bytes[1676] = 0xd0;
    bytes[1677] = 7; // APE2 version2000, empty footer size32.
    bytes[1680] = 32;
    copy_bytes(bytes + 1700, "TAG", 3);
    check(mp3_stream_initialize(&stream, {bytes, 1828}) == Mp3Error::none && stream.audio_end == 1668,
          "MP3 bounded APE footer and ID3v1 suffix preserve frame duration without decoding tag bytes");
    bytes[1680] = 255;
    bytes[1681] = 255;
    check(mp3_stream_initialize(&retained, {bytes, 1828}) == Mp3Error::malformed,
          "MP3 APE size overflow is refused before source-bound subtraction");

    const u32 tagged_bytes = silence(bytes, 5);
    copy_bytes(bytes + 21, "Xing", 4);
    big32(bytes + 25, 3);
    big32(bytes + 29, 4);
    big32(bytes + 33, tagged_bytes);
    copy_bytes(bytes + 37, "LAME3.100", 9);
    constexpr u32 encoder_delay = 576, encoder_padding = 1000;
    bytes[58] = static_cast<u8>(encoder_delay >> 4);
    bytes[59] = static_cast<u8>((encoder_delay << 4) | (encoder_padding >> 8));
    bytes[60] = static_cast<u8>(encoder_padding);
    check(mp3_stream_initialize(&stream, {bytes, tagged_bytes}) == Mp3Error::none && stream.gapless &&
              stream.audio_begin == 417 && stream.delay_frames == 1105 && stream.padding_frames == 471 &&
              stream.total_frames == 3032,
          "MP3 Xing/LAME source counts and decoder synthesis delay produce exact audible trim");
    u64 audible = 0;
    for (;;) {
        const auto next = mp3_stream_next(&stream);
        if (next.error == Mp3Error::end_of_stream)
            break;
        if (!next.ok())
            break;
        audible += next.duration_frames - next.trim_front_frames - next.trim_back_frames;
    }
    check(audible == stream.total_frames,
          "MP3 packet front/back trims sum exactly to metadata audible duration");
    big32(bytes + 29, 5);
    check(mp3_stream_initialize(&retained, {bytes, tagged_bytes}) == Mp3Error::malformed,
          "MP3 stale Xing frame count cannot override actual scanned duration");
    big32(bytes + 29, 4);
    bytes[59] = static_cast<u8>(encoder_delay << 4);
    bytes[60] = 1;
    check(mp3_stream_initialize(&retained, {bytes, tagged_bytes}) == Mp3Error::unsupported,
          "MP3 unrepresentable LAME tail trim is explicit unsupported rather than fabricated samples");

    check(mp3_stream_initialize(&stream, {kMp3OriginalTone, sizeof(kMp3OriginalTone)}) == Mp3Error::none &&
              stream.total_frames == kMp3OriginalToneFrames && stream.frame_count == 5 && !stream.gapless &&
              mp3_decoder_initialize(&decoder, {storage, state_bytes}, 44100, 1) == Mp3Error::none,
          "MP3 original nonzero LAME tone raw frames admit exact5760 native frames");
    float reference[kMp3OriginalToneFrames]{};
    u32 written = 0;
    bool tone_ok = true;
    while (written < kMp3OriginalToneFrames) {
        const auto next = mp3_stream_next(&stream);
        decoded = mp3_decoder_decode(&decoder, next.bytes, {pcm, 2304});
        if (!next.ok() || !decoded.ok() || decoded.frames != next.duration_frames) {
            tone_ok = false;
            break;
        }
        copy_bytes(reference + written, pcm, decoded.frames * sizeof(float));
        written += decoded.frames;
    }
    float peak = 0;
    for (float value : reference) {
        const float magnitude = value < 0 ? -value : value;
        if (magnitude > peak)
            peak = magnitude;
    }
    for (const auto& point : kMp3OriginalToneReference)
        tone_ok = tone_ok && close(reference[point.frame], point.value);
    check(tone_ok && written == kMp3OriginalToneFrames && peak > 0.11f && peak < 0.13f,
          "MP3 real reservoir tone matches17 independent FFmpeg float samples within1e-4 and nonzero "
          "amplitude");
    mp3_decoder_release(&decoder);

    // Repeated original packets create a longer real reservoir/DSP trajectory;
    // compare every audible sought sample against independently decoded linear PCM.
    u8 longer[sizeof(kMp3OriginalTone) * 6]{};
    for (u32 i = 0; i < 6; ++i)
        copy_bytes(longer + i * sizeof(kMp3OriginalTone), kMp3OriginalTone, sizeof(kMp3OriginalTone));
    float linear[kMp3OriginalToneFrames * 6]{};
    bool long_ok = mp3_stream_initialize(&stream, {longer, sizeof(longer)}) == Mp3Error::none &&
                   mp3_decoder_initialize(&decoder, {storage, state_bytes}, 44100, 1) == Mp3Error::none;
    written = 0;
    while (long_ok && written < stream.total_frames) {
        const auto next = mp3_stream_next(&stream);
        decoded = mp3_decoder_decode(&decoder, next.bytes, {pcm, 2304});
        long_ok = next.ok() && decoded.ok() && decoded.frames == next.duration_frames;
        if (long_ok) {
            copy_bytes(linear + written, pcm, decoded.frames * sizeof(float));
            written += decoded.frames;
        }
    }
    Mp3SeekPoint points[4]{};
    Mp3SeekIndex index{};
    long_ok = long_ok && mp3_stream_build_index(stream, &index, {points, 4}) == Mp3Error::none;
    check(
        long_ok && index.point_count == 4 && points[3].frame_index == 24,
        "MP3 four-point caller bank sparsely indexes30 real reservoir frames without proportional seeking");
    const u64 targets[]{0, 1000, 15001, 23040, 34459, 34560};
    for (u64 target : targets) {
        bool seek_ok = long_ok && mp3_decoder_reset(&decoder) == Mp3Error::none &&
                       mp3_stream_seek(&stream, target, &index) == Mp3Error::none;
        u64 offset = target;
        while (seek_ok) {
            const auto next = mp3_stream_next(&stream);
            if (next.error == Mp3Error::end_of_stream)
                break;
            if (!next.ok()) {
                seek_ok = false;
                break;
            }
            decoded = mp3_decoder_decode(&decoder, next.bytes, {pcm, 2304});
            const u32 kept = next.duration_frames - next.trim_front_frames - next.trim_back_frames;
            if (decoded.error == Mp3Error::need_preroll && !kept)
                continue;
            if (!decoded.ok() || decoded.frames != next.duration_frames) {
                seek_ok = false;
                break;
            }
            for (u32 i = 0; i < kept; ++i) {
                seek_ok = seek_ok && offset < stream.total_frames &&
                          close(pcm[next.trim_front_frames + i], linear[offset], 0.000001f);
                ++offset;
            }
        }
        check(seek_ok && offset == stream.total_frames,
              "MP3 exact sparse seek primes reservoir/DSP and reproduces every linear suffix sample");
    }
    const Mp3Stream before = stream;
    stopped = {0, 1};
    check(mp3_stream_seek(&stream, 1000, &index, cancel, &stopped) == Mp3Error::cancelled &&
              stream.cursor_offset == before.cursor_offset && stream.cursor_frame == before.cursor_frame &&
              stream.discard_before == before.discard_before,
          "MP3 cancelled seek preserves exact cursor and discard authority");
    index.source = kMp3OriginalTone;
    check(mp3_stream_seek(&stream, 1000, &index) == Mp3Error::invalid_input,
          "MP3 source-index identity mismatch cannot reinterpret checkpoint offsets");
    stopped = {0, 4};
    check(mp3_stream_build_index(stream, &index, {points, 4}, cancel, &stopped) == Mp3Error::cancelled &&
              !index.point_count,
          "MP3 cancelled index rebuild withdraws partially overwritten checkpoint authority");
    mp3_decoder_release(&decoder);
    check(!decoder.initialized && !decoder.state &&
              mp3_decoder_decode(&decoder, {}, {pcm, 2304}).error == Mp3Error::invalid_input,
          "MP3 release detaches caller state and inactive decode never invokes native core");
}
} // namespace maicord
