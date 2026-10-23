#pragma once

#include "maicord/app/cdn_media_pipeline.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/app/cdn_client.hpp"
#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/app/local_draft_preview.hpp"
#include "maicord/app/attachment_media_probe.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11Device;

namespace maicord {

struct Mp4Decoder;
struct ProgressiveVideoFetch;

inline constexpr u32 kAccountMediaCacheEntryCapacity = 2'048;
inline constexpr u32 kAccountMediaCachePageCapacity = 4'096;
inline constexpr u32 kAccountMediaCacheIndexCapacity = 4'096;
// Negative media results are cached in a small four-way hash table. A visible
// channel can contain far more than sixteen unsupported external embed URLs,
// so a linear cache would retry and log every bad URL on each paint pump.
inline constexpr u32 kAccountMediaFailureCapacity = 128;
inline constexpr u32 kAccountMediaFailureWays = 4;
inline constexpr u64 kAccountMediaFailureRetryMs = 60 * 1000;

enum class AccountMediaRequestState : u8 {
    invalid,
    ready,
    queued,
    pending,
    busy,
    retry_wait,
    refresh_required,
    unsupported,
    failed,
};

enum class AccountMediaError : u8 {
    none,
    invalid_input,
    memory,
    client,
    target,
    cache,
    http,
    decode,
};

enum class AccountMediaVideoFailure : u8 {
    none,
    invalid_input,
    truncated_stream,
    malformed_stream,
    unsupported_stream,
    memory,
    unsupported_backend,
    device_removed,
    native_failure,
    demux_malformed,
    demux_unsupported,
    demux_no_video,
    unexpected_gif,
    unexpected_png,
    unexpected_webp,
    unexpected_jpeg,
    unexpected_avif,
};

struct AccountMediaRequestResult {
    u64 retry_at_ms;
    CdnClientError client_error;
    CdnHttpError http_error;
    PngError png_error;
    WebpError webp_error;
    ApngError apng_error;
    GifError gif_error;
    JpegError jpeg_error;
    AnimatedWebpError animated_webp_error;
    LossyWebpError lossy_webp_error;
    AttachmentUrlError attachment_url_error;
    MediaUrlError media_url_error;
    CdnTargetError target_error;
    AccountMediaRequestState state;
    AccountMediaError error;
    // These two bytes occupy existing padding, so field offsets and the
    // 24-byte by-value account ABI are unchanged.
    AttachmentQueryError attachment_query_error;
    AccountMediaVideoFailure video_failure;

