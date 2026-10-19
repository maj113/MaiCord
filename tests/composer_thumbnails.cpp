#include "maicord/app/local_draft_preview.hpp"
#include "maicord/app/local_preview_thumbnail.hpp"
#include "maicord/app/local_video_poster.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/deflate.hpp"
#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/media/png_stream.hpp"
#include "maicord/media/webp_thumbnail.hpp"
#include "maicord/ui/composer_attachment_strip.hpp"
#include "../src/media/png_common.hpp"
#include "media_fixture.hpp"
#include "webp_fixture.hpp"

// Composer attachment previews beyond the first strip work: MP4/MOV posters
// read only the movie header and first samples, large PNG/WebP thumbnails
// stream with bounded memory, and spoilered thumbnails blur. GPU decode of
// the poster itself needs hardware and is covered by live verification.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

// ---- Virtual MP4: a 48 MiB non-faststart movie whose payload is generated --
struct Box {
    u8* bytes;
    u32 count;
    void u8v(u8 v) noexcept { bytes[count++] = v; }
    void u16v(u16 v) noexcept { u8v(static_cast<u8>(v >> 8)); u8v(static_cast<u8>(v)); }
    void u32v(u32 v) noexcept { byte_store_be32(bytes + count, v); count += 4; }
    void zero(u32 n) noexcept { for (u32 i = 0; i < n; ++i) u8v(0); }
    u32 begin(const char* type) noexcept {
        const u32 at = count;
        u32v(0);
        for (u32 i = 0; i < 4; ++i) u8v(static_cast<u8>(type[i]));
        return at;
    }
    void end(u32 at) noexcept { byte_store_be32(bytes + at, count - at); }
};
struct VirtualMovie {
    u8 head[64];
    u8 moov[1024];
    u32 head_bytes, moov_bytes, moov_offset;
    u64 file_bytes;
    u32 reads;
    bool fail;
};
inline constexpr u32 kMoviePayload = 48u * 1024u * 1024u, kFirstSample = 4096;
void build_movie(VirtualMovie* m, const char* codec, bool fragmented) noexcept {
    *m = VirtualMovie{};
    Box head{m->head, 0};
    const u32 ftyp = head.begin("ftyp");
    head.u32v(0x69736f6du);
    head.u32v(0x200u);
    head.u32v(0x61766331u);
    head.end(ftyp);
    const u32 mdat_offset = head.count;
    head.u32v(1);
    head.u8v('m'); head.u8v('d'); head.u8v('a'); head.u8v('t');
    head.u32v(0);
    head.u32v(16u + kMoviePayload);
    m->head_bytes = head.count;
    const u32 samples_offset = mdat_offset + 16;
    m->moov_offset = samples_offset + kMoviePayload;
    Box b{m->moov, 0};
    if (fragmented) {
        const u32 moof = b.begin("moof");
        b.zero(8);
        b.end(moof);
    }
    const u32 moov = b.begin("moov");
    const u32 trak = b.begin("trak");
    const u32 mdia = b.begin("mdia");
    const u32 mdhd = b.begin("mdhd");
    b.u32v(0); b.u32v(0); b.u32v(0); b.u32v(1000); b.u32v(400); b.u16v(0); b.u16v(0);
    b.end(mdhd);
    const u32 hdlr = b.begin("hdlr");
    b.u32v(0); b.u32v(0); b.u32v(0x76696465u);
    b.end(hdlr);
    const u32 minf = b.begin("minf");
    const u32 stbl = b.begin("stbl");
    const u32 stsd = b.begin("stsd");
    b.u32v(0); b.u32v(1);
    const u32 entry = b.begin(codec);
    b.zero(78);
    b.bytes[entry + 8 + 25] = 64; // 64 x 48
    b.bytes[entry + 8 + 27] = 48;
    const u32 avcc = b.begin("avcC");
    b.u8v(1); b.u8v(66); b.u8v(0); b.u8v(30); b.u8v(0xff); b.u8v(0xe1);
    b.u16v(2); b.u8v(0x67); b.u8v(0);
    b.u8v(1); b.u16v(2); b.u8v(0x68); b.u8v(0);
    b.end(avcc);
    b.end(entry);
    b.end(stsd);
    const u32 stts = b.begin("stts");
    b.u32v(0); b.u32v(1); b.u32v(4); b.u32v(100);
    b.end(stts);
    const u32 stsc = b.begin("stsc");
    b.u32v(0); b.u32v(1); b.u32v(1); b.u32v(4); b.u32v(1);
    b.end(stsc);
    const u32 stsz = b.begin("stsz");
    b.u32v(0); b.u32v(0); b.u32v(4);
    // A small IDR, then 1 MiB samples; the rest of the mdat is never indexed.
    b.u32v(kFirstSample); b.u32v(1u << 20); b.u32v(1u << 20); b.u32v(1u << 20);
    b.end(stsz);
    const u32 stco = b.begin("stco");
    b.u32v(0); b.u32v(1); b.u32v(samples_offset);
    b.end(stco);
    const u32 stss = b.begin("stss");
    b.u32v(0); b.u32v(1); b.u32v(1);
    b.end(stss);
    b.end(stbl);
    b.end(minf);
    b.end(mdia);
    b.end(trak);
    b.end(moov);
    m->moov_bytes = b.count;
    m->file_bytes = static_cast<u64>(m->moov_offset) + m->moov_bytes;
}
bool read_movie(void* context, u64 offset, Slice<u8> output) noexcept {
    auto& m = *static_cast<VirtualMovie*>(context);
    ++m.reads;
    if (m.fail || offset + output.count > m.file_bytes)
        return false;
    const u64 samples = m.head_bytes;
    for (u32 i = 0; i < output.count; ++i) {
        const u64 at = offset + i;
        u8 value = 0;
        if (at < m.head_bytes) {
            value = m.head[at];
        } else if (at >= m.moov_offset) {
            value = m.moov[at - m.moov_offset];
        } else if (at - samples < 4) {
            // First sample: one length-prefixed IDR NAL.
            const u8 length[4]{0, 0, static_cast<u8>((kFirstSample - 4) >> 8), static_cast<u8>(kFirstSample - 4)};
            value = length[at - samples];
        } else if (at - samples == 4) {
            value = 0x65;
        } else {
            value = static_cast<u8>(at * 31u + 7u);
        }
        output[i] = value;
    }
    return true;
}
LocalVideoPosterStep prepare_movie(VirtualMovie* movie, u64 budget, LocalVideoPosterReader* reader,
                                   ProgressiveSource* source) noexcept {
    static u8 scratch[kLocalVideoPosterScratchBytes];
    *reader = {read_movie, movie, scratch, budget, 0, 0};
    if (!progressive_source_initialize(source, static_cast<u32>(movie->file_bytes)))
        return LocalVideoPosterStep::read_failed;
    return local_video_poster_prepare(source, reader);
}

