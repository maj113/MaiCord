#include "maicord/app/account_media.hpp"
#include "maicord/app/local_preview_thumbnail.hpp"
#include "maicord/app/local_video_poster.hpp"
#include "maicord/media/png_stream.hpp"
#include "maicord/media/webp_thumbnail.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/lottie_program.hpp"
#include "maicord/media/animation_timeline.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/app/progressive_video_fetch.hpp"
#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/app/embed_resolver.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/media/hevc_mp4_demux.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/media/regional_flag_svg.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/winsock.hpp"
#include "maicord/sys/crt.hpp"


namespace maicord {
// Exact ordered records and a compact UTF-8 pool. Names are retained for the
// queue lifetime; neither hovering nor returning to a track refetches them.
struct CollectionText { char* data{}; u32 bytes{}, capacity{}; };
static bool collection_text_reserve(CollectionText& text, u32 required) noexcept {
    if (required > 8u * 1024 * 1024) return false;
    if (required <= text.capacity) return true;
    u32 capacity = text.capacity ? text.capacity : 4096;
    while (capacity < required) capacity *= 2;
    auto* data = static_cast<char*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!data) return false;
    copy_bytes(data, text.data, text.bytes);
    if (text.data) VirtualFree(text.data, 0, MEM_RELEASE);
    text.data = data; text.capacity = capacity; return true;
}
struct CollectionTrack { MediaTrackId id{}; u64 retry_at_ms{}; u32 title_offset{}, preview_offset{}; u16 title_bytes{}, preview_bytes{}; };
struct AccountMediaCollection {
    MediaAssetKey key;
    u64 retry_at_ms;
    u32 revision, url_bytes;
    u16 count, selected, initial, failed_track;
    char url[2048], client[32];
    CollectionText titles;
    u16 continuation_bytes;
};
static void release_collection(AccountMediaCollection* collection) noexcept {
    if (!collection) return;
    if (collection->titles.data) VirtualFree(collection->titles.data, 0, MEM_RELEASE);
    VirtualFree(collection, 0, MEM_RELEASE);
}
namespace {

inline constexpr u32 kCacheEntryStorageBytes =
    kAccountMediaCacheEntryCapacity * sizeof(EncodedMediaCacheEntry);
inline constexpr u32 kCachePageOwnerBytes = kAccountMediaCachePageCapacity * sizeof(u16);
inline constexpr u32 kCacheIndexBytes = kAccountMediaCacheIndexCapacity * sizeof(u32);
inline constexpr u32 kCacheStorageBytes = kAccountMediaCachePageCapacity * kEncodedMediaCachePageBytes;
inline constexpr u32 kScanlineStorageBytes = maximum(2'097'664u, kGifPixelMaximum);
inline constexpr u32 kOwnedAllocationBytes = kCacheEntryStorageBytes + kCachePageOwnerBytes +
                                             kCacheIndexBytes + kCacheStorageBytes + kScanlineStorageBytes;

AccountMediaRequestResult result(AccountMediaRequestState state,
                                 AccountMediaError error = AccountMediaError::none) noexcept {
    AccountMediaRequestResult value{};
    value.state = state;
    value.error = error;
    return value;
}

#include "account_media_classify.inc"

bool png_animation_body(ConstSlice<u8> body) noexcept {
    constexpr u8 signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!body.data || body.count < sizeof(signature))
        return false;
    for (u32 index = 0; index < sizeof(signature); ++index)
        if (body[index] != signature[index])
            return false;
    for (u32 cursor = sizeof(signature); cursor <= body.count && body.count - cursor >= 12;) {
        const u32 length = (static_cast<u32>(body[cursor]) << 24) |
                           (static_cast<u32>(body[cursor + 1]) << 16) |
                           (static_cast<u32>(body[cursor + 2]) << 8) | body[cursor + 3];
        if (length > body.count - cursor - 12)
            return false;
        const u8* type = body.data + cursor + 4;
        if (type[0] == 'a' && type[1] == 'c' && type[2] == 'T' && type[3] == 'L')
            return length == 8;
        if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T')
            return false;
        cursor += length + 12;
    }
    return false;
}

bool catalog_png_cached_animation(AccountMedia* media, MediaAssetKey key, u64 now_seconds) noexcept {
    if (!catalog_png_key(key))
        return false;
    auto lease = encoded_media_cache_acquire(media->pipeline.cache, key, now_seconds);
    const bool animation = lease.acquired && png_animation_body(lease.lookup.bytes);
    if (lease.acquired)
        encoded_media_cache_release(media->pipeline.cache, &lease);
    return animation;
}

void refresh_next_wake(AccountMedia* media) noexcept {
    media->next_wake_at_ms = 0;
    for (u32 index = 0; index < kAccountMediaFailureCapacity; ++index) {
        const AccountMediaFailure& failed = media->failed[index];
        if (failed.retry_at_ms != 0 && failed.retry_at_ms != ~0ull &&
            (media->next_wake_at_ms == 0 || failed.retry_at_ms < media->next_wake_at_ms)) {
            media->next_wake_at_ms = failed.retry_at_ms;
        }
    }
}

AccountMediaFailure* find_failure(AccountMedia* media, MediaAssetKey key) noexcept {
    static_assert(kAccountMediaFailureCapacity % kAccountMediaFailureWays == 0);
    constexpr u32 bucket_count = kAccountMediaFailureCapacity / kAccountMediaFailureWays;
    static_assert((bucket_count & (bucket_count - 1)) == 0);
    const u32 first = static_cast<u32>(media_asset_hash(key)) & (bucket_count - 1u);
    const u32 begin = first * kAccountMediaFailureWays;
    for (u32 index = begin; index < begin + kAccountMediaFailureWays; ++index) {
        if (media->failed[index].retry_at_ms != 0 && media_asset_key_equal(media->failed[index].key, key)) {
            return &media->failed[index];
        }
    }
    return nullptr;
}

void clear_failure(AccountMedia* media, MediaAssetKey key) noexcept {
    AccountMediaFailure* failed = find_failure(media, key);
    if (failed) {
        *failed = AccountMediaFailure{};
        refresh_next_wake(media);
    }
}

// Records the failure with its retry deadline and returns it.
AccountMediaRequestResult remember_failure(AccountMedia* media, MediaAssetKey key, u64 now_ms,
                                           AccountMediaRequestResult failure) noexcept {
    AccountMediaFailure* slot = find_failure(media, key);
    if (!slot) {
        constexpr u32 bucket_count = kAccountMediaFailureCapacity / kAccountMediaFailureWays;
        const u32 begin =
            (static_cast<u32>(media_asset_hash(key)) & (bucket_count - 1u)) * kAccountMediaFailureWays;
        for (u32 index = begin; index < begin + kAccountMediaFailureWays; ++index) {
            if (media->failed[index].retry_at_ms == 0) {
                slot = &media->failed[index];
                break;
            }
        }
        if (!slot) {
            slot = &media->failed[begin];
            for (u32 index = begin + 1; index < begin + kAccountMediaFailureWays; ++index) {
                // Prefer replacing a transient entry that becomes eligible
                // sooner.  Permanent parse/target failures remain cached
                // unless their small hash set is itself saturated.
                if (media->failed[index].retry_at_ms < slot->retry_at_ms) {
                    slot = &media->failed[index];
                }
            }
            if (slot->retry_at_ms == ~0ull) {
                slot = &media->failed[begin];
            }
        }
    }
    const bool permanent = failure.state == AccountMediaRequestState::invalid ||
                           failure.state == AccountMediaRequestState::unsupported ||
                           failure.error == AccountMediaError::invalid_input ||
                           failure.error == AccountMediaError::target;
    slot->key = key;
    slot->retry_at_ms = permanent ? ~0ull : maximum(failure.retry_at_ms,
                                                    deadline_after(now_ms, kAccountMediaFailureRetryMs));
    failure.retry_at_ms = slot->retry_at_ms;
    media->last_failure = failure;
    ++media->failures;
    refresh_next_wake(media);
    return failure;
}

// Publishes the staged frame; a zero request id is a cache hit rather than an admitted request.
void mark_frame_ready(AccountMedia* media, MediaAssetKey key, u64 request_id) noexcept {
    media->frame_ready = true;
    media->last_failure = AccountMediaRequestResult{};
    clear_failure(media, key);
    if (request_id != 0)
        ++media->admitted;
    else
        ++media->cache_ready;
}

void capture_catalog_source(AccountMedia* media, MediaAssetKey key) noexcept {
    media->frame_catalog_scope =
        key.kind == MediaAssetKind::catalog_preview || key.kind == MediaAssetKind::local_draft_preview
            ? media->active_catalog_scope
            : 0;
    media->frame_catalog_generation =
        key.kind == MediaAssetKind::catalog_preview || key.kind == MediaAssetKind::local_draft_preview
            ? media->active_catalog_generation
            : 0;
}

void publish_png_frame(AccountMedia* media, MediaAssetKey key, const CdnStaticPngResult& decoded,
                       u64 request_id, u64 source_message_id = 0,
                       u32 source_message_revision = 0) noexcept {
    media->frame_image = decoded.image;
    media->frame_key = key;
    media->frame_bytes = decoded.scanlines.count;
    media->frame_request_id = request_id;
    media->frame_source_message_id = source_message_id;
    media->frame_source_message_revision = source_message_revision;
    capture_catalog_source(media, key);
    media->frame_kind = AccountMediaFrameKind::png_scanlines;
    mark_frame_ready(media, key, request_id);
}

void publish_webp_frame(AccountMedia* media, MediaAssetKey key, const CdnStaticWebpResult& decoded,
                        u64 request_id, u64 source_message_id = 0,
                        u32 source_message_revision = 0) noexcept {
    media->frame_webp_image = decoded.image;
    if (decoded.image.codec == WebpCodec::vp8_lossy) {
        media->frame_vp8 = decoded.vp8_frame;
        media->frame_vp8_header = decoded.vp8_header;
    }
    media->frame_key = key;
    media->frame_bytes =
        decoded.image.codec == WebpCodec::vp8_lossy ? decoded.alpha.count : decoded.rgba8.count;
    media->frame_request_id = request_id;
    media->frame_source_message_id = source_message_id;
    media->frame_source_message_revision = source_message_revision;
    capture_catalog_source(media, key);
    media->frame_kind = decoded.image.codec == WebpCodec::vp8_lossy ? AccountMediaFrameKind::vp8_keyframe
                                                                    : AccountMediaFrameKind::straight_rgba8;
    mark_frame_ready(media, key, request_id);
}

void publish_jpeg_frame(AccountMedia* media, MediaAssetKey key, const CdnStaticJpegResult& decoded,
                        u64 request_id, u64 source_message_id, u32 source_message_revision) noexcept {
    const bool reduced = decoded.rgba8.count != 0;
    media->frame_jpeg_image = decoded.image;
    if (reduced) {
        media->frame_webp_image = WebpImage{};
        media->frame_webp_image.width = decoded.rgba_width;
        media->frame_webp_image.height = decoded.rgba_height;
    }
    media->frame_key = key;
    media->frame_bytes = reduced ? decoded.rgba8.count : 0;
    media->frame_request_id = request_id;
    media->frame_source_message_id = source_message_id;
    media->frame_source_message_revision = source_message_revision;
    capture_catalog_source(media, key);
    media->frame_kind =
        reduced ? AccountMediaFrameKind::straight_rgba8 : AccountMediaFrameKind::jpeg_coefficients;
    mark_frame_ready(media, key, request_id);
}

void publish_animated_webp_frame(AccountMedia* media, MediaAssetKey key,
                                 const CdnAnimatedWebpResult& decoded, u64 request_id) noexcept {
    media->frame_source_message_id = media->active_source_message_id;
    media->frame_source_message_revision = media->active_source_message_revision;
    capture_catalog_source(media, key);
    media->frame_webp_image = decoded.decode.frame_image;
    media->frame_animated_webp = decoded.decode.frame;
    if (decoded.decode.frame.codec == AnimatedWebpCodec::vp8_lossy) {
        media->frame_vp8 = decoded.vp8_frame;
        media->frame_vp8_header = decoded.vp8_header;
    }
    media->frame_key = key;
    media->frame_bytes = decoded.decode.frame.codec == AnimatedWebpCodec::vp8_lossy ? decoded.alpha.count
                                                                                    : decoded.rgba8.count;
    media->frame_request_id = request_id;
    media->frame_duration_us = decoded.decode.duration_us;
    media->frame_index = decoded.decode.frame_index;
    media->frame_count = decoded.frame_count;
    media->frame_play_count = decoded.play_count;
    media->frame_cache_generation = decoded.cache_generation;
    media->frame_canvas_width = decoded.canvas_width;
    media->frame_canvas_height = decoded.canvas_height;
    media->frame_kind = decoded.decode.frame.codec == AnimatedWebpCodec::vp8_lossy
                            ? AccountMediaFrameKind::animated_webp_vp8
                            : AccountMediaFrameKind::animated_webp_rgba8;
    mark_frame_ready(media, key, request_id);
}

void publish_apng_frame(AccountMedia* media, MediaAssetKey key, const CdnApngResult& decoded,
                        u64 request_id) noexcept {
    media->frame_source_message_id = media->active_source_message_id;
    media->frame_source_message_revision = media->active_source_message_revision;
    capture_catalog_source(media, key);
    media->frame_image = decoded.image;
    media->frame_apng = decoded.decode.frame;
    media->frame_key = key;
    media->frame_bytes = decoded.scanlines.count;
    media->frame_request_id = request_id;
    media->frame_duration_us = decoded.decode.duration_us;
    media->frame_index = decoded.decode.frame_index;
    media->frame_count = decoded.frame_count;
    media->frame_play_count = decoded.play_count;
    media->frame_cache_generation = decoded.cache_generation;
    media->frame_canvas_width = static_cast<u16>(decoded.canvas_width);
    media->frame_canvas_height = static_cast<u16>(decoded.canvas_height);
    media->frame_kind = AccountMediaFrameKind::apng_scanlines;
    mark_frame_ready(media, key, request_id);
}

void publish_gif_frame(AccountMedia* media, MediaAssetKey key, const CdnGifResult& decoded,
                       u64 request_id) noexcept {
    media->frame_source_message_id = media->active_source_message_id;
    media->frame_source_message_revision = media->active_source_message_revision;
    capture_catalog_source(media, key);
    media->frame_gif = decoded.decode.frame;
    media->frame_gif_palette = decoded.decode.palette;
    media->frame_key = key;
    media->frame_bytes = decoded.indices.count;
    media->frame_request_id = request_id;
    media->frame_duration_us = decoded.decode.duration_us;
    media->frame_index = decoded.decode.frame_index;
    media->frame_count = decoded.frame_count;
    media->frame_play_count = decoded.play_count;
    media->frame_cache_generation = decoded.cache_generation;
    media->frame_canvas_width = static_cast<u16>(decoded.canvas_width);
    media->frame_canvas_height = static_cast<u16>(decoded.canvas_height);
    media->frame_kind = AccountMediaFrameKind::gif_indices;
    mark_frame_ready(media, key, request_id);
}

void publish_mp4_frame(AccountMedia* media, MediaAssetKey key, const Mp4DecodedFrame& decoded,
                       u64 request_id, u64 source_message_id, u32 source_message_revision,
                       u32 frame_index) noexcept {
    media->frame_key = key;
    media->frame_bytes = 0;
    media->frame_request_id = request_id;
    media->frame_source_message_id = source_message_id;
    media->frame_source_message_revision = source_message_revision;
    capture_catalog_source(media, key);
    media->frame_duration_us = decoded.duration_us;
    media->frame_index = frame_index;
    media->frame_count = kAnimationTimelineFrameMaximum;
    media->frame_play_count = 0;
    media->frame_cache_generation = media->mp4_generation;
    media->frame_canvas_width = decoded.display_width;
    media->frame_canvas_height = decoded.display_height;
    media->frame_kind = AccountMediaFrameKind::video_nv12_gpu;
    media->mp4_used_ms = clock_monotonic_milliseconds();
    mark_frame_ready(media, key, request_id);
}