    constexpr bool ok() const noexcept {
        return error == AccountMediaError::none;
    }
};

enum class AccountMediaFrameKind : u8 {
    none,
    png_scanlines,
    straight_rgba8,
    apng_scanlines,
    gif_indices,
    animated_webp_rgba8,
    vp8_keyframe,
    animated_webp_vp8,
    jpeg_coefficients,
    video_nv12_gpu = 10, // Values 9 and 12 are reserved.
    // Worker-compiled ColrPaintProgram bytes (64 px flags, 128 px Lottie).
    regional_flag_paint = 11,
    lottie_paint = 13,
    audio_source = 14, // Metadata only; its encoded source stays with the worker.
};

enum class AccountMediaSourceKind : u8 {
    none,
    message,
    catalog_preview,
    local_draft_preview,
};

struct AccountMediaFrame {
    u64 content_identity;
    const GpuVideoFrame* gpu_video;
    const PngImage* image;
    const GifPalette* gif_palette;
    const Vp8Keyframe* vp8_frame;
    const Vp8ControlHeader* vp8_header;
    const Vp8ModeWorkspace* vp8_modes;
    const Vp8CoefficientWorkspace* vp8_coefficients;
    const JpegImage* jpeg_image;
    const JpegCoefficientWorkspace* jpeg_coefficients;
    ConstSlice<u8> bytes;
    MediaAssetKey key;
    u64 request_id;
    u64 source_message_id;
    u64 duration_us;
    u32 width;
    u32 height;
    u32 row_pitch;
    u32 frame_index;
    u32 frame_count;
    u32 play_count;
    u32 source_message_revision;
    union {
        ApngFrame apng_frame;
        GifFrame gif_frame;
        AnimatedWebpFrame animated_webp_frame;
    };
    // Animated WebP canvas colour, always transparent black: ANIM's background
    // colour is a hint that browsers and libwebp's WebPAnimDecoder ignore (the
    // canvas starts and disposes transparent, like GIF and APNG), so
    // transparent pixels show the chat behind them.
    u32 background_rgba;
    u16 cache_generation;
    // Video: natural square-pixel display extent. width/height and gpu_video
    // hold decoded pixel extents. Other animations: composition canvas.
    u16 canvas_width;
    u16 canvas_height;
    AccountMediaFrameKind kind;
    u8 reserved;
    u64 source_catalog_scope;
    u32 source_catalog_generation;
    AccountMediaSourceKind source_kind;
    u8 source_reserved[3];
    // Nonzero only for a worker-owned local GPU poster. Not a content hash.
    u64 lease_serial;
};

inline bool local_draft_preview_frame_matches(const AccountMediaFrame& frame,
                                              const AccountMediaLocalPreviewRequest& request) noexcept {
    return frame.source_kind == AccountMediaSourceKind::local_draft_preview &&
           media_asset_key_equal(frame.key, request.key) &&
           frame.source_catalog_scope == request.owner_scope &&
           frame.source_catalog_generation == request.owner_generation &&
           frame.source_message_id == request.key.id &&
           frame.source_message_revision == request.key.variant;
}

struct AccountMediaCatalogPreviewRequest {
    Utf8View url;
    MediaAssetKey key;
    u64 catalog_scope;
    u32 catalog_generation;
    u16 width;
    u16 height;
    MediaAttachmentFormat format;
    u8 reserved[4];
    // Borrowed host device for an MP4 (VIDEO favourite) tile's hardware
    // decoder; it must outlive account stop. Other formats ignore it.
    ID3D11Device* video_device;
};

struct AccountMediaAttachmentRequest {
    Utf8View signed_url;
    MediaAssetKey key;
    u64 source_message_id;
    u32 source_message_revision;
    u16 proxy_width;
    u16 proxy_height;
    CdnImageType image_type;
    u8 reserved[3];
    // Borrowed from the host. It must outlive account stop; the decoder's
    // device manager takes its own COM reference while playback is active.
    ID3D11Device* video_device;
};

struct AccountMediaGuildIconRequest {
    Utf8View icon_hash;
    MediaAssetKey key;
    u32 reserved[2];
};

struct AccountMediaFailure {
    MediaAssetKey key;
    u64 retry_at_ms;
};

struct AccountMediaEncodedAudioSource {
    MediaAssetKey key;
    EncodedMediaCacheLease lease;
    ConstSlice<u8> bytes;
    EncodedAudioMetadata metadata;
    void* transient_allocation;
    u64 content_identity;
    u64 source_message_id;
    u64 duration_us;
    u32 source_message_revision;
    u16 cache_generation;
    u16 reserved;
};

struct AccountMedia {
    CdnClient client;
    EncodedMediaCache cache;
    CdnMediaPipeline pipeline;
    PngImage frame_image;
    WebpImage frame_webp_image;
    JpegImage frame_jpeg_image;
    Vp8Keyframe frame_vp8;
    Vp8ControlHeader frame_vp8_header;
    union {
        ApngFrame frame_apng;
        GifFrame frame_gif;
        AnimatedWebpFrame frame_animated_webp;
    };
    GifPalette frame_gif_palette;
    Vp8lWorkspace* webp_workspace;
    // JPEG and lossy WebP share one reservation. Each decode starts the required
    // trivial object lifetime. Pipeline pointers name the active frame storage.
    // VP8L alpha stays separate because lossless and lossy decode can overlap.
    LossyWebpWorkspace* lossy_webp_workspace;
    AnimatedWebpWorkspace* animated_webp_workspace;
    ApngWorkspace* apng_workspace;
    GifWorkspace* gif_workspace;
    JpegWorkspace* jpeg_workspace;
    Mp4Decoder owned_mp4_decoder;
    Mp4Decoder* mp4_decoder;
    ID3D11Device* video_device;
    MediaAssetKey active_key;
    MediaAssetKey frame_key;
    MediaAssetKey mp4_key;
    void* allocation;
    void* expanded_scanlines;
    void* lottie_workspace;
    u64 request_id;
    u64 active_request_id;
    u64 frame_request_id;
    u64 active_source_message_id;
    u64 frame_source_message_id;
    u64 active_catalog_scope;
    u64 frame_catalog_scope;
    u64 frame_duration_us;
    u64 mp4_source_message_id;
    u64 mp4_content_identity;
    // Monotonic time the resident decoder last published a frame. Worker
    // admission keeps actively playing decoders off new video sources.
    u64 mp4_used_ms;
    u64 next_wake_at_ms;
    u32 frame_bytes;
    u32 frame_index;
    u32 frame_count;
    u32 frame_play_count;
    u32 mp4_source_message_revision;
    u32 active_source_message_revision;
    u32 frame_source_message_revision;
    u32 active_catalog_generation;
    u32 frame_catalog_generation;
    u32 admitted;
    u32 cache_ready;
    u32 failures;
    AccountMediaRequestResult last_failure;
    AccountMediaFailure failed[kAccountMediaFailureCapacity];
    bool active;
    bool frame_ready;
    bool client_started;
    bool started;
    bool owns_cache;
    u8 ownership_reserved[3];
    u16 frame_cache_generation;
    u16 frame_canvas_width;
    u16 frame_canvas_height;
    u16 mp4_generation;
    u16 active_proxy_width;
    u16 active_proxy_height;
    AccountMediaFrameKind frame_kind;
    CdnImageType active_image_type;
    AccountMediaEncodedAudioSource audio_source;
    u16 audio_generation;
    OggOpusCancelled audio_cancelled;
    void* audio_cancel_context;
    // Attachment MP4s stream through HTTP Range into this lane-owned sparse
    // source, which the decoder borrows; released only after the decoder.
    ProgressiveVideoFetch* progressive;
    bool progressive_active;
    bool progressive_continue;
};

// Complete progressive sources at most this large enter the encoded cache.
inline constexpr u32 kAccountMediaProgressiveCacheMaximumBytes = 2u * 1024u * 1024u;
// A VIDEO GIF favourite's picker tile source (Klipy's MP4s are well under 1 MiB).
inline constexpr u32 kAccountMediaCatalogVideoMaximumBytes = 16u * 1024u * 1024u;

AccountMediaRequestResult account_media_initialize(AccountMedia* media) noexcept;
// Strong image signatures distinguish a proxy poster from malformed MP4
// without exposing bytes or weakening the decoder's format validation.
AccountMediaVideoFailure account_media_video_body_failure(ConstSlice<u8> body) noexcept;
AccountMediaRequestResult account_media_initialize_shared(AccountMedia* media,
                                                          EncodedMediaCache* shared_cache) noexcept;
AccountMediaRequestResult account_media_request_static_asset(AccountMedia* media, MediaAssetKey key,
                                                             u64 monotonic_now_ms, u64 unix_now_seconds,
                                                             Utf8View guild_icon_hash = {},
                                                             u32 identity_size = 0) noexcept;
// Hash-addressed guild icon, user avatar or group DM icon. reserved[0] is the
// CDN size (a power of two from 16 through 4096, zero selects 128); the
// asset key stays size-free so every surface shares one decode.
AccountMediaRequestResult account_media_request_guild_icon(AccountMedia* media,
                                                           const AccountMediaGuildIconRequest& request,
                                                           u64 monotonic_now_ms,
                                                           u64 unix_now_seconds) noexcept;
AccountMediaRequestResult account_media_request_attachment(AccountMedia* media,
                                                           const AccountMediaAttachmentRequest& request,
                                                           u64 monotonic_now_ms,
                                                           u64 unix_now_seconds) noexcept;
AccountMediaRequestResult account_media_request_catalog_preview(
    AccountMedia* media, const AccountMediaCatalogPreviewRequest& request, u64 monotonic_now_ms,
    u64 unix_now_seconds) noexcept;
// Worker-only: local bytes are decoded through the worker's codec workspaces
// and shared encoded cache. This never submits CDN traffic.
AccountMediaRequestResult account_media_decode_local_preview(AccountMedia*,
                                                             const AccountMediaLocalPreviewRequest&,
                                                             ConstSlice<u8> encoded) noexcept;
// Worker-only composer MP4/MOV poster. The file is read through `read` (exact
// reads; false cancels) into a sparse lane-owned source: the movie header and
// the first samples only. The first frame is published as a GPU NV12 frame;
// the decoder and source stay on the lane until account_media_release_video
// (the next lane command, preview retirement or account stop). Missing
// devices, unsupported codecs and malformed files fail without a retry.
AccountMediaRequestResult account_media_decode_local_video_preview(AccountMedia*,
                                                                   const AccountMediaLocalPreviewRequest&,
                                                                   AttachmentProbeRead read,
                                                                   void* context) noexcept;
// True while the lane's decoder holds a local draft preview poster.
bool account_media_local_video_resident(const AccountMedia*) noexcept;
AccountMediaRequestResult account_media_advance_animation(AccountMedia* media, MediaAssetKey key,
                                                          u16 cache_generation, u32 frame_index,
                                                          u64 unix_now_seconds) noexcept;
bool account_media_pump(AccountMedia* media, u64 monotonic_now_ms, u64 unix_now_seconds) noexcept;
bool account_media_take_frame(AccountMedia* media, AccountMediaFrame* frame) noexcept;
// Returns scanline storage a large decode grew past the lane's own, once its
// frame is taken; the next decode grows again if it needs to.
bool account_media_trim_scanlines(AccountMedia* media) noexcept;
bool account_media_video_matches(const AccountMedia* media, MediaAssetKey key,
                                 u16 cache_generation = 0) noexcept;
bool account_media_audio_matches(const AccountMedia*, MediaAssetKey, u16 cache_generation = 0) noexcept;
// Worker only, after joined playback releases its borrowed demux/source.
void account_media_release_audio_source(AccountMedia*) noexcept;
// Worker only: releases the MP4 decoder, then the progressive source it borrows.
void account_media_release_video(AccountMedia*) noexcept;
// Worker only: stores a complete, small progressive source once.
void account_media_cache_progressive(AccountMedia*, MediaAssetKey) noexcept;
// Worker only: submits one range of the lane's progressive source.
bool account_media_submit_progressive(AccountMedia*, CdnByteRange) noexcept;
HANDLE account_media_notification_handle(const AccountMedia* media) noexcept;
bool account_media_stop(AccountMedia* media, u32 timeout_ms) noexcept;

static_assert(sizeof(AccountMediaRequestResult) == 24);
static_assert(offsetof(AccountMediaRequestResult, state) == 20);
static_assert(offsetof(AccountMediaRequestResult, error) == 21);
static_assert(offsetof(AccountMediaRequestResult, attachment_query_error) == 22);
static_assert(offsetof(AccountMediaRequestResult, video_failure) == 23);
static_assert(sizeof(AccountMediaAttachmentRequest) == 64);
static_assert(sizeof(AccountMediaGuildIconRequest) == 40);
static_assert(sizeof(AccountMediaCatalogPreviewRequest) == 64);
static_assert(sizeof(AccountMediaFrame) == 232);

} // namespace maicord
