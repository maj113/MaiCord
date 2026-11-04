#include "account_media_internal.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/model/chat/asset_hash.hpp"
#include "maicord/app/media/embed_resolver.hpp"
#include "maicord/protocol/media/embed_provider.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {

AccountMediaRequestResult account_media_request_static_asset(AccountMedia* media, const MediaAssetKey& key,
                                                             u64 monotonic_now_ms, u64 unix_now_seconds,
                                                             const Utf8View& guild_icon_hash, u32 identity_size) noexcept {
    if (!media || !media->started || monotonic_now_ms == 0 || unix_now_seconds == 0 ||
        !media_asset_key_valid(key) || key.kind == MediaAssetKind::catalog_preview) {
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    if (!supported_key(key)) {
        return result(AccountMediaRequestState::unsupported);
    }
    u8 guild_target[kCdnMaximumTargetBytes];
    CdnTargetResult guild_target_result{};
    const bool hashed_identity = key.kind == MediaAssetKind::guild_icon ||
                                 key.kind == MediaAssetKind::user_avatar ||
                                 key.kind == MediaAssetKind::channel_icon;
    if (hashed_identity) {
        const bool animated = asset_hash_animated(guild_icon_hash);
        const bool animated_key = key.flags == media_asset_flag_animated;
        const u32 size = identity_size ? identity_size : kCdnIdentityDefaultSize;
        const Slice<u8> output{guild_target, sizeof(guild_target)};
        guild_target_result =
            key.kind == MediaAssetKind::guild_icon
                ? write_guild_icon_cdn_target(key.id, guild_icon_hash, size, output, animated_key)
            : key.kind == MediaAssetKind::user_avatar
                ? write_user_avatar_cdn_target(key.id, guild_icon_hash, size, output)
                : write_channel_icon_cdn_target(key.id, guild_icon_hash, size, output);
        // A server icon's still key also names an a_ hash: the official rail
        // fetches the CDN's still (.webp without animated=true) until hovered.
        const bool flags_match = key.kind == MediaAssetKind::guild_icon ? !animated_key || animated
                                                                         : animated == animated_key;
        if (!guild_target_result.ok() || cdn_guild_icon_variant(guild_icon_hash) != key.variant ||
            !flags_match) {
            return result(AccountMediaRequestState::failed, AccountMediaError::target);
        }
    } else if (guild_icon_hash.bytes != 0 || guild_icon_hash.data) {
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    AccountMediaRequestResult settled;
    if (request_in_flight(media, key, &settled) || failure_cooling(media, key, monotonic_now_ms, &settled))
        return settled;
    if (webp_asset(key) && !ensure_webp_workspace(media)) {
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    }
    if (webp_asset(key) && animated_asset(key) && !ensure_animated_webp_workspace(media)) {
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    }
    if (apng_asset(key) && !ensure_apng_workspace(media)) {
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    }
    if (key.kind == MediaAssetKind::sticker && key.variant == 4 && !ensure_gif_workspace(media)) {
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    }
    if (key.kind == MediaAssetKind::regional_flag)
        return publish_bundled_flag(media, key)
                   ? result(AccountMediaRequestState::ready)
                   : result(AccountMediaRequestState::failed, AccountMediaError::decode);
    CacheProbe probe{key, monotonic_now_ms, unix_now_seconds, 0, 0, {}};
    if (lottie_asset(key)) {
        const auto lottie = probe_lottie(media, &probe);
        if (lottie == LottieProbe::published)
            return result(AccountMediaRequestState::ready);
        if (lottie == LottieProbe::rejected)
            return remember_failure(media, key, monotonic_now_ms,
                                    result(AccountMediaRequestState::failed, AccountMediaError::decode));
    } else if (key.kind == MediaAssetKind::attachment) {
        if (probe_static_jpeg(media, &probe, &settled))
            return settled;
        // Attachment downloads use the signed URL from the message payload.
        // MediaAssetKey contains identity only, so a cache miss cannot be turned
        // into a CDN URL here.
        return result(AccountMediaRequestState::unsupported);
    } else if (webp_asset(key) && animated_asset(key)) {
        CdnAnimatedWebpResult cached;
        cdn_media_pipeline_lookup_animated_webp(&cached, &media->pipeline, key, unix_now_seconds);
        if (cached.state == CdnAnimatedWebpState::lossy_workspace_required) {
            if (!ensure_lossy_webp_workspace(media)) {
                return result(AccountMediaRequestState::failed, AccountMediaError::memory);
            }
            cdn_media_pipeline_lookup_animated_webp(&cached, &media->pipeline, key, unix_now_seconds);
        }
        if (settle_probe(
                media, &probe, &settled, cached,
                [&]() noexcept { publish_animated_webp_frame(media, key, cached, 0); },
                [&](AccountMediaRequestResult* failure) noexcept {
                    failure->webp_error = cached.decode.frame_decode.error;
                    failure->animated_webp_error = cached.decode.error;
                    failure->lossy_webp_error = cached.decode.lossy_decode.error;
                }))
            return settled;
    } else if (webp_asset(key)) {
        if (probe_static_webp(media, &probe, &settled))
            return settled;
    } else if (key.variant == 2) {
        if (probe_apng(media, &probe, &settled))
            return settled;
    } else if (key.variant == 4) {
        if (probe_gif(media, &probe, &settled))
            return settled;
    } else {
        if (probe_static_png(media, &probe, &settled))
            return settled;
    }
    u8 target[kCdnMaximumTargetBytes]{};
    CdnTargetResult written{};
    if (hashed_identity) {
        copy_bytes(target, guild_target, guild_target_result.bytes);
        written = guild_target_result;
    } else {
        written =
            key.kind == MediaAssetKind::custom_emoji
                ? write_custom_emoji_cdn_target(key.id, key.flags == media_asset_flag_animated, 64,
                                                Slice<u8>{target, sizeof(target)})
                : write_sticker_cdn_target(key.id, key.variant, Slice<u8>{target, sizeof(target)});
    }
    if (!written.ok()) {
        return result(AccountMediaRequestState::failed, AccountMediaError::target);
    }
    const AccountMediaRequestResult client = ensure_client_started(media);
    if (!client.ok()) {
        return remember_failure(media, key, monotonic_now_ms, client);
    }
    return submit_fetch(media, key, monotonic_now_ms,
                        CdnClientRequest{ConstSlice<u8>{target, written.bytes}, probe.etag, 0,
                                         kCdnClientDefaultTimeoutMs, written.host,
                                         lottie_asset(key) ? CdnImageType::lottie_json
                                         : webp_asset(key)   ? CdnImageType::webp
                                         : key.variant == 4  ? CdnImageType::gif
                                                             : CdnImageType::png});
}

AccountMediaRequestResult account_media_request_guild_icon(AccountMedia* media,
                                                           const AccountMediaGuildIconRequest& request,
                                                           u64 monotonic_now_ms,
                                                           u64 unix_now_seconds) noexcept {
    const u32 size = request.reserved[0];
    if ((size != 0 && (size < 16 || size > 4'096 || (size & (size - 1)) != 0)) || request.reserved[1] != 0 ||
        (request.key.kind != MediaAssetKind::guild_icon && request.key.kind != MediaAssetKind::user_avatar &&
         request.key.kind != MediaAssetKind::channel_icon)) {
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    return account_media_request_static_asset(media, request.key, monotonic_now_ms, unix_now_seconds,
                                              request.icon_hash, size);
}

static bool collection_matches(const AccountMediaCollection* collection, const AccountMediaUrlRequest& request) noexcept {
    return collection && media_asset_key_equal(collection->key, request.key) && collection->revision == request.source_message_revision &&
        utf8_equal({collection->url, collection->url_bytes}, request.url);
}
static EmbedResolveResult load_collection(AccountMedia* media, const AccountMediaUrlRequest& request) noexcept {
    auto* previous = collection_matches(media->collection, request) ? media->collection : nullptr;
    const bool youtube = youtube_collection_locator(request.url).list.bytes != 0;
    const bool apple = apple_music_collection_locator(request.url).id.bytes != 0;
    if (previous && (!youtube || request.collection_track != previous->count || !previous->continuation_bytes))
        return {EmbedPlayerError::none, UpdateTransportError::none, 0, 200};
    u64 tracks[kMediaCollectionTracks]; SoundcloudCollection metadata{}; YoutubeCollectionPage page{};
    struct Names {
        CollectionText text;
        u64* tracks;
        bool append(u16 index, const MediaTrackTitle& title, Utf8View preview = {}) noexcept {
            if (!collection_text_reserve(text, text.bytes + 6 + title.bytes + preview.bytes)) return false;
            const u16 preview_bytes=static_cast<u16>(preview.bytes);
            copy_bytes(text.data+text.bytes,&index,2); copy_bytes(text.data+text.bytes+2,&title.bytes,2); copy_bytes(text.data+text.bytes+4,&preview_bytes,2);
            copy_bytes(text.data+text.bytes+6,title.text,title.bytes); copy_bytes(text.data+text.bytes+6+title.bytes,preview.data,preview.bytes);
            text.bytes+=6+title.bytes+preview.bytes; return true;
        }
        ~Names() noexcept { if (text.data) pages_release(text.data); }
    } names{{},tracks};
    const MediaTrackTitleSink sink{&names, +[](void* opaque, u16 index, const MediaTrackTitle& title) noexcept {
        return static_cast<Names*>(opaque)->append(index,title);
    }};
    const AppleMusicCollectionSink album_sink{&names,+[](void* opaque,u16 index,u64 id,const MediaTrackTitle& title,Utf8View preview) noexcept {
        auto& names=*static_cast<Names*>(opaque); names.tracks[index-1]=id; return names.append(index,title,preview);
    }};
    u16 album_count{};

    Utf8View continuation{};
    if (previous) continuation = {reinterpret_cast<const char*>(previous + 1) + previous->count * sizeof(CollectionTrack), previous->continuation_bytes};
    auto resolved = youtube ? youtube_collection_resolve(media->provider_resolver, request.url, continuation, &page, {}, sink)
        : apple ? apple_music_collection_resolve(media->provider_resolver,request.url,&album_count,album_sink)
        : soundcloud_collection_resolve(media->provider_resolver, request.url, {tracks, kMediaCollectionTracks}, &metadata, {}, sink);
    if (apple) metadata.count=album_count;
    if (!resolved.ok()) return resolved;
    u32 overlap = 0, old_count = previous ? previous->count : 0;
    // Continuations can repeat the last item(s); remove only an exact boundary
    // overlap. Repeated songs elsewhere in a playlist retain their positions.
    if (previous) {
        const auto* old = reinterpret_cast<const CollectionTrack*>(previous + 1);
        for (u32 n = 1; n <= minimum<u32>(old_count, page.count); ++n) {
            bool match = true;
            for (u32 i = 0; i < n; ++i) if (!media_track_id_equal(old[old_count - n + i].id, page.tracks[i])) { match = false; break; }
            if (match) overlap = n;
        }
        if (page.count == overlap || (continuation.bytes && utf8_equal(continuation, {page.continuation, page.continuation_bytes}))) return {EmbedPlayerError::unavailable};
    }
    const u32 count = youtube ? old_count + page.count - overlap : metadata.count;
    if (count > kMediaCollectionTracks) return {EmbedPlayerError::capacity};
    if (request.collection_track > count) return {EmbedPlayerError::invalid_input};
    auto* collection = static_cast<AccountMediaCollection*>(commit_pages(
        sizeof(AccountMediaCollection) + count * sizeof(CollectionTrack) + (youtube ? page.continuation_bytes : 0)));
    if (!collection) return {EmbedPlayerError::capacity};
    if (previous) *collection = *previous;
    collection->key = request.key;
    if (!youtube) copy_bytes(collection->client, metadata.client, sizeof(metadata.client));
    collection->revision = request.source_message_revision; collection->url_bytes = request.url.bytes;
    copy_bytes(collection->url, request.url.data, request.url.bytes);
    collection->count = static_cast<u16>(count);
    if (!previous) collection->initial = youtube ? (page.selected ? page.selected : 1) : static_cast<u16>(metadata.selected + 1);
    auto* ids = reinterpret_cast<CollectionTrack*>(collection + 1);
    if (youtube) {
        if (previous) copy_bytes(ids, previous + 1, old_count * sizeof(CollectionTrack));
        for (u32 i = overlap; i < page.count; ++i) ids[old_count + i - overlap].id = page.tracks[i];
        collection->continuation_bytes = page.continuation_bytes;
        copy_bytes(ids + count, page.continuation, page.continuation_bytes);
    } else for (u32 i = 0; i < count; ++i) { ids[i].id.numeric = tracks[i]; ids[i].id.apple_track=apple; }
    if (media->collection) {
        if (previous) pages_release(media->collection);
        else release_collection(media->collection);
    }
    media->collection = collection;
    for (u32 at = 0; at + 6 <= names.text.bytes;) {
        u16 ordinal{}, bytes{}, preview{};
        copy_bytes(&ordinal,names.text.data+at,2); copy_bytes(&bytes,names.text.data+at+2,2); copy_bytes(&preview,names.text.data+at+4,2); at+=6;
        if (u32{bytes}+preview > names.text.bytes-at || bytes>kMediaTrackTitleBytes) break;
        if (ordinal > overlap) {
            const u16 index = static_cast<u16>(old_count + ordinal - overlap);
            MediaTrackTitle title{}; title.bytes = bytes; copy_bytes(title.text, names.text.data + at, bytes);
            const AccountMediaCollectionTitleRequest query{request.key, request.source_message_revision, index};
            account_media_collection_title_store(media, query, ids[index - 1].id, title, 0);
            if (preview && collection_text_reserve(collection->titles,collection->titles.bytes+preview)) {
                ids[index-1].preview_offset=collection->titles.bytes; ids[index-1].preview_bytes=preview;
                copy_bytes(collection->titles.data+collection->titles.bytes,names.text.data+at+bytes,preview); collection->titles.bytes+=preview;
            }
        }
        at += bytes+preview;
    }
    return resolved;
}
static bool decode_cached_mp4(AccountMedia* media, const AccountMediaUrlRequest& request,
                              u64 unix_now_seconds) noexcept {
    auto lease = encoded_media_cache_acquire(media->pipeline.cache, request.key, unix_now_seconds);
    if (!lease.acquired)
        return false;
    bool decoded = false;
    if (lease.lookup.state == EncodedMediaCacheLookupState::fresh) {
        account_media_release_video(media);
        decoded = mp4_decoder_initialize(ensure_mp4_decoder(media), lease.lookup.bytes, request.proxy_width,
                                         request.proxy_height, media->video_device);
    }
    encoded_media_cache_release(media->pipeline.cache, &lease);
    if (!decoded)
        return false;
    media->active_key = request.key;
    adopt_mp4_source(media);
    const Mp4DecodedFrame frame = mp4_decoder_read(media->mp4_decoder);
    reset_value<MediaAssetKey>(&media->active_key);
    if (!frame.ok()) {
        mp4_decoder_release(media->mp4_decoder);
        encoded_media_cache_erase(media->pipeline.cache, request.key);
        return false;
    }
    publish_mp4_frame(media, request.key, frame, 0, request.source_message_id, request.source_message_revision, 0);
    return true;
}

static AccountMediaRequestResult queue_progressive(AccountMedia* media, const AccountMediaUrlRequest& request,
    u64 now_ms, u64 unix_seconds) noexcept {
    const auto client = ensure_client_started(media);
    if (!client.ok()) return client;
    if (cdn_client_status(&media->client).phase == CdnClientPhase::response_ready)
        cdn_client_consume_response(&media->client,
            [](void*, const CdnClientResponse&, ConstSlice<u8>) noexcept { return true; }, nullptr);
    const auto next = progressive_video_fetch_next(media->progressive, false, unix_seconds);
    if (next.step != ProgressiveVideoStep::submit || !submit_progressive(media, next.range))
        return remember_failure(media, request.key, now_ms, result(AccountMediaRequestState::failed, AccountMediaError::client));
    media->active_key = request.key;
    media->active_source_message_id = request.source_message_id;
    media->active_source_message_revision = request.source_message_revision;
    media->active_proxy_width = request.proxy_width; media->active_proxy_height = request.proxy_height;
    media->active_image_type = request.image_type;
    media->progressive_active = media->active = true;
    return result(AccountMediaRequestState::queued);
}
// Inline MP4s start with a head range; continue_progressive drives the rest.
static AccountMediaRequestResult request_progressive(AccountMedia* media, const AccountMediaUrlRequest& request,
                                                     const ConstSlice<u8>& target, CdnHost host,
                                                     u64 expires_at_seconds, u64 monotonic_now_ms,
                                                     u64 unix_now_seconds) noexcept {
    if (!media->collection && !request.companion_url.bytes && decode_cached_mp4(media, request, unix_now_seconds))
        return result(AccountMediaRequestState::ready);
    account_media_release_video(media);
    media->progressive = allocate_fetch();
    if (!media->progressive || !progressive_video_fetch_begin(media->progressive, target, host, expires_at_seconds,
            request.companion_url.bytes ? ProgressiveVideoKind::fragmented_video : ProgressiveVideoKind::movie))
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    if (request.companion_url.bytes) {
        media->progressive_audio = allocate_fetch();
        if (!media->progressive_audio || !progressive_video_fetch_begin(media->progressive_audio,
                {reinterpret_cast<const u8*>(request.companion_url.data), request.companion_url.bytes}, host,
                expires_at_seconds, ProgressiveVideoKind::fragmented_audio)) {
            account_media_release_video(media); return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        }
        media->progressive_audio->refreshable = false;
    }
    media->progressive->refreshable = request.key.kind == MediaAssetKind::attachment;
    return queue_progressive(media, request, monotonic_now_ms, unix_now_seconds);
}

// Spotify album art for activity cards: exactly https://i.scdn.co/image/<id>.
constexpr char kActivityArtOrigin[] = "https://i.scdn.co/";
static bool activity_art_url(const Utf8View& url) noexcept {
    constexpr u32 bytes = sizeof(kActivityArtOrigin) - 1;
    return url.data && url.bytes > bytes && bytes_equal(url.data, kActivityArtOrigin, bytes);
}
static MediaUrlResult parse_activity_art_url(const Utf8View& url, const Slice<u8>& target_output) noexcept {
    constexpr u32 origin_bytes = sizeof(kActivityArtOrigin) - 2;
    if (!activity_art_url(url))
        return {0, CdnHost::none, MediaUrlError::invalid_host};
    const ConstSlice<u8> target{reinterpret_cast<const u8*>(url.data + origin_bytes), url.bytes - origin_bytes};
    if (!activity_art_target_valid(target))
        return {0, CdnHost::none, MediaUrlError::invalid_target};
    if (target.count > target_output.count)
        return {0, CdnHost::none, MediaUrlError::output_capacity};
    copy_bytes(target_output.data, target.data, target.count);
    return {target.count, CdnHost::activity_art, MediaUrlError::none};
}

// Drops the previous video and reserves the video and audio fetches of a split-stream source.
static bool reserve_stream_fetches(AccountMedia* media) noexcept {
    account_media_release_video(media);
    media->progressive = allocate_fetch();
    media->progressive_audio = allocate_fetch();
    return media->progressive && media->progressive_audio;
}
// Applies a provider's retry-after wait to a failure, else `fallback_ms` (none when 0).
static void provider_retry(AccountMediaRequestResult* failure, u64 now_ms, u32 retry_after_seconds,
                           u64 fallback_ms) noexcept {
    const u64 wait = retry_after_seconds ? u64{retry_after_seconds} * 1000 : fallback_ms;
    if (wait)
        failure->retry_at_ms = deadline_after(now_ms, wait);
}
static AccountMediaRequestResult request_provider_media(AccountMedia*, const AccountMediaUrlRequest&,
                                                        u64, u64) noexcept;

namespace account_media_detail {

AccountMediaRequestResult request_url_media(AccountMedia* media,
                                                   const AccountMediaUrlRequest& request,
                                                   u64 monotonic_now_ms, u64 unix_now_seconds) noexcept {
    if (!media || !media->started || monotonic_now_ms == 0 || unix_now_seconds == 0 ||
        (request.key.kind != MediaAssetKind::attachment && request.key.kind != MediaAssetKind::embed &&
         request.key.kind != MediaAssetKind::profile_image &&
         request.key.kind != MediaAssetKind::catalog_preview) ||
        !supported_key(request.key) ||
        ((request.key.kind == MediaAssetKind::attachment || request.key.kind == MediaAssetKind::embed) &&
         (request.source_message_id == 0 || request.source_message_revision == 0)) ||
        ((request.proxy_width == 0) != (request.proxy_height == 0)) || request.proxy_width > 4'096 ||
        request.proxy_height > 4'096 ||
        (request.image_type != CdnImageType::png &&
         request.image_type != CdnImageType::webp &&
         request.image_type != CdnImageType::jpeg && !audio_image_type(request.image_type) &&
         request.image_type != CdnImageType::mp4 && request.image_type != CdnImageType::gif &&
         (request.image_type != CdnImageType::lottie_json ||
          request.key.kind != MediaAssetKind::catalog_preview))) {
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    media->video_device = request.video_device;
    const MediaAttachmentFormat expected_format =
        request.image_type == CdnImageType::png           ? MediaAttachmentFormat::png
        : request.image_type == CdnImageType::webp        ? MediaAttachmentFormat::webp
        : request.image_type == CdnImageType::jpeg        ? MediaAttachmentFormat::jpeg
        : request.image_type == CdnImageType::gif         ? MediaAttachmentFormat::gif
        : request.image_type == CdnImageType::lottie_json ? MediaAttachmentFormat::lottie
        : request.image_type == CdnImageType::ogg_opus    ? MediaAttachmentFormat::ogg_opus
        : request.image_type == CdnImageType::mp3         ? MediaAttachmentFormat::mp3
        : request.image_type == CdnImageType::wav         ? MediaAttachmentFormat::wav
        : request.image_type == CdnImageType::m4a         ? MediaAttachmentFormat::m4a
                                                                 : MediaAttachmentFormat::mp4;
    const bool attachment_key = request.key.kind == MediaAssetKind::attachment;
    const bool catalog_key = request.key.kind == MediaAssetKind::catalog_preview;
    const MediaAttachmentFormat key_format = catalog_key      ? media_asset_catalog_format(request.key)
                                             : attachment_key ? media_asset_attachment_format(request.key)
                                                              : media_asset_embed_format(request.key);
    if ((attachment_key && key_format != expected_format &&
         !(request.image_type == CdnImageType::jpeg &&
           key_format == MediaAttachmentFormat::unknown)) ||
        (!attachment_key &&
         (key_format != expected_format || (request.key.kind == MediaAssetKind::embed &&
                                            (request.key.id != request.source_message_id ||
                                             request.key.variant != request.source_message_revision))))) {
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    const bool collection = embed_collection_url(request.url);
    AccountMediaRequestResult settled;
    if (collection)
        clear_failure(media, request.key);
    else if (failure_cooling(media, request.key, monotonic_now_ms, &settled))
        return settled;
    if (request.key.kind == MediaAssetKind::embed && request.provider_host == CdnHost::none &&
        ((request.progressive && (embed_video_locator(request.url).id.bytes || youtube_collection_locator(request.url).list.bytes)) ||
         (request.progressive && request.image_type == CdnImageType::m4a && (soundcloud_track_url(request.url) || collection)) ||
         (request.image_type == CdnImageType::mp3 && (spotify_audio_locator(request.url).bytes || amazon_music_track_locator(request.url).id.bytes)) ||
         ((request.image_type == CdnImageType::m4a || request.image_type == CdnImageType::mp4) &&
          apple_music_track_locator(request.url).id.bytes)))
        return request_provider_media(media, request, monotonic_now_ms, unix_now_seconds);
    u8 target[kCdnMaximumTargetBytes]{};
    u8 proxy_target[kCdnMaximumTargetBytes];
    u32 target_bytes = 0;
    CdnHost target_host = CdnHost::none;
    u64 expires_at_seconds = ~0ull;
    if (request.provider_expires && embed_stream_location(request.provider_host, request.url)) {
        copy_bytes(target, request.url.data, request.url.bytes);
        target_bytes = request.url.bytes;
        target_host = request.provider_host;
        expires_at_seconds = request.provider_expires;
    } else if (attachment_key) {
        const AttachmentUrlResult parsed = parse_signed_attachment_url(
            request.url, request.key.id, Slice<u8>{target, sizeof(target)});
        if (!parsed.ok()) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::target);
            failure.attachment_url_error = parsed.error;
            failure.attachment_query_error = parsed.query_error;
            return remember_failure(media, request.key, monotonic_now_ms, failure);
        }
        target_bytes = parsed.target_bytes;
        target_host = parsed.host;
        expires_at_seconds = parsed.expires_at_seconds;
    } else {
        const MediaUrlResult parsed =
            catalog_key ? parse_catalog_preview_url(request.url, Slice<u8>{target, sizeof(target)},
                                                    key_format, request.proxy_width, request.proxy_height)
            : request.key.kind == MediaAssetKind::profile_image &&
                    request.image_type == CdnImageType::jpeg && !request.proxy_width && activity_art_url(request.url)
                ? parse_activity_art_url(request.url, Slice<u8>{target, sizeof(target)})
                : parse_media_url(request.url, Slice<u8>{target, sizeof(target)});
        if (!parsed.ok()) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::target);
            failure.media_url_error = parsed.error;
            return remember_failure(media, request.key, monotonic_now_ms, failure);
        }
        target_bytes = parsed.target_bytes;
        target_host = parsed.host;
    }
    ConstSlice<u8> request_target{target, target_bytes};
    if (request.companion_url.bytes && (!request.progressive || request.image_type != CdnImageType::mp4 ||
        target_host != CdnHost::google_video || !embed_stream_location(target_host, request.companion_url)))
        return result(AccountMediaRequestState::invalid, AccountMediaError::target);
    if (audio_image_type(request.image_type) &&
        ((!attachment_key && !(request.key.kind == MediaAssetKind::embed &&
            ((request.image_type == CdnImageType::mp3 && (target_host == CdnHost::spotify_preview || target_host == CdnHost::amazon_music_preview)) ||
             (request.image_type == CdnImageType::m4a && target_host == CdnHost::apple_music_preview)))) ||
         request.proxy_width || request.proxy_height))
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    if (!catalog_key && request.proxy_width != 0 && request.image_type != CdnImageType::mp4) {
        const CdnTargetResult proxied = write_media_proxy_target(
            request_target, target_host, request.proxy_width, request.proxy_height,
            request.image_type == CdnImageType::png    ? CdnMediaFormat::png
            : request.image_type == CdnImageType::webp ? CdnMediaFormat::webp
            : request.image_type == CdnImageType::gif  ? CdnMediaFormat::source
                                                              : CdnMediaFormat::jpeg,
            Slice<u8>{proxy_target, sizeof(proxy_target)}, animated_asset(request.key));
        if (!proxied.ok()) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::target);
            failure.target_error = proxied.error;
            return remember_failure(media, request.key, monotonic_now_ms, failure);
        }
        request_target = ConstSlice<u8>{proxy_target, proxied.bytes};
        target_host = proxied.host;
    }
    if (request_in_flight(media, request.key, &settled))
        return settled;
    media->active_source_message_id = request.source_message_id;
    media->active_source_message_revision = request.source_message_revision;
    media->active_catalog_scope = request.catalog_scope;
    media->active_catalog_generation = request.catalog_generation;
    CacheProbe probe{request.key, monotonic_now_ms, unix_now_seconds, request.source_message_id,
                     request.source_message_revision, {}};
    if (audio_image_type(request.image_type)) {
        CdnEncodedAudioResult cached;
        cdn_media_pipeline_lookup_audio(&cached, &media->pipeline, request.key, unix_now_seconds,
                                        media->audio_cancelled, media->audio_cancel_context);
        if (cached.ready()) {
            if (publish_audio_source(media, request.key, &cached, 0))
                return result(AccountMediaRequestState::ready);
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        }
        if (cached.state != CdnEncodedAudioState::cache_miss &&
            cached.state != CdnEncodedAudioState::fetch_required) {
            auto failure = result(AccountMediaRequestState::failed, AccountMediaError::decode);
            return remember_failure(media, request.key, monotonic_now_ms, failure);
        }
        probe.etag = embed_audio_preview_limit(target_host) ? ConstSlice<u8>{} : cached.etag;
    } else if (lottie_asset(request.key)) {
        if (probe_lottie(media, &probe) == LottieProbe::published)
            return result(AccountMediaRequestState::ready);
    } else if (webp_asset(request.key) && animated_asset(request.key)) {
        if (!ensure_webp_workspace(media) || !ensure_animated_webp_workspace(media)) {
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        }
        CdnAnimatedWebpResult cached;
        cdn_media_pipeline_lookup_animated_webp(&cached, &media->pipeline, request.key, unix_now_seconds);
        if (cached.state == CdnAnimatedWebpState::lossy_workspace_required &&
            ensure_lossy_webp_workspace(media)) {
            cdn_media_pipeline_lookup_animated_webp(&cached, &media->pipeline, request.key,
                                                    unix_now_seconds);
        }
        if (settle_probe(
                media, &probe, &settled, cached,
                [&]() noexcept { publish_animated_webp_frame(media, request.key, cached, 0); },
                [&](AccountMediaRequestResult* failure) noexcept {
                    failure->animated_webp_error = cached.decode.error;
                }))
            return settled;
    } else if (apng_asset(request.key) ||
               catalog_png_cached_animation(media, request.key, unix_now_seconds)) {
        if (!ensure_apng_workspace(media))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        if (probe_apng(media, &probe, &settled))
            return settled;
    } else if (gif_asset(request.key)) {
        if (!ensure_gif_workspace(media)) {
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        }
        if (probe_gif(media, &probe, &settled))
            return settled;
    } else if (request.image_type == CdnImageType::mp4) {
        if (!video_asset(request.key)) {
            return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
        }
        // A picker tile reopened after its decoder moved on restarts from the
        // shared encoded cache instead of refetching the source.
        if (catalog_key && decode_cached_mp4(media, request, unix_now_seconds))
            return result(AccountMediaRequestState::ready);
    } else if (request.image_type == CdnImageType::jpeg) {
        if (probe_static_jpeg(media, &probe, &settled))
            return settled;
    } else if (request.image_type == CdnImageType::webp) {
        if (!ensure_webp_workspace(media)) {
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        }
        if (probe_static_webp(media, &probe, &settled))
            return settled;
    } else {
        if (probe_static_png(media, &probe, &settled))
            return settled;
    }
    if (expires_at_seconds <= unix_now_seconds) {
        return result(AccountMediaRequestState::refresh_required);
    }

    const AccountMediaRequestResult client = ensure_client_started(media);
    if (!client.ok()) {
        return remember_failure(media, request.key, monotonic_now_ms, client);
    }
    if ((attachment_key || request.progressive) && request.image_type == CdnImageType::mp4)
        return request_progressive(media, request, request_target, target_host, expires_at_seconds,
                                   monotonic_now_ms, unix_now_seconds);
    const auto queued = submit_fetch(
        media, request.key, monotonic_now_ms,
        CdnClientRequest{request_target, probe.etag, 0, kCdnClientDefaultTimeoutMs, target_host,
                         request.image_type});
    if (queued.state == AccountMediaRequestState::queued) {
        media->active_source_message_id = request.source_message_id;
        media->active_source_message_revision = request.source_message_revision;
        media->active_proxy_width = request.proxy_width;
        media->active_proxy_height = request.proxy_height;
        media->active_image_type = request.image_type;
    }
    return queued;
}

} // namespace account_media_detail

static AccountMediaRequestResult request_provider_media(AccountMedia* media, const AccountMediaUrlRequest& request,
    u64 now_ms, u64 unix_seconds) noexcept {
    AccountMediaRequestResult settled;
    if (request_in_flight(media, request.key, &settled))
        return settled;
    const bool collection = embed_collection_url(request.url);
    const bool resident_collection = collection_matches(media->collection, request);
    const bool same_track = collection ? resident_collection &&
        (!request.collection_track || media->collection->selected == request.collection_track) :
        !media->collection || !media_asset_key_equal(media->collection->key, request.key);
    if (same_track && (account_media_video_matches(media, request.key, 0) || account_media_audio_matches(media, request.key, 0)))
        return result(AccountMediaRequestState::ready);
    auto locator = embed_video_locator(request.url);
    auto* resolver = media->provider_resolver;
    if (!resolver && locator.provider != EmbedProvider::imgur) {
        resolver = static_cast<EmbedResolver*>(commit_pages(sizeof(EmbedResolver)));
        if (!resolver) return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&media->provider_resolver), resolver);
    }
    if (resolver && media->audio_cancelled && media->audio_cancelled(media->audio_cancel_context))
        embed_resolver_cancel(resolver);
    EmbedStreams streams{};
    bool episode=false;
    const auto track = spotify_audio_locator(request.url,&episode);
    const auto apple = apple_music_track_locator(request.url);
    const auto amazon = amazon_music_track_locator(request.url);
    const bool youtube_collection = youtube_collection_locator(request.url).list.bytes != 0;
    const bool apple_collection = apple_music_collection_locator(request.url).id.bytes != 0;
    if ((!youtube_collection && !apple_collection && collection) || soundcloud_track_url(request.url)) {
        if (!request.progressive || request.image_type != CdnImageType::m4a)
            return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
        char track_url[128]; auto page = request.url; u16 selected = 0;
        if (collection) {
            const auto loaded = load_collection(media, request);
            if (!loaded.ok()) {
                auto failure = result(loaded.error == EmbedPlayerError::invalid_input ? AccountMediaRequestState::invalid :
                    AccountMediaRequestState::failed, AccountMediaError::http);
                provider_retry(&failure, now_ms, loaded.retry_after_seconds, kAccountMediaFailureRetryMs);
                return failure;
            }
            auto& queue = *media->collection;
            selected = request.collection_track ? request.collection_track : queue.initial;
            if (selected > queue.count) return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
            if (queue.failed_track == selected && now_ms < queue.retry_at_ms) {
                auto waiting = result(AccountMediaRequestState::retry_wait); waiting.retry_at_ms = queue.retry_at_ms; return waiting;
            }
            ProtocolWriter writer{{reinterpret_cast<u8*>(track_url), sizeof(track_url)}, 0};
            if (!protocol_writer_append_literal(&writer, "https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/") ||
                !protocol_writer_append_u64(&writer, reinterpret_cast<const CollectionTrack*>(&queue + 1)[selected - 1].id.numeric))
                return result(AccountMediaRequestState::failed, AccountMediaError::target);
            page = {track_url, writer.used};
        } else if (media->collection) { release_collection(media->collection); media->collection = nullptr; }
        account_media_release_audio_source(media);
        account_media_release_video(media);
        if (collection) media->collection->selected = 0;
        media->progressive = allocate_fetch();
        if (!media->progressive) return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        const auto resolved = soundcloud_resolve(resolver, page, media->progressive);
        if (!resolved.ok()) {
            account_media_release_video(media);
            auto failure = result(resolved.error == EmbedPlayerError::unsupported || resolved.error == EmbedPlayerError::not_embeddable
                ? AccountMediaRequestState::unsupported : AccountMediaRequestState::failed, AccountMediaError::http);
            provider_retry(&failure, now_ms, resolved.retry_after_seconds, 0);
            if (collection) {
                media->collection->failed_track = selected;
                media->collection->retry_at_ms = failure.retry_at_ms ? failure.retry_at_ms : deadline_after(now_ms, kAccountMediaFailureRetryMs);
                failure.retry_at_ms = media->collection->retry_at_ms;
                return failure;
            }
            return remember_failure(media, request.key, now_ms, failure);
        }
        if (collection) { media->collection->selected = selected; media->collection->failed_track = 0; media->collection->retry_at_ms = 0; }
        return queue_progressive(media, request, now_ms, unix_seconds);
    }
    if (youtube_collection || apple_collection) {
        const auto loaded = load_collection(media, request);
        if (!loaded.ok()) {
            auto failure = result(AccountMediaRequestState::failed, AccountMediaError::http);
            provider_retry(&failure, now_ms, loaded.retry_after_seconds, kAccountMediaFailureRetryMs);
            return failure;
        }
        const u16 wanted = request.collection_track ? request.collection_track : media->collection->initial;
        if (wanted == media->collection->count && media->collection->continuation_bytes) {
            auto extend = request; extend.collection_track = wanted;
            const auto next = load_collection(media, extend);
            if (!next.ok()) {
                auto failed = result(AccountMediaRequestState::failed, AccountMediaError::http);
                provider_retry(&failed, now_ms, next.retry_after_seconds, kAccountMediaFailureRetryMs);
                return failed;
            }
        }
        auto& queue = *media->collection;
        const u16 selected = wanted;
        if (!selected || selected > queue.count) return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
        account_media_release_audio_source(media);
        account_media_release_video(media);
        queue.selected = selected;
        const auto& item=reinterpret_cast<const CollectionTrack*>(&queue+1)[selected-1];
        if (apple_collection) {
            // The embed key is stable across songs; discard the previous song's encoded cache entry.
            encoded_media_cache_erase(media->pipeline.cache,request.key);
            if (!item.preview_bytes) return result(AccountMediaRequestState::unsupported,AccountMediaError::http);
            streams.audio.url_bytes=item.preview_bytes; streams.expires_seconds=3600;
            copy_bytes(streams.audio.url,queue.titles.data+item.preview_offset,item.preview_bytes);
        } else {
            locator.id = {item.id.youtube, 11};
            locator.provider = EmbedProvider::youtube_music;
        }
    } else if (media->collection) { release_collection(media->collection); media->collection = nullptr; }
    if (locator.provider == EmbedProvider::steam || locator.provider == EmbedProvider::twitch_vod || locator.provider == EmbedProvider::twitch_live || locator.provider == EmbedProvider::vimeo) {
        const bool vimeo = locator.provider == EmbedProvider::vimeo;
        Utf8View privacy_hash{};
        if (vimeo) vimeo_video_id(request.url, &privacy_hash);
        const bool live = locator.provider == EmbedProvider::twitch_live;
        const bool vod = live || locator.provider == EmbedProvider::twitch_vod;
        const auto steam = steam_video_locator(request.url);
        if (!request.progressive || request.image_type != CdnImageType::mp4 || !(vod || vimeo ? locator.id.bytes : steam.app.bytes))
            return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
        if (!reserve_stream_fetches(media)) {
            account_media_release_video(media); return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        }
        const u16 height = request.proxy_height ? minimum<u16>(1080, maximum<u16>(request.proxy_height, 144)) : 480;
        const auto resolved = vimeo ? vimeo_resolve(resolver, locator.id, height, unix_seconds, media->progressive, media->progressive_audio, {}, privacy_hash)
            : vod ? twitch_hls_resolve(resolver, locator.id, height, unix_seconds, media->progressive, media->progressive_audio, {}, live)
            : steam_resolve(resolver, steam.app, steam.movie, height, media->progressive, media->progressive_audio);
        if (!resolved.ok()) {
            account_media_release_video(media);
            auto failure = result(resolved.error == EmbedPlayerError::unsupported ? AccountMediaRequestState::unsupported
                                                                                : AccountMediaRequestState::failed, AccountMediaError::http);
            provider_retry(&failure, now_ms, resolved.retry_after_seconds, 0);
            return remember_failure(media, request.key, now_ms, failure);
        }
        if (media->progressive_audio->state == ProgressiveVideoFetchState::empty) {
            pages_release(media->progressive_audio); media->progressive_audio = nullptr;
        }
        return queue_progressive(media, request, now_ms, unix_seconds);
    }
    const bool preview = apple_collection || track.bytes || amazon.id.bytes || (apple.id.bytes && !apple.video);
    if (apple.id.bytes && (apple.video ? !request.progressive || request.image_type != CdnImageType::mp4
                                     : request.image_type != CdnImageType::m4a))
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    const u16 height = request.proxy_height ? minimum<u16>(1080, maximum<u16>(request.proxy_height, 144)) : 480;
    // Imgur's single-image MP4 mapping needs no metadata exchange. A resolved
    // provider_host also prevents re-entering this route for its direct URL.
    const auto resolved = apple_collection ? EmbedResolveResult{EmbedPlayerError::none,UpdateTransportError::none,0,200}
        : locator.provider == EmbedProvider::imgur
        ? EmbedResolveResult{imgur_video_streams(locator.id,&streams),UpdateTransportError::none,0,200}
        : track.bytes ? spotify_preview_resolve(resolver, track, unix_seconds, &streams,{},episode)
        : amazon.id.bytes ? amazon_music_preview_resolve(resolver, request.url, unix_seconds, &streams)
        : apple.id.bytes ? apple_music_preview_resolve(resolver, apple.id, apple.country, unix_seconds, &streams, {}, apple.video)
        : locator.provider == EmbedProvider::tiktok
        ? tiktok_resolve(resolver, locator.id, height, unix_seconds, &streams)
        : locator.provider == EmbedProvider::twitch
            ? twitch_clip_resolve(resolver, locator.id, height, unix_seconds, &streams)
        : locator.provider == EmbedProvider::streamable
            ? streamable_resolve(resolver, locator.id, height, unix_seconds, &streams)
        : locator.provider == EmbedProvider::pornhub
            ? pornhub_resolve(resolver, locator.id, height, unix_seconds, &streams)
        : locator.provider == EmbedProvider::twitter
            ? twitter_resolve(resolver, locator.id, height, unix_seconds, &streams, {}, twitter_video_locator(request.url).media_index)
            : youtube_resolve(resolver, locator.id, height, &streams);
    if(resolved.ok() && streams.live) {
        if(!request.progressive || request.image_type!=CdnImageType::mp4) {
            secure_clear_bytes(&streams,sizeof(streams));return result(AccountMediaRequestState::unsupported,AccountMediaError::invalid_input);
        }
        const bool reserved = reserve_stream_fetches(media);
        const auto live=reserved
            ? youtube_hls_resolve(resolver,{streams.video.url,streams.video.url_bytes},height,unix_seconds,media->progressive,media->progressive_audio)
            : EmbedResolveResult{EmbedPlayerError::capacity};
        secure_clear_bytes(&streams,sizeof(streams));
        if(!live.ok()) {
            account_media_release_video(media);
            auto failure=result(live.error==EmbedPlayerError::unsupported?AccountMediaRequestState::unsupported:AccountMediaRequestState::failed,
                live.error==EmbedPlayerError::capacity?AccountMediaError::memory:AccountMediaError::http);
            provider_retry(&failure, now_ms, live.retry_after_seconds, 0);
            return remember_failure(media,request.key,now_ms,failure);
        }
        return queue_progressive(media,request,now_ms,unix_seconds);
    }
    if (!resolved.ok() || (preview ? !streams.audio.url_bytes : !streams.video.url_bytes ||
        (!streams.combined && !streams.audio.url_bytes))) {
        auto failure = result(resolved.ok() || resolved.error == EmbedPlayerError::login_required ||
                              resolved.error == EmbedPlayerError::not_embeddable ||
                              resolved.error == EmbedPlayerError::unsupported ? AccountMediaRequestState::unsupported
                                                                                 : AccountMediaRequestState::failed,
                              AccountMediaError::http);
        provider_retry(&failure, now_ms, resolved.retry_after_seconds, 0);
        secure_clear_bytes(&streams, sizeof(streams));
        return remember_failure(media, request.key, now_ms, failure);
    }
    auto source = request;
    const auto& selected = preview ? streams.audio : streams.video;
    source.url = {selected.url, selected.url_bytes};
    source.provider_expires = unix_seconds + streams.expires_seconds;
    source.provider_host = track.bytes ? CdnHost::spotify_preview : amazon.id.bytes ? CdnHost::amazon_music_preview : (apple.id.bytes || apple_collection)
        ? (apple.video ? CdnHost::apple_music_video_preview : CdnHost::apple_music_preview)
        : locator.provider == EmbedProvider::tiktok ? CdnHost::tiktok_video
        : locator.provider == EmbedProvider::twitch ? CdnHost::twitch_video
        : locator.provider == EmbedProvider::streamable ? CdnHost::streamable_video
        : locator.provider == EmbedProvider::pornhub ? CdnHost::pornhub_video
        : locator.provider == EmbedProvider::twitter ? CdnHost::twitter_video
        : locator.provider == EmbedProvider::imgur ? CdnHost::imgur_video : CdnHost::google_video;
    if (!preview && !streams.combined) source.companion_url = {streams.audio.url, streams.audio.url_bytes};
    const auto requested = request_url_media(media, source, now_ms, unix_seconds);
    secure_clear_bytes(&streams, sizeof(streams));
    return requested;
}

} // namespace maicord
