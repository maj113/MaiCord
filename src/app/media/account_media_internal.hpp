#pragma once
// Shared by account_media.cpp and its parts (account_media_*.cpp); private to them.
#include "maicord/app/media/account_media.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/platform/decode/mp4_decoder.hpp"
#include "maicord/app/media/progressive_video_fetch.hpp"
#include "maicord/sys/crt.hpp"

namespace maicord {
namespace account_media_detail {}
using namespace account_media_detail;
namespace account_media_detail {

ProgressiveVideoFetch* allocate_fetch() noexcept;

} // namespace account_media_detail

// Exact ordered records and a compact UTF-8 pool. Names are retained for the
// queue lifetime; neither hovering nor returning to a track refetches them.
struct CollectionText { char* data{}; u32 bytes{}, capacity{}; };

namespace account_media_detail {

bool collection_text_reserve(CollectionText& text, u32 required) noexcept;

} // namespace account_media_detail

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

namespace account_media_detail {

void release_collection(AccountMediaCollection* collection) noexcept;
AccountMediaRequestResult result(AccountMediaRequestState state,
                                 AccountMediaError error = AccountMediaError::none) noexcept;
bool supported_key(const MediaAssetKey& key) noexcept;
bool webp_asset(const MediaAssetKey& key) noexcept;
bool lottie_asset(const MediaAssetKey& key) noexcept;
bool animated_asset(const MediaAssetKey& key) noexcept;
bool apng_asset(const MediaAssetKey& key) noexcept;
bool gif_asset(const MediaAssetKey& key) noexcept;
bool video_asset(const MediaAssetKey& key) noexcept;
bool catalog_png_key(const MediaAssetKey& key) noexcept;
bool png_animation_body(const ConstSlice<u8>& body) noexcept;
bool catalog_png_cached_animation(AccountMedia* media, const MediaAssetKey& key, u64 now_seconds) noexcept;
void clear_failure(AccountMedia* media, const MediaAssetKey& key) noexcept;
AccountMediaRequestResult remember_failure(AccountMedia* media, const MediaAssetKey& key_view, u64 now_ms,
                                           const AccountMediaRequestResult& failure_view) noexcept;
void mark_frame_ready(AccountMedia* media, const MediaAssetKey& key_view) noexcept;
void capture_catalog_source(AccountMedia* media, const MediaAssetKey& key_view) noexcept;
void stage_frame(AccountMedia* media, const MediaAssetKey& key_view, u32 bytes, u64 request_id,
                 u64 source_message_id, u32 source_message_revision) noexcept;
void publish_png_frame(AccountMedia* media, const MediaAssetKey& key, const CdnStaticPngResult& decoded,
                       u64 request_id, u64 source_message_id = 0,
                       u32 source_message_revision = 0) noexcept;
void publish_webp_frame(AccountMedia* media, const MediaAssetKey& key, const CdnStaticWebpResult& decoded,
                        u64 request_id, u64 source_message_id = 0,
                        u32 source_message_revision = 0) noexcept;
void publish_jpeg_frame(AccountMedia* media, const MediaAssetKey& key, const CdnStaticJpegResult& decoded,
                        u64 request_id, u64 source_message_id, u32 source_message_revision) noexcept;
void publish_animated_webp_frame(AccountMedia* media, const MediaAssetKey& key,
                                 const CdnAnimatedWebpResult& decoded, u64 request_id) noexcept;
void publish_apng_frame(AccountMedia* media, const MediaAssetKey& key, const CdnApngResult& decoded,
                        u64 request_id) noexcept;
void publish_gif_frame(AccountMedia* media, const MediaAssetKey& key, const CdnGifResult& decoded,
                       u64 request_id) noexcept;
void publish_mp4_frame(AccountMedia* media, const MediaAssetKey& key_view, const Mp4DecodedFrame& decoded,
                       u64 request_id, u64 source_message_id, u32 source_message_revision,
                       u32 frame_index) noexcept;
bool audio_image_type(CdnImageType type) noexcept;
bool publish_audio_frame(AccountMedia* media, const MediaAssetKey& key, u64 request_id) noexcept;
bool publish_audio_source(AccountMedia* media, const MediaAssetKey& key, CdnEncodedAudioResult* decoded,
                          u64 request_id) noexcept;
bool ensure_webp_workspace(AccountMedia* media) noexcept;
bool ensure_lossy_webp_workspace(AccountMedia* media) noexcept;
bool ensure_animated_webp_workspace(AccountMedia* media) noexcept;
bool ensure_apng_workspace(AccountMedia* media) noexcept;
bool ensure_gif_workspace(AccountMedia* media) noexcept;
bool ensure_jpeg_workspace(AccountMedia* media) noexcept;
Mp4Decoder* ensure_mp4_decoder(AccountMedia* media) noexcept;
AccountMediaRequestResult video_failure(const Mp4Decoder* decoder) noexcept;
AccountMediaRequestResult ensure_client_started(AccountMedia* media) noexcept;
void adopt_mp4_source(AccountMedia* media) noexcept;
bool submit_progressive(AccountMedia* media, CdnByteRange range, ProgressiveVideoFetch* selected = nullptr) noexcept;
bool continue_progressive(AccountMedia* media, u64 now_ms, u64 unix_now_seconds) noexcept;

struct ConsumeContext {
    AccountMedia* media;
    u64 now_ms;
};

bool publish_vector(AccountMedia* media, const MediaAssetKey& key_view, const ConstSlice<u8>& encoded_view, u64 request_id,
                    u32 frame_index = 0) noexcept;
__declspec(noinline) bool publish_bundled_flag(AccountMedia* media, MediaAssetKey key) noexcept;
bool consume_response(void* opaque, const CdnClientResponse& response,
                      ConstSlice<u8> body) noexcept;

// One codec's encoded-cache probe for a request that may need a fetch. A
// decoded hit publishes; a decode failure is remembered and an unavailable
// workspace fails unremembered. Either settles the request into *settled and
// returns true. Otherwise the cache holds nothing usable: probe->etag carries
// the validator for the fetch and the result is false.
struct CacheProbe {
    MediaAssetKey key;
    u64 now_ms, unix_seconds, source_message_id;
    u32 source_message_revision;
    ConstSlice<u8> etag;
};

// Settles the cache lookup `cached` of one codec: a decoded hit runs publish() and is ready; a failed
// decode is remembered with the codec's error fields (describe(&failure)); a miss or stale entry leaves
// its validator in probe->etag and answers false.
template <typename Result, typename Publish, typename Describe>
bool settle_probe(AccountMedia* media, CacheProbe* probe, AccountMediaRequestResult* settled,
                  const Result& cached, Publish publish, Describe describe) noexcept {
    using State = decltype(Result::state);
    if (cached.decoded()) {
        publish();
        *settled = result(AccountMediaRequestState::ready);
        return true;
    }
    if (cached.state != State::cache_miss && cached.state != State::fetch_required) {
        auto failure = result(AccountMediaRequestState::failed, AccountMediaError::decode);
        describe(&failure);
        *settled = remember_failure(media, probe->key, probe->now_ms, failure);
        return true;
    }
    probe->etag = cached.etag;
    return false;
}

bool probe_static_jpeg(AccountMedia* media, CacheProbe* probe, AccountMediaRequestResult* settled) noexcept;
bool probe_static_webp(AccountMedia* media, CacheProbe* probe, AccountMediaRequestResult* settled) noexcept;
bool probe_apng(AccountMedia* media, CacheProbe* probe, AccountMediaRequestResult* settled) noexcept;
bool probe_gif(AccountMedia* media, CacheProbe* probe, AccountMediaRequestResult* settled) noexcept;
bool probe_static_png(AccountMedia* media, CacheProbe* probe, AccountMediaRequestResult* settled) noexcept;
bool request_in_flight(const AccountMedia* media, const MediaAssetKey& key,
                       AccountMediaRequestResult* settled) noexcept;
bool failure_cooling(AccountMedia* media, const MediaAssetKey& key, u64 now_ms,
                     AccountMediaRequestResult* settled) noexcept;

enum class LottieProbe : u8 { miss, published, rejected };

LottieProbe probe_lottie(AccountMedia* media, CacheProbe* probe) noexcept;
AccountMediaRequestResult submit_fetch(AccountMedia* media, const MediaAssetKey& key_view, u64 now_ms,
                                       CdnClientRequest request) noexcept;

} // namespace account_media_detail

struct AccountMediaUrlRequest {
    Utf8View url;
    MediaAssetKey key;
    u64 source_message_id;
    u32 source_message_revision;
    u16 proxy_width;
    u16 proxy_height;
    CdnImageType image_type;
    GpuDevice* video_device;
    u64 catalog_scope;
    u32 catalog_generation;
    bool progressive = false;
    u64 provider_expires = 0;
    CdnHost provider_host = CdnHost::none;
    Utf8View companion_url{};
    u16 collection_track{};
};

namespace account_media_detail {

AccountMediaRequestResult request_url_media(AccountMedia* media,
                                                   const AccountMediaUrlRequest& request,
                                                   u64 monotonic_now_ms, u64 unix_now_seconds) noexcept;

} // namespace account_media_detail
} // namespace maicord
