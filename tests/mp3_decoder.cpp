#include "maicord/media/mp3_stream.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "fixtures/mp3_feature_streams.hpp"

namespace maicord {
namespace {
constexpr u32 kPage = 4096;
constexpr float kSentinel = 1234.5f;

u64 hash_pcm(const float* pcm, u32 count) noexcept {
    u64 hash = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < count; ++i) {
        u32 bits;
        copy_bytes(&bits, pcm + i, sizeof(bits));
        for (u32 b = 0; b < 4; ++b)
            hash = (hash ^ ((bits >> (8 * b)) & 0xff)) * 0x100000001b3ull;
    }
    return hash;
}
bool finite(const float* pcm, u32 count) noexcept {
    u32 nonfinite = 0;
    for (u32 i = 0; i < count; ++i) {
        u32 bits;
        copy_bytes(&bits, pcm + i, sizeof(bits));
        nonfinite |= static_cast<u32>((bits & 0x7f800000u) == 0x7f800000u);
    }
    return !nonfinite;
}
ConstSlice<u8> feature(u32 index) noexcept {
    return {kMp3FeatureStreams + kMp3FeatureStreamList[index].offset, kMp3FeatureStreamList[index].bytes};
}

// Decodes every packet into pcm; frames written, or ~0u on any error other
// than a leading reservoir preroll.
u32 decode_all(ConstSlice<u8> source, u8* storage, float* pcm, u32 capacity) noexcept {
    Mp3Stream stream{};
    Mp3Decoder decoder{};
    if (mp3_stream_initialize(&stream, source) != Mp3Error::none ||
        mp3_decoder_initialize(&decoder, {storage, mp3_decoder_storage_bytes()}, stream.sample_rate,
                               stream.channels) != Mp3Error::none)
        return ~0u;
    u32 frames = 0;
    for (;;) {
        const auto packet = mp3_stream_next(&stream);
        if (packet.error == Mp3Error::end_of_stream)
            break;
        if (!packet.ok() || (frames + packet.duration_frames) * stream.channels > capacity)
            return ~0u;
        const auto decoded =
            mp3_decoder_decode(&decoder, packet.bytes, {pcm + frames * stream.channels, 2304});
        if (decoded.error == Mp3Error::need_preroll && !frames)
            continue;
        if (!decoded.ok() || decoded.frames != packet.duration_frames)
            return ~0u;
        frames += decoded.frames;
    }
    mp3_decoder_release(&decoder);
    return frames;
}

// MSB-first writer for synthetic headers and side information.
struct BitWriter {
    u8* bytes;
    u32 at;
    void put(u32 value, u32 bits) noexcept {
        for (u32 i = bits; i--; ++at)
            bytes[at >> 3] = static_cast<u8>(bytes[at >> 3] | ((value >> i & 1) << (7 - (at & 7))));
    }
};
// One MPEG-1 Layer III 44.1 kHz 128 kbps frame (417 bytes): mono granules with
// the given side information, every main-data byte `fill`.
struct Granule {
    u32 part2_3, big_values, global_gain, scalefac_compress, window_switching, block_type, table;
};
void mono_frame(u8* frame, u32 main_data_begin, const Granule& g, u8 fill) noexcept {
    clear_bytes(frame, 417);
    frame[0] = 0xff;
    frame[1] = 0xfb;
    frame[2] = 0x90;
    frame[3] = 0xc0;
    BitWriter w{frame + 4, 0};
    w.put(main_data_begin, 9);
    w.put(0, 5 + 4);
    for (u32 gr = 0; gr < 2; ++gr) {
        w.put(g.part2_3, 12);
        w.put(g.big_values, 9);
        w.put(g.global_gain, 8);
        w.put(g.scalefac_compress, 4);
        w.put(g.window_switching, 1);
        if (g.window_switching) {
            w.put(g.block_type, 2);
            w.put(0, 1);
            w.put(g.table, 5);
            w.put(g.table, 5);
            w.put(0, 9);
        } else {
            w.put(g.table, 5);
            w.put(g.table, 5);
            w.put(g.table, 5);
            w.put(15, 4);
            w.put(7, 3);
        }
        w.put(0, 3);
    }
    for (u32 i = 4 + 17; i < 417; ++i)
        frame[i] = fill;
}

struct Guarded {
    u8* memory;
    u8* end() const noexcept {
        return memory + kPage * 16;
    }
};
Guarded guarded() noexcept {
    auto* memory =
        static_cast<u8*>(VirtualAlloc(nullptr, kPage * 17, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD old = 0;
    if (memory && !VirtualProtect(memory + kPage * 16, kPage, PAGE_NOACCESS, &old)) {
        VirtualFree(memory, 0, MEM_RELEASE);
        memory = nullptr;
    }
    return {memory};
}

// Leaves a recognizable pattern in the stack below the caller's frame.
__declspec(noinline) u32 dirty_stack(u8 pattern) noexcept {
    volatile u8 junk[48 * 1024];
    for (u32 i = 0; i < sizeof(junk); ++i)
        junk[i] = pattern;
    return junk[pattern];
}
} // namespace

void test_mp3_decoder(void (*check)(bool, const char*) noexcept) noexcept {
    alignas(kMp3DecoderStateAlignment) static u8 storage[8192], other[8192];
    static float pcm[6912 * 2 + 2304], pcm2[6912 * 2 + 2304];

    // Conformance: decoder paths LAME never emits, against libmad.
    bool features = true;
    for (u32 s = 0; s < sizeof(kMp3FeatureStreamList) / sizeof(kMp3FeatureStreamList[0]); ++s) {
        const auto& expected = kMp3FeatureStreamList[s];
        Mp3Stream stream{};
        const u32 frames = decode_all(feature(s), storage, pcm, sizeof(pcm) / sizeof(pcm[0]));
        bool ok = mp3_stream_initialize(&stream, feature(s)) == Mp3Error::none &&
                  stream.sample_rate == expected.sample_rate && stream.channels == expected.channels &&
                  frames == expected.frames &&
                  hash_pcm(pcm, frames * expected.channels) == expected.pcm_hash;
        for (const auto& reference : expected.reference) {
            const float difference = pcm[reference.index] - reference.value;
            ok = ok && difference < 4e-6f && difference > -4e-6f;
        }
        features = features && ok;
    }
    check(features,
          "MP3 CRC, M/S, intensity (MPEG-1 and MPEG-2), mixed blocks and MPEG-2.5 short blocks match "
          "libmad within 4e-6 with exact PCM hashes");

    // A determinism check: the same packets decode to the same PCM and the
    // same state bytes regardless of what earlier stack frames left behind.
    {
        Mp3Stream first{}, second{};
        Mp3Decoder a{}, b{};
        bool same =
            mp3_stream_initialize(&first, feature(1)) == Mp3Error::none &&
            mp3_decoder_initialize(&a, {storage, mp3_decoder_storage_bytes()}, 44100, 2) ==
                Mp3Error::none &&
            mp3_decoder_initialize(&b, {other, mp3_decoder_storage_bytes()}, 44100, 2) == Mp3Error::none;
        second = first;
        while (same) {
            const auto p = mp3_stream_next(&first), q = mp3_stream_next(&second);
            if (p.error == Mp3Error::end_of_stream)
                break;
            dirty_stack(0xa5);
            const auto r = mp3_decoder_decode(&a, p.bytes, {pcm, 2304});
            dirty_stack(0x5a);
            const auto t = mp3_decoder_decode(&b, q.bytes, {pcm2, 2304});
            same = r.ok() && t.ok() && bytes_equal(pcm, pcm2, 2304 * sizeof(float)) &&
                   bytes_equal(storage, other, mp3_decoder_storage_bytes());
        }
        mp3_decoder_release(&a);
        mp3_decoder_release(&b);
        check(same, "MP3 decoding is deterministic: identical PCM and state bytes after every packet");
    }

    // Packets cover the audio exactly once, in order: no frame decodes twice.
    {
        Mp3Stream stream{};
        bool once = mp3_stream_initialize(&stream, feature(0)) == Mp3Error::none;
        u32 offset = stream.audio_begin;
        u64 frames = 0;
        while (once) {
            const auto packet = mp3_stream_next(&stream);
            if (packet.error == Mp3Error::end_of_stream)
                break;
            once = packet.ok() && packet.bytes.data == feature(0).data + offset &&
                   packet.decoded_first_frame == frames;
            offset += packet.bytes.count;
            frames += packet.duration_frames;
        }
        check(once && offset == stream.audio_end && frames == stream.total_frames &&
                  mp3_stream_next(&stream).error == Mp3Error::end_of_stream,
              "MP3 packets tile the audio once in order and stay at the end");
    }

    // Side information bounds.
    u8 frame[417 * 2];
    const Granule silent{0, 0, 0, 0, 0, 0, 0};
    Granule g = silent;
    mono_frame(frame, 0, g, 0);
    Mp3Stream stream{};
    const bool valid = mp3_stream_initialize(&stream, {frame, 417}) == Mp3Error::none;
    g.big_values = 289;
    mono_frame(frame, 0, g, 0);
    const bool big_values = mp3_stream_initialize(&stream, {frame, 417}) == Mp3Error::malformed;
    g = silent;
    g.window_switching = 1;
    mono_frame(frame, 0, g, 0);
    const bool block_type = mp3_stream_initialize(&stream, {frame, 417}) == Mp3Error::malformed;
    g = silent;
    g.part2_3 = (417 - 21) * 4 + 1; // Both granules together one bit past the main data.
    mono_frame(frame, 0, g, 0);
    const bool overrun = mp3_stream_initialize(&stream, {frame, 417}) == Mp3Error::malformed;
    check(
        valid && big_values && block_type && overrun,
        "MP3 side information over 288 big values, block type 0 with window switching or Huffman bits past "
        "the main data is malformed");

    // Huffman decoding stays inside the granule's bits and the frame for any
    // main data: frames end at a no-access page, and the reservoir is read.
    const Guarded guard = guarded();
    check(guard.memory != nullptr, "MP3 guard pages");
    if (guard.memory) {
        bool bounded = true;
        const u8 fills[]{0x00, 0xff, 0x55, 0x80, 0x0f};
        for (u8 fill : fills) {
            for (u32 table = 1; table < 32 && bounded; table += 3) {
                // The first frame leaves 50 main-data bytes; the second reads them back.
                g = {(417 - 21 - 50) * 4, 288, 255, 15, 0, 0, table};
                mono_frame(frame, 0, g, fill);
                g.part2_3 = (417 - 21 + 50) * 4;
                mono_frame(frame + 417, 50, g, static_cast<u8>(fill ^ 0x3c));
                u8* source = guard.end() - 417 * 2;
                copy_bytes(source, frame, 417 * 2);
                for (float& sample : pcm)
                    sample = kSentinel;
                const u32 frames = decode_all({source, 417 * 2}, storage, pcm, 2304 + 1);
                bounded = frames == 2304 && finite(pcm, 2304) && pcm[2304] == kSentinel;
            }
        }
        check(bounded,
              "MP3 Huffman reads stay within part2_3_length and the frame for adversarial main data");
        // Full scale is 1.0 and nothing clips: the loudest global gain decodes
        // past it, finite.
        g = {(417 - 21) * 4, 288, 255, 0, 0, 0, 31};
        mono_frame(frame, 0, g, 0x33); // Table 24 codes with long linbits, not its all-ones (0, 0).
        float peak = 0;
        const u32 loud = decode_all({frame, 417}, storage, pcm, 1152);
        for (u32 i = 0; i < 1152 && loud == 1152; ++i)
            peak = pcm[i] > peak ? pcm[i] : -pcm[i] > peak ? -pcm[i] : peak;
        check(loud == 1152 && finite(pcm, 1152) && peak > 1.0f, "MP3 float PCM is unclipped and finite");

        // VBR tag fields are bounded by the frame: a 72-byte MPEG-2.5 8 kbps
        // Xing frame asking for frames, bytes, TOC and scale.
        u8 tiny[72]{0xff, 0xe3, 0x18, 0xc4};
        copy_bytes(tiny + 13, "Xing\0\0\0\x0f", 8);
        u8* end = guard.end() - sizeof(tiny);
        copy_bytes(end, tiny, sizeof(tiny));
        const bool xing_bounded =
            mp3_stream_initialize(&stream, {end, sizeof(tiny)}) == Mp3Error::truncated;
        // A frame count with its top bit set is a plain mismatch, not an
        // allocation size: MP3 streams allocate nothing.
        g = silent;
        mono_frame(frame, 0, g, 0);
        copy_bytes(frame + 417, frame, 417);
        copy_bytes(frame + 21, "Xing\0\0\0\x01\xff\xff\xff\xff", 12);
        const bool xing_frames = mp3_stream_initialize(&stream, {frame, 417 * 2}) == Mp3Error::malformed;
        check(xing_bounded && xing_frames,
              "MP3 Xing fields are bounded by the frame and checked against the scan");

        // Mutated feature streams at a page end: every packet the scan admits
        // decodes to finite PCM within its output.
        u32 random = 0x2545f491, admitted = 0;
        bool robust = true;
        for (u32 i = 0; i < 3000 && robust; ++i) {
            const auto source = feature(i % 5);
            u8* input = guard.end() - source.count;
            copy_bytes(input, source.data, source.count);
            for (u32 k = 0; k <= i % 4; ++k) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                input[(random >> 8) % source.count] ^= static_cast<u8>(random | 1);
            }
            for (float& sample : pcm)
                sample = kSentinel;
            const u32 frames = decode_all({input, source.count}, storage, pcm, 6912 * 2);
            if (frames == ~0u)
                continue;
            ++admitted;
            robust = finite(pcm, frames * kMp3FeatureStreamList[i % 5].channels) &&
                     pcm[frames * kMp3FeatureStreamList[i % 5].channels] == kSentinel;
        }
        check(robust && admitted > 100, "MP3 mutated streams decode to finite, bounded PCM or are refused");
        VirtualFree(guard.memory, 0, MEM_RELEASE);
    }
}
} // namespace maicord
