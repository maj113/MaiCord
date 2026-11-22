#include "maicord/protocol/media/embed_player.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/protocol/media/cdn_http.hpp"
#include "maicord/core/ascii_search.hpp"
#include "maicord/core/log_event.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {
u32 failures, scratch[8192];
u8 body[8192];
EmbedStreams result;
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
constexpr auto id = utf8_literal("aB_01234-xy");
constexpr auto video360 = utf8_literal(R"({"url":"https://rr1---sn-test.googlevideo.com/videoplayback?id=360","mimeType":"video/mp4; codecs=\"avc1.4d401e\"","contentLength":"1000000","bitrate":100000,"width":640,"height":360})");
constexpr auto video720 = utf8_literal(R"({"url":"https://rr1---sn-test.googlevideo.com/videoplayback?id=720","mimeType":"video/mp4; codecs=\"avc1.4d401f\"","contentLength":"2000000","bitrate":200000,"width":1280,"height":720})");
constexpr auto audio = utf8_literal(R"({"url":"https://rr1---sn-test.googlevideo.com/videoplayback?id=audio\u0026range=0-99","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","contentLength":"100000","bitrate":128000,"audioChannels":2,"audioTrack":{"audioIsDefault":true}})");
constexpr auto alternate = utf8_literal(R"({"url":"https://rr1---sn-test.googlevideo.com/videoplayback?id=alternate","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","contentLength":"100000","bitrate":160000,"audioChannels":2,"audioTrack":{"audioIsDefault":false}})");
Utf8View response(Utf8View a, Utf8View b = {}, Utf8View c = {}, Utf8View d = {}) noexcept {
    ProtocolWriter w{{body, sizeof(body)}, 0};
    protocol_writer_append_literal(&w, "{\"playabilityStatus\":{\"status\":\"OK\",\"playableInEmbed\":true},\"videoDetails\":{\"videoId\":\"aB_01234-xy\"},\"streamingData\":{\"expiresInSeconds\":\"21600\",\"adaptiveFormats\":[");
    bool first = true;
    const Utf8View entries[]{a, b, c, d};
    for (auto entry : entries) {
        if (!entry.bytes) continue;
        if (!first) protocol_writer_append_byte(&w, ',');
        protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(entry.data), entry.bytes);
        first = false;
    }
    protocol_writer_append_literal(&w, "]}}");
    return {reinterpret_cast<const char*>(body), w.used};
}
EmbedPlayerError decode(Utf8View source, u16 height = 480, Utf8View expected = id) noexcept {
    return youtube_player_streams(source, expected, height, {scratch, 8192}, &result);
}
void rejected(Utf8View source, EmbedPlayerError expected) noexcept {
    EmbedStreams before = result;
    check(decode(source) == expected && bytes_equal(&before, &result, sizeof(result)),
          "player errors are typed and leave output untouched");
}
// The lists below are what YouTube answered MaiCord's own player request in November 2026: every number is real.
// Only the URLs are fixtures. Each names its itag and its xtags value and nothing else.
struct Listed {
    u32 itag;
    Utf8View mime;
    u32 width, height, fps, bitrate, average;
    u64 length;
    // Further members, each after a comma.
    Utf8View more;
    // The percent-encoded xtags value of the URL: the audio track kind, or the dynamic range compression.
    Utf8View xtags;
};
#define SOUND(quality, rate) ",\"audioQuality\":\"AUDIO_QUALITY_" quality "\",\"audioSampleRate\":\"" rate "\",\"audioChannels\":2"
#define TRACK(name, id, chosen) ",\"audioTrack\":{\"displayName\":\"" name "\",\"id\":\"" id "\",\"audioIsDefault\":" chosen "}"
constexpr auto kH264High = utf8_literal("video/mp4; codecs=\"avc1.64002A\""), kH264 = utf8_literal("video/mp4; codecs=\"avc1.4D401F\"");
constexpr auto kVp9 = utf8_literal("video/webm; codecs=\"vp9\""), kVp9Hdr = utf8_literal("video/webm; codecs=\"vp9.2\"");
constexpr auto kAv1 = utf8_literal("video/mp4; codecs=\"av01.0.13M.10.0.110.09.16.09.0\"");
constexpr auto kAac = utf8_literal("audio/mp4; codecs=\"mp4a.40.2\""), kHeAac = utf8_literal("audio/mp4; codecs=\"mp4a.40.5\"");
constexpr auto kOpus = utf8_literal("audio/webm; codecs=\"opus\"");
constexpr auto kMuxed = utf8_literal("video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"");
constexpr auto kHdr = utf8_literal(",\"qualityLabel\":\"2160p60 HDR\"");
constexpr auto kDrc = utf8_literal("drc%3D1");
u8 list_body[32768];
EmbedOffer offer;
Utf8View listed(ConstSlice<Listed> adaptive, ConstSlice<Listed> combined = {}) noexcept {
    ProtocolWriter w{{list_body, sizeof(list_body)}, 0};
    bool ok = protocol_write(&w, "{\"playabilityStatus\":{\"status\":\"OK\",\"playableInEmbed\":true},\"videoDetails\":"
                                 "{\"videoId\":\"aB_01234-xy\"},\"streamingData\":{\"expiresInSeconds\":\"21600\"");
    const ConstSlice<Listed> lists[]{combined, adaptive};
    for (u32 l = 0; l < 2; ++l) {
        ok = ok && protocol_write(&w, l ? ",\"adaptiveFormats\":[" : ",\"formats\":[");
        for (u32 i = 0; i < lists[l].count; ++i) {
            const auto& f = lists[l].data[i];
            ok = ok && protocol_write(&w, i ? "," : "", "{\"itag\":", f.itag,
                                      ",\"url\":\"https://rr1---sn-test.googlevideo.com/videoplayback?itag=", f.itag,
                                      f.xtags.bytes ? "\\u0026xtags=" : "", f.xtags, "\",\"mimeType\":", json_text(f.mime),
                                      ",\"bitrate\":", f.bitrate, ",\"averageBitrate\":", f.average,
                                      ",\"contentLength\":\"", f.length, "\"");
            if (f.width)
                ok = ok && protocol_write(&w, ",\"width\":", f.width, ",\"height\":", f.height, ",\"fps\":", f.fps);
            ok = ok && protocol_write(&w, f.more, "}");
        }
        ok = ok && protocol_write(&w, "]");
    }
    check(ok && protocol_write(&w, "}}"), "the fixture list fits its buffer");
    return {reinterpret_cast<const char*>(list_body), w.used};
}
template <usize N> Utf8View listed(const Listed (&adaptive)[N]) noexcept { return listed({adaptive, N}); }
EmbedPlayerError pick(Utf8View source, u16 cap = 1080) noexcept {
    reset_value(&result);
    return youtube_player_streams(source, id, cap, {scratch, 8192}, &result, &offer);
}
// The itag of the picked video format, or of the picked audio format. 0 without one.
u32 picked(bool audio) noexcept {
    for (u32 i = 0; i < offer.count; ++i)
        if ((offer.formats[i].flags & embed_format_picked) && !(offer.formats[i].flags & embed_format_audio) != audio)
            return offer.formats[i].itag;
    return 0;
}
void line_is(const LogEvent& line, Utf8View expected, const char* label) noexcept {
    const bool same = utf8_equal({line.text, line.used}, expected);
    check(same, label);
    if (!same) {
        test_write_text(line.text);
        test_write_text("\n");
    }
}
void format_is(u32 index, Utf8View expected, const char* label) noexcept {
    LogEvent line("playback_format");
    embed_offer_format(offer.formats[index], &line);
    line_is(line, expected, label);
}
void summary_is(Utf8View expected, const char* label, bool music = false) noexcept {
    LogEvent line("playback_quality");
    embed_offer_summary(offer, music, &line);
    line_is(line, expected, label);
}

// A 2160p60 HDR video: H.264 stops at 1080p60, and 60 fps formats have no 30 fps twin.
constexpr Listed kSixty[]{
    {315, kVp9, 3840, 2160, 60, 26568143, 24040546, 942927341},
    {337, kVp9Hdr, 3840, 2160, 60, 30892627, 28894277, 1133302195, kHdr},
    {701, kAv1, 3840, 2160, 60, 26160357, 24632098, 966132483, kHdr},
    {308, kVp9, 2560, 1440, 60, 13591742, 9371701, 367580380},
    {299, kH264High, 1920, 1080, 60, 5889664, 4230996, 165950262},
    {303, kVp9, 1920, 1080, 60, 7193179, 3081815, 120876127},
    {298, kH264, 1280, 720, 60, 3504829, 2402272, 94223120},
    {135, kH264, 854, 480, 30, 1158359, 560670, 21990915},
    {134, kH264, 640, 360, 30, 633164, 320457, 12569154},
    {133, kH264, 426, 240, 30, 245611, 147800, 5797091},
    {160, kH264, 256, 144, 30, 111319, 67281, 2638953},
    {139, kHeAac, 0, 0, 0, 50092, 48817, 1915378, utf8_literal(SOUND("LOW", "22050"))},
    {139, kHeAac, 0, 0, 0, 50094, 48816, 1915365, utf8_literal(SOUND("LOW", "22050") ",\"isDrc\":true"), kDrc},
    {140, kAac, 0, 0, 0, 130599, 129492, 5079986, utf8_literal(SOUND("MEDIUM", "44100") ",\"isDrc\":true"), kDrc},
    {140, kAac, 0, 0, 0, 130607, 129492, 5079985, utf8_literal(SOUND("MEDIUM", "44100"))},
    {249, kOpus, 0, 0, 0, 57383, 50384, 1976319, utf8_literal(SOUND("LOW", "48000"))},
    {251, kOpus, 0, 0, 0, 148635, 132567, 5199989, utf8_literal(SOUND("MEDIUM", "48000"))},
    {251, kOpus, 0, 0, 0, 149829, 132700, 5205204, utf8_literal(SOUND("MEDIUM", "48000") ",\"isDrc\":true"), kDrc},
};
// A Short: every format is portrait.
constexpr Listed kPortrait[]{
    {299, kH264High, 1080, 1920, 60, 4271443, 3649284, 6895324}, {298, kH264, 720, 1280, 60, 2702629, 2280508, 4309020},
    {135, kH264, 480, 854, 30, 514796, 509444, 963678},          {134, kH264, 360, 640, 30, 368754, 307673, 582002},
    {133, kH264, 240, 426, 30, 185245, 150120, 283971},          {160, kH264, 144, 256, 30, 89421, 74409, 140755},
    {140, kAac, 0, 0, 0, 130467, 130001, 247150, utf8_literal(SOUND("MEDIUM", "44100"))},
};
// Two videos of 2007 and 2012: YouTube lists the 240p picture twice, as itag 133 and as itag 134.
constexpr Listed kTwinHigh134[]{
    {133, kH264, 320, 240, 25, 186211, 138916, 1695477}, {134, kH264, 320, 240, 25, 212209, 139811, 1706396},
    {160, kH264, 192, 144, 25, 86266, 64469, 786846},
    {140, kAac, 0, 0, 0, 130359, 129558, 1585020, utf8_literal(SOUND("MEDIUM", "44100"))},
};
constexpr Listed kTwinHigh133[]{
    {134, kH264, 320, 240, 15, 150749, 136218, 322379}, {133, kH264, 320, 240, 15, 183848, 182995, 433081},
    {160, kH264, 192, 144, 15, 83327, 82513, 195278},
    {140, kAac, 0, 0, 0, 130171, 129796, 309288, utf8_literal(SOUND("MEDIUM", "44100"))},
};
// A video of 3 hours: the 1080p file is above 4 GiB and the 720p file is above the client's 1 GiB limit.
constexpr Listed kLong[]{
    {137, kH264High, 1920, 1080, 30, 4626232, 3165985, 4407853177}, {136, kH264, 1280, 720, 30, 2078032, 837562, 1166099483},
    {135, kH264, 854, 480, 30, 1040511, 450294, 626923473},         {134, kH264, 640, 360, 30, 664172, 289241, 402697653},
    {133, kH264, 426, 240, 30, 262590, 142002, 197703103},          {160, kH264, 256, 144, 30, 127399, 77231, 107525439},
    {140, kAac, 0, 0, 0, 140790, 129471, 180258545, utf8_literal(SOUND("MEDIUM", "44100"))},
};
// A video of 24 hours: its only AAC file is above the limit, so the client can play no pair.
constexpr Listed kDay[]{
    {137, kH264High, 1920, 1080, 30, 2316680, 2002579, 22184246117}, {136, kH264, 1280, 720, 30, 747469, 453482, 5023602793},
    {135, kH264, 854, 480, 30, 527581, 241388, 2674064182},          {134, kH264, 640, 360, 30, 428420, 147425, 1633150943},
    {133, kH264, 426, 240, 30, 357612, 77815, 862032148},            {160, kH264, 256, 144, 30, 327862, 49142, 544386885},
    {140, kAac, 0, 0, 0, 215706, 129471, 1434260515, utf8_literal(SOUND("MEDIUM", "44100"))},
};
// A Spanish video with dubbed tracks. The dubbed tracks come first and have the higher bitrates.
constexpr Listed kDubbed[]{
    {137, kH264High, 1920, 1080, 24, 4344177, 3045707, 586757911},
    {140, kAac, 0, 0, 0, 131880, 129474, 28650616, utf8_literal(SOUND("MEDIUM", "44100") TRACK("English", "en.3", "false")),
     utf8_literal("acont%3Ddubbed%3Alang%3Den")},
    {140, kAac, 0, 0, 0, 131963, 129474, 28650616, utf8_literal(SOUND("MEDIUM", "44100") TRACK("Russian", "ru.3", "false")),
     utf8_literal("acont%3Ddubbed%3Alang%3Dru")},
    {140, kAac, 0, 0, 0, 131990, 129480, 28650700,
     utf8_literal(SOUND("MEDIUM", "44100") TRACK("English (US)", "en-US.10", "false")),
     utf8_literal("acont%3Ddubbed-auto%3Alang%3Den-US")},
    {140, kAac, 0, 0, 0, 131858, 129476, 28650544,
     utf8_literal(SOUND("MEDIUM", "44100") TRACK("Spanish original", "es.4", "true")),
     utf8_literal("acont%3Doriginal%3Alang%3Des")},
    {251, kOpus, 0, 0, 0, 144266, 125304, 24140597,
     utf8_literal(SOUND("MEDIUM", "48000") TRACK("Spanish original", "es.4", "true")),
     utf8_literal("acont%3Doriginal%3Alang%3Des")},
};
// A YouTube Music song: a square picture of the cover.
constexpr Listed kSong[]{
    {137, kH264High, 1080, 1080, 25, 181356, 94672, 2441611}, {136, kH264, 720, 720, 25, 77166, 41488, 1069981},
    {140, kAac, 0, 0, 0, 131059, 129584, 3341990, utf8_literal(SOUND("MEDIUM", "44100"))},
    {251, kOpus, 0, 0, 0, 136404, 126723, 3268541, utf8_literal(SOUND("MEDIUM", "48000"))},
};
// Not in today's lists. YouTube listed a 30 fps and a 60 fps format of one size in the past.
constexpr Listed kTwoRates[]{
    {136, kH264, 1280, 720, 30, 4074770, 2061858, 204567760}, {298, kH264, 1280, 720, 60, 3504829, 2402272, 94223120},
    {140, kAac, 0, 0, 0, 130607, 129492, 5079985, utf8_literal(SOUND("MEDIUM", "44100"))},
};
// Not in today's lists either: YouTube gives MaiCord's player request no combined format. Itag 18 was the usual one.
constexpr Listed kPair360[]{
    {134, kH264, 640, 360, 30, 633164, 320457, 12569154},
    {140, kAac, 0, 0, 0, 130607, 129492, 5079985, utf8_literal(SOUND("MEDIUM", "44100"))},
};
constexpr Listed kCombinedLow[]{{18, kMuxed, 640, 360, 30, 500000, 480000, 18000000, utf8_literal(SOUND("LOW", "44100"))}};
constexpr Listed kCombinedMedium[]{{22, kMuxed, 640, 360, 30, 700000, 680000, 25000000, utf8_literal(SOUND("MEDIUM", "44100"))}};
// A loud hardcore track: the content is at -4.64 LKFS, and the list gives that value and its distance to -14 LKFS.
#define LOUDNESS(distance, content) ",\"loudnessDb\":" distance ",\"trackAbsoluteLoudnessLkfs\":" content
constexpr Listed kLoud[]{
    {137, kH264High, 1080, 1080, 25, 688985, 238956, 3621382}, {136, kH264, 720, 720, 25, 235821, 84580, 1281814},
    {140, kAac, 0, 0, 0, 131444, 129610, 1964620, utf8_literal(SOUND("MEDIUM", "44100") LOUDNESS("9.360001", "-4.64"))},
    {251, kOpus, 0, 0, 0, 192520, 158573, 2403987, utf8_literal(SOUND("MEDIUM", "48000") LOUDNESS("9.360001", "-4.64"))},
};
// The same track in a list that gives only the distance, as older lists did.
constexpr Listed kLoudDistance[]{
    {137, kH264High, 1080, 1080, 25, 688985, 238956, 3621382},
    {140, kAac, 0, 0, 0, 131444, 129610, 1964620, utf8_literal(SOUND("MEDIUM", "44100") ",\"loudnessDb\":9.360001")},
};
// A calm piano track at -18.84 LKFS, below both targets.
constexpr Listed kCalm[]{
    {137, kH264High, 1080, 1080, 25, 688985, 238956, 3621382},
    {140, kAac, 0, 0, 0, 133541, 129479, 45641349, utf8_literal(SOUND("MEDIUM", "44100") LOUDNESS("-4.84", "-18.84"))},
};
// Not in today's lists: a list whose only AAC format has dynamic range compression.
constexpr Listed kLoudCompressed[]{
    {137, kH264High, 1080, 1080, 25, 688985, 238956, 3621382},
    {140, kAac, 0, 0, 0, 131444, 129610, 1964620,
     utf8_literal(SOUND("MEDIUM", "44100") ",\"isDrc\":true" LOUDNESS("9.360001", "-4.64")), kDrc},
};
#undef LOUDNESS
bool summary_has(Utf8View part, bool music) noexcept {
    LogEvent line("playback_quality");
    embed_offer_summary(offer, music, &line);
    return ascii_contains_ignore_case({line.text, line.used}, part);
}
bool near_gain(float gain, float expected) noexcept {
    const float error = gain - expected;
    return error < 1e-5f && error > -1e-5f;
}

// Not in today's lists: lists whose Opus formats are not as good as the AAC format.
constexpr Listed kOpusLow[]{
    {134, kH264, 640, 360, 30, 633164, 320457, 12569154},
    {140, kAac, 0, 0, 0, 130607, 129492, 5079985, utf8_literal(SOUND("MEDIUM", "44100"))},
    {249, kOpus, 0, 0, 0, 57383, 50384, 1976319, utf8_literal(SOUND("LOW", "48000"))},
    {250, kOpus, 0, 0, 0, 94023, 79531, 1205712, utf8_literal(SOUND("LOW", "48000"))},
};
constexpr Listed kOpusCompressed[]{
    {134, kH264, 640, 360, 30, 633164, 320457, 12569154},
    {140, kAac, 0, 0, 0, 130607, 129492, 5079985, utf8_literal(SOUND("MEDIUM", "44100"))},
    {251, kOpus, 0, 0, 0, 149829, 132700, 5205204, utf8_literal(SOUND("MEDIUM", "48000") ",\"isDrc\":true"), kDrc},
};
constexpr Listed kOpusAlone[]{
    {134, kH264, 640, 360, 30, 633164, 320457, 12569154},
    {250, kOpus, 0, 0, 0, 94023, 79531, 1205712, utf8_literal(SOUND("LOW", "48000"))},
    {251, kOpus, 0, 0, 0, 148635, 132567, 5199989, utf8_literal(SOUND("MEDIUM", "48000"))},
};
constexpr Listed kOpusSurround[]{
    {134, kH264, 640, 360, 30, 633164, 320457, 12569154},
    {140, kAac, 0, 0, 0, 130607, 129492, 5079985, utf8_literal(SOUND("MEDIUM", "44100"))},
    {338, kOpus, 0, 0, 0, 500000, 480000, 19000000,
     utf8_literal(",\"audioQuality\":\"AUDIO_QUALITY_MEDIUM\",\"audioSampleRate\":\"48000\",\"audioChannels\":4")},
};
void test_opus_choice() noexcept {
    using E = EmbedPlayerError;
    using R = EmbedFormatReason;
    check(pick(listed(kPortrait)) == E::none && picked(true) == 140 && !result.audio_fallback.url_bytes &&
              !embed_offer_quality(offer, false).opus,
          "list without Opus: the pick is itag 140 as before, and there is no fallback");
    check(pick(listed(kOpusLow)) == E::none && picked(true) == 140 && !result.audio_fallback.url_bytes &&
              offer.formats[2].reason == R::none && offer.formats[3].reason == R::none,
          "low Opus formats beside medium AAC: the pick is itag 140, because the Opus formats are a class below it");
    check(pick(listed(kOpusCompressed)) == E::none && picked(true) == 140 && !result.audio_fallback.url_bytes,
          "compressed Opus beside original AAC: the pick is itag 140, the original dynamic range");
    check(pick(listed(kOpusAlone)) == E::none && picked(true) == 251 && result.audio.bitrate == 148635 &&
              !result.audio_fallback.url_bytes && embed_offer_quality(offer, false).opus,
          "list without AAC: the pick is itag 251, the Opus format of the higher bitrate, with no fallback");
    check(pick(listed(kOpusSurround)) == E::none && picked(true) == 140 && offer.formats[2].reason == R::channels,
          "Opus with 4 channels: the client plays 1 or 2 channels, so the pick is itag 140");
}

void test_loudness() noexcept {
    using E = EmbedPlayerError;
    check(pick(listed(kLoud)) == E::none && picked(true) == 251 && (offer.formats[2].flags & embed_format_loudness) &&
              offer.formats[2].loudness == -464 && offer.formats[3].loudness == -464 &&
              !(offer.formats[0].flags & embed_format_loudness),
          "loud track: each audio format carries the content loudness, -4.64 LKFS");
    summary_is(utf8_literal("playback_quality provider=youtube_music video_pick=\"h264 1080x1080 25 239kbps\" "
                            "video_playable_best=\"h264 1080x1080 25 239kbps\" video_best=\"h264 1080x1080 25 239kbps\" "
                            "h264_best=\"h264 1080x1080 25 239kbps\" audio_pick=\"opus 159kbps 48000Hz 2ch\" "
                            "audio_playable_best=\"opus 159kbps 48000Hz 2ch\" audio_best=\"opus 159kbps 48000Hz 2ch\" "
                            "aac_best=\"aac 130kbps 44100Hz 2ch\" opus_best=\"opus 159kbps 48000Hz 2ch\" "
                            "loudness_lkfs=-4.64 target_lkfs=-7 gain_db=-2.36 formats=4 playable=4"),
               "loud track on YouTube Music: the line gives the loudness, the -7 LKFS target and the gain of -2.36 dB", true);
    check(summary_has(utf8_literal(" loudness_lkfs=-4.64 target_lkfs=-14 gain_db=-9.36 formats=4"), false),
          "loud track on YouTube: the target is -14 LKFS and the gain is -9.36 dB");
    check(embed_offer_quality(offer, true).attenuation == 236 && embed_offer_quality(offer, false).attenuation == 936,
          "loud track: the session receives 2.36 dB of attenuation for YouTube Music and 9.36 dB for YouTube");
    check(pick(listed(kLoudDistance)) == E::none && offer.formats[1].loudness == -464 &&
              embed_offer_quality(offer, true).attenuation == 236,
          "loud track, distance only: the content loudness is the distance below the -14 LKFS target of the request");
    check(pick(listed(kCalm)) == E::none && offer.formats[1].loudness == -1884 &&
              !embed_offer_quality(offer, true).attenuation && !embed_offer_quality(offer, false).attenuation &&
              summary_has(utf8_literal(" loudness_lkfs=-18.84 target_lkfs=-14 gain_db=0.00 "), false),
          "calm track: a track below the target gets no gain, because the rule never raises the level");
    check(pick(listed(kLoudCompressed)) == E::none && picked(true) == 140 && !embed_offer_quality(offer, true).attenuation &&
              summary_has(utf8_literal(" gain_db=0.00 "), true),
          "compressed format: a format with dynamic range compression gets no gain, as in YouTube's player");
    check(pick(listed(kSong)) == E::none && !embed_offer_quality(offer, true).attenuation &&
              !summary_has(utf8_literal("loudness"), true) && !summary_has(utf8_literal("gain_db"), true),
          "list without a loudness value: no gain, and the line names none");
    check(media_attenuation_gain(0) == 1.0f && near_gain(media_attenuation_gain(236), 0.7620790f) &&
              near_gain(media_attenuation_gain(936), 0.3404082f) && near_gain(media_attenuation_gain(2000), 0.1f) &&
              near_gain(media_attenuation_gain(602), 0.5000345f) && media_attenuation_gain(65535) < 1e-30f,
          "gain: 10^(-dB/20) without a math library, exact to 1e-5");
    const auto exponent = utf8_literal(R"({"url":"https://rr1---sn-test.googlevideo.com/videoplayback?id=audio","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","contentLength":"100000","bitrate":128000,"audioChannels":2,"loudnessDb":1e1,"trackAbsoluteLoudnessLkfs":"-4.64"})");
    reset_value(&result);
    check(youtube_player_streams(response(video360, exponent), id, 360, {scratch, 8192}, &result, &offer) == E::none &&
              !(offer.formats[1].flags & embed_format_loudness) && !embed_offer_quality(offer, true).attenuation,
          "loudness values the client cannot read: the format plays with no gain, and the list stays valid");
}

void test_choice() noexcept {
    using E = EmbedPlayerError;
    using R = EmbedFormatReason;
    check(pick(listed(kSixty)) == E::none && picked(false) == 299 && picked(true) == 251 && !result.combined &&
              result.video.width == 1920 && result.video.height == 1080 && result.audio.bitrate == 148635 &&
              (offer.formats[16].flags & embed_format_picked) && !(offer.formats[17].flags & embed_format_picked),
          "2160p60 HDR video: the pick is itag 299 (H.264 1080p60) with itag 251 (Opus, not the DRC twin)");
    check(utf8_equal({result.audio_fallback.url, result.audio_fallback.url_bytes},
                     utf8_literal("https://rr1---sn-test.googlevideo.com/videoplayback?itag=140")) &&
              result.audio_fallback.bitrate == 130607,
          "2160p60 HDR video: the AAC format with the original dynamic range is the fallback for the Opus format");
    check(offer.count == 18 && offer.formats[0].reason == R::container && offer.formats[2].reason == R::codec &&
              offer.formats[11].reason == R::codec && offer.formats[15].reason == R::none &&
              offer.formats[13].reason == R::none && !(offer.formats[13].flags & embed_format_picked),
          "2160p60 HDR video: VP9 in WebM is a format the client cannot read, AV1 and HE-AAC are codecs it cannot play, "
          "and Opus in WebM plays");
    summary_is(utf8_literal("playback_quality provider=youtube video_pick=\"h264 1920x1080 60 4231kbps\" "
                            "video_playable_best=\"h264 1920x1080 60 4231kbps\" video_best=\"vp9 3840x2160 60 28894kbps hdr\" "
                            "h264_best=\"h264 1920x1080 60 4231kbps\" vp9_best=\"vp9 3840x2160 60 28894kbps hdr\" "
                            "av1_best=\"av1 3840x2160 60 24632kbps hdr\" audio_pick=\"opus 133kbps 48000Hz 2ch\" "
                            "audio_playable_best=\"opus 133kbps 48000Hz 2ch\" audio_best=\"opus 133kbps 48000Hz 2ch\" "
                            "aac_best=\"aac 129kbps 44100Hz 2ch\" opus_best=\"opus 133kbps 48000Hz 2ch\" formats=18 playable=11"),
               "the summary line names the pick, the best playable and the best offered format, and the best of each codec");
    format_is(1, utf8_literal("playback_format itag=337 codec=vp9 container=webm width=3840 height=2160 fps=60 "
                              "bitrate=28894277 flags=adaptive,hdr playable=0 picked=0 reason=container"),
              "a format line: HDR VP9 in WebM");
    format_is(2, utf8_literal("playback_format itag=701 codec=av1 container=mp4 width=3840 height=2160 fps=60 "
                              "bitrate=24632098 flags=adaptive,hdr playable=0 picked=0 reason=codec"),
              "a format line: AV1 in MP4");
    format_is(4, utf8_literal("playback_format itag=299 codec=h264 container=mp4 width=1920 height=1080 fps=60 "
                              "bitrate=4230996 flags=adaptive playable=1 picked=1"),
              "a format line: the picked video");
    format_is(12, utf8_literal("playback_format itag=139 codec=he-aac container=mp4 sample_rate=22050 channels=2 "
                               "bitrate=48816 flags=adaptive,drc playable=0 picked=0 reason=codec"),
              "a format line: HE-AAC with dynamic range compression");
    format_is(13, utf8_literal("playback_format itag=140 codec=aac container=mp4 sample_rate=44100 channels=2 "
                               "bitrate=129492 flags=adaptive,drc playable=1 picked=0"),
              "a format line: playable AAC that lost to the original dynamic range");
    bool clean = true;
    for (u32 i = 0; i < offer.count; ++i) {
        LogEvent line("playback_format");
        embed_offer_format(offer.formats[i], &line);
        const Utf8View text{line.text, line.used};
        clean = clean && !ascii_contains_ignore_case(text, utf8_literal("http")) &&
                !ascii_contains_ignore_case(text, utf8_literal("googlevideo")) &&
                !ascii_contains_ignore_case(text, utf8_literal("aB_01234-xy"));
    }
    check(clean, "no format line holds a URL or the video id");
    const auto shown = embed_offer_quality(offer, false);
    check(shown.provider == MediaQualityProvider::youtube && shown.width == 1920 && shown.height == 1080 &&
              shown.fps == 60 && shown.video_bitrate == 4230996 && shown.audio_bitrate == 132567 && shown.channels == 2 &&
              shown.opus,
          "the page shows the pick's mean bitrates, frame rate and channels");

    check(pick(listed(kPortrait)) == E::none && picked(false) == 299 && result.video.width == 1080 &&
              result.video.height == 1920,
          "portrait Short: the pick is itag 299 (1080 x 1920), because the cap is the shorter side");
    check(pick(listed(kPortrait), 720) == E::none && picked(false) == 298 && pick(listed(kPortrait), 144) == E::none &&
              picked(false) == 160,
          "portrait Short: a lower cap picks the format of that shorter side");
    check(pick(listed(kTwinHigh134)) == E::none && picked(false) == 134 && result.video.bitrate == 212209 &&
              pick(listed(kTwinHigh133)) == E::none && picked(false) == 133 && result.video.bitrate == 183848,
          "240p listed twice: the pick is the twin with the higher bitrate, in either list order");
    check(pick(listed(kLong)) == E::none && picked(false) == 135 && picked(true) == 140 &&
              offer.formats[0].reason == R::length && offer.formats[1].reason == R::length &&
              result.video.content_bytes == 626923473,
          "3 hour video: the 4.4 GB and 1.2 GB files are skipped and the pick is itag 135 (480p, 627 MB)");
    reset_value(&result);
    check(youtube_player_streams(listed(kDay), id, 1080, {scratch, 8192}, &result, &offer) == E::unsupported &&
              !result.video.url_bytes && offer.count == 7 && offer.formats[0].reason == R::length &&
              offer.formats[4].reason == R::none && offer.formats[6].reason == R::length,
          "24 hour video: the list is valid, and the client can play no audio of it");
    summary_is(utf8_literal("playback_quality provider=youtube video_playable_best=\"h264 426x240 30 78kbps\" "
                            "video_best=\"h264 1920x1080 30 2003kbps\" h264_best=\"h264 1920x1080 30 2003kbps\" "
                            "audio_best=\"aac 129kbps 44100Hz 2ch\" aac_best=\"aac 129kbps 44100Hz 2ch\" formats=7 playable=2"),
               "the summary line of a list without a playable pair has no pick");
    check(pick(listed(kDubbed)) == E::none && picked(true) == 251 && result.audio.bitrate == 144266 &&
              offer.formats[1].reason == R::track && (offer.formats[1].flags & embed_format_dubbed) &&
              (offer.formats[3].flags & embed_format_dubbed) && (offer.formats[4].flags & embed_format_original) &&
              (offer.formats[5].flags & embed_format_original) && (offer.formats[5].flags & embed_format_picked) &&
              !(offer.formats[4].flags & embed_format_picked) && result.audio_fallback.bitrate == 131858,
          "dubbed video: the pick is the original track in Opus, after three dubbed tracks, and its AAC twin is the fallback");
    format_is(3, utf8_literal("playback_format itag=140 codec=aac container=mp4 sample_rate=44100 channels=2 "
                              "bitrate=129480 flags=adaptive,dubbed playable=0 picked=0 reason=track"),
              "a format line: an automatic dub");
    check(pick(listed(kSong)) == E::none && picked(false) == 137 && picked(true) == 251 && result.video.width == 1080 &&
              result.video.height == 1080,
          "YouTube Music song: the pick is itag 137 (1080 x 1080) with itag 251, though the AAC format has 3 kbit/s more");
    summary_is(utf8_literal("playback_quality provider=youtube_music video_pick=\"h264 1080x1080 25 95kbps\" "
                            "video_playable_best=\"h264 1080x1080 25 95kbps\" video_best=\"h264 1080x1080 25 95kbps\" "
                            "h264_best=\"h264 1080x1080 25 95kbps\" audio_pick=\"opus 127kbps 48000Hz 2ch\" "
                            "audio_playable_best=\"aac 130kbps 44100Hz 2ch\" audio_best=\"aac 130kbps 44100Hz 2ch\" "
                            "aac_best=\"aac 130kbps 44100Hz 2ch\" opus_best=\"opus 127kbps 48000Hz 2ch\" formats=4 playable=4"),
               "the summary line of a YouTube Music song", true);
    check(pick(listed(kTwoRates), 720) == E::none && picked(false) == 298,
          "30 and 60 fps of one size: the pick is the 60 fps format, though the 30 fps one has the higher bitrate");
    check(pick(listed({kPair360, 2}, {kCombinedLow, 1})) == E::none && !result.combined && picked(false) == 134 &&
              picked(true) == 140,
          "360p combined format with low audio: the pick is the adaptive pair with the better audio");
    check(pick(listed({kPair360, 2}, {kCombinedMedium, 1})) == E::none && result.combined && picked(false) == 22 &&
              !picked(true) && !result.audio.url_bytes,
          "360p combined format with equal audio: the pick is the combined format, which needs one stream");
    format_is(0, utf8_literal("playback_format itag=22 codec=h264 container=mp4 width=640 height=360 fps=30 "
                              "bitrate=680000 flags=combined playable=1 picked=1"),
              "a format line: a combined format");
}
// An art track of YouTube Music (MUSIC_VIDEO_TYPE_ATV): 121 seconds, every picture format is the square cover.
constexpr Listed kArtTrack[]{
    {137, kH264High, 1080, 1080, 25, 688985, 238956, 3621382}, {248, kVp9, 1080, 1080, 25, 1187950, 409438, 6205047},
    {399, kAv1, 1080, 1080, 25, 871246, 385632, 5844266},      {136, kH264, 720, 720, 25, 235821, 84580, 1281814},
    {247, kVp9, 720, 720, 25, 628699, 215807, 3270558},        {398, kAv1, 720, 720, 25, 560425, 245796, 3725043},
    {135, kH264, 480, 480, 25, 167035, 60218, 912617},         {244, kVp9, 480, 480, 25, 327446, 124079, 1880432},
    {397, kAv1, 480, 480, 25, 266428, 99114, 1502082},         {134, kH264, 360, 360, 25, 114607, 42336, 641613},
    {243, kVp9, 360, 360, 25, 208564, 88696, 1344203},         {396, kAv1, 360, 360, 25, 162874, 62708, 950350},
    {133, kH264, 240, 240, 25, 63199, 25045, 379572},          {242, kVp9, 240, 240, 25, 97599, 46411, 703369},
    {395, kAv1, 240, 240, 25, 77192, 30695, 465190},           {160, kH264, 144, 144, 25, 29578, 13705, 207707},
    {278, kVp9, 144, 144, 25, 53910, 25791, 390872},           {394, kAv1, 144, 144, 25, 36785, 17795, 269697},
    {139, kHeAac, 0, 0, 0, 50387, 48900, 741520, utf8_literal(SOUND("LOW", "22050"))},
    {140, kAac, 0, 0, 0, 131444, 129610, 1964620, utf8_literal(SOUND("MEDIUM", "44100"))},
    {249, kOpus, 0, 0, 0, 69079, 59834, 907105, utf8_literal(SOUND("LOW", "48000"))},
    {250, kOpus, 0, 0, 0, 94023, 79531, 1205712, utf8_literal(SOUND("LOW", "48000"))},
    {251, kOpus, 0, 0, 0, 192520, 158573, 2403987, utf8_literal(SOUND("MEDIUM", "48000"))},
};
// An official music video of YouTube Music (MUSIC_VIDEO_TYPE_OMV): the H.264 and audio formats of its list.
constexpr Listed kMusicVideo[]{
    {137, kH264High, 1920, 1080, 25, 4334157, 3038377, 80911999}, {136, kH264, 1280, 720, 25, 1058310, 993461, 26455880},
    {135, kH264, 854, 480, 25, 690764, 529610, 14103519},         {134, kH264, 640, 360, 25, 450863, 315092, 8390921},
    {140, kAac, 0, 0, 0, 130677, 129502, 3449447, utf8_literal(SOUND("MEDIUM", "44100"))},
    {251, kOpus, 0, 0, 0, 136544, 128930, 3433755, utf8_literal(SOUND("MEDIUM", "48000"))},
};
EmbedPlayerError pick_sound(Utf8View source) noexcept {
    reset_value(&result);
    return youtube_player_streams(source, id, 1080, {scratch, 8192}, &result, &offer, true);
}
void test_art_track() noexcept {
    using E = EmbedPlayerError;
    check(pick(listed(kArtTrack)) == E::none && picked(false) == 137 && picked(true) == 251 &&
              result.video.content_bytes == 3621382 && result.audio.content_bytes == 2403987,
          "art track without the kind: the pick is the video pair, itag 137 with itag 251");
    check(pick_sound(listed(kArtTrack)) == E::none && !picked(false) && picked(true) == 251 && !result.video.url_bytes &&
              !result.combined && result.audio.content_bytes == 2403987 && result.expires_seconds == 21600 &&
              utf8_equal({result.audio.url, result.audio.url_bytes},
                         utf8_literal("https://rr1---sn-test.googlevideo.com/videoplayback?itag=251")) &&
              utf8_equal({result.audio_fallback.url, result.audio_fallback.url_bytes},
                         utf8_literal("https://rr1---sn-test.googlevideo.com/videoplayback?itag=140")),
          "art track: the pick is itag 251 alone, 2403987 bytes in place of 6025369, with itag 140 as its fallback, and "
          "no video format is marked");
    summary_is(utf8_literal("playback_quality provider=youtube_music video_playable_best=\"h264 1080x1080 25 239kbps\" "
                            "video_best=\"vp9 1080x1080 25 409kbps\" h264_best=\"h264 1080x1080 25 239kbps\" "
                            "vp9_best=\"vp9 1080x1080 25 409kbps\" av1_best=\"av1 1080x1080 25 386kbps\" "
                            "audio_pick=\"opus 159kbps 48000Hz 2ch\" audio_playable_best=\"opus 159kbps 48000Hz 2ch\" "
                            "audio_best=\"opus 159kbps 48000Hz 2ch\" aac_best=\"aac 130kbps 44100Hz 2ch\" "
                            "opus_best=\"opus 159kbps 48000Hz 2ch\" formats=23 playable=10"),
               "art track: the summary line has no video pick", true);
    const auto shown = embed_offer_quality(offer, true);
    check(media_quality_sound_only(shown) && shown.provider == MediaQualityProvider::youtube_music &&
              shown.audio_bitrate == 158573 && shown.opus && shown.channels == 2 && !shown.video_bitrate && !shown.height &&
              !shown.fps,
          "art track: the page's quality has the audio part alone");
    check(pick(listed(kArtTrack)) == E::none && !media_quality_sound_only(embed_offer_quality(offer, true)),
          "a video pick is not a track without a picture");
    check(pick_sound(listed(kMusicVideo)) == E::none && !picked(false) && picked(true) == 251 &&
              pick(listed(kMusicVideo)) == E::none && picked(false) == 137 && picked(true) == 251 &&
              result.video.height == 1080,
          "music video: the caller decides. Without the kind the pick is itag 137 with itag 251");
    check(pick_sound(listed({kPair360, 1}, {kCombinedMedium, 1})) == E::none && result.combined && picked(false) == 22 &&
              result.video.url_bytes && !result.audio.url_bytes,
          "a list without a separate playable audio format gives the video also for an art track");
    // YouTube Music's answers to the kind request, as recorded in November 2026. Only the id is a fixture.
    constexpr auto art = utf8_literal(R"({"videoDetails":{"videoId":"aB_01234-xy","musicVideoType":"MUSIC_VIDEO_TYPE_ATV"}})");
    constexpr auto video = utf8_literal(R"({"videoDetails":{"videoId":"aB_01234-xy","musicVideoType":"MUSIC_VIDEO_TYPE_OMV"}})");
    constexpr auto upload = utf8_literal(R"({"videoDetails":{"videoId":"aB_01234-xy","musicVideoType":"MUSIC_VIDEO_TYPE_UGC"}})");
    constexpr auto plain = utf8_literal(R"({"videoDetails":{"videoId":"aB_01234-xy"}})");
    check(youtube_music_art_track(art, id), "kind: an art track");
    check(!youtube_music_art_track(video, id) && !youtube_music_art_track(upload, id) && !youtube_music_art_track(plain, id),
          "kind: a music video, a user upload and an answer without the field are videos");
    check(!youtube_music_art_track(
              utf8_literal(R"({"videoDetails":{"videoId":"aB_01234-xy","musicVideoType":"MUSIC_VIDEO_TYPE_ATV_2"}})"), id) &&
              !youtube_music_art_track(
                  utf8_literal(R"({"videoDetails":{"videoId":"aB_01234-xy","musicVideoType":"MUSIC_VIDEO_TYPE_PODCAST_EPISODE"}})"), id) &&
              !youtube_music_art_track(utf8_literal(R"({"videoDetails":{"videoId":"aB_01234-xy","musicVideoType":7}})"), id),
          "kind: a value the client does not know is a video");
    check(!youtube_music_art_track(art, utf8_literal("aB_01234-xz")) && !youtube_music_art_track({}, id) &&
              !youtube_music_art_track(utf8_literal(R"({"videoDetails":{"videoId":"aB_01234-xy","musicVideoType":"MUSIC_VIDEO_TYPE_ATV")"), id) &&
              !youtube_music_art_track(utf8_literal(R"({"error":{"code":400}})"), id) &&
              !youtube_music_art_track({reinterpret_cast<const char*>(list_body), kYoutubeMusicKindBytes + 1}, id),
          "kind: another id, an empty, cut, error or oversized answer is a video");
    u8 request[256];
    const u32 bytes = youtube_music_kind_body(id, utf8_literal("1.20261110.04.00"), {request, sizeof(request)});
    check(utf8_equal({reinterpret_cast<const char*>(request), bytes},
                     utf8_literal(R"({"context":{"client":{"clientName":"WEB_REMIX","clientVersion":"1.20261110.04.00"}},"videoId":"aB_01234-xy"})")) &&
              !youtube_music_kind_body(id, utf8_literal("1.20261110.04.00"), {request, 16}) &&
              !youtube_music_kind_body(utf8_literal("bad\"id"), utf8_literal("1"), {request, sizeof(request)}),
          "kind request: the body names the client and the video, and nothing else");
}
#undef SOUND
#undef TRACK

} // namespace
}
MAICORD_TEST_ENTRY(maicord_youtube_player_entry) {
    using namespace maicord;
    using E = EmbedPlayerError;
    auto source = response(video720, audio, video360, alternate);
    check(decode(source) == E::none && result.video.height == 360 && result.video.width == 640 &&
          result.video.content_bytes == 1000000 && result.audio.bitrate == 128000 && !result.combined &&
          result.expires_seconds == 21600 &&
          utf8_equal({result.audio.url, result.audio.url_bytes},
             utf8_literal("https://rr1---sn-test.googlevideo.com/videoplayback?id=audio&range=0-99")),
          "bounded video choice, default audio and JSON-escaped URL decoding");
    check(decode(source, 720) == E::none && result.video.height == 720, "quality follows requested extent");
    check(decode(source, 144) == E::none && result.video.height == 360, "smallest available above extent");
    const auto muxed = utf8_literal(R"({"url":"https://rr1.googlevideo.com/videoplayback?id=muxed","mimeType":"video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"","contentLength":"2000000","bitrate":250000,"width":640,"height":360,"audioQuality":"AUDIO_QUALITY_LOW"})");
    check(decode(response(video720, audio, muxed), 720) == E::none && !result.combined &&
          result.video.height == 720 && result.audio.url_bytes, "higher requested quality selects adaptive video despite a lower-resolution muxed alternative");
    check(decode(response(muxed, audio, video720), 720) == E::none && !result.combined &&
          result.video.height == 720, "adaptive quality choice is independent of format order");
    check(decode(response(video720, audio, muxed), 360) == E::none && result.combined && result.video.height == 360,
          "muxed source avoids a second stream when it already supplies the requested extent");
    check(decode(response(video720, muxed), 720) == E::none && result.combined,
          "missing compatible adaptive audio retains the working muxed alternative");
    check(decode(response(muxed, video720), 720) == E::none && result.combined,
          "muxed fallback survives an earlier appearance in the format list");
    const auto high_audio = utf8_literal(R"({"url":"https://rr1.googlevideo.com/videoplayback?id=high","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","contentLength":"200000","bitrate":258000,"audioChannels":2})");
    check(decode(response(video720, audio, high_audio),720)==E::none && result.audio.bitrate==258000,
          "high-quality stereo AAC is not discarded by the old 192 kbps limit");
    check(decode(response(video720, high_audio, audio),720)==E::none && result.audio.bitrate==258000,
          "high-quality stereo AAC selection is independent of order");
    source = response(video720, audio, video360, alternate);
    check(decode(source, 480, utf8_literal("bB_01234-xy")) == E::malformed, "wrong video identity rejected");
    rejected(response(video360), E::unsupported);
    constexpr auto live_player=utf8_literal(R"({"playabilityStatus":{"status":"OK","playableInEmbed":true},"videoDetails":{"videoId":"aB_01234-xy","isLive":true},"streamingData":{"expiresInSeconds":"21600","hlsManifestUrl":"https://manifest.googlevideo.com/api/manifest/hls_variant/id/example/file/index.m3u8"}})");
    check(decode(live_player)==E::none && result.live && result.video.url_bytes && !result.audio.url_bytes,
        "live identity resolves to a bounded clear HLS master for the native playlist lane");
    constexpr auto playlist_url=utf8_literal("https://manifest.googlevideo.com/api/manifest/hls_playlist/id/example/file/index.m3u8");
    check(embed_stream_location(CdnHost::youtube_playlist,playlist_url) &&
        !embed_stream_location(CdnHost::youtube_live,playlist_url) &&
        !embed_stream_location(CdnHost::youtube_playlist,utf8_literal("https://manifest.googlevideo.com.evil.test/api/manifest/hls_playlist/id/x")) &&
        !embed_stream_location(CdnHost::youtube_playlist,utf8_literal("https://manifest.googlevideo.com/api/manifest/hls_playlist/id/x\r\nCookie: bad")) &&
        !embed_stream_location(CdnHost::youtube_playlist,utf8_literal("https://manifest.googlevideo.com/other")),
        "YouTube live playlists have a distinct exact origin and path boundary");
    check(embed_stream_get(CdnHost::youtube_playlist,playlist_url,0,0,{body,sizeof(body)}) &&
        !embed_stream_get(CdnHost::youtube_playlist,playlist_url,0,100,{body,sizeof(body)}) &&
        embed_stream_location(CdnHost::youtube_live,utf8_literal("https://rr1.googlevideo.com/videoplayback/id/example/file/seg.ts")) &&
        !embed_stream_location(CdnHost::google_video,utf8_literal("https://rr1.googlevideo.com/videoplayback/id/example/file/seg.ts")),
        "live playlist and segment requests cannot enter the ordinary ranged MP4 lane");
    constexpr char live_binary[]="HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 188\r\n\r\n";
    const ConstSlice<u8> live_head{reinterpret_cast<const u8*>(live_binary),sizeof(live_binary)-1};
    check(validate_cdn_range_response(CdnImageType::mpeg_ts,live_head,188,{0,1024},1000,CdnHost::youtube_live).ok() &&
        !validate_cdn_range_response(CdnImageType::mpeg_ts,live_head,188,{0,1024},1000,CdnHost::google_video).ok() &&
        !validate_cdn_range_response(CdnImageType::mp4,live_head,188,{0,1024},1000,CdnHost::youtube_live).ok(),
        "YouTube live binary responses require the typed transport parser instead of ordinary MP4 admission");
    rejected(utf8_literal(R"({"playabilityStatus":{"status":"LOGIN_REQUIRED"}})"), E::login_required);
    rejected(utf8_literal(R"({"playabilityStatus":{"status":"UNPLAYABLE"}})"), E::unavailable);
    rejected(utf8_literal(R"({"playabilityStatus":{"status":"OK","playableInEmbed":false}})"), E::not_embeddable);
    rejected(utf8_literal(R"({"playabilityStatus":{"status":"OK","status":"LOGIN_REQUIRED"}})"), E::malformed);
    rejected(utf8_literal(R"({"playabilityStatus":{"status":"OK"}})"), E::malformed);
    rejected(utf8_literal("{} trailing"), E::malformed);
    const auto cipher = utf8_literal(R"({"signatureCipher":"opaque","mimeType":"video/mp4; codecs=\"avc1.4d401e\""})");
    rejected(response(cipher, audio), E::unsupported);
    const auto malformed = utf8_literal(R"({"url":"https://rr1.googlevideo.com/videoplayback?id=v","mimeType":"video/mp4; codecs=\"avc1.4d401e\"","contentLength":"bad","bitrate":100000,"width":640,"height":360})");
    rejected(response(malformed, audio), E::malformed);
    const auto unsafe = utf8_literal(R"({"url":"https://googlevideo.com.evil.test/videoplayback?id=v","mimeType":"video/mp4; codecs=\"avc1.4d401e\"","contentLength":"1000000","bitrate":100000,"width":640,"height":360})");
    rejected(response(unsafe, audio), E::malformed);
    constexpr Utf8View bad[]{utf8_literal("http://rr1.googlevideo.com/videoplayback?id=x"),
        utf8_literal("https://rr1.googlevideo.com@evil.test/videoplayback?id=x"),
        utf8_literal("https://rr1.googlevideo.com:443/videoplayback?id=x"),
        utf8_literal("https://rr1.googlevideo.com/other?id=x"),
        utf8_literal("https://rr1.googlevideo.com/videoplayback?id=%zz"),
        utf8_literal("https://rr1.googlevideo.com/videoplayback?id=x#fragment"),
        utf8_literal("https://rr1.googlevideo.com/videoplayback?id=x\r\nCookie: bad")};
    for (auto url : bad) check(!youtube_stream_url(url), "media origin/path/header boundary");
    u8 range_head[kCdnMaximumRequestHeadBytes];
    const auto url = utf8_literal("https://rr1.googlevideo.com/videoplayback?id=v");
    const u32 range_bytes = embed_stream_get(CdnHost::google_video, url, 4096, 4096, {range_head, sizeof(range_head)});
    const Utf8View head_text{reinterpret_cast<const char*>(range_head), range_bytes};
    check(range_bytes && ascii_contains_ignore_case(head_text, utf8_literal("Range: bytes=4096-8191\r\n")) &&
          !ascii_contains_ignore_case(head_text, utf8_literal("Cookie:")) &&
          !ascii_contains_ignore_case(head_text, utf8_literal("Authorization:")) &&
          !ascii_contains_ignore_case(head_text, utf8_literal("Visitor")), "media requests are ranged and credential-free");
    range_head[0] = 42;
    check(!embed_stream_get(CdnHost::google_video, url, 0, 0, {range_head, sizeof(range_head)}) && range_head[0] == 42,
          "provider download cannot silently become whole-file GET");
    check(!embed_stream_get(CdnHost::google_video, url, ~u64{0}, 4096, {range_head, sizeof(range_head)}), "range overflow rejected");
    source = response(video360, audio);
    check(youtube_player_streams(source, id, 480, {scratch, 1}, &result) == E::capacity,
          "caller bounds structural memory");
    u8 request[2048];
    constexpr auto processed= utf8_literal(R"({"url":"https://rr1.googlevideo.com/videoplayback?id=drc","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","contentLength":"100000","bitrate":128011,"audioChannels":2,"isDrc":true})");
    check(decode(response(video720,audio,processed),720)==E::none && result.audio.bitrate==128000,
        "original audio wins over a slightly larger dynamic-range-compressed alternative");
    check(decode(response(video720,processed,audio),720)==E::none && result.audio.bitrate==128000,
        "original audio selection is independent of response ordering");
    check(decode(response(video720,processed),720)==E::none && result.audio.bitrate==128011,
        "processed audio remains usable when no original representation is offered");
    check(decode(response(video720,processed,high_audio),720)==E::none && result.audio.bitrate==258000,
        "high-quality original AAC wins over processed audio");
    const u32 bytes = youtube_player_body(id, utf8_literal("public-visitor"), {request, sizeof(request)});
    check(bytes && bytes < 512, "compact native player request");
    const Utf8View request_body{reinterpret_cast<const char*>(request),bytes};
    check(ascii_contains_ignore_case(request_body,utf8_literal("\"clientName\":\"VISIONOS\"")) &&
        ascii_contains_ignore_case(request_body,utf8_literal("\"clientVersion\":\"1.02\"")) &&
        ascii_contains_ignore_case(request_body,kYoutubeNativeUserAgent),"player body and CDN user agent use one matching delivery profile");
    request[0] = 42;
    check(!youtube_player_body(id, utf8_literal("public-visitor"), {request, 1}) && request[0] == 42,
          "request capacity failure is transactional");
    check(!youtube_player_body(id, utf8_literal("bad\r\nvisitor"), {request, sizeof(request)}),
          "bootstrap metadata cannot inject headers");
    test_choice();
    test_art_track();
    test_opus_choice();
    test_loudness();
    ::maicord::test_process_exit(failures ? 1 : 0);
}
