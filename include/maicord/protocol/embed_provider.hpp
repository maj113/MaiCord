#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {
struct YoutubeCollectionLocator { Utf8View list{}, video{}; };
YoutubeCollectionLocator youtube_collection_locator(Utf8View) noexcept;
// Restore a queue omitted by embed metadata, only for the same video and one
// unambiguous list in the message. Borrows content; otherwise returns source.
Utf8View youtube_collection_source(Utf8View source, Utf8View content) noexcept;
// Provider identity is routing information, not a claim that an iframe URL
// contains decodable media. Direct video.proxy_url uses the native player.
enum class EmbedProvider : u8 {
    none, youtube, youtube_music, twitch, tiktok, spotify, apple_music, amazon_music, playstation, streamable, steam, twitch_vod, twitch_live, imgur, twitter, pornhub, vimeo, soundcloud
};
struct EmbedVideoLocator {
    Utf8View id; // Borrows the input; empty for a page without one video.
    u32 start_seconds;
    EmbedProvider provider;
};
// Bounded, allocation-free HTTPS parsing. Refuses userinfo, ports, encoded
// hosts and lookalike domains. Provider labels alone never authorize a host.
EmbedProvider embed_provider(Utf8View url) noexcept;
EmbedVideoLocator embed_video_locator(Utf8View url) noexcept;
// Direct/player and documented channel/group/album/on-demand video links.
// Preserve a supplied unlisted-link hash; the optional output borrows the URL
// and is written only after the complete locator has been validated.
Utf8View vimeo_video_id(Utf8View url, Utf8View* privacy_hash = nullptr) noexcept;
bool vimeo_privacy_hash(Utf8View) noexcept;
// Public single-track permalink. Borrows /artist/track, excluding share parameters.
Utf8View soundcloud_track_path(Utf8View) noexcept;
struct SoundcloudTrackLocator { Utf8View path; u64 id{}; };
// Direct permalinks borrow input. Widget permalinks borrow decoded; numeric
// widget tracks return an ID and require canonical-page identity verification.
SoundcloudTrackLocator soundcloud_track_locator(Utf8View, Slice<char> decoded) noexcept;
bool soundcloud_track_url(Utf8View) noexcept;
// Public /artist/sets/name, /discover/sets/{artist,track}-stations:ID, or a widget
// pointing at that path / numeric playlist.
// start_track is zero-based; the collection response must validate its range.
Utf8View soundcloud_collection_path(Utf8View) noexcept;
// Exact seed kind and ID from a canonical station path; zero seed on failure.
struct SoundcloudStation { u64 seed{}; bool track{}; };
SoundcloudStation soundcloud_station_locator(Utf8View path) noexcept;
struct SoundcloudCollectionLocator { Utf8View path; u64 id{}; u32 start_track{}; };
SoundcloudCollectionLocator soundcloud_collection_locator(Utf8View, Slice<char> decoded) noexcept;
bool soundcloud_collection_url(Utf8View) noexcept;
// One Spotify track or episode; albums, playlists and shows need a collection UI.
// Supports the public, embed and locale-prefixed links; borrows the input.
// Optional kind is written only on success.
Utf8View spotify_audio_locator(Utf8View url, bool* episode = nullptr) noexcept;
struct AmazonMusicTrackLocator { Utf8View id, host; };
// Single song, track embed, or album with an explicit trackAsin. Both the
// outer URL and any nested iframe must identify the same track and regional host.
AmazonMusicTrackLocator amazon_music_track_locator(Utf8View url) noexcept;
struct AppleMusicTrackLocator { Utf8View id, country; bool video{}; };
// Single song/music-video URL or album with one explicit i= song ID. Borrows input.
AppleMusicTrackLocator apple_music_track_locator(Utf8View url) noexcept;
AppleMusicTrackLocator apple_music_album_locator(Utf8View url) noexcept;
struct AppleMusicCollectionLocator { Utf8View id{},country{}; bool playlist{}; Utf8View path{}; };
AppleMusicCollectionLocator apple_music_collection_locator(Utf8View url) noexcept;
bool embed_collection_url(Utf8View url) noexcept;
struct SteamVideoLocator { Utf8View app, movie; };
struct TwitterVideoLocator { Utf8View id; u8 media_index{}; };
// Zero selects the post's first video; /video/N selects that exact media item.
TwitterVideoLocator twitter_video_locator(Utf8View url) noexcept;
// Explicit /video/app[/movie] or an app page (its first listed trailer).
SteamVideoLocator steam_video_locator(Utf8View url) noexcept;
// Providers with special iframe parameters in the researched client; its
// generic HTTPS iframe support is broader. Music widgets are separate surfaces.
bool embed_provider_video_frame(EmbedProvider provider) noexcept;
} // namespace maicord
