#pragma once

#include "maicord/media/animated_webp.hpp"
#include "maicord/media/apng.hpp"
#include "maicord/media/encoded_cache.hpp"
#include "maicord/media/gif.hpp"
#include "maicord/media/jpeg.hpp"
#include "maicord/media/lossy_webp.hpp"
#include "maicord/media/png.hpp"
#include "maicord/media/encoded_audio.hpp"
#include "maicord/media/webp.hpp"
#include "maicord/protocol/cdn_http.hpp"

namespace maicord {

enum class CdnStaticPngState : u8 {
    invalid,
    cache_miss,
    fetch_required,
    decoded_cache_hit,
    decoded_response,
    decoded_transient,
    http_rejected,
    decode_rejected,
    uncorrelated_not_modified,
};

struct CdnStaticPngResult {
    PngImage image;
    PngDecodeResult decode;
    EncodedMediaCacheStoreResult store;
    ConstSlice<u8> scanlines;
    ConstSlice<u8> etag;
    CdnStaticPngState state;

    constexpr bool decoded() const noexcept {
        return state == CdnStaticPngState::decoded_cache_hit ||
               state == CdnStaticPngState::decoded_response ||
               state == CdnStaticPngState::decoded_transient;
    }
};

enum class CdnStaticJpegState : u8 {
    invalid,
    cache_miss,
    fetch_required,
    decoded_cache_hit,
    decoded_response,
    decoded_transient,
    http_rejected,
    decode_rejected,
    uncorrelated_not_modified,
    jpeg_workspace_required,
};

// JPEGs wider or taller than kJpegDimensionMaximum decode on the CPU at the
// coarsest DCT scale that still reaches the drawn extent: the activity card's
// 160 px for profile images (Spotify's 640 px album art), and the full
// rendition, up to the preview cap, for attachments and embeds.
inline constexpr u32 kReducedJpegExtent = 160;
inline constexpr u32 kReducedJpegPreviewExtent = 2048;

struct CdnStaticJpegResult {
    JpegImage image;
    JpegDecodeResult decode;
    EncodedMediaCacheStoreResult store;
    // Null for a reduced image, which is oriented straight RGBA8 in rgba8.
    const JpegCoefficientWorkspace* coefficients;
    ConstSlice<u8> rgba8;
    u16 rgba_width, rgba_height;
    ConstSlice<u8> etag;
    CdnStaticJpegState state;

    constexpr bool decoded() const noexcept {
        return state == CdnStaticJpegState::decoded_cache_hit ||
               state == CdnStaticJpegState::decoded_response ||
               state == CdnStaticJpegState::decoded_transient;
    }
};

enum class CdnStaticWebpState : u8 {
    invalid,
    cache_miss,
    fetch_required,
    decoded_cache_hit,
    decoded_response,
    decoded_transient,
    http_rejected,
    decode_rejected,
    uncorrelated_not_modified,
    lossy_workspace_required,
};

struct CdnStaticWebpResult {
    WebpImage image;
    WebpDecodeResult decode;
    LossyWebpDecodeResult lossy_decode;
    Vp8Keyframe vp8_frame;
    Vp8ControlHeader vp8_header;
    EncodedMediaCacheStoreResult store;
    ConstSlice<u8> rgba8;
    ConstSlice<u8> alpha;
    ConstSlice<u8> etag;
    CdnStaticWebpState state;

    constexpr bool decoded() const noexcept {
        return state == CdnStaticWebpState::decoded_cache_hit ||
               state == CdnStaticWebpState::decoded_response ||
               state == CdnStaticWebpState::decoded_transient;
    }
};

enum class CdnApngState : u8 {
    invalid,
    cache_miss,
    fetch_required,
    generation_mismatch,
    decoded_cache_hit,
    decoded_response,
    decoded_transient,
    http_rejected,
    decode_rejected,
    uncorrelated_not_modified,
};

struct CdnApngResult {
    PngImage image;
    ApngDecodeResult decode;
    EncodedMediaCacheStoreResult store;
    ConstSlice<u8> scanlines;
    ConstSlice<u8> etag;
    u32 frame_count;
    u32 play_count;
    u32 canvas_width;
    u32 canvas_height;
    u16 cache_generation;
    CdnApngState state;
    u8 reserved;

