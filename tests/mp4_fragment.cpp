#include "maicord/media/mp4_fragment.hpp"
#include "maicord/media/mp4_fragment_source.hpp"
#include "maicord/media/mp4_fragment_audio.hpp"
#include "maicord/media/mp4_segment_store.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/platform/aac_decoder.hpp"
#include "maicord/platform/h264_gpu_decoder.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/app/account_video_session.hpp"
#include "maicord/app/progressive_video_fetch.hpp"
#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/writer.hpp"
#include "test_output.hpp"

namespace maicord {
bool fragment_audio_playback_probe() noexcept;
namespace {
u32 failures;
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
struct Writer {
    u8 data[2048]{}; u32 size{};
    void word(u32 n) noexcept { byte_store_be32(data + size, n); size += 4; }
    void zero(u32 n) noexcept { for (u32 i = 0; i < n; ++i) data[size++] = 0; }
    u32 begin(const char* kind) noexcept {
        const u32 start = size; word(0);
        for (u32 i = 0; i < 4; ++i) data[size++] = static_cast<u8>(kind[i]);
        return start;
    }
    void end(u32 start) noexcept { byte_store_be32(data + start, size - start); }
    ConstSlice<u8> view() const noexcept { return {data, size}; }
};
struct InitFixture {
    Writer w;
    u32 track_id{}, trex_id{}, scale{}, reference{}, entry{}, config{}, stts{}, description{}, rate{}, edit{}, mvex{}, moov{}, trak{}, trak_bytes{};
};
InitFixture initialization(bool audio, bool wide = false) noexcept {
    InitFixture f{}; auto& w = f.w;
    u32 start = w.begin("ftyp"); w.word(0x64617368); w.word(0); w.word(0x69736f36); w.end(start);
    const u32 moov = w.begin("moov");
    f.moov = moov;
    start = w.begin("mvhd"); const u32 movie = w.size; w.zero(wide ? 112 : 100);
    byte_store_be32(w.data + movie, wide ? 0x01000000 : 0);
    byte_store_be32(w.data + movie + (wide ? 20 : 12), 1000); w.end(start);
    const u32 trak = w.begin("trak");
    f.trak = trak;
    start = w.begin("tkhd"); const u32 track = w.size; w.zero(wide ? 96 : 84);
    byte_store_be32(w.data + track, wide ? 0x01000003 : 3); f.track_id = track + (wide ? 20 : 12); byte_store_be32(w.data + f.track_id, 7);
    byte_store_be32(w.data + track + (wide ? 56 : 44), 0x10000);
    byte_store_be32(w.data + track + (wide ? 64 : 52), 0xffff0000); w.end(start);
    const u32 edts = w.begin("edts"); start = w.begin("elst"); w.word(wide ? 0x01000000 : 0); w.word(2);
    if (wide) w.word(0); w.word(1000); if (wide) w.word(0xffffffff); w.word(0xffffffff); w.word(0x10000);
    if (wide) w.word(0); w.word(0); if (wide) w.word(0);
    f.edit = w.size; w.word(1024); f.rate = w.size; w.word(0x10000); w.end(start); w.end(edts);
    const u32 mdia = w.begin("mdia");
    start = w.begin("mdhd"); const u32 mdhd = w.size; w.zero(wide ? 36 : 24); f.scale = mdhd + (wide ? 20 : 12);
    byte_store_be32(w.data + mdhd, wide ? 0x01000000 : 0);
    byte_store_be32(w.data + f.scale, audio ? 44100 : 15360); byte_store_be32(w.data + f.scale + 4, 0xffffffff);
    if (wide) byte_store_be32(w.data + f.scale + 8, 0xffffffff);
    w.end(start);
    start = w.begin("hdlr"); const u32 handler = w.size; w.zero(24);
    byte_store_be32(w.data + handler + 8, audio ? 0x736f756e : 0x76696465); w.end(start);
    const u32 minf = w.begin("minf"), dinf = w.begin("dinf"), dref = w.begin("dref");
    w.word(0); w.word(1); start = w.begin("url "); f.reference = w.size; w.word(1); w.end(start); w.end(dref); w.end(dinf);
    const u32 stbl = w.begin("stbl"), stsd = w.begin("stsd"); w.word(0); f.description = w.size; w.word(1);
    f.entry = w.begin(audio ? "mp4a" : "avc1"); const u32 entry = w.size; w.zero(audio ? 28 : 78);
    w.data[entry + 7] = 1;
    if (audio) {
        w.data[entry + 17] = 2; byte_store_be32(w.data + entry + 24, 44100u << 16);
        f.config = w.begin("esds"); w.word(0);
        const u8 esds[]{3,25,0,1,0,4,17,0x40,0x15,0,0,0,0,0,0xfa,0,0,0,0xfa,0,5,2,0x12,0x10,6,1,2};
        for (u8 b : esds) w.data[w.size++] = b;
        w.end(f.config);
    } else {
        w.data[entry + 24] = 2; w.data[entry + 25] = 128; w.data[entry + 26] = 1; w.data[entry + 27] = 104;
        f.config = w.begin("avcC");
        // Container-description fixture; actual SPS/PPS decode has separate tests.
        const u8 config[]{1,66,0,30,255,225,0,2,0x67,0,1,0,2,0x68,0};
        for (u8 b : config) w.data[w.size++] = b;
        w.end(f.config);
        start = w.begin("pasp"); w.word(4); w.word(3); w.end(start);
    }
    w.end(f.entry); w.end(stsd);
    f.stts = w.begin("stts"); w.word(0); w.word(0); w.end(f.stts);
    start = w.begin("stsc"); w.word(0); w.word(0); w.end(start);
    start = w.begin("stsz"); w.word(0); w.word(0); w.word(0); w.end(start);
    start = w.begin("stco"); w.word(0); w.word(0); w.end(start);
    w.end(stbl); w.end(minf); w.end(mdia); w.end(trak);
    f.trak_bytes = w.size - trak;
    f.mvex = w.begin("mvex"); start = w.begin("trex"); w.word(0); f.trex_id = w.size;
    w.word(7); w.word(1); w.word(audio ? 1024 : 512); w.word(0); w.word(0); w.end(start); w.end(f.mvex); w.end(moov);
    return f;
}
void track_tests() noexcept {
    using E = Mp4DemuxError;
    const bool variants[]{false, true};
    for (bool audio : variants) {
        const auto f = initialization(audio);
        Mp4FragmentTrack track{};
        check(mp4_fragment_track_initialize(f.w.view(), audio, &track) == E::none &&
              track.audio == audio && track.defaults.track_id == 7 && !track.media_duration &&
              track.timescale == (audio ? 44100u : 15360u) && track.movie_timescale == 1000 &&
              track.empty_edit == 1000 && !track.edit_duration && track.media_start == 1024 && track.edited &&
              track.rotation == 1 && track.configuration.data >= f.w.data &&
              track.configuration.data + track.configuration.count <= f.w.data + f.w.size,
              "fragmented initialization retains exact media/movie timing and borrowed codec configuration");
        check(audio ? track.configuration.count == 2 && track.sample_rate == 44100 && track.channels == 2 &&
                        track.frames_per_packet == 1024 && !track.nal_length_bytes
                    : track.configuration.count == 15 && track.nal_length_bytes == 4 && track.width == 640 &&
                        track.height == 360 && track.sar_width == 4 && track.sar_height == 3 && !track.channels,
              "fragmented initialization reuses validated AVC and AAC description interpretation");
        const auto saved = track;
        const auto wide = initialization(audio, true);
        Mp4FragmentTrack wide_track{};
        check(mp4_fragment_track_initialize(wide.w.view(), audio, &wide_track) == E::none &&
              wide_track.defaults.track_id == 7 && wide_track.timescale == track.timescale &&
              wide_track.movie_timescale == 1000 && wide_track.rotation == 1 && !wide_track.media_duration &&
              wide_track.media_start == 1024 && wide_track.empty_edit == 1000 && !wide_track.edit_duration,
              "64-bit track/movie headers and edit lists preserve the same timeline and unknown duration");
        for (u32 n = 0; n < f.w.size; ++n)
            check(mp4_fragment_track_initialize({f.w.data, n}, audio, &track) != E::none &&
                  bytes_equal(&track, &saved, sizeof(track)), "truncated initialization cannot replace the retained track");
        const auto reject = [&](u32 offset, u32 value, E expected) noexcept {
            auto bad = f; byte_store_be32(bad.w.data + offset, value);
            check(mp4_fragment_track_initialize(bad.w.view(), audio, &track) == expected &&
                  bytes_equal(&track, &saved, sizeof(track)), "invalid initialization fails without changing borrowed track metadata");
        };
        reject(f.scale, 0, E::malformed);
        reject(f.track_id, 0, E::malformed);
        reject(f.trex_id, 8, E::malformed);
        reject(f.reference, 0, E::unsupported);
        reject(f.description, 2, E::malformed);
        reject(f.stts + 12, 1, E::malformed);
        reject(f.mvex + 4, 0x66726565, E::malformed);
        reject(f.rate, 0x20000, E::unsupported);
        reject(f.edit, 0xffffffff, E::unsupported);
        reject(f.config + 4, 0x73696e66, E::unsupported);
        reject(f.entry + 4, audio ? 0x656e6361 : 0x656e6376, E::unsupported);
        if (!audio) reject(f.config + 27, 0x61766343, E::malformed); // pasp becomes a duplicate avcC.
        auto duplicate = f;
        copy_bytes(duplicate.w.data + duplicate.w.size, f.w.data + f.trak, f.trak_bytes);
        duplicate.w.size += f.trak_bytes; duplicate.w.end(f.moov);
        check(mp4_fragment_track_initialize(duplicate.w.view(), audio, &track) == E::malformed &&
              bytes_equal(&track, &saved, sizeof(track)), "duplicate track identities cannot reuse the same fragment defaults");
        auto unknown = f; byte_store_be32(unknown.w.data + 8, 0); byte_store_be32(unknown.w.data + 16, 0);
        check(mp4_fragment_track_initialize(unknown.w.view(), audio, &track) == E::unsupported &&
              bytes_equal(&track, &saved, sizeof(track)), "unknown incompatible brands do not claim ISO fragment playback");
        check(mp4_fragment_track_initialize(f.w.view(), !audio, &track) == (audio ? E::no_video : E::no_audio) &&
              bytes_equal(&track, &saved, sizeof(track)), "initialization cannot substitute a different media handler");
    }
}
struct Fixture {
    Writer w;
    u32 tfhd{}, tfdt{}, trun{}, trun2{}, mfhd{}, mdat{}, span{}, samples{}, roll{}, groups{};
};
Fixture fragment(bool second_offset = true, bool absolute_base = false, bool wide_mdat = false, bool roll = false) noexcept {
    Fixture f{}; auto& w = f.w;
    const u32 moof = w.begin("moof");
    f.mfhd = w.begin("mfhd"); w.word(0); w.word(7); w.end(f.mfhd);
    const u32 traf = w.begin("traf");
    f.tfhd = w.begin("tfhd"); w.word(absolute_base ? 0x3b : 0x2003a); w.word(7);
    if (absolute_base) { w.word(0); w.word(1000); }
    w.word(1); w.word(100); w.word(4); w.word(0); w.end(f.tfhd);
    f.tfdt = w.begin("tfdt"); w.word(0x01000000); w.word(0); w.word(1000); w.end(f.tfdt);
    f.trun = w.begin("trun"); w.word(0x01000f01); w.word(3); const u32 offset = w.size; w.word(0);
    w.word(100); w.word(4); w.word(0); w.word(static_cast<u32>(-20));
    w.word(110); w.word(6); w.word(0x10000); w.word(30);
    w.word(90); w.word(5); w.word(0); w.word(0); w.end(f.trun);
    f.trun2 = w.begin("trun"); w.word(second_offset ? 5 : 4); w.word(2);
    const u32 second = w.size; if (second_offset) w.word(0);
    w.word(0x10000); w.end(f.trun2);
    if (roll) {
        f.roll = w.begin("sgpd"); w.word(0x01000000); w.word(0x726f6c6c); w.word(2); w.word(0); w.end(f.roll);
        f.groups = w.begin("sbgp"); w.word(0); w.word(0x726f6c6c); w.word(1); w.word(5); w.word(0); w.end(f.groups);
    }
    w.end(traf); w.end(moof);
    f.mdat = w.size;
    const u32 payload = w.size + (wide_mdat ? 16 : 8);
    byte_store_be32(w.data + offset, payload);
    if (second_offset) byte_store_be32(w.data + second, payload + 17);
    w.word(wide_mdat ? 1 : second_offset ? 33 : 31); w.word(0x6d646174);
    if (wide_mdat) { w.word(0); w.word(second_offset ? 41 : 39); }
    f.span = w.size + (second_offset ? 25 : 23); f.samples = 5;
    return f;
}
void empty_roll_tests() noexcept {
    using E = Mp4DemuxError;
    const auto f = fragment(true, false, false, true);
    Mp4Fragment parsed{};
    const Mp4FragmentDefaults defaults{7, 100, 4, 0};
    check(mp4_fragment_initialize(f.w.view(), 0, f.span, defaults, &parsed) == E::none &&
          parsed.count == 5 && parsed.first_time == 1000 && parsed.duration == 500,
          "empty roll descriptions and zero-index groups retain ordinary sample timing");
    const auto prior = parsed;
    const auto reject = [&](u32 offset, u32 value, E error) noexcept {
        auto bad = f; byte_store_be32(bad.w.data + offset, value);
        check(mp4_fragment_initialize(bad.w.view(), 0, bad.span, defaults, &parsed) == error &&
              bytes_equal(&parsed, &prior, sizeof(prior)), "unsupported or malformed roll metadata leaves the prior fragment untouched");
    };
    reject(f.roll + 12, 0x73656967, E::unsupported);
    reject(f.roll + 20, 1, E::unsupported);
    reject(f.groups + 24, 1, E::unsupported);
    reject(f.groups + 20, 6, E::malformed);
    reject(f.groups + 20, 0, E::malformed);
    reject(f.groups + 16, 2, E::malformed);
}
void multipart_segment_tests() noexcept {
    using E = Mp4DemuxError;
    Writer file{}; u32 starts[2]{}; const auto f = fragment();
    for (u32 i = 0; i < 2; ++i) {
        const auto event = file.begin("emsg"); file.zero(12); file.end(event);
        starts[i] = file.size;
        copy_bytes(file.data + file.size, f.w.data, f.w.size); file.size += f.w.size; file.zero(f.span - f.w.size);
        byte_store_be32(file.data + starts[i] + f.tfdt + 16, 1000 + 500 * i);
    }
    const auto init = initialization(false); Mp4FragmentTrack track{}; Mp4MediaSegment media{};
    check(mp4_fragment_track_initialize(init.w.view(), false, &track) == E::none &&
        mp4_media_segment_initialize(file.view(), track, &media) == E::none && media.samples == 10 &&
        media.fragment.count == 5 && media.range.time == 1000 && media.range.duration == 1000 &&
        media.presentation_end == 2000, "unindexed multipart file derives its whole timeline without a sample table");
    const auto saved = media;
    Mp4Fragment following{}; u32 next = 99;
    check(mp4_media_segment_fragment(file.view(), track, media.next_fragment, &following, &next) == E::none &&
        following.first_time == 1500 && next == file.size && following.metadata.data == file.data + starts[1],
        "multipart reader skips events and borrows only the next fragment metadata");
    const auto retained = following;
    check(mp4_media_segment_fragment(file.view(), track, next, &following, &next) == E::end_of_stream &&
        next == file.size && bytes_equal(&following, &retained, sizeof(retained)), "multipart EOF preserves the last accepted cursor");
    constexpr u32 discontinuities[]{1499, 1501};
    for (u32 time : discontinuities) {
        auto bad = file; byte_store_be32(bad.data + starts[1] + f.tfdt + 16, time);
        check(mp4_media_segment_initialize(bad.view(), track, &media) == E::malformed &&
            bytes_equal(&media, &saved, sizeof(saved)), "unmarked overlapping or discontinuous fragments are rejected transactionally");
    }
    check(mp4_media_segment_initialize({file.data, file.size - 1}, track, &media) == E::malformed &&
        bytes_equal(&media, &saved, sizeof(saved)), "a partial final fragment cannot publish a partial file timeline");
    Mp4SegmentStore store{}; Mp4FragmentSource source{};
    check(mp4_segment_store_initialize(&store, init.w.view(), false, 1, 1000, track.timescale) == E::none &&
        mp4_segment_store_request(&store, 0) == E::data_pending &&
        mp4_segment_store_accept(&store, 0, store.request_serial, file.view()) == E::none &&
        mp4_fragment_source_initialize_segments(&store, &source) == E::none, "multipart file enters the bounded source store");
    const u64 clock[]{1000, 1100, 1210, 1300, 1400, 1500, 1600, 1710, 1800, 1900};
    for (u32 i = 0; i < 10; ++i) {
        Mp4FragmentPacket packet{}, repeated{};
        check(mp4_fragment_source_peek(&source, &packet) == E::none && packet.sample.decode_time == clock[i] &&
            mp4_fragment_source_peek(&source, &repeated) == E::none && repeated.bytes.data == packet.bytes.data &&
            repeated.sample.decode_time == packet.sample.decode_time && mp4_fragment_source_commit(&source),
            "multipart source advances across inner boundaries exactly once and preserves backpressure");
    }
    Mp4FragmentPacket packet{}; Mp4FragmentPosition position{};
    check(mp4_fragment_source_peek(&source, &packet) == E::end_of_stream &&
        mp4_fragment_source_prepare(&source, 0, 7, &position) == E::none && position.packet.sample.decode_time == 1710 &&
        mp4_fragment_source_adopt(&source, position) && mp4_fragment_source_peek(&source, &packet) == E::none &&
        packet.sample.decode_time == 1710, "multipart seek uses the file sample ordinal and adopts the correct inner fragment");
    mp4_fragment_source_release(&source); mp4_segment_store_release(&store);

    file = {};
    for (u32 i = 0; i < 2; ++i) {
        const auto moof = file.begin("moof");
        auto box = file.begin("mfhd"); file.word(0); file.word(i); file.end(box);
        const auto traf = file.begin("traf");
        box = file.begin("tfhd"); file.word(0x20018); file.word(7); file.word(1024); file.word(2); file.end(box);
        box = file.begin("tfdt"); file.word(0); file.word(i * 3072); file.end(box);
        box = file.begin("trun"); file.word(1); file.word(3); const auto offset = file.size; file.word(0);
        file.end(box); file.end(traf); file.end(moof);
        box = file.begin("mdat"); byte_store_be32(file.data + offset, file.size - moof); file.zero(6); file.end(box);
    }
    const auto audio_init = initialization(true); Mp4FragmentAudio audio{};
    check(mp4_segment_store_initialize(&store, audio_init.w.view(), true, 1, 6144, 44100) == E::none &&
        mp4_segment_store_request(&store, 0) == E::data_pending &&
        mp4_segment_store_accept(&store, 0, store.request_serial, file.view()) == E::none &&
        mp4_fragment_source_initialize_segments(&store, &source) == E::none &&
        mp4_fragment_audio_initialize(&source, &audio) == E::none && audio.packet_count == 6,
        "AAC packet count covers every inner fragment");
    for (u32 i = 0; i < 6; ++i) {
        Mp4FragmentAudioPacket part{};
        check(mp4_fragment_audio_peek(&audio, &part) == E::none && part.first_source_frame == u64{i} * 1024 &&
            mp4_fragment_audio_commit(&audio), "AAC timeline remains continuous across multipart boundaries");
    }
    Mp4FragmentAudioPacket part{};
    check(mp4_fragment_audio_seek(&audio, 4) == E::none && mp4_fragment_audio_peek(&audio, &part) == E::none &&
        part.first_source_frame == 4096, "AAC seek can target a packet in a later inner fragment");
    mp4_fragment_source_release(&source); mp4_segment_store_release(&store);
}
void media_segment_tests() noexcept {
    using E = Mp4DemuxError;
    const auto f = fragment(); Writer w{};
    auto box = w.begin("styp"); w.word(0x6d736468); w.word(0); w.word(0x6d736978); w.end(box);
    const u32 index = w.begin("sidx"); w.word(0); w.word(7); w.word(1000); w.word(980);
    w.word(0); w.word(1); w.word(f.span); w.word(500); w.word(0x90000000); w.end(index);
    const u32 media = w.size;
    copy_bytes(w.data + w.size, f.w.data, f.w.size); w.size += f.w.size; w.zero(f.span - f.w.size);
    Mp4FragmentTrack track{}; track.defaults.track_id = 7; track.timescale = 1000;
    Mp4MediaSegment segment{};
    check(mp4_media_segment_initialize(w.view(), track, &segment) == E::none &&
          segment.range.offset == media && segment.range.bytes == f.span && segment.range.time == 980 &&
          segment.fragment.first_time == 1000 && segment.fragment.count == f.samples &&
          segment.fragment.metadata.data == w.data + media,
          "separate segment uses exact local offsets and distinct presentation/decode clocks without copying");
    const auto before = segment;
    for (u32 bytes = 0; bytes < w.size; ++bytes)
        check(mp4_media_segment_initialize({w.data, bytes}, track, &segment) != E::none &&
              bytes_equal(&before, &segment, sizeof(segment)), "truncated media file cannot replace an accepted segment");
    check(mp4_media_segment_initialize({w.data + index, w.size - index}, track, &segment) == E::none &&
          segment.range.offset == media - index, "separate media segment can omit the optional styp");
    track.audio = true;
    check(mp4_media_segment_initialize(w.view(), track, &segment) == E::malformed,
          "audio segments cannot reinterpret inconsistent index and decode clocks");
    track.audio = false;
    ++track.timescale;
    check(mp4_media_segment_initialize(w.view(), track, &segment) == E::malformed,
          "separate segment must use the initialization track timescale");
    --track.timescale; ++track.defaults.track_id;
    check(mp4_media_segment_initialize(w.view(), track, &segment) == E::malformed,
          "separate segment must use the initialization track identity");
    --track.defaults.track_id;
    const auto original = w;
    check(mp4_media_segment_initialize(w.view(), track, reinterpret_cast<Mp4MediaSegment*>(w.data)) == E::invalid_input &&
          bytes_equal(w.data, original.data, w.size), "separate segment output cannot overwrite borrowed media");
    byte_store_be32(w.data + index + 24, 1);
    check(mp4_media_segment_initialize(w.view(), track, &segment) == E::malformed,
          "separate media index cannot skip into the fragment header");
    w = original; byte_store_be32(w.data + index + 36, 499);
    track.audio = true; byte_store_be32(w.data + index + 20, 1000);
    check(mp4_media_segment_initialize(w.view(), track, &segment) == E::malformed,
          "audio segment duration cannot disagree with its exact sample clock");
    track.audio = false; w = original; w.zero(1);
    check(mp4_media_segment_initialize(w.view(), track, &segment) == E::malformed,
          "separate media index must cover the complete file without trailing bytes");
}
Writer stored_segment(u32 number, u32 gap = 0) noexcept {
    auto f = fragment(); byte_store_be32(f.w.data + f.tfdt + 16, 1000 + number * 500 + gap);
    Writer w{}; const u32 index = w.begin("sidx"); w.word(0); w.word(7); w.word(15360); w.word(980 + number * 500 + gap);
    w.word(0); w.word(1); w.word(f.span); w.word(500); w.word(0x90000000); w.end(index);
    copy_bytes(w.data + w.size, f.w.data, f.w.size); w.size += f.w.size; w.zero(f.span - f.w.size);
    return w;
}
void live_segment_tests() noexcept {
    using E = Mp4DemuxError;
    const auto init = initialization(false); Mp4SegmentStore store{}; Mp4FragmentSource source{};
    const auto feed = [&]() noexcept {
        const auto bytes = stored_segment(store.demand);
        return mp4_segment_store_accept(&store, store.demand, store.request_serial, bytes.view()) == E::none;
    };
    check(mp4_segment_store_initialize(&store, init.w.view(), false, 2, 500, 15360, true) == E::none &&
        mp4_fragment_source_initialize_segments(&store, &source) == E::data_pending && store.demand == 0 && feed() &&
        mp4_fragment_source_initialize_segments(&store, &source) == E::none && source.index.count == 2 &&
        source.index.duration == 500 && !store.pending && store.received_count == 1,
        "live startup needs only the first file and keeps future playlist ordinals separate from received timing");
    Mp4FragmentPacket packet{};
    for (u32 i = 0; i < 5; ++i) check(mp4_fragment_source_peek(&source, &packet) == E::none &&
        packet.segment == 0 && packet.sample.index == i && mp4_fragment_source_commit(&source), "live first file consumes each packet once");
    check(mp4_fragment_source_peek(&source, &packet) == E::data_pending && store.pending && store.demand == 1 && !store.refresh_pending,
        "an advertised live file requests media rather than another playlist");
    const auto gapped = stored_segment(1, 1); const auto generation = store.generation;
    check(mp4_segment_store_accept(&store, 1, store.request_serial, gapped.view()) == E::malformed && store.generation == generation &&
        store.received_count == 1 && feed() && store.received_count == 2,
        "a live timestamp gap cannot overwrite the active media slot");
    for (u32 i = 0; i < 5; ++i) check(mp4_fragment_source_peek(&source, &packet) == E::none &&
        packet.segment == 1 && packet.sample.index == i && mp4_fragment_source_commit(&source), "live second file preserves its stable ordinal");
    const auto serial = store.request_serial;
    check(mp4_fragment_source_peek(&source, &packet) == E::data_pending && store.refresh_pending && !store.pending &&
        !source.ended && source.index.duration == 1000 && mp4_fragment_source_peek(&source, &packet) == E::data_pending &&
        store.request_serial == serial, "the live edge requests one refresh without ending or fabricating a media request");
    check(mp4_segment_store_extend(&store, 1, 3, false) == E::none && !store.refresh_pending &&
        mp4_segment_store_find(store, 1) && store.received_count == 2 &&
        mp4_fragment_source_peek(&source, &packet) == E::data_pending && store.demand == 2 && feed() && store.received_count == 3,
        "a validated sliding window extends existing playback while retaining its active bytes");
    check(mp4_segment_store_request(&store, 0) == E::unsupported && !store.pending,
        "an evicted expired ordinal is never silently mapped to a different live file");
    Mp4FragmentRange cached{};
    store.nominal_duration = 6000; // An upper bound can point behind the live window.
    check(mp4_fragment_source_find(&source, 1980, &cached) == E::none && cached.index == 2 && !store.pending,
        "a live seek into a cached interval ignores an expired nominal hint");
    for (u32 i = 0; i < 5; ++i) check(mp4_fragment_source_peek(&source, &packet) == E::none &&
        packet.segment == 2 && packet.sample.index == i && mp4_fragment_source_commit(&source), "live appended file keeps sequential sample identity");
    check(mp4_fragment_source_peek(&source, &packet) == E::data_pending && !source.ended &&
        mp4_segment_store_extend(&store, 1, 3, true) == E::none &&
        mp4_fragment_source_peek(&source, &packet) == E::end_of_stream && source.ended && source.index.duration == 1500,
        "only a confirmed terminal playlist converts the consumed live edge into EOF");
    const auto saved = store;
    check(mp4_segment_store_extend(&store, 1, 4, false) == E::malformed && bytes_equal(&store, &saved, sizeof(saved)) &&
        store.slots[0].storage.committed == 65536 && store.slots[1].storage.committed == 65536,
        "a closed live store cannot reopen and retains exactly two bounded media slots");
    mp4_fragment_source_release(&source); mp4_segment_store_release(&store);
}
Writer live_audio_file(u32 number, u32 offset_ticks = 0) noexcept {
    Writer file{}; const auto moof = file.begin("moof");
    auto box = file.begin("mfhd"); file.word(0); file.word(number); file.end(box);
    const auto traf = file.begin("traf");
    box = file.begin("tfhd"); file.word(0x20018); file.word(7); file.word(1024); file.word(2); file.end(box);
    box = file.begin("tfdt"); file.word(0); file.word(number * 3072 + offset_ticks); file.end(box);
    box = file.begin("trun"); file.word(1); file.word(3); const auto offset = file.size; file.word(0);
    file.end(box); file.end(traf); file.end(moof);
    box = file.begin("mdat"); byte_store_be32(file.data + offset, file.size - moof); file.zero(6); file.end(box);
    return file;
}
void live_audio_tests() noexcept {
    using E = Mp4DemuxError;
    const auto init = initialization(true); Mp4SegmentStore store{}; Mp4FragmentSource source{}; Mp4FragmentAudio audio{};
    const auto first = live_audio_file(0);
    check(mp4_segment_store_initialize(&store, init.w.view(), true, 1, 3072, 44100, true) == E::none &&
        mp4_segment_store_request(&store, 0) == E::data_pending &&
        mp4_segment_store_accept(&store, 0, store.request_serial, first.view()) == E::none &&
        mp4_fragment_source_initialize_segments(&store, &source) == E::none &&
        mp4_fragment_audio_initialize(&source, &audio) == E::none, "live AAC starts with one bounded source file");
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const AccountVideoIdentity identity{1, 2, 3, 4, 0, 1, 0};
    AccountVideoSession session{};
    check(account_video_session_initialize_fragment_audio(&session, &audio, identity, event).error == AccountVideoError::invalid_input &&
          !session.initialized && !session.workspace, "finite audio-only session refuses a sliding live timeline");
    if (event) CloseHandle(event);
    Mp4FragmentAudioPacket packet{};
    for (u32 file = 0; file < 3; ++file) {
        for (u32 sample = 0; sample < 3; ++sample)
            check(mp4_fragment_audio_peek(&audio, &packet) == E::none &&
                packet.first_source_frame == u64{file * 3 + sample} * 1024 && mp4_fragment_audio_commit(&audio),
                "live AAC timeline growth preserves accepted packet positions and exact frame numbers");
        if (file == 2) break;
        check(mp4_fragment_audio_peek(&audio, &packet) == E::data_pending && store.refresh_pending && !source.ended &&
            mp4_segment_store_extend(&store, file, file + 2, false) == E::none &&
            mp4_fragment_audio_peek(&audio, &packet) == E::data_pending && store.demand == file + 1,
            "live AAC waits through refresh and media delivery without treating a temporary edge as final audio");
        const auto next = live_audio_file(file + 1);
        check(mp4_segment_store_accept(&store, store.demand, store.request_serial, next.view()) == E::none &&
            mp4_fragment_audio_refresh(&audio) == E::none && audio.packet_count == (file + 2) * 3 && audio.packet_index == (file + 1) * 3,
            "received audio extends the packet bound without rewinding the accepted cursor");
    }
    check(audio.timeline.source_frames == 9216 && audio.timeline.content_ticks == 8192 &&
        mp4_segment_store_extend(&store, 1, 3, true) == E::none && mp4_fragment_audio_peek(&audio, &packet) == E::end_of_stream,
        "terminal live AAC retains its original edit mapping and closes at the exact final frame count");
    mp4_fragment_source_release(&source); mp4_segment_store_release(&store);
}
void segment_store_tests() noexcept {
    using E = Mp4DemuxError;
    const auto init = initialization(false);
    Mp4SegmentStore store{}; Mp4FragmentSource source{};
    check(mp4_segment_store_initialize(&store, init.w.view(), false, 3, 560, 15360) == E::none &&
          store.initialization.committed == 65536 && !store.slots[0].storage.base && !store.slots[1].storage.base,
          "separate media storage commits only initialization until media arrives");
    const auto feed = [&]() noexcept {
        const auto f = stored_segment(store.demand);
        return mp4_segment_store_accept(&store, store.demand, store.request_serial, f.view()) == E::none;
    };
    check(mp4_fragment_source_initialize_segments(&store, &source) == E::data_pending && store.demand == 0 && feed() &&
          mp4_fragment_source_initialize_segments(&store, &source) == E::data_pending && store.demand == 2 &&
          store.active < 2 && store.slots[store.active].number == 0 && feed() &&
          mp4_fragment_source_initialize_segments(&store, &source) == E::none &&
          source.index.count == 3 && source.index.first_time == 980 && source.index.duration == 1500 && source.presentation_end == 2500,
          "separate source derives its exact complete timeline from first and last indexes while pinning the first file");
    Mp4FragmentPacket packet{};
    check(mp4_fragment_source_peek(&source, &packet) == E::none && packet.segment == 0 && packet.sample.index == 0 &&
          mp4_fragment_source_commit(&source), "separate source uses existing borrowed packet and commit semantics");
    const auto old_cursor = source.fragment;
    const auto* old_bytes = packet.bytes.data;
    Mp4FragmentPosition proposal{};
    check(mp4_fragment_source_prepare(&source, 1, 0, &proposal) == E::data_pending && store.demand == 1,
          "missing seek file publishes a demand without releasing the active file");
    const auto serial = store.request_serial, generation = store.generation;
    check(mp4_fragment_source_prepare(&source, 1, 0, &proposal) == E::data_pending && store.request_serial == serial &&
          bytes_equal(&old_cursor, &source.fragment, sizeof(old_cursor)) &&
          mp4_segment_store_find(store, 0)->storage.base <= old_bytes,
          "repeated pending seek preserves the current cursor, bytes, and in-flight request serial");
    auto malformed = stored_segment(1); malformed.data[0] = 255;
    check(mp4_segment_store_accept(&store, 1, serial, malformed.view()) != E::none && store.generation == generation &&
          bytes_equal(&old_cursor, &source.fragment, sizeof(old_cursor)), "malformed candidate cannot overwrite active media");
    check(mp4_fragment_source_prepare(&source, 2, 0, &proposal) == E::none && mp4_fragment_source_adopt(&source, proposal) &&
          !store.pending && source.segment.index == 2, "cached seek adopts immediately and cancels older pending demand");
    const auto stale = stored_segment(1);
    check(mp4_segment_store_accept(&store, 1, serial, stale.view()) == E::invalid_input,
          "late cancelled segment response cannot enter the current source");
    check(mp4_fragment_source_prepare(&source, 2, 0, &proposal) == E::none &&
          mp4_segment_store_request(&store, 1) == E::data_pending && feed() && !mp4_fragment_source_adopt(&source, proposal),
          "replacing candidate bytes invalidates old seek proposals by generation");
    Mp4FragmentRange range{};
    const auto cached_serial = store.request_serial, cached_generation = store.generation;
    const auto cached_cursor = source.fragment;
    check(mp4_fragment_source_find(&source, 1480, &range) == E::none && range.index == 1 && range.time == 1480 &&
          !store.pending && store.request_serial == cached_serial && store.generation == cached_generation &&
          bytes_equal(&source.fragment, &cached_cursor, sizeof(cached_cursor)) &&
          store.slots[0].storage.committed == 65536 && store.slots[1].storage.committed == 65536,
          "cached interval lookup needs zero requests or allocations and preserves the borrowed cursor despite an inaccurate nominal hint");
    mp4_fragment_source_cancel_prepare(&source);
    // Deliberately evict the candidate. The nominal duration overestimates the
    // real interval; after correcting to file1 a retry must keep that correction.
    check(mp4_segment_store_request(&store, 0) == E::data_pending && feed() &&
          mp4_fragment_source_find(&source, 1480, &range) == E::data_pending && store.demand == 1 && feed() &&
          mp4_fragment_source_find(&source, 1480, &range) == E::none && range.index == 1 && range.time == 1480,
          "seek correction follows real media clocks without oscillating between evicted nominal guesses");
    check(mp4_fragment_source_select(&source, 0), "separate source coarse rewind");
    u32 samples = 0, requests = 0; E result = E::none;
    for (u32 calls = 0; calls < 32; ++calls) {
        result = mp4_fragment_source_peek(&source, &packet);
        if (result == E::data_pending) { ++requests; if (!feed()) break; continue; }
        if (result != E::none) break;
        check(packet.segment == samples / 5 && packet.sample.index == samples % 5 &&
              mp4_fragment_source_commit(&source), "separate media sequence consumes exact sample identity across file boundaries");
        ++samples;
    }
    check(result == E::end_of_stream && samples == 15 && requests == 3 &&
          store.slots[0].storage.committed == 65536 && store.slots[1].storage.committed == 65536,
          "whole sequence uses two fixed media slots and requests only missing files");
    mp4_fragment_source_release(&source);
    const auto prior_serial = store.request_serial;
    mp4_segment_store_release(&store);
    check(!store.initialization.base && !store.slots[0].storage.base && !store.slots[1].storage.base &&
          store.request_serial != prior_serial, "joined segment release frees pages and invalidates old response serials");
    check(mp4_segment_store_initialize(&store, init.w.view(), false, 3, 560, 15360) == E::none &&
          mp4_segment_store_request(&store, 1) == E::data_pending &&
          mp4_segment_store_accept(&store, 1, serial, stale.view()) == E::invalid_input && feed(),
          "reusing a retired store cannot admit a response from its previous playback lifetime");
    mp4_segment_store_release(&store);
}
void live_alignment_tests() noexcept {
    auto* pair = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, 2 * sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(pair != nullptr, "live alignment owners"); if (!pair) return;
    SegmentedVideoFetch owners[2]{}; bool ready = true;
    for (u32 i = 0; i < 2; ++i) {
        pair[i].segmented = owners + i; pair[i].state = ProgressiveVideoFetchState::ready;
        owners[i].playlist.input = owners[i].manifest = utf8_literal("live");
        const auto init = initialization(i != 0); const auto file = i ? live_audio_file(1, 2048) : stored_segment(0);
        auto& store = owners[i].store;
        ready = ready && mp4_segment_store_initialize(&store, init.w.view(), i != 0, 3, 2, 1, true) == Mp4DemuxError::none &&
            mp4_segment_store_request(&store, 0) == Mp4DemuxError::data_pending &&
            mp4_segment_store_accept(&store, 0, store.request_serial, file.view()) == Mp4DemuxError::none &&
            mp4_fragment_source_initialize_segments(&store, &pair[i].fragment) == Mp4DemuxError::none;
        auto& track = pair[i].fragment.track;
        track.edited = false; track.edit_duration = track.empty_edit = 0; track.media_start = 0;
    }
    check(ready, "live alignment receives independently timed first track files");
    if (ready) {
        pair[0].fragment.track.edited = true;
        pair[0].fragment.track.media_start = 7;
        pair[1].fragment.track.media_start = 11;
        check(segmented_video_synchronize(pair, pair + 1) == Mp4DemuxError::unsupported &&
              !owners[0].synchronized && !owners[1].synchronized,
              "edited live snapshots cannot silently rebase their presentation timeline");
        owners[0].manifest = owners[1].manifest = {};
        const auto saved_video = pair[0].fragment.track;
        const auto saved_audio = pair[1].fragment.track;
        check(segmented_video_synchronize(pair, pair + 1) == Mp4DemuxError::none &&
              owners[0].synchronized && owners[1].synchronized &&
              bytes_equal(&saved_video, &pair[0].fragment.track, sizeof(saved_video)) &&
              bytes_equal(&saved_audio, &pair[1].fragment.track, sizeof(saved_audio)),
              "VOD synchronization preserves declared edits and independent track offsets");
        owners[0].synchronized = owners[1].synchronized = false;
        owners[0].manifest = owners[1].manifest = utf8_literal("live");
        pair[0].fragment.track.edited = false;
        pair[0].fragment.track.media_start = pair[1].fragment.track.media_start = 0;
        check(segmented_video_synchronize(pair, pair + 1) == Mp4DemuxError::data_pending && owners[0].store.pending &&
            owners[0].store.demand == 1 && pair[0].fragment.track.media_start == 0 && pair[1].fragment.track.media_start == 0 &&
            !owners[0].synchronized && !owners[1].synchronized,
            "a later audio snapshot requests missing video without publishing a half-updated origin");
        const auto next = stored_segment(1);
        check(mp4_segment_store_accept(&owners[0].store, 1, owners[0].store.request_serial, next.view()) == Mp4DemuxError::none &&
            segmented_video_synchronize(pair, pair + 1) == Mp4DemuxError::none && owners[0].synchronized && owners[1].synchronized,
            "received overlap completes live startup through the same asynchronous media owner");
        Mp4AudioSampleWindow window{}; i64 video_start{}, audio_start{};
        check(mp4_audio_sample_window(pair[1].audio.timeline, 0, &window) && !window.leading_silence_frames &&
            mp4_fragment_source_time(pair[0].fragment, static_cast<i64>(pair[0].fragment.index.first_time), &video_start) &&
            mp4_fragment_source_time(pair[1].fragment, static_cast<i64>(pair[1].fragment.index.first_time), &audio_start) &&
            video_start < 0 && audio_start == 0 && window.first_source_frame == 0 && window.end_source_frame == 3072,
            "live startup trims older video rather than adding silence to the later audio track");
        const auto origin = pair[0].fragment.track.media_start;
        check(segmented_video_synchronize(pair, pair + 1) == Mp4DemuxError::none && pair[0].fragment.track.media_start == origin,
            "live alignment remains fixed after initial synchronization");
    }
    for (u32 i = 0; i < 2; ++i) { mp4_fragment_source_release(&pair[i].fragment); mp4_segment_store_release(&owners[i].store); }
    VirtualFree(pair, 0, MEM_RELEASE);
}
void live_fetch_tests() noexcept {
    constexpr auto manifest = utf8_literal("https://euc12.playlist.ttvnw.net/v1/playlist/example.m3u8");
    u8 text[2048];
    const auto playlist = [&](u32 first, bool ended = false, bool broken = false) noexcept {
        ProtocolWriter w{{text, sizeof(text)}, 0};
        protocol_writer_append_literal(&w, "#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:");
        protocol_writer_append_u64(&w, 100 + first);
        protocol_writer_append_literal(&w, "\n#EXT-X-MAP:URI=\"https://example.hls.ttvnw.net/v1/segment/init.mp4\"\n");
        for (u32 i = first; i < first + 5; ++i) {
            protocol_writer_append_literal(&w, "#EXTINF:0.032552,\nhttps://example.hls.ttvnw.net");
            if (i & 1) protocol_writer_append_literal(&w, "/s1");
            protocol_writer_append_literal(&w, "/v1/segment/");
            protocol_writer_append_u64(&w, broken ? i + 10 : i); protocol_writer_append_literal(&w, ".mp4\n");
        }
        if (ended) protocol_writer_append_literal(&w, "#EXT-X-ENDLIST\n");
        return Utf8View{reinterpret_cast<const char*>(text), w.used};
    };
    auto* f = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(f && progressive_video_fetch_begin_hls(f, manifest, playlist(0), false, true, 100), "live fetch owns a bounded snapshot and starts near its edge");
    if (!f || !f->segmented) { if (f) VirtualFree(f, 0, MEM_RELEASE); return; }
    auto* owner = f->segmented; const auto* text_owner = owner->playlist.input.data;
    const auto init = initialization(false); u64 serial = 0;
    const auto reply = [&](ConstSlice<u8> bytes) noexcept {
        CdnResponseResult policy{}; policy.status_code = 200; policy.disposition = CdnResponseDisposition::accept_body;
        policy.body_bytes = bytes.count; policy.entity_bytes = bytes.count;
        return progressive_video_fetch_accept(f, f->inflight_id, true, policy, bytes);
    };
    for (u32 i = 0; i < 2; ++i) {
        const auto next = progressive_video_fetch_next(f, false, 100);
        check(next.step == ProgressiveVideoStep::submit && next.range.bytes && f->host == CdnHost::twitch_live,
            "live initialization and first media reuse ordinary bounded media requests");
        progressive_video_fetch_submitted(f, ++serial, next.range);
        const auto media = stored_segment(2);
        check(reply(i ? media.view() : init.w.view()) == ProgressiveVideoAccept::progress, "live startup accepts only its requested body");
    }
    check(progressive_video_fetch_next(f, false, 100).step == ProgressiveVideoStep::ready && owner->store.live &&
        owner->sequence_base == 102 && owner->store.count == 3 && owner->store.received_count == 1 &&
        segmented_video_refresh_delay(f, 100) == 2000 && progressive_video_fetch_next(f, false, 102).step == ProgressiveVideoStep::idle,
        "live startup needs no final-file probe and paused playback does not refresh");
    const auto* retained = mp4_segment_store_find(owner->store, 0);
    const auto* active_bytes = retained ? retained->storage.base : nullptr;
    const auto refresh = [&](u64 now) noexcept {
        const auto next = progressive_video_fetch_next(f, true, now);
        const bool valid = next.step == ProgressiveVideoStep::submit && !next.range.bytes && f->host == CdnHost::twitch_playlist &&
            f->image_type == CdnImageType::hls_playlist && owner->refreshing;
        progressive_video_fetch_submitted(f, ++serial, next.range); return valid;
    };
    check(refresh(102) && segmented_video_refresh_delay(f, 102) == kInfiniteWaitMilliseconds, "one in-flight playlist prevents duplicate polls");
    const u64 generation = owner->store.generation;
    progressive_video_fetch_cancelling(f);
    const auto unchanged = playlist(0);
    check(f->cancelling && reply({reinterpret_cast<const u8*>(unchanged.data), unchanged.bytes}) == ProgressiveVideoAccept::ignored &&
        owner->store.generation == generation, "cancelled whole-playlist replies cannot mutate the stream");
    check(refresh(102), "cancelled refresh can be retried by active playback");
    check(reply({reinterpret_cast<const u8*>(unchanged.data), unchanged.bytes}) == ProgressiveVideoAccept::progress &&
        owner->refresh_at == 103 && owner->store.generation == generation, "unchanged playlist waits half its target duration without media churn");
    check(refresh(103), "changed live snapshot uses the next scheduled poll");
    const auto advanced = playlist(1);
    check(reply({reinterpret_cast<const u8*>(advanced.data), advanced.bytes}) == ProgressiveVideoAccept::progress &&
        owner->store.count == 4 && owner->store.first_available == 0 && owner->refresh_at == 105 &&
        f->segmented == owner && owner->playlist.input.data == text_owner && mp4_segment_store_find(owner->store, 0)->storage.base == active_bytes,
        "sliding updates preserve active media and the same owner/text allocation");
    Mp4FragmentPacket packet{};
    for (u32 i = 0; i < 5; ++i) check(mp4_fragment_source_peek(&f->fragment, &packet) == Mp4DemuxError::none &&
        packet.segment == 0 && mp4_fragment_source_commit(&f->fragment), "live first ordinal remains the same after the playlist slides");
    check(mp4_fragment_source_peek(&f->fragment, &packet) == Mp4DemuxError::data_pending && owner->store.demand == 1,
        "the next missing ordinal requests its original media identity");
    const auto next = progressive_video_fetch_next(f, false, 104);
    const Utf8View target{reinterpret_cast<char*>(f->target), f->target_bytes};
    check(next.step == ProgressiveVideoStep::submit && next.range.bytes && f->host == CdnHost::twitch_live &&
        utf8_equal(target, utf8_literal("https://example.hls.ttvnw.net/s1/v1/segment/3.mp4")),
        "live ordinal mapping preserves mixed delivery shards through startup and sliding refreshes");
    progressive_video_fetch_submitted(f, ++serial, next.range); const auto media = stored_segment(3);
    check(reply(media.view()) == ProgressiveVideoAccept::progress, "live media returns to the media response path after playlist refresh");
    check(refresh(105), "live next refresh is scheduled after a changed snapshot");
    const auto stale = playlist(0);
    check(reply({reinterpret_cast<const u8*>(stale.data), stale.bytes}) == ProgressiveVideoAccept::ignored &&
        owner->playlist.sequence == 101 && owner->store.count == 4, "a stale playlist cannot roll back live ordinals");
    check(refresh(106), "stale snapshots retry on a bounded deadline");
    const auto broken = playlist(1, false, true);
    check(reply({reinterpret_cast<const u8*>(broken.data), broken.bytes}) == ProgressiveVideoAccept::failed &&
        owner->playlist.sequence == 101 && owner->store.count == 4 &&
        mp4_segment_store_find(owner->store, 0)->storage.base == active_bytes &&
        segmented_video_refresh_delay(f, 107) == kInfiniteWaitMilliseconds,
        "overlapping identity changes fail without replacing old text or pinned media and stop future polling");
    progressive_video_fetch_release(f); VirtualFree(f, 0, MEM_RELEASE);
}
void segmented_fetch_tests(bool hls = false, bool vimeo = false) noexcept {
    using E = Mp4DemuxError;
    constexpr auto manifest = utf8_literal("https://video.akamai.steamstatic.com/store_trailers/570/116737/0123456789abcdef0123456789abcdef01234567/1749859757/dash_h264.mpd");
    const DashTrack track{utf8_literal("2"), utf8_literal("dash_h264/init-stream$RepresentationID$.m4s"),
        utf8_literal("dash_h264/chunk-stream$RepresentationID$-$Number%05d$.m4s"), 560, 15360, 1, 3, 1400000, 854, 480};
    auto* f = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    constexpr auto playlist = utf8_literal("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:1\n#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:0.03125,\n0.mp4\n#EXTINF:0.03125,\n1.mp4\n#EXTINF:0.03125,\n2.mp4\n#EXT-X-ENDLIST\n");
    constexpr auto relative = utf8_literal("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MAP:URI=\"../../../range/prot/init?range=0-2\"\n"
        "#EXTINF:0.03125,\n../../../range/prot/0?range=3-6\n"
        "#EXTINF:0.03125,\n../../../range/prot/1?range=7-9\n"
        "#EXTINF:0.03125,\n../../../range/prot/2?range=10-12\n#EXT-X-ENDLIST\n");
    check(f && (hls ? progressive_video_fetch_begin_hls(f, vimeo
        ? utf8_literal("https://vod-adaptive-ak.vimeocdn.com/public/v2/playlist/av/track/media.m3u8?pathsig=test")
        : utf8_literal("https://d2nvs31859zcd8.cloudfront.net/recording/480p30/index-dvr.m3u8"), vimeo ? relative : playlist, false)
        : progressive_video_fetch_begin_segments(f, manifest, track, false)), "bounded separate-file fetch setup");
    if (!f || !f->segmented) { if (f) VirtualFree(f, 0, MEM_RELEASE); return; }
    const auto init = initialization(false); u64 request_id = 0;
    const auto reply = [&](ConstSlice<u8> bytes) noexcept {
        CdnResponseResult policy{}; policy.disposition = CdnResponseDisposition::accept_partial;
        policy.status_code = 206; policy.body_bytes = bytes.count; policy.entity_bytes = bytes.count;
        return progressive_video_fetch_accept(f, f->inflight_id, true, policy, bytes);
    };
    u32 transfers = 0;
    for (u32 i = 0; i < 5; ++i) {
        const auto next = progressive_video_fetch_next(f, false, 1);
        if (next.step == ProgressiveVideoStep::ready) break;
        check(next.step == ProgressiveVideoStep::submit && !next.range.first && next.range.bytes <= 8 * 1024 * 1024,
              "separate source requests bounded whole-file ranges");
        if (vimeo)
            check(f->host == CdnHost::vimeo_video &&
                  vimeo_location({reinterpret_cast<const char*>(f->target), f->target_bytes}, false),
                  "relative Vimeo initialization and media resolve to validated CDN requests");
        if (next.step != ProgressiveVideoStep::submit) break;
        progressive_video_fetch_submitted(f, ++request_id, next.range);
        const auto file = stored_segment(f->segmented->number);
        const auto accepted = reply(f->segmented->initialization ? init.w.view() : file.view());
        check(accepted == ProgressiveVideoAccept::progress, "bounded complete media response enters only its expected owner");
        ++transfers;
    }
    check(transfers == 3 && f->state == ProgressiveVideoFetchState::ready && !f->source.base &&
          f->fragment.segments == &f->segmented->store && !progressive_video_fetch_pending(f),
          "initialization plus first and last files establish readiness without a synthetic sparse entity");
    u16 begin[2]{}, end[2]{};
    check(progressive_video_fetch_buffered(f, 2000000, begin, end, 2) == 2 && begin[0] < begin[1],
          "separate source reports its actual resident intervals in order");
    Mp4FragmentPosition position{};
    if (hls) {
        Mp4FragmentRange range{};
        check(mp4_fragment_source_find(&f->fragment, 1480, &range) == E::data_pending &&
            f->segmented->store.demand == 1 && f->segmented->store.seek_attempts == 0,
            "HLS exact EXTINF lookup jumps straight to the containing file despite a much larger target duration");
    }
    check(mp4_fragment_source_prepare(&f->fragment, 1, 0, &position) == E::data_pending,
          "missing file creates demand through the same decoder cursor");
    auto next = progressive_video_fetch_next(f, false, 1);
    check(next.step == ProgressiveVideoStep::submit, "seek file is scheduled on the existing transport");
    progressive_video_fetch_submitted(f, ++request_id, next.range);
    check(mp4_fragment_source_prepare(&f->fragment, 2, 0, &position) == E::none &&
          mp4_fragment_source_adopt(&f->fragment, position) &&
          progressive_video_fetch_next(f, false, 1).step == ProgressiveVideoStep::cancel,
          "retargeted cached seek cancels the old transport request");
    progressive_video_fetch_cancelling(f);
    const auto late = stored_segment(1);
    check(reply(late.view()) == ProgressiveVideoAccept::ignored && f->fragment.segment.index == 2 &&
          !mp4_segment_store_find(f->segmented->store, 1), "late successful cancelled response cannot replace cached playback");
    progressive_video_fetch_retire(f);
    if (hls) check(f->segmented->playlist.input.data == reinterpret_cast<const char*>(f->segmented + 1) &&
        f->segmented->playlist.duration_us == 93750,
        "HLS retirement retains owned playlist timing while reclaiming media pages");
    check(f->segmented->store.initialization.committed && !f->segmented->store.slots[0].storage.committed &&
          !f->segmented->store.slots[1].storage.committed && !progressive_video_fetch_pending(f) &&
          progressive_video_fetch_restore(f), "joined retirement releases all media pages and retains only reusable initialization and clocks");
    Mp4FragmentPacket packet{};
    check(mp4_fragment_source_peek(&f->fragment, &packet) == E::data_pending, "retired source requests its first file only when played again");
    next = progressive_video_fetch_next(f, false, 1);
    progressive_video_fetch_submitted(f, ++request_id, next.range);
    const auto first = stored_segment(0);
    check(next.step == ProgressiveVideoStep::submit && reply(first.view()) == ProgressiveVideoAccept::progress &&
          mp4_fragment_source_peek(&f->fragment, &packet) == E::none && packet.segment == 0 && packet.sample.index == 0,
          "cached source resumes with original timing after media pages are reclaimed");
    next = progressive_video_fetch_next(f, true, 1);
    check(next.step == ProgressiveVideoStep::submit && f->segmented->number == 1,
          "separate-file read-ahead schedules exactly the following segment");
    progressive_video_fetch_submitted(f, ++request_id, next.range);
    check(progressive_video_fetch_next(f, true, 1).step == ProgressiveVideoStep::idle &&
          reply(late.view()) == ProgressiveVideoAccept::progress &&
          progressive_video_fetch_next(f, true, 1).step == ProgressiveVideoStep::idle && f->fragment.segment.index == 0,
          "in-flight or resident read-ahead does not issue duplicate requests or advance playback");
    check(mp4_fragment_source_prepare(&f->fragment, 2, 0, &position) == E::data_pending, "partial file rejection fixture");
    next = progressive_video_fetch_next(f, false, 1); progressive_video_fetch_submitted(f, ++request_id, next.range);
    const auto final = stored_segment(2);
    CdnResponseResult partial{}; partial.disposition = CdnResponseDisposition::accept_partial;
    partial.status_code = 206; partial.body_bytes = final.size; partial.entity_bytes = final.size + 1;
    check(progressive_video_fetch_accept(f, request_id, true, partial, final.view()) == ProgressiveVideoAccept::failed &&
          f->state == ProgressiveVideoFetchState::failed && f->fragment.segment.index == 0,
          "a response containing only a prefix cannot be admitted as a complete segment or replace current playback");
    progressive_video_fetch_release(f); VirtualFree(f, 0, MEM_RELEASE);
}
void audio_tests() noexcept {
    using E = Mp4DemuxError;
    constexpr u32 block = kProgressiveBlockBytes, origin = 44100, tail = 500, duration = 2048 + tail;
    auto init = initialization(true);
    byte_store_be32(init.w.data + init.edit, origin + 1024);
    Writer fragment{};
    const auto moof = fragment.begin("moof");
    auto box = fragment.begin("mfhd"); fragment.word(0); fragment.word(1); fragment.end(box);
    const auto traf = fragment.begin("traf");
    box = fragment.begin("tfhd"); fragment.word(0x20000); fragment.word(7); fragment.end(box);
    box = fragment.begin("tfdt"); fragment.word(0); const u32 clock = fragment.size; fragment.word(origin); fragment.end(box);
    box = fragment.begin("trun"); fragment.word(0x301); fragment.word(3);
    const u32 offset = fragment.size; fragment.word(0);
    const u32 table = fragment.size;
    fragment.word(1024); fragment.word(10); fragment.word(1024); fragment.word(10); fragment.word(tail); fragment.word(10);
    fragment.end(box); fragment.end(traf); fragment.end(moof);
    byte_store_be32(fragment.data + offset, fragment.size + 8);
    fragment.word(38); fragment.word(0x6d646174); fragment.zero(30);
    Writer index{}; box = index.begin("sidx"); index.word(0); index.word(7); index.word(44100); index.word(origin);
    index.word(block - init.w.size - 44); index.word(1);
    index.word(fragment.size); index.word(duration); index.word(0x90000000); index.end(box);
    ProgressiveSource source{}; Mp4FragmentSource stream{}; Mp4FragmentAudio audio{};
    check(progressive_source_initialize(&source, block + fragment.size), "audio fragment sparse entity");
    if (!source.base) return;
    static u8 head[block];
    clear_bytes(head, sizeof(head)); copy_bytes(head, init.w.data, init.w.size); copy_bytes(head + init.w.size, index.data, index.size);
    check(progressive_source_write(&source, 0, {head, block}) &&
          mp4_fragment_source_initialize(&source, true, &stream) == E::none &&
          mp4_fragment_audio_initialize(&stream, &audio) == E::none && audio.packet_count == 3,
          "AAC adapter derives a bounded packet capacity without fetching or copying media");
    Mp4AudioSampleWindow window{};
    check(mp4_audio_sample_window(audio.timeline, 0, &window) && window.first_source_frame == 1024 &&
          window.end_source_frame == duration && window.leading_silence_frames == 44100 && window.duration_100ns == 10345578,
          "nonzero TFDT origin and open edit preserve exact priming, empty edit and short final padding");
    Mp4FragmentAudioPacket packet{}; packet.first_source_frame = 99;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const AccountVideoIdentity identity{1, 2, 3, 4, 0, 1, 0};
    AccountVideoSession session{};
    check(account_video_session_initialize_fragment_audio(&session, &audio, identity, event).ok() &&
          session.status.duration_100ns == window.duration_100ns && session.status.has_audio &&
          session.video_eof && !session.video && !session.workspace && !session.encoded_metadata,
          "fragmented AAC starts an audio-only session without a picture decoder or PCM allocation");
    check(account_video_session_initialize_fragment_audio(&session, &audio, identity, event).error == AccountVideoError::invalid_input,
          "fragmented AAC cannot overwrite a retained session");
    check(account_video_session_release(&session, 1000).ok(), "fragmented AAC metadata-only release");
    const u32 count = audio.packet_count;
    audio.packet_count = 0;
    check(account_video_session_initialize_fragment_audio(&session, &audio, identity, event).error == AccountVideoError::invalid_input &&
          !session.initialized && !session.workspace, "fragmented AAC rejects an empty adapter without changing session ownership");
    audio.packet_count = count;
    if (event) CloseHandle(event);
    check(mp4_fragment_audio_peek(&audio, &packet) == E::data_pending && packet.first_source_frame == 99 &&
          !mp4_fragment_audio_commit(&audio) && !audio.packet_index,
          "missing AAC metadata preserves output and cannot be accepted");
    check(progressive_source_write(&source, block, fragment.view()), "AAC packet range arrives");
    for (u32 i = 0; i < 3; ++i) {
        const i64 expected = (i64{origin} + i64{i} * 1024 - (origin + 1024)) * 10000000;
        const i64 floor = expected >= 0 ? expected / 44100 : -((-expected + 44099) / 44100);
        check(mp4_fragment_audio_peek(&audio, &packet) == E::none && packet.first_source_frame == u64{i} * 1024 &&
              packet.presentation_time_100ns == 10000000 + floor && packet.bytes.count == 10,
              "AAC packet native timestamps use the original rational clock including negative preroll");
        const auto saved = packet;
        check(mp4_fragment_audio_peek(&audio, &packet) == E::none && packet.bytes.data == saved.bytes.data &&
              packet.bytes.count == saved.bytes.count && packet.first_source_frame == saved.first_source_frame &&
              packet.presentation_time_100ns == saved.presentation_time_100ns && packet.duration_100ns == saved.duration_100ns &&
              audio.packet_index == i && mp4_fragment_audio_commit(&audio) && !mp4_fragment_audio_commit(&audio),
              "AAC backpressure repeats unchanged and only one successful commit advances");
    }
    check(mp4_fragment_audio_peek(&audio, &packet) == E::end_of_stream && !source.readers[1].active,
          "AAC EOF validates accepted count and retires byte demand");
    check(mp4_fragment_audio_seek(&audio, 1) == E::none && audio.packet_index == 1 &&
          mp4_fragment_audio_peek(&audio, &packet) == E::none && packet.first_source_frame == 1024 &&
          mp4_fragment_audio_seek(&audio, audio.packet_count) == E::none &&
          mp4_fragment_audio_peek(&audio, &packet) == E::end_of_stream,
          "AAC seeks preserve the exact source-frame identity and support an explicit EOF target");
    const auto audio_before = audio;
    check(mp4_fragment_audio_seek(&audio, audio.packet_count + 1) == E::invalid_input &&
          bytes_equal(&audio, &audio_before, sizeof(audio)), "out-of-range AAC seek cannot alter the accepted cursor");
    // A malformed real moof must not inherit the timing promised by its index.
    const u32 locations[]{clock, table, table + 16};
    const u32 values[]{origin + 1, 1023, tail + 1};
    for (u32 i = 0; i < 3; ++i) {
        mp4_fragment_source_release(&stream); audio = {};
        byte_store_be32(source.base + block + locations[i], values[i]);
        check(mp4_fragment_source_initialize(&source, true, &stream) == E::none &&
              mp4_fragment_audio_initialize(&stream, &audio) == E::none,
              "indexed audio timing remains provisional until its fragment is read");
        packet.first_source_frame = 99;
        check(mp4_fragment_audio_peek(&audio, &packet) == E::unsupported && !audio.packet_index &&
              !audio.pending && packet.first_source_frame == 99,
              "AAC adapter rejects decode jumps and mismatched real packet lengths transactionally");
        copy_bytes(source.base + block, fragment.data, fragment.size);
    }
    mp4_fragment_source_release(&stream); progressive_source_release(&source);
    // An edited presentation index excludes encoder priming from its first
    // interval, while TFDT/TRUN retain the original complete AAC packets.
    auto edited_init=initialization(true);byte_store_be32(edited_init.w.data+edited_init.edit,1600);
    const auto audio_fragment=[](u32 first,u32 count,u32 last) noexcept {
        Writer w{};const auto m=w.begin("moof");
        auto b=w.begin("mfhd");w.word(0);w.word(1);w.end(b);
        const auto t=w.begin("traf");
        b=w.begin("tfhd");w.word(0x20000);w.word(7);w.end(b);
        b=w.begin("tfdt");w.word(0);w.word(first);w.end(b);
        b=w.begin("trun");w.word(0x301);w.word(count);const u32 data_offset=w.size;w.word(0);
        for(u32 i=0;i<count;++i) {w.word(i+1==count?last:1024);w.word(10);}
        w.end(b);w.end(t);w.end(m);byte_store_be32(w.data+data_offset,w.size+8);
        w.word(8+count*10);w.word(0x6d646174);w.zero(count*10);return w;
    };
    const auto first_audio=audio_fragment(0,3,1024),last_audio=audio_fragment(3072,2,500);
    Writer edited_index{};auto ix=edited_index.begin("sidx");edited_index.word(0);edited_index.word(7);
    edited_index.word(44100);edited_index.word(0);edited_index.word(block-edited_init.w.size-56);edited_index.word(2);
    edited_index.word(first_audio.size);edited_index.word(3072-1600);edited_index.word(0x90000000);
    edited_index.word(last_audio.size);edited_index.word(1524);edited_index.word(0x90000000);edited_index.end(ix);
    const u32 omitted_priming[]{1600,1024,0};
    for(u32 priming:omitted_priming) {
    byte_store_be32(edited_init.w.data+edited_init.edit,priming?priming:1024);
    byte_store_be32(edited_index.data+36,3072-priming);
    source={};stream={};audio={};
    clear_bytes(head,sizeof(head));copy_bytes(head,edited_init.w.data,edited_init.w.size);
    copy_bytes(head+edited_init.w.size,edited_index.data,edited_index.size);
    Writer media{};copy_bytes(media.data,first_audio.data,first_audio.size);media.size=first_audio.size;
    copy_bytes(media.data+media.size,last_audio.data,last_audio.size);media.size+=last_audio.size;
    check(progressive_source_initialize(&source,block+media.size) && progressive_source_write(&source,0,{head,block}) &&
        mp4_fragment_source_initialize(&source,true,&stream)==E::none,"edited AAC metadata initializes before its first media range");
    const auto empty_audio=audio;
    check(mp4_fragment_audio_initialize(&stream,&audio)==E::data_pending && bytes_equal(&audio,&empty_audio,sizeof(audio)) &&
        source.readers[1].pending && !stream.loaded,"unknown first-fragment edit mapping preserves output and publishes byte demand");
    check(progressive_source_write(&source,block,media.view()) && mp4_fragment_audio_initialize(&stream,&audio)==E::none &&
        audio.index_edit_ticks==priming && audio.packet_count==5 && stream.loaded && !stream.fragment.index,
        "actual fragment timing distinguishes fractional whole-packet and already-included AAC priming");
    for(u32 i=0;i<5;++i) check(mp4_fragment_audio_peek(&audio,&packet)==E::none &&
        packet.first_source_frame==u64{i}*1024 && mp4_fragment_audio_commit(&audio),
        "edited index keeps sequential AAC packets on the original decode grid across file boundaries");
    check(mp4_fragment_audio_peek(&audio,&packet)==E::end_of_stream,"edited AAC index reaches exact padded EOF");
    const u32 edited_seeks[]{3,1,4,0};
    for(u32 i:edited_seeks) check(mp4_fragment_audio_seek(&audio,i)==E::none &&
        mp4_fragment_audio_peek(&audio,&packet)==E::none && packet.first_source_frame==u64{i}*1024,
        "edited AAC seeks translate the index hint while preserving exact source-frame identity");
    // A correct-looking index must not conceal a contradictory real clock.
    mp4_fragment_source_release(&stream);stream={};audio={};
    byte_store_be32(source.base+block+first_audio.size+60,3073);
    check(mp4_fragment_source_initialize(&source,true,&stream)==E::none && mp4_fragment_audio_initialize(&stream,&audio)==E::none,
        "edited AAC clock proof remains deferred until packet admission");
    packet.first_source_frame=99;
    check(mp4_fragment_audio_seek(&audio,3)==E::unsupported && packet.first_source_frame==99 && !audio.packet_index && !audio.pending,
        "edited AAC mapping rejects a real decode clock mismatch without changing the cursor");
    mp4_fragment_source_release(&stream);audio={};byte_store_be32(source.base+block+60,1);
    check(mp4_fragment_source_initialize(&source,true,&stream)==E::none &&
        mp4_fragment_audio_initialize(&stream,&audio)==E::unsupported && !audio.source && !stream.loaded,
        "first fragment must prove the claimed zero-based edit mapping before initialization publishes it");
    mp4_fragment_source_release(&stream);progressive_source_release(&source);
    if(priming==1024) {
        auto* fetch=static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr,sizeof(ProgressiveVideoFetch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        check(fetch!=nullptr,"edited AAC sparse fetch fixture");
        if(fetch) {
            fetch->kind=ProgressiveVideoKind::fragmented_audio;fetch->state=ProgressiveVideoFetchState::probing;
            fetch->expires_at_seconds=2000;
            check(progressive_source_initialize(&fetch->source,block+media.size,true) &&
                progressive_source_write(&fetch->source,0,{head,block}) && !progressive_video_fetch_restore(fetch) &&
                fetch->fragment.source==&fetch->source && !fetch->audio.source && fetch->source.readers[1].pending,
                "fetch retains the initialized index and missing first-fragment demand while AAC proof waits");
            const auto pending=progressive_video_fetch_next(fetch,false,1000);
            check(pending.step==ProgressiveVideoStep::submit && pending.range.first==block && pending.range.bytes==media.size,
                "AAC proof fetch requests the missing media range without repeating initialization metadata");
            check(progressive_source_write(&fetch->source,block,media.view()) && progressive_video_fetch_restore(fetch) &&
                fetch->audio.index_edit_ticks==1024 && fetch->fragment.loaded && !fetch->fragment.fragment.index &&
                progressive_video_fetch_restore(fetch) && mp4_fragment_audio_peek(&fetch->audio,&packet)==E::none &&
                packet.first_source_frame==0,"first packet proof resumes and reuses its metadata without consuming a sample");
            progressive_video_fetch_release(fetch);VirtualFree(fetch,0,MEM_RELEASE);
        }
        // A one-fragment track has no interior index boundary from which to
        // infer alignment, so it needs the same actual-clock proof.
        byte_store_be32(head+edited_init.w.size,44);
        byte_store_be32(head+edited_init.w.size+24,block-edited_init.w.size-44);
        byte_store_be32(head+edited_init.w.size+28,1);
        stream={};audio={};
        check(progressive_source_initialize(&source,block+first_audio.size) &&
            progressive_source_write(&source,0,{head,block}) && progressive_source_write(&source,block,first_audio.view()) &&
            mp4_fragment_source_initialize(&source,true,&stream)==E::none && mp4_fragment_audio_initialize(&stream,&audio)==E::none &&
            audio.index_edit_ticks==1024 && audio.packet_count==3,"single-fragment edited AAC retains all three original packets");
        for(u32 i=0;i<3;++i) check(mp4_fragment_audio_peek(&audio,&packet)==E::none && mp4_fragment_audio_commit(&audio),
            "single-fragment edited AAC advances through each packet exactly once");
        check(mp4_fragment_audio_peek(&audio,&packet)==E::end_of_stream,"single-fragment edited AAC has an exact EOF");
        mp4_fragment_source_release(&stream);progressive_source_release(&source);
    }
    }
    // Different track, edit and PCM clocks; compare against an independent
    // small-integer rational expression rather than repeated rounded durations.
    const Mp4AudioTimeline exact{96000, 90000, 90001, 1, 90000, 48000, 90000, 1001, 90000};
    for (u32 frame = 0; frame <= 96000; frame += 997) {
        const i64 numerator = (i64{frame} * 15 * 1001 - 8 * 1001 + 720000) * 10000000;
        constexpr i64 denominator = 720000ll * 1001;
        const i64 expected = numerator >= 0 ? numerator / denominator : -((-numerator + denominator - 1) / denominator);
        Mp4AudioFrameTimeBounds bounds{};
        check(mp4_audio_frame_time_bounds_100ns(exact, frame, &bounds) && bounds.floor_100ns == expected &&
              bounds.ceil_100ns == expected + i64{numerator % denominator != 0},
              "AAC rational bounds do not drift across unlike track/movie/PCM clocks");
    }
    i64 untouched = 99;
    auto invalid = exact; invalid.media_timescale = 0;
    check(!mp4_audio_frame_time_100ns(invalid, 0, &untouched) && untouched == 99 &&
          !mp4_audio_frame_time_100ns(exact, exact.source_frames + 1, &untouched) && untouched == 99,
          "invalid AAC clocks and out-of-range frames leave timestamp output unchanged");
    const Mp4AudioTimeline preroll{3072, 0, 1024, 0, 1524, 44100, 44100, 1000, 44100};
    Mp4AudioFrameTimeBounds negative{};
    check(mp4_audio_frame_time_bounds_100ns(preroll, 0, &negative) && negative.floor_100ns == -232200 &&
          negative.ceil_100ns == -232199 && mp4_audio_sample_window(preroll, 0, &window) &&
          window.first_source_frame == 1024 && window.end_source_frame == 2548 && !window.leading_silence_frames,
          "negative AAC preroll uses mathematical floor and trims only the declared priming and padding");
    invalid = exact; invalid.source_start_ticks = ~u64{0}; invalid.media_timescale = 1;
    check(!mp4_audio_frame_time_100ns(invalid, 0, &untouched) && untouched == 99,
          "unrepresentable AAC origin cannot wrap native timestamps");
    invalid = exact;
    check(!mp4_audio_frame_time_100ns(invalid, 0, reinterpret_cast<i64*>(&invalid.media_start_ticks)) &&
          bytes_equal(&invalid, &exact, sizeof(exact)), "timestamp outputs cannot overwrite their own clock metadata");
}
void source_tests() noexcept {
    using E = Mp4DemuxError;
    constexpr u32 block = kProgressiveBlockBytes, first_offset = block, first_span = 32 * block;
    constexpr u32 second_offset = first_offset + first_span;
    const auto init = initialization(false);
    auto first = fragment(), second = fragment(false);
    first.span = first_span;
    byte_store_be32(first.w.data + first.mdat, first_span - first.mdat);
    byte_store_be32(first.w.data + first.trun2 + 16, 24 * block);
    byte_store_be32(first.w.data + first.tfdt + 16, 900);
    byte_store_be32(second.w.data + second.tfdt + 16, 1400);
    const u32 total = second_offset + second.span;
    Writer index{};
    const u32 sidx = index.begin("sidx"); index.word(0); index.word(7); index.word(15360); index.word(1000);
    index.word(first_offset - init.w.size - 56); index.word(2);
    index.word(first_span); index.word(500); index.word(0x90000000);
    index.word(second.span); index.word(500); index.word(0x90000000); index.end(sidx);
    ProgressiveSource source{}; Mp4FragmentSource stream{};
    check(progressive_source_initialize(&source, total), "fragment source reserves one sparse entity");
    if (!source.base) return;
    static u8 buffer[block];
    const auto fill = [&](u64 offset) noexcept {
        const u32 size = static_cast<u32>(minimum<u64>(block, total - offset));
        for (u32 i = 0; i < size; ++i) buffer[i] = 0xa5;
        const auto overlay = [&](u64 start, ConstSlice<u8> input) noexcept {
            const u64 begin = maximum(start, offset), end = minimum(start + input.count, offset + size);
            if (begin < end) copy_bytes(buffer + begin - offset, input.data + begin - start, static_cast<u32>(end - begin));
        };
        overlay(0, init.w.view()); overlay(init.w.size, index.view());
        overlay(first_offset, first.w.view()); overlay(second_offset, second.w.view());
        return progressive_source_write(&source, offset, {buffer, size});
    };
    u32 requests = 0;
    const auto fetch = [&]() noexcept {
        const auto plan = progressive_schedule(source, {}, false);
        if (!plan.submit || !plan.request.bytes) return false;
        ++requests;
        for (u64 at = plan.request.first; at < plan.request.first + plan.request.bytes; at += block)
            if (!fill(at)) return false;
        return true;
    };
    check(mp4_fragment_source_initialize(&source, false, &stream) == E::data_pending && !stream.source &&
          source.readers[0].pending && !source.resident_blocks && !source.pinned_blocks,
          "absent initialization registers a range demand without allocating or publishing a partial owner");
    check(fill(0) && mp4_fragment_source_initialize(&source, false, &stream) == E::none &&
          source.resident_blocks == 1 && source.pinned_blocks == 1 && stream.segment.offset == first_offset,
          "initialization pins only the small codec/index prefix and leaves all media sparse");
    Mp4FragmentPacket packet{};
    packet.segment = 99;
    check(mp4_fragment_source_peek(&stream, &packet) == E::data_pending && packet.segment == 99 &&
          !stream.pending && !mp4_fragment_source_commit(&stream) && fetch(),
          "missing moof demands bytes without consuming a packet or replacing caller output");
    const u32 before_peek = requests;
    check(mp4_fragment_source_peek(&stream, &packet) == E::none && packet.sample.index == 0 &&
          packet.sample.decode_time == 900 && packet.bytes.count == 4 && packet.bytes.data[0] == 0xa5 &&
          source.readers[0].metadata_first == first_offset && source.readers[0].metadata_bytes == first.mdat,
          "loaded fragment holds its own header and preserves decode time independently of sidx presentation time");
    const auto retained = packet;
    const auto before_seek = stream;
    Mp4FragmentPosition proposal{}; proposal.generation = 99;
    check(mp4_fragment_source_prepare(&stream, 1, 0, &proposal) == E::data_pending && proposal.generation == 99 &&
          bytes_equal(&before_seek, &stream, sizeof(stream)) && source.readers[0].metadata_first == first_offset,
          "missing seek target records demand without replacing the active packet, metadata lease or proposal");
    Mp4AccessUnit unit{}; u64 duration{};
    check(mp4_fragment_source_unit(&stream, &unit) == E::none && unit.bytes.data == packet.bytes.data &&
          unit.presentation_time_ticks == 880 && unit.presentation_time_us_signed == 990625 &&
          unit.duration_ticks == 100 && unit.duration_us == 6510 &&
          mp4_fragment_source_duration(stream, &duration) && duration == 1063542,
          "native video access units preserve raw ticks and map signed edits without accumulated rounding");
    for (u32 i = 0; i < 8; ++i)
        check(mp4_fragment_source_peek(&stream, &packet) == E::none && packet.sample.index == retained.sample.index &&
              packet.bytes.data == retained.bytes.data && stream.fragment.index == 0 && requests == before_peek,
              "decoder backpressure reuses the identical borrowed packet without requests or cursor advancement");
    check(mp4_fragment_source_commit(&stream) && !mp4_fragment_source_commit(&stream),
          "exactly one commit consumes an offered packet");
    for (u32 i = 1; i < 3; ++i)
        check(mp4_fragment_source_peek(&stream, &packet) == E::none && packet.sample.index == i &&
              mp4_fragment_source_commit(&stream), "sequential samples share the current metadata lease");
    check(mp4_fragment_source_peek(&stream, &packet) == E::data_pending && stream.fragment.index == 3 &&
          source.readers[0].missing >= first_offset + 24 * block && fetch(),
          "distant sample payload records demand while preserving the current run cursor");
    progressive_source_evict(&source, 2 * block);
    check(progressive_source_available(source, first_offset, first.w.size) &&
          !progressive_source_available(source, 2 * block, 1) && source.pinned_blocks == 1,
          "current moof survives eviction far behind playback without becoming permanently pinned");
    for (u32 i = 3; i < 5; ++i)
        check(mp4_fragment_source_peek(&stream, &packet) == E::none && packet.sample.index == i &&
              mp4_fragment_source_commit(&stream), "retained metadata still describes far-ahead sample bytes");
    check(mp4_fragment_source_peek(&stream, &packet) == E::data_pending && stream.segment.index == 1 &&
          !source.readers[0].metadata_bytes && fetch(),
          "consumed segment advances and releases its old header before fetching the next fragment");
    check(mp4_fragment_source_peek(&stream, &packet) == E::none && packet.segment == 1 &&
          packet.sample.index == 0 && packet.sample.decode_time == 1400 &&
          source.readers[0].metadata_first == second_offset,
          "next fragment uses its real decode clock and a replacement metadata lease");
    const auto saved = stream;
    check(!mp4_fragment_source_select(&stream, 3) && bytes_equal(&saved, &stream, sizeof(stream)),
          "invalid coarse seek cannot disturb a pending packet");
    check(mp4_fragment_source_prepare(&stream, 1, 2, &proposal) == E::none &&
          proposal.packet.sample.index == 2 && bytes_equal(&saved, &stream, sizeof(stream)),
          "ready seek proposal walks metadata without moving the active decoder cursor");
    check(progressive_source_trim(&source) && !mp4_fragment_source_adopt(&stream, proposal) &&
          bytes_equal(&saved, &stream, sizeof(stream)), "source mutation invalidates a previously prepared seek");
    check(fill(first_offset) && mp4_fragment_source_prepare(&stream, 0, 3, &proposal) == E::data_pending &&
          source.readers[0].metadata_first == second_offset && source.readers[0].seek_metadata_first == first_offset,
          "distant seek payload retains both the active header and its pending target header");
    progressive_source_trim(&source);
    check(progressive_source_available(source, first_offset, first.mdat) &&
          progressive_source_available(source, second_offset, second.mdat) &&
          fill(first_offset + 24 * block) && mp4_fragment_source_prepare(&stream, 0, 3, &proposal) == E::none &&
          mp4_fragment_source_adopt(&stream, proposal) && stream.fragment.index == 3 &&
          !source.readers[0].seek_metadata_bytes &&
          mp4_fragment_source_prepare(&stream, 1, 0, &proposal) == E::none && mp4_fragment_source_adopt(&stream, proposal),
          "seek survives aggressive eviction and adopts after one payload refill without refetching its header");
    check(mp4_fragment_source_prepare(&stream, 1, 2, &proposal) == E::none &&
          mp4_fragment_source_adopt(&stream, proposal) && stream.fragment.index == 2 && stream.pending &&
          mp4_fragment_source_prepare(&stream, 1, 0, &proposal) == E::none &&
          mp4_fragment_source_adopt(&stream, proposal) && stream.fragment.index == 0,
          "validated seek adoption swaps cursor and lease together without consuming its target packet");
    for (u32 i = 0; i < 5; ++i)
        check(mp4_fragment_source_peek(&stream, &packet) == E::none && mp4_fragment_source_commit(&stream),
              "last segment consumes its remaining packets");
    check(mp4_fragment_source_peek(&stream, &packet) == E::end_of_stream && !source.readers[0].active &&
          !source.readers[0].metadata_bytes && !progressive_schedule(source, {}, true).submit,
          "natural EOF releases metadata and retires its read-ahead cursor");
    check(mp4_fragment_source_select(&stream, 0) && !stream.pending && !stream.loaded &&
          !mp4_fragment_source_commit(&stream) && !source.readers[0].metadata_bytes,
          "valid coarse seek invalidates the old pending packet before the decoder generation changes");
    check(mp4_fragment_source_select(&stream, stream.index.count) &&
          mp4_fragment_source_peek(&stream, &packet) == E::end_of_stream &&
          !progressive_schedule(source, {}, true).submit && !source.readers[0].metadata_bytes,
          "EOF selection clears pending ranges and does not create background requests");
    mp4_fragment_source_release(&stream);
    progressive_source_trim(&source);
    check(!stream.source && source.resident_blocks == 1 && source.pinned_blocks == 1 && requests == 3,
          "joined source release drops all fragment payload and metadata, retaining only initialization");
    auto* transport = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(transport != nullptr, "fragment range driver allocation");
    if (transport) {
        const u8 target[]{'/','v'};
        check(progressive_video_fetch_begin(transport, {target, sizeof(target)}, CdnHost::google_video, 100,
              ProgressiveVideoKind::fragmented_video), "fragment range driver begins independently of ordinary MP4 tables");
        const auto head = progressive_video_fetch_next(transport, false, 1);
        progressive_video_fetch_submitted(transport, 1, head.range);
        CdnResponseResult policy{}; policy.disposition = CdnResponseDisposition::accept_partial;
        policy.body_bytes = block; policy.entity_bytes = total;
        check(head.step == ProgressiveVideoStep::submit &&
              progressive_video_fetch_accept(transport, 1, true, policy, {source.base, block}) == ProgressiveVideoAccept::progress &&
              progressive_video_fetch_next(transport, false, 1).step == ProgressiveVideoStep::ready &&
              transport->fragment.source == &transport->source,
              "head response establishes a stable borrowed fragment owner");
        check(mp4_fragment_source_peek(&transport->fragment, &packet) == E::data_pending,
              "fragment range driver reports missing payload demand");
        progressive_video_fetch_retire(transport);
        check(!transport->fragment.source && !transport->source.readers[0].active &&
              transport->source.resident_blocks == transport->source.pinned_blocks &&
              progressive_video_fetch_restore(transport) && transport->fragment.source == &transport->source,
              "joined cached retirement drops readers and restores from retained initialization");
        progressive_video_fetch_release(transport); VirtualFree(transport, 0, MEM_RELEASE);
    }
    progressive_source_release(&source);
}
void multiplexed_fragment_tests() noexcept {
    using E = Mp4DemuxError;
    Writer w{}; u32 headers[2]{}, tables[2]{}, offsets[2]{};
    const u32 moof = w.begin("moof");
    auto box = w.begin("mfhd"); w.word(0); w.word(1); w.end(box);
    for (u32 i = 0; i < 2; ++i) {
        const u32 traf = w.begin("traf");
        headers[i] = w.begin("tfhd"); w.word(0x20000); w.word(i + 1); w.end(headers[i]);
        box = w.begin("tfdt"); w.word(0); w.word(i ? 9000 : 1000); w.end(box);
        tables[i] = w.begin("trun"); w.word(0x301); w.word(2); offsets[i] = w.size; w.word(0);
        for (u32 n = 0; n < 2; ++n) { w.word(i ? 100 : 1024); w.word(i ? 4 : 3); }
        w.end(tables[i]); w.end(traf);
    }
    w.end(moof); box = w.begin("mdat"); const u32 payload = w.size; w.zero(14); w.end(box);
    byte_store_be32(w.data + offsets[0], payload); byte_store_be32(w.data + offsets[1], payload + 6);
    Mp4Fragment retained{};
    for (u32 i = 0; i < 2; ++i) {
        const Mp4FragmentDefaults defaults{i + 1}; Mp4Fragment cursor{}; Mp4FragmentSample sample{};
        check(mp4_fragment_initialize(w.view(), 800, w.size, defaults, &cursor) == E::none && cursor.count == 2 &&
            cursor.first_time == (i ? 9000u : 1000u) && cursor.duration == (i ? 200u : 2048u) &&
            mp4_fragment_next(&cursor, &sample) == E::none && sample.offset == 800 + payload + (i ? 6u : 0u) &&
            sample.bytes == (i ? 4u : 3u), "multiplexed moof selects only the matching track and preserves its independent clock and payload");
        retained = cursor;
    }
    const Mp4FragmentDefaults selected{2}; const auto saved = retained;
    const auto reject = [&](u32 at, u32 value, E expected) noexcept {
        auto bad = w; byte_store_be32(bad.data + at, value);
        check(mp4_fragment_initialize(bad.view(), 800, bad.size, selected, &retained) == expected &&
              bytes_equal(&retained, &saved, sizeof(saved)), "ambiguous multiplexed metadata cannot replace the retained track cursor");
    };
    reject(headers[0] + 12, 2, E::malformed);
    reject(headers[0] + 12, 0, E::malformed);
    reject(tables[0] + 4, 0x74666864, E::malformed);
    reject(headers[1] + 8, 0, E::unsupported);
    reject(offsets[1], payload - 1, E::malformed);
    auto implicit = w; byte_store_be32(implicit.data + headers[0] + 8, 0);
    check(mp4_fragment_initialize(implicit.view(), 800, implicit.size, {1}, &retained) == E::none,
          "the first traf retains ISO implicit moof-base behavior");
    check(mp4_fragment_initialize(w.view(), 800, w.size, {3}, &retained) == E::malformed,
          "a missing requested track cannot substitute another stream");
}
void run() noexcept {
    using E = Mp4DemuxError;
    Writer trex{}; auto at = trex.begin("trex");
    trex.word(0); trex.word(7); trex.word(1); trex.word(33); trex.word(3); trex.word(0); trex.end(at);
    Mp4FragmentDefaults defaults{};
    check(mp4_fragment_defaults(trex.view(), &defaults) == E::none && defaults.track_id == 7 && defaults.duration == 33,
          "fragment defaults retain the actual track identity and sample defaults");
    const auto saved_defaults = defaults;
    byte_store_be32(trex.data + 16, 2);
    check(mp4_fragment_defaults(trex.view(), &defaults) == E::unsupported &&
          bytes_equal(&defaults, &saved_defaults, sizeof(defaults)), "description switches refuse transactionally");
    const bool variants[]{false, true};
    for (bool second_offset : variants) {
        auto f = fragment(second_offset);
        Mp4Fragment cursor{};
        check(mp4_fragment_initialize(f.w.view(), 1000, f.span, defaults, &cursor) == E::none &&
              cursor.count == 5 && cursor.run_count == 2 && cursor.duration == 500 && cursor.first_time == 1000,
              "fragment initializes from metadata only, without resident sample payload or sample index allocation");
        const u64 times[]{1000, 1100, 1210, 1300, 1400};
        const i64 presentation[]{980, 1130, 1210, 1300, 1400};
        const u32 sizes[]{4, 6, 5, 4, 4}, offsets[]{0, 4, 10, second_offset ? 17u : 15u, second_offset ? 21u : 19u};
        const bool sync[]{true, false, true, false, true};
        for (u32 pass = 0; pass < 2; ++pass) {
            for (u32 i = 0; i < 5; ++i) {
                Mp4FragmentSample sample{};
                check(mp4_fragment_next(&cursor, &sample) == E::none && sample.index == i &&
                      sample.decode_time == times[i] && sample.presentation_time == presentation[i] &&
                      sample.offset == 1000 + f.mdat + 8 + offsets[i] && sample.bytes == sizes[i] && sample.sync == sync[i],
                      "fragment cursor preserves signed composition, explicit/default durations, sizes and first flags");
            }
            const auto before = cursor;
            Mp4FragmentSample sample{77}; const auto old = sample;
            check(mp4_fragment_next(&cursor, &sample) == E::end_of_stream &&
                  bytes_equal(&old, &sample, sizeof(sample)) && bytes_equal(&before, &cursor, sizeof(cursor)),
                  "fragment EOF does not consume or fabricate another sample");
            mp4_fragment_reset(&cursor);
        }
    }
    auto fixture = fragment();
    const auto wide = fragment(true, true, true);
    Mp4Fragment wide_cursor{}; Mp4FragmentSample wide_sample{};
    check(mp4_fragment_initialize(wide.w.view(), 1000, wide.span, defaults, &wide_cursor) == E::none &&
          mp4_fragment_next(&wide_cursor, &wide_sample) == E::none &&
          wide_sample.offset == 1000 + wide.mdat + 16 && wide_sample.decode_time == 1000,
          "explicit absolute data base and 64-bit mdat header preserve the sample range");
    Mp4Fragment cursor{};
    check(mp4_fragment_initialize(fixture.w.view(), 1000, fixture.span, defaults, &cursor) == E::none,
          "valid fragment retained for transactional failures");
    const auto saved = cursor;
    for (u32 n = 0; n < fixture.w.size; ++n) {
        check(mp4_fragment_initialize({fixture.w.data, n}, 1000, fixture.span, defaults, &cursor) != E::none &&
              bytes_equal(&saved, &cursor, sizeof(cursor)), "every truncated metadata/header prefix preserves cursor");
    }
    const auto reject = [&](Fixture f, u32 offset, u32 value, E expected) noexcept {
        byte_store_be32(f.w.data + offset, value);
        check(mp4_fragment_initialize(f.w.view(), 1000, f.span, defaults, &cursor) == expected &&
              bytes_equal(&saved, &cursor, sizeof(cursor)), "invalid fragment cannot replace live cursor");
    };
    reject(fixture, fixture.tfhd + 12, 8, E::malformed); // Track mismatch.
    reject(fixture, fixture.tfhd + 16, 2, E::unsupported);
    reject(fixture, fixture.tfdt + 8, 2, E::unsupported);
    reject(fixture, fixture.tfdt + 12, 0x80000000, E::malformed);
    reject(fixture, fixture.trun + 12, 65537, E::malformed);
    reject(fixture, fixture.trun + 16, 0, E::malformed); // Sample points into metadata.
    reject(fixture, fixture.trun + 24, 1000, E::malformed); // Payload overrun.
    reject(fixture, fixture.trun + 20, 0, E::malformed); // Zero duration.
    reject(fixture, fixture.trun2 + 16, fixture.mdat + 8, E::malformed); // Overlapping runs.
    reject(fixture, fixture.trun2 + 4, 0x73656e63, E::unsupported); // senc.
    reject(fixture, fixture.mdat, 8, E::malformed);
    reject(fixture, fixture.trun + 8, 0x01000f05, E::malformed); // Conflicting sample flags.
    for (u32 position = 0; position < fixture.w.size; ++position) {
        auto mutated = fixture;
        mutated.w.data[position] ^= 0x80;
        auto candidate = saved;
        const auto status = mp4_fragment_initialize(mutated.w.view(), 1000, mutated.span, defaults, &candidate);
        if (status != E::none) {
            check(bytes_equal(&candidate, &saved, sizeof(candidate)), "mutated metadata failures are transactional");
            continue;
        }
        u64 previous = 1000 + mutated.mdat + 8, time = candidate.first_time;
        for (u32 n = 0; n < candidate.count; ++n) {
            Mp4FragmentSample sample{};
            const auto result = mp4_fragment_next(&candidate, &sample);
            check(result == E::none && sample.index == n && sample.offset >= previous &&
                  sample.offset + sample.bytes <= 1000 + mutated.span && sample.decode_time == time &&
                  sample.duration && sample.bytes, "accepted mutated metadata preserves bounded ordered sample ranges and clock");
            if (result != E::none) break;
            previous = sample.offset + sample.bytes; time += sample.duration;
        }
    }
    for (u32 version = 0; version < 2; ++version) {
        Writer w{}; at = w.begin("sidx"); w.word(version << 24); w.word(7); w.word(1000);
        if (version) w.word(0); w.word(1000);
        if (version) w.word(0); w.word(4);
        w.word(2); const u32 rows = w.size;
        w.word(1000); w.word(500); w.word(0x90000000);
        w.word(1000); w.word(500); w.word(0); w.end(at);
        const u64 source = version ? 0x100000000ull : 20;
        const u64 first = source + w.size + 4;
        Mp4FragmentIndex index{};
        check(mp4_fragment_index_initialize(w.view(), source, first + 2000, &index) == E::none &&
              index.count == 2 && index.duration == 1000 && index.first_time == 1000 && index.timescale == 1000,
              "flat sidx bounds exact track ticks and 32/64-bit absolute byte offsets");
        Mp4FragmentRange range{};
        check(mp4_fragment_index_range(index, 0, &range) && range.offset == first && range.bytes == 1000 &&
              range.starts_with_sap && range.sap_type == 1 && !range.sap_delta &&
              mp4_fragment_index_seek(index, 1500, &range) && range.offset == first + 1000 && range.index == 1 &&
              !range.starts_with_sap && !range.sap_type,
              "sidx lookup finds the containing segment and reports random-access evidence without inventing it");
        const auto previous = range; const auto old = index;
        check(!mp4_fragment_index_seek(index, 2000, &range) && !mp4_fragment_index_seek(index, 999, &range) &&
              !mp4_fragment_index_range(index, 2, &range) && bytes_equal(&range, &previous, sizeof(range)),
              "index endpoint and out-of-range lookups leave output unchanged");
        byte_store_be32(w.data + rows, 0x800003e8);
        check(mp4_fragment_index_initialize(w.view(), source, first + 2000, &index) == E::unsupported &&
              bytes_equal(&index, &old, sizeof(index)), "hierarchical references cannot be mistaken for media bytes");
        byte_store_be32(w.data + rows, 1000);
        check(mp4_fragment_index_initialize(w.view(), source, first + 1999, &index) == E::malformed &&
              bytes_equal(&index, &old, sizeof(index)), "index cannot reference bytes outside the fetched entity");
    }
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_mp4_fragment_entry() noexcept {
    maicord::run(); maicord::empty_roll_tests(); maicord::multiplexed_fragment_tests(); maicord::track_tests(); maicord::source_tests(); maicord::media_segment_tests(); maicord::multipart_segment_tests(); maicord::segment_store_tests(); maicord::live_segment_tests(); maicord::live_audio_tests(); maicord::live_fetch_tests(); maicord::live_alignment_tests(); maicord::segmented_fetch_tests(); maicord::segmented_fetch_tests(true); maicord::segmented_fetch_tests(true, true); maicord::audio_tests();
    maicord::check(maicord::fragment_audio_playback_probe(), "finite fragmented audio plays and seeks to its end without a picture decoder");
    ExitProcess(maicord::failures ? 1 : 0);
}
