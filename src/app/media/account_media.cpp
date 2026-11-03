#include "account_media_internal.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/core/animation_timeline.hpp"
#include "maicord/app/media/segmented_video_fetch.hpp"
#include "maicord/app/media/embed_resolver.hpp"
#include "maicord/protocol/media/embed_provider.hpp"
#include "maicord/ui/text/regional_flag.hpp"
#include "maicord/media/image/regional_flag_svg.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace account_media_detail {

// A committed private allocation; null on failure.
__declspec(noinline) void* commit_pages(usize bytes) noexcept {
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
ProgressiveVideoFetch* allocate_fetch() noexcept {
    return static_cast<ProgressiveVideoFetch*>(commit_pages(sizeof(ProgressiveVideoFetch)));
}

bool collection_text_reserve(CollectionText& text, u32 required) noexcept {
    if (required > 8u * 1024 * 1024) return false;
    if (required <= text.capacity) return true;
    u32 capacity = text.capacity ? text.capacity : 4096;
    while (capacity < required) capacity *= 2;
    auto* data = static_cast<char*>(commit_pages(capacity));
    if (!data) return false;
    copy_bytes(data, text.data, text.bytes);
    if (text.data) VirtualFree(text.data, 0, MEM_RELEASE);
    text.data = data; text.capacity = capacity; return true;
}

void release_collection(AccountMediaCollection* collection) noexcept {
    if (!collection) return;
    if (collection->titles.data) VirtualFree(collection->titles.data, 0, MEM_RELEASE);
    VirtualFree(collection, 0, MEM_RELEASE);
}

} // namespace account_media_detail
namespace {

inline constexpr u32 kCacheEntryStorageBytes =
    kAccountMediaCacheEntryCapacity * sizeof(EncodedMediaCacheEntry);
inline constexpr u32 kCachePageOwnerBytes = kAccountMediaCachePageCapacity * sizeof(u16);
inline constexpr u32 kCacheIndexBytes = kAccountMediaCacheIndexCapacity * sizeof(u32);
inline constexpr u32 kCacheStorageBytes = kAccountMediaCachePageCapacity * kEncodedMediaCachePageBytes;
inline constexpr u32 kScanlineStorageBytes = maximum(2'097'664u, kGifPixelMaximum);
inline constexpr u32 kOwnedAllocationBytes = kCacheEntryStorageBytes + kCachePageOwnerBytes +
                                             kCacheIndexBytes + kCacheStorageBytes + kScanlineStorageBytes;

} // namespace
namespace account_media_detail {

AccountMediaRequestResult result(AccountMediaRequestState state,
                                 AccountMediaError error) noexcept {
    AccountMediaRequestResult value{};
    value.state = state;
    value.error = error;
    return value;
}

#include "account_media_classify.inc"

bool png_animation_body(const ConstSlice<u8>& body) noexcept {
    static constexpr u8 signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!body.data || body.count < sizeof(signature) || !bytes_equal(body.data, signature, sizeof(signature)))
        return false;
    for (u32 cursor = sizeof(signature); body.count - cursor >= 12;) {
        const u32 length = byte_load_be32(body.data + cursor);
        if (length > body.count - cursor - 12)
            return false;
        const u8* type = body.data + cursor + 4;
        if (bytes_equal(type, "acTL", 4))
            return length == 8;
        if (bytes_equal(type, "IDAT", 4))
            return false;
        cursor += length + 12;
    }
    return false;
}