bool audio_image_type(CdnImageType type) noexcept {
    return type == CdnImageType::ogg_opus || type == CdnImageType::mp3 ||
           type == CdnImageType::wav || type == CdnImageType::m4a;
}

bool publish_audio_frame(AccountMedia* media, MediaAssetKey key, u64 request_id) noexcept {
    const auto& source = media->audio_source;
    media->frame_key = key;
    media->frame_bytes = 0;
    media->frame_request_id = request_id;
    media->frame_source_message_id = source.source_message_id;
    media->frame_source_message_revision = source.source_message_revision;
    media->frame_catalog_scope = 0;
    media->frame_catalog_generation = 0;
    media->frame_duration_us = source.duration_us;
    media->frame_index = 0;
    media->frame_count = 0;
    media->frame_play_count = 1;
    media->frame_cache_generation = source.cache_generation;
    media->frame_canvas_width = 0;
    media->frame_canvas_height = 0;
    media->frame_kind = AccountMediaFrameKind::audio_source;
    mark_frame_ready(media, key, request_id);
    return true;
}

bool publish_audio_source(AccountMedia* media, MediaAssetKey key, CdnEncodedAudioResult* decoded,
                          u64 request_id) noexcept {
    if (!decoded->ready())
        return false;
    AccountMediaEncodedAudioSource source{};
    source.key = key;
    source.metadata = decoded->metadata;
    source.bytes = encoded_audio_bytes(source.metadata);
    if (decoded->state == CdnEncodedAudioState::ready_transient) {
        if (decoded->lease.acquired)
            encoded_media_cache_retire(media->pipeline.cache, key, decoded->lease.generation);
        source.transient_allocation =
            VirtualAlloc(nullptr, source.bytes.count, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!source.transient_allocation) {
            if (decoded->lease.acquired)
                encoded_media_cache_release(media->pipeline.cache, &decoded->lease);
            return false;
        }
        copy_bytes(source.transient_allocation, source.bytes.data, source.bytes.count);
        source.bytes = {static_cast<const u8*>(source.transient_allocation), source.bytes.count};
        if (!encoded_audio_rebind(&source.metadata, source.bytes)) {
            VirtualFree(source.transient_allocation, 0, MEM_RELEASE);
            if (decoded->lease.acquired)
                encoded_media_cache_release(media->pipeline.cache, &decoded->lease);
            return false;
        }
        if (decoded->lease.acquired) {
            encoded_media_cache_release(media->pipeline.cache, &decoded->lease);
        }
        source.content_identity = encoded_media_new_content_identity();
        if (++media->audio_generation == 0)
            ++media->audio_generation;
        source.cache_generation = media->audio_generation;
    } else {
        source.lease = decoded->lease;
        decoded->lease = {};
        source.content_identity = source.lease.lookup.content_identity;
        source.cache_generation = source.lease.generation;
    }
    source.source_message_id = media->active_source_message_id;
    source.source_message_revision = media->active_source_message_revision;
    source.duration_us = encoded_audio_duration_us(source.metadata);
    account_media_release_audio_source(media);
    media->audio_source = source;
    return publish_audio_frame(media, key, request_id);
}

// Reserves a codec workspace on first use and binds it to the pipeline.
template <typename Workspace, typename Bind> bool ensure_workspace(Workspace*& slot, Bind bind) noexcept {
    if (slot)
        return true;
    void* allocation = VirtualAlloc(nullptr, sizeof(Workspace), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!allocation)
        return false;
    slot = static_cast<Workspace*>(allocation);
    if (bind(slot))
        return true;
    VirtualFree(allocation, 0, MEM_RELEASE);
    slot = nullptr;
    return false;
}

bool ensure_webp_workspace(AccountMedia* media) noexcept {
    return ensure_workspace(media->webp_workspace, [media](Vp8lWorkspace* workspace) noexcept {
        return cdn_media_pipeline_bind_webp_workspace(&media->pipeline, workspace);
    });
}

static_assert(sizeof(JpegWorkspace) >= sizeof(LossyWebpWorkspace));
static_assert(is_trivially_default_constructible_v<JpegWorkspace>);
static_assert(is_trivially_default_constructible_v<LossyWebpWorkspace>);

