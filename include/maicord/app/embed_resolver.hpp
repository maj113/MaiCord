#pragma once
#include "maicord/app/update_https.hpp"
#include "maicord/protocol/embed_player.hpp"
#include "maicord/core/reserved_bytes.hpp"

namespace maicord {
struct ProgressiveVideoFetch;
struct YoutubeBootstrap {
    char visitor[1024];
    u16 bytes;
    u8 match_upper, match_camel, phase;
    bool complete, failed;
};
// Streaming scanner for public ytcfg visitor metadata. No HTML/JS execution,
// DOM, cookie extraction or retained page. Stops reading once the value arrives.
bool youtube_bootstrap_feed(YoutubeBootstrap*, ConstSlice<u8>) noexcept;
struct EmbedResolveTransport {
    void* context;
    UpdateTransportError (*exchange)(void*, const UpdateHttpRequest&, const HttpsExchangeOptions&,
                                     UpdateHttpResponse*) noexcept;
};
struct EmbedResolver {
    UpdateHttpsTransport https;
    volatile LONG native_state; // 0 unused, 1 initializing, 2 ready, 3 cancelled.
    YoutubeBootstrap bootstrap;
    ReservedBytes response, structurals;
    u32 response_bytes;
    char music_version[32]{};
    u8 music_version_bytes{};
};
struct EmbedResolveResult {
    EmbedPlayerError error;
    UpdateTransportError transport_error;
    u32 retry_after_seconds;
    u16 status;
    bool ok() const noexcept {
        return error == EmbedPlayerError::none && transport_error == UpdateTransportError::none && status == 200;
    }
};
// Worker-only. Zero-initialize and keep at a fixed address until joined release.
// Fetches at most one public bootstrap and one player response per call. No
// unbounded retry, cookies persisted to disk, or provider network work at idle.
EmbedResolveResult youtube_resolve(EmbedResolver*, Utf8View video_id, u16 maximum_height,
    EmbedStreams*, EmbedResolveTransport transport = {}) noexcept;
EmbedResolveResult youtube_hls_resolve(EmbedResolver*, Utf8View master, u16 maximum_height, u64 unix_seconds,
    ProgressiveVideoFetch* video, ProgressiveVideoFetch* audio, EmbedResolveTransport transport = {}) noexcept;
EmbedResolveResult tiktok_resolve(EmbedResolver*, Utf8View post_id, u16 maximum_height, u64 unix_seconds,
    EmbedStreams*, EmbedResolveTransport transport = {}) noexcept;
EmbedResolveResult twitch_clip_resolve(EmbedResolver*, Utf8View clip_id, u16 maximum_height, u64 unix_seconds,
    EmbedStreams*, EmbedResolveTransport transport = {}) noexcept;
EmbedResolveResult spotify_preview_resolve(EmbedResolver*, Utf8View track_id, u64 unix_seconds,
    EmbedStreams*, EmbedResolveTransport transport = {}, bool episode = false) noexcept;
EmbedResolveResult amazon_music_preview_resolve(EmbedResolver*, Utf8View page, u64 unix_seconds,
    EmbedStreams*, EmbedResolveTransport transport = {}) noexcept;
EmbedResolveResult streamable_resolve(EmbedResolver*, Utf8View video_id, u16 maximum_height, u64 unix_seconds,
    EmbedStreams*, EmbedResolveTransport transport = {}) noexcept;
EmbedResolveResult pornhub_resolve(EmbedResolver*, Utf8View video_id, u16 maximum_height, u64 unix_seconds,
    EmbedStreams*, EmbedResolveTransport transport = {}) noexcept;
EmbedResolveResult twitter_resolve(EmbedResolver*, Utf8View post_id, u16 maximum_height, u64 unix_seconds,
    EmbedStreams*, EmbedResolveTransport transport = {}, u8 media_index = 0) noexcept;
EmbedResolveResult apple_music_preview_resolve(EmbedResolver*, Utf8View track_id, Utf8View country, u64 unix_seconds,
    EmbedStreams*, EmbedResolveTransport transport = {}, bool video = false) noexcept;
// Empty fetch outputs. Resolves one identity-bound store record and its clear
// manifest; fetches initialization/media later through the existing CDN lane.
EmbedResolveResult steam_resolve(EmbedResolver*, Utf8View app, Utf8View movie, u16 maximum_height,
    ProgressiveVideoFetch* video, ProgressiveVideoFetch* audio, EmbedResolveTransport transport = {}) noexcept;
EmbedResolveResult twitch_hls_resolve(EmbedResolver*, Utf8View id, u16 maximum_height, u64 unix_seconds,
    ProgressiveVideoFetch* video, ProgressiveVideoFetch* audio, EmbedResolveTransport transport = {}, bool live = false) noexcept;
EmbedResolveResult vimeo_resolve(EmbedResolver*, Utf8View id, u16 maximum_height, u64 unix_seconds,
    ProgressiveVideoFetch* video, ProgressiveVideoFetch* audio, EmbedResolveTransport transport = {}, Utf8View privacy_hash = {}) noexcept;
EmbedResolveResult soundcloud_resolve(EmbedResolver*, Utf8View page, ProgressiveVideoFetch* audio,
    EmbedResolveTransport transport = {}) noexcept;
// One small public metadata GET; independent of an active track's media sources.
EmbedResolveResult soundcloud_title_resolve(EmbedResolver*, u64 track, MediaTrackTitle*,
    EmbedResolveTransport transport = {}) noexcept;
EmbedResolveResult youtube_title_resolve(EmbedResolver*, Utf8View video, MediaTrackTitle*, EmbedResolveTransport = {}) noexcept;
EmbedResolveResult youtube_collection_resolve(EmbedResolver*, Utf8View page, Utf8View continuation, YoutubeCollectionPage*, EmbedResolveTransport = {}, MediaTrackTitleSink = {}) noexcept;
// One public collection page (plus canonical lookup for numeric widgets).
// Caller owns the compact ordered ID storage; output is unchanged on failure.
EmbedResolveResult soundcloud_collection_resolve(EmbedResolver*, Utf8View page, Slice<u64> tracks,
    SoundcloudCollection*, EmbedResolveTransport transport = {}, MediaTrackTitleSink = {}) noexcept;
EmbedResolveResult soundcloud_batch_titles_resolve(EmbedResolver*, const SoundcloudTitleBatch&, u16 preferred, MediaTrackTitleSink, EmbedResolveTransport = {}) noexcept;
EmbedResolveResult apple_music_collection_resolve(EmbedResolver*, Utf8View page, u16* count, AppleMusicCollectionSink, EmbedResolveTransport = {}) noexcept;
// May be called from another thread. Release only after the worker has joined.
void embed_resolver_cancel(EmbedResolver*) noexcept;
void embed_resolver_release(EmbedResolver*) noexcept;
} // namespace maicord
