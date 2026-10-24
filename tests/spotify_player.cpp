#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/media/encoded_cache.hpp"
#include "maicord/core/ascii.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures, scratch[2048];
u8 html[8192];
constexpr auto id = utf8_literal("11dFghVXANMlKmJXsNCbNl");
constexpr auto url = utf8_literal("https://p.scdn.co/mp3-preview/0123456789abcdef0123456789abcdef01234567");
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
Utf8View page(Utf8View preview = url, Utf8View extra = {}, Utf8View identity = id, bool episode = false) noexcept {
    ProtocolWriter w{{html, sizeof(html)}, 0};
    protocol_writer_append_literal(&w, "<html><script id=\"__NEXT_DATA__\" type=\"application/json\">{\"props\":{\"pageProps\":{\"state\":{\"data\":{\"entity\":{\"type\":");
    protocol_writer_append_json_string(&w,episode ? utf8_literal("episode") : utf8_literal("track"));
    protocol_writer_append_literal(&w,",\"id\":");
    protocol_writer_append_json_string(&w, identity);
    protocol_writer_append_literal(&w, ",\"isPlayable\":true,\"duration\":207959,\"audioPreview\":{\"url\":");
    protocol_writer_append_json_string(&w, preview);
    protocol_writer_append_literal(&w, "}");
    protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(extra.data), extra.bytes);
    protocol_writer_append_literal(&w, "}}}}}}</script></html>");
    return {reinterpret_cast<const char*>(html), w.used};
}
void run() noexcept {
    using E = EmbedPlayerError;
    const Utf8View forms[]{utf8_literal("https://open.spotify.com/track/11dFghVXANMlKmJXsNCbNl?si=public"),
        utf8_literal("https://open.spotify.com/embed/track/11dFghVXANMlKmJXsNCbNl"),
        utf8_literal("https://open.spotify.com/intl-de/track/11dFghVXANMlKmJXsNCbNl")};
    for (auto form : forms) check(utf8_equal(spotify_audio_locator(form), id), "canonical public track identity");
    const Utf8View invalid[]{utf8_literal("https://open.spotify.com.evil.test/track/11dFghVXANMlKmJXsNCbNl"),
        utf8_literal("http://open.spotify.com/track/11dFghVXANMlKmJXsNCbNl"),
        utf8_literal("https://open.spotify.com/album/11dFghVXANMlKmJXsNCbNl"),
        utf8_literal("https://open.spotify.com/track/11dFghVXANMlKmJXsNCbNl/extra"),
        utf8_literal("https://open.spotify.com/track/short"),
        utf8_literal("https://open.spotify.com/intl-/track/11dFghVXANMlKmJXsNCbNl")};
    for (auto form : invalid) check(!spotify_audio_locator(form).bytes, "ambiguous or non-track URL is not playable");
    bool episode=false;
    const auto episode_page=utf8_literal("https://open.spotify.com/intl-de/embed/episode/11dFghVXANMlKmJXsNCbNl?si=public");
    check(utf8_equal(spotify_audio_locator(episode_page,&episode),id) && episode,
          "episode identity survives locale and embed prefixes");
    check(utf8_equal(spotify_audio_locator(forms[0],&episode),id) && !episode,
          "track identity clears a previous episode kind");
    u8 target[128];
    const auto target_bytes=spotify_audio_target(id,{target,sizeof(target)},true);
    check(utf8_equal({reinterpret_cast<char*>(target),target_bytes},utf8_literal("/embed/episode/11dFghVXANMlKmJXsNCbNl")),
          "episode resolver targets its own metadata kind");
    constexpr auto clip=utf8_literal("https://podz-content.spotifycdn.com/audio/clips/00dFghVXANMlKmJXsNCbNl/clip_123456_234567.mp3");
    EmbedStream episode_stream{};
    check(spotify_audio_preview(page(clip,utf8_literal(",\"uri\":\"spotify:episode:11dFghVXANMlKmJXsNCbNl\""),id,true),id,{scratch,2048},&episode_stream,true)==E::none &&
          utf8_equal({episode_stream.url,episode_stream.url_bytes},clip),
          "episode preview accepts identity-bound metadata and a distinct clip identity");
    check(spotify_audio_preview(page(clip,{},id,true),id,{scratch,2048},&episode_stream)==E::malformed &&
          spotify_audio_preview(page(url,{},id,true),id,{scratch,2048},&episode_stream,true)==E::malformed &&
          spotify_audio_preview(page(clip),id,{scratch,2048},&episode_stream)==E::malformed,
          "track and episode kinds and CDN families cannot cross");
    check(spotify_audio_preview(page(clip,utf8_literal(",\"isNineteenPlus\":true"),id,true),id,{scratch,2048},&episode_stream,true)==E::unavailable,
          "episode restriction is retained");
    const Utf8View bad_clips[]{
        utf8_literal("https://podz-content.spotifycdn.com.evil.test/audio/clips/00dFghVXANMlKmJXsNCbNl/clip_1_2.mp3"),
        utf8_literal("https://podz-content.spotifycdn.com/audio/clips/short/clip_1_2.mp3"),
        utf8_literal("https://podz-content.spotifycdn.com/audio/clips/00dFghVXANMlKmJXsNCbNl/clip__2.mp3"),
        utf8_literal("https://podz-content.spotifycdn.com/audio/clips/00dFghVXANMlKmJXsNCbNl/clip_1_2.mp3?other=1"),
        utf8_literal("https://podz-content.spotifycdn.com/audio/clips/00dFghVXANMlKmJXsNCbNl/../clip_1_2.mp3")};
    for(auto invalid_clip:bad_clips) check(!embed_stream_location(CdnHost::spotify_preview,invalid_clip),
          "episode preview rejects ambiguous paths and unrelated hosts");
    EmbedStream stream{};
    check(spotify_audio_preview(page(), id, {scratch, 2048}, &stream) == E::none &&
          utf8_equal({stream.url, stream.url_bytes}, url) && !stream.width && !stream.height && !stream.content_bytes,
          "public MP3 preview without inventing full-track duration or picture metadata");
    const auto prior = stream;
    const auto reject = [&](Utf8View text, E expected) noexcept {
        check(spotify_audio_preview(text, id, {scratch, 2048}, &stream) == expected &&
              bytes_equal(&stream, &prior, sizeof(stream)), "rejected response leaves selected preview unchanged");
    };
    reject(page(url, {}, utf8_literal("00dFghVXANMlKmJXsNCbNl")), E::malformed);
    reject(page(url, utf8_literal(",\"isPlayable\":false")), E::malformed);
    reject(page(url, utf8_literal(",\"isNineteenPlus\":true")), E::unavailable);
    reject(page(url, utf8_literal(",\"uri\":\"spotify:track:other\"")), E::malformed);
    reject(page(utf8_literal("https://evil.test/mp3-preview/0123456789abcdef0123456789abcdef01234567")), E::malformed);
    reject(page(utf8_literal("https://p.scdn.co/mp3-preview/short")), E::malformed);
    reject(utf8_literal("<html>No preview</html>"), E::unsupported);
    auto text = page();
    copy_bytes(html + text.bytes, text.data, text.bytes);
    reject({reinterpret_cast<const char*>(html), text.bytes * 2}, E::malformed);
    check(spotify_audio_preview(page(), id, {scratch, 1}, &stream) == E::capacity, "bounded JSON scratch");
    u8 request[1024];
    const u32 bytes = embed_stream_get(CdnHost::spotify_preview, url, 0, 0, {request, sizeof(request)});
    const Utf8View head{reinterpret_cast<const char*>(request), bytes};
    check(bytes && !ascii_contains_ignore_case(head, utf8_literal("Range:")) &&
          !ascii_contains_ignore_case(head, utf8_literal("Cookie:")) &&
          !ascii_contains_ignore_case(head, utf8_literal("Authorization:")), "credential-free whole-preview request");
    request[0] = 42;
    check(!embed_stream_get(CdnHost::spotify_preview, url, 1, 0, {request, sizeof(request)}) && request[0] == 42,
          "invalid whole-preview offset is transactional");
    auto key = media_asset_embed(1, 2, 9, MediaEmbedResource::video, MediaAttachmentFormat::mp3, false, MediaResolutionTier::px512, true);
    check(media_asset_key_valid(key) && media_asset_embed_format(key) == MediaAttachmentFormat::mp3 &&
          media_asset_resolution_tier(key) == MediaResolutionTier::original && media_asset_embed_index(key) == 9 &&
          media_asset_embed_snapshot(key) && media_asset_audio_format(key) == MediaAttachmentFormat::mp3,
          "audio embed identity preserves index, snapshot and original resolution");
    key.kind = MediaAssetKind::profile_image;
    check(!media_asset_key_valid(key), "audio formats cannot masquerade as profile images");
    key = media_asset_search_embed(1, 2, 9, MediaEmbedResource::video, MediaAttachmentFormat::mp3, false, MediaResolutionTier::px512, true);
    check(media_asset_key_valid(key) && media_asset_search(key) && media_asset_embed_format(key) == MediaAttachmentFormat::mp3,
          "search audio identity keeps its publication fence");
    key.flags |= media_asset_flag_animated;
    check(!media_asset_key_valid(key), "audio preview cannot enter a looping image decoder");
    static EncodedMediaCacheEntry entries[4];
    static u16 owners[4];
    static u8 storage[kEncodedMediaCachePageBytes * 4];
    EncodedMediaCache cache{};
    constexpr u8 bytes_fixture[]{1, 2, 3, 4};
    key.flags &= ~media_asset_flag_animated;
    const auto attachment = media_asset_attachment(44, MediaAttachmentFormat::mp3);
    const auto wave = media_asset_attachment(45, MediaAttachmentFormat::wav);
    check(encoded_media_cache_initialize(&cache, {entries, 4}, {owners, 4}, {storage, sizeof(storage)}) &&
          encoded_media_cache_store(&cache, key, {bytes_fixture, 4}, {{}, 2000, true, false}).stored() &&
          encoded_media_cache_store(&cache, attachment, {bytes_fixture, 4}, {{}, 2000, true, false}).stored() &&
          encoded_media_cache_store(&cache, wave, {bytes_fixture, 4}, {{}, 2000, true, false}).stored(),
          "encoded cache admits typed preview and attachment identities");
    const auto identity = encoded_media_cache_find(&cache, key, 1000).content_identity;
    check(identity && identity == encoded_media_cache_find(&cache, attachment, 1000).content_identity &&
          identity != encoded_media_cache_find(&cache, wave, 1000).content_identity,
          "preview source has a nonzero lifetime and shares identical bytes only within its audio format");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_spotify_player_entry() noexcept {
    maicord::run(); ExitProcess(maicord::failures ? 1 : 0);
}