// Both codecs finish before the worker publishes one borrowed frame. Keep one
// reservation, and commit the larger extent only when JPEG first needs it.
void* allocate_image_workspace(void* existing, usize bytes) noexcept {
    if (existing)
        return VirtualAlloc(existing, bytes, MEM_COMMIT, PAGE_READWRITE);
    if (bytes == sizeof(JpegWorkspace))
        return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* allocation = VirtualAlloc(nullptr, sizeof(JpegWorkspace), MEM_RESERVE, PAGE_READWRITE);
    if (allocation && !VirtualAlloc(allocation, bytes, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return nullptr;
    }
    return allocation;
}

bool ensure_lossy_webp_workspace(AccountMedia* media) noexcept {
    if (media->lossy_webp_workspace)
        return true;
    // These are the complete bind preconditions. Check them before storage changes.
    if (!media->pipeline.cache || !media->pipeline.scanline_storage)
        return false;
    void* allocation = media->jpeg_workspace;
    if (!allocation)
        allocation = allocate_image_workspace(nullptr, sizeof(LossyWebpWorkspace));
    if (!allocation)
        return false;
    media->lossy_webp_workspace = ::new (allocation) LossyWebpWorkspace;
    media->pipeline.lossy_webp_workspace = media->lossy_webp_workspace;
    return true;
}

struct AccountAnimatedWebpWorkspace {
    AnimatedWebpWorkspace parser;
    Arena frame_arena;
};
static_assert(kAnimatedWebpFrameCapacity == kAnimationTimelineFrameMaximum);

bool grow_animated_webp_frames(AnimatedWebpWorkspace* workspace, u32 required) noexcept {
    if (required > kAnimatedWebpFrameCapacity)
        return false;
    auto* owner = reinterpret_cast<AccountAnimatedWebpWorkspace*>(workspace);
    Arena* arena = &owner->frame_arena;
    if (!arena->base &&
        !arena_initialize(arena, static_cast<usize>(kAnimatedWebpFrameCapacity) * sizeof(AnimatedWebpFrame),
                          0, 4096))
        return false;
    const u32 capacity =
        minimum(kAnimatedWebpFrameCapacity,
                maximum(required,
                        maximum(kAnimatedWebpInlineFrameCapacity, workspace->extended_frames.count) * 2u));
    // This arena contains only the contiguous frame table. Append storage in
    // place; previously parsed controls never move or need to be copied again.
    if (!arena_allocate<AnimatedWebpFrame>(arena, capacity - workspace->extended_frames.count))
        return false;
    workspace->extended_frames = {reinterpret_cast<AnimatedWebpFrame*>(arena->base), capacity};
    return true;
}

bool ensure_animated_webp_workspace(AccountMedia* media) noexcept {
    if (media->animated_webp_workspace) {
        return true;
    }
    void* allocation = VirtualAlloc(nullptr, sizeof(AccountAnimatedWebpWorkspace), MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE);
    if (!allocation) {
        return false;
    }
    media->animated_webp_workspace = static_cast<AnimatedWebpWorkspace*>(allocation);
    media->animated_webp_workspace->grow_frames = grow_animated_webp_frames;
    if (!cdn_media_pipeline_bind_animated_webp_workspace(&media->pipeline,
                                                         media->animated_webp_workspace)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        media->animated_webp_workspace = nullptr;
        return false;
    }
    return true;
}

bool ensure_apng_workspace(AccountMedia* media) noexcept {
    return ensure_workspace(media->apng_workspace, [media](ApngWorkspace* workspace) noexcept {
        return cdn_media_pipeline_bind_apng_workspace(&media->pipeline, workspace);
    });
}

bool ensure_gif_workspace(AccountMedia* media) noexcept {
    return ensure_workspace(media->gif_workspace, [media](GifWorkspace* workspace) noexcept {
        return cdn_media_pipeline_bind_gif_workspace(&media->pipeline, workspace);
    });
}

bool ensure_jpeg_workspace(AccountMedia* media) noexcept {
    if (!media->jpeg_workspace) {
        if (!media->pipeline.cache || !media->pipeline.scanline_storage)
            return false;
        void* allocation = allocate_image_workspace(media->lossy_webp_workspace, sizeof(JpegWorkspace));
        if (!allocation)
            return false;
        media->jpeg_workspace = static_cast<JpegWorkspace*>(allocation);
    }
    // Direct thumbnail parsing also enters here. Default construction starts the
    // JPEG lifetime without clearing bytes. The parser writes every live field.
    media->jpeg_workspace = ::new (media->jpeg_workspace) JpegWorkspace;
    media->pipeline.jpeg_workspace = media->jpeg_workspace;
    return true;
}

bool ensure_mp4_decoder(AccountMedia* media) noexcept {
    if (!media->mp4_decoder)
        media->mp4_decoder = &media->owned_mp4_decoder;
    return true;
}
AccountMediaRequestResult video_failure(const Mp4Decoder* decoder) noexcept {
    const bool permanent = decoder && decoder->native.parse_error != H264Error::none;
    auto failure =
        result(permanent ? AccountMediaRequestState::unsupported : AccountMediaRequestState::failed,
               AccountMediaError::decode);
    if (decoder && decoder->native.demux_error != Mp4DemuxError::none) {
        switch (decoder->native.demux_error) {
        case Mp4DemuxError::unsupported:
            failure.video_failure = AccountMediaVideoFailure::demux_unsupported;
            break;
        case Mp4DemuxError::no_video:
            failure.video_failure = AccountMediaVideoFailure::demux_no_video;
            break;
        default:
            failure.video_failure = AccountMediaVideoFailure::demux_malformed;
            break;
        }
        if (permanent)
            failure.retry_at_ms = ~0ull;
        return failure;
    }
    if (decoder) {
        switch (decoder->native.parse_error) {
        case H264Error::invalid_input:
            failure.video_failure = AccountMediaVideoFailure::invalid_input;
            break;
        case H264Error::truncated:
            failure.video_failure = AccountMediaVideoFailure::truncated_stream;
            break;
        case H264Error::malformed:
            failure.video_failure = AccountMediaVideoFailure::malformed_stream;
            break;
        case H264Error::unsupported:
            failure.video_failure = AccountMediaVideoFailure::unsupported_stream;
            break;
        default:
            const i32 native_status = decoder->native.status;
            failure.video_failure =
                native_status == E_OUTOFMEMORY            ? AccountMediaVideoFailure::memory
                : native_status == DXGI_ERROR_UNSUPPORTED ? AccountMediaVideoFailure::unsupported_backend
                : native_status == DXGI_ERROR_DEVICE_REMOVED || native_status == DXGI_ERROR_DEVICE_RESET
                    ? AccountMediaVideoFailure::device_removed
                    : AccountMediaVideoFailure::native_failure;
            break;
        }
    }
    if (permanent)
        failure.retry_at_ms = ~0ull;
    return failure;
}

AccountMediaRequestResult ensure_client_started(AccountMedia* media) noexcept {
    if (media->client_started) {
        return result(AccountMediaRequestState::ready);
    }
    const CdnClientResult started =
        cdn_client_start(&media->client, kMediaMaximumEncodedVideoBytes);
    if (!started.ok()) {
        AccountMediaRequestResult failure =
            result(AccountMediaRequestState::failed, AccountMediaError::client);
        failure.client_error = started.error;
        return failure;
    }
    media->client_started = true;
    return result(AccountMediaRequestState::ready);
}

// A new decoder lifetime. The decoder holds one source; its identity distinguishes accepted source
// lifetimes and survives selection, replay, and seek.
void adopt_mp4_source(AccountMedia* media) noexcept {
    if (++media->mp4_generation == 0)
        ++media->mp4_generation;
    media->mp4_key = media->active_key;
    media->mp4_source_message_id = media->active_source_message_id;
    media->mp4_source_message_revision = media->active_source_message_revision;
    media->mp4_content_identity = encoded_media_new_content_identity();
}

bool submit_progressive(AccountMedia* media, CdnByteRange range, ProgressiveVideoFetch* selected = nullptr) noexcept {
    auto* fetch = selected ? selected : media->progressive;
    if (!fetch) return false;
    if (++media->request_id == 0)
        ++media->request_id;
    const auto submitted = cdn_client_submit(
        &media->client,
        CdnClientRequest{{fetch->target, fetch->target_bytes},
                         {},
                         media->request_id,
                         kCdnClientDefaultTimeoutMs,
                         fetch->host,
                         fetch->image_type,
                         range});
    if (!submitted.ok())
        return false;
    progressive_video_fetch_submitted(fetch, media->request_id, range);
    media->active_request_id = media->request_id;
    return true;
}

// Drives the head probe, moov probe and first frame. True while a range is in
// flight; false once the poster is published or a failure was recorded.
bool continue_progressive(AccountMedia* media, u64 now_ms, u64 unix_now_seconds) noexcept {
    auto* fetch = media->progressive;
    const auto failure = [&](AccountMediaRequestResult value) noexcept {
        remember_failure(media, media->active_key, now_ms, value);
        return false;
    };
    for (u32 step = 0; step < 4; ++step) {
        ProgressiveVideoFetch* selected{};
        const auto next = account_media_progressive_next(media, false, unix_now_seconds, &selected);
        if (next.step == ProgressiveVideoStep::submit)
            return submit_progressive(media, next.range, selected) ||
                   failure(result(AccountMediaRequestState::failed, AccountMediaError::client));
        if (next.step == ProgressiveVideoStep::refresh_required) {
            // Not a cooldown: the owner refreshes the signed URL and asks again.
            auto refresh = result(AccountMediaRequestState::refresh_required);
            refresh.retry_at_ms = deadline_after(now_ms, 1'000);
            media->last_failure = refresh;
            return false;
        }
        if (next.step == ProgressiveVideoStep::failed) {
            // Unsupported layouts (fragmented, oversized moov) and exhausted
            // refreshes alike retry after the normal cooldown.
            auto value = result(AccountMediaRequestState::failed, AccountMediaError::decode);
            value.video_failure = AccountMediaVideoFailure::demux_unsupported;
            return failure(value);
        }
        if (next.step == ProgressiveVideoStep::ready || fetch->state != ProgressiveVideoFetchState::ready)
            continue;
        if (fetch->kind == ProgressiveVideoKind::fragmented_audio) {
            Mp4AudioSampleWindow window{};
            if (!mp4_audio_sample_window(fetch->audio.timeline, 0, &window))
                return failure(result(AccountMediaRequestState::failed, AccountMediaError::decode));
            account_media_release_audio_source(media);
            auto& source = media->audio_source;
            source.key = media->active_key;
            source.content_identity = encoded_media_new_content_identity();
            if (!++media->audio_generation) ++media->audio_generation;
            source.cache_generation = media->audio_generation;
            source.source_message_id = media->active_source_message_id;
            source.source_message_revision = media->active_source_message_revision;
            source.duration_us = static_cast<u64>(window.duration_100ns / 10);
            publish_audio_frame(media, source.key, media->active_request_id);
            return false;
        }
        const auto bytes = progressive_source_bytes(fetch->source);
        const auto reader = progressive_source_reader(&fetch->source, ProgressiveTrack::video);
        if (!media->mp4_decoder || !media->mp4_decoder->ready) {
            if (fetch->kind != ProgressiveVideoKind::movie) {
                if (!ensure_mp4_decoder(media)) return failure(video_failure(media->mp4_decoder));
                if (!mp4_decoder_initialize_fragmented(media->mp4_decoder, &fetch->fragment, media->video_device)) {
                    if (media->mp4_decoder->native.demux_error == Mp4DemuxError::data_pending) continue;
                    return failure(video_failure(media->mp4_decoder));
                }
                adopt_mp4_source(media);
            } else {
            // The decoder configuration reads the first sample; fetch it first.
            Mp4Demuxer first{};
            const auto error = hevc_container_present(bytes) ? hevc_demux_initialize(&first, bytes, reader)
                                                             : mp4_demux_initialize(&first, bytes, reader);
            if (error != Mp4DemuxError::none) {
                auto value = video_failure(nullptr);
                value.state = AccountMediaRequestState::unsupported;
                value.video_failure = error == Mp4DemuxError::no_video ? AccountMediaVideoFailure::demux_no_video
                                      : error == Mp4DemuxError::unsupported
                                          ? AccountMediaVideoFailure::demux_unsupported
                                          : AccountMediaVideoFailure::demux_malformed;
                value.retry_at_ms = ~0ull;
                return failure(value);
            }
            progressive_source_set_duration(&fetch->source, first.duration_us);
            if (mp4_demux_next_ready(&first).error == Mp4DemuxError::data_pending)
                continue;
            if (!ensure_mp4_decoder(media) ||
                !mp4_decoder_initialize(media->mp4_decoder, bytes, media->active_proxy_width,
                                        media->active_proxy_height, media->video_device, reader))
                return failure(video_failure(media->mp4_decoder));
            adopt_mp4_source(media);
            }
        }
        const Mp4DecodedFrame decoded = mp4_decoder_read(media->mp4_decoder);
        if (decoded.error == Mp4DecodeError::needs_service) { media->progressive_continue = true; return true; }
        if (decoded.error == Mp4DecodeError::data_pending)
            continue;
        if (!decoded.ok())
            return failure(video_failure(media->mp4_decoder));
        account_media_cache_progressive(media, media->active_key);
        publish_mp4_frame(media, media->active_key, decoded, media->active_request_id,
                          media->active_source_message_id, media->active_source_message_revision, 0);
        return false;
    }
    return failure(result(AccountMediaRequestState::failed, AccountMediaError::decode));
}

struct ConsumeContext {
    AccountMedia* media;
    u64 now_ms;
};

// Lottie stickers and regional flags publish one worker-compiled paint
// program from a single lazily reserved workspace; the encoded bytes stay
// with the caller (response body or cache lease).
bool publish_vector(AccountMedia* media, MediaAssetKey key, ConstSlice<u8> encoded, u64 request_id,
                    u32 frame_index = 0) noexcept {
    const bool lottie = lottie_asset(key);
    if (!supported_key(key) || (!lottie && key.kind != MediaAssetKind::regional_flag) || !encoded.data ||
        encoded.count > (lottie ? kLottieMaximumBytes : kRegionalFlagSvgMaximumBytes))
        return false;
    // Both vector kinds compile here into the shared paint workspace; the
    // client only rasterizes the borrowed program.
    const auto workspace = +[](void* context) noexcept -> ColrPaintProgram* {
        auto* media = static_cast<AccountMedia*>(context);
        if (!media->lottie_workspace)
            media->lottie_workspace =
                VirtualAlloc(nullptr, sizeof(ColrPaintProgram), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        return static_cast<ColrPaintProgram*>(media->lottie_workspace);
    };
    LottieTiming timing{};
    if (lottie) {
        if (!lottie_program_compile_frame(encoded, frame_index, 128, &timing, workspace, media))
            return false;
    } else {
        RegionalFlagSvg image{};
        ColrPaintProgram* program = nullptr;
        if (!regional_flag_svg_parse(encoded, &image) || !(program = workspace(media)) ||
            !regional_flag_svg_compile(image, program, kRegionalFlagPixelsPerUnit))
            return false;
    }
    media->frame_key = key;
    media->frame_bytes = sizeof(ColrPaintProgram);
    media->frame_request_id = request_id;
    media->frame_source_message_id = 0;
    media->frame_source_message_revision = 0;
    capture_catalog_source(media, key);
    media->frame_duration_us = timing.duration_us;
    media->frame_index = frame_index;
    media->frame_count = lottie ? timing.frames : 0;
    media->frame_play_count = 0;
    media->frame_cache_generation = 0;
    media->frame_canvas_width = lottie ? 128 : 0;
    media->frame_canvas_height = lottie ? 128 : 0;
    media->frame_webp_image = {};
    media->frame_webp_image.width = media->frame_webp_image.height = lottie ? 128 : 64;
    media->frame_kind =
        lottie ? AccountMediaFrameKind::lottie_paint : AccountMediaFrameKind::regional_flag_paint;
    mark_frame_ready(media, key, request_id);
    return true;
}

// Separate frame so the inflate scratch only occupies stack on this path.
__declspec(noinline) bool publish_bundled_flag(AccountMedia* media, MediaAssetKey key) noexcept {
    u8 scratch[kRegionalFlagSvgMaximumBytes];
    return publish_vector(media, key,
                          regional_flag_bundled_svg(static_cast<u16>(key.id), {scratch, sizeof(scratch)}), 0);
}

bool consume_response(void* opaque, const CdnClientResponse& response,
                      ConstSlice<u8> body) noexcept {
    auto* context = static_cast<ConsumeContext*>(opaque);
    AccountMedia* media = context->media;
    if (!media->active || response.request_id != media->active_request_id) {
        return true;
    }
    const auto fail = [&](const AccountMediaRequestResult& failure) noexcept {
        remember_failure(media, media->active_key, context->now_ms, failure);
        return true;
    };
    if (media->progressive_active) {
        const auto policy = cdn_client_response_policy(response);
        const auto accepted = account_media_accept_progressive(media, response, body);
        if (accepted == ProgressiveVideoAccept::progress && policy.range_first == 0 &&
            policy.disposition == CdnResponseDisposition::accept_partial) {
            const auto body_failure = account_media_video_body_failure(body);
            if (body_failure != AccountMediaVideoFailure::none) {
                auto failure = result(AccountMediaRequestState::unsupported, AccountMediaError::decode);
                failure.video_failure = body_failure;
                return fail(failure);
            }
        }
        if (accepted == ProgressiveVideoAccept::progress || accepted == ProgressiveVideoAccept::ignored ||
            accepted == ProgressiveVideoAccept::refresh_required) {
            media->progressive_continue = true;
            return true;
        }
        if (accepted == ProgressiveVideoAccept::failed) {
            auto failure = result(AccountMediaRequestState::failed,
                                  response.transport_ok() ? AccountMediaError::http : AccountMediaError::client);
            failure.client_error = response.error;
            failure.http_error = response.http_error;
            return fail(failure);
        }
        // The server ignored Range and sent the whole bounded entity.
    }
    if (!response.transport_ok()) {
        AccountMediaRequestResult failure =
            result(AccountMediaRequestState::failed, AccountMediaError::client);
        failure.client_error = response.error;
        failure.http_error = response.http_error;
        return fail(failure);
    }
    const CdnResponseResult policy = cdn_client_response_policy(response);
    const ConstSlice<u8> conditional_etag{response.conditional_etag, response.conditional_etag_bytes};
    if (media->active_key.kind == MediaAssetKind::regional_flag || lottie_asset(media->active_key)) {
        bool published = false;
        if (policy.status_code == 200 && policy.body_bytes == body.count &&
            body.count <= cdn_encoded_body_limit(media->active_image_type)) {
            published = publish_vector(media, media->active_key, body, response.request_id);
            if (published) {
                const auto stored = encoded_media_cache_store(
                    media->pipeline.cache, media->active_key, body,
                    {policy.etag, policy.expires_at_seconds, policy.store_allowed, policy.must_revalidate});
                if (lottie_asset(media->active_key) && stored.stored())
                    media->frame_cache_generation = stored.generation;
            }
        } else if (policy.status_code == 304 && !body.count && conditional_etag.count) {
            auto lease = encoded_media_cache_acquire(media->pipeline.cache, media->active_key, 0);
            bool matches = lease.acquired && lease.lookup.etag.count == conditional_etag.count;
            for (u32 i = 0; matches && i < conditional_etag.count; ++i)
                matches = lease.lookup.etag[i] == conditional_etag[i];
            if (matches)
                published =
                    publish_vector(media, media->active_key, lease.lookup.bytes, response.request_id);
            if (published && lottie_asset(media->active_key) && policy.store_allowed)
                media->frame_cache_generation = lease.lookup.generation;
            if (lease.acquired)
                encoded_media_cache_release(media->pipeline.cache, &lease);
            if (published) {
                if (policy.store_allowed)
                    encoded_media_cache_refresh(media->pipeline.cache, media->active_key,
                                                policy.expires_at_seconds, policy.must_revalidate,
                                                policy.etag);
                else
                    encoded_media_cache_erase(media->pipeline.cache, media->active_key);
            }
        }
        if (!published)
            fail(result(AccountMediaRequestState::failed, AccountMediaError::decode));
    } else if (audio_image_type(media->active_image_type)) {
        auto decoded = cdn_media_pipeline_admit_audio(&media->pipeline, media->active_key, policy, body,
                                                      conditional_etag, media->audio_cancelled,
                                                      media->audio_cancel_context);
        if (!decoded.ready() ||
            !publish_audio_source(media, media->active_key, &decoded, response.request_id)) {
            auto failure =
                result(AccountMediaRequestState::failed, !policy.ok()      ? AccountMediaError::http
                                                         : decoded.ready() ? AccountMediaError::memory
                                                                           : AccountMediaError::decode);
            failure.http_error = policy.error;
            fail(failure);
        }
    } else if (video_asset(media->active_key) && media->active_image_type == CdnImageType::mp4) {
        const bool catalog = media->active_key.kind == MediaAssetKind::catalog_preview;
        const auto body_failure = account_media_video_body_failure(body);
        // A picker tile decodes at most an image-sized source.
        if (policy.ok() &&
            (body_failure != AccountMediaVideoFailure::none ||
             (catalog && body.count > kAccountMediaCatalogVideoMaximumBytes))) {
            auto failure = result(AccountMediaRequestState::unsupported, AccountMediaError::decode);
            failure.video_failure = body_failure;
            return fail(failure);
        }
        account_media_release_video(media);
        if (!policy.ok() || !ensure_mp4_decoder(media) ||
            !mp4_decoder_initialize(media->mp4_decoder, body, media->active_proxy_width,
                                    media->active_proxy_height, media->video_device)) {
            AccountMediaRequestResult failure =
                policy.ok() ? video_failure(media->mp4_decoder)
                            : result(AccountMediaRequestState::failed, AccountMediaError::http);
            failure.http_error = policy.error;
            return fail(failure);
        }
        adopt_mp4_source(media);
        const Mp4DecodedFrame decoded = mp4_decoder_read(media->mp4_decoder);
        if (!decoded.ok())
            return fail(video_failure(media->mp4_decoder));
        // Small picker tile sources stay in the shared encoded cache so a
        // reopened picker restarts them without another download.
        if (catalog && policy.store_allowed && body.count <= kAccountMediaProgressiveCacheMaximumBytes)
            encoded_media_cache_store(
                media->pipeline.cache, media->active_key, body,
                {policy.etag, policy.expires_at_seconds, policy.store_allowed, policy.must_revalidate});
        publish_mp4_frame(media, media->active_key, decoded, response.request_id,
                          media->active_source_message_id, media->active_source_message_revision, 0);
    } else if ((media->active_key.kind == MediaAssetKind::attachment ||
                (media->active_key.kind == MediaAssetKind::embed ||
                 media->active_key.kind == MediaAssetKind::profile_image)) &&
               media->active_image_type == CdnImageType::jpeg) {
        CdnStaticJpegResult decoded = cdn_media_pipeline_admit_static_jpeg(
            &media->pipeline, media->active_key, policy, body, conditional_etag);
        if (decoded.state == CdnStaticJpegState::jpeg_workspace_required && ensure_jpeg_workspace(media)) {
            decoded = cdn_media_pipeline_admit_static_jpeg(&media->pipeline, media->active_key, policy,
                                                           body, conditional_etag);
        }
        if (!decoded.decoded()) {
            const bool workspace_failed = decoded.state == CdnStaticJpegState::jpeg_workspace_required;
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, workspace_failed ? AccountMediaError::memory
                                                         : policy.ok()    ? AccountMediaError::decode
                                                                          : AccountMediaError::http);
            failure.http_error = policy.error;
            failure.jpeg_error = decoded.decode.error;
            return fail(failure);
        }
        publish_jpeg_frame(media, media->active_key, decoded, response.request_id,
                           media->active_source_message_id, media->active_source_message_revision);
    } else if (webp_asset(media->active_key) && animated_asset(media->active_key)) {
        CdnAnimatedWebpResult decoded = cdn_media_pipeline_admit_animated_webp(
            &media->pipeline, media->active_key, policy, body, conditional_etag);
        if (decoded.state == CdnAnimatedWebpState::lossy_workspace_required &&
            ensure_lossy_webp_workspace(media)) {
            decoded = cdn_media_pipeline_admit_animated_webp(&media->pipeline, media->active_key, policy,
                                                             body, conditional_etag);
        }
        if (!decoded.decoded()) {
            const bool workspace_failed = decoded.state == CdnAnimatedWebpState::lossy_workspace_required;
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, workspace_failed ? AccountMediaError::memory
                                                         : policy.ok()    ? AccountMediaError::decode
                                                                          : AccountMediaError::http);
            failure.http_error = policy.error;
            failure.webp_error = decoded.decode.frame_decode.error;
            failure.animated_webp_error = decoded.decode.error;
            failure.lossy_webp_error = decoded.decode.lossy_decode.error;
            return fail(failure);
        }
        publish_animated_webp_frame(media, media->active_key, decoded, response.request_id);
    } else if (webp_asset(media->active_key) ||
               ((media->active_key.kind == MediaAssetKind::attachment ||
                 (media->active_key.kind == MediaAssetKind::embed ||
                  media->active_key.kind == MediaAssetKind::profile_image)) &&
                media->active_image_type == CdnImageType::webp)) {
        CdnStaticWebpResult decoded = cdn_media_pipeline_admit_static_webp(
            &media->pipeline, media->active_key, policy, body, conditional_etag);
        if (decoded.state == CdnStaticWebpState::lossy_workspace_required &&
            ensure_lossy_webp_workspace(media)) {
            decoded = cdn_media_pipeline_admit_static_webp(&media->pipeline, media->active_key, policy,
                                                           body, conditional_etag);
        }
        if (!decoded.decoded()) {
            const bool workspace_failed = decoded.state == CdnStaticWebpState::lossy_workspace_required;
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, workspace_failed ? AccountMediaError::memory
                                                         : policy.ok()    ? AccountMediaError::decode
                                                                          : AccountMediaError::http);
            failure.http_error = policy.error;
            failure.webp_error = decoded.decode.error;
            failure.lossy_webp_error = decoded.lossy_decode.error;
            return fail(failure);
        }
        publish_webp_frame(media, media->active_key, decoded, response.request_id,
                           media->active_source_message_id, media->active_source_message_revision);
    } else if (apng_asset(media->active_key) ||
               (catalog_png_key(media->active_key) &&
                (png_animation_body(body) ||
                 (response.status_code == 304 &&
                  catalog_png_cached_animation(media, media->active_key, 0))))) {
        if (!ensure_apng_workspace(media))
            return fail(result(AccountMediaRequestState::failed, AccountMediaError::memory));
        const CdnApngResult decoded = cdn_media_pipeline_admit_apng(&media->pipeline, media->active_key,
                                                                    policy, body, conditional_etag);
        if (!decoded.decoded()) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed,
                       policy.ok() ? AccountMediaError::decode : AccountMediaError::http);
            failure.http_error = policy.error;
            failure.apng_error = decoded.decode.error;
            return fail(failure);
        }
        publish_apng_frame(media, media->active_key, decoded, response.request_id);
    } else if (gif_asset(media->active_key)) {
        const CdnGifResult decoded = cdn_media_pipeline_admit_gif(&media->pipeline, media->active_key,
                                                                  policy, body, conditional_etag);
        if (!decoded.decoded()) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed,
                       policy.ok() ? AccountMediaError::decode : AccountMediaError::http);
            failure.http_error = policy.error;
            failure.gif_error = decoded.decode.error;
            return fail(failure);
        }
        publish_gif_frame(media, media->active_key, decoded, response.request_id);
    } else {
        const CdnStaticPngResult decoded = cdn_media_pipeline_admit_static_png(
            &media->pipeline, media->active_key, policy, body, conditional_etag);
        if (!decoded.decoded()) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed,
                       policy.ok() ? AccountMediaError::decode : AccountMediaError::http);
            failure.http_error = policy.error;
            failure.png_error = decoded.decode.error;
            return fail(failure);
        }
        publish_png_frame(media, media->active_key, decoded, response.request_id,
                          media->active_source_message_id, media->active_source_message_revision);
    }
    return true;
}

