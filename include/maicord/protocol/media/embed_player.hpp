#pragma once
#include "maicord/protocol/media/cdn_url.hpp"
#include "maicord/media/containers/track_info.hpp"

namespace maicord {
inline constexpr auto kYoutubeNativeUserAgent = utf8_literal(
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/26.0 Safari/605.1.15");
inline constexpr u32 kEmbedStreamUrlBytes = 2048;
enum class EmbedPlayerError : u8 {
    none, invalid_input, capacity, malformed, login_required, unavailable, not_embeddable, unsupported
};
struct EmbedStream {
    char url[kEmbedStreamUrlBytes];
    u32 url_bytes, content_bytes, bitrate;
    u16 width, height;
};
struct EmbedStreams {
    EmbedStream video, audio;
    u32 expires_seconds;
    bool combined, live;
    // YouTube: the AAC format of the same list when `audio` is an Opus format in WebM. The client starts again
    // with it when it cannot read the WebM file. Empty without one.
    EmbedStream audio_fallback;
};
// One format of YouTube's list, as the log reports it. It holds no URL.
enum class EmbedCodec : u8 { other, h264, vp9, av1, aac, he_aac, opus };
enum class EmbedContainer : u8 { other, mp4, webm };
// Why the client cannot play a format.
//   protection: the URL has a cipher, or the format is under DRM.
//   container: the client reads no such container.     codec: the client has no such decoder.
//   size: a side is above 2048.                         length: the file is above the progressive limit.
//   channels: the audio has more than two channels.     track: the audio track is not the default one.
//   bitrate: the audio bitrate is above 512 kbit/s.
enum class EmbedFormatReason : u8 { none, protection, container, codec, size, length, channels, track, bitrate };
enum EmbedFormatFlag : u8 {
    embed_format_audio = 1,
    embed_format_combined = 2,
    embed_format_hdr = 4,
    embed_format_drc = 8,
    embed_format_original = 16,
    embed_format_dubbed = 32,
    embed_format_picked = 64,
    embed_format_loudness = 128,
};
struct EmbedFormat {
    // Bits per second: the mean bitrate when the list gives it, else the peak.
    u32 bitrate, sample_rate;
    u16 itag, width, height;
    u8 fps, channels, flags;
    EmbedCodec codec;
    EmbedContainer container;
    EmbedFormatReason reason;
    // The content loudness in 1/100 LKFS. It has a value only with embed_format_loudness.
    i16 loudness;
};
inline constexpr u32 kEmbedOfferFormats = 256;
struct EmbedOffer {
    EmbedFormat formats[kEmbedOfferFormats];
    u16 count;
};
// Transactional, no allocations. Caller supplies the structural index scratch.
// Select direct H.264 MP4 with Opus WebM or AAC MP4 audio, or a validated live HLS master in video.url.
// The cap is the picture's shorter side. Of two formats of one size the higher frame rate wins, then the higher bitrate.
// Ciphered/DRM responses remain unsupported; no provider JavaScript is run.
// `offer` receives every format of the list in the provider's order, the picked ones marked. It also receives the
// list when the client can play none of it. A malformed list or a live stream leaves its count 0.
// `sound_only` picks the audio format alone, by the same rule: video.url stays empty and no video format is marked.
// A list without a playable audio format then gives the video, as without `sound_only`.
EmbedPlayerError youtube_player_streams(Utf8View json, Utf8View expected_video_id,
    u16 maximum_height, Slice<u32> scratch, EmbedStreams* output, EmbedOffer* offer = nullptr,
    bool sound_only = false) noexcept;
// YouTube Music's answer to "which kind of track is this id". The player request above does not give the kind.
// The request goes to music.youtube.com/youtubei/v1/player with kYoutubeMusicKindTarget. The answer holds two fields.
inline constexpr auto kYoutubeMusicKindTarget =
    utf8_literal("/youtubei/v1/player?prettyPrint=false&fields=videoDetails(videoId%2CmusicVideoType)");
inline constexpr u32 kYoutubeMusicKindBytes = 256;
u32 youtube_music_kind_body(Utf8View video_id, Utf8View client_version, Slice<u8> output) noexcept;
// True only for an art track of this id: audio with one still picture (MUSIC_VIDEO_TYPE_ATV). A music video, a user
// upload, another kind, an answer without the field and a malformed answer all give false.
bool youtube_music_art_track(Utf8View json, Utf8View video_id) noexcept;
struct LogEvent;
// The media worker's two "[media]" log lines for one YouTube list. No line holds a URL, a video id or a title.
//
// playback_quality: one line at each playback start.
//   provider=youtube video_pick="h264 1920x1080 60 4231kbps" video_playable_best="..." video_best="vp9 3840x2160 60
//   28894kbps hdr" h264_best="..." vp9_best="..." av1_best="..." audio_pick="aac 129kbps 44100Hz 2ch"
//   audio_playable_best="..." audio_best="opus 133kbps 48000Hz 2ch" aac_best="..." opus_best="..." formats=18 playable=8
// A video value is the codec, width x height, frame rate and mean bitrate, with "hdr" for an HDR format. An audio value
// is the codec, mean bitrate, sample rate and channel count. A key without a format stays out of the line: a list the
// client cannot play has no pick. The best video format has the longest shorter side, then the highest frame rate,
// then the highest bitrate. The best audio format has the highest bitrate. A dubbed track and a dynamic-range-compressed
// one never count as a best audio format.
// With a loudness value on the picked audio the line ends with the content loudness, the target of the provider's web
// player and the gain the client applies, before the two counts: loudness_lkfs=-4.64 target_lkfs=-7 gain_db=-2.36
void embed_offer_summary(const EmbedOffer& offer, bool music, LogEvent* line) noexcept;
// playback_format: one line for each format of the list, written only when Developer Mode is on.
//   itag=337 codec=vp9 container=webm width=3840 height=2160 fps=60 bitrate=28894277 flags=adaptive,hdr playable=0
//   picked=0 reason=container
// An audio format has sample_rate and channels in place of width, height and fps. The flags are adaptive or combined,
// then hdr, drc, original and dubbed. The reason is an EmbedFormatReason: protected, container, codec, size, length,
// channels, track or bitrate.
void embed_offer_format(const EmbedFormat& format, LogEvent* line) noexcept;
// What the picked formats give the player's page.
MediaPlaybackQuality embed_offer_quality(const EmbedOffer& offer, bool music) noexcept;
// Exact Google video CDN boundary and /videoplayback target, HTTPS only.
// Does not authorize forwarding browser cookies to that CDN.
bool youtube_stream_url(Utf8View url) noexcept;
struct EmbedStreamLocation { Utf8View host, target; };
bool embed_stream_location(CdnHost, const Utf8View& url, EmbedStreamLocation* = nullptr) noexcept;
constexpr bool embed_stream_host(CdnHost host) noexcept {
    return host == CdnHost::google_video || host == CdnHost::tiktok_video || host == CdnHost::twitch_video ||
           host == CdnHost::spotify_preview || host == CdnHost::streamable_video || host == CdnHost::apple_music_preview ||
           host == CdnHost::apple_music_video_preview || host == CdnHost::steam_video || host == CdnHost::twitch_vod ||
           host == CdnHost::twitch_live || host == CdnHost::twitch_playlist || host == CdnHost::imgur_video || host == CdnHost::twitter_video ||
           host == CdnHost::amazon_music_preview || host == CdnHost::youtube_live || host == CdnHost::youtube_playlist || host == CdnHost::pornhub_video || host == CdnHost::vimeo_video || host == CdnHost::soundcloud_audio;
}
bool vimeo_location(Utf8View, bool playlist, EmbedStreamLocation* = nullptr) noexcept;
bool soundcloud_location(Utf8View, bool playlist, EmbedStreamLocation* = nullptr) noexcept;
// Public oEmbed title, bound to its exact numeric track iframe. No media request.
// Output is unchanged on malformed, oversized or mismatched metadata.
EmbedPlayerError soundcloud_track_title(Utf8View json, u64 track, Slice<u32>, MediaTrackTitle*) noexcept;
// Public page identity, streamability and full AAC rendition must all match.
// Outputs are transactional; request URLs contain ephemeral public grants.
EmbedPlayerError soundcloud_track_request(Utf8View html, Utf8View path, Slice<u32>, EmbedStream*, u64 expected_id = 0) noexcept;
EmbedPlayerError soundcloud_widget_page(Utf8View html, EmbedStream*, bool collection = false) noexcept;
EmbedPlayerError soundcloud_manifest(Utf8View json, Slice<u32>, EmbedStream*) noexcept;
inline constexpr u32 kMediaCollectionTracks = 8192;
inline constexpr u32 kYoutubeCollectionPageTracks = 200;
struct YoutubeCollectionPage {
    MediaTrackId tracks[kYoutubeCollectionPageTracks]{};
    char continuation[16384]{};
    u16 count{}, selected{}, continuation_bytes{};
};
// A validated queue window, in provider order; continuation belongs to this exact list.
EmbedPlayerError youtube_collection_page(Utf8View, Utf8View list, Slice<u32>, YoutubeCollectionPage*, MediaTrackTitleSink = {}) noexcept;
EmbedPlayerError youtube_track_title(Utf8View, Utf8View video, Slice<u32>, MediaTrackTitle*) noexcept;
// id is the playlist ID, or the track/artist seed ID for a station. The owner
// also retains the exact canonical source URL, so the two cannot alias.
// Album callbacks borrow decoded metadata. Discard it if the parser fails.
// Preview may be empty for unavailable songs; preserve their ordered positions.
struct AppleMusicCollectionSink {
    void* context{};
    bool (*write)(void*, u16, u64, const MediaTrackTitle&, Utf8View preview) noexcept{};
};
EmbedPlayerError apple_music_album(Utf8View json, Utf8View id, Utf8View country,
    Slice<u32>, u16* count, AppleMusicCollectionSink) noexcept;
// Zero IDs retain disabled entries without requesting their preview assets.
struct AppleMusicPlaylist { u64 tracks[200]{}; u16 count{}; };
EmbedPlayerError apple_music_playlist(Utf8View html, Utf8View id, Utf8View country,
    Slice<u32>, AppleMusicPlaylist*, AppleMusicCollectionSink) noexcept;
EmbedPlayerError apple_music_preview_batch(Utf8View json, ConstSlice<u64> tracks, u16 offset,
    Utf8View country, Slice<u32>, AppleMusicCollectionSink) noexcept;
struct SoundcloudCollection { u64 id{}; u32 count{}, selected{}; char client[32]{}; };
// Retains ordered identities and emits available titles from a public collection. Counts
// must match exactly: partial pages cannot silently truncate the collection.
// Both outputs remain unchanged on failure. Discard emitted titles on failure.
// Individual track playback still
// requires its own public/streamability/embedding checks when selected.
EmbedPlayerError soundcloud_collection(Utf8View html, Utf8View path, u64 expected_id,
    u32 selected, Slice<u32> scratch, Slice<u64> tracks, SoundcloudCollection*, MediaTrackTitleSink = {}) noexcept;
EmbedPlayerError soundcloud_batch_titles(Utf8View, const SoundcloudTitleBatch&, u16 preferred, Slice<u32>, MediaTrackTitleSink) noexcept;
// Identity-bound public or explicitly shared unlisted player configuration.
// Never evaluates scripts or removes password/domain restrictions.
EmbedPlayerError vimeo_video_manifest(Utf8View html, Utf8View id, Slice<u32>, EmbedStream*, Utf8View privacy_hash = {}) noexcept;
inline constexpr u32 kPornhubMetadataMaximumBytes = 128 * 1024;
bool pornhub_video_id(Utf8View) noexcept;
u32 pornhub_embed_target(Utf8View id, Slice<u8>) noexcept;
bool pornhub_metadata_location(Utf8View, EmbedStreamLocation* = nullptr) noexcept;
// Reads declarative embed configuration only. The page must identify the exact
// requested video and expose a public MP4 metadata endpoint. No JavaScript runs.
EmbedPlayerError pornhub_embed_metadata(Utf8View html, Utf8View id, Slice<u32>, EmbedStream*) noexcept;
EmbedPlayerError pornhub_video_streams(Utf8View json, u16 maximum_height, u64 now,
    Slice<u32>, EmbedStreams*) noexcept;
constexpr bool embed_playlist_host(CdnHost host) noexcept {
    return host==CdnHost::twitch_playlist || host==CdnHost::youtube_playlist;
}
u64 twitter_post_id(const Utf8View&) noexcept;
u32 twitter_video_target(Utf8View id, Slice<u8>) noexcept;
inline constexpr u32 kTwitterMetadataMaximumBytes = 128 * 1024;
// Public, identity-bound post metadata. Only its own clear MP4 media is used;
// quoted posts, unavailable media and HLS-only variants never substitute for it.
EmbedPlayerError twitter_video_streams(Utf8View json, Utf8View id, u16 maximum_height,
    u8 media_index, Slice<u32> scratch, EmbedStreams*) noexcept;
bool imgur_video_id(const Utf8View&) noexcept;
// Single-file Imgur IDs map directly to their public MP4; no metadata request
// or allocation. Native range/MIME/container validation still proves playback.
// Albums and gallery IDs are not image IDs. Output is transactional.
EmbedPlayerError imgur_video_streams(Utf8View id, EmbedStreams*) noexcept;
constexpr u32 embed_audio_preview_limit(CdnHost host) noexcept {
    return host == CdnHost::spotify_preview || host == CdnHost::amazon_music_preview ? 2 * 1024 * 1024
         : host == CdnHost::apple_music_preview ? 4 * 1024 * 1024 : 0;
}
bool amazon_music_track_id(Utf8View) noexcept;
bool amazon_music_redirect(Utf8View url, Utf8View id, EmbedStreamLocation*) noexcept;
EmbedPlayerError amazon_music_preview_stream(Utf8View url, u64 now, EmbedStreams*) noexcept;
bool apple_music_track_id(const Utf8View&) noexcept;
u32 steam_id(const Utf8View&) noexcept;
u32 steam_video_target(Utf8View app, Slice<u8>) noexcept;
bool steam_manifest_location(Utf8View url, Utf8View app = {}, EmbedStreamLocation* = nullptr) noexcept;
bool steam_media_location(Utf8View url, EmbedStreamLocation* = nullptr) noexcept;
bool twitch_vod_id(const Utf8View&) noexcept;
bool twitch_channel_id(Utf8View) noexcept;
u32 twitch_playback_body(Utf8View id, Slice<u8>, bool live = false) noexcept;
// Builds the exact usher target only after binding public token claims to ID,
// authorization and expiry. Output contains a playback token; never log it.
EmbedPlayerError twitch_playback_manifest(Utf8View json, Utf8View id, u64 now,
    Slice<u32> scratch, EmbedStream*, bool live = false) noexcept;
bool twitch_vod_location(const Utf8View& url, bool playlist, EmbedStreamLocation* = nullptr) noexcept;
bool twitch_live_location(const Utf8View& url, bool playlist, EmbedStreamLocation* = nullptr, bool transport = false) noexcept;
EmbedPlayerError steam_video_manifest(Utf8View json, Utf8View app, Utf8View movie,
    Slice<u32> scratch, EmbedStream*) noexcept;
u32 apple_music_track_target(const Utf8View& id, const Utf8View& country, const Slice<u8>&) noexcept;
inline constexpr u32 kAppleMusicMetadataMaximumBytes = 64 * 1024;
// One matching track/storefront from the public lookup API. Selects only its
// audio or music-video preview; decoded bytes still require clear codecs.
EmbedPlayerError apple_music_track_preview(Utf8View json, Utf8View id, Utf8View country,
    Slice<u32> scratch, EmbedStream*, bool video = false) noexcept;
bool streamable_video_id(const Utf8View&) noexcept;
inline constexpr u32 kStreamableMetadataMaximumBytes = 64 * 1024;
u32 streamable_video_target(Utf8View id, Slice<u8>) noexcept;
// One completed video; selects a bounded progressive MP4 rendition from the
// public metadata endpoint. Signed media URL and output publish transactionally.
EmbedPlayerError streamable_video_streams(Utf8View json, Utf8View id, u16 maximum_height,
    u64 unix_seconds, Slice<u32> scratch, EmbedStreams*) noexcept;
bool spotify_audio_id(const Utf8View&) noexcept;
inline constexpr u32 kSpotifyEmbedMaximumBytes = 128 * 1024;
u32 spotify_audio_target(Utf8View id, Slice<u8>, bool episode = false) noexcept;
// Reads the embed page's declarative JSON only; no HTML/JS execution. One
// matching, playable track/episode and an exact preview-CDN URL are required.
// Resolves a preview, never the full song/episode. Transactional output.
EmbedPlayerError spotify_audio_preview(Utf8View html, Utf8View id, Slice<u32> scratch, EmbedStream*, bool episode = false) noexcept;
// Video requires a range; audio previews permit a bounded whole-preview GET (0, 0).
u32 embed_stream_get(CdnHost, Utf8View url, u64 first, u32 bytes, Slice<u8>) noexcept;
bool twitch_clip_id(const Utf8View&) noexcept;
u32 twitch_clip_body(Utf8View id, Slice<u8>) noexcept;
EmbedPlayerError twitch_clip_streams(Utf8View json, Utf8View id, u16 maximum_height,
    u64 unix_seconds, Slice<u32> scratch, EmbedStreams*) noexcept;
u32 tiktok_player_target(Utf8View id, Slice<u8>) noexcept;
EmbedPlayerError tiktok_player_streams(Utf8View json, Utf8View id, u16 maximum_height,
    u64 unix_seconds, Slice<u32> scratch, EmbedStreams*) noexcept;
// Native client context observed in local research. visitor is public bootstrap
// metadata, never a Google credential. Output is untouched on failure.
u32 youtube_player_body(Utf8View video_id, Utf8View visitor, Slice<u8> output) noexcept;
} // namespace maicord
