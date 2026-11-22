// The WebM Opus reader (src/media/containers/webm_opus.cpp), the audio adapter over it (mp4_fragment_audio.cpp) and
// the float output of the Opus decoder, on the fixture of tools/generate_webm_fixtures.py.
//
//   packets      every packet of the file, in order and after a seek to each index, against ffprobe's list
//   no Cues      the same file with its Cues turned into a Void element: the reader walks the Cluster headers
//   sparse       a file whose bytes arrive in pieces: data_pending names the missing byte and changes nothing
//   damage       a file cut at every length, and 4,000 files with changed bytes behind a page with no access:
//                an error value or packets inside the file, never a read outside it
//   adapter      the packet grid, the times and the trim that the session's audio pump works with
//   decoder      the float output equals the PCM16 output where that does not saturate, and keeps what it cuts
#include "../../fixtures/webm/webm_fixtures.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/media/audio/opus_decoder.hpp"
#include "maicord/media/containers/mp4_fragment_audio.hpp"
#include "maicord/media/containers/webm_opus.hpp"

namespace maicord {
namespace {
using E = Mp4DemuxError;
u32 g_failures;
void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

u32 load32(const u8* at) noexcept {
    return at[0] | (u32{at[1]} << 8) | (u32{at[2]} << 16) | (u32{at[3]} << 24);
}
constexpr u32 kPackets = 151, kFrames = 960, kDelay = 312, kDiscard = 648;
u32 listed_size(u32 packet) noexcept {
    return load32(kWebmOpusStereoPackets + 16 + packet * 8);
}
u32 listed_hash(u32 packet) noexcept {
    return load32(kWebmOpusStereoPackets + 20 + packet * 8);
}
u32 hash(ConstSlice<u8> bytes) noexcept {
    u32 value = 0x811C9DC5u;
    for (u32 i = 0; i < bytes.count; ++i)
        value = (value ^ bytes[i]) * 0x01000193u;
    return value;
}
bool listed(ConstSlice<u8> packet, u32 index) noexcept {
    return packet.count == listed_size(index) && hash(packet) == listed_hash(index);
}
constexpr ConstSlice<u8> kFile{kWebmOpusStereo, sizeof(kWebmOpusStereo)};

// Every packet in order, then a seek to each index and to the end.
bool reads_all(ConstSlice<u8> file, Mp4SourceAvailability availability = {}) noexcept {
    WebmOpus w{};
    if (webm_opus_initialize(&w, file, availability) != E::none || w.packet_count != kPackets)
        return false;
    ConstSlice<u8> packet{};
    bool ok = true;
    for (u32 i = 0; ok && i < kPackets; ++i) {
        ok = webm_opus_peek(&w, &packet) == E::none && listed(packet, i) && webm_opus_peek(&w, &packet) == E::none &&
             listed(packet, i) && w.packet_index == i;
        webm_opus_commit(&w);
    }
    ok = ok && webm_opus_peek(&w, &packet) == E::end_of_stream;
    for (u32 i = kPackets; ok && i--;)
        ok = webm_opus_seek(&w, i) == E::none && w.packet_index == i && webm_opus_peek(&w, &packet) == E::none &&
             listed(packet, i);
    return ok && webm_opus_seek(&w, kPackets) == E::none && webm_opus_peek(&w, &packet) == E::end_of_stream &&
           webm_opus_seek(&w, kPackets + 1) == E::invalid_input && w.packet_index == kPackets;
}

void packets() noexcept {
    WebmOpus w{};
    check(webm_present(kFile) && !webm_present({kFile.data + 1, 8}) && !webm_present({kFile.data, 3}) &&
              webm_opus_initialize(&w, kFile) == E::none && w.channels == 2 && w.pre_skip == kDelay &&
              w.frames_per_packet == kFrames && w.packet_count == kPackets && w.scale == 1'000'000 && w.track == 1 &&
              w.output_gain_q8 == 0 && w.cues && w.cues_end > w.cues && w.clusters > w.cues_end &&
              w.clusters_end == sizeof(kWebmOpusStereo),
          "WebM head: an Opus track of 2 channels in 20 ms packets, with its Cues before the first Cluster");
    check(w.timeline.source_frames == kPackets * kFrames && w.timeline.media_start_ticks == kDelay &&
              w.timeline.content_ticks == 144'000 && w.timeline.sample_rate == 48'000,
          "WebM timeline: 3 seconds stay after the codec delay and the padding of the last block");
    check(reads_all(kFile), "WebM packets: each packet matches the list, in order and after a seek to it");
    u64 cursor = 0;
    WebmOpusCluster cluster{};
    u32 clusters = 0, next_packet = 0;
    u64 next_offset = w.clusters;
    bool joined = true;
    while (webm_opus_next_cluster(w, &cursor, &cluster)) {
        joined = joined && cluster.offset == next_offset && cluster.first_packet == next_packet &&
                 cluster.end_packet > cluster.first_packet;
        next_offset = cluster.offset + cluster.bytes;
        next_packet = cluster.end_packet;
        ++clusters;
    }
    check(joined && clusters == 8 && next_packet == kPackets && next_offset == sizeof(kWebmOpusStereo),
          "WebM Cues: 8 Clusters cover the file and the packets without a gap");
}

alignas(16) u8 g_copy[sizeof(kWebmOpusStereo)];
// The offset of the Cues element in the fixture. The SeekHead names the same identifier first, as a value.
u32 cues_offset() noexcept {
    u32 found = 0;
    for (u32 i = 0; i < 700; ++i)
        if (kWebmOpusStereo[i] == 0x1C && kWebmOpusStereo[i + 1] == 0x53 && kWebmOpusStereo[i + 2] == 0xBB &&
            kWebmOpusStereo[i + 3] == 0x6B)
            found = i;
    return found;
}

void no_cues() noexcept {
    copy_bytes(g_copy, kWebmOpusStereo, sizeof(g_copy));
    const u32 at = cues_offset();
    // The Cues element (a 4-byte identifier, a 2-byte size) becomes a Void element of the same length.
    const u32 content = ((kWebmOpusStereo[at + 4] & 0x3Fu) << 8 | kWebmOpusStereo[at + 5]) + 3;
    g_copy[at] = 0xEC;
    g_copy[at + 1] = static_cast<u8>(0x40 | content >> 8);
    g_copy[at + 2] = static_cast<u8>(content);
    WebmOpus w{};
    check(at && webm_opus_initialize(&w, {g_copy, sizeof(g_copy)}) == E::none && !w.cues && w.packet_count == kPackets &&
              w.timeline.content_ticks == 144'000,
          "WebM without Cues: the reader finds the last Cluster from the Cluster headers");
    check(reads_all({g_copy, sizeof(g_copy)}), "WebM without Cues: each packet matches the list after a seek to it");
    u64 cursor = 0;
    WebmOpusCluster cluster{};
    check(!webm_opus_next_cluster(w, &cursor, &cluster), "WebM without Cues: the reader names no Cluster ranges");
}

// A file whose bytes arrive in blocks of 1 KiB, as a range fetch delivers them.
struct Sparse {
    bool present[64];
    u64 missing;
    u32 refused;
};
bool sparse_available(void* context, u64 offset, u64 bytes) noexcept {
    auto& s = *static_cast<Sparse*>(context);
    for (u64 block = offset / 1024; block <= (offset + maximum<u64>(bytes, 1) - 1) / 1024; ++block)
        if (block >= 64 || !s.present[block]) {
            s.missing = maximum<u64>(offset, block * 1024);
            ++s.refused;
            return false;
        }
    return true;
}

void sparse() noexcept {
    Sparse s{};
    const Mp4SourceAvailability availability{sparse_available, &s};
    WebmOpus w{}, empty{};
    u32 rounds = 0;
    E error = E::data_pending;
    bool untouched = true;
    while (rounds < 64 && (error = webm_opus_initialize(&w, kFile, availability)) == E::data_pending) {
        untouched = untouched && bytes_equal(&w, &empty, sizeof(w)) && s.missing < sizeof(kWebmOpusStereo);
        s.present[s.missing / 1024] = true;
        ++rounds;
    }
    u32 held = 0;
    for (const bool block : s.present)
        held += block;
    // The head, the first Cluster's first packet and the last Cluster: far less than the 45 blocks of the file.
    check(error == E::none && untouched && rounds == held && held >= 3 && held <= 10 && w.packet_count == kPackets,
          "WebM sparse: the reader opens from the head and the last Cluster, and asks for one missing block at a time");
    check(webm_opus_seek(&w, 75) == E::data_pending && w.packet_index == 0 && w.position == 0,
          "WebM sparse: a seek into bytes that are not there changes nothing");
    rounds = 0;
    while (rounds < 16 && (error = webm_opus_seek(&w, 75)) == E::data_pending) {
        s.present[s.missing / 1024] = true;
        ++rounds;
    }
    ConstSlice<u8> packet{};
    check(error == E::none && w.packet_index == 75, "WebM sparse: the seek succeeds when its Cluster is there");
    bool walked = true;
    u32 pending = 0;
    for (u32 i = 75; walked && i < kPackets;) {
        const auto before = w;
        error = webm_opus_peek(&w, &packet);
        if (error == E::data_pending) {
            walked = bytes_equal(&before, &w, sizeof(w));
            s.present[s.missing / 1024] = true;
            ++pending;
            continue;
        }
        walked = error == E::none && listed(packet, i);
        webm_opus_commit(&w);
        ++i;
    }
    check(walked && pending > 0 && webm_opus_peek(&w, &packet) == E::end_of_stream,
          "WebM sparse: a packet that is not there yet leaves the cursor, and the same packet comes when it is");
}

// Reads what a damaged file gives: at most 400 packets in order and three seeks. Every packet must lie inside it.
bool survives(ConstSlice<u8> file) noexcept {
    WebmOpus w{};
    if (webm_opus_initialize(&w, file) != E::none)
        return true;
    ConstSlice<u8> packet{};
    const u32 targets[]{0, w.packet_count / 2, w.packet_count ? w.packet_count - 1 : 0, w.packet_count};
    for (const u32 target : targets) {
        if (webm_opus_seek(&w, target) != E::none)
            continue;
        for (u32 i = 0; i < 100 && webm_opus_peek(&w, &packet) == E::none; ++i) {
            if (packet.data < file.data || packet.count > file.count ||
                static_cast<usize>(packet.data - file.data) > file.count - packet.count)
                return false;
            webm_opus_commit(&w);
        }
    }
    return true;
}

void damage() noexcept {
    // The file sits at the end of its pages, and the page after it has no access: a read past the end faults.
    constexpr usize kPage = 4096, kRoom = (sizeof(kWebmOpusStereo) + kPage - 1) & ~(kPage - 1);
    auto* pages = static_cast<u8*>(pages_reserve(kRoom + kPage));
    if (!pages || !pages_commit(pages, kRoom)) {
        check(false, "WebM damage: the guarded pages");
        return;
    }
    bool cut = true;
    WebmOpus w{};
    for (u32 length = 0; length < sizeof(kWebmOpusStereo); ++length) {
        u8* const file = pages + kRoom - length;
        copy_bytes(file, kWebmOpusStereo, length);
        reset_value(&w);
        // A cut file ends before the end that its Segment names.
        cut = cut && webm_opus_initialize(&w, {file, length}) != E::none && !w.encoded.data;
    }
    check(cut, "WebM damage: a file cut at any length is refused, and nothing reads past its end");
    u8* const file = pages + kRoom - sizeof(kWebmOpusStereo);
    u32 seed = 0x2545F491u, opened = 0;
    bool inside = true;
    const auto random = [&]() noexcept {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (u32 round = 0; inside && round < 4000; ++round) {
        copy_bytes(file, kWebmOpusStereo, sizeof(kWebmOpusStereo));
        // Half of the files have their changes in the head and the first Cluster, where the structure is.
        const u32 span = round & 1 ? static_cast<u32>(sizeof(kWebmOpusStereo)) : 1200u;
        for (u32 changes = 1 + random() % 4; changes; --changes) {
            const u32 at = random() % span;
            const u32 kind = random() % 3;
            file[at] = kind == 0 ? static_cast<u8>(random()) : kind == 1 ? 0xFF : static_cast<u8>(file[at] ^ (1u << (random() % 8)));
        }
        reset_value(&w);
        opened += webm_opus_initialize(&w, {file, sizeof(kWebmOpusStereo)}) == E::none;
        inside = survives({file, sizeof(kWebmOpusStereo)});
    }
    check(inside && opened > 100 && opened < 4000,
          "WebM damage: 4,000 files with changed bytes give an error or packets inside the file");
    pages_release(pages, kRoom + kPage);
}

void adapter() noexcept {
    WebmOpus w{};
    Mp4FragmentAudio audio{};
    Mp4FragmentAudioFormat format{};
    check(!mp4_fragment_audio_format(audio, &format) && mp4_fragment_audio_attach(&w, &audio) == E::invalid_input,
          "WebM adapter: an empty adapter has no format, and an empty reader does not attach");
    check(webm_opus_initialize(&w, kFile) == E::none && mp4_fragment_audio_attach(&w, &audio) == E::none &&
              audio.webm == &w && !audio.source && audio.packet_count == kPackets && audio.packet_index == 0 &&
              mp4_fragment_audio_attach(&w, &audio) == E::invalid_input,
          "WebM adapter: the adapter binds to the reader at its first packet, once");
    check(mp4_fragment_audio_format(audio, &format) && format.opus && format.sample_rate == 48'000 &&
              format.frames_per_packet == kFrames && format.channels == 2 && !format.configuration.count &&
              format.gain_q8 == 0,
          "WebM adapter: the format is Opus at 48 kHz, 2 channels, 960 frames in each packet");
    Mp4AudioSampleWindow window{};
    check(mp4_audio_sample_window(audio.timeline, 0, &window) && window.duration_100ns == 30'000'000 &&
              window.first_source_frame == kDelay && window.end_source_frame == kPackets * kFrames - kDiscard &&
              mp4_audio_sample_window(audio.timeline, 10'000'000, &window) && window.first_source_frame == kDelay + 48'000,
          "WebM adapter: the window of 3 seconds starts after the codec delay and ends before the padding");
    Mp4FragmentAudioPacket packet{};
    check(mp4_fragment_audio_refresh(&audio) == E::none && mp4_fragment_audio_peek(&audio, &packet) == E::none &&
              listed(packet.bytes, 0) && packet.first_source_frame == 0 && packet.presentation_time_100ns == -65'000 &&
              packet.duration_100ns == 200'000 && audio.pending,
          "WebM adapter: the first packet starts 6.5 ms before the content and lasts 20 ms");
    check(mp4_fragment_audio_commit(&audio) && audio.packet_index == 1 && !mp4_fragment_audio_commit(&audio) &&
              mp4_fragment_audio_peek(&audio, &packet) == E::none && listed(packet.bytes, 1) &&
              packet.first_source_frame == kFrames && packet.presentation_time_100ns == 135'000,
          "WebM adapter: a commit moves to the next packet, and only once for one peek");
    check(mp4_fragment_audio_seek(&audio, 100) == E::none && audio.packet_index == 100 && !audio.pending &&
              mp4_fragment_audio_peek(&audio, &packet) == E::none && listed(packet.bytes, 100) &&
              packet.first_source_frame == 100 * kFrames && packet.presentation_time_100ns == 19'935'000,
          "WebM adapter: a seek to packet 100 gives that packet at 1.9935 seconds");
    check(mp4_fragment_audio_seek(&audio, kPackets) == E::none && audio.packet_index == kPackets &&
              mp4_fragment_audio_peek(&audio, &packet) == E::end_of_stream &&
              mp4_fragment_audio_seek(&audio, kPackets + 1) == E::invalid_input && audio.packet_index == kPackets,
          "WebM adapter: the packet count is the end, and a packet past it is refused");
}

OpusDecoder g_plain, g_wide, g_loud, g_loud_wide;
i16 g_pcm16[kFrames * 2], g_work[kFrames * 2];
float g_float[kFrames * 2];

void decoder() noexcept {
    WebmOpus w{};
    check(webm_opus_initialize(&w, kFile) == E::none, "WebM decode: the fixture opens");
    opus_decoder_start(&g_plain, 2);
    opus_decoder_start(&g_wide, 2);
    // A header gain of +12 dB pushes the fixture's peaks above full scale.
    opus_decoder_start(&g_loud, 2, 12 * 256);
    opus_decoder_start(&g_loud_wide, 2, 12 * 256);
    ConstSlice<u8> packet{};
    bool decoded = true, ranges = true;
    float worst = 0, loud_peak = 0;
    i32 loud_pcm16_peak = 0;
    double energy = 0;
    u32 packets = 0;
    for (; packets < kPackets && webm_opus_peek(&w, &packet) == E::none; ++packets) {
        decoded = decoded && opus_decode(&g_plain, packet, {g_pcm16, kFrames * 2}) == kFrames &&
                  opus_decode_float(&g_wide, packet, {g_work, kFrames * 2}, g_float) == kFrames;
        ranges = ranges && opus_decoder_final_range(&g_plain) == opus_decoder_final_range(&g_wide);
        for (u32 n = 0; n < kFrames * 2; ++n) {
            const float difference = g_float[n] * 32768.f - static_cast<float>(g_pcm16[n]);
            worst = maximum(worst, difference < 0 ? -difference : difference);
            energy += static_cast<double>(g_float[n]) * g_float[n];
        }
        decoded = decoded && opus_decode(&g_loud, packet, {g_pcm16, kFrames * 2}) == kFrames &&
                  opus_decode_float(&g_loud_wide, packet, {g_work, kFrames * 2}, g_float) == kFrames;
        for (u32 n = 0; n < kFrames * 2; ++n) {
            loud_pcm16_peak = maximum<i32>(loud_pcm16_peak, g_pcm16[n] < 0 ? -g_pcm16[n] : g_pcm16[n]);
            loud_peak = maximum(loud_peak, g_float[n] < 0 ? -g_float[n] : g_float[n]);
        }
        webm_opus_commit(&w);
    }
    const double mean = energy / (kPackets * kFrames * 2);
    check(decoded && packets == kPackets && ranges && mean > 0.03 && mean < 0.2,
          "Opus float: each packet of the fixture decodes to 960 frames of the signal, with the range coder state of "
          "the PCM16 decode");
    check(worst <= 0.5001f, "Opus float: the float output times 32768 rounds to the PCM16 output, sample for sample");
    check(loud_pcm16_peak >= 32767 && loud_peak > 1.5f && loud_peak < 4.0f,
          "Opus float: with +12 dB of header gain the PCM16 output saturates and the float output keeps its peaks");
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_webm_opus_entry) {
    maicord::packets();
    maicord::no_cues();
    maicord::sparse();
    maicord::damage();
    maicord::adapter();
    maicord::decoder();
    maicord::test_exit(maicord::g_failures, "maicord_webm_opus");
}