bool catalog_png_cached_animation(AccountMedia* media, const MediaAssetKey& key, u64 now_seconds) noexcept {
    if (!catalog_png_key(key))
        return false;
    auto lease = encoded_media_cache_acquire(media->pipeline.cache, key, now_seconds);
    const bool animation = lease.acquired && png_animation_body(lease.lookup.bytes);
    if (lease.acquired)
        encoded_media_cache_release(media->pipeline.cache, &lease);
    return animation;
}

} // namespace account_media_detail
namespace {

AccountMediaFailure* find_failure(AccountMedia* media, const MediaAssetKey& key) noexcept {
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

} // namespace
namespace account_media_detail {

void clear_failure(AccountMedia* media, const MediaAssetKey& key) noexcept {
    if (AccountMediaFailure* failed = find_failure(media, key))
        reset_value<AccountMediaFailure>(failed);
}

// Records the failure with its retry deadline and returns it.
AccountMediaRequestResult remember_failure(AccountMedia* media, const MediaAssetKey& key_view, u64 now_ms,
                                           const AccountMediaRequestResult& failure_view) noexcept {
    const MediaAssetKey key = key_view; // the media state is rewritten below
    AccountMediaRequestResult failure = failure_view;
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
    return failure;
}

// Publishes the staged frame.
void mark_frame_ready(AccountMedia* media, const MediaAssetKey& key_view) noexcept {
    const MediaAssetKey key = key_view; // the media state is rewritten below
    media->frame_ready = true;
    reset_value<AccountMediaRequestResult>(&media->last_failure);
    clear_failure(media, key);
}

void capture_catalog_source(AccountMedia* media, const MediaAssetKey& key_view) noexcept {
    const MediaAssetKey key = key_view; // the media state is rewritten below
    media->frame_catalog_scope =
        key.kind == MediaAssetKind::catalog_preview || key.kind == MediaAssetKind::local_draft_preview
            ? media->active_catalog_scope
            : 0;
    media->frame_catalog_generation =
        key.kind == MediaAssetKind::catalog_preview || key.kind == MediaAssetKind::local_draft_preview
            ? media->active_catalog_generation
            : 0;
}

// The identity every published frame carries. The caller sets the frame kind
// and animation fields, then marks the frame ready.
void stage_frame(AccountMedia* media, const MediaAssetKey& key_view, u32 bytes, u64 request_id,
                 u64 source_message_id, u32 source_message_revision) noexcept {
    const MediaAssetKey key = key_view; // the media state is rewritten below
    media->frame_key = key;
    media->frame_bytes = bytes;
    media->frame_request_id = request_id;
    media->frame_source_message_id = source_message_id;
    media->frame_source_message_revision = source_message_revision;
    capture_catalog_source(media, key);
}

void publish_png_frame(AccountMedia* media, const MediaAssetKey& key, const CdnStaticPngResult& decoded,
                       u64 request_id, u64 source_message_id,
                       u32 source_message_revision) noexcept {
    media->frame_image = decoded.image;
    stage_frame(media, key, decoded.scanlines.count, request_id, source_message_id,
                source_message_revision);
    media->frame_kind = AccountMediaFrameKind::png_scanlines;
    mark_frame_ready(media, key);
}

void publish_webp_frame(AccountMedia* media, const MediaAssetKey& key, const CdnStaticWebpResult& decoded,
                        u64 request_id, u64 source_message_id,
                        u32 source_message_revision) noexcept {
    copy_value(&media->frame_webp_image, decoded.image);
    const bool lossy = decoded.image.codec == WebpCodec::vp8_lossy;
    if (lossy) {
        copy_value(&media->frame_vp8, decoded.vp8_frame);
        media->frame_vp8_header = decoded.vp8_header;
    }
    stage_frame(media, key, lossy ? decoded.alpha.count : decoded.rgba8.count, request_id,
                source_message_id, source_message_revision);
    media->frame_kind = lossy ? AccountMediaFrameKind::vp8_keyframe : AccountMediaFrameKind::straight_rgba8;
    mark_frame_ready(media, key);
}

void publish_jpeg_frame(AccountMedia* media, const MediaAssetKey& key, const CdnStaticJpegResult& decoded,
                        u64 request_id, u64 source_message_id, u32 source_message_revision) noexcept {
    const bool reduced = decoded.rgba8.count != 0;
    media->frame_jpeg_image = decoded.image;
    if (reduced) {
        reset_value<WebpImage>(&media->frame_webp_image);
        media->frame_webp_image.width = decoded.rgba_width;
        media->frame_webp_image.height = decoded.rgba_height;
    }
    stage_frame(media, key, reduced ? decoded.rgba8.count : 0, request_id, source_message_id,
                source_message_revision);
    media->frame_kind =
        reduced ? AccountMediaFrameKind::straight_rgba8 : AccountMediaFrameKind::jpeg_coefficients;
    mark_frame_ready(media, key);
}

} // namespace account_media_detail
namespace {

// The playback fields every animated codec result publishes.
template <typename Result> void stage_animation(AccountMedia* media, const Result& decoded) noexcept {
    media->frame_duration_us = decoded.decode.duration_us;
    media->frame_index = decoded.decode.frame_index;
    media->frame_count = decoded.frame_count;
    media->frame_play_count = decoded.play_count;
    media->frame_cache_generation = decoded.cache_generation;
    media->frame_canvas_width = static_cast<u16>(decoded.canvas_width);
    media->frame_canvas_height = static_cast<u16>(decoded.canvas_height);
}

} // namespace
namespace account_media_detail {

void publish_animated_webp_frame(AccountMedia* media, const MediaAssetKey& key,
                                 const CdnAnimatedWebpResult& decoded, u64 request_id) noexcept {
    const bool lossy = decoded.decode.frame.codec == AnimatedWebpCodec::vp8_lossy;
    media->frame_webp_image = decoded.decode.frame_image;
    media->frame_animated_webp = decoded.decode.frame;
    if (lossy) {
        copy_value(&media->frame_vp8, decoded.vp8_frame);
        media->frame_vp8_header = decoded.vp8_header;
    }
    stage_frame(media, key, lossy ? decoded.alpha.count : decoded.rgba8.count, request_id,
                media->active_source_message_id, media->active_source_message_revision);
    stage_animation(media, decoded);
    media->frame_kind =
        lossy ? AccountMediaFrameKind::animated_webp_vp8 : AccountMediaFrameKind::animated_webp_rgba8;
    mark_frame_ready(media, key);
}

void publish_apng_frame(AccountMedia* media, const MediaAssetKey& key, const CdnApngResult& decoded,
                        u64 request_id) noexcept {
    media->frame_image = decoded.image;
    media->frame_apng = decoded.decode.frame;
    stage_frame(media, key, decoded.scanlines.count, request_id, media->active_source_message_id,
                media->active_source_message_revision);
    stage_animation(media, decoded);
    media->frame_kind = AccountMediaFrameKind::apng_scanlines;
    mark_frame_ready(media, key);
}

void publish_gif_frame(AccountMedia* media, const MediaAssetKey& key, const CdnGifResult& decoded,
                       u64 request_id) noexcept {
    copy_value(&media->frame_gif, decoded.decode.frame);
    media->frame_gif_palette = decoded.decode.palette;
    stage_frame(media, key, decoded.indices.count, request_id, media->active_source_message_id,
                media->active_source_message_revision);
    stage_animation(media, decoded);
    media->frame_kind = AccountMediaFrameKind::gif_indices;
    mark_frame_ready(media, key);
}

void publish_mp4_frame(AccountMedia* media, const MediaAssetKey& key_view, const Mp4DecodedFrame& decoded,
                       u64 request_id, u64 source_message_id, u32 source_message_revision,
                       u32 frame_index) noexcept {
    const MediaAssetKey key = key_view; // the media state is rewritten below
    stage_frame(media, key, 0, request_id, source_message_id, source_message_revision);
    media->frame_duration_us = decoded.duration_us;
    media->frame_index = frame_index;
    media->frame_count = kAnimationTimelineFrameMaximum;
    media->frame_play_count = 0;
    media->frame_cache_generation = media->mp4_generation;
    media->frame_canvas_width = decoded.display_width;
    media->frame_canvas_height = decoded.display_height;
    media->frame_kind = AccountMediaFrameKind::video_nv12_gpu;
    media->mp4_used_ms = clock_monotonic_milliseconds();
    mark_frame_ready(media, key);
}

bool audio_image_type(CdnImageType type) noexcept {
    return type == CdnImageType::ogg_opus || type == CdnImageType::mp3 ||
           type == CdnImageType::wav || type == CdnImageType::m4a;
}

bool publish_audio_frame(AccountMedia* media, const MediaAssetKey& key, u64 request_id) noexcept {
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
    mark_frame_ready(media, key);
    return true;
}

bool publish_audio_source(AccountMedia* media, const MediaAssetKey& key, CdnEncodedAudioResult* decoded,
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
            commit_pages(source.bytes.count);
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
        reset_value(&decoded->lease);
        source.content_identity = source.lease.lookup.content_identity;
        source.cache_generation = source.lease.generation;
    }
    source.source_message_id = media->active_source_message_id;
    source.source_message_revision = media->active_source_message_revision;
    source.duration_us = encoded_audio_duration_us(source.metadata);
    account_media_release_audio_source(media);
    copy_value(&media->audio_source, source);
    return publish_audio_frame(media, key, request_id);
}

} // namespace account_media_detail
namespace {

// Reserves a codec workspace on first use and binds it to the pipeline.
template <typename Workspace, typename Bind> bool ensure_workspace(Workspace*& slot, Bind bind) noexcept {
    if (slot)
        return true;
    void* allocation = commit_pages(sizeof(Workspace));
    if (!allocation)
        return false;
    slot = static_cast<Workspace*>(allocation);
    if (bind(slot))
        return true;
    VirtualFree(allocation, 0, MEM_RELEASE);
    slot = nullptr;
    return false;
}

} // namespace
namespace account_media_detail {

bool ensure_webp_workspace(AccountMedia* media) noexcept {
    return ensure_workspace(media->webp_workspace, [media](Vp8lWorkspace* workspace) noexcept {
        return cdn_media_pipeline_bind_webp_workspace(&media->pipeline, workspace);
    });
}

} // namespace account_media_detail
namespace {

static_assert(sizeof(JpegWorkspace) >= sizeof(LossyWebpWorkspace));
static_assert(is_trivially_default_constructible_v<JpegWorkspace>);
static_assert(is_trivially_default_constructible_v<LossyWebpWorkspace>);

// Both codecs finish before the worker publishes one borrowed frame. Keep one
// reservation, and commit the larger extent only when JPEG first needs it.
void* allocate_image_workspace(void* existing, usize bytes) noexcept {
    if (existing)
        return VirtualAlloc(existing, bytes, MEM_COMMIT, PAGE_READWRITE);
    if (bytes == sizeof(JpegWorkspace))
        return commit_pages(bytes);
    void* allocation = VirtualAlloc(nullptr, sizeof(JpegWorkspace), MEM_RESERVE, PAGE_READWRITE);
    if (allocation && !VirtualAlloc(allocation, bytes, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return nullptr;
    }
    return allocation;
}

} // namespace
namespace account_media_detail {

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

} // namespace account_media_detail
namespace {

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

} // namespace
namespace account_media_detail {

bool ensure_animated_webp_workspace(AccountMedia* media) noexcept {
    if (media->animated_webp_workspace) {
        return true;
    }
    void* allocation = commit_pages(sizeof(AccountAnimatedWebpWorkspace));
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

Mp4Decoder* ensure_mp4_decoder(AccountMedia* media) noexcept {
    if (!media->mp4_decoder)
        media->mp4_decoder = &media->owned_mp4_decoder;
    return media->mp4_decoder;
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

bool submit_progressive(AccountMedia* media, CdnByteRange range, ProgressiveVideoFetch* selected) noexcept {
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

} // namespace account_media_detail
namespace {

Slice<u8> grow_scanlines(void* owner, u32 bytes) noexcept {
    auto* media = static_cast<AccountMedia*>(owner);
    if (!bytes || bytes > kPngDecodeBufferMaximumBytes || media->frame_ready)
        return {};
    const u32 capacity = (bytes + 4095u) & ~4095u;
    auto* replacement =
        static_cast<u8*>(commit_pages(capacity));
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
        media->allocation = commit_pages(kScanlineStorageBytes);
    return static_cast<u8*>(media->allocation);
}

AccountMediaRequestResult initialize_media(AccountMedia* media, EncodedMediaCache* shared_cache) noexcept {
    if (!media || (shared_cache && (!shared_cache->entries || !shared_cache->storage))) {
        return result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    SecureZeroMemory(media, sizeof(*media));
    const u32 allocation_bytes = shared_cache ? kScanlineStorageBytes : kOwnedAllocationBytes;
    media->allocation = commit_pages(allocation_bytes);
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
namespace account_media_detail {

bool probe_static_jpeg(AccountMedia* media, CacheProbe* probe, AccountMediaRequestResult* settled) noexcept {
    CdnStaticJpegResult cached;
    cdn_media_pipeline_lookup_static_jpeg(&cached, &media->pipeline, probe->key, probe->unix_seconds);
    if (cached.state == CdnStaticJpegState::jpeg_workspace_required) {
        if (!ensure_jpeg_workspace(media)) {
            *settled = result(AccountMediaRequestState::failed, AccountMediaError::memory);
            return true;
        }
        cdn_media_pipeline_lookup_static_jpeg(&cached, &media->pipeline, probe->key, probe->unix_seconds);
    }
    return settle_probe(
        media, probe, settled, cached,
        [&]() noexcept {
            publish_jpeg_frame(media, probe->key, cached, 0, probe->source_message_id,
                               probe->source_message_revision);
        },
        [&](AccountMediaRequestResult* failure) noexcept { failure->jpeg_error = cached.decode.error; });
}

bool probe_static_webp(AccountMedia* media, CacheProbe* probe, AccountMediaRequestResult* settled) noexcept {
    CdnStaticWebpResult cached;
    cdn_media_pipeline_lookup_static_webp(&cached, &media->pipeline, probe->key, probe->unix_seconds);
    if (cached.state == CdnStaticWebpState::lossy_workspace_required) {
        if (!ensure_lossy_webp_workspace(media)) {
            *settled = result(AccountMediaRequestState::failed, AccountMediaError::memory);
            return true;
        }
        cdn_media_pipeline_lookup_static_webp(&cached, &media->pipeline, probe->key, probe->unix_seconds);
    }
    return settle_probe(
        media, probe, settled, cached,
        [&]() noexcept {
            publish_webp_frame(media, probe->key, cached, 0, probe->source_message_id,
                               probe->source_message_revision);
        },
        [&](AccountMediaRequestResult* failure) noexcept {
            failure->webp_error = cached.decode.error;
            failure->lossy_webp_error = cached.lossy_decode.error;
        });
}

bool probe_apng(AccountMedia* media, CacheProbe* probe, AccountMediaRequestResult* settled) noexcept {
    CdnApngResult cached;
    cdn_media_pipeline_lookup_apng(&cached, &media->pipeline, probe->key, probe->unix_seconds);
    return settle_probe(
        media, probe, settled, cached, [&]() noexcept { publish_apng_frame(media, probe->key, cached, 0); },
        [&](AccountMediaRequestResult* failure) noexcept { failure->apng_error = cached.decode.error; });
}

bool probe_gif(AccountMedia* media, CacheProbe* probe, AccountMediaRequestResult* settled) noexcept {
    CdnGifResult cached;
    cdn_media_pipeline_lookup_gif(&cached, &media->pipeline, probe->key, probe->unix_seconds);
    return settle_probe(
        media, probe, settled, cached, [&]() noexcept { publish_gif_frame(media, probe->key, cached, 0); },
        [&](AccountMediaRequestResult* failure) noexcept { failure->gif_error = cached.decode.error; });
}

bool probe_static_png(AccountMedia* media, CacheProbe* probe, AccountMediaRequestResult* settled) noexcept {
    CdnStaticPngResult cached;
    cdn_media_pipeline_lookup_static_png(&cached, &media->pipeline, probe->key, probe->unix_seconds);
    return settle_probe(
        media, probe, settled, cached,
        [&]() noexcept {
            publish_png_frame(media, probe->key, cached, 0, probe->source_message_id,
                              probe->source_message_revision);
        },
        [&](AccountMediaRequestResult* failure) noexcept { failure->png_error = cached.decode.error; });
}

// The staged or in-flight request, if any, settles a new request for `key`.
bool request_in_flight(const AccountMedia* media, const MediaAssetKey& key,
                       AccountMediaRequestResult* settled) noexcept {
    if (media->frame_ready)
        *settled = result(media_asset_key_equal(media->frame_key, key) ? AccountMediaRequestState::ready
                                                                       : AccountMediaRequestState::busy);
    else if (media->active)
        *settled = result(media_asset_key_equal(media->active_key, key) ? AccountMediaRequestState::pending
                                                                        : AccountMediaRequestState::busy);
    else
        return false;
    return true;
}

// A remembered failure still cooling down settles the request; an expired one is forgotten.
bool failure_cooling(AccountMedia* media, const MediaAssetKey& key, u64 now_ms,
                     AccountMediaRequestResult* settled) noexcept {
    AccountMediaFailure* failed = find_failure(media, key);
    if (!failed)
        return false;
    if (now_ms >= failed->retry_at_ms) {
        reset_value<AccountMediaFailure>(failed);
        return false;
    }
    *settled = result(AccountMediaRequestState::retry_wait);
    settled->retry_at_ms = failed->retry_at_ms;
    return true;
}

// A fresh cached Lottie publishes; a stale entry's validator goes to probe->etag. A fresh entry that
// no longer compiles is erased and reported rejected.
LottieProbe probe_lottie(AccountMedia* media, CacheProbe* probe) noexcept {
    auto lease = encoded_media_cache_acquire(media->pipeline.cache, probe->key, probe->unix_seconds);
    const bool fresh = lease.acquired && lease.lookup.state == EncodedMediaCacheLookupState::fresh;
    const bool published = fresh && publish_vector(media, probe->key, lease.lookup.bytes, 0);
    if (published)
        media->frame_cache_generation = lease.lookup.generation;
    if (lease.acquired && !fresh && lease.lookup.etag.count <= sizeof(media->pipeline.result_etag)) {
        media->pipeline.result_etag_bytes = lease.lookup.etag.count;
        copy_bytes(media->pipeline.result_etag, lease.lookup.etag.data, lease.lookup.etag.count);
        probe->etag = {media->pipeline.result_etag, media->pipeline.result_etag_bytes};
    }
    if (lease.acquired)
        encoded_media_cache_release(media->pipeline.cache, &lease);
    if (fresh && !published)
        encoded_media_cache_erase(media->pipeline.cache, probe->key);
    return published ? LottieProbe::published : fresh ? LottieProbe::rejected : LottieProbe::miss;
}

// Queues the client request for `key` and marks it active; a refused
// submission is remembered.
AccountMediaRequestResult submit_fetch(AccountMedia* media, const MediaAssetKey& key_view, u64 now_ms,
                                       CdnClientRequest request) noexcept {
    const MediaAssetKey key = key_view; // the media state is rewritten below
    if (++media->request_id == 0)
        ++media->request_id;
    request.request_id = media->request_id;
    if (!cdn_client_submit(&media->client, request).ok())
        return remember_failure(media, key, now_ms,
                                result(AccountMediaRequestState::failed, AccountMediaError::client));
    media->active_key = key;
    media->active_request_id = media->request_id;
    media->active = true;
    return result(AccountMediaRequestState::queued);
}

} // namespace account_media_detail

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

bool account_media_video_matches(const AccountMedia* media, const MediaAssetKey& key,
                                 u16 cache_generation) noexcept {
    return media && media->mp4_decoder && media->mp4_decoder->ready && video_asset(key) &&
           (!media->progressive || media->progressive->state != ProgressiveVideoFetchState::failed) &&
           media_asset_key_equal(media->mp4_key, key) &&
           (cache_generation == 0 || media->mp4_generation == cache_generation);
}

bool account_media_audio_matches(const AccountMedia* media, const MediaAssetKey& key,
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

AccountMediaCollectionState account_media_collection_state(const AccountMedia* media, const MediaAssetKey& key, bool ready) noexcept {
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
    reset_value(title);
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
    reset_value(out); copy_bytes(out->client,queue.client,sizeof(out->client));
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
    reset_value(&media->audio_source);
}

AccountMediaVideoFailure account_media_video_body_failure(const ConstSlice<u8>& body) noexcept {
    if (!body.data)
        return AccountMediaVideoFailure::none;
    if (body.count >= 6 && bytes_equal(body.data, "GIF8", 4) && (body[4] == '7' || body[4] == '9') && body[5] == 'a')
        return AccountMediaVideoFailure::unexpected_gif;
    if (body.count >= 8 && bytes_equal(body.data, "\x89PNG\r\n\x1a\n", 8))
        return AccountMediaVideoFailure::unexpected_png;
    if (body.count >= 12 && bytes_equal(body.data, "RIFF", 4) && bytes_equal(body.data + 8, "WEBP", 4))
        return AccountMediaVideoFailure::unexpected_webp;
    if (body.count >= 3 && body[0] == 0xff && body[1] == 0xd8 && body[2] == 0xff)
        return AccountMediaVideoFailure::unexpected_jpeg;
    if (body.count >= 12 && bytes_equal(body.data + 4, "ftypavi", 7) && (body[11] == 'f' || body[11] == 's'))
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

AccountMediaRequestResult account_media_advance_animation(AccountMedia* media, const MediaAssetKey& key,
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
        CdnAnimatedWebpResult decoded;
        cdn_media_pipeline_decode_animated_webp_frame(&decoded, &media->pipeline, key, cache_generation,
                                                      frame_index, unix_now_seconds);
        if (decoded.state == CdnAnimatedWebpState::lossy_workspace_required) {
            if (!ensure_lossy_webp_workspace(media)) {
                return result(AccountMediaRequestState::failed, AccountMediaError::memory);
            }
            cdn_media_pipeline_decode_animated_webp_frame(&decoded, &media->pipeline, key, cache_generation,
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
        CdnGifResult decoded;
        cdn_media_pipeline_decode_gif_frame(&decoded, &media->pipeline, key, cache_generation, frame_index,
                                            unix_now_seconds);
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
    CdnApngResult decoded;
    cdn_media_pipeline_decode_apng_frame(&decoded, &media->pipeline, key, cache_generation, frame_index,
                                         unix_now_seconds);
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
    reset_value<MediaAssetKey>(&media->active_key);
    media->active_request_id = 0;
    media->active_source_message_id = 0;
    media->active_source_message_revision = 0;
    media->active_proxy_width = 0;
    media->active_catalog_scope = 0;
    media->active_catalog_generation = 0;
    media->active_proxy_height = 0;
    return true;
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
    reset_value<MediaAssetKey>(&media->frame_key);
    media->frame_bytes = 0;
    media->frame_request_id = 0;
    media->frame_source_message_id = 0;
    media->frame_duration_us = 0;
    media->frame_index = 0;
    media->frame_count = 0;
    media->frame_play_count = 0;
    media->frame_source_message_revision = 0;
    reset_value<GifFrame>(&media->frame_gif);
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
                                                         const ConstSlice<u8>& body) noexcept {
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
    auto* resolver = *static_cast<EmbedResolver* const volatile*>(&media->provider_resolver);
    if (resolver) embed_resolver_cancel(resolver);
}

void account_media_cache_progressive(AccountMedia* media, const MediaAssetKey& key) noexcept {
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