    constexpr bool decoded() const noexcept {
        return state == CdnApngState::decoded_cache_hit || state == CdnApngState::decoded_response ||
               state == CdnApngState::decoded_transient;
    }
};

enum class CdnGifState : u8 {
    invalid,
    cache_miss,
    fetch_required,
    generation_mismatch,
    decoded_cache_hit,
    decoded_response,
    decoded_transient,
    http_rejected,
    decode_rejected,
    uncorrelated_not_modified,
};

struct CdnGifResult {
    GifDecodeResult decode;
    EncodedMediaCacheStoreResult store;
    ConstSlice<u8> indices;
    ConstSlice<u8> etag;
    u32 frame_count;
    u32 play_count;
    u32 canvas_width;
    u32 canvas_height;
    u16 cache_generation;
    CdnGifState state;
    u8 reserved;

    constexpr bool decoded() const noexcept {
        return state == CdnGifState::decoded_cache_hit || state == CdnGifState::decoded_response ||
               state == CdnGifState::decoded_transient;
    }
};

enum class CdnAnimatedWebpState : u8 {
    invalid,
    cache_miss,
    fetch_required,
    generation_mismatch,
    decoded_cache_hit,
    decoded_response,
    decoded_transient,
    http_rejected,
    decode_rejected,
    uncorrelated_not_modified,
    lossy_workspace_required,
};

struct CdnAnimatedWebpResult {
    AnimatedWebpDecodeResult decode;
    Vp8Keyframe vp8_frame;
    Vp8ControlHeader vp8_header;
    EncodedMediaCacheStoreResult store;
    ConstSlice<u8> rgba8;
    ConstSlice<u8> alpha;
    ConstSlice<u8> etag;
    u32 background_rgba;
    u32 frame_count;
    u32 play_count;
    u16 cache_generation;
    u16 canvas_width;
    u16 canvas_height;
    CdnAnimatedWebpState state;
    u8 reserved;