Slice<u8> grow_scanlines(void* owner, u32 bytes) noexcept {
    auto* media = static_cast<AccountMedia*>(owner);
    if (!bytes || bytes > kPngDecodeBufferMaximumBytes || media->frame_ready)
        return {};
    const u32 capacity = (bytes + 4095u) & ~4095u;
    auto* replacement =
        static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!replacement)
        return {};
    void* previous = media->expanded_scanlines;
    media->expanded_scanlines = replacement;
    // Install the replacement before release. All later decodes use this output.
    media->pipeline.scanline_storage = replacement;
    media->pipeline.scanline_capacity = capacity;
    if (previous) {
        VirtualFree(previous, 0, MEM_RELEASE);
    } else if (media->allocation) {
        if (media->owns_cache) {
            static_assert((kOwnedAllocationBytes - kScanlineStorageBytes) % 4096u == 0);
            static_assert(kScanlineStorageBytes % 4096u == 0);
            auto* unused =
                static_cast<u8*>(media->allocation) + kOwnedAllocationBytes - kScanlineStorageBytes;
            VirtualFree(unused, kScanlineStorageBytes, MEM_DECOMMIT);
        } else if (VirtualFree(media->allocation, 0, MEM_RELEASE)) {
            media->allocation = nullptr;
        }
    }
    return {replacement, capacity};
}
// The lane's own scanline storage: the tail of the owned allocation, or a
// separate one when the encoded cache is shared.
u8* base_scanlines(AccountMedia* media) noexcept {
    if (media->owns_cache) {
        auto* base = static_cast<u8*>(media->allocation) + kOwnedAllocationBytes - kScanlineStorageBytes;
        return VirtualAlloc(base, kScanlineStorageBytes, MEM_COMMIT, PAGE_READWRITE) ? base : nullptr;
    }
    if (!media->allocation)
        media->allocation = VirtualAlloc(nullptr, kScanlineStorageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return static_cast<u8*>(media->allocation);
}

AccountMediaRequestResult initialize_media(AccountMedia* media, EncodedMediaCache* shared_cache) noexcept {
    if (!media || (shared_cache && (!shared_cache->entries || !shared_cache->storage))) {
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    SecureZeroMemory(media, sizeof(*media));
    const u32 allocation_bytes = shared_cache ? kScanlineStorageBytes : kOwnedAllocationBytes;
    media->allocation = VirtualAlloc(nullptr, allocation_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!media->allocation) {
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    }
    auto* storage = static_cast<u8*>(media->allocation);
    EncodedMediaCache* cache = shared_cache;
    u8* scanlines = storage;
    if (!shared_cache) {
        auto* entries = reinterpret_cast<EncodedMediaCacheEntry*>(storage);
        auto* page_owners = reinterpret_cast<u16*>(storage + kCacheEntryStorageBytes);
        auto* index_buckets =
            reinterpret_cast<u32*>(storage + kCacheEntryStorageBytes + kCachePageOwnerBytes);
        u8* encoded_storage = storage + kCacheEntryStorageBytes + kCachePageOwnerBytes + kCacheIndexBytes;
        scanlines = encoded_storage + kCacheStorageBytes;
        if (!encoded_media_cache_initialize(
                &media->cache, Slice<EncodedMediaCacheEntry>{entries, kAccountMediaCacheEntryCapacity},
                Slice<u16>{page_owners, kAccountMediaCachePageCapacity},
                Slice<u8>{encoded_storage, kCacheStorageBytes},
                Slice<u32>{index_buckets, kAccountMediaCacheIndexCapacity})) {
            VirtualFree(media->allocation, 0, MEM_RELEASE);
            SecureZeroMemory(media, sizeof(*media));
            return result(AccountMediaRequestState::failed, AccountMediaError::cache);
        }
        cache = &media->cache;
        media->owns_cache = true;
    }
    if (!cdn_media_pipeline_initialize(&media->pipeline, cache,
                                       Slice<u8>{scanlines, kScanlineStorageBytes})) {
        VirtualFree(media->allocation, 0, MEM_RELEASE);
        SecureZeroMemory(media, sizeof(*media));
        return result(AccountMediaRequestState::failed, AccountMediaError::cache);
    }
    media->pipeline.scanline_owner = media;
    media->pipeline.grow_scanlines = grow_scanlines;
    media->started = true;
    return result(AccountMediaRequestState::ready);
}

} // namespace

bool account_media_trim_scanlines(AccountMedia* media) noexcept {
    if (!media || !media->started || !media->expanded_scanlines || media->frame_ready)
        return false;
    u8* base = base_scanlines(media);
    if (!base)
        return false;
    VirtualFree(media->expanded_scanlines, 0, MEM_RELEASE);
    media->expanded_scanlines = nullptr;
    media->pipeline.scanline_storage = base;
    media->pipeline.scanline_capacity = kScanlineStorageBytes;
    return true;
}

bool account_media_video_matches(const AccountMedia* media, MediaAssetKey key,
                                 u16 cache_generation) noexcept {
    return media && media->mp4_decoder && media->mp4_decoder->ready && video_asset(key) &&
           (!media->progressive || media->progressive->state != ProgressiveVideoFetchState::failed) &&
           media_asset_key_equal(media->mp4_key, key) &&
           (cache_generation == 0 || media->mp4_generation == cache_generation);
}

bool account_media_audio_matches(const AccountMedia* media, MediaAssetKey key,
                                 u16 cache_generation) noexcept {
    const bool streaming = media && media->progressive &&
        media->progressive->kind == ProgressiveVideoKind::fragmented_audio &&
        media->progressive->state == ProgressiveVideoFetchState::ready;
    return media && (streaming || (encoded_audio_ready(media->audio_source.metadata) && media->audio_source.bytes.data)) &&
           media->audio_source.content_identity &&
           media_asset_key_valid(key) && media_attachment_format_audio_supported(media_asset_audio_format(key)) &&
           media_asset_key_equal(media->audio_source.key, key) &&
           (!cache_generation || media->audio_source.cache_generation == cache_generation);
}

AccountMediaCollectionState account_media_collection_state(const AccountMedia* media, MediaAssetKey key, bool ready) noexcept {
    if (!media || !media->collection || !media_asset_key_equal(media->collection->key, key) ||
        (ready && !account_media_audio_matches(media, key, 0) && !account_media_video_matches(media, key, 0))) return {};
    const auto& collection = *media->collection;
    return {collection.count, collection.selected ? collection.selected : collection.failed_track ? collection.failed_track : collection.initial, collection.continuation_bytes != 0};
}
bool account_media_collection_title(AccountMedia* media, const AccountMediaCollectionTitleRequest& request,
    MediaTrackTitle* title, MediaTrackId* id, u64* retry_at_ms) noexcept {
    auto* queue = media ? media->collection : nullptr;
    if (!queue || !title || !id || !retry_at_ms || !request.track || request.track > queue->count ||
        queue->revision != request.source_revision || !media_asset_key_equal(queue->key, request.key)) return 0;
    *title = {};
    const auto& track = reinterpret_cast<const CollectionTrack*>(queue + 1)[request.track - 1];
    *retry_at_ms = track.retry_at_ms;
    title->bytes = track.title_bytes;
    if (title->bytes) copy_bytes(title->text, queue->titles.data + track.title_offset, title->bytes);
    *id = track.id;
    return true;
}

bool account_media_collection_title_batch(AccountMedia* media, const AccountMediaCollectionTitleRequest& request,
    SoundcloudTitleBatch* out) noexcept {
    MediaTrackTitle title{}; MediaTrackId id{}; u64 retry{};
    if (!out || !account_media_collection_title(media,request,&title,&id,&retry) || id.youtube_track || id.apple_track) return false;
    const auto& queue=*media->collection;
    if (!queue.client[0]) return false;
    *out={}; copy_bytes(out->client,queue.client,sizeof(out->client));
    const auto* records=reinterpret_cast<const CollectionTrack*>(&queue+1);
    const u32 first=(request.track-1)/50*50, end=minimum<u32>(queue.count,first+50);
    const u64 now=clock_monotonic_milliseconds();
    for (u32 i=first;i<end;++i) if (!records[i].title_bytes && records[i].retry_at_ms<=now)
        out->tracks[out->count++]={records[i].id.numeric,static_cast<u16>(i+1)};
    return out->count!=0;
}
bool account_media_collection_title_store(AccountMedia* media, const AccountMediaCollectionTitleRequest& request,
    const MediaTrackId& track_id, const MediaTrackTitle& title, u64 retry_at_ms) noexcept {
    MediaTrackTitle ignored{}; MediaTrackId id{}; u64 retry{};
    if (title.bytes > sizeof(title.text) ||
        !account_media_collection_title(media, request, &ignored, &id, &retry) || !media_track_id_equal(id, track_id)) return false;
    auto& queue = *media->collection;
    auto& track = reinterpret_cast<CollectionTrack*>(&queue + 1)[request.track - 1];
    if (!track.title_bytes && title.bytes) {
        if (!collection_text_reserve(queue.titles, queue.titles.bytes + title.bytes)) return false;
        track.title_offset = queue.titles.bytes; track.title_bytes = title.bytes;
        copy_bytes(queue.titles.data + queue.titles.bytes, title.text, title.bytes); queue.titles.bytes += title.bytes;
    }
    track.retry_at_ms = title.bytes ? 0 : retry_at_ms;
    return true;
}
void account_media_release_audio_source(AccountMedia* media) noexcept {
    if (!media)
        return;
    if (media->audio_source.lease.acquired)
        encoded_media_cache_release(media->pipeline.cache, &media->audio_source.lease);
    if (media->audio_source.transient_allocation)
        VirtualFree(media->audio_source.transient_allocation, 0, MEM_RELEASE);
    media->audio_source = {};
}

AccountMediaVideoFailure account_media_video_body_failure(ConstSlice<u8> body) noexcept {
    if (!body.data)
        return AccountMediaVideoFailure::none;
    if (body.count >= 6 && body[0] == 'G' && body[1] == 'I' && body[2] == 'F' && body[3] == '8' &&
        (body[4] == '7' || body[4] == '9') && body[5] == 'a')
        return AccountMediaVideoFailure::unexpected_gif;
    if (body.count >= 8 && body[0] == 0x89 && body[1] == 'P' && body[2] == 'N' && body[3] == 'G' &&
        body[4] == 0x0d && body[5] == 0x0a && body[6] == 0x1a && body[7] == 0x0a)
        return AccountMediaVideoFailure::unexpected_png;
    if (body.count >= 12 && body[0] == 'R' && body[1] == 'I' && body[2] == 'F' && body[3] == 'F' &&
        body[8] == 'W' && body[9] == 'E' && body[10] == 'B' && body[11] == 'P')
        return AccountMediaVideoFailure::unexpected_webp;
    if (body.count >= 3 && body[0] == 0xff && body[1] == 0xd8 && body[2] == 0xff)
        return AccountMediaVideoFailure::unexpected_jpeg;
    if (body.count >= 12 && body[4] == 'f' && body[5] == 't' && body[6] == 'y' && body[7] == 'p' &&
        body[8] == 'a' && body[9] == 'v' && body[10] == 'i' && (body[11] == 'f' || body[11] == 's'))
        return AccountMediaVideoFailure::unexpected_avif;
    return AccountMediaVideoFailure::none;
}

AccountMediaRequestResult account_media_initialize(AccountMedia* media) noexcept {
    return initialize_media(media, nullptr);
}

AccountMediaRequestResult account_media_initialize_shared(AccountMedia* media,
                                                          EncodedMediaCache* shared_cache) noexcept {
    return initialize_media(media, shared_cache);
}

AccountMediaRequestResult account_media_request_static_asset(AccountMedia* media, MediaAssetKey key,
                                                             u64 monotonic_now_ms, u64 unix_now_seconds,
                                                             Utf8View guild_icon_hash, u32 identity_size) noexcept {
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
    if (media->frame_ready) {
        return result(media_asset_key_equal(media->frame_key, key) ? AccountMediaRequestState::ready
                                                      : AccountMediaRequestState::busy);
    }
    if (media->active) {
        return result(media_asset_key_equal(media->active_key, key) ? AccountMediaRequestState::pending
                                                       : AccountMediaRequestState::busy);
    }
    const AccountMediaFailure* failed = find_failure(media, key);
    if (failed && monotonic_now_ms < failed->retry_at_ms) {
        AccountMediaRequestResult waiting = result(AccountMediaRequestState::retry_wait);
        waiting.retry_at_ms = failed->retry_at_ms;
        return waiting;
    }
    clear_failure(media, key);
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
    ConstSlice<u8> etag{};
    if (key.kind == MediaAssetKind::regional_flag || lottie_asset(key)) {
        auto lease = encoded_media_cache_acquire(media->pipeline.cache, key, unix_now_seconds);
        const bool fresh = lease.acquired && lease.lookup.state == EncodedMediaCacheLookupState::fresh;
        const bool published = fresh && publish_vector(media, key, lease.lookup.bytes, 0);
        if (published && lottie_asset(key))
            media->frame_cache_generation = lease.lookup.generation;
        if (lease.acquired && !fresh && lease.lookup.etag.count <= sizeof(media->pipeline.result_etag)) {
            media->pipeline.result_etag_bytes = lease.lookup.etag.count;
            copy_bytes(media->pipeline.result_etag, lease.lookup.etag.data, lease.lookup.etag.count);
            etag = {media->pipeline.result_etag, media->pipeline.result_etag_bytes};
        }
        if (lease.acquired)
            encoded_media_cache_release(media->pipeline.cache, &lease);
        if (published)
            return result(AccountMediaRequestState::ready);
        if (fresh) {
            encoded_media_cache_erase(media->pipeline.cache, key);
            return remember_failure(media, key, monotonic_now_ms,
                                    result(AccountMediaRequestState::failed, AccountMediaError::decode));
        }
        if (key.kind == MediaAssetKind::regional_flag) {
            return publish_bundled_flag(media, key)
                       ? result(AccountMediaRequestState::ready)
                       : result(AccountMediaRequestState::failed, AccountMediaError::decode);
        }
    } else if (key.kind == MediaAssetKind::attachment) {
        CdnStaticJpegResult cached =
            cdn_media_pipeline_lookup_static_jpeg(&media->pipeline, key, unix_now_seconds);
        if (cached.state == CdnStaticJpegState::jpeg_workspace_required) {
            if (!ensure_jpeg_workspace(media)) {
                return result(AccountMediaRequestState::failed, AccountMediaError::memory);
            }
            cached = cdn_media_pipeline_lookup_static_jpeg(&media->pipeline, key, unix_now_seconds);
        }
        if (cached.decoded()) {
            publish_jpeg_frame(media, key, cached, 0, 0, 0);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnStaticJpegState::cache_miss &&
            cached.state != CdnStaticJpegState::fetch_required) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.jpeg_error = cached.decode.error;
            return remember_failure(media, key, monotonic_now_ms, failure);
        }
        // Attachment downloads use the signed URL from the message payload.
        // MediaAssetKey contains identity only, so a cache miss cannot be turned
        // into a CDN URL here.
        return result(AccountMediaRequestState::unsupported);
    } else if (webp_asset(key) && animated_asset(key)) {
        CdnAnimatedWebpResult cached =
            cdn_media_pipeline_lookup_animated_webp(&media->pipeline, key, unix_now_seconds);
        if (cached.state == CdnAnimatedWebpState::lossy_workspace_required) {
            if (!ensure_lossy_webp_workspace(media)) {
                return result(AccountMediaRequestState::failed, AccountMediaError::memory);
            }
            cached = cdn_media_pipeline_lookup_animated_webp(&media->pipeline, key, unix_now_seconds);
        }
        if (cached.decoded()) {
            publish_animated_webp_frame(media, key, cached, 0);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnAnimatedWebpState::cache_miss &&
            cached.state != CdnAnimatedWebpState::fetch_required) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.webp_error = cached.decode.frame_decode.error;
            failure.animated_webp_error = cached.decode.error;
            failure.lossy_webp_error = cached.decode.lossy_decode.error;
            return remember_failure(media, key, monotonic_now_ms, failure);
        }
        etag = cached.etag;
    } else if (webp_asset(key)) {
        CdnStaticWebpResult cached =
            cdn_media_pipeline_lookup_static_webp(&media->pipeline, key, unix_now_seconds);
        if (cached.state == CdnStaticWebpState::lossy_workspace_required) {
            if (!ensure_lossy_webp_workspace(media)) {
                return result(AccountMediaRequestState::failed, AccountMediaError::memory);
            }
            cached = cdn_media_pipeline_lookup_static_webp(&media->pipeline, key, unix_now_seconds);
        }
        if (cached.decoded()) {
            publish_webp_frame(media, key, cached, 0);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnStaticWebpState::cache_miss &&
            cached.state != CdnStaticWebpState::fetch_required) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.webp_error = cached.decode.error;
            failure.lossy_webp_error = cached.lossy_decode.error;
            return remember_failure(media, key, monotonic_now_ms, failure);
        }
        etag = cached.etag;
    } else if (key.variant == 2) {
        const CdnApngResult cached =
            cdn_media_pipeline_lookup_apng(&media->pipeline, key, unix_now_seconds);
        if (cached.decoded()) {
            publish_apng_frame(media, key, cached, 0);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnApngState::cache_miss && cached.state != CdnApngState::fetch_required) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.apng_error = cached.decode.error;
            return remember_failure(media, key, monotonic_now_ms, failure);
        }
        etag = cached.etag;
    } else if (key.variant == 4) {
        const CdnGifResult cached = cdn_media_pipeline_lookup_gif(&media->pipeline, key, unix_now_seconds);
        if (cached.decoded()) {
            publish_gif_frame(media, key, cached, 0);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnGifState::cache_miss && cached.state != CdnGifState::fetch_required) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.gif_error = cached.decode.error;
            return remember_failure(media, key, monotonic_now_ms, failure);
        }
        etag = cached.etag;
    } else {
        const CdnStaticPngResult cached =
            cdn_media_pipeline_lookup_static_png(&media->pipeline, key, unix_now_seconds);
        if (cached.decoded()) {
            publish_png_frame(media, key, cached, 0);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnStaticPngState::cache_miss &&
            cached.state != CdnStaticPngState::fetch_required) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.png_error = cached.decode.error;
            return remember_failure(media, key, monotonic_now_ms, failure);
        }
        etag = cached.etag;
    }
    u8 target[kCdnMaximumTargetBytes]{};
    CdnTargetResult written{};
    if (key.kind == MediaAssetKind::regional_flag) {
        written = {regional_flag_svg_target(static_cast<u16>(key.id),
                                            {reinterpret_cast<char*>(target), sizeof(target)}),
                   CdnHost::flag_assets, CdnTargetError::none};
        if (!written.bytes)
            written.error = CdnTargetError::invalid_input;
    } else if (hashed_identity) {
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
    if (++media->request_id == 0) {
        ++media->request_id;
    }
    const CdnClientSubmitResult submitted = cdn_client_submit(
        &media->client,
        CdnClientRequest{ConstSlice<u8>{target, written.bytes}, etag, media->request_id,
                         kCdnClientDefaultTimeoutMs, written.host,
                         key.kind == MediaAssetKind::regional_flag
                                    ? CdnImageType::regional_flag_svg
                         : lottie_asset(key) ? CdnImageType::lottie_json
                         : webp_asset(key)   ? CdnImageType::webp
                         : key.variant == 4  ? CdnImageType::gif
                                                    : CdnImageType::png});
    if (!submitted.ok()) {
        AccountMediaRequestResult failure =
            result(AccountMediaRequestState::failed, AccountMediaError::client);
        return remember_failure(media, key, monotonic_now_ms, failure);
    }
    media->active_key = key;
    media->active_request_id = media->request_id;
    media->active = true;
    return result(AccountMediaRequestState::queued);
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

struct AccountMediaUrlRequest {
    Utf8View url;
    MediaAssetKey key;
    u64 source_message_id;
    u32 source_message_revision;
    u16 proxy_width;
    u16 proxy_height;
    CdnImageType image_type;
    ID3D11Device* video_device;
    u64 catalog_scope;
    u32 catalog_generation;
    bool progressive = false;
    u64 provider_expires = 0;
    CdnHost provider_host = CdnHost::none;
    Utf8View companion_url{};
    u16 collection_track{};
};

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
        ~Names() noexcept { if (text.data) VirtualFree(text.data, 0, MEM_RELEASE); }
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
    auto* collection = static_cast<AccountMediaCollection*>(VirtualAlloc(nullptr,
        sizeof(AccountMediaCollection) + count * sizeof(CollectionTrack) + (youtube ? page.continuation_bytes : 0), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
        if (previous) VirtualFree(media->collection, 0, MEM_RELEASE);
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
        decoded = ensure_mp4_decoder(media) &&
                  mp4_decoder_initialize(media->mp4_decoder, lease.lookup.bytes, request.proxy_width,
                                         request.proxy_height, media->video_device);
    }
    encoded_media_cache_release(media->pipeline.cache, &lease);
    if (!decoded)
        return false;
    media->active_key = request.key;
    adopt_mp4_source(media);
    const Mp4DecodedFrame frame = mp4_decoder_read(media->mp4_decoder);
    media->active_key = MediaAssetKey{};
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
                                                     ConstSlice<u8> target, CdnHost host,
                                                     u64 expires_at_seconds, u64 monotonic_now_ms,
                                                     u64 unix_now_seconds) noexcept {
    if (!media->collection && !request.companion_url.bytes && decode_cached_mp4(media, request, unix_now_seconds))
        return result(AccountMediaRequestState::ready);
    account_media_release_video(media);
    media->progressive = static_cast<ProgressiveVideoFetch*>(
        VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!media->progressive || !progressive_video_fetch_begin(media->progressive, target, host, expires_at_seconds,
            request.companion_url.bytes ? ProgressiveVideoKind::fragmented_video : ProgressiveVideoKind::movie))
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    if (request.companion_url.bytes) {
        media->progressive_audio = static_cast<ProgressiveVideoFetch*>(
            VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
static bool activity_art_url(Utf8View url) noexcept {
    constexpr u32 bytes = sizeof(kActivityArtOrigin) - 1;
    return url.data && url.bytes > bytes && bytes_equal(url.data, kActivityArtOrigin, bytes);
}
static MediaUrlResult parse_activity_art_url(Utf8View url, Slice<u8> target_output) noexcept {
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

static AccountMediaRequestResult request_provider_media(AccountMedia*, const AccountMediaUrlRequest&,
                                                        u64, u64) noexcept;
static AccountMediaRequestResult request_url_media(AccountMedia* media,
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
    const AccountMediaFailure* failed = collection ? nullptr : find_failure(media, request.key);
    if (failed && monotonic_now_ms < failed->retry_at_ms) {
        AccountMediaRequestResult waiting = result(AccountMediaRequestState::retry_wait);
        waiting.retry_at_ms = failed->retry_at_ms;
        return waiting;
    }
    clear_failure(media, request.key);
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
    if (media->frame_ready) {
        return result(media_asset_key_equal(media->frame_key, request.key) ? AccountMediaRequestState::ready
                                                              : AccountMediaRequestState::busy);
    }
    if (media->active) {
        return result(media_asset_key_equal(media->active_key, request.key) ? AccountMediaRequestState::pending
                                                               : AccountMediaRequestState::busy);
    }
    media->active_source_message_id = request.source_message_id;
    media->active_source_message_revision = request.source_message_revision;
    media->active_catalog_scope = request.catalog_scope;
    media->active_catalog_generation = request.catalog_generation;
    ConstSlice<u8> etag{};
    if (audio_image_type(request.image_type)) {
        auto cached = cdn_media_pipeline_lookup_audio(&media->pipeline, request.key, unix_now_seconds,
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
        etag = embed_audio_preview_limit(target_host) ? ConstSlice<u8>{} : cached.etag;
    } else if (lottie_asset(request.key)) {
        auto lease = encoded_media_cache_acquire(media->pipeline.cache, request.key, unix_now_seconds);
        const bool fresh = lease.acquired && lease.lookup.state == EncodedMediaCacheLookupState::fresh;
        const bool published = fresh && publish_vector(media, request.key, lease.lookup.bytes, 0);
        if (published)
            media->frame_cache_generation = lease.lookup.generation;
        if (lease.acquired && !fresh && lease.lookup.etag.count <= sizeof(media->pipeline.result_etag)) {
            media->pipeline.result_etag_bytes = lease.lookup.etag.count;
            copy_bytes(media->pipeline.result_etag, lease.lookup.etag.data, lease.lookup.etag.count);
            etag = {media->pipeline.result_etag, media->pipeline.result_etag_bytes};
        }
        if (lease.acquired)
            encoded_media_cache_release(media->pipeline.cache, &lease);
        if (published)
            return result(AccountMediaRequestState::ready);
        if (fresh)
            encoded_media_cache_erase(media->pipeline.cache, request.key);
    } else if (webp_asset(request.key) && animated_asset(request.key)) {
        if (!ensure_webp_workspace(media) || !ensure_animated_webp_workspace(media)) {
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        }
        auto cached =
            cdn_media_pipeline_lookup_animated_webp(&media->pipeline, request.key, unix_now_seconds);
        if (cached.state == CdnAnimatedWebpState::lossy_workspace_required &&
            ensure_lossy_webp_workspace(media)) {
            cached =
                cdn_media_pipeline_lookup_animated_webp(&media->pipeline, request.key, unix_now_seconds);
        }
        if (cached.decoded()) {
            publish_animated_webp_frame(media, request.key, cached, 0);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnAnimatedWebpState::cache_miss &&
            cached.state != CdnAnimatedWebpState::fetch_required) {
            auto failure = result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.animated_webp_error = cached.decode.error;
            return remember_failure(media, request.key, monotonic_now_ms, failure);
        }
        etag = cached.etag;
    } else if (apng_asset(request.key) ||
               catalog_png_cached_animation(media, request.key, unix_now_seconds)) {
        if (!ensure_apng_workspace(media))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        const auto cached = cdn_media_pipeline_lookup_apng(&media->pipeline, request.key, unix_now_seconds);
        if (cached.decoded()) {
            publish_apng_frame(media, request.key, cached, 0);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnApngState::cache_miss && cached.state != CdnApngState::fetch_required) {
            auto failure = result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.apng_error = cached.decode.error;
            return remember_failure(media, request.key, monotonic_now_ms, failure);
        }
        etag = cached.etag;
    } else if (gif_asset(request.key)) {
        if (!ensure_gif_workspace(media)) {
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        }
        const auto cached = cdn_media_pipeline_lookup_gif(&media->pipeline, request.key, unix_now_seconds);
        if (cached.decoded()) {
            publish_gif_frame(media, request.key, cached, 0);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnGifState::cache_miss && cached.state != CdnGifState::fetch_required) {
            auto failure = result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.gif_error = cached.decode.error;
            return remember_failure(media, request.key, monotonic_now_ms, failure);
        }
        etag = cached.etag;
    } else if (request.image_type == CdnImageType::mp4) {
        if (!video_asset(request.key)) {
            return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
        }
        // A picker tile reopened after its decoder moved on restarts from the
        // shared encoded cache instead of refetching the source.
        if (catalog_key && decode_cached_mp4(media, request, unix_now_seconds))
            return result(AccountMediaRequestState::ready);
    } else if (request.image_type == CdnImageType::jpeg) {
        CdnStaticJpegResult cached =
            cdn_media_pipeline_lookup_static_jpeg(&media->pipeline, request.key, unix_now_seconds);
        if (cached.state == CdnStaticJpegState::jpeg_workspace_required) {
            if (!ensure_jpeg_workspace(media)) {
                return result(AccountMediaRequestState::failed, AccountMediaError::memory);
            }
            cached = cdn_media_pipeline_lookup_static_jpeg(&media->pipeline, request.key, unix_now_seconds);
        }
        if (cached.decoded()) {
            publish_jpeg_frame(media, request.key, cached, 0, request.source_message_id,
                               request.source_message_revision);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnStaticJpegState::cache_miss &&
            cached.state != CdnStaticJpegState::fetch_required) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.jpeg_error = cached.decode.error;
            return remember_failure(media, request.key, monotonic_now_ms, failure);
        }
        etag = cached.etag;
    } else if (request.image_type == CdnImageType::webp) {
        if (!ensure_webp_workspace(media)) {
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        }
        CdnStaticWebpResult cached =
            cdn_media_pipeline_lookup_static_webp(&media->pipeline, request.key, unix_now_seconds);
        if (cached.state == CdnStaticWebpState::lossy_workspace_required) {
            if (!ensure_lossy_webp_workspace(media)) {
                return result(AccountMediaRequestState::failed, AccountMediaError::memory);
            }
            cached = cdn_media_pipeline_lookup_static_webp(&media->pipeline, request.key, unix_now_seconds);
        }
        if (cached.decoded()) {
            publish_webp_frame(media, request.key, cached, 0, request.source_message_id,
                               request.source_message_revision);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnStaticWebpState::cache_miss &&
            cached.state != CdnStaticWebpState::fetch_required) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.webp_error = cached.decode.error;
            failure.lossy_webp_error = cached.lossy_decode.error;
            return remember_failure(media, request.key, monotonic_now_ms, failure);
        }
        etag = cached.etag;
    } else {
        const CdnStaticPngResult cached =
            cdn_media_pipeline_lookup_static_png(&media->pipeline, request.key, unix_now_seconds);
        if (cached.decoded()) {
            publish_png_frame(media, request.key, cached, 0, request.source_message_id,
                              request.source_message_revision);
            return result(AccountMediaRequestState::ready);
        }
        if (cached.state != CdnStaticPngState::cache_miss &&
            cached.state != CdnStaticPngState::fetch_required) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed, AccountMediaError::decode);
            failure.png_error = cached.decode.error;
            return remember_failure(media, request.key, monotonic_now_ms, failure);
        }
        etag = cached.etag;
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
    if (++media->request_id == 0) {
        ++media->request_id;
    }
    const CdnClientSubmitResult submitted = cdn_client_submit(
        &media->client,
        CdnClientRequest{request_target, etag, media->request_id, kCdnClientDefaultTimeoutMs,
                         target_host, request.image_type});
    if (!submitted.ok()) {
        AccountMediaRequestResult failure =
            result(AccountMediaRequestState::failed, AccountMediaError::client);
        return remember_failure(media, request.key, monotonic_now_ms, failure);
    }
    media->active_key = request.key;
    media->active_request_id = media->request_id;
    media->active_source_message_id = request.source_message_id;
    media->active_source_message_revision = request.source_message_revision;
    media->active_proxy_width = request.proxy_width;
    media->active_proxy_height = request.proxy_height;
    media->active_image_type = request.image_type;
    media->active = true;
    return result(AccountMediaRequestState::queued);
}

static AccountMediaRequestResult request_provider_media(AccountMedia* media, const AccountMediaUrlRequest& request,
    u64 now_ms, u64 unix_seconds) noexcept {
    if (media->frame_ready)
        return result(media_asset_key_equal(media->frame_key, request.key) ? AccountMediaRequestState::ready
                                                                         : AccountMediaRequestState::busy);
    if (media->active)
        return result(media_asset_key_equal(media->active_key, request.key) ? AccountMediaRequestState::pending
                                                                          : AccountMediaRequestState::busy);
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
        resolver = static_cast<EmbedResolver*>(VirtualAlloc(nullptr, sizeof(EmbedResolver),
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
                failure.retry_at_ms = deadline_after(now_ms, loaded.retry_after_seconds ? u64{loaded.retry_after_seconds} * 1000 : kAccountMediaFailureRetryMs);
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
        media->progressive = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!media->progressive) return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        const auto resolved = soundcloud_resolve(resolver, page, media->progressive);
        if (!resolved.ok()) {
            account_media_release_video(media);
            auto failure = result(resolved.error == EmbedPlayerError::unsupported || resolved.error == EmbedPlayerError::not_embeddable
                ? AccountMediaRequestState::unsupported : AccountMediaRequestState::failed, AccountMediaError::http);
            if (resolved.retry_after_seconds) failure.retry_at_ms = deadline_after(now_ms, u64{resolved.retry_after_seconds} * 1000);
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
            failure.retry_at_ms = deadline_after(now_ms, loaded.retry_after_seconds ? u64{loaded.retry_after_seconds} * 1000 : kAccountMediaFailureRetryMs);
            return failure;
        }
        const u16 wanted = request.collection_track ? request.collection_track : media->collection->initial;
        if (wanted == media->collection->count && media->collection->continuation_bytes) {
            auto extend = request; extend.collection_track = wanted;
            const auto next = load_collection(media, extend);
            if (!next.ok()) {
                auto failed = result(AccountMediaRequestState::failed, AccountMediaError::http);
                failed.retry_at_ms = deadline_after(now_ms, next.retry_after_seconds ? u64{next.retry_after_seconds} * 1000 : kAccountMediaFailureRetryMs); return failed;
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
        account_media_release_video(media);
        media->progressive = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        media->progressive_audio = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!media->progressive || !media->progressive_audio) {
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
            if (resolved.retry_after_seconds) failure.retry_at_ms = deadline_after(now_ms, u64{resolved.retry_after_seconds} * 1000);
            return remember_failure(media, request.key, now_ms, failure);
        }
        if (media->progressive_audio->state == ProgressiveVideoFetchState::empty) {
            VirtualFree(media->progressive_audio, 0, MEM_RELEASE); media->progressive_audio = nullptr;
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
        account_media_release_video(media);
        media->progressive=static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr,sizeof(ProgressiveVideoFetch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        media->progressive_audio=static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr,sizeof(ProgressiveVideoFetch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        const auto live=media->progressive && media->progressive_audio
            ? youtube_hls_resolve(resolver,{streams.video.url,streams.video.url_bytes},height,unix_seconds,media->progressive,media->progressive_audio)
            : EmbedResolveResult{EmbedPlayerError::capacity};
        secure_clear_bytes(&streams,sizeof(streams));
        if(!live.ok()) {
            account_media_release_video(media);
            auto failure=result(live.error==EmbedPlayerError::unsupported?AccountMediaRequestState::unsupported:AccountMediaRequestState::failed,
                live.error==EmbedPlayerError::capacity?AccountMediaError::memory:AccountMediaError::http);
            if(live.retry_after_seconds) failure.retry_at_ms=deadline_after(now_ms,u64{live.retry_after_seconds}*1000);
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
        if (resolved.retry_after_seconds) failure.retry_at_ms = deadline_after(now_ms, u64{resolved.retry_after_seconds} * 1000);
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
AccountMediaRequestResult account_media_request_attachment(AccountMedia* media,
                                                           const AccountMediaAttachmentRequest& request,
                                                           u64 monotonic_now_ms,
                                                           u64 unix_now_seconds) noexcept {
    if (request.options.progressive > 1 || request.options.reserved[0] || request.options.reserved[1] ||
        request.options.collection_track > kMediaCollectionTracks ||
        (request.options.collection_track && (!request.options.progressive || (!(soundcloud_collection_url(request.signed_url) || apple_music_collection_locator(request.signed_url).id.bytes) || request.image_type != CdnImageType::m4a) &&
            (!youtube_collection_locator(request.signed_url).list.bytes || request.image_type != CdnImageType::mp4))) ||
        (request.options.progressive &&
         (request.key.kind != MediaAssetKind::embed || (request.image_type != CdnImageType::mp4 && request.image_type != CdnImageType::m4a))) ||
        request.key.kind == MediaAssetKind::catalog_preview)
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    AccountMediaUrlRequest source{request.signed_url,
                                        request.key,
                                        request.source_message_id,
                                        request.source_message_revision,
                                        request.proxy_width,
                                        request.proxy_height,
                                        request.image_type,
                                        request.video_device,
                                        0,
                                        0,
                                        request.options.progressive != 0};
    source.collection_track = request.options.collection_track;
    return request_url_media(media, source, monotonic_now_ms, unix_now_seconds);
}

AccountMediaRequestResult account_media_request_catalog_preview(
    AccountMedia* media, const AccountMediaCatalogPreviewRequest& request, u64 monotonic_now_ms,
    u64 unix_now_seconds) noexcept {
    bool reserved = false;
    for (u8 value : request.reserved)
        reserved = reserved || value != 0;
    if (reserved || !media_asset_key_valid(request.key) ||
        request.key.kind != MediaAssetKind::catalog_preview || !request.catalog_scope ||
        !request.catalog_generation || request.key.variant != request.catalog_generation ||
        (request.format != MediaAttachmentFormat::webp && request.format != MediaAttachmentFormat::png &&
         request.format != MediaAttachmentFormat::gif && request.format != MediaAttachmentFormat::lottie &&
         request.format != MediaAttachmentFormat::mp4) ||
        request.format != media_asset_catalog_format(request.key) || !request.width || !request.height ||
        request.width > 512 || request.height > 512)
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    const AccountMediaUrlRequest source{
        request.url,
        request.key,
        0,
        0,
        request.width,
        request.height,
        request.format == MediaAttachmentFormat::webp     ? CdnImageType::webp
        : request.format == MediaAttachmentFormat::png    ? CdnImageType::png
        : request.format == MediaAttachmentFormat::lottie ? CdnImageType::lottie_json
        : request.format == MediaAttachmentFormat::mp4    ? CdnImageType::mp4
                                                          : CdnImageType::gif,
        request.video_device,
        request.catalog_scope,
        request.catalog_generation};
    return request_url_media(media, source, monotonic_now_ms, unix_now_seconds);
}

AccountMediaRequestResult account_media_advance_animation(AccountMedia* media, MediaAssetKey key,
                                                          u16 cache_generation, u32 frame_index,
                                                          u64 unix_now_seconds) noexcept {
    const bool animated_webp = webp_asset(key) && animated_asset(key);
    const bool video = video_asset(key) && account_media_video_matches(media, key, cache_generation);
    const bool animated_sticker =
        (key.kind == MediaAssetKind::sticker && (key.variant == 2 || key.variant == 4)) ||
        catalog_png_key(key);
    if (!media || !media->started || !supported_key(key) ||
        (!lottie_asset(key) && !animated_webp && !animated_sticker && !apng_asset(key) && !gif_asset(key) &&
         !video) ||
        cache_generation == 0 || unix_now_seconds == 0) {
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    if (media->active || media->frame_ready) {
        return result(AccountMediaRequestState::busy);
    }
    // A cold cache advance carries no borrowed UI pointer. Main must match it to
    // its pending request ID and the typed initial source.
    media->active_source_message_id = 0;
    media->active_source_message_revision = 0;
    media->active_catalog_scope = 0;
    media->active_catalog_generation = 0;
    if (lottie_asset(key)) {
        auto lease = encoded_media_cache_acquire(media->pipeline.cache, key, unix_now_seconds);
        const bool matches = lease.acquired && lease.lookup.generation == cache_generation;
        const bool published = matches && publish_vector(media, key, lease.lookup.bytes, 0, frame_index);
        if (published)
            media->frame_cache_generation = lease.lookup.generation;
        if (lease.acquired)
            encoded_media_cache_release(media->pipeline.cache, &lease);
        return result(published ? AccountMediaRequestState::ready : AccountMediaRequestState::failed,
                      published ? AccountMediaError::none
                      : matches ? AccountMediaError::decode
                                : AccountMediaError::cache);
    }
    if (animated_webp && (!ensure_webp_workspace(media) || !ensure_animated_webp_workspace(media))) {
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    }
    if ((apng_asset(key) || catalog_png_key(key)) && !ensure_apng_workspace(media)) {
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    }
    if (gif_asset(key) && !ensure_gif_workspace(media)) {
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    }
    if (video) {
        const Mp4DecodedFrame decoded = mp4_decoder_read(media->mp4_decoder);
        // Progressive attachments fetch only during selected playback.
        if (decoded.error == Mp4DecodeError::data_pending)
            return result(AccountMediaRequestState::busy);
        if (!decoded.ok()) {
            return video_failure(media->mp4_decoder);
        }
        publish_mp4_frame(media, key, decoded, 0, 0, 0, frame_index);
        return result(AccountMediaRequestState::ready);
    }
    if (animated_webp) {
        CdnAnimatedWebpResult decoded = cdn_media_pipeline_decode_animated_webp_frame(
            &media->pipeline, key, cache_generation, frame_index, unix_now_seconds);
        if (decoded.state == CdnAnimatedWebpState::lossy_workspace_required) {
            if (!ensure_lossy_webp_workspace(media)) {
                return result(AccountMediaRequestState::failed, AccountMediaError::memory);
            }
            decoded = cdn_media_pipeline_decode_animated_webp_frame(&media->pipeline, key, cache_generation,
                                                                    frame_index, unix_now_seconds);
        }
        if (!decoded.decoded()) {
            AccountMediaRequestResult failure =
                result(AccountMediaRequestState::failed,
                       decoded.state == CdnAnimatedWebpState::cache_miss ||
                               decoded.state == CdnAnimatedWebpState::generation_mismatch
                           ? AccountMediaError::cache
                           : AccountMediaError::decode);
            failure.webp_error = decoded.decode.frame_decode.error;
            failure.animated_webp_error = decoded.decode.error;
            failure.lossy_webp_error = decoded.decode.lossy_decode.error;
            return failure;
        }
        publish_animated_webp_frame(media, key, decoded, 0);
        return result(AccountMediaRequestState::ready);
    }
    if (gif_asset(key)) {
        const CdnGifResult decoded = cdn_media_pipeline_decode_gif_frame(
            &media->pipeline, key, cache_generation, frame_index, unix_now_seconds);
        if (!decoded.decoded()) {
            AccountMediaRequestResult failure = result(
                AccountMediaRequestState::failed, decoded.state == CdnGifState::cache_miss ||
                                                          decoded.state == CdnGifState::generation_mismatch
                                                      ? AccountMediaError::cache
                                                      : AccountMediaError::decode);
            failure.gif_error = decoded.decode.error;
            return failure;
        }
        publish_gif_frame(media, key, decoded, 0);
        return result(AccountMediaRequestState::ready);
    }
    const CdnApngResult decoded = cdn_media_pipeline_decode_apng_frame(
        &media->pipeline, key, cache_generation, frame_index, unix_now_seconds);
    if (!decoded.decoded()) {
        AccountMediaRequestResult failure = result(
            AccountMediaRequestState::failed,
            decoded.state == CdnApngState::cache_miss || decoded.state == CdnApngState::generation_mismatch
                ? AccountMediaError::cache
                : AccountMediaError::decode);
        failure.apng_error = decoded.decode.error;
        return failure;
    }
    publish_apng_frame(media, key, decoded, 0);
    return result(AccountMediaRequestState::ready);
}

bool account_media_pump(AccountMedia* media, u64 monotonic_now_ms, u64 unix_now_seconds) noexcept {
    if (!media || !media->started || monotonic_now_ms == 0 || unix_now_seconds == 0) {
        return false;
    }
    if (!media->client_started) {
        return true;
    }
    const CdnClientResult status = cdn_client_status(&media->client);
    if (status.phase != CdnClientPhase::response_ready && !media->progressive_continue) {
        return status.phase != CdnClientPhase::failed;
    }
    if (status.phase == CdnClientPhase::failed) return false;
    if (status.phase == CdnClientPhase::response_ready) {
    ConsumeContext context{media, monotonic_now_ms};
    const CdnClientTakeResult consumed =
        cdn_client_consume_response(&media->client, consume_response, &context);
    if (!consumed.ok()) {
        return false;
    }
    }
    if (media->progressive_continue) {
        media->progressive_continue = false;
        if (continue_progressive(media, monotonic_now_ms, unix_now_seconds))
            return true;
    }
    media->progressive_active = false;
    media->active = false;
    media->active_key = MediaAssetKey{};
    media->active_request_id = 0;
    media->active_source_message_id = 0;
    media->active_source_message_revision = 0;
    media->active_proxy_width = 0;
    media->active_catalog_scope = 0;
    media->active_catalog_generation = 0;
    media->active_proxy_height = 0;
    return true;
}

namespace {
static_assert(kLocalDraftPreviewThumbnailJpegBytes == kJpegThumbnailEncodedMaximumBytes);
// Video posters are reduced on the GPU to the same extent as image thumbnails.
static_assert(kGpuVideoPosterExtent == kLocalPreviewThumbnailExtent);
static_assert(kLocalDraftPreviewThumbnailWebpBytes == kWebpThumbnailEncodedMaximumBytes);
struct ThumbnailMemory {
    u8* base = nullptr;
    ~ThumbnailMemory() noexcept {
        if (base)
            VirtualFree(base, 0, MEM_RELEASE);
    }
    u8* allocate(u64 bytes) noexcept {
        if (!bytes || bytes > 0x7fffffffull)
            return nullptr;
        base = static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), MEM_RESERVE | MEM_COMMIT,
                                             PAGE_READWRITE));
        return base;
    }
};
// Output, orientation copy, column sums and one expanded source row.
struct ThumbnailTarget {
    ThumbnailMemory memory;
    Slice<u8> output, rotate, row;
    Slice<u32> sums;
    bool allocate(u32 source_width) noexcept {
        constexpr u32 image = kLocalPreviewThumbnailExtent * kLocalPreviewThumbnailExtent * 4u;
        constexpr u32 sum_bytes = (kLocalPreviewThumbnailExtent + 1u) * 5u * sizeof(u32);
        const u64 row_bytes = static_cast<u64>(source_width) * 4u;
        u8* base = memory.allocate(image * 2ull + sum_bytes + row_bytes);
        if (!base)
            return false;
        output = {base, image};
        rotate = {base + image, image};
        sums = {reinterpret_cast<u32*>(base + image * 2u), sum_bytes / sizeof(u32)};
        row = {base + image * 2u + sum_bytes, static_cast<u32>(row_bytes)};
        return true;
    }
};
AccountMediaRequestResult publish_local_thumbnail(AccountMedia* media, MediaAssetKey key, const u8* rgba, u32 width,
                                                  u32 height) noexcept {
    const u32 bytes = width * height * 4u;
    if (!width || !height || bytes > media->pipeline.scanline_capacity || !media->pipeline.scanline_storage)
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    copy_bytes(media->pipeline.scanline_storage, rgba, bytes);
    media->frame_webp_image = WebpImage{};
    media->frame_webp_image.width = width;
    media->frame_webp_image.height = height;
    media->frame_key = key;
    media->frame_bytes = bytes;
    media->frame_request_id = 0;
    media->frame_source_message_id = key.id;
    media->frame_source_message_revision = key.variant;
    capture_catalog_source(media, key);
    media->frame_kind = AccountMediaFrameKind::straight_rgba8;
    mark_frame_ready(media, key, 0);
    media->frame_cache_generation = 0;
    media->frame_count = 1;
    media->frame_index = 0;
    media->frame_duration_us = 0;
    media->frame_play_count = 1;
    return result(AccountMediaRequestState::ready);
}
// Unsupported: the caller continues with the ordinary GPU preview path.
AccountMediaRequestResult decode_local_thumbnail(AccountMedia* media, const AccountMediaLocalPreviewRequest& request,
                                                 ConstSlice<u8> encoded) noexcept {
    const auto key = request.key;
    const auto format = media_asset_catalog_format(key);
    const auto legacy = [&]() noexcept {
        return request.snapshot.bytes <= local_draft_preview_encoded_limit(format)
                   ? result(AccountMediaRequestState::unsupported)
                   : result(AccountMediaRequestState::failed, AccountMediaError::decode);
    };
    // Stills the GPU VP8 path admits (<= 512 px, <= 2 MiB) keep it; larger
    // lossy WebPs decode on the CPU one macroblock row at a time.
    const bool large_webp =
        format == MediaAttachmentFormat::webp &&
        (request.snapshot.bytes > local_draft_preview_encoded_limit(format) ||
         (encoded.data && [&]() noexcept {
             WebpImage image{};
             return webp_parse(encoded, &image).error == WebpError::dimensions;
         }()));
    if (format == MediaAttachmentFormat::webp && !large_webp)
        return legacy();
    if (!encoded.data)
        // Small JPEGs may already sit in the encoded cache; everything else
        // asks the lane to read the file.
        return format == MediaAttachmentFormat::jpeg ? legacy()
                                                     : result(AccountMediaRequestState::failed, AccountMediaError::cache);
    media->active_catalog_scope = request.owner_scope;
    media->active_catalog_generation = request.owner_generation;
    media->active_source_message_id = key.id;
    media->active_source_message_revision = key.variant;
    LocalThumbnailSink sink{};
    ThumbnailTarget target{};
    u8 orientation = 1;
    auto& pipeline = media->pipeline;
    if (format == MediaAttachmentFormat::png) {
        // Every PNG thumbnail streams: rows are inflated through a sliding
        // window, reconstructed against the previous row and box-filtered at
        // once, so dimensions past the 2048 px GPU path need no scanline copy.
        PngImage image{};
        PngStreamLayout layout{};
        const auto parsed = png_stream_parse(encoded, &image, &layout);
        // Adam7 is decoded whole by the GPU preview path (<= 2048 px).
        if (parsed.error == PngError::unsupported_interlace)
            return legacy();
        if (!parsed.ok())
            return result(AccountMediaRequestState::failed, AccountMediaError::decode);
        const u64 span_bytes = (static_cast<u64>(layout.idat_chunks) * sizeof(DeflateInputSpan) + 15u) & ~15ull;
        ThumbnailMemory stream;
        u8* base = stream.allocate(span_bytes + kPngStreamWindowBytes + layout.row_buffer_bytes);
        if (!base || !target.allocate(image.width) ||
            !local_thumbnail_begin(&sink, image.width, image.height, kLocalPreviewThumbnailExtent, target.output,
                                   target.sums))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        struct Rows {
            const PngImage* image;
            LocalThumbnailSink* sink;
            u8* rgba;
        } rows{&image, &sink, target.row.data};
        const auto decoded = png_stream_decode(
            encoded, image, layout, {reinterpret_cast<DeflateInputSpan*>(base), layout.idat_chunks},
            {base + span_bytes, kPngStreamWindowBytes}, {base + span_bytes + kPngStreamWindowBytes, layout.row_buffer_bytes},
            [](void* context, u32, const u8* row) noexcept {
                auto& r = *static_cast<Rows*>(context);
                // Reconstructed RGBA8 already has the sink's byte format.
                // The sink consumes the row before the stream reuses it.
                if (r.image->color_type == 6 && r.image->bit_depth == 8)
                    return local_thumbnail_push_row(r.sink, row);
                return local_thumbnail_png_row(*r.image, row, r.rgba) && local_thumbnail_push_row(r.sink, r.rgba);
            },
            &rows, &pipeline.workspace);
        if (!decoded.ok())
            return result(AccountMediaRequestState::failed, AccountMediaError::decode);
    } else if (format == MediaAttachmentFormat::webp) {
        WebpThumbnailLayout layout{};
        if (webp_thumbnail_parse(encoded, &layout) != WebpThumbnailError::none || layout.alpha_chunk.data)
            return result(AccountMediaRequestState::failed, AccountMediaError::decode);
        ThumbnailMemory scratch;
        u8* base = scratch.allocate(layout.workspace_bytes);
        if (!base || !target.allocate(layout.width) ||
            !local_thumbnail_begin(&sink, layout.width, layout.height, kLocalPreviewThumbnailExtent, target.output,
                                   target.sums))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        const auto decoded = webp_thumbnail_decode(
            layout, {base, layout.workspace_bytes},
            [](void* context, u32, const u8* rgba) noexcept {
                return local_thumbnail_push_row(static_cast<LocalThumbnailSink*>(context), rgba);
            },
            &sink);
        if (decoded != WebpThumbnailError::none)
            return result(AccountMediaRequestState::failed, AccountMediaError::decode);
    } else if (format == MediaAttachmentFormat::gif) {
        if (!ensure_gif_workspace(media))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        GifImage animation{};
        pipeline.gif_content_identity = 0;
        const auto parsed = gif_parse(encoded, &animation, media->gif_workspace);
        if (!parsed.ok() || !animation.frame_count)
            return result(AccountMediaRequestState::failed, AccountMediaError::decode);
        // The first frame, like the official static preview; the strip adds a GIF tag.
        const auto decoded = gif_decode_parsed_frame(encoded, animation, 0, media->gif_workspace,
                                                     {pipeline.scanline_storage, pipeline.scanline_capacity});
        if (!decoded.ok() || !target.allocate(animation.canvas_width) ||
            !local_thumbnail_begin(&sink, animation.canvas_width, animation.canvas_height, kLocalPreviewThumbnailExtent,
                                   target.output, target.sums))
            return result(AccountMediaRequestState::failed, decoded.ok() ? AccountMediaError::memory : AccountMediaError::decode);
        const ConstSlice<u8> indices{pipeline.scanline_storage, decoded.pixels};
        for (u32 y = 0; y < animation.canvas_height; ++y)
            if (!local_thumbnail_gif_row(decoded.frame, decoded.palette, indices, animation.canvas_width, y,
                                         target.row.data) ||
                !local_thumbnail_push_row(&sink, target.row.data))
                return result(AccountMediaRequestState::failed, AccountMediaError::decode);
    } else if (format == MediaAttachmentFormat::jpeg) {
        if (!ensure_jpeg_workspace(media))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        JpegImage image{};
        // Small baseline/progressive files keep the exact GPU IDCT path.
        if (encoded.count <= kJpegEncodedMaximumBytes && jpeg_parse(encoded, &image, media->jpeg_workspace).ok())
            return legacy();
        JpegThumbnailLayout layout{};
        if (!jpeg_thumbnail_parse(encoded, kLocalPreviewThumbnailExtent, &image, media->jpeg_workspace, &layout).ok())
            return result(AccountMediaRequestState::failed, AccountMediaError::decode);
        ThumbnailMemory scratch;
        u8* scratch_base = scratch.allocate(layout.coefficient_bytes + layout.plane_bytes);
        if (!scratch_base || !target.allocate(layout.width) ||
            !local_thumbnail_begin(&sink, layout.width, layout.height, kLocalPreviewThumbnailExtent, target.output,
                                   target.sums))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        const auto decoded = jpeg_thumbnail_decode_rows(
            encoded, image, layout, {scratch_base, static_cast<u32>(layout.coefficient_bytes + layout.plane_bytes)},
            target.row,
            [](void* context, u32, const u8* rgba) noexcept {
                return local_thumbnail_push_row(static_cast<LocalThumbnailSink*>(context), rgba);
            },
            &sink);
        if (!decoded.ok())
            return result(AccountMediaRequestState::failed, AccountMediaError::decode);
        orientation = static_cast<u8>(image.orientation);
    } else {
        return result(AccountMediaRequestState::failed, AccountMediaError::decode);
    }
    u32 width = sink.width, height = sink.height;
    if (!local_thumbnail_complete(sink) ||
        !local_thumbnail_orient(target.output, &width, &height, orientation, target.rotate))
        return result(AccountMediaRequestState::failed, AccountMediaError::decode);
    return publish_local_thumbnail(media, key, target.output.data, width, height);
}
} // namespace

AccountMediaRequestResult account_media_decode_local_preview(AccountMedia* media,
                                                             const AccountMediaLocalPreviewRequest& request,
                                                             ConstSlice<u8> encoded) noexcept {
    if (!media || !media->started || media->active || media->frame_ready ||
        !local_draft_preview_request_valid(request) ||
        ((encoded.data || encoded.count) && (!encoded.data || encoded.count != request.snapshot.bytes)))
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    if (local_draft_preview_thumbnail(request.key)) {
        const auto thumbnail = decode_local_thumbnail(media, request, encoded);
        if (thumbnail.state != AccountMediaRequestState::unsupported)
            return thumbnail;
    }
    const auto key = request.key;
    const auto format = media_asset_catalog_format(key);
    // This cache entry is local typed data, not an HTTP response. The parser
    // enforces each codec's encoded and decoded size limits.
    EncodedMediaCacheStoreResult stored{};
    if (encoded.data)
        stored = encoded_media_cache_store(media->pipeline.cache, key, encoded, {{}, ~0ull, true, false});
    else {
        const auto resident = encoded_media_cache_find(media->pipeline.cache, key, 0);
        if (resident.found() && resident.bytes.count == request.snapshot.bytes)
            stored = {EncodedMediaCacheStoreState::stored, resident.generation};
    }
    if (!stored.stored())
        return result(AccountMediaRequestState::failed, AccountMediaError::cache);
    media->active_catalog_scope = request.owner_scope;
    media->active_catalog_generation = request.owner_generation;
    media->active_source_message_id = key.id;
    media->active_source_message_revision = key.variant;
    AccountMediaRequestResult decoded = result(AccountMediaRequestState::failed, AccountMediaError::decode);
    if (format == MediaAttachmentFormat::png) {
        const auto value = cdn_media_pipeline_lookup_static_png(&media->pipeline, key, 0);
        decoded.png_error = value.decode.error;
        if (value.decoded())
            publish_png_frame(media, key, value, 0, key.id, key.variant);
    } else if (format == MediaAttachmentFormat::jpeg) {
        if (!ensure_jpeg_workspace(media))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        const auto value = cdn_media_pipeline_lookup_static_jpeg(&media->pipeline, key, 0);
        decoded.jpeg_error = value.decode.error;
        if (value.decoded())
            publish_jpeg_frame(media, key, value, 0, key.id, key.variant);
    } else if (format == MediaAttachmentFormat::webp) {
        if (!ensure_webp_workspace(media))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        auto value = cdn_media_pipeline_lookup_static_webp(&media->pipeline, key, 0);
        if (value.state == CdnStaticWebpState::lossy_workspace_required &&
            ensure_lossy_webp_workspace(media))
            value = cdn_media_pipeline_lookup_static_webp(&media->pipeline, key, 0);
        decoded.webp_error = value.decode.error;
        decoded.lossy_webp_error = value.lossy_decode.error;
        if (value.decoded())
            publish_webp_frame(media, key, value, 0, key.id, key.variant);
    } else if (format == MediaAttachmentFormat::gif) {
        if (!ensure_gif_workspace(media))
            return result(AccountMediaRequestState::failed, AccountMediaError::memory);
        const auto value = cdn_media_pipeline_lookup_gif(&media->pipeline, key, 0);
        decoded.gif_error = value.decode.error;
        if (value.decoded())
            publish_gif_frame(media, key, value, 0);
    }
    if (!media->frame_ready) {
        encoded_media_cache_erase(media->pipeline.cache, key);
        return decoded;
    }
    media->frame_cache_generation = stored.generation;
    media->frame_count = 1;
    media->frame_index = 0;
    media->frame_duration_us = 0;
    media->frame_play_count = 1;
    return result(AccountMediaRequestState::ready);
}

namespace {
AccountMediaRequestResult poster_failure(LocalVideoPosterStep step) noexcept {
    // A file that cannot become a poster stays a file card; only a failed
    // read (cancellation, a busy disk) may be asked for again.
    auto failure = result(step == LocalVideoPosterStep::read_failed ? AccountMediaRequestState::failed
                                                                    : AccountMediaRequestState::unsupported,
                          step == LocalVideoPosterStep::read_failed ? AccountMediaError::invalid_input
                                                                    : AccountMediaError::decode);
    failure.video_failure = step == LocalVideoPosterStep::unsupported ? AccountMediaVideoFailure::demux_unsupported
                            : step == LocalVideoPosterStep::malformed ? AccountMediaVideoFailure::demux_malformed
                            : step == LocalVideoPosterStep::budget    ? AccountMediaVideoFailure::memory
                                                                      : AccountMediaVideoFailure::none;
    if (step != LocalVideoPosterStep::read_failed)
        failure.retry_at_ms = ~0ull;
    return failure;
}
} // namespace

AccountMediaRequestResult account_media_decode_local_video_preview(AccountMedia* media,
                                                                   const AccountMediaLocalPreviewRequest& request,
                                                                   AttachmentProbeRead read,
                                                                   void* context) noexcept {
    if (!media || !media->started || media->active || media->frame_ready || !read ||
        !local_draft_preview_request_valid(request) || !local_draft_preview_thumbnail(request.key) ||
        media_asset_catalog_format(request.key) != MediaAttachmentFormat::mp4 ||
        request.snapshot.bytes > kMediaMaximumProgressiveVideoBytes)
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    if (!request.video_device)
        return poster_failure(LocalVideoPosterStep::unsupported);
    account_media_release_video(media);
    media->progressive = static_cast<ProgressiveVideoFetch*>(
        VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* scratch = static_cast<u8*>(
        VirtualAlloc(nullptr, kLocalVideoPosterScratchBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!media->progressive || !scratch ||
        !progressive_source_initialize(&media->progressive->source, static_cast<u32>(request.snapshot.bytes))) {
        if (scratch)
            VirtualFree(scratch, 0, MEM_RELEASE);
        account_media_release_video(media);
        return result(AccountMediaRequestState::failed, AccountMediaError::memory);
    }
    auto* source = &media->progressive->source;
    LocalVideoPosterReader reader{read, context, scratch, kLocalVideoPosterReadBudget, 0, 0};
    const auto finish = [&](AccountMediaRequestResult value) noexcept {
        VirtualFree(scratch, 0, MEM_RELEASE);
        if (value.state != AccountMediaRequestState::ready)
            account_media_release_video(media);
        media->active_key = MediaAssetKey{};
        return value;
    };
    auto step = local_video_poster_prepare(source, &reader);
    if (step != LocalVideoPosterStep::ready)
        return finish(poster_failure(step));
    const auto bytes = progressive_source_bytes(*source);
    const auto availability = progressive_source_reader(source, ProgressiveTrack::video);
    media->video_device = request.video_device;
    // Native extent: the renderer reduces the poster to a thumbnail on the GPU.
    if (!ensure_mp4_decoder(media) ||
        !mp4_decoder_initialize(media->mp4_decoder, bytes, 0, 0, request.video_device, availability)) {
        auto failure = video_failure(media->mp4_decoder);
        failure.retry_at_ms = ~0ull;
        if (failure.state == AccountMediaRequestState::failed)
            failure.state = AccountMediaRequestState::unsupported;
        return finish(failure);
    }
    media->active_key = request.key;
    media->active_catalog_scope = request.owner_scope;
    media->active_catalog_generation = request.owner_generation;
    media->active_source_message_id = request.key.id;
    media->active_source_message_revision = request.key.variant;
    adopt_mp4_source(media);
    Mp4DecodedFrame decoded{};
    // Reordered streams may submit a few access units before the first
    // picture; each pending one costs a bounded read from the file.
    for (u32 attempt = 0; attempt < 16; ++attempt) {
        decoded = mp4_decoder_read(media->mp4_decoder, false);
        if (decoded.error != Mp4DecodeError::data_pending)
            break;
        if (!local_video_poster_service(source, &reader, &step))
            return finish(poster_failure(step == LocalVideoPosterStep::ready ? LocalVideoPosterStep::malformed : step));
    }
    if (!decoded.ok() || !decoded.gpu) {
        auto failure = video_failure(media->mp4_decoder);
        failure.retry_at_ms = ~0ull;
        if (failure.state == AccountMediaRequestState::failed)
            failure.state = AccountMediaRequestState::unsupported;
        return finish(failure);
    }
    // The poster never plays and the GPU holds the picture: drop the sample
    // payload, keeping only the pinned box headers and moov.
    progressive_source_trim(source);
    if (!decoded.duration_us)
        decoded.duration_us = 1;
    publish_mp4_frame(media, request.key, decoded, 0, request.key.id, request.key.variant, 0);
    return finish(result(AccountMediaRequestState::ready));
}

bool account_media_local_video_resident(const AccountMedia* media) noexcept {
    return media && media->mp4_key.kind == MediaAssetKind::local_draft_preview &&
           ((media->mp4_decoder && media->mp4_decoder->ready) || media->progressive);
}

bool account_media_take_frame(AccountMedia* media, AccountMediaFrame* frame) noexcept {
    const bool jpeg = media && media->frame_kind == AccountMediaFrameKind::jpeg_coefficients;
    const bool video = media && media->frame_kind == AccountMediaFrameKind::video_nv12_gpu;
    const bool audio = media && media->frame_kind == AccountMediaFrameKind::audio_source;
    if (!media || !frame || !media->frame_ready || !media->pipeline.scanline_storage ||
        (media->frame_bytes == 0 && media->frame_kind != AccountMediaFrameKind::vp8_keyframe &&
         media->frame_kind != AccountMediaFrameKind::animated_webp_vp8 && !jpeg && !video && !audio) ||
        media->frame_kind == AccountMediaFrameKind::none) {
        return false;
    }
    const bool png_scanlines = media->frame_kind == AccountMediaFrameKind::png_scanlines ||
                               media->frame_kind == AccountMediaFrameKind::apng_scanlines;
    AccountMediaFrame output{};
    output.image = png_scanlines ? &media->frame_image : nullptr;
    output.jpeg_image = jpeg ? &media->frame_jpeg_image : nullptr;
    output.jpeg_coefficients =
        jpeg && media->pipeline.jpeg_workspace ? &media->pipeline.jpeg_workspace->coefficients : nullptr;
    output.gif_palette =
        media->frame_kind == AccountMediaFrameKind::gif_indices ? &media->frame_gif_palette : nullptr;
    const bool vp8 = media->frame_kind == AccountMediaFrameKind::vp8_keyframe ||
                     media->frame_kind == AccountMediaFrameKind::animated_webp_vp8;
    if (vp8) {
        const u32 alpha_offset = media->frame_webp_image.width * media->frame_webp_image.height * 4u;
        output.bytes =
            ConstSlice<u8>{media->pipeline.scanline_storage + (media->frame_bytes != 0 ? alpha_offset : 0u),
                           media->frame_bytes};
        output.vp8_frame = &media->frame_vp8;
        output.vp8_header = &media->frame_vp8_header;
        output.vp8_modes = media->pipeline.lossy_webp_workspace ? &media->pipeline.lossy_webp_workspace->modes : nullptr;
        output.vp8_coefficients =
            media->pipeline.lossy_webp_workspace ? &media->pipeline.lossy_webp_workspace->coefficients : nullptr;
    } else if (media->frame_kind == AccountMediaFrameKind::lottie_paint ||
               media->frame_kind == AccountMediaFrameKind::regional_flag_paint) {
        output.bytes = {reinterpret_cast<const u8*>(media->lottie_workspace), sizeof(ColrPaintProgram)};
    } else if (audio) {
        output.content_identity = media->audio_source.content_identity;
    } else if (video) {
        output.gpu_video = media->mp4_decoder->gpu;
    } else {
        output.bytes = ConstSlice<u8>{media->pipeline.scanline_storage, media->frame_bytes};
    }
    output.key = media->frame_key;
    output.request_id = media->frame_request_id;
    output.source_message_id = media->frame_source_message_id;
    output.duration_us = media->frame_duration_us;
    output.width = audio           ? 0
                   : video         ? output.gpu_video->width
                   : jpeg          ? jpeg_display_width(media->frame_jpeg_image)
                   : png_scanlines ? media->frame_image.width
                   : media->frame_kind == AccountMediaFrameKind::gif_indices
                       ? media->frame_gif.width
                       : media->frame_webp_image.width;
    output.height = audio           ? 0
                    : video         ? output.gpu_video->height
                    : jpeg          ? jpeg_display_height(media->frame_jpeg_image)
                    : png_scanlines ? media->frame_image.height
                    : media->frame_kind == AccountMediaFrameKind::gif_indices
                        ? media->frame_gif.height
                        : media->frame_webp_image.height;
    output.row_pitch = (video || audio) ? 0
                       : jpeg           ? jpeg_display_width(media->frame_jpeg_image) * 4u
                       : png_scanlines  ? media->frame_image.row_stride
                       : media->frame_kind == AccountMediaFrameKind::gif_indices ? media->frame_gif.width
                       : vp8 ? media->frame_webp_image.width
                             : media->frame_webp_image.width * 4;
    output.frame_index = media->frame_index;
    output.frame_count = media->frame_count;
    output.play_count = media->frame_play_count;
    output.source_message_revision = media->frame_source_message_revision;
    output.source_kind = media->frame_key.kind == MediaAssetKind::local_draft_preview
                             ? AccountMediaSourceKind::local_draft_preview
                         : media->frame_key.kind == MediaAssetKind::catalog_preview
                             ? AccountMediaSourceKind::catalog_preview
                         : media->frame_source_message_id && media->frame_source_message_revision
                             ? AccountMediaSourceKind::message
                             : AccountMediaSourceKind::none;
    output.source_catalog_scope = media->frame_catalog_scope;
    output.source_catalog_generation = media->frame_catalog_generation;
    if (media->frame_kind == AccountMediaFrameKind::apng_scanlines) {
        output.apng_frame = media->frame_apng;
    } else if (media->frame_kind == AccountMediaFrameKind::gif_indices) {
        output.gif_frame = media->frame_gif;
    } else if (media->frame_kind == AccountMediaFrameKind::animated_webp_rgba8 ||
               media->frame_kind == AccountMediaFrameKind::animated_webp_vp8) {
        output.animated_webp_frame = media->frame_animated_webp;
    }
    output.cache_generation = media->frame_cache_generation;
    output.canvas_width = media->frame_canvas_width;
    output.canvas_height = media->frame_canvas_height;
    output.kind = media->frame_kind;
    *frame = output;
    media->frame_ready = false;
    media->frame_key = MediaAssetKey{};
    media->frame_bytes = 0;
    media->frame_request_id = 0;
    media->frame_source_message_id = 0;
    media->frame_duration_us = 0;
    media->frame_index = 0;
    media->frame_count = 0;
    media->frame_play_count = 0;
    media->frame_source_message_revision = 0;
    media->frame_gif = GifFrame{};
    media->frame_catalog_scope = 0;
    media->frame_catalog_generation = 0;
    media->frame_cache_generation = 0;
    media->frame_canvas_width = 0;
    media->frame_canvas_height = 0;
    media->frame_kind = AccountMediaFrameKind::none;
    return true;
}

HANDLE account_media_notification_handle(const AccountMedia* media) noexcept {
    return media && media->started && media->client_started
               ? cdn_client_notification_handle(&media->client)
               : nullptr;
}

bool account_media_stop(AccountMedia* media, u32 timeout_ms) noexcept {
    if (!media || !media->started || timeout_ms == 0) {
        return false;
    }
    if (media->client_started) {
        const CdnClientResult stopped = cdn_client_stop(&media->client, timeout_ms);
        if (stopped.phase != CdnClientPhase::stopped) {
            return false;
        }
    }
    account_media_release_audio_source(media);
    if (media->collection) { release_collection(media->collection); media->collection = nullptr; }
    if (media->allocation) {
        VirtualFree(media->allocation, 0, MEM_RELEASE);
    }
    if (media->lottie_workspace) {
        arena_release(&static_cast<ColrPaintProgram*>(media->lottie_workspace)->storage);
        VirtualFree(media->lottie_workspace, 0, MEM_RELEASE);
    }
    if (media->expanded_scanlines)
        VirtualFree(media->expanded_scanlines, 0, MEM_RELEASE);
    if (media->webp_workspace) {
        VirtualFree(media->webp_workspace, 0, MEM_RELEASE);
    }
    if (media->lossy_webp_workspace) {
        VirtualFree(media->lossy_webp_workspace, 0, MEM_RELEASE);
    }
    if (media->animated_webp_workspace) {
        arena_release(
            &reinterpret_cast<AccountAnimatedWebpWorkspace*>(media->animated_webp_workspace)->frame_arena);
        VirtualFree(media->animated_webp_workspace, 0, MEM_RELEASE);
    }
    if (media->apng_workspace) {
        VirtualFree(media->apng_workspace, 0, MEM_RELEASE);
    }
    if (media->gif_workspace) {
        VirtualFree(media->gif_workspace, 0, MEM_RELEASE);
    }
    if (media->jpeg_workspace &&
        static_cast<void*>(media->jpeg_workspace) != media->lossy_webp_workspace) {
        VirtualFree(media->jpeg_workspace, 0, MEM_RELEASE);
    }
    account_media_release_video(media);
    if (media->mp4_decoder && media->mp4_decoder != &media->owned_mp4_decoder)
        VirtualFree(media->mp4_decoder, 0, MEM_RELEASE);
    if (media->provider_resolver) {
        embed_resolver_release(media->provider_resolver);
        VirtualFree(media->provider_resolver, 0, MEM_RELEASE);
    }
    SecureZeroMemory(media, sizeof(*media));
    return true;
}

void account_media_release_video(AccountMedia* media) noexcept {
    if (!media)
        return;
    if (media->mp4_decoder)
        mp4_decoder_release(media->mp4_decoder);
    if (media->progressive) {
        progressive_video_fetch_release(media->progressive);
        VirtualFree(media->progressive, 0, MEM_RELEASE);
        media->progressive = nullptr;
    }
    if (media->progressive_audio) {
        progressive_video_fetch_release(media->progressive_audio);
        VirtualFree(media->progressive_audio, 0, MEM_RELEASE); media->progressive_audio = nullptr;
    }
}

bool account_media_submit_progressive(AccountMedia* media, CdnByteRange range, bool audio) noexcept {
    auto* fetch = media ? (audio ? media->progressive_audio : media->progressive) : nullptr;
    return fetch && media->client_started && submit_progressive(media, range, fetch);
}
ProgressiveVideoAccept account_media_accept_progressive(AccountMedia* media, const CdnClientResponse& response,
                                                         ConstSlice<u8> body) noexcept {
    if (!media || !response.request_id) return ProgressiveVideoAccept::ignored;
    ProgressiveVideoFetch* sources[]{media->progressive, media->progressive_audio};
    for (auto* fetch : sources)
        if (fetch && fetch->inflight_id == response.request_id) {
            u64 reference=0;
            const auto* peer=fetch==sources[0]?sources[1]:sources[0];
            if(fetch->segmented && fetch->segmented->initialization && fetch->segmented->playlist.transport &&
                peer && peer->segmented && peer->segmented->store.transport && peer->segmented->store.initialization.base) {
                // First-file arrival order must not pick two different epochs
                // when the 33-bit transport clock wraps between audio/video.
                const auto& track=peer->segmented->store.track;
                if(track.media_start>=0 && track.timescale) {
                    const u64 first=static_cast<u64>(track.media_start),seconds=first/track.timescale;
                    if(seconds<(~u64{0})/90000) reference=seconds*90000+(first%track.timescale)*90000/track.timescale;
                }
            }
            return progressive_video_fetch_accept(fetch, response.request_id, response.transport_ok(),
                                                   cdn_client_response_policy(response), body,reference);
        }
    return ProgressiveVideoAccept::ignored;
}
ProgressiveVideoNext account_media_progressive_next(AccountMedia* media, bool read_ahead, u64 now,
                                                    ProgressiveVideoFetch** selected) noexcept {
    if (!media || !media->progressive || !selected) return {{}, ProgressiveVideoStep::failed};
    auto* video = media->progressive; auto* audio = media->progressive_audio;
    ProgressiveVideoFetch* sources[]{video, audio};
    const auto pending = progressive_video_fetch_pending;
    for (auto* f : sources) if (f) {
        if (!f->segmented) progressive_source_evict(&f->source, progressive_source_budget(f->source));
        if (f->state == ProgressiveVideoFetchState::failed) {
            video->state = ProgressiveVideoFetchState::failed; *selected = f; return {{}, ProgressiveVideoStep::failed};
        }
    }
    // One shared CDN request at a time. Urgent audio may preempt video read-ahead
    // after a seek; the cancelled response must retire before another submit.
    for (auto* f : sources) if (f && f->inflight_id) {
        *selected = f;
        if (f == video && pending(audio) && !f->cancelling) return {{}, ProgressiveVideoStep::cancel};
        return progressive_video_fetch_next(f, read_ahead, now);
    }
    for (auto* f : sources) if (f && f->state != ProgressiveVideoFetchState::ready) {
        *selected = f; return progressive_video_fetch_next(f, false, now);
    }
    const auto synchronized = video->kind == ProgressiveVideoKind::fragmented_audio
        ? Mp4DemuxError::none : segmented_video_synchronize(video, audio);
    if (synchronized == Mp4DemuxError::data_pending) {
        for (auto* f : sources) if (pending(f)) { *selected = f; return progressive_video_fetch_next(f, false, now); }
    }
    if (synchronized != Mp4DemuxError::none) {
        video->state = ProgressiveVideoFetchState::failed; *selected = video; return {{}, ProgressiveVideoStep::failed};
    }
    ProgressiveVideoFetch* ordered[]{pending(audio) ? audio : video, pending(audio) ? video : audio};
    for (auto* f : ordered) if (pending(f)) {
        const auto next = progressive_video_fetch_next(f, false, now);
        if (next.step != ProgressiveVideoStep::idle) { *selected = f; return next; }
    }
    // Fill the much smaller audio window first; video uses the remaining turns.
    ProgressiveVideoFetch* ahead[]{audio, video};
    for (auto* f : ahead) if (f) {
        const auto next = progressive_video_fetch_next(f, read_ahead, now);
        if (next.step != ProgressiveVideoStep::idle) { *selected = f; return next; }
    }
    *selected = video; return {};
}
void account_media_cancel_provider(AccountMedia* media) noexcept {
    if (!media) return;
    auto* resolver = static_cast<EmbedResolver*>(InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(&media->provider_resolver), nullptr, nullptr));
    if (resolver) embed_resolver_cancel(resolver);
}

void account_media_cache_progressive(AccountMedia* media, MediaAssetKey key) noexcept {
    auto* fetch = media ? media->progressive : nullptr;
    if (!fetch || fetch->kind != ProgressiveVideoKind::movie || fetch->cached || !progressive_source_complete(fetch->source) ||
        fetch->source.total_bytes > kAccountMediaProgressiveCacheMaximumBytes)
        return;
    fetch->cached = true;
    encoded_media_cache_store(media->pipeline.cache, key, progressive_source_bytes(fetch->source),
                              {{}, fetch->cache_expires_at_seconds, fetch->cache_store_allowed,
                               fetch->cache_must_revalidate});
}

} // namespace maicord
