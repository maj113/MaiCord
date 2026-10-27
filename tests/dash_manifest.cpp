#include "maicord/protocol/dash_manifest.hpp"
#include "maicord/core/byte_order.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
constexpr auto fixture = utf8_literal(R"(<?xml version="1.0"?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT42.9S">
 <Period start="PT0.0S"><AdaptationSet>
  <Representation id="v0" mimeType="video/mp4" codecs="avc1.640029" width="1920" height="1080" bandwidth="5800000">
   <SegmentTemplate timescale="1000000" duration="3000000" startNumber="1" initialization="dash/init-$RepresentationID$.m4s" media="dash/part-$RepresentationID$-$Number%05d$.m4s"/>
  </Representation>
  <Representation id="v2" mimeType="video/mp4" codecs="avc1.640029" width="854" height="480" bandwidth="1400000">
   <SegmentTemplate timescale="1000000" duration="3000000" initialization="dash/init-$RepresentationID$.m4s" media="dash/part-$RepresentationID$-$Number%05d$.m4s"/>
  </Representation>
 </AdaptationSet><AdaptationSet lang="en">
  <Representation id="a0" mimeType="audio/mp4" codecs="mp4a.40.2" bandwidth="192000">
   <SegmentTemplate timescale="1000000" duration="3000000" initialization="dash/init-$RepresentationID$.m4s" media="dash/part-$RepresentationID$-$Number$.m4s"/>
  </Representation>
  <Representation id="thumbnails" mimeType="image/jpeg" width="1536" height="864" bandwidth="0"><BaseURL>thumbs.jpg</BaseURL></Representation>
 </AdaptationSet></Period>
</MPD>)");
static u8 changed[16384];
Utf8View replace(Utf8View old_text, Utf8View new_text) noexcept {
    for (u32 i = 0; i + old_text.bytes <= fixture.bytes; ++i) {
        if (!bytes_equal(fixture.data + i, old_text.data, old_text.bytes)) continue;
        const u32 length = fixture.bytes - old_text.bytes + new_text.bytes;
        if (length > sizeof(changed)) return {};
        copy_bytes(changed, fixture.data, i);
        copy_bytes(changed + i, new_text.data, new_text.bytes);
        copy_bytes(changed + i + new_text.bytes, fixture.data + i + old_text.bytes, fixture.bytes - i - old_text.bytes);
        return {reinterpret_cast<const char*>(changed), length};
    }
    return {};
}
void run() noexcept {
    using E = DashManifestError;
    DashManifest m{};
    check(dash_manifest_parse(fixture, 720, &m) == E::none && m.duration_us == 42900000 &&
          m.video.height == 480 && m.video.width == 854 && m.video.segment_count == 15 &&
          m.video.start_number == 1 && m.video.timescale == 1000000 && m.video.segment_duration == 3000000 &&
          utf8_equal(m.video.id, utf8_literal("v2")) && utf8_equal(m.audio.id, utf8_literal("a0")) && m.audio.bandwidth == 192000,
          "bounded DASH selection keeps exact addressing metadata and chooses AVC within the requested height");
    check(m.video.media.data >= fixture.data && m.video.media.data + m.video.media.bytes <= fixture.data + fixture.bytes,
          "DASH template metadata borrows the original immutable document without allocations");
    u8 output[512]{};
    auto size = dash_segment_path(m.video, 0, true, {output, sizeof(output)});
    check(utf8_equal({reinterpret_cast<const char*>(output), size}, utf8_literal("dash/init-v2.m4s")), "DASH initialization resolves representation identity");
    size = dash_segment_path(m.video, 14, false, {output, sizeof(output)});
    check(utf8_equal({reinterpret_cast<const char*>(output), size}, utf8_literal("dash/part-v2-00015.m4s")), "DASH number padding and final segment ordinal");
    size = dash_segment_path(m.audio, 7, false, {output, sizeof(output)});
    check(utf8_equal({reinterpret_cast<const char*>(output), size}, utf8_literal("dash/part-a0-8.m4s")), "DASH unpadded audio sequence");
    output[0] = 42;
    check(!dash_segment_path(m.video, 15, false, {output, sizeof(output)}) &&
          !dash_segment_path(m.video, 0, false, {output, 1}) && output[0] == 42,
          "DASH path boundaries and insufficient capacity preserve caller output");
    auto aliased = m.video;
    check(!dash_segment_path(aliased, 0, false, {reinterpret_cast<u8*>(&aliased), sizeof(aliased)}) &&
          bytes_equal(&aliased, &m.video, sizeof(aliased)), "DASH path output cannot overwrite its own borrowed metadata");
    check(dash_manifest_parse(fixture, 1080, &m) == E::none && m.video.height == 1080 &&
          dash_manifest_parse(fixture, 144, &m) == E::none && m.video.height == 480,
          "DASH selects the requested rendition or the smallest available larger rendition");
    const auto prior = m;
    const auto reject = [&](Utf8View source, E expected) noexcept {
        check(source.bytes && dash_manifest_parse(source, 720, &m) == expected && bytes_equal(&m, &prior, sizeof(m)),
              "unsupported or malformed DASH cannot replace an accepted selection");
    };
    reject(replace(utf8_literal("type=\"static\""), utf8_literal("type=\"dynamic\"")), E::unsupported);
    reject(replace(utf8_literal("<Period start=\"PT0.0S\">"), utf8_literal("<Period start=\"PT1S\">")), E::unsupported);
    reject(replace(utf8_literal("<AdaptationSet>"), utf8_literal("<AdaptationSet><ContentProtection/>")), E::unsupported);
    reject(replace(utf8_literal("<AdaptationSet>"), utf8_literal("<AdaptationSet><SegmentTimeline/>")), E::unsupported);
    reject(replace(utf8_literal("startNumber=\"1\""), utf8_literal("startNumber=\"4294967295\"")), E::capacity);
    reject(replace(utf8_literal("duration=\"3000000\""), utf8_literal("duration=\"1\"")), E::capacity);
    reject(replace(utf8_literal("duration=\"3000000\""), utf8_literal("duration=\"0\"")), E::malformed);
    reject(replace(utf8_literal("timescale=\"1000000\""), utf8_literal("timescale=\"0\"")), E::malformed);
    reject(replace(utf8_literal("startNumber=\"1\""), utf8_literal("presentationTimeOffset=\"1\"")), E::unsupported);
    reject(replace(utf8_literal("id=\"v0\""), utf8_literal("id=\"a0\"")), E::malformed);
    reject(replace(utf8_literal("id=\"v0\""), utf8_literal("id=\"v0\" id=\"v1\"")), E::malformed);
    reject(replace(utf8_literal("</Period>"), utf8_literal("</Period><Period/>")), E::unsupported);
    reject(replace(utf8_literal("</Period>"), utf8_literal("</AdaptationSet>")), E::malformed);
    reject(replace(utf8_literal("<?xml version=\"1.0\"?>"), utf8_literal("<!DOCTYPE MPD SYSTEM 'https://example.test/schema'>")), E::unsupported);
    reject(replace(utf8_literal("PT42.9S"), utf8_literal("PT24H1S")), E::malformed);
    reject(replace(utf8_literal("PT42.9S"), utf8_literal("PT0S")), E::malformed);
    reject(replace(utf8_literal("PT42.9S"), utf8_literal("PT42.1234567S")), E::malformed);
    reject(replace(utf8_literal("PT42.9S"), utf8_literal("PT1S1M")), E::malformed);
    reject(replace(utf8_literal("xmlns=\"urn:mpeg:dash:schema:mpd:2011\""), utf8_literal("xmlns=\"urn:other\"")), E::unsupported);
    reject(replace(utf8_literal("<MPD "), utf8_literal("<x:MPD ")), E::unsupported);
    reject(replace(utf8_literal("dash/init-$RepresentationID$.m4s"), utf8_literal("../init.m4s")), E::unsupported);
    reject(replace(utf8_literal("dash/init-$RepresentationID$.m4s"), utf8_literal("https://example.test/init.m4s")), E::unsupported);
    reject(replace(utf8_literal("$Number%05d$"), utf8_literal("$Number%099d$")), E::unsupported);
    reject(replace(utf8_literal("$Number%05d$"), utf8_literal("$Time$")), E::unsupported);
    reject(replace(utf8_literal("<Representation id=\"v0\""), utf8_literal("junk<Representation id=\"v0\"")), E::malformed);
    reject(replace(utf8_literal("mp4a.40.2"), utf8_literal("mp4a.40.5")), E::unsupported);
    reject(replace(utf8_literal("thumbs.jpg"), utf8_literal("bad\x01text")), E::malformed);
    const auto inherited = replace(utf8_literal("<AdaptationSet lang=\"en\">\n  <Representation id=\"a0\" mimeType=\"audio/mp4\" codecs=\"mp4a.40.2\""),
        utf8_literal("<AdaptationSet mimeType=\"audio/mp4\" codecs=\"mp4a.40.2\"><Representation id=\"a0\""));
    check(dash_manifest_parse(inherited, 720, &m) == E::none && m.audio.bandwidth == 192000,
          "DASH audio MIME and codec inherit from their adaptation set");
    const auto irrelevant = replace(utf8_literal("<BaseURL>thumbs.jpg</BaseURL>"),
        utf8_literal("<SegmentTemplate media=\"thumb-$Time$.jpg\"/>"));
    check(dash_manifest_parse(irrelevant, 720, &m) == E::none && m.audio.bandwidth == 192000,
          "irrelevant image templates do not prevent clear native audio and video selection");
    const auto thumbnails = replace(utf8_literal("</Period>"), utf8_literal(
        "<AdaptationSet mimeType=\"image/avif\" contentType=\"image\"><SegmentTemplate media=\"$RepresentationID$/$Number%05d$.avif\" duration=\"90\"/>"
        "<Representation id=\"tiles\" bandwidth=\"0\"/></AdaptationSet></Period>"));
    check(dash_manifest_parse(thumbnails, 720, &m) == E::none && m.audio.bandwidth == 192000,
          "provider thumbnail adaptation templates do not reject the playable tracks");
    const auto silent = replace(utf8_literal("mimeType=\"audio/mp4\""), utf8_literal("mimeType=\"image/jpeg\""));
    check(dash_manifest_parse(silent, 720, &m) == E::none && !m.audio.id.bytes && m.video.id.bytes,
          "genuinely silent video is allowed without silently dropping an unsupported audio track");
    const auto valid = replace(utf8_literal("PT42.9S"), utf8_literal("PT1M0.000001S"));
    check(dash_manifest_parse(valid, 720, &m) == E::none && m.duration_us == 60000001 && m.video.segment_count == 21,
          "DASH fractional duration uses integer microseconds and rounds only segment coverage upward");
    copy_bytes(changed, fixture.data, fixture.bytes);
    check(dash_manifest_parse({reinterpret_cast<const char*>(changed), fixture.bytes}, 720,
          reinterpret_cast<DashManifest*>(changed)) == E::invalid_input && bytes_equal(changed, fixture.data, fixture.bytes),
          "DASH selection output cannot overwrite the document that owns its views");
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_dash_manifest_entry() noexcept {
    maicord::run(); ExitProcess(maicord::failures ? 1 : 0);
}
