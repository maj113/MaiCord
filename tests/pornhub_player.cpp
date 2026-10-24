#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures, scratch[4096];
u8 text[8192];
constexpr auto id = utf8_literal("683dbe0849d99");
constexpr auto endpoint = utf8_literal("https://www.pornhub.com/video/get_media?s=fixture&v=123&e=2000&t=fixture");
constexpr auto low = utf8_literal(R"({"format":"mp4","width":854,"height":480,"videoUrl":"https://ee.phncdn.com/videos/fixture/video.mp4?validto=2000&hash=fixture"})");
constexpr auto high = utf8_literal(R"({"format":"mp4","width":1920,"height":1080,"videoUrl":"https://ev.phncdn.com/videos/fixture/video.mp4?validto=3000&hash=fixture"})");
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
Utf8View page(Utf8View key = id, Utf8View url = endpoint, bool blocked = false, bool duplicate = false) noexcept {
    ProtocolWriter w{{text, sizeof(text)}, 0};
    protocol_writer_append_literal(&w, "<script>var flashvars = {\"link_url\":\"https://www.pornhub.com/view_video.php?viewkey=");
    protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(key.data), key.bytes);
    protocol_writer_append_literal(&w, "\",\"video_title\":\"fixture }; flashvars = {}\",\"video_unavailable\":");
    if (blocked) protocol_writer_append_literal(&w, "true");
    else protocol_writer_append_literal(&w, "\"false\"");
    protocol_writer_append_literal(&w, ",\"video_unavailable_country\":false,\"mediaDefinitions\":[{\"format\":\"mp4\",\"remote\":true,\"videoUrl\":\"");
    protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(url.data), url.bytes);
    protocol_writer_append_literal(&w, "\"}]};");
    if (duplicate) protocol_writer_append_literal(&w, "flashvars = {};");
    protocol_writer_append_literal(&w, "</script>");
    return {reinterpret_cast<char*>(text), w.used};
}
Utf8View rows(Utf8View first, Utf8View second = {}) noexcept {
    ProtocolWriter w{{text, sizeof(text)}, 0};
    protocol_writer_append_literal(&w, "[");
    protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(first.data), first.bytes);
    if (second.bytes) {
        protocol_writer_append_literal(&w, ",");
        protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(second.data), second.bytes);
    }
    protocol_writer_append_literal(&w, "]");
    return {reinterpret_cast<char*>(text), w.used};
}
void run() noexcept {
    using E = EmbedPlayerError;
    const Utf8View accepted[]{utf8_literal("https://www.pornhub.com/view_video.php?viewkey=683dbe0849d99"),
        utf8_literal("https://pornhub.com/embed/683dbe0849d99")};
    for (auto url : accepted) {
        const auto locator = embed_video_locator(url);
        check(locator.provider == EmbedProvider::pornhub && utf8_equal(locator.id, id), "PH public and embed URLs bind the same video");
    }
    const Utf8View rejected[]{utf8_literal("http://www.pornhub.com/embed/683dbe0849d99"),
        utf8_literal("https://www.pornhub.com.evil.test/embed/683dbe0849d99"),
        utf8_literal("https://www.pornhub.com@evil.test/embed/683dbe0849d99"),
        utf8_literal("https://www.pornhub.com:443/embed/683dbe0849d99"),
        utf8_literal("https://www.pornhub.com/embed/683dbe0849d99/extra"),
        utf8_literal("https://www.pornhub.com/view_video.php?viewkey=683dbe0849d99&viewkey=683dbe0849d99")};
    for (auto url : rejected) check(!embed_video_locator(url).id.bytes, "PH malformed or ambiguous locators cannot route playback");
    check(pornhub_video_id(utf8_literal("ph683dbe0849d99")), "PH legacy viewkey prefix is supported");
    EmbedStream metadata{};
    check(pornhub_embed_metadata(page(), id, {scratch, 4096}, &metadata) == E::none &&
          utf8_equal({metadata.url, metadata.url_bytes}, endpoint), "PH declarative config binds identity and ignores script-like text inside JSON strings");
    const auto saved = metadata;
    check(pornhub_embed_metadata(page(utf8_literal("783dbe0849d99")), id, {scratch, 4096}, &metadata) == E::malformed &&
          pornhub_embed_metadata(page(id, endpoint, true), id, {scratch, 4096}, &metadata) == E::unavailable &&
          pornhub_embed_metadata(page(id, endpoint, false, true), id, {scratch, 4096}, &metadata) == E::malformed &&
          pornhub_embed_metadata(page(id, utf8_literal("https://evil.test/video/get_media?s=x")), id, {scratch, 4096}, &metadata) == E::malformed &&
          bytes_equal(&metadata, &saved, sizeof(saved)), "PH identity, availability, duplicate configuration and metadata origin failures are transactional");
    check(pornhub_embed_metadata(page(), id, {scratch, 1}, &metadata) == E::capacity &&
          pornhub_embed_metadata(utf8_literal("<script>flashvars = {"), id, {scratch, 4096}, &metadata) == E::malformed,
          "PH truncated config and exhausted JSON index fail explicitly");
    EmbedStreams streams{};
    check(pornhub_video_streams(rows(low, high), 720, 1000, {scratch, 4096}, &streams) == E::none &&
          streams.combined && streams.video.height == 480 && streams.expires_seconds == 1000 && !streams.audio.url_bytes,
          "PH selects a bounded combined MP4 without a duplicate audio fetch");
    check(pornhub_video_streams(rows(high, low), 1080, 1000, {scratch, 4096}, &streams) == E::none &&
          streams.video.height == 1080 && streams.expires_seconds == 2000, "PH quality choice is independent of metadata order");
    const auto previous = streams;
    check(pornhub_video_streams(rows(low), 720, 2000, {scratch, 4096}, &streams) == E::unavailable &&
          pornhub_video_streams(utf8_literal("[]"), 720, 1000, {scratch, 4096}, &streams) == E::unavailable &&
          pornhub_video_streams(rows(low, utf8_literal("{}")), 720, 1000, {scratch, 4096}, &streams) == E::none,
          "PH expired or absent MP4 renditions report unavailable; unknown formats are ignored");
    streams = previous;
    check(pornhub_video_streams(rows(low, utf8_literal(R"({"format":"mp4","width":854,"height":480,"videoUrl":"https://ee.phncdn.com.evil.test/videos/video.mp4?validto=2000"})")),
          720, 1000, {scratch, 4096}, &streams) == E::malformed && bytes_equal(&streams, &previous, sizeof(streams)),
          "PH unsafe later rendition cannot partially publish an earlier stream");
    const auto url = Utf8View{streams.video.url, streams.video.url_bytes};
    const auto n = embed_stream_get(CdnHost::pornhub_video, url, 4096, 4096, {text, sizeof(text)});
    bool range = false, referer = false, credentials = false;
    const Utf8View fragments[]{utf8_literal("Range: bytes=4096-8191"), utf8_literal("Referer: https://www.pornhub.com/"), utf8_literal("Authorization:")};
    bool* found[]{&range, &referer, &credentials};
    for (u32 f = 0; f < 3; ++f) for (u32 i = 0; i + fragments[f].bytes <= n; ++i)
        *found[f] |= bytes_equal(text + i, fragments[f].data, fragments[f].bytes);
    check(n && range && referer && !credentials, "PH media requests carry a fixed origin referer and bounded range without account credentials");
}
}
}
extern "C" void maicord_pornhub_player_entry() noexcept {
    maicord::run(); ExitProcess(maicord::failures ? 1 : 0);
}