void test_video_posters(Check check) noexcept {
    static VirtualMovie movie;
    static ProgressiveSource source;
    LocalVideoPosterReader reader{};
    build_movie(&movie, "avc1", false);
    auto step = prepare_movie(&movie, kLocalVideoPosterReadBudget, &reader, &source);
    const bool first_resident = progressive_source_available(source, movie.head_bytes, kFirstSample);
    check(step == LocalVideoPosterStep::ready && first_resident && reader.bytes_read <= 2u * 1024u * 1024u &&
              movie.file_bytes > 48u * 1024u * 1024u &&
              source.resident_blocks * static_cast<u64>(kProgressiveBlockBytes) <= 2u * 1024u * 1024u,
          "MP4 poster reads the head, the trailing moov and the first H.264 sample, never the 48 MiB payload");
    progressive_source_release(&source);

    build_movie(&movie, "vp09", false);
    step = prepare_movie(&movie, kLocalVideoPosterReadBudget, &reader, &source);
    // The demuxer reports unknown sample entries as malformed; either way the
    // poster fails permanently and the tile keeps its icon.
    check(step == LocalVideoPosterStep::unsupported || step == LocalVideoPosterStep::malformed,
          "VP9 and other codecs fall back to the video file icon");
    progressive_source_release(&source);
    build_movie(&movie, "hvc1", true);
    step = prepare_movie(&movie, kLocalVideoPosterReadBudget, &reader, &source);
    check(step == LocalVideoPosterStep::unsupported, "fragmented movies fall back to the video file icon");
    progressive_source_release(&source);
    build_movie(&movie, "avc1", false);
    step = prepare_movie(&movie, 512u * 1024u, &reader, &source);
    check(step == LocalVideoPosterStep::budget && reader.bytes_read <= 512u * 1024u,
          "a poster never reads past its byte budget");
    progressive_source_release(&source);
    movie.fail = true;
    step = prepare_movie(&movie, kLocalVideoPosterReadBudget, &reader, &source);
    check(step == LocalVideoPosterStep::read_failed, "a cancelled or failed read stops the poster");
    progressive_source_release(&source);
    movie.fail = false;
    movie.moov_bytes = 20; // truncated movie header
    movie.file_bytes = static_cast<u64>(movie.moov_offset) + movie.moov_bytes;
    step = prepare_movie(&movie, kLocalVideoPosterReadBudget, &reader, &source);
    check(step == LocalVideoPosterStep::malformed, "a truncated movie header is malformed, not decoded");
    progressive_source_release(&source);

    // Keys, limits and GPU reduction for posters.
    const auto poster = media_asset_local_draft_preview(5, 3u | kLocalDraftPreviewThumbnailVariant,
                                                        MediaAttachmentFormat::mp4);
    AccountMediaLocalPreviewRequest request{};
    const wchar_t path[] = L"C:\\clip.mp4";
    request.snapshot = {{1, 2, 3, 0}, 200ull * 1024u * 1024u};
    request.path = {path, 11};
    request.key = poster;
    request.owner_scope = 9;
    request.owner_generation = 3u | kLocalDraftPreviewThumbnailVariant;
    const bool large_ok = local_draft_preview_request_valid(request);
    request.snapshot.bytes = 2ull * 1024u * 1024u * 1024u;
    const bool huge_rejected = !local_draft_preview_request_valid(request);
    check(media_asset_key_valid(poster) &&
              !media_asset_key_valid(media_asset_local_draft_preview(5, 3, MediaAttachmentFormat::mp4)) &&
              large_ok && huge_rejected,
          "only composer thumbnails admit MP4 posters, up to the sparse-source limit, without reading the file");
    check(gpu_video_frame_reduction(1920, 1080, kGpuVideoPosterExtent) == 5 &&
              gpu_video_frame_reduced(1920, 5) == 384 && gpu_video_frame_reduced(1080, 5) == 216 &&
              gpu_video_frame_reduction(3840, 2160, kGpuVideoPosterExtent) == 10 &&
              gpu_video_frame_reduction(320, 240, kGpuVideoPosterExtent) == 1 &&
              gpu_video_frame_reduction(4096, 64, 100) == kGpuVideoReductionMaximum &&
              kGpuVideoPosterExtent == kLocalPreviewThumbnailExtent,
          "posters reduce on the GPU to the 400 px thumbnail extent with a bounded box step");
}