    constexpr bool decoded() const noexcept {
        return state == CdnAnimatedWebpState::decoded_cache_hit ||
               state == CdnAnimatedWebpState::decoded_response ||
               state == CdnAnimatedWebpState::decoded_transient;
    }
};

struct CdnMediaPipeline {
    EncodedMediaCache* cache;
    Vp8lWorkspace* webp_workspace;
    LossyWebpWorkspace* lossy_webp_workspace;
    AnimatedWebpWorkspace* animated_webp_workspace;
    ApngWorkspace* apng_workspace;
    GifWorkspace* gif_workspace;
    // The identity covers immutable encoded bytes. The image borrows only this lane's workspace.
    // Clear the identity before any direct gif_parse call on that workspace.
    GifImage gif_image;
    GifParseResult gif_parse_result;
    u64 gif_content_identity;
    JpegWorkspace* jpeg_workspace;
    DeflateWorkspace workspace;
    u8* scanline_storage;
    u32 scanline_capacity;
    // Uses existing alignment padding. The workspace marker guards shared parser use.
    bool apng_metadata_valid;
    // Optional owner growth after validating dimensions, with no borrowed frame.
    void* scanline_owner;
    Slice<u8> (*grow_scanlines)(void*, u32) noexcept;
    // Returned cache metadata is copied here before an encoded-entry lease is
    // released. Each codec lane owns one pipeline, so callers can submit the
    // conditional request without retaining a pointer into evictable storage.
    u8 result_etag[kEncodedMediaCacheMaximumEtagBytes];
    u32 result_etag_bytes;
};

enum class CdnEncodedAudioState : u8 {
    invalid,
    cache_miss,
    fetch_required,
    ready_cache_hit,
    ready_response,
    ready_transient,
    http_rejected,
    decode_rejected,
    uncorrelated_not_modified,
};
// A ready cache result transfers its lease to the retained lane owner. A
// transient result with no lease borrows response bytes only until its callback
// returns; the lane must copy and rebind them before publishing metadata.
// A no-store 304 transfers a retired lease until joined playback release.
struct CdnEncodedAudioResult {
    EncodedAudioMetadata metadata;
    EncodedMediaCacheLease lease;
    ConstSlice<u8> etag;
    CdnEncodedAudioState state;
    EncodedAudioError error;
    bool ready() const noexcept {
        return state == CdnEncodedAudioState::ready_cache_hit ||
               state == CdnEncodedAudioState::ready_response ||
               state == CdnEncodedAudioState::ready_transient;
    }
};
CdnEncodedAudioResult cdn_media_pipeline_lookup_audio(CdnMediaPipeline*, MediaAssetKey, u64 now_seconds,
                                                      OggOpusCancelled cancelled = nullptr,
                                                      void* context = nullptr) noexcept;
CdnEncodedAudioResult cdn_media_pipeline_admit_audio(CdnMediaPipeline*, MediaAssetKey,
                                                     const CdnResponseResult&, ConstSlice<u8> body,
                                                     ConstSlice<u8> conditional_etag = {},
                                                     OggOpusCancelled cancelled = nullptr,
                                                     void* context = nullptr) noexcept;

bool cdn_media_pipeline_initialize(CdnMediaPipeline* pipeline, EncodedMediaCache* cache,
                                   Slice<u8> scanline_storage) noexcept;
bool cdn_media_pipeline_bind_webp_workspace(CdnMediaPipeline* pipeline, Vp8lWorkspace* workspace) noexcept;
bool cdn_media_pipeline_bind_animated_webp_workspace(CdnMediaPipeline* pipeline,
                                                     AnimatedWebpWorkspace* workspace) noexcept;
bool cdn_media_pipeline_bind_apng_workspace(CdnMediaPipeline* pipeline, ApngWorkspace* workspace) noexcept;
bool cdn_media_pipeline_bind_gif_workspace(CdnMediaPipeline* pipeline, GifWorkspace* workspace) noexcept;

// A stale result carries the cached ETag for a conditional request but does
// not spend decode CPU. Fresh hits are fully parsed, inflated, and filtered.
CdnStaticPngResult cdn_media_pipeline_lookup_static_png(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                        u64 now_seconds) noexcept;

// Consumes only PNG metadata produced by validate_static_image_response.
// A 200 body is decoded before it is published into the encoded cache. A 304
// must correlate to an existing ETag-bearing entry.
CdnStaticPngResult cdn_media_pipeline_admit_static_png(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                       const CdnResponseResult& response,
                                                       ConstSlice<u8> decoded_body,
                                                       ConstSlice<u8> conditional_etag = {}) noexcept;

CdnStaticWebpResult cdn_media_pipeline_lookup_static_webp(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                          u64 now_seconds) noexcept;
CdnStaticWebpResult cdn_media_pipeline_admit_static_webp(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                         const CdnResponseResult& response,
                                                         ConstSlice<u8> decoded_body,
                                                         ConstSlice<u8> conditional_etag = {}) noexcept;

CdnStaticJpegResult cdn_media_pipeline_lookup_static_jpeg(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                          u64 now_seconds) noexcept;
CdnStaticJpegResult cdn_media_pipeline_admit_static_jpeg(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                         const CdnResponseResult& response,
                                                         ConstSlice<u8> decoded_body,
                                                         ConstSlice<u8> conditional_etag = {}) noexcept;

CdnApngResult cdn_media_pipeline_lookup_apng(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                             u64 now_seconds) noexcept;
CdnApngResult cdn_media_pipeline_admit_apng(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                            const CdnResponseResult& response, ConstSlice<u8> decoded_body,
                                            ConstSlice<u8> conditional_etag = {}) noexcept;
CdnApngResult cdn_media_pipeline_decode_apng_frame(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                   u16 cache_generation, u32 frame_index,
                                                   u64 now_seconds) noexcept;

CdnGifResult cdn_media_pipeline_lookup_gif(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                           u64 now_seconds) noexcept;
CdnGifResult cdn_media_pipeline_admit_gif(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                          const CdnResponseResult& response, ConstSlice<u8> decoded_body,
                                          ConstSlice<u8> conditional_etag = {}) noexcept;
CdnGifResult cdn_media_pipeline_decode_gif_frame(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                 u16 cache_generation, u32 frame_index,
                                                 u64 now_seconds) noexcept;

CdnAnimatedWebpResult cdn_media_pipeline_lookup_animated_webp(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                              u64 now_seconds) noexcept;
CdnAnimatedWebpResult cdn_media_pipeline_admit_animated_webp(CdnMediaPipeline* pipeline, MediaAssetKey key,
                                                             const CdnResponseResult& response,
                                                             ConstSlice<u8> decoded_body,
                                                             ConstSlice<u8> conditional_etag = {}) noexcept;
CdnAnimatedWebpResult cdn_media_pipeline_decode_animated_webp_frame(CdnMediaPipeline* pipeline,
                                                                    MediaAssetKey key, u16 cache_generation,
                                                                    u32 frame_index,
                                                                    u64 now_seconds) noexcept;

static_assert(sizeof(CdnMediaPipeline) <= 1'600);

} // namespace maicord
