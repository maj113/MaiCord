#include "maicord/app/progressive_video_fetch.hpp"
#include "maicord/media/progressive_source.hpp"
#include "maicord/protocol/cdn_http.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

// Progressive MP4 playback: Range/206 parsing, sparse-source bookkeeping, moov
// probing, pending reads, scheduling, signed-URL refresh, the resident memory
// bound and an end-to-end fetch against a fake range server. Every MP4 is an
// original fixture generated here.
namespace maicord {
namespace {

constexpr u32 kSampleCount = 60;
constexpr u32 kSamplesPerChunk = 5;
constexpr u32 kGop = 10;
constexpr u64 kNow = 1'700'000'000ull;

struct Fixture {
    u8* bytes;
    u32 count;
    u32 capacity;
    u32 sample_bytes;
    u32 stco_entries;
    u32 mdat_payload;
    bool ok;
};

void put_u8(Fixture* f, u8 value) noexcept {
    if (!f->ok || f->count >= f->capacity) {
        f->ok = false;
        return;
    }
    f->bytes[f->count++] = value;
}
void put_u16(Fixture* f, u16 value) noexcept {
    put_u8(f, static_cast<u8>(value >> 8));
    put_u8(f, static_cast<u8>(value));
}
void put_u32(Fixture* f, u32 value) noexcept {
    put_u16(f, static_cast<u16>(value >> 16));
    put_u16(f, static_cast<u16>(value));
}
void patch_u32(Fixture* f, u32 at, u32 value) noexcept {
    for (u32 i = 0; i < 4; ++i)
        f->bytes[at + i] = static_cast<u8>(value >> (24 - 8 * i));
}
u32 begin_box(Fixture* f, const char* type) noexcept {
    const u32 at = f->count;
    put_u32(f, 0);
    for (u32 i = 0; i < 4; ++i)
        put_u8(f, static_cast<u8>(type[i]));
    return at;
}
void end_box(Fixture* f, u32 at) noexcept {
    if (f->ok)
        patch_u32(f, at, f->count - at);
}
u8 payload_byte(u32 sample, u32 index) noexcept {
    return static_cast<u8>(sample * 31u + index * 7u + 3u);
}
void put_moov(Fixture* f) noexcept {
    const u32 moov = begin_box(f, "moov");
    const u32 trak = begin_box(f, "trak");
    const u32 mdia = begin_box(f, "mdia");
    const u32 mdhd = begin_box(f, "mdhd");
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, 1'000);
    put_u32(f, kSampleCount * 100);
    put_u32(f, 0);
    end_box(f, mdhd);
    const u32 hdlr = begin_box(f, "hdlr");
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, 0x76696465u);
    end_box(f, hdlr);
    const u32 minf = begin_box(f, "minf");
    const u32 stbl = begin_box(f, "stbl");
    const u32 stsd = begin_box(f, "stsd");
    put_u32(f, 0);
    put_u32(f, 1);
    const u32 avc1 = begin_box(f, "avc1");
    for (u32 i = 0; i < 78; ++i)
        put_u8(f, i == 25 || i == 27 ? 16 : 0);
    const u32 avcc = begin_box(f, "avcC");
    constexpr u8 configuration[]{1, 66, 0, 30, 0xff, 0xe1, 0, 2, 0x67, 0, 1, 0, 2, 0x68, 0};
    for (u8 value : configuration)
        put_u8(f, value);
    end_box(f, avcc);
    end_box(f, avc1);
    end_box(f, stsd);
    const u32 stts = begin_box(f, "stts");
    put_u32(f, 0);
    put_u32(f, 1);
    put_u32(f, kSampleCount);
    put_u32(f, 100);
    end_box(f, stts);
    const u32 stsc = begin_box(f, "stsc");
    put_u32(f, 0);
    put_u32(f, 1);
    put_u32(f, 1);
    put_u32(f, kSamplesPerChunk);
    put_u32(f, 1);
    end_box(f, stsc);
    const u32 stsz = begin_box(f, "stsz");
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, kSampleCount);
    for (u32 i = 0; i < kSampleCount; ++i)
        put_u32(f, f->sample_bytes);
    end_box(f, stsz);
    const u32 stco = begin_box(f, "stco");
    put_u32(f, 0);
    put_u32(f, kSampleCount / kSamplesPerChunk);
    f->stco_entries = f->count;
    for (u32 i = 0; i < kSampleCount / kSamplesPerChunk; ++i)
        put_u32(f, 0);
    end_box(f, stco);
    const u32 stss = begin_box(f, "stss");
    put_u32(f, 0);
    put_u32(f, kSampleCount / kGop);
    for (u32 i = 0; i < kSampleCount; i += kGop)
        put_u32(f, i + 1);
    end_box(f, stss);
    end_box(f, stbl);
    end_box(f, minf);
    end_box(f, mdia);
    end_box(f, trak);
    end_box(f, moov);
}
void put_mdat(Fixture* f) noexcept {
    const u32 mdat = begin_box(f, "mdat");
    f->mdat_payload = f->count;
    for (u32 s = 0; s < kSampleCount; ++s) {
        put_u32(f, f->sample_bytes - 4);
        put_u8(f, s % kGop == 0 ? u8{0x65} : u8{0x41});
        for (u32 i = 5; i < f->sample_bytes && f->ok; ++i)
            f->bytes[f->count++] = payload_byte(s, i);
    }
    end_box(f, mdat);
}
// ftyp, then moov and mdat in either order; one avc1 track, ten GOPs.
bool build(Fixture* f, u32 sample_bytes, bool moov_at_end) noexcept {
    const u32 capacity = 4096 + kSampleCount * (sample_bytes + 8);
    *f = {};
    f->bytes = static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    f->capacity = capacity;
    f->sample_bytes = sample_bytes;
    f->ok = f->bytes != nullptr;
    const u32 ftyp = begin_box(f, "ftyp");
    put_u32(f, 0x69736f6du);
    put_u32(f, 0x200u);
    end_box(f, ftyp);
    if (moov_at_end) {
        put_mdat(f);
        put_moov(f);
    } else {
        put_moov(f);
        put_mdat(f);
    }
    for (u32 i = 0; f->ok && i < kSampleCount / kSamplesPerChunk; ++i)
        patch_u32(f, f->stco_entries + i * 4, f->mdat_payload + i * kSamplesPerChunk * sample_bytes);
    return f->ok;
}
void release(Fixture* f) noexcept {
    if (f->bytes)
        VirtualFree(f->bytes, 0, MEM_RELEASE);
    *f = {};
}