// ---- Fixed-Huffman deflate writer with long-distance matches ----------------
struct Bits {
    u8* out;
    u32 capacity, count, bits;
    u64 accumulator;
    void put(u32 value, u32 length) noexcept {
        accumulator |= static_cast<u64>(value) << bits;
        bits += length;
        while (bits >= 8 && count < capacity) {
            out[count++] = static_cast<u8>(accumulator);
            accumulator >>= 8;
            bits -= 8;
        }
    }
    void huffman(u32 code, u32 length) noexcept {
        u32 reversed = 0;
        for (u32 i = 0; i < length; ++i)
            reversed |= ((code >> i) & 1u) << (length - 1u - i);
        put(reversed, length);
    }
    void symbol(u32 value) noexcept {
        if (value <= 143) huffman(0x30 + value, 8);
        else if (value <= 255) huffman(0x190 + value - 144, 9);
        else if (value <= 279) huffman(value - 256, 7);
        else huffman(0xC0 + value - 280, 8);
    }
    void flush() noexcept {
        if (bits)
            put(0, 8 - bits);
    }
};
constexpr u16 kLengthBase[29]{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                              131, 163, 195, 227, 258};
constexpr u8 kLengthExtra[29]{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 kDistanceBase[30]{1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 kDistanceExtra[30]{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
                                13, 13};
u32 build_matches_stream(u8* expected, u32 total, u8* zlib, u32 capacity) noexcept {
    Bits w{zlib + 2, capacity - 6, 0, 0, 0};
    zlib[0] = 0x78;
    zlib[1] = 0x01;
    w.put(1, 1); // final
    w.put(1, 2); // fixed Huffman
    u32 seed = 12345, pos = 0;
    const auto random = [&]() noexcept { return (seed = seed * 1103515245u + 12345u) >> 16; };
    while (pos < 40000) {
        expected[pos] = static_cast<u8>(random());
        w.symbol(expected[pos++]);
    }
    constexpr u32 distances[]{1, 3, 300, 4096, 32768, 20000, 31999};
    constexpr u32 lengths[]{3, 17, 258, 130, 258, 66};
    for (u32 n = 0; pos + 300 < total; ++n) {
        const u32 distance = distances[n % 7], length = lengths[n % 6];
        for (u32 i = 0; i < length; ++i, ++pos)
            expected[pos] = expected[pos - distance];
        u32 code = 0;
        while (code + 1 < 29 && kLengthBase[code + 1] <= length)
            ++code;
        w.symbol(257 + code);
        w.put(length - kLengthBase[code], kLengthExtra[code]);
        u32 dcode = 0;
        while (dcode + 1 < 30 && kDistanceBase[dcode + 1] <= distance)
            ++dcode;
        w.huffman(dcode, 5);
        w.put(distance - kDistanceBase[dcode], kDistanceExtra[dcode]);
        if (n % 5 == 0)
            for (u32 i = 0; i < 5; ++i) {
                expected[pos] = static_cast<u8>(random());
                w.symbol(expected[pos++]);
            }
    }
    w.symbol(256);
    w.flush();
    const u32 checksum = png_fixture_adler32({expected, pos});
    byte_store_be32(zlib + 2 + w.count, checksum);
    return 2 + w.count + 4;
}
struct Collect {
    u8* data;
    u32 count, capacity, calls;
    bool refuse;
};
bool collect(void* context, ConstSlice<u8> bytes) noexcept {
    auto& c = *static_cast<Collect*>(context);
    ++c.calls;
    if (c.refuse || bytes.count > c.capacity - c.count)
        return false;
    copy_bytes(c.data + c.count, bytes.data, bytes.count);
    c.count += bytes.count;
    return true;
}

void test_windowed_inflate(Check check) noexcept {
    static u8 expected[400u * 1024u], zlib[400u * 1024u], direct[400u * 1024u], streamed[400u * 1024u];
    static u8 window[kZlibWindowedMinimumBytes];
    const u32 zlib_bytes = build_matches_stream(expected, 380u * 1024u, zlib, sizeof(zlib));
    u32 total = 0;
    DeflateWorkspace workspace{};
    const DeflateInputSpan span{zlib, zlib_bytes};
    const auto reference = zlib_inflate({&span, 1}, {direct, sizeof(direct)}, ~0u, &workspace);
    total = reference.output_bytes;
    Collect sink{streamed, 0, sizeof(streamed), 0, false};
    const auto windowed = zlib_inflate_windowed({&span, 1}, {window, sizeof(window)}, ~0u, collect, &sink, &workspace);
    bool same = reference.ok() && windowed.ok() && total > 2 * sizeof(window) && sink.count == total &&
                windowed.output_bytes == total && sink.calls > 2;
    for (u32 i = 0; same && i < total; ++i)
        same = streamed[i] == direct[i] && direct[i] == expected[i];
    check(same, "windowed inflate streams 380 KiB with 32 KiB back-references through a 128 KiB window");
    zlib[zlib_bytes - 1] ^= 1u;
    sink = {streamed, 0, sizeof(streamed), 0, false};
    const auto corrupt = zlib_inflate_windowed({&span, 1}, {window, sizeof(window)}, ~0u, collect, &sink, &workspace);
    zlib[zlib_bytes - 1] ^= 1u;
    sink = {streamed, 0, sizeof(streamed), 0, true};
    const auto refused = zlib_inflate_windowed({&span, 1}, {window, sizeof(window)}, ~0u, collect, &sink, &workspace);
    const auto small = zlib_inflate_windowed({&span, 1}, {window, sizeof(window) / 2}, ~0u, collect, &sink, &workspace);
    check(corrupt.error == ZlibInflateError::checksum && refused.error == ZlibInflateError::stream_failed &&
              small.error == ZlibInflateError::invalid_input,
          "windowed inflate verifies Adler-32, honours a refusing sink and requires the full window");
}

// ---- PNG streaming --------------------------------------------------------
struct ThumbRows {
    const PngImage* image;
    LocalThumbnailSink* sink;
    u8* rgba;
};
bool thumb_row(void* context, u32, const u8* row) noexcept {
    auto& r = *static_cast<ThumbRows*>(context);
    return local_thumbnail_png_row(*r.image, row, r.rgba) && local_thumbnail_push_row(r.sink, r.rgba);
}
u32 rgba_at(const u8* rgba, u32 width, u32 x, u32 y) noexcept {
    const u8* p = rgba + (static_cast<usize>(y) * width + x) * 4u;
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
           static_cast<u32>(p[3]) << 24;
}
// Rewrites the IDAT data into pieces of at most piece bytes.
u32 rechunk_idat(const u8* png, u32 bytes, u32 piece, u8* out, u32 capacity) noexcept {
    u32 used = 8, cursor = 8;
    copy_bytes(out, png, 8);
    static u8 idat[128u * 1024u];
    u32 idat_bytes = 0;
    bool written = false;
    while (cursor + 12 <= bytes) {
        const u32 length = static_cast<u32>(png[cursor]) << 24 | static_cast<u32>(png[cursor + 1]) << 16 |
                           static_cast<u32>(png[cursor + 2]) << 8 | png[cursor + 3];
        const u8* type = png + cursor + 4;
        const bool is_idat = type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T';
        if (is_idat) {
            copy_bytes(idat + idat_bytes, type + 4, length);
            idat_bytes += length;
        } else {
            if (idat_bytes && !written) {
                for (u32 at = 0; at < idat_bytes; at += piece) {
                    const u32 n = minimum(piece, idat_bytes - at);
                    if (used + 12 + n > capacity)
                        return 0;
                    byte_store_be32(out + used, n);
                    copy_bytes(out + used + 4, "IDAT", 4);
                    copy_bytes(out + used + 8, idat + at, n);
                    byte_store_be32(out + used + 8 + n, png_crc32(out + used + 4, n + 4));
                    used += 12 + n;
                }
                written = true;
            }
            if (used + 12 + length > capacity)
                return 0;
            copy_bytes(out + used, png + cursor, 12 + length);
            used += 12 + length;
        }
        cursor += 12 + length;
    }
    return used;
}

void test_png_streaming(Check check) noexcept {
    static u8 png[96u * 1024u], zlib[96u * 1024u], chunked[160u * 1024u];
    static u8 output[kLocalPreviewThumbnailExtent * kLocalPreviewThumbnailExtent * 4u];
    static u32 sums[(kLocalPreviewThumbnailExtent + 1) * 5];
    static u8 row[2600u * 4u], window[kPngStreamWindowBytes], rows[2u * (2600u * 4u + 1u)];
    static DeflateInputSpan spans[256];
    PngFixtureResult fixture{};
    const bool built = build_rgba8_png_fixture(2600, 6, {png, sizeof(png)}, {zlib, sizeof(zlib)}, &fixture,
                                               [](u32 x, u32, u32, u32) noexcept -> u32 {
                                                   return x < 1300 ? 0xFF0000FFu : 0xFFFF0000u;
                                               });
    PngImage legacy{};
    const bool legacy_rejects = built && png_parse({png, fixture.encoded_bytes}, &legacy).error == PngError::dimensions;
    const auto decode = [&](ConstSlice<u8> encoded, LocalThumbnailSink* sink) noexcept {
        PngImage image{};
        PngStreamLayout layout{};
        DeflateWorkspace workspace{};
        if (!png_stream_parse(encoded, &image, &layout).ok() || layout.idat_chunks > 256 ||
            !local_thumbnail_begin(sink, image.width, image.height, kLocalPreviewThumbnailExtent,
                                   {output, sizeof(output)}, {sums, sizeof(sums) / 4}))
            return false;
        ThumbRows context{&image, sink, row};
        return png_stream_decode(encoded, image, layout, {spans, 256}, {window, sizeof(window)},
                                 {rows, layout.row_buffer_bytes}, thumb_row, &context, &workspace)
                   .ok() &&
               local_thumbnail_complete(*sink);
    };
    LocalThumbnailSink sink{};
    const bool wide = built && decode({png, fixture.encoded_bytes}, &sink);
    check(legacy_rejects && wide && sink.width == 372 && sink.height == 1 &&
              rgba_at(output, sink.width, 10, 0) == 0xFF0000FFu && rgba_at(output, sink.width, 360, 0) == 0xFFFF0000u,
          "PNGs wider than 2048 px stream into a 400 px thumbnail through two scanlines");
    const u32 chunked_bytes = built ? rechunk_idat(png, fixture.encoded_bytes, 512, chunked, sizeof(chunked)) : 0;
    const bool many_rejected = chunked_bytes && !png_parse({chunked, chunked_bytes}, &legacy).ok();
    LocalThumbnailSink chunked_sink{};
    const bool many = chunked_bytes && decode({chunked, chunked_bytes}, &chunked_sink);
    check(many_rejected && many && chunked_sink.width == 372 && rgba_at(output, 372, 10, 0) == 0xFF0000FFu,
          "PNGs with more than 64 IDAT chunks decode for thumbnails");
    // A corrupted IDAT fails the CRC; a filter byte outside 0..4 fails the row.
    chunked[chunked_bytes - 20] ^= 0x40u;
    PngImage image{};
    PngStreamLayout layout{};
    const bool crc = png_stream_parse({chunked, chunked_bytes}, &image, &layout).error == PngError::chunk_crc;
    static u8 bad_filter[96u * 1024u];
    bool filter_rejected = false;
    if (built) {
        copy_bytes(bad_filter, png, fixture.encoded_bytes);
        // The fixture stores one IDAT whose deflate data is a stored block:
        // zlib header (2) + block header (5), then the first filter byte.
        u32 at = 8;
        while (at + 12 <= fixture.encoded_bytes && !(bad_filter[at + 4] == 'I' && bad_filter[at + 7] == 'T'))
            at += 12 + (static_cast<u32>(bad_filter[at]) << 24 | static_cast<u32>(bad_filter[at + 1]) << 16 |
                        static_cast<u32>(bad_filter[at + 2]) << 8 | bad_filter[at + 3]);
        const u32 length = static_cast<u32>(bad_filter[at + 2]) << 8 | bad_filter[at + 3];
        bad_filter[at + 8 + 7] = 9;
        byte_store_be32(bad_filter + at + 8 + length - 4, png_fixture_adler32({bad_filter + at + 8 + 7, length - 11}));
        byte_store_be32(bad_filter + at + 8 + length, png_crc32(bad_filter + at + 4, length + 4));
        LocalThumbnailSink bad_sink{};
        filter_rejected = !decode({bad_filter, fixture.encoded_bytes}, &bad_sink);
    }
    check(crc && filter_rejected, "corrupt PNG chunks and invalid scanline filters keep the file icon");
    static u8 apng[16u * 1024u], apng_zlib[16u * 1024u];
    ApngFixtureResult animated{};
    LocalThumbnailSink apng_sink{};
    const bool apng_ok = build_rgba8_apng_default_fixture({apng, sizeof(apng)}, {apng_zlib, sizeof(apng_zlib)}, &animated) &&
                         decode({apng, animated.encoded_bytes}, &apng_sink);
    check(apng_ok, "APNG thumbnails decode the default image");
}

// ---- WebP CPU path ---------------------------------------------------------
struct WebpCollect {
    u64 hash;
    u32 rows, width;
    u32 first, later; // first pixel of row 0 and row 8
    u8 alpha[64];
    LocalThumbnailSink* sink;
};
bool webp_row(void* context, u32 y, const u8* rgba) noexcept {
    auto& c = *static_cast<WebpCollect*>(context);
    c.hash = hash64_bytes(c.hash, rgba, c.width * 4u);
    if (y == 0)
        c.first = rgba_at(rgba, c.width, 0, 0);
    if (y == 8)
        c.later = rgba_at(rgba, c.width, c.width - 1, 0);
    if (y < 1)
        for (u32 x = 0; x < 64 && x < c.width; ++x)
            c.alpha[x] = rgba[x * 4 + 3];
    ++c.rows;
    return !c.sink || local_thumbnail_push_row(c.sink, rgba);
}
// A keyframe whose partitions are all zero bytes decodes deterministically:
// every macroblock is B_PRED with DC subblocks, chroma DC and no residual.
u32 build_zero_webp(u8* out, u32 width, u32 height, bool alpha, u8 alpha_filter, const u8* alpha_plane) noexcept {
    constexpr u32 first_partition = 48u * 1024u, tokens = 64u * 1024u;
    const u32 vp8_bytes = 10 + first_partition + tokens;
    u32 at = 12;
    copy_bytes(out, "RIFF", 4);
    copy_bytes(out + 8, "WEBP", 4);
    if (alpha) {
        copy_bytes(out + at, "VP8X", 4);
        byte_store_le32(out + at + 4, 10);
        clear_bytes(out + at + 8, 10);
        out[at + 8] = 0x10;
        const u32 w = width - 1, h = height - 1;
        out[at + 12] = static_cast<u8>(w); out[at + 13] = static_cast<u8>(w >> 8); out[at + 14] = static_cast<u8>(w >> 16);
        out[at + 15] = static_cast<u8>(h); out[at + 16] = static_cast<u8>(h >> 8); out[at + 17] = static_cast<u8>(h >> 16);
        at += 18;
        const u32 alpha_bytes = 1 + width * height;
        copy_bytes(out + at, "ALPH", 4);
        byte_store_le32(out + at + 4, alpha_bytes);
        out[at + 8] = static_cast<u8>(alpha_filter << 2);
        copy_bytes(out + at + 9, alpha_plane, width * height);
        at += 8 + alpha_bytes + (alpha_bytes & 1u);
    }
    copy_bytes(out + at, "VP8 ", 4);
    byte_store_le32(out + at + 4, vp8_bytes);
    u8* frame = out + at + 8;
    const u32 tag = 0u | (0u << 1) | (1u << 4) | (first_partition << 5);
    frame[0] = static_cast<u8>(tag);
    frame[1] = static_cast<u8>(tag >> 8);
    frame[2] = static_cast<u8>(tag >> 16);
    frame[3] = 0x9D; frame[4] = 0x01; frame[5] = 0x2A;
    frame[6] = static_cast<u8>(width); frame[7] = static_cast<u8>(width >> 8);
    frame[8] = static_cast<u8>(height); frame[9] = static_cast<u8>(height >> 8);
    clear_bytes(frame + 10, first_partition + tokens);
    at += 8 + vp8_bytes;
    byte_store_le32(out + 4, at - 8);
    return at;
}

void test_webp_thumbnails(Check check) noexcept {
    static u8 workspace_storage[8u * 1024u * 1024u];
    u8* workspace = reinterpret_cast<u8*>((reinterpret_cast<usize>(workspace_storage) + 15u) & ~static_cast<usize>(15));
    const u32 workspace_capacity = sizeof(workspace_storage) - 16;
    // Exact against libwebp: reconstruction, loop filter and fancy upsampling.
    WebpThumbnailLayout layout{};
    WebpCollect collected{1'469'598'103'934'665'603ull, 0, 32, 0, 0, {}, nullptr};
    const bool parsed = webp_thumbnail_parse({kWebpLossy32x24, sizeof(kWebpLossy32x24)}, &layout) ==
                        WebpThumbnailError::none;
    const bool decoded = parsed && layout.workspace_bytes <= workspace_capacity &&
                         webp_thumbnail_decode(layout, {workspace, workspace_capacity}, webp_row, &collected) ==
                             WebpThumbnailError::none;
    check(decoded && layout.width == 32 && layout.height == 24 && collected.rows == 24 &&
              collected.hash == kWebpLossy32x24RgbaFnv1a,
          "the CPU WebP path matches libwebp's RGBA for a multi-macroblock lossy frame");

    // A 1104 x 600 frame (beyond the 512 px GPU path) streams row by row.
    static u8 large[256u * 1024u];
    const u32 large_bytes = build_zero_webp(large, 1104, 600, false, 0, nullptr);
    static u8 output[kLocalPreviewThumbnailExtent * kLocalPreviewThumbnailExtent * 4u];
    static u32 sums[(kLocalPreviewThumbnailExtent + 1) * 5];
    LocalThumbnailSink sink{};
    collected = {1'469'598'103'934'665'603ull, 0, 1104, 0, 0, {}, &sink};
    const bool large_parsed = webp_thumbnail_parse({large, large_bytes}, &layout) == WebpThumbnailError::none &&
                              layout.width == 1104 && layout.height == 600 &&
                              layout.workspace_bytes < 2u * 1024u * 1024u;
    const bool large_decoded =
        large_parsed &&
        local_thumbnail_begin(&sink, 1104, 600, kLocalPreviewThumbnailExtent, {output, sizeof(output)},
                              {sums, sizeof(sums) / 4}) &&
        webp_thumbnail_decode(layout, {workspace, workspace_capacity}, webp_row, &collected) == WebpThumbnailError::none;
    // DC prediction from the 127/129 borders gives luma 128 in the first
    // subblock row and 129 below it: RGB 130 and 132 after conversion.
    check(large_decoded && collected.rows == 600 && collected.first == 0xFF828282u &&
              collected.later == 0xFF848484u && local_thumbnail_complete(sink) && sink.width == 368 &&
              sink.height == 200 && rgba_at(output, 368, 200, 150) == 0xFF848484u,
          "lossy WebPs wider than 512 px decode on the CPU with a few rows of memory");

    // Uncompressed ALPH with the horizontal filter is reconstructed per row.
    static u8 alpha_plane[48u * 32u];
    for (u32 y = 0; y < 32; ++y)
        for (u32 x = 0; x < 48; ++x) {
            // Filtered deltas for alpha(x, y) = x * 5: row 0 predicts from the
            // left, later rows' first pixel from above.
            alpha_plane[y * 48 + x] = static_cast<u8>(x == 0 ? 0 : 5);
        }
    const u32 alpha_bytes = build_zero_webp(large, 48, 32, true, 1, alpha_plane);
    collected = {1'469'598'103'934'665'603ull, 0, 48, 0, 0, {}, nullptr};
    bool alpha_ok = webp_thumbnail_parse({large, alpha_bytes}, &layout) == WebpThumbnailError::none &&
                    layout.alpha.data && layout.alpha_filter == 1 &&
                    webp_thumbnail_decode(layout, {workspace, workspace_capacity}, webp_row, &collected) ==
                        WebpThumbnailError::none;
    for (u32 x = 0; alpha_ok && x < 48; ++x)
        alpha_ok = collected.alpha[x] == static_cast<u8>(x * 5);
    check(alpha_ok && collected.rows == 32, "uncompressed WebP alpha planes are unfiltered row by row");

    // Unsupported variants keep the file icon.
    static u8 lossless[64];
    copy_bytes(lossless, "RIFF\x1c\x00\x00\x00WEBPVP8L\x10\x00\x00\x00", 20);
    clear_bytes(lossless + 20, 16);
    const auto lossless_error = webp_thumbnail_parse({lossless, 36}, &layout);
    const u32 compressed = build_zero_webp(large, 48, 32, true, 0, alpha_plane);
    large[12 + 18 + 8] = 1; // ALPH compression 1: VP8L-coded alpha
    const auto alpha_error = webp_thumbnail_parse({large, compressed}, &layout);
    const bool alpha_chunk = layout.alpha_chunk.data == large + 12 + 18 + 8 && !layout.alpha.data;
    const auto truncated = webp_thumbnail_parse({large, 16}, &layout);
    check(lossless_error == WebpThumbnailError::unsupported_lossless && alpha_error == WebpThumbnailError::none &&
              alpha_chunk && truncated == WebpThumbnailError::container,
          "lossless and truncated WebPs are refused; a compressed alpha plane is left to the caller");
}

void test_spoiler_blur(Check check) noexcept {
    UiComposerAttachmentTile tile{};
    tile.id = 1;
    tile.filename = utf8_literal("cat.png");
    tile.kind = UiAttachmentFileKind::image;
    tile.phase = UiComposerAttachmentPhase::ready;
    tile.preview = media_asset_local_draft_preview(1, 1u | kLocalDraftPreviewThumbnailVariant, MediaAttachmentFormat::png);
    tile.preview_ready = true;
    tile.spoiler = true;
    UiComposerAttachmentStripView view{};
    view.tiles = {&tile, 1};
    view.strip = {0, 0, 600, ui_composer_attachment_strip_height()};
    view.hovered = view.selected = kUiComposerAttachmentNoTile;
    UiRect media{}, clip{};
    const bool visible = ui_composer_attachment_thumbnail_visible(view, 0, &media, &clip);
    const UiRect fitted = ui_composer_attachment_fit(media, 400, 300);
    const float blur = ui_composer_attachment_thumbnail_blur(tile, {0, 0, 184, 184});
    const float landscape = ui_composer_attachment_thumbnail_blur(tile, fitted);
    const float small = ui_composer_attachment_thumbnail_blur(tile, {0, 0, 40, 20});
    UiComposerAttachmentTag tags[kUiComposerAttachmentTags];
    char duration[16];
    const u32 tag_count = ui_composer_attachment_tile_tags(tile, media, {tags, kUiComposerAttachmentTags}, {duration, 16});
    const bool spoiler_tag = tag_count && utf8_equal(tags[tag_count - 1].text, utf8_literal("SPOILER"));
    tile.spoiler = false;
    check(visible && blur == kUiComposerAttachmentSpoilerBlur && small == 5.f &&
              landscape > 34.4f && landscape < 34.6f && spoiler_tag &&
              ui_composer_attachment_thumbnail_blur(tile, fitted) == 0.f,
          "spoilered thumbnails stay visible with a 44 px blur (capped for small images) "
          "and the SPOILER tag");
    tile.kind = UiAttachmentFileKind::video;
    tile.duration_ms = 65'000;
    tile.spoiler = true;
    const u32 video_tags = ui_composer_attachment_tile_tags(tile, media, {tags, kUiComposerAttachmentTags}, {duration, 16});
    check(video_tags == 2 && utf8_equal(tags[0].text, utf8_literal("1:05")) &&
              utf8_equal(tags[1].text, utf8_literal("SPOILER")),
          "spoilered video posters keep their duration and SPOILER tags");
}
} // namespace

void test_composer_thumbnails(void (*check)(bool, const char*) noexcept) noexcept {
    test_video_posters(check);
    test_windowed_inflate(check);
    test_png_streaming(check);
    test_webp_thumbnails(check);
    test_spoiler_blur(check);
}

} // namespace maicord
