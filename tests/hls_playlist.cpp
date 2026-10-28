#include "maicord/protocol/hls_playlist.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "test_output.hpp"
namespace maicord {
namespace {
u32 failures;
void check(bool ok, const char* label) noexcept { test_check(&failures, ok, label); }
constexpr auto header = utf8_literal("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:10\n#EXT-X-MAP:URI=\"init.mp4\"\n");
constexpr auto tail = utf8_literal("#EXTINF:2.5,\n0.mp4\n#EXTINF:1.333333,title\n1.mp4\n#EXTINF:10,\nfolder/2-muted.mp4\n#EXT-X-ENDLIST\n");
constexpr auto reference = utf8_literal("#EXTM3U\r\n#EXT-X-VERSION:7\r\n#EXT-X-TARGETDURATION:10\r\n#EXT-X-MEDIA-SEQUENCE:42\r\n"
    "#EXT-X-PLAYLIST-TYPE:VOD\r\n#EXT-X-MAP:URI=\"init.mp4\"\r\n#EXT-X-KEY:METHOD=NONE\r\n#ignored comment\r\n"
    "#EXTINF:2.5,\r\n0.mp4\r\n#EXTINF:1.333333,title\r\n1.mp4\r\n#EXTINF:10,\r\nfolder/2-muted.mp4\r\n#EXT-X-ENDLIST");
struct Text {
    char data[4096]{}; u32 size{};
    void add(Utf8View s) noexcept { if (s.bytes <= sizeof(data) - size) { copy_bytes(data + size, s.data, s.bytes); size += s.bytes; } }
    Utf8View view() const noexcept { return {data, size}; }
};
void run() noexcept {
    using E = HlsPlaylistError; HlsMediaPlaylist playlist{};
    check(hls_media_playlist(reference, &playlist) == E::none && playlist.segments == 3 && playlist.sequence == 42 &&
        playlist.target_seconds == 10 && playlist.duration_us == 13833333 && playlist.maximum_duration_us == 10000000 &&
        utf8_equal(playlist.initialization, utf8_literal("init.mp4")) && playlist.input.data == reference.data,
        "complete HLS playlist retains exact decimal durations and borrowed initialization without allocating an index");
    HlsMediaSegment s{};
    check(hls_media_segment(playlist, 0, &s) && s.index == 0 && !s.time_us && s.duration_us == 2500000 &&
        utf8_equal(s.uri, utf8_literal("0.mp4")), "HLS first-file lookup returns its exact extent");
    check(hls_media_follow(playlist, s, &s) && s.index == 1 && s.time_us == 2500000 && s.duration_us == 1333333 &&
        utf8_equal(s.uri, utf8_literal("1.mp4")), "HLS sequential lookup resumes after the previous URI and supports in-place advancement");
    check(hls_media_follow(playlist, s, &s) && s.index == 2 && s.time_us == 3833333 && s.duration_us == 10000000,
        "HLS variable-duration files do not use target duration as an exact clock");
    const auto end = s;
    check(!hls_media_follow(playlist, s, &s) && !hls_media_segment(playlist, 3, &s) &&
        !hls_media_find(playlist, playlist.duration_us, &s) && bytes_equal(&s, &end, sizeof(s)), "HLS EOF and invalid seeks preserve caller output");
    check(hls_media_find(playlist, 2499999, &s) && s.index == 0 &&
        hls_media_find(playlist, 2500000, &s) && s.index == 1 &&
        hls_media_find(playlist, 3833333, &s) && s.index == 2 &&
        hls_media_segment(playlist, 2, &s) && utf8_equal(s.uri, utf8_literal("folder/2-muted.mp4")),
        "HLS timestamp lookup uses exact half-open boundaries and allows provider-muted filenames");
    const auto saved = playlist;
    const auto reject = [&](Utf8View middle, E expected, bool append_tail = true) noexcept {
        Text text; text.add(header); text.add(middle); if (append_tail) text.add(tail);
        check(hls_media_playlist(text.view(), &playlist) == expected && bytes_equal(&playlist, &saved, sizeof(saved)),
              "unsupported or malformed HLS cannot replace the accepted playlist");
    };
    constexpr Utf8View unsupported[]{
        utf8_literal("#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n"), utf8_literal("#EXT-X-KEY\n"),
        utf8_literal("#EXT-X-DISCONTINUITY\n"), utf8_literal("#EXT-X-BYTERANGE:123@0\n"),
        utf8_literal("#EXT-X-GAP\n"), utf8_literal("#EXT-X-I-FRAMES-ONLY\n"),
        utf8_literal("#EXT-X-PART:DURATION=1,URI=\"part.mp4\"\n"),
        utf8_literal("#EXT-X-MAP:URI=\"other.mp4\"\n"), utf8_literal("#EXT-X-PLAYLIST-TYPE:UNKNOWN\n")};
    for (auto text : unsupported) reject(text, E::unsupported);
    constexpr Utf8View malformed[]{utf8_literal("#EXT-X-VERSION:7\n"), utf8_literal("#EXT-X-TARGETDURATION:10\n"),
        utf8_literal("#EXT-X-MEDIA-SEQUENCE:18446744073709551615\n"), utf8_literal("#EXT-X-STREAM-INF:BANDWIDTH=1000\n"),
        utf8_literal("#EXTINF:1,\n#EXTINF:2,\n"), utf8_literal("#EXTINF:0,\n"), utf8_literal("#EXTINF:1.0000001,\n"),
        utf8_literal("#comment\rbreak\n"), utf8_literal("#comment\tbreak\n"), utf8_literal("#comment\xc0\x80\n")};
    for (auto text : malformed) reject(text, E::malformed);
    reject(utf8_literal("#EXTINF:10.5,\n0.mp4\n#EXT-X-ENDLIST\n"), E::malformed, false);
    reject(utf8_literal("#EXTINF:1,\n0.mp4\n"), E::unsupported, false);
    reject(utf8_literal("#EXT-X-PLAYLIST-TYPE:EVENT\n#EXTINF:1,\n0.mp4\n"), E::unsupported, false);
    Text event; event.add(header); event.add(utf8_literal("#EXT-X-PLAYLIST-TYPE:EVENT\n")); event.add(tail);
    HlsMediaPlaylist completed{};
    check(hls_media_playlist(event.view(), &completed) == E::none && completed.duration_us == saved.duration_us,
        "an ended event playlist has the same complete finite timeline as VOD");
    reject(utf8_literal("#EXTINF:1,\n0.mp4\n#EXT-X-ENDLIST\n#EXTINF:1,\n1.mp4\n"), E::malformed, false);
    constexpr Utf8View paths[]{utf8_literal("../file.mp4"), utf8_literal("/file.mp4"), utf8_literal("dir/../file.mp4"),
        utf8_literal("dir//file.mp4"), utf8_literal("dir/./file.mp4"), utf8_literal("%2e%2e/file.mp4"),
        utf8_literal("https://example.test/file.mp4"), utf8_literal("file.mp4?token=value"), utf8_literal("file\\x.mp4")};
    for (auto path : paths) {
        Text text; text.add(utf8_literal("#EXTINF:1,\n")); text.add(path); text.add(utf8_literal("\n#EXT-X-ENDLIST\n"));
        reject(text.view(), E::unsupported, false);
    }
    Text aliased; aliased.add(header); aliased.add(tail);
    const auto original = aliased;
    check(hls_media_playlist(aliased.view(), reinterpret_cast<HlsMediaPlaylist*>(aliased.data)) == E::invalid_input &&
        bytes_equal(&aliased, &original, sizeof(aliased)), "HLS output cannot overwrite its borrowed input");
    static char large[256 * 1024 + 1]{};
    check(hls_media_playlist({large, sizeof(large)}, &playlist) == E::capacity, "HLS rejects oversized playlists before scanning");
    copy_bytes(large, header.data, header.bytes); u32 used = header.bytes;
    constexpr auto short_segment = utf8_literal("#EXTINF:0.001,\na.mp4\n");
    for (u32 i = 0; i < 8193; ++i) { copy_bytes(large + used, short_segment.data, short_segment.bytes); used += short_segment.bytes; }
    check(hls_media_playlist({large, used}, &playlist) == E::capacity, "HLS segment count stays bounded without allocating a URI table");
}
void master() noexcept {
    using E = HlsPlaylistError;
    constexpr auto audio = utf8_literal("#EXT-X-STREAM-INF:BANDWIDTH=202277,CODECS=\"mp4a.40.2\",VIDEO=\"audio_only\"\naudio/index.m3u8\n");
    Text text; text.add(utf8_literal("#EXTM3U\n")); text.add(audio);
    text.add(utf8_literal("#EXT-X-STREAM-INF:BANDWIDTH=3258400,CODECS=\"avc1.4D4020,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=60.000\n720/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1352874,CODECS=\"avc1.4D401F,mp4a.40.2\",RESOLUTION=852x480\n480/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=655073,CODECS=\"avc1.4D401E,mp4a.40.2\",RESOLUTION=640x360\n360/index.m3u8\n"));
    HlsMasterPlaylist result{};
    check(hls_master_playlist(text.view(), 480, &result) == E::none && result.video.height == 480 && result.video.width == 852 &&
        utf8_equal(result.video.uri, utf8_literal("480/index.m3u8")) && result.audio.bandwidth == 202277,
        "HLS master selects bounded AVC video and independent AAC from quoted codec lists");
    check(hls_master_playlist(text.view(), 200, &result) == E::none && result.video.height == 360,
        "HLS master falls back to the smallest supported video above the requested height");
    check(hls_master_playlist(text.view(), 720, &result, 1400000) == E::none && result.video.height == 480 &&
        result.audio.bandwidth == 202277, "HLS bandwidth budget selects the best fitting video without replacing the audio rendition");
    const auto saved = result;
    const auto reject = [&](Utf8View line, E expected) noexcept {
        Text bad; bad.add(utf8_literal("#EXTM3U\n")); bad.add(audio); bad.add(line);
        check(hls_master_playlist(bad.view(), 480, &result) == expected && bytes_equal(&result, &saved, sizeof(saved)),
            "invalid HLS master preserves the previously selected pair");
    };
    reject(utf8_literal("#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1.4D401F\",RESOLUTION=852x480\n"), E::malformed);
    reject(utf8_literal("#EXT-X-STREAM-INF:BANDWIDTH=1,BANDWIDTH=2,CODECS=\"avc1.4D401F\"\nx\n"), E::malformed);
    reject(utf8_literal("#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=avc1.4D401F\nx\n"), E::malformed);
    reject(utf8_literal("#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1.4D401F\",RESOLUTION=852x480,\nx\n"), E::malformed);
    reject(utf8_literal("#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"hvc1.1.6.L120\",RESOLUTION=852x480\nx\n"), E::unsupported);
    reject(utf8_literal("#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1.4D401F\",RESOLUTION=852x480\n../x\n"), E::malformed);
    reject(utf8_literal("#EXT-X-SESSION-KEY:METHOD=AES-128,URI=\"key\"\n"), E::unsupported);
    reject(utf8_literal("#EXTINF:1,\nx\n"), E::malformed);
    check(hls_master_playlist(text.view(), 0, &result) == E::invalid_input, "HLS master rejects an invalid quality cap");
    constexpr auto grouped_video=utf8_literal("#EXT-X-STREAM-INF:BANDWIDTH=3000000,CODECS=\"avc1.4D4020,mp4a.40.2\",RESOLUTION=1280x720,AUDIO=\"aac\"\nvideo.m3u8\n");
    constexpr auto groups=utf8_literal("#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"he-aac\",NAME=\"Other\",DEFAULT=YES,URI=\"wrong.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Alternative\",AUTOSELECT=YES,URI=\"alternate.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Original\",DEFAULT=YES,AUTOSELECT=YES,URI=\"original.m3u8\"\n");
    for(u32 order=0;order<2;++order) {
        Text grouped;grouped.add(utf8_literal("#EXTM3U\n"));grouped.add(audio);
        grouped.add(order?grouped_video:groups);grouped.add(order?groups:grouped_video);
        check(hls_master_playlist(grouped.view(),720,&result)==E::none && result.video.height==720 &&
            utf8_equal(result.audio.uri,utf8_literal("original.m3u8")),
            "selected video binds its default AUDIO group regardless of order or unrelated audio-only streams");
    }
    Text missing;missing.add(utf8_literal("#EXTM3U\n"));missing.add(audio);missing.add(grouped_video);
    const auto group_saved=result;
    check(hls_master_playlist(missing.view(),720,&result)==E::unsupported && bytes_equal(&result,&group_saved,sizeof(result)),
        "missing referenced audio group cannot silently use an unrelated audio-only stream");
    missing.add(groups);
    missing.add(utf8_literal("#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Duplicate default\",DEFAULT=YES,URI=\"duplicate.m3u8\"\n"));
    check(hls_master_playlist(missing.view(),720,&result)==E::malformed && bytes_equal(&result,&group_saved,sizeof(result)),
        "ambiguous default audio cannot replace the accepted selection");
    Text no_uri;no_uri.add(utf8_literal("#EXTM3U\n"));no_uri.add(grouped_video);
    no_uri.add(utf8_literal("#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Muxed\",DEFAULT=YES\n"));
    check(hls_master_playlist(no_uri.view(),720,&result)==E::unsupported,
        "an in-band audio group is not mistaken for an independent audio source");
}
void sliding() noexcept {
    constexpr auto ts=utf8_literal("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:100\n#EXTINF:2,\nhttps://x.hls.ttvnw.net/v1/segment/a.ts\n");
    constexpr auto mapped=utf8_literal("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:100\n#EXT-X-MAP:URI=\"https://x.hls.ttvnw.net/v1/segment/init.mp4\"\n#EXTINF:2,\nhttps://x.hls.ttvnw.net/v1/segment/a.mp4\n");
    HlsMediaPlaylist transport{},fragmented{}; HlsPlaylistAdvance format{};
    check(hls_media_playlist(ts,&transport,HlsPlaylistMode::sliding)==HlsPlaylistError::none && transport.transport &&
        !transport.initialization.bytes && hls_media_playlist(mapped,&fragmented,HlsPlaylistMode::sliding)==HlsPlaylistError::none &&
        !fragmented.transport && hls_media_update(transport,fragmented,&format)==HlsPlaylistError::unsupported &&
        hls_media_playlist(ts,&transport,HlsPlaylistMode::complete)==HlsPlaylistError::unsupported,
        "live no-MAP transport is explicit and cannot change format during refresh or enter the complete MP4 path");
    using E = HlsPlaylistError; constexpr auto mode = HlsPlaylistMode::sliding;
    constexpr auto a = utf8_literal("#EXTINF:2,\nhttps://media.example.test/a.mp4?cap=a%2Fb\n");
    constexpr auto b = utf8_literal("#EXTINF:0.5,\nhttps://media.example.test/b.mp4\n");
    constexpr auto c = utf8_literal("#EXTINF:3,\nhttps://media.example.test/c.mp4\n");
    constexpr auto d = utf8_literal("#EXTINF:2.25,\nhttps://media.example.test/d.mp4\n");
    const auto snapshot = [](Utf8View sequence, Utf8View files, bool ended = false,
        Utf8View initialization = utf8_literal("https://media.example.test/init.mp4"), Utf8View discontinuity = utf8_literal("7")) noexcept {
        Text text; text.add(utf8_literal("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:3\n#EXT-X-MEDIA-SEQUENCE:"));
        text.add(sequence); text.add(utf8_literal("\n#EXT-X-DISCONTINUITY-SEQUENCE:")); text.add(discontinuity);
        text.add(utf8_literal("\n#EXT-X-MAP:URI=\"")); text.add(initialization); text.add(utf8_literal("\"\n"));
        text.add(files); if (ended) text.add(utf8_literal("#EXT-X-ENDLIST\n")); return text;
    };
    Text abc; abc.add(a); abc.add(b); abc.add(c);
    Text bcd; bcd.add(b); bcd.add(c); bcd.add(d);
    auto old_text = snapshot(utf8_literal("100"), abc.view());
    auto next_text = snapshot(utf8_literal("101"), bcd.view());
    HlsMediaPlaylist old{}, next{};
    check(hls_media_playlist(old_text.view(), &old, mode) == E::none && !old.ended && old.sequence == 100 &&
        old.duration_us == 5500000 && old.mode == mode && old.discontinuity_sequence == 7 &&
        hls_media_playlist(next_text.view(), &next, mode) == E::none,
        "sliding HLS retains bounded unended snapshots with provider-admitted HTTPS media URLs");
    HlsPlaylistAdvance advance{};
    check(hls_media_update(old, next, &advance) == E::none && advance.changed && advance.discarded_us == 2000000 &&
        advance.new_segments == 1, "sliding refresh preserves overlapping identities and advances the window by exact discarded durations");
    check(hls_media_update(next, next, &advance) == E::none && !advance.changed && !advance.discarded_us && !advance.new_segments,
        "unchanged live snapshots do not reset the playback origin or invent new files");
    Text abcd = abc; abcd.add(d);
    auto appended_text = snapshot(utf8_literal("100"), abcd.view()); HlsMediaPlaylist appended{};
    check(hls_media_playlist(appended_text.view(), &appended, mode) == E::none &&
        hls_media_update(old, appended, &advance) == E::none && advance.changed && !advance.discarded_us && advance.new_segments == 1,
        "an accumulating event can append media without shifting its start");
    auto adjacent_text = snapshot(utf8_literal("103"), d); HlsMediaPlaylist adjacent{};
    check(hls_media_playlist(adjacent_text.view(), &adjacent, mode) == E::none &&
        hls_media_update(old, adjacent, &advance) == E::none && advance.discarded_us == old.duration_us && advance.new_segments == 1,
        "adjacent snapshots retain a known continuous clock even without a shared file");
    auto ended_text = snapshot(utf8_literal("101"), bcd.view(), true); HlsMediaPlaylist ended{};
    check(hls_media_playlist(ended_text.view(), &ended, mode) == E::none && ended.ended &&
        hls_media_update(next, ended, &advance) == E::none && advance.changed && !advance.new_segments && !advance.discarded_us,
        "ENDLIST closes the same live window without altering its media timing");
    const auto saved = advance;
    check(hls_media_update(ended, next, &advance) == E::stale && bytes_equal(&advance, &saved, sizeof(saved)) &&
        hls_media_update(next, old, &advance) == E::stale, "stale responses cannot reopen an ended stream or roll its sequence backwards");
    const auto reject = [&](const Text& text, E expected) noexcept {
        HlsMediaPlaylist candidate{}; HlsPlaylistAdvance result{123, 456, true}; const auto before = result;
        check(hls_media_playlist(text.view(), &candidate, mode) == E::none && hls_media_update(old, candidate, &result) == expected &&
            bytes_equal(&result, &before, sizeof(before)), "rejected sliding refreshes preserve caller state transactionally");
    };
    reject(snapshot(utf8_literal("100"), a), E::stale);
    reject(snapshot(utf8_literal("104"), d), E::gap);
    Text changed_duration; changed_duration.add(utf8_literal("#EXTINF:1,\nhttps://media.example.test/b.mp4\n")); changed_duration.add(c); changed_duration.add(d);
    reject(snapshot(utf8_literal("101"), changed_duration.view()), E::malformed);
    Text changed_uri; changed_uri.add(utf8_literal("#EXTINF:0.5,\nhttps://media.example.test/replaced.mp4\n")); changed_uri.add(c); changed_uri.add(d);
    reject(snapshot(utf8_literal("101"), changed_uri.view()), E::malformed);
    reject(snapshot(utf8_literal("101"), bcd.view(), false, utf8_literal("https://media.example.test/other-init.mp4")), E::unsupported);
    reject(snapshot(utf8_literal("101"), bcd.view(), false, utf8_literal("https://media.example.test/init.mp4"), utf8_literal("8")), E::unsupported);
    Text ab; ab.add(a); ab.add(b);
    auto edge_text = snapshot(utf8_literal("18446744073709551614"), ab.view());
    auto last_text = snapshot(utf8_literal("18446744073709551615"), b, true);
    HlsMediaPlaylist edge{}, last{};
    check(hls_media_playlist(edge_text.view(), &edge, mode) == E::none && hls_media_playlist(last_text.view(), &last, mode) == E::none &&
        hls_media_update(edge, last, &advance) == E::none && advance.discarded_us == 2000000 && !advance.new_segments,
        "sliding sequence comparisons do not overflow at the final uint64 identity");
    Text too_many; for (u32 i = 0; i < 129; ++i) too_many.add(utf8_literal("#EXTINF:1,\na.mp4\n"));
    const auto large = snapshot(utf8_literal("0"), too_many.view());
    check(hls_media_playlist(large.view(), &next, mode) == E::capacity, "live snapshot segment count has a smaller fixed bound than VOD");
    auto incomplete_vod = snapshot(utf8_literal("0"), a); incomplete_vod.add(utf8_literal("#EXT-X-PLAYLIST-TYPE:VOD\n"));
    check(hls_media_playlist(incomplete_vod.view(), &next, mode) == E::unsupported,
        "sliding mode cannot disguise an incomplete declared VOD as live media");
    check(hls_media_playlist(old_text.view(), &next) == E::unsupported,
        "existing complete-playlist callers still reject unended or absolute-URI streams");
}
void relative_uris() noexcept {
    using E = HlsPlaylistError;
    constexpr auto base = utf8_literal("https://vod-adaptive-ak.vimeocdn.com/exp=2000000000~acl=%2Fasset%2F*/asset/v2/playlist/av/primary/prot/key/media.m3u8?old=1");
    char output[2048];
    struct Case { Utf8View input, expected; };
    constexpr Case cases[]{
        {utf8_literal("../../../../../range/prot/key/movie.mp4?pathsig=abc&range=0-802"),
         utf8_literal("https://vod-adaptive-ak.vimeocdn.com/exp=2000000000~acl=%2Fasset%2F*/asset/v2/range/prot/key/movie.mp4?pathsig=abc&range=0-802")},
        {utf8_literal("./audio.m3u8?x=%2F%3A"),utf8_literal("https://vod-adaptive-ak.vimeocdn.com/exp=2000000000~acl=%2Fasset%2F*/asset/v2/playlist/av/primary/prot/key/audio.m3u8?x=%2F%3A")},
        {utf8_literal("?new=2"),utf8_literal("https://vod-adaptive-ak.vimeocdn.com/exp=2000000000~acl=%2Fasset%2F*/asset/v2/playlist/av/primary/prot/key/media.m3u8?new=2")},
        {utf8_literal("/root/file.mp4"),utf8_literal("https://vod-adaptive-ak.vimeocdn.com/root/file.mp4")},
        {utf8_literal("a/../b/./part.mp4"),utf8_literal("https://vod-adaptive-ak.vimeocdn.com/exp=2000000000~acl=%2Fasset%2F*/asset/v2/playlist/av/primary/prot/key/b/part.mp4")},
        {utf8_literal("https://other.example/part.mp4?x=1"),utf8_literal("https://other.example/part.mp4?x=1")}};
    for (const auto& c : cases) {
        const auto bytes = hls_resolve_uri(base, c.input, {output,sizeof(output)});
        check(bytes && utf8_equal({output,bytes},c.expected),"HLS resolves bounded signed relative references without inheriting the old query");
        output[0]='!';
        check(!hls_resolve_uri(base,c.input,{output,bytes-1}) && output[0]=='!',"HLS URI capacity failure leaves output untouched");
    }
    constexpr Utf8View rejected[]{utf8_literal("//evil.example/a"),utf8_literal("http://example.com/a"),
        utf8_literal("https://user@example.com/a"),utf8_literal("https://example.com:443/a"),
        utf8_literal("a#fragment"),utf8_literal("a%0d%0aHeader=x"),utf8_literal("%2e%2e/part.mp4"),
        utf8_literal("a%2f..%2fpart.mp4"),utf8_literal("part.mp4?x=%"),utf8_literal("part.mp4?x=%5c"),
        utf8_literal("a//b.mp4"),utf8_literal("../../../../../../../../../../part.mp4")};
    for (const auto uri : rejected) {
        output[0]='!';
        check(!hls_resolve_uri(base,uri,{output,sizeof(output)}) && output[0]=='!',"HLS rejects ambiguous authority, escaping paths and malformed references transactionally");
    }
    constexpr auto media=utf8_literal("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:6\n#EXT-X-MAP:URI=\"../../range/init.mp4?range=0-802\"\n#EXTINF:6.066667,\n../../range/file.mp4?range=1207-1718699\n#EXT-X-ENDLIST\n");
    HlsMediaPlaylist playlist{};HlsMediaSegment segment{};
    check(hls_media_playlist(media,&playlist,HlsPlaylistMode::complete_uri)==E::none && playlist.ended &&
          !playlist.transport && hls_media_segment(playlist,0,&segment) && segment.duration_us==6066667 &&
          hls_resolve_uri(base,segment.uri,{output,sizeof(output)}),"HLS complete URI mode retains finite fragmented playlists and borrowed signed references");
    check(hls_media_playlist(media,&playlist)==E::unsupported,"existing local VOD admission remains strict");
    constexpr auto master=utf8_literal("#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Main\",DEFAULT=YES,URI=\"../audio/media.m3u8?key=2\"\n#EXT-X-STREAM-INF:BANDWIDTH=3312127,CODECS=\"avc1.640020,mp4a.40.2\",RESOLUTION=1280x720,AUDIO=\"aac\"\nvideo/media.m3u8?key=1\n");
    HlsMasterPlaylist selected{};
    check(hls_master_playlist(master,720,&selected,~u32{0},true)==E::none && selected.video.height==720 &&
          selected.audio.uri.bytes && hls_master_playlist(master,720,&selected)==E::malformed,
          "relative master mode selects independent audio and video while default admission stays unchanged");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_hls_playlist_entry() noexcept {
    maicord::run(); maicord::master(); maicord::sliding(); maicord::relative_uris(); ExitProcess(maicord::failures ? 1 : 0);
}