u32 put_text(char* out, u32 at, const char* text) noexcept {
    while (*text)
        out[at++] = *text++;
    return at;
}
u32 put_number(char* out, u32 at, u64 value) noexcept {
    char digits[24];
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        out[at++] = digits[--count];
    return at;
}
ConstSlice<u8> head(const char* text) noexcept {
    u32 count = 0;
    while (text[count])
        ++count;
    return {reinterpret_cast<const u8*>(text), count};
}

// Fake CDN: answers one range with a 206 (or a configured failure mode),
// validated through the same protocol parser the client uses.
struct FakeServer {
    const Fixture* source;
    char head[512];
    u32 requests;
    u32 expire_at_request; // 1-based; answers 403 once
    bool ignore_range;
    bool short_answers;
};
struct FakeAnswer {
    CdnResponseResult policy;
    ConstSlice<u8> body;
};
FakeAnswer serve(FakeServer* server, CdnByteRange range) noexcept {
    ++server->requests;
    u32 at = 0;
    ConstSlice<u8> body{};
    if (server->expire_at_request == server->requests) {
        at = put_text(server->head, at, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
    } else if (server->ignore_range) {
        body = {server->source->bytes, server->source->count};
        at = put_text(server->head, at, "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: ");
        at = put_number(server->head, at, body.count);
        at = put_text(server->head, at, "\r\n\r\n");
    } else {
        const u64 last = minimum<u64>(range.first + range.bytes, server->source->count) - 1;
        u64 bytes = last - range.first + 1;
        if (server->short_answers && bytes >= 2 * kProgressiveBlockBytes)
            bytes = bytes / 2 + 1; // Not block-aligned: the trailing partial block is refetched.
        body = {server->source->bytes + range.first, static_cast<u32>(bytes)};
        at = put_text(server->head, at, "HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\n"
                                        "Cache-Control: max-age=3600\r\nContent-Length: ");
        at = put_number(server->head, at, bytes);
        at = put_text(server->head, at, "\r\nContent-Range: bytes ");
        at = put_number(server->head, at, range.first);
        at = put_text(server->head, at, "-");
        at = put_number(server->head, at, range.first + bytes - 1);
        at = put_text(server->head, at, "/");
        at = put_number(server->head, at, server->source->count);
        at = put_text(server->head, at, "\r\n\r\n");
    }
    const auto policy = validate_cdn_range_response(
        CdnImageType::mp4, {reinterpret_cast<const u8*>(server->head), at}, body.count, range, kNow);
    return {policy, body};
}

// One fetch step: submit what the driver asks for and feed the answer back.
ProgressiveVideoAccept pump(ProgressiveVideoFetch* fetch, FakeServer* server, bool read_ahead, u64* id,
                            ProgressiveVideoStep* step) noexcept {
    const auto next = progressive_video_fetch_next(fetch, read_ahead, kNow);
    *step = next.step;
    if (next.step != ProgressiveVideoStep::submit)
        return ProgressiveVideoAccept::ignored;
    progressive_video_fetch_submitted(fetch, ++*id, next.range);
    const auto answer = serve(server, next.range);
    return progressive_video_fetch_accept(fetch, *id, true, answer.policy, answer.body);
}

int http_tests() noexcept {
    u8 output[kCdnMaximumRequestHeadBytes];
    const auto target = head("/attachments/1/2/movie.mp4");
    const auto written = write_static_cdn_get_request(CdnImageType::mp4, CdnHost::cdn,
                                                      target, {}, {output, sizeof(output)},
                                                      {1048576, 2097152});
    const auto expected = head("Range: bytes=1048576-3145727\r\n");
    bool found = false;
    for (u32 i = 0; written.ok() && i + expected.count <= written.bytes; ++i)
        found = found || bytes_equal(output + i, expected.data, expected.count);
    if (!found)
        return 101;
    if (write_static_cdn_get_request(CdnImageType::mp4, CdnHost::cdn, target,
                                     head("\"e\""), {output, sizeof(output)}, {0, 16})
            .ok() ||
        write_static_cdn_get_request(CdnImageType::png, CdnHost::cdn, target, {},
                                     {output, sizeof(output)}, {0, 16})
            .ok())
        return 102;
    const auto validate = [](const char* text, u32 body, CdnByteRange range) noexcept {
        return validate_cdn_range_response(CdnImageType::mp4, head(text), body, range, kNow);
    };
    const CdnByteRange range{100, 50};
    auto valid = validate("HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\nContent-Length: 50\r\n"
                          "Content-Range: bytes 100-149/1000\r\n\r\n",
                          50, range);
    if (!valid.ok() || valid.disposition != CdnResponseDisposition::accept_partial ||
        valid.range_first != 100 || valid.entity_bytes != 1000 || valid.body_bytes != 50)
        return 103;
    // A short answer at the entity end is still a valid prefix of the request.
    valid = validate("HTTP/1.1 206 Partial Content\r\nContent-Type: video/quicktime\r\n"
                     "Content-Range: bytes 100-119/120\r\n\r\n",
                     20, range);
    if (!valid.ok() || valid.entity_bytes != 120)
        return 104;
    const char* invalid[]{
        "HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\nContent-Range: bytes 101-150/1000\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\nContent-Range: bytes 100-148/1000\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\nContent-Range: bytes 100-150/1000\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\nContent-Range: bytes 100-149/*\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\nContent-Range: bytes 100-149/120\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Type: multipart/byteranges; boundary=x\r\n"
        "Content-Range: bytes 100-149/1000\r\n\r\n",
    };
    for (const char* text : invalid)
        if (validate(text, 50, range).ok())
            return 105;
    if (validate("HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\nContent-Range: bytes 100-149/1000\r\n"
                 "Content-Range: bytes 100-149/1000\r\n\r\n",
                 50, range)
            .error != CdnHttpError::duplicate_field)
        return 106;
    // A server that ignores Range sends the whole entity with 200.
    const auto ignored = validate("HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: 700\r\n\r\n",
                                  700, range);
    if (!ignored.ok() || ignored.disposition != CdnResponseDisposition::accept_body ||
        ignored.entity_bytes != 700 || ignored.range_first != 0)
        return 107;
    const char* expired[]{"HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\n\r\n",
                          "HTTP/1.1 410 Gone\r\nContent-Length: 9\r\n\r\n"};
    for (const char* text : expired)
        if (validate(text, 9, range).disposition != CdnResponseDisposition::source_expired)
            return 108;
    if (validate("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */1000\r\n\r\n", 0, range).ok() ||
        validate_static_image_response(CdnImageType::mp4,
                                       head("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"), 0,
                                       kNow)
            .ok())
        return 109;
    return 0;
}

int sparse_tests() noexcept {
    ProgressiveSource s{};
    constexpr u32 total = 3 * kProgressiveBlockBytes + 1000;
    if (!progressive_source_initialize(&s, total) || s.block_count != 4 || progressive_source_complete(s))
        return 201;
    static u8 data[2 * kProgressiveBlockBytes];
    for (u32 i = 0; i < sizeof(data); ++i)
        data[i] = static_cast<u8>(i * 13u);
    // Unaligned write: only the fully covered block 1 becomes resident.
    if (!progressive_source_write(&s, 100, {data, kProgressiveBlockBytes * 2}) || s.resident_blocks != 1 ||
        progressive_source_available(s, 0, 1) || !progressive_source_available(s, kProgressiveBlockBytes, 64) ||
        progressive_source_available(s, kProgressiveBlockBytes, kProgressiveBlockBytes + 1) ||
        s.base[kProgressiveBlockBytes] != data[kProgressiveBlockBytes - 100])
        return 202;
    // The short final block counts once the write reaches the entity end.
    if (!progressive_source_write(&s, 3 * kProgressiveBlockBytes, {data, 1000}) || s.resident_blocks != 2 ||
        !progressive_source_available(s, total - 1000, 1000) || progressive_source_available(s, total - 1, 2))
        return 203;
    if (progressive_source_write(&s, total - 10, {data, 11}) || progressive_source_available(s, total, 1) ||
        !progressive_source_available(s, total, 0))
        return 204;
    if (!progressive_source_write(&s, 0, {data, kProgressiveBlockBytes}) ||
        !progressive_source_write(&s, 2 * kProgressiveBlockBytes, {data, kProgressiveBlockBytes}) ||
        !progressive_source_complete(s) || progressive_source_payload_blocks(s) != 4)
        return 205;
    // Pinned metadata survives trimming and eviction.
    progressive_source_pin(&s, 10, 20);
    if (s.pinned_blocks != 1 || progressive_source_payload_blocks(s) != 3 || progressive_source_trim(&s) != 3 ||
        !progressive_source_available(s, 0, kProgressiveBlockBytes) || progressive_source_available(s, total - 1, 1))
        return 206;
    const u64 generation = s.generation;
    if (!progressive_source_write(&s, kProgressiveBlockBytes, {data, kProgressiveBlockBytes}) ||
        s.generation == generation)
        return 207;
    progressive_source_release(&s);
    return s.base ? 208 : 0;
}

ProgressiveSource* new_source() noexcept {
    return static_cast<ProgressiveSource*>(
        VirtualAlloc(nullptr, sizeof(ProgressiveSource), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}
void free_source(ProgressiveSource* s) noexcept {
    progressive_source_release(s);
    VirtualFree(s, 0, MEM_RELEASE);
}
bool fill(ProgressiveSource* s, const Fixture& f, ProgressiveRange r) noexcept {
    return progressive_source_write(s, r.first, {f.bytes + r.first, r.bytes});
}

int probe_tests() noexcept {
    Fixture f{};
    if (!build(&f, 100 * 1024, true))
        return 301;
    auto* s = new_source();
    int failure = 0;
    if (!progressive_source_initialize(s, f.count) || !fill(s, f, {0, kProgressiveHeadProbeBytes}))
        failure = 302;
    // Head probe holds ftyp and the mdat header; moov follows the media data.
    auto probe = progressive_mp4_probe(s);
    const u64 mdat_end = f.mdat_payload + u64{kSampleCount} * f.sample_bytes;
    if (!failure && (probe.state != ProgressiveProbeState::need_data ||
                     probe.need.first != mdat_end / kProgressiveBlockBytes * kProgressiveBlockBytes ||
                     probe.need.first + probe.need.bytes != f.count))
        failure = 303;
    if (!failure && (!fill(s, f, probe.need) ||
                     (probe = progressive_mp4_probe(s)).state != ProgressiveProbeState::ready ||
                     !s->pinned_blocks || progressive_source_trim(s) == 0 ||
                     progressive_source_available(*s, kProgressiveHeadProbeBytes, 1) ||
                     progressive_mp4_probe(s).state != ProgressiveProbeState::ready))
        failure = 304;
    free_source(s);
    release(&f);
    if (failure)
        return failure;
    // Moov at the start is ready from the head alone; a fragmented movie is refused.
    if (!build(&f, 100 * 1024, false))
        return 305;
    s = new_source();
    if (!progressive_source_initialize(s, f.count) || !fill(s, f, {0, kProgressiveHeadProbeBytes}) ||
        progressive_mp4_probe(s).state != ProgressiveProbeState::ready)
        failure = 306;
    free_source(s);
    s = new_source();
    f.bytes[f.mdat_payload - 4] = 'm';
    f.bytes[f.mdat_payload - 3] = 'o';
    f.bytes[f.mdat_payload - 2] = 'o';
    f.bytes[f.mdat_payload - 1] = 'f';
    if (!failure && (!progressive_source_initialize(s, f.count) || !fill(s, f, {0, kProgressiveHeadProbeBytes}) ||
                     progressive_mp4_probe(s).state != ProgressiveProbeState::unsupported))
        failure = 307;
    free_source(s);
    release(&f);
    return failure;
}

int pending_tests() noexcept {
    Fixture f{};
    if (!build(&f, 100 * 1024, true))
        return 401;
    int failure = 0;
    auto* s = new_source();
    Mp4Demuxer complete{}, demux{};
    const auto video = progressive_source_reader(s, ProgressiveTrack::video);
    const auto& reader = s->readers[0];
    const u64 tail = (f.mdat_payload + u64{kSampleCount} * f.sample_bytes) / kProgressiveBlockBytes *
                     kProgressiveBlockBytes;
    if (!progressive_source_initialize(s, f.count) || !fill(s, f, {0, kProgressiveHeadProbeBytes}) ||
        !fill(s, f, {tail, static_cast<u32>(f.count - tail)}) ||
        progressive_mp4_probe(s).state != ProgressiveProbeState::ready ||
        mp4_demux_initialize(&complete, {f.bytes, f.count}) != Mp4DemuxError::none ||
        mp4_demux_initialize(&demux, progressive_source_bytes(*s), video) != Mp4DemuxError::none)
        failure = 402;
    // Samples inside the head read; the first absent one is pending and the
    // cursor stays put, recording exactly where the track is blocked.
    u32 read = 0;
    Mp4AccessUnit unit{};
    while (!failure && (unit = mp4_demux_next_ready(&demux)).ok())
        ++read;
    const u32 at = demux.sample_index;
    if (!failure && (unit.error != Mp4DemuxError::data_pending || read == 0 || read == kSampleCount ||
                     !reader.pending || reader.missing != kProgressiveHeadProbeBytes ||
                     mp4_demux_next_ready(&demux).error != Mp4DemuxError::data_pending || demux.sample_index != at))
        failure = 403;
    // Seek: the IDR payload is pending until its range arrives, then the
    // progressive answer equals the complete-source seek.
    Mp4Demuxer sought = demux, reference = complete;
    Mp4SeekPoint point{}, expected{};
    if (!failure && (mp4_demux_seek_sync(&sought, 5'250'000, &point) != Mp4DemuxError::data_pending ||
                     mp4_demux_seek_sync(&reference, 5'250'000, &expected) != Mp4DemuxError::none ||
                     expected.sample_index != 50 || !reader.pending))
        failure = 404;
    const auto plan = progressive_schedule(*s, {}, false);
    if (!failure && (!plan.submit || plan.request.first > reader.missing ||
                     plan.request.first + plan.request.bytes <= reader.missing || !fill(s, f, plan.request) ||
                     mp4_demux_seek_sync(&sought, 5'250'000, &point) != Mp4DemuxError::none ||
                     point.sample_index != expected.sample_index ||
                     point.presentation_time_us != expected.presentation_time_us))
        failure = 405;
    if (!failure) {
        unit = mp4_demux_next_ready(&sought);
        if (!unit.ok() || unit.sample_index != 50 ||
            unit.bytes[4] != 0x65 || unit.bytes[100] != payload_byte(50, 100))
            failure = 406;
    }
    free_source(s);
    release(&f);
    return failure;
}

int schedule_tests() noexcept {
    auto* s = new_source();
    int failure = 0;
    constexpr u32 total = 64u * 1024u * 1024u;
    static u8 zero[kProgressiveRequestBytes];
    const auto video = progressive_source_reader(s, ProgressiveTrack::video);
    const auto audio = progressive_source_reader(s, ProgressiveTrack::audio);
    if (!progressive_source_initialize(s, total))
        failure = 501;
    // 64 MiB over 64 s: 20 s of read-ahead is 20 MiB.
    progressive_source_set_duration(s, 64'000'000);
    if (!failure && s->ahead_bytes != 20u * 1024u * 1024u)
        failure = 502;
    progressive_source_set_duration(s, 6'400'000'000ull);
    if (!failure && s->ahead_bytes != kProgressiveMinimumAheadBytes)
        failure = 503;
    progressive_source_set_duration(s, 64'000'000);
    if (!failure && (progressive_schedule(*s, {}, true).submit || !progressive_source_write(s, 0, {zero, sizeof(zero)})))
        failure = 504;
    // Read-ahead from an available cursor starts at the first gap.
    video.available(video.context, 1024, 4096);
    auto plan = progressive_schedule(*s, {}, true);
    if (!failure && (!plan.submit || plan.request.first != kProgressiveRequestBytes ||
                     plan.request.bytes != kProgressiveRequestBytes || progressive_schedule(*s, {}, false).submit))
        failure = 505;
    // An in-flight range inside the window is waited on, not duplicated.
    if (!failure && (progressive_schedule(*s, plan.request, true).submit ||
                     progressive_schedule(*s, plan.request, true).cancel_inflight))
        failure = 506;
    // Seek: a refused read far away makes the old range stale and outranks read-ahead.
    video.available(video.context, 40u * 1024u * 1024u + 123, 5000);
    const auto stale = progressive_schedule(*s, plan.request, true);
    plan = progressive_schedule(*s, {}, true);
    if (!failure && (!stale.cancel_inflight || stale.submit || !plan.submit ||
                     plan.request.first != 40u * 1024u * 1024u))
        failure = 507;
    // The audio track's refused read is served first when it is earlier.
    audio.available(audio.context, 30u * 1024u * 1024u, 100);
    plan = progressive_schedule(*s, {}, true);
    if (!failure && (!plan.submit || plan.request.first != 30u * 1024u * 1024u))
        failure = 508;
    free_source(s);
    return failure;
}

int memory_tests() noexcept {
    auto* s = new_source();
    int failure = 0;
    constexpr u32 total = 96u * 1024u * 1024u;
    static u8 zero[kProgressiveRequestBytes];
    const auto video = progressive_source_reader(s, ProgressiveTrack::video);
    const auto audio = progressive_source_reader(s, ProgressiveTrack::audio);
    if (!progressive_source_initialize(s, total))
        failure = 601;
    progressive_source_set_duration(s, 1'000'000); // Saturates the read-ahead at its maximum.
    progressive_source_pin(s, 0, 4096);
    const u32 budget = kProgressiveResidentBudgetBytes / kProgressiveBlockBytes;
    u32 peak = 0, writes = 0;
    // Play straight through: video and audio cursors advance together.
    for (u64 playhead = 0; !failure && playhead < total; playhead += 1024u * 1024u) {
        video.available(video.context, playhead, 1000);
        audio.available(audio.context, playhead + 64 * 1024, 1000);
        for (u32 step = 0; step < 64; ++step) {
            progressive_source_evict(s, kProgressiveResidentBudgetBytes);
            const auto plan = progressive_schedule(*s, {}, true);
            if (!plan.submit)
                break;
            if (!progressive_source_write(s, plan.request.first, {zero, plan.request.bytes})) {
                failure = 602;
                break;
            }
            ++writes;
            peak = maximum(peak, progressive_source_payload_blocks(*s));
        }
        // The read-ahead window is filled up to its target (or the entity end).
        const u64 window_end = minimum<u64>(playhead + s->ahead_bytes, total);
        if (!failure && !progressive_source_available(*s, playhead, window_end - playhead))
            failure = 603;
    }
    if (!failure && (peak > budget || !writes || s->resident_blocks > budget + s->pinned_blocks ||
                     !progressive_source_available(*s, 0, 4096)))
        failure = 604;
    // Far-behind data was released; recent data behind the cursor was kept.
    if (!failure && (progressive_source_available(*s, 8u * 1024u * 1024u, 1) ||
                     !progressive_source_available(*s, total - 2u * 1024u * 1024u, 1024u * 1024u)))
        failure = 605;
    free_source(s);
    return failure;
}

int refresh_tests() noexcept {
    Fixture f{};
    if (!build(&f, 100 * 1024, true))
        return 701;
    auto* fetch = static_cast<ProgressiveVideoFetch*>(
        VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    int failure = 0;
    FakeServer server{&f, {}, 0, 2, false, false};
    u64 id = 0;
    ProgressiveVideoStep step{};
    if (!progressive_video_fetch_begin(fetch, head("/attachments/1/2/old.mp4"), CdnHost::cdn, kNow + 60) ||
        pump(fetch, &server, false, &id, &step) != ProgressiveVideoAccept::progress)
        failure = 702;
    // The second request meets an expired signature: no bytes, a refresh request.
    if (!failure && (pump(fetch, &server, false, &id, &step) != ProgressiveVideoAccept::refresh_required ||
                     progressive_video_fetch_next(fetch, false, kNow).step != ProgressiveVideoStep::refresh_required))
        failure = 703;
    if (!failure && (!progressive_video_fetch_refresh(fetch, head("/attachments/1/2/new.mp4"), CdnHost::media,
                                                      kNow + 3600) ||
                     fetch->host != CdnHost::media ||
                     pump(fetch, &server, false, &id, &step) != ProgressiveVideoAccept::progress ||
                     progressive_video_fetch_next(fetch, false, kNow).step != ProgressiveVideoStep::ready))
        failure = 704;
    // A stale or cancelled response never touches the source.
    if (!failure) {
        progressive_video_fetch_submitted(fetch, ++id, {0, 16});
        progressive_video_fetch_cancelling(fetch);
        const auto answer = serve(&server, {0, 16});
        if (progressive_video_fetch_accept(fetch, id + 1, true, answer.policy, answer.body) !=
                ProgressiveVideoAccept::ignored ||
            progressive_video_fetch_accept(fetch, id, false, {}, {}) != ProgressiveVideoAccept::ignored ||
            fetch->state == ProgressiveVideoFetchState::failed)
            failure = 705;
    }
    // A different entity length after refresh is a different file: fail.
    if (!failure) {
        progressive_video_fetch_submitted(fetch, ++id, {0, 16});
        f.count -= 1;
        const auto answer = serve(&server, {0, 16});
        f.count += 1;
        if (progressive_video_fetch_accept(fetch, id, true, answer.policy, answer.body) !=
                ProgressiveVideoAccept::failed ||
            progressive_video_fetch_next(fetch, true, kNow).step != ProgressiveVideoStep::failed)
            failure = 706;
    }
    // Expiry is detected before submitting, and repeated refreshes are bounded.
    if (!failure && (!progressive_video_fetch_begin(fetch, head("/a"), CdnHost::cdn, kNow) ||
                     progressive_video_fetch_next(fetch, false, kNow).step != ProgressiveVideoStep::refresh_required))
        failure = 707;
    for (u32 i = 0; !failure && i < kProgressiveVideoMaximumRefreshes; ++i)
        if (!progressive_video_fetch_refresh(fetch, head("/a"), CdnHost::cdn, kNow))
            failure = 708;
        else
            progressive_video_fetch_next(fetch, false, kNow);
    if (!failure && fetch->state != ProgressiveVideoFetchState::failed)
        failure = 709;
    // A server that ignores Range hands the whole bounded entity to the caller.
    server = {&f, {}, 0, 0, true, false};
    if (!failure && (!progressive_video_fetch_begin(fetch, head("/b"), CdnHost::cdn, kNow + 60) ||
                     pump(fetch, &server, false, &id, &step) != ProgressiveVideoAccept::whole_entity))
        failure = 710;
    progressive_video_fetch_release(fetch);
    VirtualFree(fetch, 0, MEM_RELEASE);
    release(&f);
    return failure;
}

// Probe, play to the end, seek back and replay with short (unaligned) answers.
int end_to_end_tests() noexcept {
    Fixture f{};
    if (!build(&f, 150 * 1024, true))
        return 801;
    auto* fetch = static_cast<ProgressiveVideoFetch*>(
        VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    int failure = 0;
    FakeServer server{&f, {}, 0, 0, false, true};
    u64 id = 0;
    ProgressiveVideoStep step{};
    if (!progressive_video_fetch_begin(fetch, head("/attachments/1/2/v.mp4"), CdnHost::cdn, kNow + 600))
        failure = 802;
    for (u32 i = 0; !failure && i < 16 && fetch->state == ProgressiveVideoFetchState::probing; ++i)
        if (pump(fetch, &server, false, &id, &step) == ProgressiveVideoAccept::failed)
            failure = 803;
    if (!failure && (fetch->state != ProgressiveVideoFetchState::ready || server.requests > 4))
        failure = 804;
    Mp4Demuxer demux{};
    auto* s = &fetch->source;
    if (!failure && mp4_demux_initialize(&demux, progressive_source_bytes(*s),
                                         progressive_source_reader(s, ProgressiveTrack::video)) != Mp4DemuxError::none)
        failure = 805;
    progressive_source_set_duration(s, demux.duration_us);
    const auto play = [&](u32 first_sample) noexcept {
        u32 sample = first_sample;
        for (u32 guard = 0; guard < 1000 && sample < kSampleCount; ++guard) {
            const auto unit = mp4_demux_next_ready(&demux);
            if (unit.error == Mp4DemuxError::data_pending) {
                if (pump(fetch, &server, true, &id, &step) == ProgressiveVideoAccept::failed)
                    return false;
                continue;
            }
            if (!unit.ok() || unit.sample_index != sample || unit.bytes.count != f.sample_bytes ||
                !bytes_equal(unit.bytes.data, f.bytes + (unit.bytes.data - s->base), unit.bytes.count))
                return false;
            ++sample;
            // Read-ahead also runs while samples are available.
            if (pump(fetch, &server, true, &id, &step) == ProgressiveVideoAccept::failed)
                return false;
        }
        return sample == kSampleCount;
    };
    if (!failure && !play(0))
        failure = 806;
    if (!failure && !progressive_source_complete(*s))
        failure = 807;
    u16 begin[4]{}, end[4]{};
    if (!failure && (progressive_video_fetch_buffered(fetch, demux.duration_us, begin, end, 4) != 1 ||
                     begin[0] != 0 || end[0] != 10000))
        failure = 808;
    // Drop everything but metadata, seek near the end and play on.
    progressive_source_trim(s);
    Mp4SeekPoint point{};
    if (!failure && mp4_demux_seek_sync(&demux, 4'100'000, &point) != Mp4DemuxError::data_pending)
        failure = 809;
    for (u32 i = 0; !failure && i < 8; ++i) {
        const auto sought = mp4_demux_seek_sync(&demux, 4'100'000, &point);
        if (sought == Mp4DemuxError::none)
            break;
        if (sought != Mp4DemuxError::data_pending ||
            pump(fetch, &server, true, &id, &step) == ProgressiveVideoAccept::failed)
            failure = 810;
    }
    if (!failure && (point.sample_index != 40 || !play(40)))
        failure = 811;
    // After the trim only pinned head samples and the replayed tail are buffered.
    const u32 ranges = progressive_video_fetch_buffered(fetch, demux.duration_us, begin, end, 4);
    if (!failure && (ranges != 2 || begin[0] != 0 || begin[1] > 6667 || end[1] != 10000))
        failure = 812;
    progressive_video_fetch_release(fetch);
    VirtualFree(fetch, 0, MEM_RELEASE);
    release(&f);
    return failure;
}

int run() noexcept {
    int (*const tests[])() noexcept{http_tests,     sparse_tests,  probe_tests,     pending_tests,
                                    schedule_tests, memory_tests,  refresh_tests,   end_to_end_tests};
    for (auto test : tests)
        if (const int failure = test()) {
            char text[48];
            u32 at = put_number(text, put_text(text, 0, "progressive_video failure "), failure);
            text[at++] = '\n';
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, at, &written, nullptr);
            return failure;
        }
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_progressive_video_tests_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
